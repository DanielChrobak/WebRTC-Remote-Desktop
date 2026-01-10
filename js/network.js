/**
 * @file network.js
 * @brief WebRTC connection management and signaling
 * @copyright 2025-2026 Daniel Chrobak
 */

import './renderer.js';
import { setKeyboardLockFns } from './input.js';
import { MSG, C, S, $, mkBuf, Stage } from './state.js';
import { handleAudioPkt, closeAudio, initDecoder, decodeFrame, setReqKeyFn } from './media.js';
import { updateStats, updateMonOpts, updateFpsOpts, setNetCbs, updateLoadingStage, showLoading, hideLoading, isLoadingVisible, isKeyboardLocked, exitFullscreen } from './ui.js';

setKeyboardLockFns(isKeyboardLocked, exitFullscreen);

let baseUrl = '';
const AUTH_KEY = 'remote_desktop_auth', CONN_KEY = 'remote_desktop_connection';

// Optimized: No STUN for LAN, only use if needed for NAT traversal
const STUN_SERVERS = [
    { urls: 'stun:stun.l.google.com:19302' },
    { urls: 'stun:stun1.l.google.com:19302' }
];

const tsUs = (t = performance.now()) => Math.floor((performance.timeOrigin + t) * 1000);
const toSrvUs = t => tsUs(t) - S.clockOff;
const sendMsg = buf => { if (S.dc?.readyState !== 'open') return false; try { S.dc.send(buf); return true; } catch { return false; } };

let creds = null, authResolve = null, authReject = null, hasConnected = false, waitFirstFrame = false, connAttempts = 0, pingInterval = null;
let useStunServers = false; // Start without STUN for faster LAN connections

const connEl = { overlay: $('connectOverlay'), url: $('localUrlInput'), btn: $('connectLocalBtn'), err: $('connectError') };
const authEl = { overlay: $('authOverlay'), user: $('usernameInput'), pin: $('pinInput'), err: $('authError'), btn: $('authSubmit') };

const validUser = u => u?.length >= 3 && u.length <= 32 && /^[a-zA-Z0-9_-]+$/.test(u);
const validPin = p => p?.length === 6 && /^\d{6}$/.test(p);
const validCreds = c => c && validUser(c.username) && validPin(c.pin);

const getSaved = () => { try { return JSON.parse(localStorage.getItem(AUTH_KEY)); } catch { return null; } };
const saveCreds = (u, p) => { try { localStorage.setItem(AUTH_KEY, JSON.stringify({ username: u, pin: p })); } catch {} };
const clearCreds = () => { try { localStorage.removeItem(AUTH_KEY); } catch {} };
const clearPing = () => { if (pingInterval) { clearInterval(pingInterval); pingInterval = null; } };

const loadConnSettings = () => { try { const s = JSON.parse(localStorage.getItem(CONN_KEY)); if (s?.localUrl) connEl.url.value = s.localUrl; } catch {} };
const saveConnSettings = () => { try { localStorage.setItem(CONN_KEY, JSON.stringify({ localUrl: connEl.url.value })); } catch {} };

const showConnModal = (err = '') => { connEl.err.textContent = err; connEl.overlay.classList.add('visible'); hideLoading(); };
const hideConnModal = () => { connEl.overlay.classList.remove('visible'); connEl.err.textContent = ''; };

const setAuthErr = (err, el) => { authEl.err.textContent = err; [authEl.user, authEl.pin].forEach(e => e.classList.toggle('error', e === el)); el?.focus(); };
const showAuthModal = (err = '') => { authEl.user.value = authEl.pin.value = ''; setAuthErr(err, err ? authEl.user : null); authEl.overlay.classList.add('visible'); authEl.btn.disabled = false; setTimeout(() => authEl.user.focus(), 100); };
const hideAuthModal = () => { authEl.overlay.classList.remove('visible'); setAuthErr('', null); };

