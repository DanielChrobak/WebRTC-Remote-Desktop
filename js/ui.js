/**
 * @file ui.js
 * @brief User interface management
 * @copyright 2025-2026 Daniel Chrobak
 */

import { S, $, resetStats, Stage } from './state.js';
import { getLatStats, getJitterStats } from './renderer.js';
import { setTouchMode } from './input.js';
import { toggleAudio } from './media.js';

const loadingEl = $('loadingOverlay');
const statusEl = $('loadingStatus');
const subEl = $('loadingSubstatus');

const stages = {
    ice: $('stageIce'),
    signal: $('stageSignal'),
    connect: $('stageConnect'),
    auth: $('stageAuth'),
    stream: $('stageStream')
};

const msgs = {
    [Stage.IDLE]: ['Connecting...', 'Initializing'],
    [Stage.ICE]: ['Connecting...', 'Gathering ICE'],
    [Stage.SIGNAL]: ['Connecting...', 'Signaling'],
    [Stage.CONNECT]: ['Connecting...', 'Peer connection'],
    [Stage.AUTH]: ['Authenticating...', 'Verifying'],
    [Stage.STREAM]: ['Almost there...', 'Waiting for stream'],
    [Stage.OK]: ['Connected', ''],
    [Stage.ERR]: ['Failed', 'Retrying...']
};

const order = [Stage.IDLE, Stage.ICE, Stage.SIGNAL, Stage.CONNECT, Stage.AUTH, Stage.STREAM, Stage.OK];

const stageMap = [
    [stages.ice, Stage.ICE],
    [stages.signal, Stage.SIGNAL],
    [stages.connect, Stage.CONNECT],
    [stages.auth, Stage.AUTH],
    [stages.stream, Stage.STREAM]
];

/**
 * Updates the loading stage indicator.
 * @param {string} stage - Current stage
 * @param {string|null} errMsg - Optional error message
 */
export const updateLoadingStage = (stage, errMsg = null) => {
    S.stage = stage;
    const [st, sub] = msgs[stage] || msgs[Stage.IDLE];

    statusEl.textContent = S.isReconnecting ? 'Reconnecting...' : st;
    subEl.textContent = errMsg || sub;

    const idx = order.indexOf(stage);

    stageMap.forEach(([el, s]) => {
        const i = order.indexOf(s);
        el.classList.remove('active', 'done', 'error');

        const c = stage === Stage.ERR
            ? (i <= idx ? 'error' : '')
            : i < idx ? 'done' : i === idx ? 'active' : '';

        if (c) el.classList.add(c);
    });

    loadingEl.classList.toggle('reconnecting', S.isReconnecting);
};

/**
 * Shows the loading overlay.
 * @param {boolean} isReconnect - Whether this is a reconnection attempt
 */
export const showLoading = (isReconnect = false) => {
    S.isReconnecting = isReconnect;
    S.firstFrameReceived = false;
    loadingEl.classList.remove('hidden');
    updateLoadingStage(Stage.IDLE);
};

/**
 * Hides the loading overlay.
 */
export const hideLoading = () => {
    S.firstFrameReceived = true;
    updateLoadingStage(Stage.OK);
    setTimeout(() => loadingEl.classList.add('hidden'), 300);
};

/**
 * Checks if the loading overlay is visible.
 * @returns {boolean} True if loading is visible
 */
export const isLoadingVisible = () => !loadingEl.classList.contains('hidden');

const conOut = $('conOut');
const orig = {
    log: console.log,
    info: console.info,
    warn: console.warn,
    error: console.error
};

let logCnt = 0;

/**
 * Formats a timestamp for log display.
 * @returns {string} Formatted timestamp
 */
const fmtTs = () => {
    const n = new Date();
    return `${n.toLocaleTimeString('en-US', { hour12: false })}.${String(n.getMilliseconds()).padStart(3, '0')}`;
};

/**
 * Formats console arguments for display.
 * @param {Array} a - Console arguments
 * @returns {string} Formatted string
 */
const fmtArgs = a => {
    const r = [];
    for (let i = 0; i < a.length; i++) {
        const x = a[i];
        if (x == null) {
            r.push(String(x));
            continue;
        }

        if (typeof x === 'string') {
            const s = x.replace(/%c/g, '');
            while (i + 1 < a.length && typeof a[i + 1] === 'string' &&
                   /color:|font|background/.test(a[i + 1])) i++;
            if (s.trim()) r.push(s);
        } else {
            r.push(typeof x === 'object' ? JSON.stringify(x) : String(x));
        }
    }
    return r.join(' ');
};

/**
 * Adds a log entry to the console panel.
 * @param {string} t - Log type
 * @param {Array} a - Log arguments
 */
const addLog = (t, a) => {
    const e = document.createElement('div');
    e.className = `le ${t}`;
    e.innerHTML = `<span class="lt">${fmtTs()}</span><span class="lm">${fmtArgs(a).replace(/</g, '&lt;')}</span>`;

    conOut.appendChild(e);

    if (++logCnt > 200) {
        conOut.removeChild(conOut.firstChild);
        logCnt--;
    }

    conOut.scrollTop = conOut.scrollHeight;
};

