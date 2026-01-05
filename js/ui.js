import { S, $, resetStats, Stage } from './state.js';
import { getLatStats, getJitterStats } from './renderer.js';
import { setTouchMode } from './input.js';
import { toggleAudio } from './media.js';
import { enableClipboard, disableClipboard, isClipboardEnabled } from './clipboard.js';

const loadingEl = $('loadingOverlay'), statusEl = $('loadingStatus'), subEl = $('loadingSubstatus');
const stages = { ice: $('stageIce'), signal: $('stageSignal'), connect: $('stageConnect'), auth: $('stageAuth'), stream: $('stageStream') };

const msgs = { [Stage.IDLE]: ['Connecting...', 'Initializing'], [Stage.ICE]: ['Connecting...', 'Gathering ICE'], [Stage.SIGNAL]: ['Connecting...', 'Signaling'],
    [Stage.CONNECT]: ['Connecting...', 'Peer connection'], [Stage.AUTH]: ['Authenticating...', 'Verifying'], [Stage.STREAM]: ['Almost there...', 'Waiting for stream'],
    [Stage.OK]: ['Connected', ''], [Stage.ERR]: ['Failed', 'Retrying...'] };

const order = [Stage.IDLE, Stage.ICE, Stage.SIGNAL, Stage.CONNECT, Stage.AUTH, Stage.STREAM, Stage.OK];
const stageMap = [[stages.ice, Stage.ICE], [stages.signal, Stage.SIGNAL], [stages.connect, Stage.CONNECT], [stages.auth, Stage.AUTH], [stages.stream, Stage.STREAM]];

export const updateLoadingStage = (stage, errMsg = null) => {
    S.stage = stage; const [st, sub] = msgs[stage] || msgs[Stage.IDLE];
    statusEl.textContent = S.isReconnecting ? 'Reconnecting...' : st; subEl.textContent = errMsg || sub;
    const idx = order.indexOf(stage);
    stageMap.forEach(([el, s]) => { const i = order.indexOf(s); el.classList.remove('active', 'done', 'error');
        const c = stage === Stage.ERR ? (i <= idx ? 'error' : '') : i < idx ? 'done' : i === idx ? 'active' : ''; c && el.classList.add(c); });
    loadingEl.classList.toggle('reconnecting', S.isReconnecting);
};

export const showLoading = (isReconnect = false) => { S.isReconnecting = isReconnect; S.firstFrameReceived = false; loadingEl.classList.remove('hidden'); updateLoadingStage(Stage.IDLE); };
export const hideLoading = () => { S.firstFrameReceived = true; updateLoadingStage(Stage.OK); setTimeout(() => loadingEl.classList.add('hidden'), 300); };
export const isLoadingVisible = () => !loadingEl.classList.contains('hidden');

const conOut = $('conOut'), orig = { log: console.log, info: console.info, warn: console.warn, error: console.error };
let logCnt = 0;

const fmtTs = () => { const n = new Date(); return `${n.toLocaleTimeString('en-US', { hour12: false })}.${String(n.getMilliseconds()).padStart(3, '0')}`; };
const fmtArgs = a => { const r = []; for (let i = 0; i < a.length; i++) { const x = a[i]; if (x == null) { r.push(String(x)); continue; }
    if (typeof x === 'string') { const s = x.replace(/%c/g, ''); while (i + 1 < a.length && typeof a[i + 1] === 'string' && /color:|font|background/.test(a[i + 1])) i++; s.trim() && r.push(s); }
    else r.push(typeof x === 'object' ? JSON.stringify(x) : String(x)); } return r.join(' '); };

const addLog = (t, a) => { const e = document.createElement('div'); e.className = `le ${t}`; e.innerHTML = `<span class="lt">${fmtTs()}</span><span class="lm">${fmtArgs(a).replace(/</g, '&lt;')}</span>`;
    conOut.appendChild(e); if (++logCnt > 200) { conOut.removeChild(conOut.firstChild); logCnt--; } conOut.scrollTop = conOut.scrollHeight; };

