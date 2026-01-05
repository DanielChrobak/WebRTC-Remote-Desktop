import { MSG, C, S } from './state.js';

// FNV-1a hash for deduplication
const hashData = (data) => {
    let hash = 0xcbf29ce484222325n;
    const bytes = typeof data === 'string' ? new TextEncoder().encode(data) : new Uint8Array(data);
    for (const byte of bytes) {
        hash ^= BigInt(byte);
        hash = BigInt.asUintN(64, hash * 0x100000001b3n);
    }
    return hash;
};

let sendClipboardFn = null;

export const setClipboardSendFn = (fn) => { sendClipboardFn = fn; };

// Send text to host clipboard
export const sendClipboardText = (text) => {
    if (!S.clipboardEnabled || !sendClipboardFn || !text || text.length > C.MAX_CLIPBOARD_TEXT) return false;

    const hash = hashData(text);
    if (hash === S.clipboardLastSentHash) return false;
    S.clipboardLastSentHash = hash;

    const textBytes = new TextEncoder().encode(text);
    const buffer = new ArrayBuffer(8 + textBytes.length);
    const view = new DataView(buffer);
    view.setUint32(0, MSG.CLIPBOARD_TEXT, true);
    view.setUint32(4, textBytes.length, true);
    new Uint8Array(buffer, 8).set(textBytes);

    if (sendClipboardFn(buffer)) {
        S.stats.clipboard++;
        console.info(`Clipboard: sent ${textBytes.length} bytes text`);
        return true;
    }
    return false;
};

// Send image to host clipboard (as PNG)
export const sendClipboardImage = async (blob) => {
    if (!S.clipboardEnabled || !sendClipboardFn || !blob) return false;
    if (blob.size > C.MAX_CLIPBOARD_SIZE) {
        console.warn('Clipboard: image too large');
        return false;
    }

    try {
        const arrayBuffer = await blob.arrayBuffer();
        const hash = hashData(arrayBuffer);
        if (hash === S.clipboardLastSentHash) return false;
        S.clipboardLastSentHash = hash;

        // Get image dimensions
        const img = await createImageBitmap(blob);
        const width = img.width, height = img.height;
        img.close();

        const buffer = new ArrayBuffer(16 + arrayBuffer.byteLength);
        const view = new DataView(buffer);
        view.setUint32(0, MSG.CLIPBOARD_IMAGE, true);
        view.setUint32(4, width, true);
        view.setUint32(8, height, true);
        view.setUint32(12, arrayBuffer.byteLength, true);
        new Uint8Array(buffer, 16).set(new Uint8Array(arrayBuffer));

        if (sendClipboardFn(buffer)) {
            S.stats.clipboard++;
            console.info(`Clipboard: sent ${width}x${height} image (${arrayBuffer.byteLength} bytes)`);
            return true;
        }
    } catch (e) {
        console.warn('Clipboard: failed to send image', e.message);
    }
    return false;
};

// Handle incoming clipboard text from host
export const handleClipboardText = async (data) => {
    const view = new DataView(data);
    const length = view.getUint32(4, true);
    if (data.byteLength < 8 + length || length > C.MAX_CLIPBOARD_TEXT) return;

    const text = new TextDecoder().decode(new Uint8Array(data, 8, length));
    const hash = hashData(text);
    if (hash === S.clipboardLastSentHash) return; // Skip if we just sent this

    S.clipboardIgnoreNext = true;

    try {
        await navigator.clipboard.writeText(text);
        S.stats.clipboard++;
        console.info(`Clipboard: received ${length} bytes text from host`);
    } catch (e) {
        console.warn('Clipboard: failed to write text', e.message);
    }
};

