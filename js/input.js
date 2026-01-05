import { MSG, C, S } from './state.js';
import { canvas, canvasW, canvasH, calcVp, renderZoomed } from './renderer.js';

let absX = 0.5, absY = 0.5, tStartX = 0, tStartY = 0, tStartT = 0, tMoved = false, tDrag = false;
let lastTX = 0, lastTY = 0, tId = null, lpTimer = null, lpActive = false, tf2Start = 0;
let pinch = false, pinchDist = 0, pinchScale = 1;

const BMAP = { 0: 0, 2: 1, 1: 2, 3: 3, 4: 4 };
const mkBuf = (size, fn) => { const b = new ArrayBuffer(size), v = new DataView(b); fn(v); return b; };

const send = (type, ...a) => {
    if ((!S.controlEnabled && !S.touchEnabled) || S.dc?.readyState !== 'open') return;
    const makers = {
        move: () => { S.stats.moves++; return mkBuf(12, v => { v.setUint32(0, MSG.MOUSE_MOVE, true); v.setFloat32(4, a[0], true); v.setFloat32(8, a[1], true); }); },
        btn: () => { S.stats.clicks++; return mkBuf(6, v => { v.setUint32(0, MSG.MOUSE_BTN, true); v.setUint8(4, a[0]); v.setUint8(5, a[1] ? 1 : 0); }); },
        wheel: () => mkBuf(8, v => { v.setUint32(0, MSG.MOUSE_WHEEL, true); v.setInt16(4, Math.round(a[0]), true); v.setInt16(6, Math.round(a[1]), true); }),
        key: () => { S.stats.keys++; return mkBuf(10, v => { v.setUint32(0, MSG.KEY, true); v.setUint16(4, a[0], true); v.setUint16(6, a[1], true); v.setUint8(8, a[2] ? 1 : 0); v.setUint8(9, a[3]); }); }
    };
    try { S.dc.send(makers[type]()); } catch {}
};

const toNorm = (cx, cy) => {
    if (S.W <= 0 || S.H <= 0) return null;
    const r = canvas.getBoundingClientRect(), dpr = devicePixelRatio || 1;
    const vp = calcVp(S.W, S.H, canvasW, canvasH), x = (cx - r.left) * dpr, y = (cy - r.top) * dpr;
    S.lastVp = vp;
    return (x < vp.x || x > vp.x + vp.w || y < vp.y || y > vp.y + vp.h) ? null
        : { x: Math.max(0, Math.min(1, (x - vp.x) / vp.w)), y: Math.max(0, Math.min(1, (y - vp.y) / vp.h)) };
};

const reqLock = () => (canvas.requestPointerLock || canvas.mozRequestPointerLock || canvas.webkitRequestPointerLock)?.call(canvas);
const exitLock = () => (document.exitPointerLock || document.mozExitPointerLock || document.webkitExitPointerLock)?.call(document);

document.addEventListener('pointerlockchange', () => {
    S.pointerLocked = !!(document.pointerLockElement || document.mozPointerLockElement || document.webkitPointerLockElement);
    console.info('Pointer lock:', S.pointerLocked ? 'active' : 'inactive');
    S.pointerLocked && (absX = absY = 0.5);
});

const getMods = e => (e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0) | (e.metaKey ? 8 : 0);

const handlers = {
    move: e => {
        if (!S.controlEnabled && !S.touchEnabled) return;
        if (S.pointerLocked) { absX = Math.max(0, Math.min(1, absX + e.movementX * 0.001)); absY = Math.max(0, Math.min(1, absY + e.movementY * 0.001)); send('move', absX, absY); }
        else { const p = toNorm(e.clientX, e.clientY); p && send('move', p.x, p.y); }
    },
    down: e => { if (S.controlEnabled || S.touchEnabled) { e.preventDefault(); send('btn', BMAP[e.button] ?? 0, true); } },
    up: e => { if (S.controlEnabled || S.touchEnabled) { e.preventDefault(); send('btn', BMAP[e.button] ?? 0, false); } },
    wheel: e => { if (S.controlEnabled || S.touchEnabled) { e.preventDefault(); send('wheel', e.deltaX, e.deltaY); } },
    ctx: e => S.controlEnabled && e.preventDefault(),
    keyD: e => { if (!S.controlEnabled) return; if (e.key === 'Escape' && S.pointerLocked) return exitLock(); !e.metaKey && e.preventDefault(); send('key', e.keyCode, 0, true, getMods(e)); },
    keyU: e => { if (!S.controlEnabled) return; !e.metaKey && e.preventDefault(); send('key', e.keyCode, 0, false, getMods(e)); }
};

