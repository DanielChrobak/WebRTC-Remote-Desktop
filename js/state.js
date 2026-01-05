export const MSG = {
    PING: 0x504E4750, FPS_SET: 0x46505343, HOST_INFO: 0x484F5354, FPS_ACK: 0x46505341,
    REQUEST_KEY: 0x4B455952, MONITOR_LIST: 0x4D4F4E4C, MONITOR_SET: 0x4D4F4E53,
    AUDIO_DATA: 0x41554449, MOUSE_MOVE: 0x4D4F5645, MOUSE_BTN: 0x4D42544E,
    MOUSE_WHEEL: 0x4D57484C, KEY: 0x4B455920,
    CLIPBOARD_TEXT: 0x434C5054, CLIPBOARD_IMAGE: 0x434C5049,
    CLIPBOARD_REQUEST: 0x434C5052, CLIPBOARD_ACK: 0x434C5041,
    AUTH_REQUEST: 0x41555448, AUTH_RESPONSE: 0x41555452
};

export const C = {
    HEADER: 21, AUDIO_HEADER: 16, PING_MS: 200, CODEC: 'av01.0.05M.08', MAX_FRAMES: 4,
    AUDIO_RATE: 48000, AUDIO_CH: 2, AUDIO_BUF: 0.04,
    DC: { ordered: false, maxRetransmits: 2 },
    TOUCH_SENS: 0.5, TAP_MS: 200, TAP_THRESH: 10, LONG_MS: 400,
    MIN_ZOOM: 1, MAX_ZOOM: 5, PINCH_SENS: 0.01,
    MAX_CLIPBOARD_SIZE: 10485760, MAX_CLIPBOARD_TEXT: 1048576
};

export const ConnectionStage = { IDLE: 'idle', ICE_GATHERING: 'ice', SIGNALING: 'signal', CONNECTING: 'connect', AUTHENTICATING: 'auth', STREAMING: 'stream', CONNECTED: 'connected', ERROR: 'error' };

const defIce = () => ({ stunServers: 0, turnServers: 0, candidates: { host: 0, srflx: 0, relay: 0, prflx: 0 }, connectionType: 'unknown', selectedPair: null, usingTurn: false, configSource: 'unknown' });

const defStats = () => ({ tRecv: 0, tDec: 0, tRend: 0, tDropNet: 0, tDropDec: 0, tBytes: 0, tAudio: 0, recv: 0, dec: 0, rend: 0, bytes: 0, audio: 0, moves: 0, clicks: 0, keys: 0, clipboard: 0, lastUpdate: performance.now() });

export const S = {
    pc: null, dc: null, decoder: null,
    ready: false, needKey: true, reinit: false, hwAccel: 'unknown', lastCapTs: 0,
    W: 0, H: 0, rtt: 0, clockOff: 0, clockSync: false, clockSamples: [],
    hostFps: 60, clientFps: 60, currentFps: 60, currentFpsMode: 0, fpsSent: false, authenticated: false,
    monitors: [], currentMon: 0,
    audioCtx: null, audioEnabled: false, audioDecoder: null, audioGain: null, audioPlaying: false, audioNextTime: 0,
    controlEnabled: false, pointerLocked: false, lastVp: { x: 0, y: 0, w: 0, h: 0 },
    touchEnabled: false, touchMode: 'trackpad', touchX: 0.5, touchY: 0.5, zoom: 1, zoomX: 0, zoomY: 0,
    statsOn: false, consoleOn: false,
    connectionStage: ConnectionStage.IDLE, isReconnecting: false, firstFrameReceived: false,
    ice: defIce(),
    stats: defStats(),
    lat: { encode: [], network: [], decode: [], queue: [], render: [] },
    jitter: { last: 0, deltas: [] },
    chunks: new Map(), frameMeta: new Map(), lastFrameId: 0,
    clipboardEnabled: true, clipboardLastSentHash: 0, clipboardIgnoreNext: false
};

export const resetStats = () => Object.assign(S.stats, { recv: 0, dec: 0, rend: 0, bytes: 0, audio: 0, moves: 0, clicks: 0, keys: 0, clipboard: 0, lastUpdate: performance.now() });

export const resetIceStats = () => Object.assign(S.ice, defIce());
