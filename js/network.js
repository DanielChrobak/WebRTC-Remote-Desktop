/**
 * @file network.js
 * @brief WebRTC connection management and signaling
 * @copyright 2025-2026 Daniel Chrobak
 *
 * Handles WebRTC peer connection establishment, ICE gathering,
 * signaling via HTTP, authentication, and data channel management.
 */

import './renderer.js';
import './input.js';
import { MSG, C, S, $, mkBuf, Stage } from './state.js';
import { handleAudioPkt, closeAudio, initDecoder, decodeFrame, setReqKeyFn } from './media.js';
import {
    updateStats,
    updateMonOpts,
    updateFpsOpts,
    setNetCbs,
    updateLoadingStage,
    showLoading,
    hideLoading,
    isLoadingVisible
} from './ui.js';

/**
 * Base URL for HTTP signaling server
 * @type {string}
 */
let baseUrl = '';

/**
 * LocalStorage keys for persistent settings
 */
const AUTH_KEY = 'remote_desktop_auth';
const CONN_KEY = 'remote_desktop_connection';

/**
 * STUN servers for ICE candidate gathering
 */
const STUN = [
    { urls: 'stun:stun.l.google.com:19302' },
    { urls: 'stun:stun1.l.google.com:19302' }
];

/**
 * Converts performance timestamp to microseconds since Unix epoch
 * @param {number} t - Performance timestamp (defaults to current time)
 * @returns {number} Timestamp in microseconds
 */
const tsUs = (t = performance.now()) => Math.floor((performance.timeOrigin + t) * 1000);

/**
 * Converts local timestamp to server-relative microseconds
 * @param {number} t - Local timestamp
 * @returns {number} Server-relative timestamp
 */
const toSrvUs = t => tsUs(t) - S.clockOff;

/**
 * Sends a message over the data channel
 * @param {ArrayBuffer} buf - Message buffer to send
 * @returns {boolean} True if message was sent successfully
 */
const sendMsg = buf => {
    if (S.dc?.readyState !== 'open') return false;
    try {
        S.dc.send(buf);
        return true;
    } catch {
        return false;
    }
};

/**
 * Authentication state
 */
let creds = null;
let authResolve = null;
let authReject = null;
let hasConnected = false;
let waitFirstFrame = false;
let connAttempts = 0;
let pingInterval = null;

/**
 * Connection modal DOM elements
 */
const connEl = {
    overlay: $('connectOverlay'),
    url: $('localUrlInput'),
    btn: $('connectLocalBtn'),
    err: $('connectError')
};

/**
 * Authentication modal DOM elements
 */
const authEl = {
    overlay: $('authOverlay'),
    user: $('usernameInput'),
    pin: $('pinInput'),
    err: $('authError'),
    btn: $('authSubmit')
};

/**
 * Validates username format
 * @param {string} u - Username to validate
 * @returns {boolean} True if valid
 */
const validUser = u => u?.length >= 3 && u.length <= 32 && /^[a-zA-Z0-9_-]+$/.test(u);

/**
 * Validates PIN format
 * @param {string} p - PIN to validate
 * @returns {boolean} True if valid
 */
const validPin = p => p?.length === 6 && /^\d{6}$/.test(p);

/**
 * Validates complete credentials object
 * @param {Object} c - Credentials object
 * @returns {boolean} True if valid
 */
const validCreds = c => c && validUser(c.username) && validPin(c.pin);

/**
 * Retrieves saved credentials from localStorage
 * @returns {Object|null} Saved credentials or null
 */
const getSaved = () => {
    try {
        return JSON.parse(localStorage.getItem(AUTH_KEY));
    } catch {
        return null;
    }
};

/**
 * Saves credentials to localStorage
 * @param {string} u - Username
 * @param {string} p - PIN
 */
const saveCreds = (u, p) => {
    try {
        localStorage.setItem(AUTH_KEY, JSON.stringify({ username: u, pin: p }));
    } catch {}
};

/**
 * Clears saved credentials from localStorage
 */
const clearCreds = () => {
    try {
        localStorage.removeItem(AUTH_KEY);
    } catch {}
};

/**
 * Clears the ping interval timer
 */
const clearPing = () => {
    if (pingInterval) {
        clearInterval(pingInterval);
        pingInterval = null;
    }
};

/**
 * Loads connection settings from localStorage
 */