const sendAuth = (u, p) => {
    if (S.dc?.readyState !== 'open') return console.error('DC not open for auth'), false;
    const ub = new TextEncoder().encode(u), pb = new TextEncoder().encode(p);
    const buf = new ArrayBuffer(6 + ub.length + pb.length), v = new DataView(buf);
    v.setUint32(0, MSG.AUTH_REQUEST, true);
    v.setUint8(4, ub.length);
    v.setUint8(5, pb.length);
    new Uint8Array(buf, 6).set(ub);
    new Uint8Array(buf, 6 + ub.length).set(pb);
    try { S.dc.send(buf); console.info('Auth request sent'); return true; } catch (e) { console.error('Auth send failed:', e); return false; }
};

const handleAuthResp = data => {
    if (data.byteLength < 6) return false;
    const v = new DataView(data);
    if (v.getUint32(0, true) !== MSG.AUTH_RESPONSE) return false;

    const ok = v.getUint8(4) === 1, errLen = v.getUint8(5);
    if (ok) { console.info('Auth successful'); S.authenticated = true; hideAuthModal(); }
    else {
        const msg = errLen && data.byteLength >= 6 + errLen ? new TextDecoder().decode(new Uint8Array(data, 6, errLen)) : 'Invalid credentials';
        console.warn('Auth failed:', msg);
        S.authenticated = false;
        clearCreds();
        creds = null;
        showAuthModal(msg);
        authReject?.(new Error(msg));
    }
    authResolve?.(ok);
    authResolve = authReject = null;
    return true;
};

connEl.btn.addEventListener('click', async () => {
    let url = connEl.url.value.trim();
    if (!url) { connEl.err.textContent = 'Please enter a server URL'; return; }
    if (!url.startsWith('http://') && !url.startsWith('https://')) url = 'http://' + url;
    url = url.replace(/\/+$/, '');
    connEl.url.value = url;
    baseUrl = url;
    saveConnSettings();
    hideConnModal();
    showLoading(false);
    updateLoadingStage(Stage.SIGNAL, 'Connecting to server...');
    try { await connect(); } catch (e) { showConnModal('Connection failed: ' + e.message); }
});

authEl.user.addEventListener('input', e => { e.target.value = e.target.value.replace(/[^a-zA-Z0-9_-]/g, '').slice(0, 32); setAuthErr('', null); });
authEl.user.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); authEl.pin.focus(); } });
authEl.pin.addEventListener('input', e => { e.target.value = e.target.value.replace(/\D/g, '').slice(0, 6); setAuthErr('', null); });
authEl.pin.addEventListener('keydown', e => { if (e.key === 'Enter' && validCreds({ username: authEl.user.value, pin: authEl.pin.value })) authEl.btn.click(); });

authEl.btn.addEventListener('click', () => {
    const u = authEl.user.value, p = authEl.pin.value;
    if (!validUser(u)) { setAuthErr('Username must be 3-32 characters', authEl.user); return; }
    if (!validPin(p)) { setAuthErr('PIN must be exactly 6 digits', authEl.pin); return; }
    authEl.btn.disabled = true;
    authEl.err.textContent = '';
    creds = { username: u, pin: p };
    saveCreds(u, p);
    if (S.dc?.readyState === 'open') sendAuth(u, p);
    else { hideAuthModal(); authResolve?.(true); authResolve = authReject = null; }
});

$('disconnectBtn')?.addEventListener('click', () => { cleanup(); hasConnected = false; showConnModal(); });

export const sendMonSel = i => sendMsg(mkBuf(5, v => { v.setUint32(0, MSG.MONITOR_SET, true); v.setUint8(4, i); }));
export const sendFps = (fps, mode) => sendMsg(mkBuf(7, v => { v.setUint32(0, MSG.FPS_SET, true); v.setUint16(4, fps, true); v.setUint8(6, mode); }));
export const reqKey = () => sendMsg(mkBuf(4, v => v.setUint32(0, MSG.REQUEST_KEY, true)));

