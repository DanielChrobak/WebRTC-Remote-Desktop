import { S } from './state.js';

const $ = id => document.getElementById(id);
export const canvas = $('c');
export let canvasW = 0, canvasH = 0;

export const gl = canvas.getContext('webgl2', {
    alpha: false, depth: false, stencil: false, antialias: false,
    desynchronized: true, powerPreference: 'high-performance', preserveDrawingBuffer: false
});

const initWebGL = () => {
    if (!gl) return console.error('WebGL2 not supported'), false;
    const dbg = gl.getExtension('WEBGL_debug_renderer_info');
    if (dbg) {
        try {
            const gpu = gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL);
            if (gpu) console.info('GPU:', gpu);
        } catch (e) { /* GPU info unavailable */ }
    }
    gl.disable(gl.BLEND); gl.disable(gl.DEPTH_TEST); gl.disable(gl.DITHER);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);

    const vs = gl.createShader(gl.VERTEX_SHADER), fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(vs, `#version 300 es\nin vec2 a_pos,a_tex;out vec2 v;void main(){gl_Position=vec4(a_pos,0.,1.);v=a_tex;}`);
    gl.shaderSource(fs, `#version 300 es\nprecision highp float;in vec2 v;uniform sampler2D u;out vec4 o;void main(){o=texture(u,v);}`);
    gl.compileShader(vs); gl.compileShader(fs);

    const pg = gl.createProgram();
    gl.attachShader(pg, vs); gl.attachShader(pg, fs); gl.linkProgram(pg); gl.useProgram(pg);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1,0,1,1,-1,1,1,-1,1,0,0,1,1,1,0]), gl.STATIC_DRAW);

    const pL = gl.getAttribLocation(pg, 'a_pos'), tL = gl.getAttribLocation(pg, 'a_tex');
    gl.enableVertexAttribArray(pL); gl.vertexAttribPointer(pL, 2, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(tL); gl.vertexAttribPointer(tL, 2, gl.FLOAT, false, 16, 8);

    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    [gl.TEXTURE_WRAP_S, gl.TEXTURE_WRAP_T, gl.TEXTURE_MIN_FILTER, gl.TEXTURE_MAG_FILTER].forEach((p, i) =>
        gl.texParameteri(gl.TEXTURE_2D, p, i < 2 ? gl.CLAMP_TO_EDGE : gl.LINEAR));

    updateSize();
    gl.viewport(0, 0, canvasW, canvasH);
    gl.clearColor(0.039, 0.039, 0.043, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    return console.info('WebGL2 ready'), true;
};

export const updateSize = () => {
    const dpr = devicePixelRatio || 1;
    const dW = Math.round(canvas.clientWidth * dpr), dH = Math.round(canvas.clientHeight * dpr);
    if (canvas.width !== dW || canvas.height !== dH) canvas.width = canvasW = dW, canvas.height = canvasH = dH;
};

export const calcVp = (vW, vH, dW, dH) => {
    const va = vW / vH, da = dW / dH;
    return da > va
        ? { x: Math.round((dW - (w = Math.round(dH * va))) / 2), y: 0, w, h: dH }
        : { x: 0, y: Math.round((dH - (h = Math.round(dW / va))) / 2), w: dW, h };
    var w, h;
};

const applyVp = vp => {
    S.zoom > 1
        ? gl.viewport(Math.round(vp.x - S.zoomX * vp.w * S.zoom), Math.round(vp.y + (S.zoomY * S.zoom - (S.zoom - 1)) * vp.h), Math.round(vp.w * S.zoom), Math.round(vp.h * S.zoom))
        : gl.viewport(vp.x, vp.y, vp.w, vp.h);
};

const addLat = (n, v) => { if (Number.isFinite(v) && v >= 0 && v < 10000) { S.lat[n].push(v); S.lat[n].length > 60 && S.lat[n].shift(); } };

export const render = (frame, meta) => {
    const t1 = performance.now(), vW = frame.displayWidth, vH = frame.displayHeight;
    updateSize();

    if (S.W !== vW || S.H !== vH) {
        const was = S.W > 0; S.W = vW; S.H = vH;
        console.info(`Resolution: ${vW}x${vH}`);
        was && (S.needKey = true);
    }

    gl.viewport(0, 0, canvasW, canvasH); gl.clearColor(0.039, 0.039, 0.043, 1); gl.clear(gl.COLOR_BUFFER_BIT);
    const vp = calcVp(vW, vH, canvasW, canvasH); S.lastVp = vp; applyVp(vp);

    try { gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame); gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); gl.flush(); }
    catch (e) { console.error('GL:', e.message); }

    if (meta) {
        addLat('decode', t1 - meta.decStart); addLat('render', performance.now() - t1);
        addLat('encode', meta.encMs); addLat('network', meta.netMs); addLat('queue', meta.queueMs);
        S.frameMeta.delete(meta.capTs);
    }
    S.stats.rend++; S.stats.tRend++; frame.close();
};

export const renderZoomed = () => {
    if (!gl || S.W <= 0 || S.H <= 0) return;
    updateSize(); gl.viewport(0, 0, canvasW, canvasH); gl.clearColor(0.039, 0.039, 0.043, 1); gl.clear(gl.COLOR_BUFFER_BIT);
    applyVp(calcVp(S.W, S.H, canvasW, canvasH)); gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); gl.flush();
};

const getStats = a => a.length ? { avg: a.reduce((x, y) => x + y, 0) / a.length, min: Math.min(...a), max: Math.max(...a) } : { avg: 0 };
export const getLatStats = n => getStats(S.lat[n]);
export const getJitterStats = () => {
    const d = S.jitter.deltas;
    return d.length ? { avg: d.reduce((x, y) => x + y, 0) / d.length, max: Math.max(...d) } : { avg: 0, max: 0 };
};

const handleResize = () => {
    updateSize();
    if (S.W > 0 && S.H > 0 && gl) {
        gl.viewport(0, 0, canvasW, canvasH); gl.clearColor(0.039, 0.039, 0.043, 1); gl.clear(gl.COLOR_BUFFER_BIT);
        applyVp(calcVp(S.W, S.H, canvasW, canvasH)); gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); gl.flush();
    }
};

let rto;
const debResize = () => { clearTimeout(rto); rto = setTimeout(handleResize, 50); };
const delayedResize = () => { setTimeout(handleResize, 100); setTimeout(handleResize, 300); };

['resize'].forEach(e => window.addEventListener(e, debResize));
window.addEventListener('orientationchange', delayedResize);
window.screen?.orientation?.addEventListener('change', () => { console.info('Orientation:', screen.orientation.type); setTimeout(handleResize, 100); });
window.visualViewport?.addEventListener('resize', debResize);
window.matchMedia?.('(orientation: portrait)').addEventListener?.('change', e => { console.info('Orientation:', e.matches ? 'portrait' : 'landscape'); setTimeout(handleResize, 100); });

initWebGL();