['log', 'info', 'warn', 'error'].forEach(m => {
    console[m] = (...a) => {
        orig[m](...a);
        addLog(m, a);
    };
});

window.onerror = (m, s, l, c) => {
    console.error(`Error: ${m} at ${s}:${l}:${c}`);
    return false;
};

window.onunhandledrejection = e => console.error('Promise:', e.reason);

/**
 * Clears the console log.
 */
export const clearLogs = () => {
    conOut.innerHTML = '';
    logCnt = 0;
};

let applyFpsFn = null;
let sendMonFn = null;

/**
 * Sets network callback functions.
 * @param {function} f - FPS apply callback
 * @param {function} m - Monitor select callback
 */
export const setNetCbs = (f, m) => {
    applyFpsFn = f;
    sendMonFn = m;
};

const pnl = $('pnl');
const sc = $('sc');
const statsEl = $('stats');
const conEl = $('con');
const fpsSel = $('fpsSel');
const monSel = $('monSel');

/**
 * Toggles the settings panel visibility.
 * @param {boolean} on - Show/hide state
 */
const togglePnl = on => {
    ['pnl', 'bk', 'edge'].forEach(id => $(id).classList.toggle('on', on));
    if (on) $('pnlX').focus();
};

$('edge').onclick = () => togglePnl(true);
$('pnlX').onclick = $('bk').onclick = () => togglePnl(false);

document.onkeydown = e => {
    if (e.key === 'Escape' && pnl.classList.contains('on') && !S.controlEnabled) {
        togglePnl(false);
    }
};

/**
 * Binds a toggle button to a state property.
 * @param {string} id - Button element ID
 * @param {string} key - State key
 * @param {HTMLElement} target - Target element to toggle
 */
const bindTog = (id, key, target) => {
    const el = $(id);
    el.onclick = () => {
        S[key] = !S[key];
        el.classList.toggle('on', S[key]);
        target.classList.toggle('on', S[key]);
    };
    el.onkeydown = e => {
        if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            el.click();
        }
    };
};

bindTog('togS', 'statsOn', statsEl);
bindTog('togC', 'consoleOn', conEl);

$('conClr').onclick = clearLogs;
fpsSel.onchange = () => applyFpsFn?.(fpsSel.value);
monSel.onchange = () => sendMonFn?.(+monSel.value);
$('aBtn').onclick = toggleAudio;

document.querySelectorAll('input[name="tm"]').forEach(r => {
    r.addEventListener('change', e => {
        if (e.target.checked) setTouchMode(e.target.value);
    });
});

const fsi = $('fsi');
const fsp = $('fsp');
const fst = $('fst');

const FS = [
    'M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3',
    'M8 3v3a2 2 0 0 1-2 2H3M16 3v3a2 2 0 0 0 2 2h3M8 21v-3a2 2 0 0 0-2-2H3M16 21v-3a2 2 0 0 1 2-2h3'
];

/**
 * Checks if fullscreen is active.
 * @returns {boolean} True if in fullscreen mode
 */
const isFs = () => !!(document.fullscreenElement || document.webkitFullscreenElement);

if (!(document.fullscreenEnabled || document.webkitFullscreenEnabled ||
      document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen)) {
    $('fs').closest('.fss')?.classList.add('un');
}

fsi.setAttribute('fill', 'none');
fsi.setAttribute('stroke', 'currentColor');
fsi.setAttribute('stroke-width', '2');

/**
 * Updates fullscreen button state.
 */
const updFS = () => {
    const is = isFs();
    fsp.setAttribute('d', FS[is ? 1 : 0]);
    fst.textContent = is ? 'Exit Fullscreen' : 'Fullscreen';
};

updFS();