setReqKeyFn(reqKey);

const updJitter = (t, cap, prev) => {
    if (S.jitter.last > 0 && prev > 0) {
        const d = Math.abs((t - S.jitter.last) - (cap - prev) / 1000);
        if (d < 1000) { S.jitter.deltas.push(d); if (S.jitter.deltas.length > 60) S.jitter.deltas.shift(); }
    }
    S.jitter.last = t;
};

const parseMonList = data => {
    const v = new DataView(data);
    let o = 4;
    const cnt = v.getUint8(o++);
    S.currentMon = v.getUint8(o++);
    S.monitors = Array.from({ length: cnt }, () => {
        const idx = v.getUint8(o++), w = v.getUint16(o, true), h = v.getUint16(o + 2, true), rr = v.getUint16(o + 4, true);
        o += 6;
        const prim = v.getUint8(o++) === 1, nl = v.getUint8(o++), nm = new TextDecoder().decode(new Uint8Array(data, o, nl));
        o += nl;
        return { index: idx, width: w, height: h, refreshRate: rr, isPrimary: prim, name: nm };
    });
    updateMonOpts();
};

export const selDefFps = () => {
    const sel = $('fpsSel');
    if (!sel?.options.length) return S.clientFps;
    const opts = [...sel.options].map(o => +o.value);
    const best = opts.reduce((b, o) => Math.abs(o - S.clientFps) < Math.abs(b - S.clientFps) ? o : b, opts[0]);
    sel.value = opts.includes(S.clientFps) ? S.clientFps : best;
    return +sel.value;
};

export const applyFps = val => {
    const fps = +val, mode = fps === S.hostFps ? 1 : fps === S.clientFps ? 2 : 0;
    if (sendFps(fps, mode)) { S.currentFps = fps; S.currentFpsMode = mode; S.fpsSent = true; }
};

const isNewer = (n, l) => { const d = (n - l) >>> 0; return d > 0 && d < 0x80000000; };

const tryDrop = (id, fr, rt) => {
    if (fr.received === fr.total) processFrame(id, fr, rt);
    else { S.chunks.delete(id); S.stats.tDropNet++; if (fr.isKey) { S.needKey = true; reqKey(); } }
};

const processFrame = (fid, fr, rt) => {
    const ct = performance.now();
    if (!fr.parts.every(p => p)) { S.chunks.delete(fid); S.stats.tDropNet++; return; }

    const buf = fr.total === 1 ? fr.parts[0] : fr.parts.reduce((a, p) => { a.set(p, a.off); a.off += p.byteLength; return a; }, Object.assign(new Uint8Array(fr.parts.reduce((s, p) => s + p.byteLength, 0)), { off: 0 }));

    S.stats.recv++;
    S.stats.tRecv++;
    if (fid > S.lastFrameId) S.lastFrameId = fid;

    let netMs = S.rtt / 2;
    if (S.clockSync) { const c = (toSrvUs(fr.firstTime) - (fr.capTs + fr.encMs * 1000)) / 1000; if (c >= 0 && c < 1000) netMs = c; }

    updJitter(ct, fr.capTs, S.lastProcessedCapTs || 0);
    S.lastProcessedCapTs = fr.capTs;

    const data = { buf, capTs: fr.capTs, encMs: fr.encMs, netMs, isKey: fr.isKey, fcT: ct, fId: fid };

    const onFrame = () => {
        decodeFrame(data);
        if (waitFirstFrame && isLoadingVisible()) { waitFirstFrame = false; hideLoading(); hasConnected = true; }
    };

    if (S.ready) onFrame();
    else if (fr.isKey) (function tryAgain() { if (S.ready) onFrame(); else setTimeout(tryAgain, 5); })();

    S.chunks.delete(fid);
};

