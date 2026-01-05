import { S, resetStats, ConnectionStage } from './state.js';
import { getLatStats, getJitterStats } from './renderer.js';
import { setTouchMode } from './input.js';
import { toggleAudio } from './media.js';
import { enableClipboard, disableClipboard, isClipboardEnabled } from './clipboard.js';

const $ = id => document.getElementById(id);
const loadingOverlay = $('loadingOverlay'), loadingStatus = $('loadingStatus'), loadingSubstatus = $('loadingSubstatus');
const stages = { ice: $('stageIce'), signal: $('stageSignal'), connect: $('stageConnect'), auth: $('stageAuth'), stream: $('stageStream') };

const stageMessages = {
    [ConnectionStage.IDLE]: ['Connecting...', 'Initializing'],
    [ConnectionStage.ICE_GATHERING]: ['Connecting...', 'Gathering ICE candidates'],
    [ConnectionStage.SIGNALING]: ['Connecting...', 'Exchanging session info'],
    [ConnectionStage.CONNECTING]: ['Connecting...', 'Establishing peer connection'],
    [ConnectionStage.AUTHENTICATING]: ['Authenticating...', 'Verifying credentials'],
    [ConnectionStage.STREAMING]: ['Almost there...', 'Waiting for first frame'],
    [ConnectionStage.CONNECTED]: ['Connected', ''],
    [ConnectionStage.ERROR]: ['Connection Failed', 'Retrying...']
};

const stageOrder = [ConnectionStage.IDLE, ConnectionStage.ICE_GATHERING, ConnectionStage.SIGNALING, ConnectionStage.CONNECTING, ConnectionStage.AUTHENTICATING, ConnectionStage.STREAMING, ConnectionStage.CONNECTED];
const stageMap = [[stages.ice, ConnectionStage.ICE_GATHERING], [stages.signal, ConnectionStage.SIGNALING], [stages.connect, ConnectionStage.CONNECTING], [stages.auth, ConnectionStage.AUTHENTICATING], [stages.stream, ConnectionStage.STREAMING]];

export const updateLoadingStage = (stage, errorMsg = null) => {
    S.connectionStage = stage;
    const [status, substatus] = stageMessages[stage] || stageMessages[ConnectionStage.IDLE];
    loadingStatus.textContent = S.isReconnecting ? 'Reconnecting...' : status;
    loadingSubstatus.textContent = errorMsg || substatus;
    const currentIdx = stageOrder.indexOf(stage);
    stageMap.forEach(([el, s]) => {
        const sIdx = stageOrder.indexOf(s);
        el.classList.remove('active', 'done', 'error');
        const cls = stage === ConnectionStage.ERROR ? (sIdx <= currentIdx ? 'error' : '') : sIdx < currentIdx ? 'done' : sIdx === currentIdx ? 'active' : '';
        if (cls) el.classList.add(cls);
    });
    loadingOverlay.classList.toggle('reconnecting', S.isReconnecting);
};

export const showLoading = (isReconnect = false) => { S.isReconnecting = isReconnect; S.firstFrameReceived = false; loadingOverlay.classList.remove('hidden'); updateLoadingStage(ConnectionStage.IDLE); };
export const hideLoading = () => { S.firstFrameReceived = true; updateLoadingStage(ConnectionStage.CONNECTED); setTimeout(() => loadingOverlay.classList.add('hidden'), 300); };
export const isLoadingVisible = () => !loadingOverlay.classList.contains('hidden');

const conOut = $('conOut');
const origCon = { log: console.log, info: console.info, warn: console.warn, error: console.error };
let logCnt = 0;

const fmtTs = () => { const n = new Date(); return `${n.toLocaleTimeString('en-US', { hour12: false })}.${String(n.getMilliseconds()).padStart(3, '0')}`; };

const fmtArgs = a => [...a].reduce((r, x, i, arr) => {
    if (x == null) return [...r, String(x)];
    if (typeof x === 'string') {
        let s = x.replace(/%c/g, '');
        while (i + 1 < arr.length && typeof arr[i + 1] === 'string' && /color:|font|background/.test(arr[i + 1])) arr.splice(++i, 1);
        return s.trim() ? [...r, s] : r;
    }
    return [...r, typeof x === 'object' ? JSON.stringify(x) : String(x)];
}, []).join(' ');

const addLog = (t, a) => {
    const e = document.createElement('div');
    e.className = `le ${t}`;
    e.innerHTML = `<span class="lt">${fmtTs()}</span><span class="lm">${fmtArgs(a).replace(/</g, '&lt;')}</span>`;
    conOut.appendChild(e);
    if (++logCnt > 200) { conOut.removeChild(conOut.firstChild); logCnt--; }
    conOut.scrollTop = conOut.scrollHeight;
};

