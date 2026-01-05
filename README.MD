# Remote Desktop

A low-latency WebRTC-based remote desktop solution with AV1 encoding. Supports both local network connections and port-forwarding-free remote access via a Cloudflare Worker signaling server.

## Features

- **AV1 Hardware Encoding** - Uses NVENC, QSV, or AMF for efficient video encoding
- **WebRTC** - Low-latency peer-to-peer streaming with TURN relay fallback
- **No Port Forwarding Required** - Remote mode uses a Cloudflare Worker for signaling
- **Audio Capture** - System audio streaming via Opus codec
- **Full Input Control** - Mouse, keyboard, and touch input support
- **Clipboard Sync** - Bidirectional text and image clipboard synchronization
- **Multi-Monitor** - Switch between displays on the fly

## Architecture

```
┌──────────┐         ┌──────────────┐         ┌──────────┐
│   Host   │────────►│  Cloudflare  │◄────────│  Client  │
│ (no open │ outbound│   Worker     │ outbound│ (Browser)│
│  ports)  │  HTTPS  │ (signaling)  │  HTTPS  │          │
└────┬─────┘         └──────────────┘         └────┬─────┘
     │                                             │
     │         ┌────────────────────┐              │
     └────────►│    WebRTC P2P      │◄─────────────┘
               │  (or TURN relay)   │
               └────────────────────┘
```

Both host and client only make **outbound connections** - neither requires open ports.

---

## Quick Start

### 1. Set Up Signaling Server (For Remote Access)

Skip this step if you only need local network access.

