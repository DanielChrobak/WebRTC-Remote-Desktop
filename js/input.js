/**
 * @file input.js
 * @brief Mouse, keyboard, and touch input handling
 * @copyright 2025-2026 Daniel Chrobak
 */

import { MSG, C, S, mkBuf } from './state.js';
import { canvas, canvasW, canvasH, calcVp, renderZoomed } from './renderer.js';

let tStartX = 0, tStartY = 0, tStartT = 0, tMoved = false, tDrag = false;
let lastTX = 0, lastTY = 0, tId = null, lpTimer = null;
let tf2Start = 0, tf2Active = false, pinch = false, pinchPending = false;
let pinchDist = 0, pinchScale = 1, scroll = false, scrollPending = false;
let lastScrollX = 0, lastScrollY = 0, scrollAccX = 0, scrollAccY = 0;
let escapeDownTime = 0, escapeHoldTimer = null, escapeProgressInterval = null, escapeSentToRemote = false;

const ESCAPE_HOLD_DURATION = 1500;
const SCROLL_THRESH = 8, SCROLL_SENS = 2.5;
const BMAP = { 0: 0, 2: 1, 1: 2, 3: 3, 4: 4 };

let escapeIndicator = null, escapeProgress = null;
let isKeyboardLockedFn = () => false, exitFullscreenFn = () => {};

export const setKeyboardLockFns = (fn, exitFn) => { isKeyboardLockedFn = fn; exitFullscreenFn = exitFn; };

const getEscapeIndicator = () => {
    if (!escapeIndicator) {
        escapeIndicator = document.getElementById('escapeIndicator');
        escapeProgress = document.getElementById('escapeProgress');
    }
    return { indicator: escapeIndicator, progress: escapeProgress };
};

const showEscapeIndicator = () => {
    const { indicator, progress } = getEscapeIndicator();
    if (indicator && progress) {
        progress.style.setProperty('--progress', '0%');
        indicator.classList.add('visible');
    }
};

const hideEscapeIndicator = () => getEscapeIndicator().indicator?.classList.remove('visible');

const updateEscapeProgress = percent => getEscapeIndicator().progress?.style.setProperty('--progress', `${Math.min(100, percent)}%`);

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
    const r = canvas.getBoundingClientRect();
    const dpr = devicePixelRatio || 1;
    const vp = S.lastVp = calcVp(S.W, S.H, canvasW, canvasH);
    const x = (cx - r.left) * dpr, y = (cy - r.top) * dpr;
    if (x < vp.x || x > vp.x + vp.w || y < vp.y || y > vp.y + vp.h) return null;
    return { x: Math.max(0, Math.min(1, (x - vp.x) / vp.w)), y: Math.max(0, Math.min(1, (y - vp.y) / vp.h)) };
};

const getMods = e => (e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0) | (e.metaKey ? 8 : 0);

const isInputFocused = () => {
    const el = document.activeElement;
    return el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable);
};

const clearEscapeTimer = () => {
    clearTimeout(escapeHoldTimer);
    clearInterval(escapeProgressInterval);
    escapeHoldTimer = escapeProgressInterval = null;
    hideEscapeIndicator();
};

const handleEscapeKey = (e, down) => {
    const keyboardLocked = isKeyboardLockedFn();

    if (down && keyboardLocked && escapeDownTime === 0) {
        escapeDownTime = performance.now();
        escapeSentToRemote = false;
        showEscapeIndicator();
        escapeProgressInterval = setInterval(() => updateEscapeProgress((performance.now() - escapeDownTime) / ESCAPE_HOLD_DURATION * 100), 50);
        escapeHoldTimer = setTimeout(() => { console.info('Escape held - exiting fullscreen'); clearEscapeTimer(); exitFullscreenFn(); escapeDownTime = 0; }, ESCAPE_HOLD_DURATION);
        send('key', e.keyCode, 0, true, getMods(e));
        escapeSentToRemote = true;
        e.preventDefault();
        return true;
    }

    if (!down && escapeDownTime > 0) {
        clearEscapeTimer();
        if (escapeSentToRemote) send('key', e.keyCode, 0, false, getMods(e));
        escapeDownTime = 0;
        escapeSentToRemote = false;
        e.preventDefault();
        return true;
    }

    return false;
};

const H = {
    move: e => { if (S.controlEnabled || S.touchEnabled) { const p = toNorm(e.clientX, e.clientY); if (p) send('move', p.x, p.y); } },
    down: e => { if (S.controlEnabled || S.touchEnabled) { e.preventDefault(); send('btn', BMAP[e.button] ?? 0, true); } },
    up: e => { if (S.controlEnabled || S.touchEnabled) { e.preventDefault(); send('btn', BMAP[e.button] ?? 0, false); } },
    wheel: e => { if (S.controlEnabled || S.touchEnabled) { e.preventDefault(); send('wheel', e.deltaX, e.deltaY); } },
    ctx: e => { if (S.controlEnabled) e.preventDefault(); },
    keyD: e => { if (!S.controlEnabled || isInputFocused()) return; if (e.key === 'Escape' && handleEscapeKey(e, true)) return; if (!e.metaKey) e.preventDefault(); send('key', e.keyCode, 0, true, getMods(e)); },
    keyU: e => { if (!S.controlEnabled || isInputFocused()) return; if (e.key === 'Escape' && handleEscapeKey(e, false)) return; if (!e.metaKey) e.preventDefault(); send('key', e.keyCode, 0, false, getMods(e)); }
};