['log', 'info', 'warn', 'error'].forEach(m => console[m] = (...a) => { origCon[m](...a); addLog(m, a); });
window.onerror = (m, s, l, c) => { console.error(`Error: ${m} at ${s}:${l}:${c}`); return false; };
window.onunhandledrejection = e => console.error('Promise:', e.reason);

export const clearLogs = () => { conOut.innerHTML = ''; logCnt = 0; };

let applyFpsFn = null, sendMonFn = null;
export const setNetCbs = (f, m) => { applyFpsFn = f; sendMonFn = m; };

const edge = $('edge'), bk = $('bk'), pnl = $('pnl'), sc = $('sc');
const statsEl = $('stats'), conEl = $('con'), fpsSel = $('fpsSel'), monSel = $('monSel');

const togglePnl = on => { ['pnl', 'bk', 'edge'].forEach(id => $(id).classList.toggle('on', on)); on && $('pnlX').focus(); };
edge.onclick = () => togglePnl(true);
$('pnlX').onclick = bk.onclick = () => togglePnl(false);
document.onkeydown = e => { if (e.key === 'Escape' && pnl.classList.contains('on') && !S.controlEnabled) togglePnl(false); };

const bindTog = (id, key, target) => {
    const el = $(id);
    el.onclick = () => { S[key] = !S[key]; el.classList.toggle('on', S[key]); target.classList.toggle('on', S[key]); };
    el.onkeydown = e => (e.key === 'Enter' || e.key === ' ') && (e.preventDefault(), el.click());
};

bindTog('togS', 'statsOn', statsEl);
bindTog('togC', 'consoleOn', conEl);
$('conClr').onclick = clearLogs;

const togClip = $('togClip'), clipSt = $('clipSt'), clipStT = $('clipStT');
if (togClip) {
    togClip.onclick = () => {
        const enabled = !isClipboardEnabled();
        enabled ? enableClipboard() : disableClipboard();
        togClip.classList.toggle('on', enabled); clipSt.classList.toggle('on', enabled);
        clipStT.textContent = enabled ? 'Clipboard syncing is active' : 'Clipboard syncing is disabled';
    };
    togClip.onkeydown = e => (e.key === 'Enter' || e.key === ' ') && (e.preventDefault(), togClip.click());
}

fpsSel.onchange = () => applyFpsFn?.(fpsSel.value);
monSel.onchange = () => sendMonFn?.(+monSel.value);
$('aBtn').onclick = toggleAudio;

document.querySelectorAll('input[name="tm"]').forEach(r => r.addEventListener('change', e => e.target.checked && setTouchMode(e.target.value)));

const fsi = $('fsi'), fsp = $('fsp'), fst = $('fst');
const FS_PATHS = ['M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3', 'M8 3v3a2 2 0 0 1-2 2H3M16 3v3a2 2 0 0 0 2 2h3M8 21v-3a2 2 0 0 0-2-2H3M16 21v-3a2 2 0 0 1 2-2h3'];

const fsOk = !!(document.fullscreenEnabled || document.webkitFullscreenEnabled || document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen);
if (!fsOk) $('fs').closest('.fss')?.classList.add('un');
fsi.setAttribute('fill', 'none'); fsi.setAttribute('stroke', 'currentColor'); fsi.setAttribute('stroke-width', '2');

const updFS = () => { const is = !!(document.fullscreenElement || document.webkitFullscreenElement); fsp.setAttribute('d', FS_PATHS[is ? 1 : 0]); fst.textContent = is ? 'Exit Fullscreen' : 'Fullscreen'; };
updFS();

$('fs').onclick = () => {
    const is = document.fullscreenElement || document.webkitFullscreenElement;
    is ? (document.exitFullscreen || document.webkitExitFullscreen).call(document)
       : (document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen)?.call(document.documentElement).catch(e => console.warn('Fullscreen:', e.message));
};

['fullscreenchange', 'webkitfullscreenchange'].forEach(e => document.addEventListener(e, updFS));

const fn = (v, d = 2) => v.toFixed(d);
const fp = (n, d) => d <= 0 ? '0.0' : ((n / d) * 100).toFixed(1);
const clr = (v, g, w) => v < g ? 'g' : v < w ? '' : 'w';
const row = (l, v, c = '') => `<div class="sr"><span>${l}</span><span class="sv ${c}">${v}</span></div>`;
const sec = (t, ...rows) => `<div class="ss"><div class="sst">${t}</div>${rows.join('')}</div>`;

