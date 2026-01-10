/**
 * @file state.js
 * @brief Shared application state and constants
 * @copyright 2025-2026 Daniel Chrobak
 */

/**
 * Message type identifiers for WebRTC data channel protocol.
 * These magic numbers identify different message types in binary communication.
 */
export const MSG = {
    PING: 0x504E4750,
    FPS_SET: 0x46505343,
    HOST_INFO: 0x484F5354,
    FPS_ACK: 0x46505341,
    REQUEST_KEY: 0x4B455952,
    MONITOR_LIST: 0x4D4F4E4C,
    MONITOR_SET: 0x4D4F4E53,
    AUDIO_DATA: 0x41554449,
    MOUSE_MOVE: 0x4D4F5645,
    MOUSE_BTN: 0x4D42544E,
    MOUSE_WHEEL: 0x4D57484C,
    KEY: 0x4B455920,
    AUTH_REQUEST: 0x41555448,
    AUTH_RESPONSE: 0x41555452
};

/**
 * Application configuration constants.
 */
export const C = {
    HEADER: 21,
    AUDIO_HEADER: 16,
    PING_MS: 200,
    CODEC: 'av01.0.05M.08',
    MAX_FRAMES: 6,
    FRAME_TIMEOUT_MS: 100,
    AUDIO_RATE: 48000,
    AUDIO_CH: 2,
    AUDIO_BUF: 0.04,
    DC: { ordered: false, maxRetransmits: 2 },
    TOUCH_SENS: 0.5,
    TAP_MS: 200,
    TAP_THRESH: 10,
    LONG_MS: 400,
    MIN_ZOOM: 1,
    MAX_ZOOM: 5,
    PINCH_SENS: 0.01
};

/**
 * Connection stage identifiers for UI state management.
 */
export const Stage = {
    IDLE: 'idle',
    ICE: 'ice',
    SIGNAL: 'signal',
    CONNECT: 'connect',
    AUTH: 'auth',
    STREAM: 'stream',
    OK: 'connected',
    ERR: 'error'
};

/**
 * Global application state object.
 * Contains all mutable state for the client application.
 */
export const S = {
    pc: null,
    dc: null,
    decoder: null,
    ready: false,
    needKey: true,
    reinit: false,
    hwAccel: 'unknown',
    lastCapTs: 0,
    W: 0,
    H: 0,
    rtt: 0,
    clockOff: 0,
    clockSync: false,
    clockSamples: [],
    hostFps: 60,
    clientFps: 60,
    currentFps: 60,
    currentFpsMode: 0,
    fpsSent: false,
    authenticated: false,
    monitors: [],
    currentMon: 0,
    audioCtx: null,
    audioEnabled: false,
    audioDecoder: null,
    audioGain: null,
    audioPlaying: false,
    audioNextTime: 0,
    controlEnabled: false,
    pointerLocked: false,
    lastVp: { x: 0, y: 0, w: 0, h: 0 },
    touchEnabled: false,
    touchMode: 'trackpad',
    touchX: 0.5,
    touchY: 0.5,
    zoom: 1,
    zoomX: 0,
    zoomY: 0,
    statsOn: false,
    consoleOn: false,
    stage: Stage.IDLE,
    isReconnecting: false,
    firstFrameReceived: false,
    stats: {
        tRecv: 0,
        tDec: 0,
        tRend: 0,
        tDropNet: 0,
        tDropDec: 0,
        tBytes: 0,
        tAudio: 0,
        recv: 0,
        dec: 0,
        rend: 0,
        bytes: 0,
        audio: 0,
        moves: 0,
        clicks: 0,
        keys: 0,
        lastUpdate: performance.now()
    },
    lat: {
        encode: [],
        network: [],
        decode: [],
        queue: [],
        render: []
    },
    jitter: { last: 0, deltas: [] },
    chunks: new Map(),
    frameMeta: new Map(),
    lastFrameId: 0,
    lastProcessedCapTs: 0
};

/**
 * Resets periodic statistics counters.
 */
export const resetStats = () => Object.assign(S.stats, {
    recv: 0,
    dec: 0,
    rend: 0,
    bytes: 0,
    audio: 0,
    moves: 0,
    clicks: 0,
    keys: 0,
    lastUpdate: performance.now()
});

/**
 * DOM element selector shorthand.
 * @param {string} id - Element ID
 * @returns {HTMLElement|null} The element or null
 */
export const $ = id => document.getElementById(id);

/**
 * Creates an ArrayBuffer with initialized DataView.
 * @param {number} sz - Buffer size in bytes
 * @param {function} fn - Initialization function receiving DataView
 * @returns {ArrayBuffer} Initialized buffer
 */
export const mkBuf = (sz, fn) => {
    const b = new ArrayBuffer(sz);
    const v = new DataView(b);
    fn(v);
    return b;
};