['log', 'info', 'warn', 'error'].forEach(m => console[m] = (...a) => { orig[m](...a); addLog(m, a); });
window.onerror = (m, s, l, c) => { console.error(`Error: ${m} at ${s}:${l}:${c}`); return false; };
window.onunhandledrejection = e => console.error('Promise:', e.reason);

export const clearLogs = () => { conOut.innerHTML = ''; logCnt = 0; };

let applyFpsFn = null, sendMonFn = null;
export const setNetCbs = (f, m) => { applyFpsFn = f; sendMonFn = m; };

const pnl = $('pnl'), sc = $('sc'), statsEl = $('stats'), conEl = $('con'), fpsSel = $('fpsSel'), monSel = $('monSel');

const togglePnl = on => { ['pnl', 'bk', 'edge'].forEach(id => $(id).classList.toggle('on', on)); on && $('pnlX').focus(); };
$('edge').onclick = () => togglePnl(true);
$('pnlX').onclick = $('bk').onclick = () => togglePnl(false);
document.onkeydown = e => { if (e.key === 'Escape' && pnl.classList.contains('on') && !S.controlEnabled) togglePnl(false); };

const bindTog = (id, key, target) => { const el = $(id); el.onclick = () => { S[key] = !S[key]; el.classList.toggle('on', S[key]); target.classList.toggle('on', S[key]); };
    el.onkeydown = e => (e.key === 'Enter' || e.key === ' ') && (e.preventDefault(), el.click()); };

bindTog('togS', 'statsOn', statsEl); bindTog('togC', 'consoleOn', conEl);
$('conClr').onclick = clearLogs;

const togClip = $('togClip'), clipSt = $('clipSt'), clipStT = $('clipStT');
if (togClip) { togClip.onclick = () => { const en = !isClipboardEnabled(); en ? enableClipboard() : disableClipboard();
    togClip.classList.toggle('on', en); clipSt.classList.toggle('on', en); clipStT.textContent = en ? 'Active' : 'Disabled'; };
    togClip.onkeydown = e => (e.key === 'Enter' || e.key === ' ') && (e.preventDefault(), togClip.click()); }

fpsSel.onchange = () => applyFpsFn?.(fpsSel.value);
monSel.onchange = () => sendMonFn?.(+monSel.value);
$('aBtn').onclick = toggleAudio;

document.querySelectorAll('input[name="tm"]').forEach(r => r.addEventListener('change', e => e.target.checked && setTouchMode(e.target.value)));

const fsi = $('fsi'), fsp = $('fsp'), fst = $('fst');
const FS = ['M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3', 'M8 3v3a2 2 0 0 1-2 2H3M16 3v3a2 2 0 0 0 2 2h3M8 21v-3a2 2 0 0 0-2-2H3M16 21v-3a2 2 0 0 1 2-2h3'];
const isFs = () => !!(document.fullscreenElement || document.webkitFullscreenElement);

if (!(document.fullscreenEnabled || document.webkitFullscreenEnabled || document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen)) $('fs').closest('.fss')?.classList.add('un');
fsi.setAttribute('fill', 'none'); fsi.setAttribute('stroke', 'currentColor'); fsi.setAttribute('stroke-width', '2');

const updFS = () => { const is = isFs(); fsp.setAttribute('d', FS[is ? 1 : 0]); fst.textContent = is ? 'Exit Fullscreen' : 'Fullscreen'; };
updFS();

$('fs').onclick = () => { isFs() ? (document.exitFullscreen || document.webkitExitFullscreen).call(document)
    : (document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen)?.call(document.documentElement).catch(e => console.warn('Fullscreen:', e.message)); };

['fullscreenchange', 'webkitfullscreenchange'].forEach(e => document.addEventListener(e, updFS));

