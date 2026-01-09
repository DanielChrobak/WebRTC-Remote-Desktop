import './renderer.js';
import './input.js';
import { MSG, C, S, $, mkBuf, Stage } from './state.js';
import { handleAudioPkt, closeAudio, initDecoder, decodeFrame, setReqKeyFn } from './media.js';
import { updateStats, updateMonOpts, updateFpsOpts, setNetCbs, updateLoadingStage, showLoading, hideLoading, isLoadingVisible } from './ui.js';

let baseUrl = '';
const AUTH_KEY = 'remote_desktop_auth', CONN_KEY = 'remote_desktop_connection';
const STUN = [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun1.l.google.com:19302' }];

// ==================== DEBUG MODULE ====================
const DEBUG = {
    enabled: true,
    verbose: false, // Set to true for per-packet logging

    // Drop reason counters
    drops: {
        timeout: 0,
        bufferFull: 0,
        incomplete: 0,
        stale: 0,
        outOfOrder: 0,
        chunkMissing: 0,
        total: 0
    },

    // Chunk tracking per frame
    frameChunks: new Map(), // frameId -> { expected, received, firstArrival, chunks: Set }

    // Network quality metrics
    network: {
        avgChunkGap: 0,
        maxChunkGap: 0,
        chunkGaps: [],
        lastChunkTime: 0,
        packetsPerSecond: 0,
        packetCount: 0,
        lastPacketCountTime: 0
    },

    // Log a frame drop with reason
    logDrop(frameId, reason, details = {}) {
        this.drops[reason]++;
        this.drops.total++;

        const frameInfo = this.frameChunks.get(frameId);
        const received = frameInfo ? frameInfo.received : '?';
        const expected = frameInfo ? frameInfo.expected : '?';
        const age = frameInfo ? (performance.now() - frameInfo.firstArrival).toFixed(1) : '?';

        console.warn(`🔴 FRAME DROP #${frameId}: ${reason.toUpperCase()}`, {
            received: `${received}/${expected} chunks`,
            age: `${age}ms`,
            rtt: `${S.rtt.toFixed(1)}ms`,
            buffered: S.chunks.size,
            ...details
        });

        // Log missing chunks if available
        if (frameInfo && reason === 'incomplete' || reason === 'timeout') {
            const missing = [];
            for (let i = 0; i < frameInfo.expected; i++) {
                if (!frameInfo.chunks.has(i)) missing.push(i);
            }
            if (missing.length > 0 && missing.length <= 10) {
                console.warn(`   Missing chunks: [${missing.join(', ')}]`);
            } else if (missing.length > 10) {
                console.warn(`   Missing ${missing.length} chunks (first 5: [${missing.slice(0, 5).join(', ')}...])`);
            }
        }

        this.frameChunks.delete(frameId);
    },

    // Track chunk arrival
    trackChunk(frameId, chunkIdx, totalChunks, isNew) {
        const now = performance.now();

        // Track packet rate
        this.network.packetCount++;
        if (now - this.network.lastPacketCountTime > 1000) {
            this.network.packetsPerSecond = this.network.packetCount;
            this.network.packetCount = 0;
            this.network.lastPacketCountTime = now;
        }

        // Track inter-chunk gaps
        if (this.network.lastChunkTime > 0) {
            const gap = now - this.network.lastChunkTime;
            this.network.chunkGaps.push(gap);
            if (this.network.chunkGaps.length > 100) this.network.chunkGaps.shift();
            this.network.maxChunkGap = Math.max(this.network.maxChunkGap, gap);
            this.network.avgChunkGap = this.network.chunkGaps.reduce((a, b) => a + b, 0) / this.network.chunkGaps.length;
        }
        this.network.lastChunkTime = now;

        // Track frame chunks
        if (!this.frameChunks.has(frameId)) {
            this.frameChunks.set(frameId, {
                expected: totalChunks,
                received: 0,
                firstArrival: now,
                lastArrival: now,
                chunks: new Set(),
                isKey: false
            });
        }

        const frame = this.frameChunks.get(frameId);
        if (!frame.chunks.has(chunkIdx)) {
            frame.chunks.add(chunkIdx);
            frame.received++;
            frame.lastArrival = now;
        }

        if (this.verbose) {
            console.debug(`📦 Chunk ${chunkIdx + 1}/${totalChunks} for frame #${frameId} (${frame.received}/${totalChunks} received)`);
        }
    },

    // Log frame completion
    logComplete(frameId) {
        const frame = this.frameChunks.get(frameId);
        if (frame) {
            const assemblyTime = performance.now() - frame.firstArrival;
            if (this.verbose || assemblyTime > 50) {
                console.info(`✅ Frame #${frameId} complete: ${frame.expected} chunks in ${assemblyTime.toFixed(1)}ms`);
            }
        }
        this.frameChunks.delete(frameId);
    },

    // Get summary stats
    getSummary() {
        return {
            drops: { ...this.drops },
            network: {
                avgChunkGap: this.network.avgChunkGap.toFixed(2) + 'ms',
                maxChunkGap: this.network.maxChunkGap.toFixed(2) + 'ms',
                packetsPerSecond: this.network.packetsPerSecond
            },
            bufferedFrames: S.chunks.size,
            rtt: S.rtt.toFixed(1) + 'ms'
        };
    },

    // Print periodic summary
    printSummary() {
        if (this.drops.total > 0) {
            console.info('📊 Frame Drop Summary:', this.getSummary());
        }
    },

    // Reset counters
    reset() {
        Object.keys(this.drops).forEach(k => this.drops[k] = 0);
        this.frameChunks.clear();
        this.network.chunkGaps = [];
        this.network.maxChunkGap = 0;
    }
};

// Print debug summary every 5 seconds
setInterval(() => DEBUG.enabled && DEBUG.printSummary(), 5000);

// Expose debug to console
window.frameDebug = DEBUG;
console.info('🔧 Frame debugging enabled. Access via window.frameDebug');
console.info('   - frameDebug.verbose = true  → Enable per-packet logging');
console.info('   - frameDebug.getSummary()    → Get current stats');
console.info('   - frameDebug.reset()         → Reset counters');

// ==================== END DEBUG MODULE ====================

const tsUs = (t = performance.now()) => Math.floor((performance.timeOrigin + t) * 1000);
const toSrvUs = t => tsUs(t) - S.clockOff;
const sendMsg = buf => { if (S.dc?.readyState !== 'open') return false; try { S.dc.send(buf); return true; } catch { return false; } };

let creds = null, authResolve = null, authReject = null, hasConnected = false, waitFirstFrame = false, connAttempts = 0, pingInterval = null;

const connEl = { overlay: $('connectOverlay'), url: $('localUrlInput'), btn: $('connectLocalBtn'), err: $('connectError') };
const authEl = { overlay: $('authOverlay'), user: $('usernameInput'), pin: $('pinInput'), err: $('authError'), btn: $('authSubmit') };

const validUser = u => u?.length >= 3 && u.length <= 32 && /^[a-zA-Z0-9_-]+$/.test(u);
const validPin = p => p?.length === 6 && /^\d{6}$/.test(p);
const validCreds = c => c && validUser(c.username) && validPin(c.pin);
const getSaved = () => { try { return JSON.parse(localStorage.getItem(AUTH_KEY)); } catch { return null; } };
const saveCreds = (u, p) => { try { localStorage.setItem(AUTH_KEY, JSON.stringify({ username: u, pin: p })); } catch {} };
const clearCreds = () => { try { localStorage.removeItem(AUTH_KEY); } catch {} };
const clearPing = () => { pingInterval && (clearInterval(pingInterval), pingInterval = null); };

const loadConnSettings = () => { try { const s = JSON.parse(localStorage.getItem(CONN_KEY)); s?.localUrl && (connEl.url.value = s.localUrl); } catch {} };
const saveConnSettings = () => { try { localStorage.setItem(CONN_KEY, JSON.stringify({ localUrl: connEl.url.value })); } catch {} };
const showConnModal = (err = '') => { connEl.err.textContent = err; connEl.overlay.classList.remove('hidden'); hideLoading(); };
const hideConnModal = () => { connEl.overlay.classList.add('hidden'); connEl.err.textContent = ''; };

const setAuthErr = (err, el) => { authEl.err.textContent = err; [authEl.user, authEl.pin].forEach(e => e.classList.toggle('error', e === el)); el?.focus(); };
const showAuthModal = (err = '') => { authEl.user.value = authEl.pin.value = ''; setAuthErr(err, err ? authEl.user : null); authEl.overlay.classList.remove('hidden'); authEl.btn.disabled = false; setTimeout(() => authEl.user.focus(), 100); };
const hideAuthModal = () => { authEl.overlay.classList.add('hidden'); setAuthErr('', null); };

const sendAuth = (u, p) => {
    if (S.dc?.readyState !== 'open') return console.error('DC not open for auth'), false;
    const ub = new TextEncoder().encode(u), pb = new TextEncoder().encode(p);
    const buf = new ArrayBuffer(6 + ub.length + pb.length), v = new DataView(buf);
    v.setUint32(0, MSG.AUTH_REQUEST, true); v.setUint8(4, ub.length); v.setUint8(5, pb.length);
    new Uint8Array(buf, 6).set(ub); new Uint8Array(buf, 6 + ub.length).set(pb);
    try { S.dc.send(buf); console.info('Auth request sent'); return true; } catch (e) { console.error('Auth send failed:', e); return false; }
};

const handleAuthResp = data => {
    if (data.byteLength < 6) return false;
    const v = new DataView(data); if (v.getUint32(0, true) !== MSG.AUTH_RESPONSE) return false;
    const ok = v.getUint8(4) === 1, errLen = v.getUint8(5);
    if (ok) { console.info('Auth successful'); S.authenticated = true; hideAuthModal(); }
    else { const msg = errLen && data.byteLength >= 6 + errLen ? new TextDecoder().decode(new Uint8Array(data, 6, errLen)) : 'Invalid credentials';
        console.warn('Auth failed:', msg); S.authenticated = false; clearCreds(); creds = null; showAuthModal(msg); authReject?.(new Error(msg)); }
    authResolve?.(ok); authResolve = authReject = null; return true;
};

connEl.btn.addEventListener('click', async () => {
    let url = connEl.url.value.trim(); if (!url) return connEl.err.textContent = 'Please enter a server URL';
    if (!url.startsWith('http://') && !url.startsWith('https://')) url = 'http://' + url;
    url = url.replace(/\/+$/, ''); connEl.url.value = url; baseUrl = url; saveConnSettings();
    hideConnModal(); showLoading(false); updateLoadingStage(Stage.SIGNAL, 'Connecting to server...');
    try { await connect(); } catch (e) { showConnModal('Connection failed: ' + e.message); }
});

authEl.user.addEventListener('input', e => { e.target.value = e.target.value.replace(/[^a-zA-Z0-9_-]/g, '').slice(0, 32); setAuthErr('', null); });
authEl.user.addEventListener('keydown', e => e.key === 'Enter' && (e.preventDefault(), authEl.pin.focus()));
authEl.pin.addEventListener('input', e => { e.target.value = e.target.value.replace(/\D/g, '').slice(0, 6); setAuthErr('', null); });
authEl.pin.addEventListener('keydown', e => e.key === 'Enter' && validCreds({ username: authEl.user.value, pin: authEl.pin.value }) && authEl.btn.click());

authEl.btn.addEventListener('click', () => {
    const u = authEl.user.value, p = authEl.pin.value;
    if (!validUser(u)) return setAuthErr('Username must be 3-32 characters', authEl.user);
    if (!validPin(p)) return setAuthErr('PIN must be exactly 6 digits', authEl.pin);
    authEl.btn.disabled = true; authEl.err.textContent = ''; creds = { username: u, pin: p }; saveCreds(u, p);
    S.dc?.readyState === 'open' ? sendAuth(u, p) : (hideAuthModal(), authResolve?.(true), authResolve = authReject = null);
});

$('disconnectBtn')?.addEventListener('click', () => { cleanup(); hasConnected = false; showConnModal(); });

export const sendMonSel = i => sendMsg(mkBuf(5, v => { v.setUint32(0, MSG.MONITOR_SET, true); v.setUint8(4, i); }));
export const sendFps = (fps, mode) => sendMsg(mkBuf(7, v => { v.setUint32(0, MSG.FPS_SET, true); v.setUint16(4, fps, true); v.setUint8(6, mode); }));
export const reqKey = () => sendMsg(mkBuf(4, v => v.setUint32(0, MSG.REQUEST_KEY, true)));
setReqKeyFn(reqKey);

const updJitter = (t, cap, prev) => { if (S.jitter.last > 0 && prev > 0) { const d = Math.abs((t - S.jitter.last) - (cap - prev) / 1000); if (d < 1000) { S.jitter.deltas.push(d); S.jitter.deltas.length > 60 && S.jitter.deltas.shift(); } } S.jitter.last = t; };

const parseMonList = data => {
    const v = new DataView(data); let o = 4; const cnt = v.getUint8(o++); S.currentMon = v.getUint8(o++);
    S.monitors = Array.from({ length: cnt }, () => { const idx = v.getUint8(o++), w = v.getUint16(o, true), h = v.getUint16(o + 2, true), rr = v.getUint16(o + 4, true);
        o += 6; const prim = v.getUint8(o++) === 1, nl = v.getUint8(o++), nm = new TextDecoder().decode(new Uint8Array(data, o, nl)); o += nl;
        return { index: idx, width: w, height: h, refreshRate: rr, isPrimary: prim, name: nm }; });
    updateMonOpts();
};

export const selDefFps = () => { const sel = $('fpsSel'); if (!sel?.options.length) return S.clientFps;
    const opts = [...sel.options].map(o => +o.value), best = opts.reduce((b, o) => Math.abs(o - S.clientFps) < Math.abs(b - S.clientFps) ? o : b, opts[0]);
    sel.value = opts.includes(S.clientFps) ? S.clientFps : best; return +sel.value; };

export const applyFps = val => { const fps = +val, mode = fps === S.hostFps ? 1 : fps === S.clientFps ? 2 : 0;
    if (sendFps(fps, mode)) { S.currentFps = fps; S.currentFpsMode = mode; S.fpsSent = true; } };

const isNewer = (n, l) => { const d = (n - l) >>> 0; return d > 0 && d < 0x80000000; };

// Calculate how far behind a frame is from the newest
const frameLag = (fid) => {
    if (S.newestFrameId === 0) return 0;
    const d = (S.newestFrameId - fid) >>> 0;
    return d < 0x80000000 ? d : 0;
};

// Determine if a frame should be dropped
const shouldDropFrame = (id, fr, rt, newestId) => {
    const age = rt - fr.firstTime;
    const completion = fr.received / fr.total;
    const lag = frameLag(id);

    const timeout = C.FRAME_TIMEOUT_MS;
    const keyTimeout = timeout + C.KEYFRAME_GRACE_MS;

    // Drop incomplete frames after timeout
    if (age > (fr.isKey ? keyTimeout : timeout)) {
        return true;
    }

    // Drop frames that are too far behind
    if (lag > C.MAX_FRAME_LAG) {
        return true;
    }

    // Don't drop nearly-complete frames unless very old
    if (completion >= C.COMPLETION_THRESHOLD && age < timeout * 1.5) {
        return false;
    }

    return false;
};

// *** CRITICAL FIX: Always request keyframe when ANY frame is dropped ***
const tryDrop = (id, fr, rt, reason = 'timeout') => {
    if (fr.received === fr.total) {
        processFrame(id, fr, rt);
    } else {
        S.chunks.delete(id);
        S.stats.tDropNet++;

        // Enhanced debug logging
        DEBUG.logDrop(id, reason, {
            received: fr.received,
            total: fr.total,
            completion: ((fr.received / fr.total) * 100).toFixed(1) + '%',
            isKey: fr.isKey,
            age: (rt - fr.firstTime).toFixed(1) + 'ms',
            lag: frameLag(id)
        });

        // *** THE FIX: Always set needKey and request keyframe when ANY frame is dropped ***
        // This prevents subsequent delta frames from being sent to the decoder,
        // which would fail with error -22 because they depend on the dropped frame.
        S.needKey = true;
        reqKey();

        if (fr.isKey) {
            console.warn(`⚠️ Keyframe #${id} dropped! Decoder will wait for new keyframe.`);
        } else {
            console.warn(`⚠️ Delta frame #${id} dropped! Requesting keyframe to resync decoder.`);
        }
    }
};

const processFrame = (fid, fr, rt) => {
    const ct = performance.now();
    if (!fr.parts.every(p => p)) {
        S.chunks.delete(fid);
        S.stats.tDropNet++;
        DEBUG.logDrop(fid, 'chunkMissing', {
            partsNull: fr.parts.map((p, i) => p ? null : i).filter(i => i !== null)
        });
        // Also request keyframe when frame has missing chunks
        S.needKey = true;
        reqKey();
        return;
    }
    const buf = fr.total === 1 ? fr.parts[0] : fr.parts.reduce((a, p) => (a.set(p, a.off), a.off += p.byteLength, a), Object.assign(new Uint8Array(fr.parts.reduce((s, p) => s + p.byteLength, 0)), { off: 0 }));
    S.stats.recv++; S.stats.tRecv++; if (fid > S.lastFrameId) S.lastFrameId = fid;
    let netMs = S.rtt / 2; if (S.clockSync) { const c = (toSrvUs(fr.firstTime) - (fr.capTs + fr.encMs * 1000)) / 1000; if (c >= 0 && c < 1000) netMs = c; }
    updJitter(ct, fr.capTs, S.lastProcessedCapTs || 0); S.lastProcessedCapTs = fr.capTs;
    const data = { buf, capTs: fr.capTs, encMs: fr.encMs, netMs, isKey: fr.isKey, fcT: ct, fId: fid };
    const onFrame = () => { decodeFrame(data); if (waitFirstFrame && isLoadingVisible()) { waitFirstFrame = false; hideLoading(); hasConnected = true; } };
    S.ready ? onFrame() : fr.isKey && (function try_() { S.ready ? onFrame() : setTimeout(try_, 5); })();
    S.chunks.delete(fid);

    // Log completion
    DEBUG.logComplete(fid);
};

const handleMsg = e => {
    const rt = performance.now(); if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) return;
    const v = new DataView(e.data), mg = v.getUint32(0, true), len = e.data.byteLength;

    if (mg === MSG.AUTH_RESPONSE) return handleAuthResp(e.data);
    if (mg === MSG.PING && len === 24) {
        const cs = v.getBigUint64(8, true), st = v.getBigUint64(16, true), cr = BigInt(tsUs());
        S.rtt = Number(cr - cs) / 1000; S.clockSamples.push(Number(cr - (st + (cr - cs) / 2n))); S.clockSamples.length > 8 && S.clockSamples.shift();
        S.clockOff = [...S.clockSamples].sort((a, b) => a - b)[S.clockSamples.length >> 1];
        if (S.clockSamples.length >= 3 && !S.clockSync) S.clockSync = true;

        // Log RTT changes for debugging (only if very high)
        if (DEBUG.enabled && S.rtt > 200) {
            console.warn(`⚠️ High RTT: ${S.rtt.toFixed(1)}ms`);
        }
        return;
    }
    if (mg === MSG.HOST_INFO && len === 6) { S.hostFps = v.getUint16(4, true); updateFpsOpts();
        if (!S.fpsSent) setTimeout(() => applyFps(selDefFps()), 50);
        if (isLoadingVisible()) { updateLoadingStage(Stage.STREAM); waitFirstFrame = true; } return; }
    if (mg === MSG.FPS_ACK && len === 7) { S.currentFps = v.getUint16(4, true); S.currentFpsMode = v.getUint8(6); return; }
    if (mg === MSG.MONITOR_LIST && len >= 6) return parseMonList(e.data);
    if (mg === MSG.AUDIO_DATA && len >= 16) return handleAudioPkt(e.data);
    if (len < C.HEADER) return;

    S.stats.bytes += len; S.stats.tBytes += len;
    const cap = Number(v.getBigUint64(0, true)), enc = v.getUint32(8, true), fid = v.getUint32(12, true);
    const cidx = v.getUint16(16, true), tot = v.getUint16(18, true), typ = v.getUint8(20), chunk = new Uint8Array(e.data, C.HEADER);

    // Track newest frame ID for lag calculation
    if (isNewer(fid, S.newestFrameId) || S.newestFrameId === 0) {
        S.newestFrameId = fid;
    }

    // Track chunk for debugging
    DEBUG.trackChunk(fid, cidx, tot, !S.chunks.has(fid));
    if (typ === 1 && DEBUG.frameChunks.has(fid)) {
        DEBUG.frameChunks.get(fid).isKey = true;
    }

    // Check for stale frames (arrived after we've already processed them)
    if (S.lastFrameId > 0 && !isNewer(fid, S.lastFrameId) && fid !== S.lastFrameId) {
        // Don't log every stale chunk - it's normal for some to arrive late
        if (DEBUG.verbose) {
            DEBUG.logDrop(fid, 'stale', {
                lastFrameId: S.lastFrameId,
                frameDelta: S.lastFrameId - fid
            });
        }
        return;
    }

    // Smart frame cleanup - only drop frames that should be dropped
    for (const [id, fr] of S.chunks) {
        if (fr.received < fr.total && shouldDropFrame(id, fr, rt, S.newestFrameId)) {
            const age = rt - fr.firstTime;
            const reason = age > C.FRAME_TIMEOUT_MS ? 'timeout' :
                          frameLag(id) > C.MAX_FRAME_LAG ? 'outOfOrder' : 'timeout';
            tryDrop(id, fr, rt, reason);
        }
    }

    if (!S.chunks.has(fid)) {
        // Create new frame entry
        S.chunks.set(fid, { parts: Array(tot).fill(null), total: tot, received: 0, capTs: cap, encMs: enc / 1000, firstTime: rt, isKey: typ === 1 });

        // Buffer overflow protection - only if we really have too many frames
        if (S.chunks.size > C.MAX_FRAMES) {
            let did = null, dage = 0, dcompletion = 1;

            // Find the best candidate to drop:
            // Prefer: non-keyframes, older frames, less complete frames
            for (const [id, fr] of S.chunks) {
                if (id !== fid && fr.received !== fr.total) {
                    const a = rt - fr.firstTime;
                    const comp = fr.received / fr.total;

                    // Prioritize dropping: old, incomplete, non-keyframe
                    const score = a * (1 - comp) * (fr.isKey ? 0.5 : 1);
                    const currentScore = dage * (1 - dcompletion) * (S.chunks.get(did)?.isKey ? 0.5 : 1);

                    if (!did || score > currentScore) {
                        dage = a;
                        dcompletion = comp;
                        did = id;
                    }
                }
            }

            if (did) {
                const fr = S.chunks.get(did);
                tryDrop(did, fr, rt, 'bufferFull');
            }
        }
    }

    const fr = S.chunks.get(fid); if (!fr || fr.parts[cidx]) return;
    fr.parts[cidx] = chunk; fr.received++;
    if (fr.received === fr.total) processFrame(fid, fr, rt);
};