const loadConnSettings = () => {
    try {
        const s = JSON.parse(localStorage.getItem(CONN_KEY));
        if (s?.localUrl) {
            connEl.url.value = s.localUrl;
        }
    } catch {}
};

/**
 * Saves connection settings to localStorage
 */
const saveConnSettings = () => {
    try {
        localStorage.setItem(CONN_KEY, JSON.stringify({ localUrl: connEl.url.value }));
    } catch {}
};

/**
 * Shows the connection modal
 * @param {string} err - Error message to display
 */
const showConnModal = (err = '') => {
    connEl.err.textContent = err;
    connEl.overlay.classList.add('visible');
    hideLoading();
};

/**
 * Hides the connection modal
 */
const hideConnModal = () => {
    connEl.overlay.classList.remove('visible');
    connEl.err.textContent = '';
};

/**
 * Sets authentication error state
 * @param {string} err - Error message
 * @param {HTMLElement} el - Element to highlight
 */
const setAuthErr = (err, el) => {
    authEl.err.textContent = err;
    [authEl.user, authEl.pin].forEach(e => e.classList.toggle('error', e === el));
    el?.focus();
};

/**
 * Shows the authentication modal
 * @param {string} err - Error message to display
 */
const showAuthModal = (err = '') => {
    authEl.user.value = authEl.pin.value = '';
    setAuthErr(err, err ? authEl.user : null);
    authEl.overlay.classList.add('visible');
    authEl.btn.disabled = false;
    setTimeout(() => authEl.user.focus(), 100);
};

/**
 * Hides the authentication modal
 */
const hideAuthModal = () => {
    authEl.overlay.classList.remove('visible');
    setAuthErr('', null);
};

/**
 * Sends authentication request over data channel
 * @param {string} u - Username
 * @param {string} p - PIN
 * @returns {boolean} True if request was sent
 */
const sendAuth = (u, p) => {
    if (S.dc?.readyState !== 'open') {
        console.error('DC not open for auth');
        return false;
    }

    const ub = new TextEncoder().encode(u);
    const pb = new TextEncoder().encode(p);
    const buf = new ArrayBuffer(6 + ub.length + pb.length);
    const v = new DataView(buf);

    v.setUint32(0, MSG.AUTH_REQUEST, true);
    v.setUint8(4, ub.length);
    v.setUint8(5, pb.length);
    new Uint8Array(buf, 6).set(ub);
    new Uint8Array(buf, 6 + ub.length).set(pb);

    try {
        S.dc.send(buf);
        console.info('Auth request sent');
        return true;
    } catch (e) {
        console.error('Auth send failed:', e);
        return false;
    }
};

/**
 * Handles authentication response from server
 * @param {ArrayBuffer} data - Response data
 * @returns {boolean} True if response was handled
 */