const fn = (v, d = 2) => v.toFixed(d), fp = (n, d) => d <= 0 ? '0.0' : ((n / d) * 100).toFixed(1), clr = (v, g, w) => v < g ? 'g' : v < w ? '' : 'w';
const row = (l, v, c = '') => `<div class="sr"><span>${l}</span><span class="sv ${c}">${v}</span></div>`;
const sec = (t, ...r) => `<div class="ss"><div class="sst">${t}</div>${r.join('')}</div>`;

export const updateStats = () => {
    const now = performance.now(), dt = (now - S.stats.lastUpdate) / 1000; if (!S.statsOn) return resetStats();
    const [enc, net, dec, , ren] = ['encode', 'network', 'decode', 'queue', 'render'].map(getLatStats), jit = getJitterStats();
    const { recv, dec: dS, rend, bytes, audio, moves, clicks, keys, tRecv, tDropNet, tDropDec } = S.stats;
    const [inF, decF, rndF, br, aud] = [recv / dt, dS / dt, rend / dt, (bytes * 8) / 1048576 / dt, audio / dt];
    const tDrop = tDropNet + tDropDec, tFr = tRecv + tDrop, dropP = tFr > 0 ? (tDrop / tFr) * 100 : 0;
    const mode = S.currentFpsMode === 1 ? ' (H)' : S.currentFpsMode === 2 ? ' (C)' : '';
    sc.innerHTML = [
        sec('Stream', row('Monitor', S.monitors.length ? `#${S.currentMon + 1}` : '-'), row('Res', `${S.W}x${S.H}`), row('Codec', `AV1 ${S.hwAccel}`), row('FPS', `${S.currentFps}${mode}`), row('Bitrate', `${fn(br, 1)} Mbps`)),
        sec('FPS', row('In', fn(inF, 1)), row('Decode', fn(decF, 1)), row('Render', fn(rndF, 1))),
        sec('Audio', row('Status', S.audioEnabled ? 'On' : 'Off', S.audioEnabled ? 'g' : ''), row('Pkt/s', fn(aud, 1))),
        sec('Input', row('Status', S.controlEnabled ? (S.pointerLocked ? 'Locked' : 'On') : 'Off', S.controlEnabled ? 'g' : ''), row('Mouse/s', fn(moves / dt, 0)), row('Click+Key', `${clicks}+${keys}`)),
        sec('Touch', row('Mode', S.touchEnabled ? S.touchMode : 'Off', S.touchEnabled ? 'g' : ''), row('Pos', `${fn(S.touchX)},${fn(S.touchY)}`), row('Zoom', `${fn(S.zoom)}x`)),
        sec('Latency', row('RTT', `${fn(S.rtt, 1)}ms`, clr(S.rtt, 20, 50)), row('Enc', `${fn(enc.avg, 1)}ms`), row('Net', `${fn(net.avg, 1)}ms`), row('Dec', `${fn(dec.avg)}ms`), row('Ren', `${fn(ren.avg)}ms`)),
        sec('Quality', row('Jitter', `${fn(jit.avg)}ms`), row('Sync', S.clockSync ? 'Y' : 'N'), row('Recv', tRecv), row('Drop', `${tDrop} (${fp(tDrop, tFr)}%)`, clr(dropP, 1, 5)))
    ].join('');
    resetStats();
};

export const updateMonOpts = () => { monSel.innerHTML = S.monitors.length ? S.monitors.map(m => `<option value="${m.index}">#${m.index + 1}${m.isPrimary ? '*' : ''} ${m.width}x${m.height}@${m.refreshRate}</option>`).join('') : '<option value="0">Waiting...</option>'; monSel.value = S.currentMon; };

export const updateFpsOpts = () => { const prev = fpsSel.value, vals = [...new Set([15, 30, 60, S.hostFps, S.clientFps])].sort((a, b) => a - b);
    fpsSel.innerHTML = vals.map(f => `<option value="${f}">${f}${f === S.hostFps && f === S.clientFps ? ' (Native)' : f === S.hostFps ? ' (Host)' : f === S.clientFps ? ' (Client)' : ''}</option>`).join('');
    if ([...fpsSel.options].some(o => o.value === prev)) fpsSel.value = prev; };