export const updateStats = () => {
    const now = performance.now(), dt = (now - S.stats.lastUpdate) / 1000;
    if (!S.statsOn) return resetStats();

    const [enc, net, dec, que, ren, jit] = ['encode', 'network', 'decode', 'queue', 'render'].map(getLatStats).concat([getJitterStats()]);
    const { recv, dec: decS, rend, bytes, audio, moves, clicks, keys, tRecv, tDropNet, tDropDec } = S.stats;
    const [inF, decF, rndF, br, aud] = [recv / dt, decS / dt, rend / dt, (bytes * 8) / 1048576 / dt, audio / dt];
    const tDrop = tDropNet + tDropDec, tFr = tRecv + tDrop, dropP = tFr > 0 ? (tDrop / tFr) * 100 : 0;
    const { ice } = S, modeL = S.currentFpsMode === 1 ? ' (Host)' : S.currentFpsMode === 2 ? ' (Client)' : '';

    const connTypeLabel = { relay: 'TURN Relay', stun: 'STUN (Direct)', direct: 'Direct (P2P)' }[ice.connectionType] || 'Connecting...';
    const configLabel = { metered: 'Metered API', manual: 'Manual Config', fallback: 'Fallback (STUN only)' }[ice.configSource] || 'Loading...';
    const turnLabel = ice.usingTurn ? 'Active' : ice.turnServers > 0 ? 'Available' : 'None';

    sc.innerHTML = [
        sec('Stream', row('Monitor', S.monitors.length ? `Display ${S.currentMon + 1}` : '-'), row('Resolution', `${S.W}x${S.H}`), row('Codec', `AV1 (${S.hwAccel})`), row('Target FPS', `${S.currentFps}${modeL}`), row('Bitrate', `${fn(br, 1)} Mbps`)),
        sec('ICE / Network', row('Config Source', configLabel), row('ICE Servers', `${ice.stunServers} STUN, ${ice.turnServers} TURN`), row('Connection Type', connTypeLabel, ice.connectionType === 'relay' ? 'w' : ice.connectionType === 'direct' ? 'g' : ''), row('TURN Relay', turnLabel, ice.usingTurn ? 'w' : ice.turnServers > 0 ? '' : 'b'), row('Candidates', `H:${ice.candidates.host} S:${ice.candidates.srflx} R:${ice.candidates.relay}`)),
        sec('Frame Rates', row('Network', `${fn(inF, 1)} fps`), row('Decode', `${fn(decF, 1)} fps`), row('Render', `${fn(rndF, 1)} fps`)),
        sec('Audio', row('Status', S.audioEnabled ? 'Enabled' : 'Disabled', S.audioEnabled ? 'g' : ''), row('Packets/sec', fn(aud, 1)), row('Total Received', S.stats.tAudio)),
        sec('Input', row('Status', S.controlEnabled ? (S.pointerLocked ? 'Locked' : 'Active') : 'Disabled', S.controlEnabled ? 'g' : ''), row('Mouse Moves/sec', fn(moves / dt, 0)), row('Clicks + Keys', `${clicks} + ${keys}`)),
        sec('Touch', row('Status', S.touchEnabled ? `Enabled (${S.touchMode})` : 'Disabled', S.touchEnabled ? 'g' : ''), row('Cursor Pos', `${fn(S.touchX)}, ${fn(S.touchY)}`), row('Zoom', `${fn(S.zoom)}x`, S.zoom > 1 ? 'g' : ''), row('Zoom Offset', `${fn(S.zoomX)}, ${fn(S.zoomY)}`)),
        sec('Latency', row('Round Trip', `${fn(S.rtt, 1)} ms`, clr(S.rtt, 20, 50)), row('Encode', `${fn(enc.avg, 1)} ms`), row('Network', `${fn(net.avg, 1)} ms`), row('Decode', `${fn(dec.avg)} ms`), row('Render', `${fn(ren.avg)} ms`), row('Queue', `${fn(que.avg)} ms`)),
        sec('Quality', row('Jitter', `${fn(jit.avg)} ms`), row('Clock Sync', S.clockSync ? 'Yes' : 'No'), row('Frames Received', tRecv), row('Frames Dropped', `${tDrop} (${fp(tDrop, tFr)}%)`, clr(dropP, 1, 5)))
    ].join('');
    resetStats();
};

export const updateMonOpts = () => {
    monSel.innerHTML = S.monitors.length
        ? S.monitors.map(m => `<option value="${m.index}">Display ${m.index + 1}${m.isPrimary ? ' (Primary)' : ''} - ${m.width}x${m.height} @ ${m.refreshRate}Hz</option>`).join('')
        : '<option value="0">Waiting for host...</option>';
    monSel.value = S.currentMon;
};

export const updateFpsOpts = () => {
    const prev = fpsSel.value, vals = [...new Set([15, 30, 60, S.hostFps, S.clientFps])].sort((a, b) => a - b);
    fpsSel.innerHTML = vals.map(f => `<option value="${f}">${f} FPS${f === S.hostFps && f === S.clientFps ? ' (Host & Client Native)' : f === S.hostFps ? ' (Host Native)' : f === S.clientFps ? ' (Client Native)' : ''}</option>`).join('');
    if ([...fpsSel.options].some(o => o.value === prev)) fpsSel.value = prev;
};