$('fs').onclick = () => {
    if (isFs()) {
        (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    } else {
        (document.documentElement.requestFullscreen ||
         document.documentElement.webkitRequestFullscreen)?.call(document.documentElement)
            .catch(e => console.warn('Fullscreen:', e.message));
    }
};

['fullscreenchange', 'webkitfullscreenchange'].forEach(e => {
    document.addEventListener(e, updFS);
});

/**
 * Formats a number with specified decimal places.
 * @param {number} v - Value to format
 * @param {number} d - Decimal places
 * @returns {string} Formatted number
 */
const fn = (v, d = 2) => v.toFixed(d);

/**
 * Calculates percentage.
 * @param {number} n - Numerator
 * @param {number} d - Denominator
 * @returns {string} Formatted percentage
 */
const fp = (n, d) => d <= 0 ? '0.0' : ((n / d) * 100).toFixed(1);

/**
 * Gets color class based on value thresholds.
 * @param {number} v - Value to check
 * @param {number} g - Good threshold
 * @param {number} w - Warning threshold
 * @returns {string} Color class
 */
const clr = (v, g, w) => v < g ? 'g' : v < w ? '' : 'w';

/**
 * Creates a stats row HTML string.
 * @param {string} l - Label
 * @param {string} v - Value
 * @param {string} c - CSS class
 * @returns {string} HTML string
 */
const row = (l, v, c = '') => `<div class="sr"><span>${l}</span><span class="sv ${c}">${v}</span></div>`;

/**
 * Creates a stats section HTML string.
 * @param {string} t - Title
 * @param {...string} r - Rows
 * @returns {string} HTML string
 */
const sec = (t, ...r) => `<div class="ss"><div class="sst">${t}</div>${r.join('')}</div>`;

/**
 * Updates the statistics display.
 */
export const updateStats = () => {
    const now = performance.now();
    const dt = (now - S.stats.lastUpdate) / 1000;

    if (!S.statsOn) {
        resetStats();
        return;
    }

    const [enc, net, dec, , ren] = ['encode', 'network', 'decode', 'queue', 'render'].map(getLatStats);
    const jit = getJitterStats();

    const { recv, dec: dS, rend, bytes, audio, moves, clicks, keys, tRecv, tDropNet, tDropDec } = S.stats;

    const [inF, decF, rndF, br, aud] = [
        recv / dt,
        dS / dt,
        rend / dt,
        (bytes * 8) / 1048576 / dt,
        audio / dt
    ];

    const tDrop = tDropNet + tDropDec;
    const tFr = tRecv + tDrop;
    const dropP = tFr > 0 ? (tDrop / tFr) * 100 : 0;
    const mode = S.currentFpsMode === 1 ? ' (H)' : S.currentFpsMode === 2 ? ' (C)' : '';

    sc.innerHTML = [
        sec('Stream',
            row('Monitor', S.monitors.length ? `#${S.currentMon + 1}` : '-'),
            row('Res', `${S.W}x${S.H}`),
            row('Codec', `AV1 ${S.hwAccel}`),
            row('FPS', `${S.currentFps}${mode}`),
            row('Bitrate', `${fn(br, 1)} Mbps`)
        ),
        sec('FPS',
            row('In', fn(inF, 1)),
            row('Decode', fn(decF, 1)),
            row('Render', fn(rndF, 1))
        ),
        sec('Audio',
            row('Status', S.audioEnabled ? 'On' : 'Off', S.audioEnabled ? 'g' : ''),
            row('Pkt/s', fn(aud, 1))
        ),
        sec('Input',
            row('Status', S.controlEnabled ? (S.pointerLocked ? 'Locked' : 'On') : 'Off', S.controlEnabled ? 'g' : ''),
            row('Mouse/s', fn(moves / dt, 0)),
            row('Click+Key', `${clicks}+${keys}`)
        ),
        sec('Touch',
            row('Mode', S.touchEnabled ? S.touchMode : 'Off', S.touchEnabled ? 'g' : ''),
            row('Pos', `${fn(S.touchX)},${fn(S.touchY)}`),
            row('Zoom', `${fn(S.zoom)}x`)
        ),
        sec('Latency',
            row('RTT', `${fn(S.rtt, 1)}ms`, clr(S.rtt, 20, 50)),
            row('Enc', `${fn(enc.avg, 1)}ms`),
            row('Net', `${fn(net.avg, 1)}ms`),
            row('Dec', `${fn(dec.avg)}ms`),
            row('Ren', `${fn(ren.avg)}ms`)
        ),
        sec('Quality',
            row('Jitter', `${fn(jit.avg)}ms`),
            row('Sync', S.clockSync ? 'Y' : 'N'),
            row('Recv', tRecv),
            row('Drop', `${tDrop} (${fp(tDrop, tFr)}%)`, clr(dropP, 1, 5))
        )
    ].join('');

    resetStats();
};

/**
 * Updates the monitor selection dropdown.
 */
export const updateMonOpts = () => {
    monSel.innerHTML = S.monitors.length
        ? S.monitors.map(m =>
            `<option value="${m.index}">#${m.index + 1}${m.isPrimary ? '*' : ''} ${m.width}x${m.height}@${m.refreshRate}</option>`
          ).join('')
        : '<option value="0">Waiting...</option>';

    monSel.value = S.currentMon;
};

/**
 * Updates the FPS selection dropdown.
 */
export const updateFpsOpts = () => {
    const prev = fpsSel.value;
    const vals = [...new Set([15, 30, 60, S.hostFps, S.clientFps])].sort((a, b) => a - b);

    fpsSel.innerHTML = vals.map(f =>
        `<option value="${f}">${f}${
            f === S.hostFps && f === S.clientFps ? ' (Native)'
            : f === S.hostFps ? ' (Host)'
            : f === S.clientFps ? ' (Client)'
            : ''
        }</option>`
    ).join('');

    if ([...fpsSel.options].some(o => o.value === prev)) {
        fpsSel.value = prev;
    }
};