const setupDC = () => {
    S.dc.binaryType = 'arraybuffer';
    S.dc.onopen = async () => {
        S.fpsSent = S.authenticated = false;
        updateLoadingStage(Stage.AUTH);
        console.info('Data channel opened');

        // Log connection quality hints
        console.info('📡 Connection established. Monitoring for frame drops...');
        DEBUG.reset();

        if (!validCreds(creds)) { const s = getSaved(); if (validCreds(s)) creds = s; }
        validCreds(creds) ? sendAuth(creds.username, creds.pin) : showAuthModal();
        await initDecoder(); clearPing();
        pingInterval = setInterval(() => S.dc?.readyState === 'open' && S.dc.send(mkBuf(16, v => { v.setUint32(0, MSG.PING, true); v.setBigUint64(8, BigInt(tsUs()), true); })), C.PING_MS);
    };
    S.dc.onclose = () => { S.fpsSent = S.authenticated = false; clearPing(); };
    S.dc.onerror = e => console.error('DataChannel:', e);
    S.dc.onmessage = handleMsg;
};

const resetState = () => { clearPing(); S.dc?.close(); S.pc?.close(); try { S.decoder?.state !== 'closed' && S.decoder?.close(); } catch {}
    S.dc = S.pc = S.decoder = null; S.ready = S.clockSync = S.fpsSent = S.authenticated = false;
    S.jitter.last = 0; S.jitter.deltas = []; waitFirstFrame = false; S.chunks.clear(); S.lastFrameId = 0; S.newestFrameId = 0; S.lastProcessedCapTs = 0; S.frameMeta.clear();
    DEBUG.reset();
};