const handleMsg = e => {
    const rt = performance.now();
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) return;

    const v = new DataView(e.data), mg = v.getUint32(0, true), len = e.data.byteLength;

    if (mg === MSG.AUTH_RESPONSE) return handleAuthResp(e.data);

    if (mg === MSG.PING && len === 24) {
        const cs = v.getBigUint64(8, true), st = v.getBigUint64(16, true), cr = BigInt(tsUs());
        S.rtt = Number(cr - cs) / 1000;
        S.clockSamples.push(Number(cr - (st + (cr - cs) / 2n)));
        if (S.clockSamples.length > 8) S.clockSamples.shift();
        S.clockOff = [...S.clockSamples].sort((a, b) => a - b)[S.clockSamples.length >> 1];
        if (S.clockSamples.length >= 3 && !S.clockSync) S.clockSync = true;
        return;
    }

    if (mg === MSG.HOST_INFO && len === 6) {
        S.hostFps = v.getUint16(4, true);
        updateFpsOpts();
        if (!S.fpsSent) setTimeout(() => applyFps(selDefFps()), 50);
        if (isLoadingVisible()) { updateLoadingStage(Stage.STREAM); waitFirstFrame = true; }
        return;
    }

    if (mg === MSG.FPS_ACK && len === 7) { S.currentFps = v.getUint16(4, true); S.currentFpsMode = v.getUint8(6); return; }
    if (mg === MSG.MONITOR_LIST && len >= 6) return parseMonList(e.data);
    if (mg === MSG.AUDIO_DATA && len >= 16) return handleAudioPkt(e.data);
    if (len < C.HEADER) return;

    S.stats.bytes += len;
    S.stats.tBytes += len;

    const cap = Number(v.getBigUint64(0, true)), enc = v.getUint32(8, true), fid = v.getUint32(12, true);
    const cidx = v.getUint16(16, true), tot = v.getUint16(18, true), typ = v.getUint8(20);
    const chunk = new Uint8Array(e.data, C.HEADER);

    if (S.lastFrameId > 0 && !isNewer(fid, S.lastFrameId) && fid !== S.lastFrameId) return;

    for (const [id, fr] of S.chunks) if (fr.received < fr.total && rt - fr.firstTime > C.FRAME_TIMEOUT_MS) tryDrop(id, fr, rt);

    if (!S.chunks.has(fid)) {
        for (const [id, fr] of S.chunks) if (isNewer(fid, id) && fr.received < fr.total) tryDrop(id, fr, rt);
        S.chunks.set(fid, { parts: Array(tot).fill(null), total: tot, received: 0, capTs: cap, encMs: enc / 1000, firstTime: rt, isKey: typ === 1 });

        if (S.chunks.size > C.MAX_FRAMES) {
            let did = null, dage = 0;
            for (const [id, fr] of S.chunks) if (id !== fid && fr.received !== fr.total) { const a = rt - fr.firstTime; if (a > dage && !fr.isKey) { dage = a; did = id; } }
            if (!did) for (const [id, fr] of S.chunks) if (id !== fid && fr.received !== fr.total) { const a = rt - fr.firstTime; if (a > dage) { dage = a; did = id; } }
            if (did) tryDrop(did, S.chunks.get(did), rt);
        }
    }

    const fr = S.chunks.get(fid);
    if (!fr || fr.parts[cidx]) return;
    fr.parts[cidx] = chunk;
    fr.received++;
    if (fr.received === fr.total) processFrame(fid, fr, rt);
};

const setupDC = () => {
    S.dc.binaryType = 'arraybuffer';

    S.dc.onopen = async () => {
        S.fpsSent = S.authenticated = false;
        updateLoadingStage(Stage.AUTH);
        console.info('Data channel opened');

        if (!validCreds(creds)) { const s = getSaved(); if (validCreds(s)) creds = s; }
        if (validCreds(creds)) sendAuth(creds.username, creds.pin);
        else showAuthModal();

        await initDecoder();
        clearPing();
        pingInterval = setInterval(() => { if (S.dc?.readyState === 'open') S.dc.send(mkBuf(16, v => { v.setUint32(0, MSG.PING, true); v.setBigUint64(8, BigInt(tsUs()), true); })); }, C.PING_MS);
    };

    S.dc.onclose = () => { S.fpsSent = S.authenticated = false; clearPing(); };
    S.dc.onerror = e => console.error('DataChannel:', e);
    S.dc.onmessage = handleMsg;
};

