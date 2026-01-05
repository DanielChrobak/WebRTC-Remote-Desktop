import { MSG, C, S, mkBuf } from './state.js';
import { canvas, canvasW, canvasH, calcVp, renderZoomed } from './renderer.js';

let absX = 0.5, absY = 0.5, tStartX = 0, tStartY = 0, tStartT = 0, tMoved = false, tDrag = false;
let lastTX = 0, lastTY = 0, tId = null, lpTimer = null, lpActive = false, tf2Start = 0, tf2Active = false;
let pinch = false, pinchPending = false, pinchDist = 0, pinchScale = 1;
let scrolling = false, scrollPending = false, lastScrollX = 0, lastScrollY = 0, scrollAccumX = 0, scrollAccumY = 0;
const SCROLL_THRESH = 8, SCROLL_SENS = 2.5, BMAP = { 0: 0, 2: 1, 1: 2, 3: 3, 4: 4 };

const send = (type, ...a) => {
    if ((!S.controlEnabled && !S.touchEnabled) || S.dc?.readyState !== 'open') return;
    const buf = {
        move: () => { S.stats.moves++; return mkBuf(12, v => { v.setUint32(0, MSG.MOUSE_MOVE, true); v.setFloat32(4, a[0], true); v.setFloat32(8, a[1], true); }); },
        btn: () => { S.stats.clicks++; return mkBuf(6, v => { v.setUint32(0, MSG.MOUSE_BTN, true); v.setUint8(4, a[0]); v.setUint8(5, a[1] ? 1 : 0); }); },
        wheel: () => mkBuf(8, v => { v.setUint32(0, MSG.MOUSE_WHEEL, true); v.setInt16(4, Math.round(a[0]), true); v.setInt16(6, Math.round(a[1]), true); }),
        key: () => { S.stats.keys++; return mkBuf(10, v => { v.setUint32(0, MSG.KEY, true); v.setUint16(4, a[0], true); v.setUint16(6, a[1], true); v.setUint8(8, a[2] ? 1 : 0); v.setUint8(9, a[3]); }); }
    }[type]();
    try { S.dc.send(buf); } catch {}
};

