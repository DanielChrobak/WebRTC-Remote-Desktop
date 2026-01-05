import { MSG, C, S } from './state.js';

const hashData = data => {
    let hash = 0xcbf29ce484222325n;
    const bytes = typeof data === 'string' ? new TextEncoder().encode(data) : new Uint8Array(data);
    for (const byte of bytes) hash = BigInt.asUintN(64, (hash ^ BigInt(byte)) * 0x100000001b3n);
    return hash;
};

let sendFn = null, pendingText = null, pendingImage = null;

export const setClipboardSendFn = fn => { sendFn = fn; };

const mkBuf = (size, fn) => { const b = new ArrayBuffer(size), v = new DataView(b); fn(v); return b; };

export const sendClipboardText = text => {
    if (!S.clipboardEnabled || !sendFn || !text || text.length > C.MAX_CLIPBOARD_TEXT) return false;
    const hash = hashData(text);
    if (hash === S.clipboardLastSentHash) return false;
    S.clipboardLastSentHash = hash;
    const textBytes = new TextEncoder().encode(text);
    const buf = mkBuf(8 + textBytes.length, v => { v.setUint32(0, MSG.CLIPBOARD_TEXT, true); v.setUint32(4, textBytes.length, true); });
    new Uint8Array(buf, 8).set(textBytes);
    if (sendFn(buf)) { S.stats.clipboard++; console.info(`Clipboard: sent ${textBytes.length} bytes text`); return true; }
    return false;
};

export const sendClipboardImage = async blob => {
    if (!S.clipboardEnabled || !sendFn || !blob || blob.size > C.MAX_CLIPBOARD_SIZE) return false;
    try {
        const arrayBuffer = await blob.arrayBuffer();
        const hash = hashData(arrayBuffer);
        if (hash === S.clipboardLastSentHash) return false;
        S.clipboardLastSentHash = hash;
        const img = await createImageBitmap(blob);
        const { width, height } = img; img.close();
        const buf = mkBuf(16 + arrayBuffer.byteLength, v => {
            v.setUint32(0, MSG.CLIPBOARD_IMAGE, true); v.setUint32(4, width, true);
            v.setUint32(8, height, true); v.setUint32(12, arrayBuffer.byteLength, true);
        });
        new Uint8Array(buf, 16).set(new Uint8Array(arrayBuffer));
        if (sendFn(buf)) { S.stats.clipboard++; console.info(`Clipboard: sent ${width}x${height} image`); return true; }
    } catch (e) { console.warn('Clipboard: failed to send image', e.message); }
    return false;
};

const writeClipboard = async (text, image) => {
    S.clipboardIgnoreNext = true;
    try {
        if (text) { await navigator.clipboard.writeText(text); S.stats.clipboard++; console.info(`Clipboard: wrote ${text.length} bytes text`); }
        else if (image) {
            const blob = new Blob([image.pngData], { type: 'image/png' });
            await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]);
            S.stats.clipboard++; console.info(`Clipboard: wrote ${image.width}x${image.height} image`);
        }
    } catch (e) { document.hasFocus() && console.warn('Clipboard: write failed', e.message); }
};

export const handleClipboardText = async data => {
    const view = new DataView(data), length = view.getUint32(4, true);
    if (data.byteLength < 8 + length || length > C.MAX_CLIPBOARD_TEXT) return;
    const text = new TextDecoder().decode(new Uint8Array(data, 8, length));
    if (hashData(text) === S.clipboardLastSentHash) return;
    if (!document.hasFocus()) { pendingText = text; pendingImage = null; return; }
    await writeClipboard(text, null);
};

export const handleClipboardImage = async data => {
    const view = new DataView(data);
    const width = view.getUint32(4, true), height = view.getUint32(8, true), dataLength = view.getUint32(12, true);
    if (data.byteLength < 16 + dataLength || dataLength > C.MAX_CLIPBOARD_SIZE) return;
    const pngData = new Uint8Array(data, 16, dataLength);
    if (hashData(pngData) === S.clipboardLastSentHash) return;
    if (!document.hasFocus()) { pendingImage = { pngData: pngData.slice(), width, height }; pendingText = null; return; }
    await writeClipboard(null, { pngData, width, height });
};

const writePendingClipboard = async () => {
    if (pendingText) { const t = pendingText; pendingText = null; await writeClipboard(t, null); }
    else if (pendingImage) { const i = pendingImage; pendingImage = null; await writeClipboard(null, i); }
};

export const requestClipboard = () => sendFn && sendFn(mkBuf(4, v => v.setUint32(0, MSG.CLIPBOARD_REQUEST, true)));

export const syncLocalClipboard = async () => {
    if (!S.clipboardEnabled || S.clipboardIgnoreNext) { S.clipboardIgnoreNext = false; return; }
    try {
        const perm = await navigator.permissions.query({ name: 'clipboard-read' });
        if (perm.state === 'denied') return;
        const items = await navigator.clipboard.read();
        for (const item of items) {
            if (item.types.includes('image/png')) return sendClipboardImage(await item.getType('image/png'));
            if (item.types.includes('text/plain')) {
                const text = await (await item.getType('text/plain')).text();
                if (text.trim()) return sendClipboardText(text);
            }
        }
    } catch {
        try { const text = await navigator.clipboard.readText(); text?.trim() && sendClipboardText(text); } catch {}
    }
};

let clipboardMonitorActive = false;
export const startClipboardMonitor = () => {
    if (clipboardMonitorActive) return;
    clipboardMonitorActive = true;
    window.addEventListener('focus', () => {
        if (S.clipboardEnabled && S.dc?.readyState === 'open')
            writePendingClipboard().then(() => setTimeout(syncLocalClipboard, 100));
    });
    ['copy', 'cut'].forEach(e => document.addEventListener(e, () => {
        if (S.clipboardEnabled && S.dc?.readyState === 'open') setTimeout(syncLocalClipboard, 100);
    }));
    console.info('Clipboard: monitoring started');
};

export const handleClipboardMessage = data => {
    if (!S.clipboardEnabled || data.byteLength < 4) return false;
    const magic = new DataView(data).getUint32(0, true);
    if (magic === MSG.CLIPBOARD_TEXT) { handleClipboardText(data); return true; }
    if (magic === MSG.CLIPBOARD_IMAGE) { handleClipboardImage(data); return true; }
    return magic === MSG.CLIPBOARD_ACK;
};

export const enableClipboard = () => { S.clipboardEnabled = true; console.info('Clipboard: sync enabled'); };
export const disableClipboard = () => { S.clipboardEnabled = false; console.info('Clipboard: sync disabled'); };
export const isClipboardEnabled = () => S.clipboardEnabled;