export const enableControl = () => {
    if (S.controlEnabled) return;
    S.controlEnabled = true; console.info('Control enabled');
    canvas.addEventListener('mousemove', handlers.move); canvas.addEventListener('mousedown', handlers.down);
    canvas.addEventListener('mouseup', handlers.up); canvas.addEventListener('contextmenu', handlers.ctx);
    canvas.addEventListener('wheel', handlers.wheel, { passive: false });
    document.addEventListener('keydown', handlers.keyD); document.addEventListener('keyup', handlers.keyU);
    canvas.style.cursor = 'none';
};

export const disableControl = () => {
    if (!S.controlEnabled) return;
    S.controlEnabled = false; console.info('Control disabled');
    S.pointerLocked && exitLock();
    canvas.removeEventListener('mousemove', handlers.move); canvas.removeEventListener('mousedown', handlers.down);
    canvas.removeEventListener('mouseup', handlers.up); canvas.removeEventListener('contextmenu', handlers.ctx);
    canvas.removeEventListener('wheel', handlers.wheel);
    document.removeEventListener('keydown', handlers.keyD); document.removeEventListener('keyup', handlers.keyU);
    canvas.style.cursor = 'default';
};

const pinchDist2 = (t1, t2) => Math.hypot(t2.clientX - t1.clientX, t2.clientY - t1.clientY);

const updateZoomOff = () => {
    if (S.zoom <= 1) return S.zoomX = S.zoomY = 0;
    const vf = 1 / S.zoom, mo = 1 - vf;
    S.zoomX = S.zoomX * 0.7 + Math.max(0, Math.min(mo, S.touchX - vf / 2)) * 0.3;
    S.zoomY = S.zoomY * 0.7 + Math.max(0, Math.min(mo, S.touchY - vf / 2)) * 0.3;
};

const clearLp = () => { lpTimer && (clearTimeout(lpTimer), lpTimer = null); };