// Handle incoming clipboard image from host
export const handleClipboardImage = async (data) => {
    const view = new DataView(data);
    const width = view.getUint32(4, true);
    const height = view.getUint32(8, true);
    const dataLength = view.getUint32(12, true);

    if (data.byteLength < 16 + dataLength || dataLength > C.MAX_CLIPBOARD_SIZE) return;

    const pngData = new Uint8Array(data, 16, dataLength);
    const hash = hashData(pngData);
    if (hash === S.clipboardLastSentHash) return;

    S.clipboardIgnoreNext = true;

    try {
        const blob = new Blob([pngData], { type: 'image/png' });
        const item = new ClipboardItem({ 'image/png': blob });
        await navigator.clipboard.write([item]);
        S.stats.clipboard++;
        console.info(`Clipboard: received ${width}x${height} image from host`);
    } catch (e) {
        console.warn('Clipboard: failed to write image', e.message);
    }
};

// Request current clipboard from host
export const requestClipboard = () => {
    if (!sendClipboardFn) return false;
    const buffer = new ArrayBuffer(4);
    new DataView(buffer).setUint32(0, MSG.CLIPBOARD_REQUEST, true);
    return sendClipboardFn(buffer);
};

// Read local clipboard and send to host
export const syncLocalClipboard = async () => {
    if (!S.clipboardEnabled || S.clipboardIgnoreNext) {
        S.clipboardIgnoreNext = false;
        return;
    }

    try {
        // Check for permission
        const permission = await navigator.permissions.query({ name: 'clipboard-read' });
        if (permission.state === 'denied') {
            console.warn('Clipboard: read permission denied');
            return;
        }

        // Try to read clipboard
        const items = await navigator.clipboard.read();

        for (const item of items) {
            // Check for image first (higher priority)
            if (item.types.includes('image/png')) {
                const blob = await item.getType('image/png');
                await sendClipboardImage(blob);
                return;
            }

            // Check for text
            if (item.types.includes('text/plain')) {
                const blob = await item.getType('text/plain');
                const text = await blob.text();
                if (text.trim()) {
                    sendClipboardText(text);
                    return;
                }
            }
        }
    } catch (e) {
        // Fallback to text-only read
        try {
            const text = await navigator.clipboard.readText();
            if (text && text.trim()) {
                sendClipboardText(text);
            }
        } catch (e2) {
            // Clipboard read failed (likely not focused or no permission)
        }
    }
};

// Monitor clipboard changes (when window regains focus)
let clipboardMonitorActive = false;

export const startClipboardMonitor = () => {
    if (clipboardMonitorActive) return;
    clipboardMonitorActive = true;

    // Check clipboard when window gains focus
    window.addEventListener('focus', () => {
        if (S.clipboardEnabled && S.dc?.readyState === 'open') {
            setTimeout(syncLocalClipboard, 100);
        }
    });

    // Listen for copy events
    document.addEventListener('copy', () => {
        if (S.clipboardEnabled && S.dc?.readyState === 'open') {
            setTimeout(syncLocalClipboard, 100);
        }
    });

    // Listen for cut events
    document.addEventListener('cut', () => {
        if (S.clipboardEnabled && S.dc?.readyState === 'open') {
            setTimeout(syncLocalClipboard, 100);
        }
    });

    console.info('Clipboard: monitoring started');
};

// Handle clipboard message from host
export const handleClipboardMessage = (data) => {
    if (!S.clipboardEnabled || data.byteLength < 4) return false;

    const magic = new DataView(data).getUint32(0, true);

    if (magic === MSG.CLIPBOARD_TEXT) {
        handleClipboardText(data);
        return true;
    }

    if (magic === MSG.CLIPBOARD_IMAGE) {
        handleClipboardImage(data);
        return true;
    }

    if (magic === MSG.CLIPBOARD_ACK) {
        // Host acknowledged our clipboard data
        return true;
    }

    return false;
};

export const enableClipboard = () => {
    S.clipboardEnabled = true;
    console.info('Clipboard: sync enabled');
};

export const disableClipboard = () => {
    S.clipboardEnabled = false;
    console.info('Clipboard: sync disabled');
};

export const isClipboardEnabled = () => S.clipboardEnabled;