const resetState = () => {
    clearPing();
    S.dc?.close();
    S.pc?.close();
    try { if (S.decoder?.state !== 'closed') S.decoder?.close(); } catch {}
    S.dc = S.pc = S.decoder = null;
    S.ready = S.clockSync = S.fpsSent = S.authenticated = false;
    S.jitter.last = 0;
    S.jitter.deltas = [];
    waitFirstFrame = false;
    S.chunks.clear();
    S.lastFrameId = 0;
    S.lastProcessedCapTs = 0;
    S.frameMeta.clear();
};

// Check if we're on a local network
const isLocalNetwork = () => {
    const host = window.location.hostname;
    return host === 'localhost' ||
           host === '127.0.0.1' ||
           host.startsWith('192.168.') ||
           host.startsWith('10.') ||
           /^172\.(1[6-9]|2[0-9]|3[01])\./.test(host);
};

const connect = async () => {
    try {
        const s = getSaved();
        if (validCreds(s)) creds = s;

        updateLoadingStage(Stage.ICE);
        resetState();

        // OPTIMIZATION: Skip STUN for local networks - much faster!
        const isLocal = isLocalNetwork();
        const iceServers = isLocal ? [] : STUN_SERVERS;

        console.info(`Connection mode: ${isLocal ? 'LAN (no STUN)' : 'WAN (with STUN)'}`);

        const pc = S.pc = new RTCPeerConnection({
            iceServers,
            iceCandidatePoolSize: 4,  // Reduced for faster gathering
            bundlePolicy: 'max-bundle',
            rtcpMuxPolicy: 'require'
        });

        let hasHostCandidate = false;
        let iceCandidateCount = 0;

        pc.onicecandidate = e => {
            if (e.candidate) {
                iceCandidateCount++;
                if (e.candidate.type === 'host') hasHostCandidate = true;
                console.info(`ICE candidate ${iceCandidateCount}: ${e.candidate.type} (${e.candidate.protocol})`);
            }
        };

        pc.onconnectionstatechange = () => {
            console.info('Connection:', pc.connectionState);
            if (pc.connectionState === 'connected') {
                connAttempts = 0;
                useStunServers = false; // Reset for next connection
            }
            if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
                if (authEl.overlay.classList.contains('visible')) {
                    console.info('Connection closed during auth - waiting for user');
                    return;
                }
                connAttempts++;

                // If first attempt failed without STUN, try with STUN
                if (connAttempts === 1 && !useStunServers && isLocal) {
                    console.warn('LAN connection failed, retrying with STUN...');
                    useStunServers = true;
                    showLoading(true);
                    updateLoadingStage(Stage.ICE, 'Retrying with STUN...');
                    setTimeout(connect, 100);
                    return;
                }

                if (connAttempts >= 3) {
                    showConnModal('Connection failed. Please check the server and try again.');
                } else {
                    const delay = Math.min(1000 * Math.pow(1.5, connAttempts), 10000);
                    console.warn(`Reconnecting in ${Math.round(delay)}ms`);
                    showLoading(true);
                    updateLoadingStage(Stage.ERR, 'Reconnecting...');
                    setTimeout(connect, delay);
                }
            }
        };

        pc.oniceconnectionstatechange = () => {
            console.info('ICE:', pc.iceConnectionState);
            if (pc.iceConnectionState === 'failed') {
                updateLoadingStage(Stage.ERR, 'ICE failed');
            }
        };

        pc.onicecandidateerror = e => {
            if (e.errorCode !== 701) console.error('ICE error:', e.errorCode, e.errorText);
        };

        S.dc = pc.createDataChannel('screen', C.DC);
        setupDC();

        // Create offer immediately
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);

        updateLoadingStage(Stage.SIGNAL, 'Gathering ICE...');

        // OPTIMIZATION: Don't wait for complete gathering!
        // Use trickle ICE - wait just enough for host candidates (fastest)
        // For LAN: 50-150ms is usually enough for host candidates
        // For WAN: Wait a bit longer for server reflexive candidates
        const gatherTimeout = isLocal ? 150 : 500;

        await new Promise(resolve => {
            let resolved = false;

            const done = () => {
                if (resolved) return;
                resolved = true;
                resolve();
            };

            // Quick timeout - don't wait for all candidates
            const quickTimeout = setTimeout(() => {
                if (hasHostCandidate || iceCandidateCount > 0) {
                    console.info(`Fast gather: ${iceCandidateCount} candidates in ${gatherTimeout}ms`);
                    done();
                }
            }, gatherTimeout);

            // Fallback timeout
            const fallbackTimeout = setTimeout(() => {
                console.info(`Gather timeout: ${iceCandidateCount} candidates`);
                done();
            }, isLocal ? 300 : 1000);

            // If gathering completes early, use it
            if (pc.iceGatheringState === 'complete') {
                clearTimeout(quickTimeout);
                clearTimeout(fallbackTimeout);
                done();
                return;
            }

            pc.addEventListener('icegatheringstatechange', () => {
                if (pc.iceGatheringState === 'complete') {
                    clearTimeout(quickTimeout);
                    clearTimeout(fallbackTimeout);
                    console.info(`Gather complete: ${iceCandidateCount} candidates`);
                    done();
                }
            });
        });

        updateLoadingStage(Stage.SIGNAL, 'Sending offer...');
        console.info('Sending offer to', baseUrl);

        // Send offer with current candidates (trickle ICE style)
        const res = await fetch(`${baseUrl}/api/offer`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                sdp: pc.localDescription.sdp,
                type: pc.localDescription.type
            })
        });

        if (!res.ok) throw new Error('Server rejected offer');

        const ans = await res.json();
        console.info('Received answer');
        updateLoadingStage(Stage.CONNECT);
        await pc.setRemoteDescription(new RTCSessionDescription(ans));

    } catch (e) {
        console.error('Connect error:', e.message);
        throw e;
    }
};

