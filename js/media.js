import { C, S } from './state.js';
import { render } from './renderer.js';

let reqKey = null, scheduledSources = [];
export const setReqKeyFn = fn => reqKey = fn;

export const initDecoder = async (force = false) => {
    if (!window.VideoDecoder) return console.error('VideoDecoder not supported');
    S.ready = false; S.needKey = true; S.lastCapTs = 0;
    if (force) S.W = S.H = 0;
    try { S.decoder?.state !== 'closed' && S.decoder.close(); } catch {}

    const decoder = new VideoDecoder({
        output: f => render(f, S.frameMeta.get(f.timestamp)),
        error: e => {
            console.error('Decoder:', e.message);
            S.ready = true; S.needKey = true; reqKey?.();
            if (!S.reinit) { S.reinit = true; setTimeout(async () => { await initDecoder(); S.reinit = false; }, 50); }
        }
    });
    S.decoder = decoder;

    for (const [hw, label] of [[true, 'HW'], [false, 'SW']]) {
        const cfg = { codec: C.CODEC, optimizeForLatency: true, latencyMode: 'realtime', hardwareAcceleration: hw ? 'prefer-hardware' : 'prefer-software' };
        try { const sup = await VideoDecoder.isConfigSupported(cfg); if (sup.supported) { decoder.configure(sup.config); S.hwAccel = label; console.info(`Decoder: ${label}`); break; } } catch {}
    }

    if (decoder.state !== 'configured') { console.error('Decoder config failed'); S.hwAccel = 'NONE'; return; }
    S.ready = true; reqKey?.();
};

export const decodeFrame = data => {
    if (!S.ready || (!data.isKey && S.needKey)) { S.stats.tDropDec++; !S.ready || reqKey?.(); return; }
    if (!S.decoder || S.decoder.state !== 'configured') {
        S.stats.tDropDec++;
        if (!S.reinit) { S.reinit = true; setTimeout(async () => { await initDecoder(); S.reinit = false; }, 50); }
        return;
    }

    try {
        const q = S.decoder.decodeQueueSize;
        if ((q > 4 && !data.isKey) || q > 6) { reqKey?.(); S.stats.tDropDec++; return; }
        if (!Number.isFinite(data.capTs) || data.capTs < 0) { S.stats.tDropDec++; return; }

        const ds = performance.now();
        S.frameMeta.set(data.capTs, { capTs: data.capTs, encMs: data.encMs, netMs: data.netMs, queueMs: ds - data.fcT, decStart: ds });
        if (S.frameMeta.size > 30) [...S.frameMeta.keys()].sort((a, b) => a - b).slice(0, -20).forEach(k => S.frameMeta.delete(k));

        const dur = S.lastCapTs > 0 && data.capTs > S.lastCapTs ? data.capTs - S.lastCapTs : 16667;
        S.lastCapTs = data.capTs;
        S.decoder.decode(new EncodedVideoChunk({ type: data.isKey ? 'key' : 'delta', timestamp: data.capTs, duration: dur, data: data.buf }));
        S.stats.dec++; S.stats.tDec++;
        if (data.isKey) S.needKey = false;
    } catch (e) { console.error('Decode:', e.message); S.stats.tDropDec++; S.needKey = true; S.ready = true; reqKey?.(); }
};

export const initAudio = async () => {
    if (S.audioCtx) return true;
    try {
        const ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: C.AUDIO_RATE, latencyHint: 'interactive' });
        const gain = ctx.createGain(); gain.gain.value = 1; gain.connect(ctx.destination);
        S.audioCtx = ctx; S.audioGain = gain;
        if (ctx.state === 'suspended') await ctx.resume();

        if (window.AudioDecoder) {
            try {
                const dec = new AudioDecoder({ output: d => S.audioEnabled && S.audioCtx && playAudio(d), error: e => console.warn('Audio decode:', e.message) });
                dec.configure({ codec: 'opus', sampleRate: C.AUDIO_RATE, numberOfChannels: C.AUDIO_CH });
                S.audioDecoder = dec;
            } catch (e) { console.warn('AudioDecoder unavailable:', e.message); }
        }
        console.info('Audio initialized:', ctx.sampleRate, 'Hz');
        return true;
    } catch (e) { console.error('Audio init failed:', e.message); return false; }
};

const playAudio = ad => {
    const { audioCtx: ctx, audioGain: gain } = S;
    if (!ctx || !gain) return;
    try {
        const { numberOfFrames: nf, numberOfChannels: nc, format: fmt, sampleRate: sr } = ad;
        const buf = ctx.createBuffer(nc, nf, sr), isPlanar = fmt.includes('planar'), isS16 = fmt.includes('s16');

        for (let ch = 0; ch < nc; ch++) {
            const pi = isPlanar ? ch : 0, sz = ad.allocationSize({ planeIndex: pi });
            const tv = isS16 ? new Int16Array(sz / 2) : new Float32Array(sz / 4), cd = new Float32Array(nf);
            ad.copyTo(tv, { planeIndex: pi });
            for (let i = 0; i < nf; i++) cd[i] = isS16 ? tv[isPlanar ? i : i * nc + ch] / 32768 : tv[isPlanar ? i : i * nc + ch];
            buf.copyToChannel(cd, ch);
        }

        const dur = nf / sr, now = ctx.currentTime;
        const needsReset = !S.audioPlaying || S.audioNextTime < now - 0.1 || S.audioNextTime > now + 0.5;
        if (needsReset && scheduledSources.length) { scheduledSources.forEach(s => { try { s.stop(); s.disconnect(); } catch {} }); scheduledSources = []; }

        const st = needsReset ? (S.audioPlaying = true, now + C.AUDIO_BUF) : S.audioNextTime;
        const src = ctx.createBufferSource(); src.buffer = buf; src.connect(gain); src.start(st);
        scheduledSources.push(src);
        src.onended = () => { const i = scheduledSources.indexOf(src); i >= 0 && scheduledSources.splice(i, 1); };
        while (scheduledSources.length > 10) { try { scheduledSources.shift().disconnect(); } catch {} }
        S.audioNextTime = st + dur;
    } catch {} finally { ad.close(); }
};

export const handleAudioPkt = data => {
    if (!S.audioEnabled || !S.audioCtx) return;
    S.stats.audio++; S.stats.tAudio++;
    const v = new DataView(data), samples = v.getUint16(12, true), len = v.getUint16(14, true);
    if (len > data.byteLength - C.AUDIO_HEADER || S.audioDecoder?.state !== 'configured') return;
    try { S.audioDecoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: performance.now() * 1000, duration: (samples / C.AUDIO_RATE) * 1e6, data: new Uint8Array(data, C.AUDIO_HEADER, len) })); } catch {}
};

export const toggleAudio = () => {
    const btn = document.getElementById('aBtn'), st = document.getElementById('aSt'), stT = document.getElementById('aStT'), btnT = document.getElementById('aTxt');
    if (!S.audioEnabled) {
        initAudio().then(ok => {
            if (ok) { S.audioEnabled = S.audioPlaying = true; S.audioNextTime = 0; btn.classList.add('on'); st.classList.add('on'); btnT.textContent = 'Disable'; stT.textContent = 'Audio active'; }
            else stT.textContent = 'Audio failed';
        });
    } else { S.audioEnabled = S.audioPlaying = false; S.audioNextTime = 0; btn.classList.remove('on'); st.classList.remove('on'); btnT.textContent = 'Enable'; stT.textContent = 'Tap to enable audio'; }
};

export const closeAudio = () => S.audioCtx?.close();