const touchHandlers = {
    start: e => {
        if (!S.touchEnabled || tId !== null) return;
        const t = e.touches[0]; tId = t.identifier;
        tStartX = lastTX = t.clientX; tStartY = lastTY = t.clientY;
        tStartT = performance.now(); tMoved = tDrag = lpActive = false;
        clearLp();
        lpTimer = setTimeout(() => { if (!tMoved && tId !== null) { lpActive = tDrag = true; send('btn', 0, true); console.info('Long press drag'); } lpTimer = null; }, C.LONG_MS);
        if (S.touchMode === 'direct') { const p = toNorm(t.clientX, t.clientY); p && (S.touchX = p.x, S.touchY = p.y, send('move', p.x, p.y)); }
        e.preventDefault();
    },
    move: e => {
        if (pinch || !S.touchEnabled || tId === null) return;
        const t = [...e.touches].find(x => x.identifier === tId);
        if (!t) return;
        const dx = t.clientX - lastTX, dy = t.clientY - lastTY;
        if (Math.abs(t.clientX - tStartX) > C.TAP_THRESH || Math.abs(t.clientY - tStartY) > C.TAP_THRESH) { clearLp(); tMoved = true; }
        if (S.touchMode === 'trackpad') {
            const sens = C.TOUCH_SENS / Math.min(canvas.getBoundingClientRect().width, canvas.getBoundingClientRect().height);
            S.touchX = Math.max(0, Math.min(1, S.touchX + dx * sens)); S.touchY = Math.max(0, Math.min(1, S.touchY + dy * sens));
            send('move', S.touchX, S.touchY);
            S.zoom > 1 && (updateZoomOff(), renderZoomed(), updateTouchUI());
        } else { const p = toNorm(t.clientX, t.clientY); p && (S.touchX = p.x, S.touchY = p.y, send('move', p.x, p.y)); }
        lastTX = t.clientX; lastTY = t.clientY; e.preventDefault();
    },
    end: e => {
        if (!S.touchEnabled || ![...e.changedTouches].some(t => t.identifier === tId)) return;
        clearLp();
        if (!tMoved && performance.now() - tStartT < C.TAP_MS) { send('btn', 0, true); setTimeout(() => send('btn', 0, false), 50); console.info('Tap'); }
        else if (tDrag) { send('btn', 0, false); console.info('Drag ended'); }
        tId = null; tDrag = lpActive = false; e.preventDefault();
    },
    cancel: () => { clearLp(); tDrag && (send('btn', 0, false), console.info('Touch cancelled')); tId = null; tDrag = lpActive = pinch = false; },
    f2Start: e => S.touchEnabled && e.touches.length === 2 && (tf2Start = performance.now()),
    f2End: e => {
        if (S.touchEnabled && tf2Start && performance.now() - tf2Start < C.TAP_MS && e.touches.length === 0 && !pinch) {
            send('btn', 1, true); setTimeout(() => send('btn', 1, false), 50); console.info('Two-finger tap');
        }
        tf2Start = 0;
    },
    pStart: e => {
        if (!S.touchEnabled || S.touchMode !== 'trackpad' || e.touches.length !== 2) return;
        pinch = true; pinchDist = pinchDist2(e.touches[0], e.touches[1]); pinchScale = S.zoom;
        clearLp(); tId = null; tDrag = false;
        e.preventDefault(); console.info('Pinch started');
    },
    pMove: e => {
        if (!S.touchEnabled || !pinch || e.touches.length !== 2) return;
        const ns = Math.max(C.MIN_ZOOM, Math.min(C.MAX_ZOOM, pinchScale + (pinchDist2(e.touches[0], e.touches[1]) - pinchDist) * C.PINCH_SENS));
        if (Math.abs(ns - S.zoom) > 0.01) { ns > 1 ? (S.zoom = ns, updateZoomOff()) : (S.zoom = ns, S.zoomX = S.zoomY = 0); renderZoomed(); updateTouchUI(); }
        e.preventDefault();
    },
    pEnd: e => {
        if (!pinch || e.touches.length >= 2) return;
        pinch = false; console.info(`Pinch ended: ${S.zoom.toFixed(2)}x`);
        S.zoom < 1.05 && (S.zoom = 1, S.zoomX = S.zoomY = 0, renderZoomed()); updateTouchUI();
    }
};

export const enableTouch = () => {
    if (S.touchEnabled) return;
    S.touchEnabled = true; S.touchX = S.touchY = 0.5; S.zoom = 1; S.zoomX = S.zoomY = 0;
    console.info(`Touch enabled (${S.touchMode})`);
    const add = (ev, fn, opts) => canvas.addEventListener(ev, fn, opts);
    add('touchstart', touchHandlers.start, { passive: false }); add('touchmove', touchHandlers.move, { passive: false });
    add('touchend', touchHandlers.end, { passive: false }); add('touchcancel', touchHandlers.cancel);
    add('touchstart', touchHandlers.f2Start); add('touchend', touchHandlers.f2End);
    add('touchstart', touchHandlers.pStart, { passive: false }); add('touchmove', touchHandlers.pMove, { passive: false });
    add('touchend', touchHandlers.pEnd, { passive: false }); add('touchcancel', touchHandlers.pEnd);
    updateTouchUI();
};

export const setTouchMode = m => {
    if (m !== 'trackpad' && m !== 'direct') return;
    S.touchMode = m; console.info(`Touch mode: ${m}`);
    m !== 'trackpad' && (S.zoom = 1, S.zoomX = S.zoomY = 0, renderZoomed()); updateTouchUI();
};

export const updateTouchUI = () => {
    const el = document.getElementById('tStT'); if (!el) return;
    el.textContent = S.touchMode === 'trackpad'
        ? (S.zoom > 1 ? `Zoomed ${S.zoom.toFixed(1)}x - Pinch to zoom, slide to move cursor` : 'Slide to move cursor, tap to click, pinch to zoom, long-press+slide to drag')
        : 'Touch positions cursor directly, tap to click, long-press+slide to drag';
};

export const isTouchDev = () => 'ontouchstart' in window || navigator.maxTouchPoints > 0;
export const isDesktop = () => window.matchMedia('(pointer: fine)').matches;

canvas.addEventListener('click', () => {
    if (isDesktop()) { !S.controlEnabled && enableControl(); S.controlEnabled && !S.pointerLocked && reqLock(); }
});

isTouchDev() && enableTouch();
