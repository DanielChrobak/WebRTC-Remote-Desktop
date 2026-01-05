# Remote Desktop

A low-latency WebRTC-based remote desktop solution with AV1 encoding for local network connections.

## Features

- **AV1 Hardware Encoding** - Uses NVENC, QSV, or AMF for efficient video encoding
- **WebRTC** - Low-latency peer-to-peer streaming
- **Audio Capture** - System audio streaming via Opus codec
- **Full Input Control** - Mouse, keyboard, and touch input support
- **Clipboard Sync** - Bidirectional text and image clipboard synchronization
- **Multi-Monitor** - Switch between displays on the fly

## Architecture

```
┌──────────┐                           ┌──────────┐
│   Host   │◄─────────────────────────►│  Client  │
│ (Server) │      Local Network        │ (Browser)│
│          │        WebRTC P2P         │          │
└──────────┘                           └──────────┘
```

---

## Quick Start

### 1. Build the Host Application (Windows)

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

### 2. Run the Host

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

Configuration is saved to `auth.json`.

#### Example Output
```
==========================================
         REMOTE DESKTOP SERVER
==========================================

  Local:  http://localhost:6060

  User: admin | Display: 60Hz
==========================================
```

---

### 3. Connect as Client

1. Open a browser on any device on the same network
2. Navigate to `http://<HOST_IP>:6060`
3. Enter your username and PIN

---

## Configuration Files

### `auth.json`
Created on first run. Contains authentication settings.
```json
{
  "username": "admin",
  "pin": "123456"
}
```

### `vcpkg.json`
Dependencies for the C++ host application.

---

## Web Client Files

The host serves these files on port 6060:

| File | Purpose |
|------|---------|
| `index.html` | Main UI |
| `styles.css` | Styling |
| `js/network.js` | WebRTC connection |
| `js/renderer.js` | WebGL video rendering |
| `js/input.js` | Mouse/keyboard/touch handling |
| `js/media.js` | Video/audio decoding |
| `js/clipboard.js` | Clipboard sync |
| `js/state.js` | Shared state |
| `js/ui.js` | Settings panel |

---

## Troubleshooting

### Connection Issues

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
- All media streams are encrypted via DTLS (WebRTC standard)

---

## License

BSL 1.1