const handleAuthResp = data => {
    if (data.byteLength < 6) return false;

    const v = new DataView(data);
    if (v.getUint32(0, true) !== MSG.AUTH_RESPONSE) return false;

    const ok = v.getUint8(4) === 1;
    const errLen = v.getUint8(5);

    if (ok) {
        console.info('Auth successful');
        S.authenticated = true;
        hideAuthModal();
    } else {
        const msg = errLen && data.byteLength >= 6 + errLen
            ? new TextDecoder().decode(new Uint8Array(data, 6, errLen))
            : 'Invalid credentials';
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

/**
 * Connection button click handler
 */
connEl.btn.addEventListener('click', async () => {
    let url = connEl.url.value.trim();
    if (!url) {
        connEl.err.textContent = 'Please enter a server URL';
        return;
    }

    if (!url.startsWith('http://') && !url.startsWith('https://')) {
        url = 'http://' + url;
    }

    url = url.replace(/\/+$/, '');
    connEl.url.value = url;
    baseUrl = url;
    saveConnSettings();

    hideConnModal();
    showLoading(false);
    updateLoadingStage(Stage.SIGNAL, 'Connecting to server...');

    try {
        await connect();
    } catch (e) {
        showConnModal('Connection failed: ' + e.message);
    }
});

/**
 * Username input validation
 */
authEl.user.addEventListener('input', e => {
    e.target.value = e.target.value.replace(/[^a-zA-Z0-9_-]/g, '').slice(0, 32);
    setAuthErr('', null);
});

authEl.user.addEventListener('keydown', e => {
    if (e.key === 'Enter') {
        e.preventDefault();
        authEl.pin.focus();
    }
});

/**
 * PIN input validation
 */
authEl.pin.addEventListener('input', e => {
    e.target.value = e.target.value.replace(/\D/g, '').slice(0, 6);
    setAuthErr('', null);
});

authEl.pin.addEventListener('keydown', e => {
    if (e.key === 'Enter' && validCreds({ username: authEl.user.value, pin: authEl.pin.value })) {
        authEl.btn.click();
    }
});

/**
 * Authentication submit handler
 */
authEl.btn.addEventListener('click', () => {
    const u = authEl.user.value;
    const p = authEl.pin.value;

    if (!validUser(u)) {
        setAuthErr('Username must be 3-32 characters', authEl.user);
        return;
    }

    if (!validPin(p)) {
        setAuthErr('PIN must be exactly 6 digits', authEl.pin);
        return;
    }

    authEl.btn.disabled = true;
    authEl.err.textContent = '';
    creds = { username: u, pin: p };
    saveCreds(u, p);

    if (S.dc?.readyState === 'open') {
        sendAuth(u, p);
    } else {
        hideAuthModal();
        authResolve?.(true);
        authResolve = authReject = null;
    }
});

/**
 * Disconnect button handler
 */
$('disconnectBtn')?.addEventListener('click', () => {
    cleanup();
    hasConnected = false;
    showConnModal();
});

/**
 * Sends monitor selection to server
 * @param {number} i - Monitor index
 * @returns {boolean} True if message was sent
 */
export const sendMonSel = i => sendMsg(mkBuf(5, v => {
    v.setUint32(0, MSG.MONITOR_SET, true);
    v.setUint8(4, i);
}));

/**
 * Sends FPS configuration to server
 * @param {number} fps - Target frame rate
 * @param {number} mode - FPS mode (0=custom, 1=host, 2=client)
 * @returns {boolean} True if message was sent
 */
export const sendFps = (fps, mode) => sendMsg(mkBuf(7, v => {
    v.setUint32(0, MSG.FPS_SET, true);
    v.setUint16(4, fps, true);
    v.setUint8(6, mode);
}));

/**
 * Requests a keyframe from server
 * @returns {boolean} True if request was sent
 */
export const reqKey = () => sendMsg(mkBuf(4, v => v.setUint32(0, MSG.REQUEST_KEY, true)));

setReqKeyFn(reqKey);

/**
 * Updates jitter measurement
 * @param {number} t - Current time
 * @param {number} cap - Capture timestamp
 * @param {number} prev - Previous capture timestamp
 */
const updJitter = (t, cap, prev) => {
    if (S.jitter.last > 0 && prev > 0) {
        const d = Math.abs((t - S.jitter.last) - (cap - prev) / 1000);
        if (d < 1000) {
            S.jitter.deltas.push(d);
            if (S.jitter.deltas.length > 60) {
                S.jitter.deltas.shift();
            }
        }
    }
    S.jitter.last = t;
};

/**
 * Parses monitor list message from server
 * @param {ArrayBuffer} data - Message data
 */
const parseMonList = data => {
    const v = new DataView(data);
    let o = 4;
    const cnt = v.getUint8(o++);
    S.currentMon = v.getUint8(o++);

    S.monitors = Array.from({ length: cnt }, () => {
        const idx = v.getUint8(o++);
        const w = v.getUint16(o, true);
        const h = v.getUint16(o + 2, true);
        const rr = v.getUint16(o + 4, true);
        o += 6;
        const prim = v.getUint8(o++) === 1;
        const nl = v.getUint8(o++);
        const nm = new TextDecoder().decode(new Uint8Array(data, o, nl));
        o += nl;
        return { index: idx, width: w, height: h, refreshRate: rr, isPrimary: prim, name: nm };
    });

    updateMonOpts();
};

/**
 * Selects the default FPS option closest to client refresh rate
 * @returns {number} Selected FPS value
 */
export const selDefFps = () => {
    const sel = $('fpsSel');
    if (!sel?.options.length) return S.clientFps;

    const opts = [...sel.options].map(o => +o.value);
    const best = opts.reduce((b, o) =>
        Math.abs(o - S.clientFps) < Math.abs(b - S.clientFps) ? o : b,
        opts[0]
    );

    sel.value = opts.includes(S.clientFps) ? S.clientFps : best;
    return +sel.value;
};

/**
 * Applies FPS setting and sends to server
 * @param {number|string} val - FPS value
 */
export const applyFps = val => {
    const fps = +val;
    const mode = fps === S.hostFps ? 1 : fps === S.clientFps ? 2 : 0;

    if (sendFps(fps, mode)) {
        S.currentFps = fps;
        S.currentFpsMode = mode;
        S.fpsSent = true;
    }
};

/**
 * Checks if frame ID is newer than last processed
 * @param {number} n - New frame ID
 * @param {number} l - Last frame ID
 * @returns {boolean} True if newer
 */
const isNewer = (n, l) => {
    const d = (n - l) >>> 0;
    return d > 0 && d < 0x80000000;
};

/**
 * Attempts to process or drop an incomplete frame
 * @param {number} id - Frame ID
 * @param {Object} fr - Frame data
 * @param {number} rt - Receive time
 */
const tryDrop = (id, fr, rt) => {
    if (fr.received === fr.total) {
        processFrame(id, fr, rt);
    } else {
        S.chunks.delete(id);
        S.stats.tDropNet++;
        if (fr.isKey) {
            S.needKey = true;
            reqKey();
        }
    }
};

/**
 * Processes a complete frame for decoding
 * @param {number} fid - Frame ID
 * @param {Object} fr - Frame data
 * @param {number} rt - Receive time
 */
const processFrame = (fid, fr, rt) => {
    const ct = performance.now();

    if (!fr.parts.every(p => p)) {
        S.chunks.delete(fid);
        S.stats.tDropNet++;
        return;
    }

    const buf = fr.total === 1
        ? fr.parts[0]
        : fr.parts.reduce((a, p) => {
            a.set(p, a.off);
            a.off += p.byteLength;
            return a;
        }, Object.assign(new Uint8Array(fr.parts.reduce((s, p) => s + p.byteLength, 0)), { off: 0 }));

    S.stats.recv++;
    S.stats.tRecv++;

    if (fid > S.lastFrameId) {
        S.lastFrameId = fid;
    }

    let netMs = S.rtt / 2;
    if (S.clockSync) {
        const c = (toSrvUs(fr.firstTime) - (fr.capTs + fr.encMs * 1000)) / 1000;
        if (c >= 0 && c < 1000) {
            netMs = c;
        }
    }

    updJitter(ct, fr.capTs, S.lastProcessedCapTs || 0);
    S.lastProcessedCapTs = fr.capTs;

    const data = {
        buf,
        capTs: fr.capTs,
        encMs: fr.encMs,
        netMs,
        isKey: fr.isKey,
        fcT: ct,
        fId: fid
    };

    const onFrame = () => {
        decodeFrame(data);
        if (waitFirstFrame && isLoadingVisible()) {
            waitFirstFrame = false;
            hideLoading();
            hasConnected = true;
        }
    };

    if (S.ready) {
        onFrame();
    } else if (fr.isKey) {
        (function tryAgain() {
            if (S.ready) {
                onFrame();
            } else {
                setTimeout(tryAgain, 5);
            }
        })();
    }

    S.chunks.delete(fid);
};

/**
 * Handles incoming data channel messages
 * @param {MessageEvent} e - Message event
 */
const handleMsg = e => {
    const rt = performance.now();

    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) {
        return;
    }

    const v = new DataView(e.data);
    const mg = v.getUint32(0, true);
    const len = e.data.byteLength;

    if (mg === MSG.AUTH_RESPONSE) {
        return handleAuthResp(e.data);
    }

    if (mg === MSG.PING && len === 24) {
        const cs = v.getBigUint64(8, true);
        const st = v.getBigUint64(16, true);
        const cr = BigInt(tsUs());

        S.rtt = Number(cr - cs) / 1000;
        S.clockSamples.push(Number(cr - (st + (cr - cs) / 2n)));

        if (S.clockSamples.length > 8) {
            S.clockSamples.shift();
        }

        S.clockOff = [...S.clockSamples].sort((a, b) => a - b)[S.clockSamples.length >> 1];

        if (S.clockSamples.length >= 3 && !S.clockSync) {
            S.clockSync = true;
        }
        return;
    }

    if (mg === MSG.HOST_INFO && len === 6) {
        S.hostFps = v.getUint16(4, true);
        updateFpsOpts();

        if (!S.fpsSent) {
            setTimeout(() => applyFps(selDefFps()), 50);
        }

        if (isLoadingVisible()) {
            updateLoadingStage(Stage.STREAM);
            waitFirstFrame = true;
        }
        return;
    }

    if (mg === MSG.FPS_ACK && len === 7) {
        S.currentFps = v.getUint16(4, true);
        S.currentFpsMode = v.getUint8(6);
        return;
    }

    if (mg === MSG.MONITOR_LIST && len >= 6) {
        return parseMonList(e.data);
    }

    if (mg === MSG.AUDIO_DATA && len >= 16) {
        return handleAudioPkt(e.data);
    }

    if (len < C.HEADER) {
        return;
    }

    S.stats.bytes += len;
    S.stats.tBytes += len;

    const cap = Number(v.getBigUint64(0, true));
    const enc = v.getUint32(8, true);
    const fid = v.getUint32(12, true);
    const cidx = v.getUint16(16, true);
    const tot = v.getUint16(18, true);
    const typ = v.getUint8(20);
    const chunk = new Uint8Array(e.data, C.HEADER);

    if (S.lastFrameId > 0 && !isNewer(fid, S.lastFrameId) && fid !== S.lastFrameId) {
        return;
    }

    for (const [id, fr] of S.chunks) {
        if (fr.received < fr.total && rt - fr.firstTime > C.FRAME_TIMEOUT_MS) {
            tryDrop(id, fr, rt);
        }
    }

    if (!S.chunks.has(fid)) {
        for (const [id, fr] of S.chunks) {
            if (isNewer(fid, id) && fr.received < fr.total) {
                tryDrop(id, fr, rt);
            }
        }

        S.chunks.set(fid, {
            parts: Array(tot).fill(null),
            total: tot,
            received: 0,
            capTs: cap,
            encMs: enc / 1000,
            firstTime: rt,
            isKey: typ === 1
        });

        if (S.chunks.size > C.MAX_FRAMES) {
            let did = null;
            let dage = 0;

            for (const [id, fr] of S.chunks) {
                if (id !== fid && fr.received !== fr.total) {
                    const a = rt - fr.firstTime;
                    if (a > dage && !fr.isKey) {
                        dage = a;
                        did = id;
                    }
                }
            }

            if (!did) {
                for (const [id, fr] of S.chunks) {
                    if (id !== fid && fr.received !== fr.total) {
                        const a = rt - fr.firstTime;
                        if (a > dage) {
                            dage = a;
                            did = id;
                        }
                    }
                }
            }

            if (did) {
                tryDrop(did, S.chunks.get(did), rt);
            }
        }
    }

    const fr = S.chunks.get(fid);
    if (!fr || fr.parts[cidx]) {
        return;
    }

    fr.parts[cidx] = chunk;
    fr.received++;

    if (fr.received === fr.total) {
        processFrame(fid, fr, rt);
    }
};

/**
 * Sets up the data channel event handlers
 */
const setupDC = () => {
    S.dc.binaryType = 'arraybuffer';

    S.dc.onopen = async () => {
        S.fpsSent = S.authenticated = false;
        updateLoadingStage(Stage.AUTH);
        console.info('Data channel opened');

        if (!validCreds(creds)) {
            const s = getSaved();
            if (validCreds(s)) {
                creds = s;
            }
        }

        if (validCreds(creds)) {
            sendAuth(creds.username, creds.pin);
        } else {
            showAuthModal();
        }

        await initDecoder();
        clearPing();

        pingInterval = setInterval(() => {
            if (S.dc?.readyState === 'open') {
                S.dc.send(mkBuf(16, v => {
                    v.setUint32(0, MSG.PING, true);
                    v.setBigUint64(8, BigInt(tsUs()), true);
                }));
            }
        }, C.PING_MS);
    };

    S.dc.onclose = () => {
        S.fpsSent = S.authenticated = false;
        clearPing();
    };

    S.dc.onerror = e => console.error('DataChannel:', e);
    S.dc.onmessage = handleMsg;
};

/**
 * Resets all connection state
 */
const resetState = () => {
    clearPing();
    S.dc?.close();
    S.pc?.close();

    try {
        if (S.decoder?.state !== 'closed') {
            S.decoder?.close();
        }
    } catch {}

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

/**
 * Establishes WebRTC connection to server
 */
const connect = async () => {
    try {
        const s = getSaved();
        if (validCreds(s)) {
            creds = s;
        }

        updateLoadingStage(Stage.ICE);
        resetState();

        const pc = S.pc = new RTCPeerConnection({
            iceServers: STUN,
            iceCandidatePoolSize: 10,
            bundlePolicy: 'max-bundle',
            rtcpMuxPolicy: 'require'
        });

        pc.onicecandidate = e => {
            if (e.candidate) {
                console.info(`ICE candidate: ${e.candidate.type} (${e.candidate.protocol})`);
            }
        };

        pc.onconnectionstatechange = () => {
            console.info('Connection:', pc.connectionState);

            if (pc.connectionState === 'connected') {
                connAttempts = 0;
            }

            if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
                if (authEl.overlay.classList.contains('visible')) {
                    console.info('Connection closed during auth - waiting for user');
                    return;
                }

                connAttempts++;

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
            if (e.errorCode !== 701) {
                console.error('ICE error:', e.errorCode, e.errorText);
            }
        };

        S.dc = pc.createDataChannel('screen', C.DC);
        setupDC();

        await pc.setLocalDescription(await pc.createOffer());
        updateLoadingStage(Stage.SIGNAL, 'Gathering ICE...');

        await new Promise(r => {
            if (pc.iceGatheringState === 'complete') {
                return r();
            }

            const to = setTimeout(r, 5000);
            pc.addEventListener('icegatheringstatechange', () => {
                if (pc.iceGatheringState === 'complete') {
                    clearTimeout(to);
                    r();
                }
            });
        });

        updateLoadingStage(Stage.SIGNAL, 'Sending offer...');
        console.info('Sending offer to', baseUrl);

        const res = await fetch(`${baseUrl}/api/offer`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                sdp: pc.localDescription.sdp,
                type: pc.localDescription.type
            })
        });

        if (!res.ok) {
            throw new Error('Server rejected offer');
        }

        const ans = await res.json();
        console.info('Received answer');

        updateLoadingStage(Stage.CONNECT);
        await pc.setRemoteDescription(new RTCSessionDescription(ans));

    } catch (e) {
        console.error('Connect error:', e.message);
        throw e;
    }
};

