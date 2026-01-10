/**
 * @file renderer.js
 * @brief WebGL2 video rendering engine
 * @copyright 2025-2026 Daniel Chrobak
 */

import { S } from './state.js';

export const canvas = document.getElementById('c');
export let canvasW = 0;
export let canvasH = 0;

export const gl = canvas.getContext('webgl2', {
    alpha: false,
    depth: false,
    stencil: false,
    antialias: false,
    desynchronized: true,
    powerPreference: 'high-performance',
    preserveDrawingBuffer: false
});

let w, h;

/**
 * Initializes the WebGL2 rendering context.
 * Sets up shaders, buffers, and textures for video frame rendering.
 * @returns {boolean} True if initialization succeeded
 */
const init = () => {
    if (!gl) {
        console.error('WebGL2 not supported');
        return false;
    }

    const dbg = gl.getExtension('WEBGL_debug_renderer_info');
    if (dbg) {
        try {
            const gpu = gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL);
            if (gpu) console.info('GPU:', gpu);
        } catch {}
    }

    gl.disable(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.DITHER);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);

    const vs = gl.createShader(gl.VERTEX_SHADER);
    const fs = gl.createShader(gl.FRAGMENT_SHADER);

    gl.shaderSource(vs, `#version 300 es
in vec2 a_pos, a_tex;
out vec2 v;
void main() {
    gl_Position = vec4(a_pos, 0., 1.);
    v = a_tex;
}`);

    gl.shaderSource(fs, `#version 300 es
precision highp float;
in vec2 v;
uniform sampler2D u;
out vec4 o;
void main() {
    o = texture(u, v);
}`);

    gl.compileShader(vs);
    gl.compileShader(fs);

    if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS) ||
        !gl.getShaderParameter(fs, gl.COMPILE_STATUS)) {
        console.error('Shader:', gl.getShaderInfoLog(vs) || gl.getShaderInfoLog(fs));
        return false;
    }

    const pg = gl.createProgram();
    gl.attachShader(pg, vs);
    gl.attachShader(pg, fs);
    gl.linkProgram(pg);

    if (!gl.getProgramParameter(pg, gl.LINK_STATUS)) {
        console.error('Link:', gl.getProgramInfoLog(pg));
        return false;
    }

    gl.useProgram(pg);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
        -1, -1, 0, 1,
         1, -1, 1, 1,
        -1,  1, 0, 0,
         1,  1, 1, 0
    ]), gl.STATIC_DRAW);

    const pL = gl.getAttribLocation(pg, 'a_pos');
    const tL = gl.getAttribLocation(pg, 'a_tex');

    gl.enableVertexAttribArray(pL);
    gl.vertexAttribPointer(pL, 2, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(tL);
    gl.vertexAttribPointer(tL, 2, gl.FLOAT, false, 16, 8);

    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    [gl.TEXTURE_WRAP_S, gl.TEXTURE_WRAP_T, gl.TEXTURE_MIN_FILTER, gl.TEXTURE_MAG_FILTER]
        .forEach((p, i) => gl.texParameteri(gl.TEXTURE_2D, p, i < 2 ? gl.CLAMP_TO_EDGE : gl.LINEAR));

    updateSize();
    clear();
    console.info('WebGL2 ready');
    return true;
};

/**
 * Updates canvas dimensions based on device pixel ratio.
 */
export const updateSize = () => {
    const dpr = devicePixelRatio || 1;
    const dW = Math.round(canvas.clientWidth * dpr);
    const dH = Math.round(canvas.clientHeight * dpr);

    if (canvas.width !== dW || canvas.height !== dH) {
        canvas.width = canvasW = dW;
        canvas.height = canvasH = dH;
    }
};

/**
 * Calculates viewport dimensions maintaining aspect ratio.
 * @param {number} vW - Video width
 * @param {number} vH - Video height
 * @param {number} dW - Display width
 * @param {number} dH - Display height
 * @returns {Object} Viewport coordinates and dimensions
 */
export const calcVp = (vW, vH, dW, dH) => {
    const va = vW / vH;
    const da = dW / dH;

    if (da > va) {
        w = Math.round(dH * va);
        return { x: Math.round((dW - w) / 2), y: 0, w, h: dH };
    } else {
        h = Math.round(dW / va);
        return { x: 0, y: Math.round((dH - h) / 2), w: dW, h };
    }
};

/**
 * Applies viewport with optional zoom transformation.
 * @param {Object} vp - Viewport object
 */
