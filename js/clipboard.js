import { MSG, C, S, mkBuf } from './state.js';

const hash = d => { let h = 0xcbf29ce484222325n; for (const b of new Uint8Array(typeof d === 'string' ? new TextEncoder().encode(d) : d)) h = BigInt.asUintN(64, (h ^ BigInt(b)) * 0x100000001b3n); return h; };

let sendFn = null, pending = null, ignoreUntil = 0;

export const setClipboardSendFn = fn => sendFn = fn;

export const sendClipboardText = text => {
    if (!S.clipboardEnabled || !sendFn || !text || text.length > C.MAX_CLIPBOARD_TEXT) return false;
    const h = hash(text); if (h === S.clipboardLastSentHash) return false;
    S.clipboardLastSentHash = h;
    const tb = new TextEncoder().encode(text), buf = mkBuf(8 + tb.length, v => { v.setUint32(0, MSG.CLIPBOARD_TEXT, true); v.setUint32(4, tb.length, true); });
    new Uint8Array(buf, 8).set(tb);
    return sendFn(buf) ? (S.stats.clipboard++, console.info(`Clipboard: sent ${tb.length}b text`), true) : false;
};

export const sendClipboardImage = async blob => {
    if (!S.clipboardEnabled || !sendFn || !blob || blob.size > C.MAX_CLIPBOARD_SIZE) return false;
    try {
        const ab = await blob.arrayBuffer(), h = hash(ab); if (h === S.clipboardLastSentHash) return false;
        S.clipboardLastSentHash = h;
        const img = await createImageBitmap(blob), { width: w, height: ht } = img; img.close();
        const buf = mkBuf(16 + ab.byteLength, v => { v.setUint32(0, MSG.CLIPBOARD_IMAGE, true); v.setUint32(4, w, true); v.setUint32(8, ht, true); v.setUint32(12, ab.byteLength, true); });
        new Uint8Array(buf, 16).set(new Uint8Array(ab));
        return sendFn(buf) ? (S.stats.clipboard++, console.info(`Clipboard: sent ${w}x${ht} image`), true) : false;
    } catch (e) { console.warn('Clipboard: send image failed', e.message); return false; }
};

const write = async (text, image) => {
    ignoreUntil = performance.now() + 500;
    try {
        if (text) { await navigator.clipboard.writeText(text); S.stats.clipboard++; console.info(`Clipboard: wrote ${text.length}b text`); }
        else if (image) { await navigator.clipboard.write([new ClipboardItem({ 'image/png': new Blob([image.png], { type: 'image/png' }) })]); S.stats.clipboard++; console.info(`Clipboard: wrote ${image.w}x${image.h} image`); }
    } catch (e) { document.hasFocus() && console.warn('Clipboard: write failed', e.message); }
};

const handleText = async data => {
    const v = new DataView(data), len = v.getUint32(4, true);
    if (data.byteLength < 8 + len || len > C.MAX_CLIPBOARD_TEXT) return;
    const text = new TextDecoder().decode(new Uint8Array(data, 8, len));
    if (hash(text) === S.clipboardLastSentHash) return;
    document.hasFocus() ? write(text, null) : pending = { text };
};

const handleImage = async data => {
    const v = new DataView(data), w = v.getUint32(4, true), h = v.getUint32(8, true), len = v.getUint32(12, true);
    if (data.byteLength < 16 + len || len > C.MAX_CLIPBOARD_SIZE) return;
    const png = new Uint8Array(data, 16, len);
    if (hash(png) === S.clipboardLastSentHash) return;
    document.hasFocus() ? write(null, { png, w, h }) : pending = { png: png.slice(), w, h };
};

export const requestClipboard = () => sendFn?.(mkBuf(4, v => v.setUint32(0, MSG.CLIPBOARD_REQUEST, true)));

export const syncLocalClipboard = async () => {
    if (!S.clipboardEnabled || performance.now() < ignoreUntil) return;
    try {
        if ((await navigator.permissions.query({ name: 'clipboard-read' })).state === 'denied') return;
        for (const item of await navigator.clipboard.read()) {
            if (item.types.includes('image/png')) return sendClipboardImage(await item.getType('image/png'));
            if (item.types.includes('text/plain')) { const t = await (await item.getType('text/plain')).text(); t?.trim() && sendClipboardText(t); return; }
        }
    } catch { try { const t = await navigator.clipboard.readText(); t?.trim() && sendClipboardText(t); } catch {} }
};

let active = false;
export const startClipboardMonitor = () => {
    if (active) return; active = true;
    window.addEventListener('focus', () => {
        if (!S.clipboardEnabled || S.dc?.readyState !== 'open') return;
        (pending?.text ? write(pending.text, null) : pending?.png ? write(null, pending) : Promise.resolve()).then(() => { pending = null; setTimeout(syncLocalClipboard, 100); });
    });
    ['copy', 'cut'].forEach(e => document.addEventListener(e, () => S.clipboardEnabled && S.dc?.readyState === 'open' && setTimeout(syncLocalClipboard, 100)));
    console.info('Clipboard: monitoring started');
};

export const handleClipboardMessage = data => {
    if (!S.clipboardEnabled || data.byteLength < 4) return false;
    const m = new DataView(data).getUint32(0, true);
    if (m === MSG.CLIPBOARD_TEXT) { handleText(data); return true; }
    if (m === MSG.CLIPBOARD_IMAGE) { handleImage(data); return true; }
    return m === MSG.CLIPBOARD_ACK;
};

export const enableClipboard = () => { S.clipboardEnabled = true; console.info('Clipboard: enabled'); };
export const disableClipboard = () => { S.clipboardEnabled = false; console.info('Clipboard: disabled'); };
export const isClipboardEnabled = () => S.clipboardEnabled;