export const enableControl = () => {
    if (S.controlEnabled) return;
    S.controlEnabled = true;
    console.info('Control enabled');
    canvas.addEventListener('mousemove', H.move);
    canvas.addEventListener('mousedown', H.down);
    canvas.addEventListener('mouseup', H.up);
    canvas.addEventListener('contextmenu', H.ctx);
    canvas.addEventListener('wheel', H.wheel, { passive: false });
    document.addEventListener('keydown', H.keyD);
    document.addEventListener('keyup', H.keyU);
};

export const disableControl = () => {
    if (!S.controlEnabled) return;
    S.controlEnabled = false;
    console.info('Control disabled');
    canvas.removeEventListener('mousemove', H.move);
    canvas.removeEventListener('mousedown', H.down);
    canvas.removeEventListener('mouseup', H.up);
    canvas.removeEventListener('contextmenu', H.ctx);
    canvas.removeEventListener('wheel', H.wheel);
    document.removeEventListener('keydown', H.keyD);
    document.removeEventListener('keyup', H.keyU);
};

const dist2 = (t1, t2) => Math.hypot(t2.clientX - t1.clientX, t2.clientY - t1.clientY);
const clearLp = () => { if (lpTimer) { clearTimeout(lpTimer); lpTimer = null; } };

const updateZoom = () => {
    if (S.zoom <= 1) { S.zoomX = S.zoomY = 0; return; }
    const vf = 1 / S.zoom, mo = 1 - vf;
    S.zoomX = S.zoomX * 0.7 + Math.max(0, Math.min(mo, S.touchX - vf / 2)) * 0.3;
    S.zoomY = S.zoomY * 0.7 + Math.max(0, Math.min(mo, S.touchY - vf / 2)) * 0.3;
};

export const updateTouchUI = () => {
    const el = document.getElementById('tStT');
    if (el) el.textContent = S.touchMode === 'trackpad' ? (S.zoom > 1 ? `${S.zoom.toFixed(1)}x zoom` : 'Trackpad mode') : 'Direct mode';
};

const tStart = e => {
    if (!S.touchEnabled) return;
    const ts = e.touches;

    if (ts.length === 2) {
        tf2Start = tf2Start || Math.min(performance.now(), tStartT || performance.now());
        tf2Active = true;
        clearLp();
        tMoved = true;
        if (S.touchMode === 'trackpad') {
            pinchPending = scrollPending = true;
            pinchDist = dist2(ts[0], ts[1]);
            pinchScale = S.zoom;
            lastScrollX = (ts[0].clientX + ts[1].clientX) / 2;
            lastScrollY = (ts[0].clientY + ts[1].clientY) / 2;
            scrollAccX = scrollAccY = 0;
            tId = null;
            tDrag = false;
        }
        e.preventDefault();
        return;
    }

    if (tId !== null) return;
    const t = ts[0];
    tId = t.identifier;
    tStartX = lastTX = t.clientX;
    tStartY = lastTY = t.clientY;
    tStartT = performance.now();
    tMoved = tDrag = false;
    clearLp();

    lpTimer = setTimeout(() => { if (!tMoved && tId !== null) { tDrag = true; send('btn', 0, true); console.info('Long press drag'); } lpTimer = null; }, C.LONG_MS);

    if (S.touchMode === 'direct') {
        const p = toNorm(t.clientX, t.clientY);
        if (p) { S.touchX = p.x; S.touchY = p.y; send('move', p.x, p.y); }
    }
    e.preventDefault();
};