const connect = async () => {
    try {
        const s = getSaved(); if (validCreds(s)) creds = s;
        updateLoadingStage(Stage.ICE); resetState();
        const pc = S.pc = new RTCPeerConnection({ iceServers: STUN, iceCandidatePoolSize: 10, bundlePolicy: 'max-bundle', rtcpMuxPolicy: 'require' });
        pc.onicecandidate = e => e.candidate && console.info(`ICE candidate: ${e.candidate.type} (${e.candidate.protocol})`);
        pc.onconnectionstatechange = () => { console.info('Connection:', pc.connectionState);
            if (pc.connectionState === 'connected') connAttempts = 0;
            if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') { connAttempts++;
                if (connAttempts >= 3) showConnModal('Connection failed. Please check the server and try again.');
                else { const delay = Math.min(1000 * Math.pow(1.5, connAttempts), 10000); console.warn(`Reconnecting in ${Math.round(delay)}ms`);
                    showLoading(true); updateLoadingStage(Stage.ERR, 'Reconnecting...'); setTimeout(connect, delay); } } };
        pc.oniceconnectionstatechange = () => { console.info('ICE:', pc.iceConnectionState); if (pc.iceConnectionState === 'failed') updateLoadingStage(Stage.ERR, 'ICE failed'); };
        pc.onicecandidateerror = e => e.errorCode !== 701 && console.error('ICE error:', e.errorCode, e.errorText);

        S.dc = pc.createDataChannel('screen', C.DC); setupDC();
        await pc.setLocalDescription(await pc.createOffer());
        updateLoadingStage(Stage.SIGNAL, 'Gathering ICE...');

        await new Promise(r => { if (pc.iceGatheringState === 'complete') return r(); const to = setTimeout(r, 5000);
            pc.addEventListener('icegatheringstatechange', () => pc.iceGatheringState === 'complete' && (clearTimeout(to), r())); });

        updateLoadingStage(Stage.SIGNAL, 'Sending offer...'); console.info('Sending offer to', baseUrl);
        const res = await fetch(`${baseUrl}/api/offer`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sdp: pc.localDescription.sdp, type: pc.localDescription.type }) });
        if (!res.ok) throw new Error('Server rejected offer');
        const ans = await res.json(); console.info('Received answer');
        updateLoadingStage(Stage.CONNECT); await pc.setRemoteDescription(new RTCSessionDescription(ans));
    } catch (e) { console.error('Connect error:', e.message); throw e; }
};

export const detectFps = async () => { if (window.screen?.refreshRate) return Math.round(window.screen.refreshRate);
    return new Promise(r => { let f = 0, lt = performance.now(); const s = [];
        const m = now => { if (++f > 1) s.push(now - lt); lt = now; s.length < 30 ? requestAnimationFrame(m)
            : r([30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 240, 360].find(rt => Math.abs(rt - Math.round(1000 / s.sort((a, b) => a - b)[s.length >> 1])) <= 5) || Math.round(1000 / s[s.length >> 1])); };
        requestAnimationFrame(m); }); };

export const cleanup = () => { clearPing(); S.dc?.close(); S.pc?.close(); };

(async () => {
    setNetCbs(applyFps, sendMonSel); S.clientFps = await detectFps(); updateFpsOpts(); loadConnSettings();
    baseUrl = window.location.origin; connEl.url.value = baseUrl; showLoading(false);
    try { await connect(); } catch (e) { showConnModal('Connection failed: ' + e.message); }
    setInterval(updateStats, 1000);
})();

window.onbeforeunload = () => { cleanup(); closeAudio(); };
