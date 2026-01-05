import './renderer.js';
import './input.js';
import { MSG, C, S, ConnectionStage, resetIceStats } from './state.js';
import { handleAudioPkt, closeAudio, initDecoder, decodeFrame, setReqKeyFn } from './media.js';
import { updateStats, updateMonOpts, updateFpsOpts, setNetCbs, updateLoadingStage, showLoading, hideLoading, isLoadingVisible } from './ui.js';
import { handleClipboardMessage, setClipboardSendFn, startClipboardMonitor, syncLocalClipboard } from './clipboard.js';

const $ = id => document.getElementById(id);
const tsUs = (t = performance.now()) => Math.floor((performance.timeOrigin + t) * 1000);
const toSrvUs = t => tsUs(t) - S.clockOff;
const mkBuf = (sz, fn) => { const b = new ArrayBuffer(sz), v = new DataView(b); fn(v); return b; };
const sendMsg = buf => { if (S.dc?.readyState !== 'open') return false; try { S.dc.send(buf); return true; } catch { return false; } };

const AUTH_KEY = 'remote_desktop_auth';
let currentCreds = null, authResolve = null, authRejectFn = null;
let cachedIce = null, iceFetched = false, hasConnected = false, waitFirstFrame = false, connAttempts = 0, pingInterval = null;
const MAX_DELAY = 10000, STUN_FALLBACK = [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun1.l.google.com:19302' }];

const authEl = { overlay: $('authOverlay'), user: $('usernameInput'), pin: $('pinInput'), err: $('authError'), btn: $('authSubmit') };
const validUser = u => u?.length >= 3 && u.length <= 32 && /^[a-zA-Z0-9_-]+$/.test(u);
const validPin = p => p?.length === 6 && /^\d{6}$/.test(p);
const getSaved = () => { try { return JSON.parse(localStorage.getItem(AUTH_KEY)); } catch { return null; } };
const saveCreds = (u, p) => { try { localStorage.setItem(AUTH_KEY, JSON.stringify({ username: u, pin: p })); } catch {} };
const clearCreds = () => { try { localStorage.removeItem(AUTH_KEY); } catch {} };
const clearPing = () => { pingInterval && (clearInterval(pingInterval), pingInterval = null); };
const validCreds = c => c && validUser(c.username) && validPin(c.pin);

const setAuthErr = (err, el) => { authEl.err.textContent = err; [authEl.user, authEl.pin].forEach(e => e.classList.toggle('error', e === el)); el?.focus(); };

const showAuthModal = (err = '') => {
    authEl.user.value = authEl.pin.value = '';
    setAuthErr(err, err ? authEl.user : null);
    authEl.overlay.classList.add('visible');
    authEl.btn.disabled = false;
    setTimeout(() => authEl.user.focus(), 100);
};

const hideAuthModal = () => { authEl.overlay.classList.remove('visible'); setAuthErr('', null); };

const sendAuth = (username, pin) => {
    if (S.dc?.readyState !== 'open') return console.error('Data channel not open for auth'), false;
    const ub = new TextEncoder().encode(username), pb = new TextEncoder().encode(pin);
    const buf = new ArrayBuffer(6 + ub.length + pb.length), v = new DataView(buf);
    v.setUint32(0, MSG.AUTH_REQUEST, true); v.setUint8(4, ub.length); v.setUint8(5, pb.length);
    new Uint8Array(buf, 6).set(ub); new Uint8Array(buf, 6 + ub.length).set(pb);
    try { S.dc.send(buf); console.info('Auth request sent over WebRTC data channel'); return true; }
    catch (e) { console.error('Failed to send auth request:', e); return false; }
};

const handleAuthResponse = data => {
    if (data.byteLength < 6) return false;
    const v = new DataView(data);
    if (v.getUint32(0, true) !== MSG.AUTH_RESPONSE) return false;
    const success = v.getUint8(4) === 1, errLen = v.getUint8(5);
    if (success) {
        console.info('Authentication successful (WebRTC)');
        S.authenticated = true; hideAuthModal();
    } else {
        const errMsg = errLen && data.byteLength >= 6 + errLen ? new TextDecoder().decode(new Uint8Array(data, 6, errLen)) : 'Invalid credentials';
        console.warn('Authentication failed:', errMsg);
        S.authenticated = false; clearCreds(); currentCreds = null;
        showAuthModal(errMsg);
        authRejectFn?.(new Error(errMsg));
    }
    authResolve?.(success); authResolve = authRejectFn = null;
    return true;
};

authEl.user.addEventListener('input', e => { e.target.value = e.target.value.replace(/[^a-zA-Z0-9_-]/g, '').slice(0, 32); setAuthErr('', null); });
authEl.user.addEventListener('keydown', e => e.key === 'Enter' && (e.preventDefault(), authEl.pin.focus()));
authEl.pin.addEventListener('input', e => { e.target.value = e.target.value.replace(/\D/g, '').slice(0, 6); setAuthErr('', null); });
authEl.pin.addEventListener('keydown', e => e.key === 'Enter' && validCreds({ username: authEl.user.value, pin: authEl.pin.value }) && authEl.btn.click());

authEl.btn.addEventListener('click', () => {
    const u = authEl.user.value, p = authEl.pin.value;
    if (!validUser(u)) return setAuthErr('Username must be 3-32 characters (letters, numbers, _, -)', authEl.user);
    if (!validPin(p)) return setAuthErr('PIN must be exactly 6 digits', authEl.pin);
    authEl.btn.disabled = true; authEl.err.textContent = '';
    currentCreds = { username: u, pin: p }; saveCreds(u, p);
    S.dc?.readyState === 'open' ? sendAuth(u, p) : (hideAuthModal(), authResolve?.(true), authResolve = authRejectFn = null);
});

const fetchTurn = async () => {
    if (iceFetched && cachedIce) return cachedIce;
    resetIceStats();
    try {
        console.info('Fetching TURN configuration...');
        const res = await fetch(`${location.origin}/api/turn`);
        if (!res.ok) throw new Error(`Server returned ${res.status}`);
        const cfg = await res.json();
        let servers = [];
        if (cfg.meteredEnabled && cfg.fetchUrl) {
            try {
                console.info('Fetching Metered TURN credentials...');
                S.ice.configSource = 'metered';
                const mr = await fetch(cfg.fetchUrl);
                servers = mr.ok ? await mr.json() : (console.warn('Metered API fetch failed'), S.ice.configSource = 'fallback', cfg.servers || []);
                servers.length && console.info(`Loaded ${servers.length} ICE servers from Metered API`);
            } catch (e) { console.warn('Metered API error:', e.message); S.ice.configSource = 'fallback'; servers = cfg.servers || []; }
        } else { S.ice.configSource = cfg.servers?.length ? 'manual' : 'fallback'; servers = cfg.servers || []; }
        if (!servers.length) { console.warn('No ICE servers configured, using Google STUN'); S.ice.configSource = 'fallback'; servers = STUN_FALLBACK; }
        S.ice.stunServers = servers.filter(s => s.urls?.startsWith('stun:')).length;
        S.ice.turnServers = servers.filter(s => s.urls?.startsWith('turn:') || s.urls?.startsWith('turns:')).length;
        console.info(`ICE servers: ${S.ice.stunServers} STUN, ${S.ice.turnServers} TURN`);
        cachedIce = servers; iceFetched = true;
        return servers;
    } catch (e) {
        console.error('Failed to fetch TURN config:', e.message);
        Object.assign(S.ice, { configSource: 'fallback', stunServers: 2, turnServers: 0 });
        return STUN_FALLBACK;
    }
};

const updateConnType = async pc => {
    try {
        const stats = await pc.getStats();
        for (const [, r] of stats) {
            if (r.type === 'candidate-pair' && r.state === 'succeeded' && r.nominated) {
                let lc = null, rc = null;
                for (const [, x] of stats) {
                    if (x.type === 'local-candidate' && x.id === r.localCandidateId) lc = x;
                    if (x.type === 'remote-candidate' && x.id === r.remoteCandidateId) rc = x;
                }
                if (lc) {
                    const t = lc.candidateType;
                    S.ice.connectionType = t === 'relay' ? 'relay' : t === 'srflx' ? 'stun' : 'direct';
                    S.ice.usingTurn = t === 'relay';
                    console.info(`%cConnection using ${t === 'relay' ? 'TURN relay' : t === 'srflx' ? 'STUN' : 'P2P'}`, `color: ${t === 'relay' ? '#f59e0b' : '#22c55e'}`);
                    S.ice.selectedPair = { local: lc, remote: rc, rtt: r.currentRoundTripTime };
                }
                break;
            }
        }
    } catch (e) { console.warn('Failed to get connection stats:', e.message); }
};

export const sendMonSel = i => sendMsg(mkBuf(5, v => { v.setUint32(0, MSG.MONITOR_SET, true); v.setUint8(4, i); }));
export const sendFps = (fps, mode) => sendMsg(mkBuf(7, v => { v.setUint32(0, MSG.FPS_SET, true); v.setUint16(4, fps, true); v.setUint8(6, mode); }));
export const reqKey = () => sendMsg(mkBuf(4, v => v.setUint32(0, MSG.REQUEST_KEY, true)));
export const sendClipboard = buf => sendMsg(buf instanceof ArrayBuffer ? new Uint8Array(buf) : buf);
setReqKeyFn(reqKey);
setClipboardSendFn(sendClipboard);

const updJitter = (t, cap, prev) => {
    if (S.jitter.last > 0 && prev > 0) {
        const d = Math.abs((t - S.jitter.last) - (cap - prev) / 1000);
        if (d < 1000) { S.jitter.deltas.push(d); S.jitter.deltas.length > 60 && S.jitter.deltas.shift(); }
    }
    S.jitter.last = t;
};

const parseMonList = data => {
    const v = new DataView(data); let o = 4;
    const cnt = v.getUint8(o++); S.currentMon = v.getUint8(o++);
    S.monitors = Array.from({ length: cnt }, () => {
        const idx = v.getUint8(o++), w = v.getUint16(o, true), h = v.getUint16(o + 2, true), rr = v.getUint16(o + 4, true);
        o += 6; const prim = v.getUint8(o++) === 1, nl = v.getUint8(o++), nm = new TextDecoder().decode(new Uint8Array(data, o, nl)); o += nl;
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

const isFrameIdNewer = (newId, lastId) => { const diff = (newId - lastId) >>> 0; return diff > 0 && diff < 0x80000000; };

const tryDropFrame = (id, fr, rt) => {
    if (fr.received === fr.total) processCompleteFrame(id, fr, rt);
    else { S.chunks.delete(id); S.stats.tDropNet++; if (fr.isKey) { S.needKey = true; reqKey(); } }
};

const processCompleteFrame = (fid, fr, rt) => {
    const ct = performance.now();
    if (!fr.parts.every(p => p)) { S.chunks.delete(fid); S.stats.tDropNet++; return; }
    const buf = fr.total === 1 ? fr.parts[0] : fr.parts.reduce((a, p) => (a.set(p, a.off), a.off += p.byteLength, a),
        Object.assign(new Uint8Array(fr.parts.reduce((s, p) => s + p.byteLength, 0)), { off: 0 }));
    S.stats.recv++; S.stats.tRecv++;
    if (fid > S.lastFrameId) S.lastFrameId = fid;
    let netMs = S.rtt / 2;
    if (S.clockSync) { const c = (toSrvUs(fr.firstTime) - (fr.capTs + fr.encMs * 1000)) / 1000; if (c >= 0 && c < 1000) netMs = c; }
    const prevCapTs = S.lastProcessedCapTs || 0;
    updJitter(ct, fr.capTs, prevCapTs);
    S.lastProcessedCapTs = fr.capTs;
    const data = { buf, capTs: fr.capTs, encMs: fr.encMs, netMs, isKey: fr.isKey, fcT: ct, fId: fid };
    const onFrame = () => { decodeFrame(data); if (waitFirstFrame && isLoadingVisible()) { waitFirstFrame = false; hideLoading(); hasConnected = true; } };
    S.ready ? onFrame() : fr.isKey && (function try_() { S.ready ? onFrame() : setTimeout(try_, 5); })();
    S.chunks.delete(fid);
};

const handleMsg = e => {
    const rt = performance.now();
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) return;
    const v = new DataView(e.data), mg = v.getUint32(0, true), len = e.data.byteLength;

    if (mg === MSG.AUTH_RESPONSE) return handleAuthResponse(e.data);
    if (mg === MSG.PING && len === 24) {
        const cs = v.getBigUint64(8, true), st = v.getBigUint64(16, true), cr = BigInt(tsUs());
        S.rtt = Number(cr - cs) / 1000;
        S.clockSamples.push(Number(cr - (st + (cr - cs) / 2n))); S.clockSamples.length > 8 && S.clockSamples.shift();
        S.clockOff = [...S.clockSamples].sort((a, b) => a - b)[S.clockSamples.length >> 1];
        if (S.clockSamples.length >= 3 && !S.clockSync) S.clockSync = true;
        return;
    }
    if (mg === MSG.HOST_INFO && len === 6) {
        S.hostFps = v.getUint16(4, true); updateFpsOpts();
        if (!S.fpsSent) setTimeout(() => applyFps(selDefFps()), 50);
        if (isLoadingVisible()) { updateLoadingStage(ConnectionStage.STREAMING); waitFirstFrame = true; }
        return;
    }
    if (mg === MSG.FPS_ACK && len === 7) { S.currentFps = v.getUint16(4, true); S.currentFpsMode = v.getUint8(6); return; }
    if (mg === MSG.MONITOR_LIST && len >= 6) return parseMonList(e.data);
    if (mg === MSG.AUDIO_DATA && len >= 16) return handleAudioPkt(e.data);
    if (mg === MSG.CLIPBOARD_TEXT || mg === MSG.CLIPBOARD_IMAGE || mg === MSG.CLIPBOARD_ACK) return handleClipboardMessage(e.data);
    if (len < C.HEADER) return;

    S.stats.bytes += len; S.stats.tBytes += len;
    const cap = Number(v.getBigUint64(0, true)), enc = v.getUint32(8, true), fid = v.getUint32(12, true);
    const cidx = v.getUint16(16, true), tot = v.getUint16(18, true), typ = v.getUint8(20);
    const chunk = new Uint8Array(e.data, C.HEADER);

    if (S.lastFrameId > 0 && !isFrameIdNewer(fid, S.lastFrameId) && fid !== S.lastFrameId) return;

    for (const [id, fr] of S.chunks) {
        if (fr.received < fr.total && rt - fr.firstTime > C.FRAME_TIMEOUT_MS) tryDropFrame(id, fr, rt);
    }

    if (!S.chunks.has(fid)) {
        for (const [id, fr] of S.chunks) {
            if (isFrameIdNewer(fid, id) && fr.received < fr.total) tryDropFrame(id, fr, rt);
        }
        S.chunks.set(fid, { parts: Array(tot).fill(null), total: tot, received: 0, capTs: cap, encMs: enc / 1000, firstTime: rt, isKey: typ === 1 });

        if (S.chunks.size > C.MAX_FRAMES) {
            let did = null, dage = 0;
            for (const [id, fr] of S.chunks) {
                if (id !== fid && fr.received !== fr.total) {
                    const a = rt - fr.firstTime;
                    if (a > dage && !fr.isKey) { dage = a; did = id; }
                }
            }
            if (!did) for (const [id, fr] of S.chunks) {
                if (id !== fid && fr.received !== fr.total) {
                    const a = rt - fr.firstTime;
                    if (a > dage) { dage = a; did = id; }
                }
            }
            if (did) tryDropFrame(did, S.chunks.get(did), rt);
        }
    }
    const fr = S.chunks.get(fid);
    if (!fr || fr.parts[cidx]) return;
    fr.parts[cidx] = chunk;
    fr.received++;
    if (fr.received === fr.total) processCompleteFrame(fid, fr, rt);
};

const setupDC = () => {
    S.dc.binaryType = 'arraybuffer';
    S.dc.onopen = async () => {
        S.fpsSent = S.authenticated = false;
        updateLoadingStage(ConnectionStage.AUTHENTICATING);
        console.info('Data channel opened, sending authentication...');
        if (!validCreds(currentCreds)) {
            const saved = getSaved();
            if (validCreds(saved)) currentCreds = saved;
        }
        validCreds(currentCreds) ? sendAuth(currentCreds.username, currentCreds.pin) : showAuthModal();
        await initDecoder();
        clearPing();
        pingInterval = setInterval(() => S.dc?.readyState === 'open' && S.dc.send(mkBuf(16, v => { v.setUint32(0, MSG.PING, true); v.setBigUint64(8, BigInt(tsUs()), true); })), C.PING_MS);
        startClipboardMonitor(); syncLocalClipboard();
    };
    S.dc.onclose = () => { S.fpsSent = S.authenticated = false; clearPing(); };
    S.dc.onerror = e => console.error('DataChannel:', e);
    S.dc.onmessage = handleMsg;
};

const resetState = () => {
    clearPing();
    S.dc?.close(); S.pc?.close(); try { S.decoder?.state !== 'closed' && S.decoder?.close(); } catch {}
    S.dc = S.pc = S.decoder = null;
    S.ready = S.clockSync = S.fpsSent = S.authenticated = false;
    S.jitter.last = 0; S.jitter.deltas = []; waitFirstFrame = false;
    S.chunks.clear(); S.lastFrameId = 0; S.lastProcessedCapTs = 0; S.frameMeta.clear();
    Object.assign(S.ice, { candidates: { host: 0, srflx: 0, relay: 0, prflx: 0 }, connectionType: 'unknown', usingTurn: false, selectedPair: null });
};

export const connect = async () => {
    try {
        const saved = getSaved();
        if (validCreds(saved)) currentCreds = saved;
        showLoading(hasConnected);
        updateLoadingStage(ConnectionStage.ICE_GATHERING);
        resetState();

        const iceServers = await fetchTurn();
        const pc = S.pc = new RTCPeerConnection({ iceServers, iceCandidatePoolSize: 10, bundlePolicy: 'max-bundle', rtcpMuxPolicy: 'require' });

        pc.onicecandidate = e => {
            if (!e.candidate) return;
            const t = e.candidate.type, p = e.candidate.protocol;
            S.ice.candidates[t] = (S.ice.candidates[t] || 0) + 1;
            console.info(t === 'relay' ? `%cTURN relay candidate (${p})` : `${t} candidate (${p})`, t === 'relay' ? 'color: #22c55e' : '');
        };
        pc.onconnectionstatechange = () => {
            console.info('Connection state:', pc.connectionState);
            if (pc.connectionState === 'connected') { connAttempts = 0; setTimeout(() => updateConnType(pc), 500); }
            if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
                connAttempts++;
                const delay = Math.min(1000 * Math.pow(1.5, connAttempts), MAX_DELAY);
                console.warn(`Reconnecting in ${Math.round(delay)}ms (attempt ${connAttempts})`);
                showLoading(true); updateLoadingStage(ConnectionStage.ERROR, `Reconnecting in ${Math.round(delay / 1000)}s...`);
                if (connAttempts >= 3) { cachedIce = null; iceFetched = false; console.info('Cleared ICE server cache'); }
                setTimeout(connect, delay);
            }
        };
        pc.oniceconnectionstatechange = () => {
            console.info('ICE connection state:', pc.iceConnectionState);
            if (pc.iceConnectionState === 'failed') { console.error('ICE connection failed'); updateLoadingStage(ConnectionStage.ERROR, 'ICE connection failed'); }
            if (pc.iceConnectionState === 'connected' || pc.iceConnectionState === 'completed') updateConnType(pc);
        };
        pc.onicecandidateerror = e => e.errorCode !== 701 && console.error('ICE error:', e.errorCode, e.errorText, e.url);

        S.dc = pc.createDataChannel('screen', C.DC);
        setupDC();
        await pc.setLocalDescription(await pc.createOffer());
        updateLoadingStage(ConnectionStage.SIGNALING);

        await new Promise(r => {
            if (pc.iceGatheringState === 'complete') return r();
            const to = setTimeout(r, 5000);
            pc.addEventListener('icegatheringstatechange', () => pc.iceGatheringState === 'complete' && (clearTimeout(to), r()));
        });

        const res = await fetch(`${location.origin}/offer`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sdp: pc.localDescription.sdp, type: pc.localDescription.type }) });
        if (!res.ok) throw new Error(`Server ${res.status}`);
        await pc.setRemoteDescription(new RTCSessionDescription(await res.json()));
        updateLoadingStage(ConnectionStage.CONNECTING);
        console.info('WebRTC connection initiated');
    } catch (e) {
        console.error('Connect:', e.message);
        connAttempts++;
        updateLoadingStage(ConnectionStage.ERROR, e.message);
        setTimeout(connect, Math.min(1000 * Math.pow(1.5, connAttempts), MAX_DELAY));
    }
};

export const detectFps = async () => {
    if (window.screen?.refreshRate) return Math.round(window.screen.refreshRate);
    return new Promise(r => {
        let f = 0, lt = performance.now(); const s = [];
        const m = now => { if (++f > 1) s.push(now - lt); lt = now; s.length < 30 ? requestAnimationFrame(m) : r([30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 240, 360].find(rt => Math.abs(rt - Math.round(1000 / s.sort((a, b) => a - b)[s.length >> 1])) <= 5) || Math.round(1000 / s[s.length >> 1])); };
        requestAnimationFrame(m);
    });
};

export const cleanup = () => { clearPing(); S.dc?.close(); S.pc?.close(); };

(async () => { setNetCbs(applyFps, sendMonSel); S.clientFps = await detectFps(); updateFpsOpts(); connect(); setInterval(updateStats, 1000); })();
window.onbeforeunload = () => { cleanup(); closeAudio(); };