const tMove = e => {
    if (!S.touchEnabled) return;
    const ts = e.touches;

    if (ts.length === 2 && S.touchMode === 'trackpad') {
        if (!pinchPending && !pinch && !scrollPending && !scroll) return;

        const nd = dist2(ts[0], ts[1]), dd = Math.abs(nd - pinchDist);
        const cx = (ts[0].clientX + ts[1].clientX) / 2, cy = (ts[0].clientY + ts[1].clientY) / 2;
        const sdx = cx - lastScrollX, sdy = cy - lastScrollY;

        if (!pinch && !scroll) {
            if (dd > 15) { pinch = true; scrollPending = false; console.info('Pinch started'); }
            else if (Math.abs(sdx) > SCROLL_THRESH || Math.abs(sdy) > SCROLL_THRESH) { scroll = true; pinchPending = false; console.info('Two-finger scroll started'); }
        }

        if (pinch) {
            const ns = Math.max(C.MIN_ZOOM, Math.min(C.MAX_ZOOM, pinchScale + (nd - pinchDist) * C.PINCH_SENS));
            if (Math.abs(ns - S.zoom) > 0.01) {
                S.zoom = ns;
                if (ns > 1) updateZoom(); else S.zoomX = S.zoomY = 0;
                renderZoomed();
                updateTouchUI();
            }
        }

        if (scroll) {
            scrollAccX += sdx;
            scrollAccY += sdy;
            let sx = 0, sy = 0;
            if (Math.abs(scrollAccX) >= 1) { sx = scrollAccX * SCROLL_SENS; scrollAccX = 0; }
            if (Math.abs(scrollAccY) >= 1) { sy = -scrollAccY * SCROLL_SENS; scrollAccY = 0; }
            if (sx || sy) send('wheel', sx, sy);
            lastScrollX = cx;
            lastScrollY = cy;
        }
        e.preventDefault();
        return;
    }

    if (pinch || scroll || tId === null) return;
    const t = [...ts].find(x => x.identifier === tId);
    if (!t) return;

    const dx = t.clientX - lastTX, dy = t.clientY - lastTY;
    if (Math.abs(t.clientX - tStartX) > C.TAP_THRESH || Math.abs(t.clientY - tStartY) > C.TAP_THRESH) { clearLp(); tMoved = true; }

    if (S.touchMode === 'trackpad') {
        const r = canvas.getBoundingClientRect();
        S.touchX = Math.max(0, Math.min(1, S.touchX + dx * C.TOUCH_SENS / r.width));
        S.touchY = Math.max(0, Math.min(1, S.touchY + dy * C.TOUCH_SENS / r.height));
        send('move', S.touchX, S.touchY);
        if (S.zoom > 1) { updateZoom(); renderZoomed(); updateTouchUI(); }
    } else {
        const p = toNorm(t.clientX, t.clientY);
        if (p) { S.touchX = p.x; S.touchY = p.y; send('move', p.x, p.y); }
    }
    lastTX = t.clientX;
    lastTY = t.clientY;
    e.preventDefault();
};

const tEnd = e => {
    if (!S.touchEnabled) return;
    const ts = e.touches, ct = [...e.changedTouches];

    if ((pinch || pinchPending || scroll || scrollPending) && ts.length < 2) {
        pinchPending = scrollPending = false;
        if (pinch) { pinch = tf2Active = false; tf2Start = 0; console.info(`Pinch ended: ${S.zoom.toFixed(2)}x`); if (S.zoom < 1.05) { S.zoom = 1; S.zoomX = S.zoomY = 0; renderZoomed(); } updateTouchUI(); }
        if (scroll) { scroll = tf2Active = false; tf2Start = 0; scrollAccX = scrollAccY = 0; console.info('Two-finger scroll ended'); }
    }

    if (tf2Active && ts.length === 0 && !pinch && !scroll) {
        if (performance.now() - tf2Start < C.TAP_MS * 1.5) { send('btn', 1, true); setTimeout(() => send('btn', 1, false), 50); console.info('Two-finger tap (right click)'); }
        tf2Start = 0;
        tf2Active = false;
    } else if (ts.length === 0) {
        tf2Start = 0;
        tf2Active = false;
    }

    if (!ct.some(t => t.identifier === tId)) return;
    clearLp();

    if (!tMoved && !tf2Active && performance.now() - tStartT < C.TAP_MS) { send('btn', 0, true); setTimeout(() => send('btn', 0, false), 50); console.info('Tap'); }
    else if (tDrag) { send('btn', 0, false); console.info('Drag ended'); }

    tId = null;
    tDrag = false;
    e.preventDefault();
};

const tCancel = () => {
    clearLp();
    if (tDrag) { send('btn', 0, false); console.info('Touch cancelled'); }
    tId = null;
    tDrag = pinch = pinchPending = tf2Active = scroll = scrollPending = false;
    tf2Start = scrollAccX = scrollAccY = 0;
};

export const enableTouch = () => {
    if (S.touchEnabled) return;
    S.touchEnabled = true;
    S.touchX = S.touchY = 0.5;
    S.zoom = 1;
    S.zoomX = S.zoomY = 0;
    console.info(`Touch enabled (${S.touchMode})`);
    canvas.addEventListener('touchstart', tStart, { passive: false });
    canvas.addEventListener('touchmove', tMove, { passive: false });
    canvas.addEventListener('touchend', tEnd, { passive: false });
    canvas.addEventListener('touchcancel', tCancel);
    updateTouchUI();
};

export const setTouchMode = m => {
    if (m !== 'trackpad' && m !== 'direct') return;
    S.touchMode = m;
    console.info(`Touch mode: ${m}`);
    if (m !== 'trackpad') { S.zoom = 1; S.zoomX = S.zoomY = 0; renderZoomed(); }
    updateTouchUI();
};

export const isTouchDev = () => 'ontouchstart' in window || navigator.maxTouchPoints > 0;
export const isDesktop = () => window.matchMedia('(pointer: fine)').matches;

if (isDesktop()) enableControl();
if (isTouchDev()) enableTouch();