const toNorm = (cx, cy) => {
    if (S.W <= 0 || S.H <= 0) return null;
    const r = canvas.getBoundingClientRect(), dpr = devicePixelRatio || 1;
    const vp = S.lastVp = calcVp(S.W, S.H, canvasW, canvasH), x = (cx - r.left) * dpr, y = (cy - r.top) * dpr;
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
const getSens = () => { const b = 0.001; return S.W > 0 && S.H > 0 ? { x: b, y: b * S.W / S.H } : { x: b, y: b }; };

const handlers = {
    move: e => {
        if (!S.controlEnabled && !S.touchEnabled) return;
        if (S.pointerLocked) {
            const s = getSens();
            absX = Math.max(0, Math.min(1, absX + e.movementX * s.x));
            absY = Math.max(0, Math.min(1, absY + e.movementY * s.y));
            send('move', absX, absY);
        } else { const p = toNorm(e.clientX, e.clientY); p && send('move', p.x, p.y); }
    },
    down: e => { (S.controlEnabled || S.touchEnabled) && (e.preventDefault(), send('btn', BMAP[e.button] ?? 0, true)); },
    up: e => { (S.controlEnabled || S.touchEnabled) && (e.preventDefault(), send('btn', BMAP[e.button] ?? 0, false)); },
    wheel: e => { (S.controlEnabled || S.touchEnabled) && (e.preventDefault(), send('wheel', e.deltaX, e.deltaY)); },
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
const clearLp = () => lpTimer && (clearTimeout(lpTimer), lpTimer = null);

const updateZoomOff = () => {
    if (S.zoom <= 1) return S.zoomX = S.zoomY = 0;
    const vf = 1 / S.zoom, mo = 1 - vf;
    S.zoomX = S.zoomX * 0.7 + Math.max(0, Math.min(mo, S.touchX - vf / 2)) * 0.3;
    S.zoomY = S.zoomY * 0.7 + Math.max(0, Math.min(mo, S.touchY - vf / 2)) * 0.3;
};

const handleTouchStart = e => {
    if (!S.touchEnabled) return;
    const touches = e.touches;
    if (touches.length === 2) {
        tf2Start = tf2Start || Math.min(performance.now(), tStartT || performance.now());
        tf2Active = true; clearLp(); tMoved = true;
        if (S.touchMode === 'trackpad') {
            pinchPending = scrollPending = true;
            pinchDist = pinchDist2(touches[0], touches[1]); pinchScale = S.zoom;
            lastScrollX = (touches[0].clientX + touches[1].clientX) / 2;
            lastScrollY = (touches[0].clientY + touches[1].clientY) / 2;
            scrollAccumX = scrollAccumY = 0; tId = null; tDrag = false;
        }
        e.preventDefault(); return;
    }
    if (tId !== null) return;
    const t = touches[0];
    tId = t.identifier; tStartX = lastTX = t.clientX; tStartY = lastTY = t.clientY;
    tStartT = performance.now(); tMoved = tDrag = lpActive = false; clearLp();
    lpTimer = setTimeout(() => { if (!tMoved && tId !== null) { lpActive = tDrag = true; send('btn', 0, true); console.info('Long press drag'); } lpTimer = null; }, C.LONG_MS);
    if (S.touchMode === 'direct') { const p = toNorm(t.clientX, t.clientY); if (p) { S.touchX = p.x; S.touchY = p.y; send('move', p.x, p.y); } }
    e.preventDefault();
};

const handleTouchMove = e => {
    if (!S.touchEnabled) return;
    const touches = e.touches;
    if (touches.length === 2 && S.touchMode === 'trackpad') {
        if (!pinchPending && !pinch && !scrollPending && !scrolling) return;
        const newDist = pinchDist2(touches[0], touches[1]), distDelta = Math.abs(newDist - pinchDist);
        const centerX = (touches[0].clientX + touches[1].clientX) / 2, centerY = (touches[0].clientY + touches[1].clientY) / 2;
        const scrollDeltaX = centerX - lastScrollX, scrollDeltaY = centerY - lastScrollY;

        if (!pinch && !scrolling) {
            if (distDelta > 15) { pinch = true; scrollPending = false; console.info('Pinch started'); }
            else if (Math.abs(scrollDeltaX) > SCROLL_THRESH || Math.abs(scrollDeltaY) > SCROLL_THRESH) { scrolling = true; pinchPending = false; console.info('Two-finger scroll started'); }
        }
        if (pinch) {
            const ns = Math.max(C.MIN_ZOOM, Math.min(C.MAX_ZOOM, pinchScale + (newDist - pinchDist) * C.PINCH_SENS));
            if (Math.abs(ns - S.zoom) > 0.01) { S.zoom = ns; ns > 1 ? updateZoomOff() : (S.zoomX = S.zoomY = 0); renderZoomed(); updateTouchUI(); }
        }
        if (scrolling) {
            scrollAccumX += scrollDeltaX; scrollAccumY += scrollDeltaY;
            let sentX = 0, sentY = 0;
            if (Math.abs(scrollAccumX) >= 1) { sentX = scrollAccumX * SCROLL_SENS; scrollAccumX = 0; }
            if (Math.abs(scrollAccumY) >= 1) { sentY = -scrollAccumY * SCROLL_SENS; scrollAccumY = 0; }
            (sentX || sentY) && send('wheel', sentX, sentY);
            lastScrollX = centerX; lastScrollY = centerY;
        }
        e.preventDefault(); return;
    }
    if (pinch || scrolling || tId === null) return;
    const t = [...touches].find(x => x.identifier === tId);
    if (!t) return;
    const dx = t.clientX - lastTX, dy = t.clientY - lastTY;
    if (Math.abs(t.clientX - tStartX) > C.TAP_THRESH || Math.abs(t.clientY - tStartY) > C.TAP_THRESH) { clearLp(); tMoved = true; }

    if (S.touchMode === 'trackpad') {
        const rect = canvas.getBoundingClientRect();
        S.touchX = Math.max(0, Math.min(1, S.touchX + dx * C.TOUCH_SENS / rect.width));
        S.touchY = Math.max(0, Math.min(1, S.touchY + dy * C.TOUCH_SENS / rect.height));
        send('move', S.touchX, S.touchY);
        if (S.zoom > 1) { updateZoomOff(); renderZoomed(); updateTouchUI(); }
    } else { const p = toNorm(t.clientX, t.clientY); if (p) { S.touchX = p.x; S.touchY = p.y; send('move', p.x, p.y); } }
    lastTX = t.clientX; lastTY = t.clientY; e.preventDefault();
};

const handleTouchEnd = e => {
    if (!S.touchEnabled) return;
    const touches = e.touches, changedTouches = [...e.changedTouches];

    if (pinch || pinchPending || scrolling || scrollPending) {
        if (touches.length < 2) {
            pinchPending = scrollPending = false;
            if (pinch) { pinch = tf2Active = false; tf2Start = 0; console.info(`Pinch ended: ${S.zoom.toFixed(2)}x`); if (S.zoom < 1.05) { S.zoom = 1; S.zoomX = S.zoomY = 0; renderZoomed(); } updateTouchUI(); }
            if (scrolling) { scrolling = tf2Active = false; tf2Start = 0; scrollAccumX = scrollAccumY = 0; console.info('Two-finger scroll ended'); }
        }
    }
    if (tf2Active && touches.length === 0 && !pinch && !scrolling) {
        if (performance.now() - tf2Start < C.TAP_MS * 1.5) { send('btn', 1, true); setTimeout(() => send('btn', 1, false), 50); console.info('Two-finger tap (right click)'); }
        tf2Start = 0; tf2Active = false;
    } else if (touches.length === 0) { tf2Start = 0; tf2Active = false; }

    if (!changedTouches.some(t => t.identifier === tId)) return;
    clearLp();
    if (!tMoved && !tf2Active && performance.now() - tStartT < C.TAP_MS) { send('btn', 0, true); setTimeout(() => send('btn', 0, false), 50); console.info('Tap'); }
    else if (tDrag) { send('btn', 0, false); console.info('Drag ended'); }
    tId = null; tDrag = lpActive = false; e.preventDefault();
};

const handleTouchCancel = () => {
    clearLp();
    tDrag && (send('btn', 0, false), console.info('Touch cancelled'));
    tId = null; tDrag = lpActive = pinch = pinchPending = tf2Active = scrolling = scrollPending = false;
    tf2Start = scrollAccumX = scrollAccumY = 0;
};

export const enableTouch = () => {
    if (S.touchEnabled) return;
    S.touchEnabled = true; S.touchX = S.touchY = 0.5; S.zoom = 1; S.zoomX = S.zoomY = 0;
    console.info(`Touch enabled (${S.touchMode})`);
    canvas.addEventListener('touchstart', handleTouchStart, { passive: false });
    canvas.addEventListener('touchmove', handleTouchMove, { passive: false });
    canvas.addEventListener('touchend', handleTouchEnd, { passive: false });
    canvas.addEventListener('touchcancel', handleTouchCancel);
    updateTouchUI();
};

export const setTouchMode = m => {
    if (m !== 'trackpad' && m !== 'direct') return;
    S.touchMode = m; console.info(`Touch mode: ${m}`);
    if (m !== 'trackpad') { S.zoom = 1; S.zoomX = S.zoomY = 0; renderZoomed(); }
    updateTouchUI();
};

export const updateTouchUI = () => {
    const el = document.getElementById('tStT');
    el && (el.textContent = S.touchMode === 'trackpad' ? (S.zoom > 1 ? `${S.zoom.toFixed(1)}x zoom` : 'Trackpad mode') : 'Direct mode');
};

export const isTouchDev = () => 'ontouchstart' in window || navigator.maxTouchPoints > 0;
export const isDesktop = () => window.matchMedia('(pointer: fine)').matches;

canvas.addEventListener('click', () => {
    if (isDesktop()) { if (!S.controlEnabled) enableControl(); if (S.controlEnabled && !S.pointerLocked) reqLock(); }
});

isTouchDev() && enableTouch();