#### Create Cloudflare Account
1. Go to [dash.cloudflare.com/sign-up](https://dash.cloudflare.com/sign-up)
2. Verify your email (no credit card required)

#### Install Wrangler CLI
```bash
npm install -g wrangler
wrangler login
```

#### Create KV Namespace
```bash
wrangler kv:namespace create ROOMS
```

This outputs something like:
```
{ binding = "ROOMS", id = "abc123..." }
```

#### Update `wrangler.toml`
Replace the KV namespace ID with your own:
```toml
name = "remote-desktop-signaling"
main = "src/index.js"
compatibility_date = "2024-01-01"

[[kv_namespaces]]
binding = "ROOMS"
id = "YOUR_KV_ID_HERE"
```

#### Deploy
```bash
wrangler deploy
```

You'll get a URL like: `https://remote-desktop-signaling.YOUR_USERNAME.workers.dev`

---

### 2. Get TURN Server Credentials (Recommended)

TURN servers relay traffic when direct P2P connections fail (firewalls, symmetric NAT, etc.).

#### Option A: Metered.ca (Recommended)
1. Sign up at [metered.ca](https://www.metered.ca/)
2. Create an application
3. Copy your API key and domain
4. Update `turn_config.json`:

```json
{
  "metered": {
    "enabled": true,
    "apiKey": "YOUR_API_KEY",
    "domain": "YOUR_APP.metered.live",
    "fetchUrl": "https://YOUR_APP.metered.live/api/v1/turn/credentials?apiKey=YOUR_API_KEY"
  },
  "manual": {
    "enabled": false
  },
  "fallback": {
    "enabled": true,
    "servers": [
      { "urls": "stun:stun.l.google.com:19302" },
      { "urls": "stun:stun1.l.google.com:19302" }
    ]
  }
}
```

#### Option B: Self-Hosted or Other Provider
```json
{
  "metered": {
    "enabled": false
  },
  "manual": {
    "enabled": true,
    "credentials": {
      "username": "YOUR_USERNAME",
      "password": "YOUR_PASSWORD"
    },
    "servers": [
      { "urls": "stun:your-server.com:3478" },
      { "urls": "turn:your-server.com:3478", "username": "", "credential": "" },
      { "urls": "turns:your-server.com:443?transport=tcp", "username": "", "credential": "" }
    ]
  },
  "fallback": {
    "enabled": true,
    "servers": [
      { "urls": "stun:stun.l.google.com:19302" }
    ]
  }
}
```

---

### 3. Build the Host Application (Windows)

#### Prerequisites
- Windows 10/11 (64-bit)
- Visual Studio 2022 with C++ workload
- [vcpkg](https://github.com/microsoft/vcpkg) package manager

#### Install vcpkg
```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install
```

#### Build
```batch
build.bat
```

This will:
1. Install dependencies via vcpkg (libdatachannel, ffmpeg, opus, etc.)
2. Configure with CMake
3. Build the Release executable

Output: `build\bin\Release\ScreenShare.exe`

---

### 4. Run the Host

```batch
run.bat
```

Or run directly (requires Administrator for input injection):
```batch
cd build\bin\Release
ScreenShare.exe
```

#### First-Time Setup
On first run, you'll be prompted to configure:

1. **Username** (3-32 alphanumeric characters)
2. **PIN** (6 digits) - Used for client authentication
3. **Signaling Server URL** (optional) - Your Cloudflare Worker URL for remote access
4. **Host ID** (optional) - 6-character ID (e.g., `ABC123`) for clients to connect

Configuration is saved to `auth.json`.

#### Example Output
```
==========================================
         REMOTE DESKTOP SERVER
==========================================

  Local:  http://localhost:6060
  Remote: Host ID: ABC123
          Server:  https://your-worker.workers.dev

  User: admin | Display: 60Hz
==========================================
```

---

### 5. Connect as Client

#### Local Network
1. Open a browser on any device on the same network
2. Navigate to `http://<HOST_IP>:6060`
3. Enter your username and PIN

#### Remote Access
1. Open the web client (can be hosted anywhere, or use the local server)
2. Click the "Remote" tab
3. Enter:
   - **Signaling Server**: Your Cloudflare Worker URL
   - **Host ID**: The 6-character code shown on the host
4. Click "Connect"
5. Enter your username and PIN

---

## Configuration Files

### `auth.json`
Created on first run. Contains authentication and remote access settings.
```json
{
  "username": "admin",
  "pin": "123456",
  "signalingUrl": "https://your-worker.workers.dev",
  "hostId": "ABC123"
}
```

### `turn_config.json`
TURN/STUN server configuration for WebRTC connectivity.

### `vcpkg.json`
Dependencies for the C++ host application.

---

## Cloudflare Workers Free Tier Usage

The signaling server is designed to minimize Cloudflare Workers usage:

| Operation | Frequency | Daily Estimate | Type |
|-----------|-----------|----------------|------|
| Host poll | 1/second | ~86,400 | READ |
| Host answer | per connection | ~10 | WRITE |
| Client offer | per connection | ~10 | WRITE |
| Client poll | ~15/connection | ~150 | READ |

**Total**: ~86,500 reads (86.5% of free tier), ~60 writes (6% of free tier)

No heartbeats - host offline detection uses a simple 15-second timeout.

---

## Web Client Files

The host serves these files on port 6060:

| File | Purpose |
|------|---------|
| `index.html` | Main UI |
| `styles.css` | Styling |
| `js/network.js` | WebRTC & signaling |
| `js/renderer.js` | WebGL video rendering |
| `js/input.js` | Mouse/keyboard/touch handling |
| `js/media.js` | Video/audio decoding |
| `js/clipboard.js` | Clipboard sync |
| `js/state.js` | Shared state |
| `js/ui.js` | Settings panel |

---

## Troubleshooting

### Connection Issues

**"Host not responding"**
- Verify the Host ID is correct
- Check that the signaling server URL is accessible
- Ensure the host application is running

**Falls back to TURN relay**
- This is normal when P2P fails (corporate firewalls, symmetric NAT)
- Verify your TURN credentials are correct
- Check the Stats overlay (Settings > Debug > Stats Overlay)

**No video after connecting**
- Check if authentication succeeded
- The host console shows `[LIVE]` when streaming
- Try refreshing the page

### Build Issues

**vcpkg not found**
- Set `VCPKG_ROOT` environment variable, or
- Install vcpkg to `C:\vcpkg`

**FFmpeg/libdatachannel errors**
- Run `vcpkg install --triplet x64-windows` manually
- Check that Visual Studio 2022 is installed with C++ workload

---

## Security Notes

- Always use a strong 6-digit PIN
- The signaling server only exchanges connection metadata, not video/audio
- All media streams are encrypted via DTLS (WebRTC standard)
- Consider restricting CORS on your Cloudflare Worker for production use

---

## License

MIT
