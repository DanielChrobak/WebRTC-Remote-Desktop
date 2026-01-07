export const MSG = {
    PING: 0x504E4750, FPS_SET: 0x46505343, HOST_INFO: 0x484F5354, FPS_ACK: 0x46505341,
    REQUEST_KEY: 0x4B455952, MONITOR_LIST: 0x4D4F4E4C, MONITOR_SET: 0x4D4F4E53,
    AUDIO_DATA: 0x41554449, MOUSE_MOVE: 0x4D4F5645, MOUSE_BTN: 0x4D42544E,
    MOUSE_WHEEL: 0x4D57484C, KEY: 0x4B455920,
    AUTH_REQUEST: 0x41555448, AUTH_RESPONSE: 0x41555452
};

export const C = {
    HEADER: 21,
    AUDIO_HEADER: 16,
    PING_MS: 200,
    CODEC: 'av01.0.05M.08',

    // Frame buffering - tuned for reliability over lowest latency
    MAX_FRAMES: 12,              // Increased buffer for more tolerance
    FRAME_TIMEOUT_MS: 300,       // Increased from 150ms - more tolerant of network jitter

    // Smart frame dropping thresholds
    COMPLETION_THRESHOLD: 0.95,  // Increased from 0.9 - wait longer for nearly-complete frames
    MAX_FRAME_LAG: 4,            // Increased from 2 - don't drop frames too aggressively
    KEYFRAME_GRACE_MS: 500,      // Increased from 300 - give keyframes more time

    // Audio settings
    AUDIO_RATE: 48000,
    AUDIO_CH: 2,
    AUDIO_BUF: 0.02,             // Minimal audio buffer for low latency

    DC: { ordered: false, maxRetransmits: 0 },

    // Touch settings
    TOUCH_SENS: 0.5,
    TAP_MS: 200,
    TAP_THRESH: 10,
    LONG_MS: 400,

    // Zoom settings
    MIN_ZOOM: 1,
    MAX_ZOOM: 5,
    PINCH_SENS: 0.01
};

export const Stage = { IDLE: 'idle', ICE: 'ice', SIGNAL: 'signal', CONNECT: 'connect', AUTH: 'auth', STREAM: 'stream', OK: 'connected', ERR: 'error' };

export const S = {
    pc: null, dc: null, decoder: null,
    ready: false, needKey: true, reinit: false, hwAccel: 'unknown', lastCapTs: 0,
    W: 0, H: 0, rtt: 0, clockOff: 0, clockSync: false, clockSamples: [],
    hostFps: 60, clientFps: 60, currentFps: 60, currentFpsMode: 0, fpsSent: false, authenticated: false,
    monitors: [], currentMon: 0,
    audioCtx: null, audioEnabled: false, audioDecoder: null, audioGain: null, audioPlaying: false, audioNextTime: 0,
    controlEnabled: false, lastVp: { x: 0, y: 0, w: 0, h: 0 },
    touchEnabled: false, touchMode: 'trackpad', touchX: 0.5, touchY: 0.5, zoom: 1, zoomX: 0, zoomY: 0,
    statsOn: false, consoleOn: false,
    stage: Stage.IDLE, isReconnecting: false, firstFrameReceived: false,
    stats: { tRecv: 0, tDec: 0, tRend: 0, tDropNet: 0, tDropDec: 0, tBytes: 0, tAudio: 0, recv: 0, dec: 0, rend: 0, bytes: 0, audio: 0, moves: 0, clicks: 0, keys: 0, lastUpdate: performance.now() },
    lat: { encode: [], network: [], decode: [], queue: [], render: [] },
    jitter: { last: 0, deltas: [] },
    chunks: new Map(), frameMeta: new Map(), lastFrameId: 0, lastProcessedCapTs: 0,
    newestFrameId: 0  // Track the highest frame ID we've seen
};

export const resetStats = () => Object.assign(S.stats, { recv: 0, dec: 0, rend: 0, bytes: 0, audio: 0, moves: 0, clicks: 0, keys: 0, lastUpdate: performance.now() });
export const $ = id => document.getElementById(id);
export const mkBuf = (sz, fn) => { const b = new ArrayBuffer(sz), v = new DataView(b); fn(v); return b; };