const applyVp = vp => {
    if (S.zoom > 1) {
        gl.viewport(
            Math.round(vp.x - S.zoomX * vp.w * S.zoom),
            Math.round(vp.y + (S.zoomY * S.zoom - (S.zoom - 1)) * vp.h),
            Math.round(vp.w * S.zoom),
            Math.round(vp.h * S.zoom)
        );
    } else {
        gl.viewport(vp.x, vp.y, vp.w, vp.h);
    }
};

/**
 * Clears the canvas with background color.
 */
const clear = () => {
    gl.viewport(0, 0, canvasW, canvasH);
    gl.clearColor(0.039, 0.039, 0.043, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
};

/**
 * Draws the current frame to the canvas.
 * @param {Object} vp - Viewport object
 */
const draw = vp => {
    clear();
    applyVp(vp);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.flush();
};

/**
 * Adds a latency measurement to the tracking array.
 * @param {string} n - Latency category name
 * @param {number} v - Latency value in milliseconds
 */
const addLat = (n, v) => {
    if (Number.isFinite(v) && v >= 0 && v < 10000) {
        S.lat[n].push(v);
        if (S.lat[n].length > 60) S.lat[n].shift();
    }
};

/**
 * Renders a decoded video frame.
 * @param {VideoFrame} frame - Decoded video frame
 * @param {Object} meta - Frame metadata
 */
export const render = (frame, meta) => {
    const t1 = performance.now();
    const vW = frame.displayWidth;
    const vH = frame.displayHeight;

    updateSize();

    if (S.W !== vW || S.H !== vH) {
        const was = S.W > 0;
        S.W = vW;
        S.H = vH;
        console.info(`Resolution: ${vW}x${vH}`);
        if (was) S.needKey = true;
    }

    const vp = S.lastVp = calcVp(vW, vH, canvasW, canvasH);
    clear();
    applyVp(vp);

    try {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        gl.flush();
    } catch (e) {
        console.error('GL:', e.message);
    }

    if (meta) {
        addLat('decode', t1 - meta.decStart);
        addLat('render', performance.now() - t1);
        addLat('encode', meta.encMs);
        addLat('network', meta.netMs);
        addLat('queue', meta.queueMs);
        S.frameMeta.delete(meta.capTs);
    }

    S.stats.rend++;
    S.stats.tRend++;
    frame.close();
};

/**
 * Re-renders the current frame with zoom applied.
 */
export const renderZoomed = () => {
    if (gl && S.W > 0 && S.H > 0) {
        updateSize();
        draw(calcVp(S.W, S.H, canvasW, canvasH));
    }
};

/**
 * Calculates statistics for a latency array.
 * @param {number[]} a - Array of latency values
 * @returns {Object} Statistics object with avg, min, max
 */
const stats = a => {
    if (!a.length) return { avg: 0 };
    return {
        avg: a.reduce((x, y) => x + y, 0) / a.length,
        min: Math.min(...a),
        max: Math.max(...a)
    };
};

/**
 * Gets latency statistics for a category.
 * @param {string} n - Category name
 * @returns {Object} Statistics object
 */
export const getLatStats = n => stats(S.lat[n]);

/**
 * Gets jitter statistics.
 * @returns {Object} Jitter statistics with avg and max
 */
export const getJitterStats = () => {
    const d = S.jitter.deltas;
    if (!d.length) return { avg: 0, max: 0 };
    return {
        avg: d.reduce((x, y) => x + y, 0) / d.length,
        max: Math.max(...d)
    };
};

/**
 * Handles window resize events.
 */
const onResize = () => {
    updateSize();
    if (S.W > 0 && S.H > 0 && gl) {
        draw(calcVp(S.W, S.H, canvasW, canvasH));
    }
};

let rto;
const debounceResize = () => {
    clearTimeout(rto);
    rto = setTimeout(onResize, 50);
};

window.addEventListener('resize', debounceResize);

window.addEventListener('orientationchange', () => {
    setTimeout(onResize, 100);
    setTimeout(onResize, 300);
});

window.screen?.orientation?.addEventListener('change', () => {
    console.info('Orientation:', screen.orientation.type);
    setTimeout(onResize, 100);
});

window.visualViewport?.addEventListener('resize', debounceResize);

window.matchMedia?.('(orientation: portrait)').addEventListener?.('change', e => {
    console.info('Orientation:', e.matches ? 'portrait' : 'landscape');
    setTimeout(onResize, 100);
});

init();