/**
 * Detects client display refresh rate
 * @returns {Promise<number>} Detected refresh rate in Hz
 */
export const detectFps = async () => {
    if (window.screen?.refreshRate) {
        return Math.round(window.screen.refreshRate);
    }

    return new Promise(r => {
        let f = 0;
        let lt = performance.now();
        const s = [];

        const m = now => {
            if (++f > 1) {
                s.push(now - lt);
            }
            lt = now;

            if (s.length < 30) {
                requestAnimationFrame(m);
            } else {
                const sorted = s.sort((a, b) => a - b);
                const median = sorted[sorted.length >> 1];
                const calculated = Math.round(1000 / median);

                const common = [30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 240, 360];
                const closest = common.find(rt => Math.abs(rt - calculated) <= 5);

                r(closest || calculated);
            }
        };

        requestAnimationFrame(m);
    });
};

/**
 * Cleans up connection resources
 */
export const cleanup = () => {
    clearPing();
    S.dc?.close();
    S.pc?.close();
};

/**
 * Application initialization
 */
(async () => {
    setNetCbs(applyFps, sendMonSel);
    S.clientFps = await detectFps();
    updateFpsOpts();
    loadConnSettings();

    const isLocal = window.location.hostname === 'localhost' ||
        window.location.hostname === '127.0.0.1' ||
        /^192\.168\.|^10\.|^172\.(1[6-9]|2[0-9]|3[01])\./.test(window.location.hostname);

    if (isLocal) {
        console.info('Detected local server, connecting...');
        baseUrl = window.location.origin;
        connEl.url.value = baseUrl;
        showLoading(false);

        try {
            await connect();
        } catch (e) {
            showConnModal('Connection failed: ' + e.message);
        }
    } else {
        console.info('Not on local server, showing connection modal');
        showConnModal();
    }

    setInterval(updateStats, 1000);
})();

/**
 * Cleanup on page unload
 */
window.onbeforeunload = () => {
    cleanup();
    closeAudio();
};