export const detectFps = async () => {
    if (window.screen?.refreshRate) return Math.round(window.screen.refreshRate);

    return new Promise(r => {
        let f = 0, lt = performance.now();
        const s = [];
        const m = now => {
            if (++f > 1) s.push(now - lt);
            lt = now;
            if (s.length < 30) requestAnimationFrame(m);
            else {
                const sorted = s.sort((a, b) => a - b), median = sorted[sorted.length >> 1], calculated = Math.round(1000 / median);
                const common = [30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 240, 360];
                r(common.find(rt => Math.abs(rt - calculated) <= 5) || calculated);
            }
        };
        requestAnimationFrame(m);
    });
};

export const cleanup = () => { clearPing(); S.dc?.close(); S.pc?.close(); };

(async () => {
    setNetCbs(applyFps, sendMonSel);
    S.clientFps = await detectFps();
    updateFpsOpts();
    loadConnSettings();

    const isLocal = window.location.hostname === 'localhost' || window.location.hostname === '127.0.0.1' || /^192\.168\.|^10\.|^172\.(1[6-9]|2[0-9]|3[01])\./.test(window.location.hostname);

    if (isLocal) {
        console.info('Detected local server, connecting...');
        baseUrl = window.location.origin;
        connEl.url.value = baseUrl;
        showLoading(false);
        try { await connect(); } catch (e) { showConnModal('Connection failed: ' + e.message); }
    } else {
        console.info('Not on local server, showing connection modal');
        showConnModal();
    }

    setInterval(updateStats, 1000);
})();

window.onbeforeunload = () => { cleanup(); closeAudio(); };
