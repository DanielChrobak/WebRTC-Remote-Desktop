# WebRTC Remote Desktop

A high-performance, low-latency remote desktop solution using WebRTC for real-time streaming with AV1 hardware encoding.

## Overview

WebRTC Remote Desktop enables secure, low-latency screen sharing over local networks using modern web technologies. The server captures the host display using the Windows Graphics Capture API and encodes it with AV1 hardware acceleration (NVENC, QSV, or AMF). Clients connect via any modern web browser without plugins.

## Features

- **AV1 Hardware Encoding** - Leverages GPU encoders (NVIDIA NVENC, Intel QSV, AMD AMF) with software fallback
- **Ultra-Low Latency** - Optimized pipeline for sub-50ms end-to-end latency on local networks
- **WebRTC Transport** - Encrypted peer-to-peer streaming with automatic ICE negotiation
- **System Audio** - Captures and streams system audio via Opus codec
- **Full Input Control** - Complete mouse, keyboard, and touch input support
- **Multi-Monitor** - Dynamic monitor switching during active sessions
- **Touch Gestures** - Trackpad and direct touch modes with pinch-to-zoom
- **Authentication** - PIN-based authentication with rate limiting

## System Requirements

### Host (Server)

- Windows 10/11 (64-bit)
- Visual Studio 2022 with C++ Desktop workload
- DirectX 11 compatible GPU
- [vcpkg](https://github.com/microsoft/vcpkg) package manager

### Client

- Modern web browser with WebRTC support (Chrome, Firefox, Edge, Safari)
- Hardware video decoding recommended for best performance

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                           HOST (Server)                             │
├─────────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────────────────┐ │
│  │   Screen    │───▶│     AV1     │───▶│       WebRTC            │ │
│  │   Capture   │    │   Encoder   │    │   Data Channel          │ │
│  └─────────────┘    └─────────────┘    └───────────┬─────────────┘ │
│  ┌─────────────┐    ┌─────────────┐                │               │
│  │    Audio    │───▶│    Opus     │────────────────┤               │
│  │   Capture   │    │   Encoder   │                │               │
│  └─────────────┘    └─────────────┘                │               │
│  ┌─────────────┐                                   │               │
│  │    Input    │◀──────────────────────────────────┘               │
│  │   Handler   │                                                   │
│  └─────────────┘                                                   │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  │ DTLS/SRTP Encrypted
                                  │ Local Network
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         CLIENT (Browser)                            │
├─────────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────────────────┐ │
│  │   WebRTC    │───▶│    AV1      │───▶│       WebGL2            │ │
│  │   Receive   │    │   Decoder   │    │       Renderer          │ │
│  └─────────────┘    └─────────────┘    └─────────────────────────┘ │
│  ┌─────────────┐    ┌─────────────┐                                │
│  │    Audio    │◀───│    Opus     │                                │
│  │   Playback  │    │   Decoder   │                                │
│  └─────────────┘    └─────────────┘                                │
│  ┌─────────────┐                                                   │
│  │    Input    │───────────────────────────────────────────────────│
│  │   Capture   │                                                   │
│  └─────────────┘                                                   │
└─────────────────────────────────────────────────────────────────────┘
```

## Quick Start

### 1. Install Prerequisites

```powershell
# Install vcpkg
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install
```

### 2. Build the Server

```batch
build.bat
```

The build process will:
1. Install dependencies via vcpkg (libdatachannel, FFmpeg, Opus, etc.)
2. Configure the project with CMake
3. Build the Release executable

Output: `build\bin\Release\WebRTCRemoteDesktop.exe`

### 3. Run the Server

```batch
run.bat
```

On first run, you'll configure:
- **Username** (3-32 alphanumeric characters)
- **PIN** (6 digits)

Configuration is saved to `auth.json`.

### 4. Connect from Client

1. Open a web browser on any device on the same network
2. Navigate to `http://<HOST_IP>:6060`
3. Enter the server URL if not auto-detected
4. Authenticate with username and PIN

## Configuration

### auth.json

```json
{
  "username": "admin",
  "pin": "123456"
}
```

### vcpkg.json

Dependencies are automatically managed by vcpkg:
- libdatachannel (WebRTC)
- cpp-httplib (HTTP server)
- nlohmann-json (JSON parsing)
- opus (Audio codec)
- openssl (TLS/DTLS)
- ffmpeg (AV1 encoding)

## Web Client

The server hosts these files on port 6060:

| File | Description |
|------|-------------|
| `index.html` | Main application shell |
| `styles.css` | UI styling |
| `js/network.js` | WebRTC connection management |
| `js/renderer.js` | WebGL2 video rendering |
| `js/input.js` | Mouse, keyboard, touch handling |
| `js/media.js` | Video and audio decoding |
| `js/state.js` | Application state management |
| `js/ui.js` | Settings panel and controls |

## Controls

### Desktop (Mouse + Keyboard)

- **Click canvas** - Capture mouse (pointer lock)
- **ESC** - Release mouse capture
- **All keys** - Passed through to host

### Touch Devices

#### Trackpad Mode (Default)
- **One finger drag** - Move cursor
- **Tap** - Left click
- **Long press + drag** - Drag operation
- **Two finger tap** - Right click
- **Two finger drag** - Scroll
- **Pinch** - Zoom view

#### Direct Mode
- **Touch** - Move cursor to touch position
- **Tap** - Left click
- **Long press + drag** - Drag operation

## Performance Tuning

### Encoder Selection Priority

1. **NVIDIA NVENC** - Lowest latency, best quality
2. **Intel QSV** - Good latency, integrated GPU friendly
3. **AMD AMF** - Good performance on AMD GPUs
4. **libsvtav1** - Fast software fallback
5. **libaom-av1** - Universal software fallback

### Optimizing Latency

- Use wired network connections when possible
- Select frame rate matching host display refresh rate
- Ensure GPU hardware encoding is active
- Client browser should use hardware video decoding

## Troubleshooting

### Connection Issues

**Cannot connect to server**
- Verify server is running (console shows `[LIVE]` when streaming)
- Check firewall allows port 6060 (TCP) and 50000-50100 (UDP)
- Ensure devices are on the same network

**Video not displaying**
- Check authentication succeeded
- Verify browser supports AV1 decoding
- Try refreshing the page

### Build Issues

**vcpkg not found**
- Set `VCPKG_ROOT` environment variable
- Or install vcpkg to `C:\vcpkg`

**Encoder initialization failed**
- Update GPU drivers
- Verify DirectX 11 support
- Check FFmpeg codec availability

### Performance Issues

**High latency**
- Check network connection quality
- Reduce frame rate in settings
- Verify hardware encoding is active (check console output)

**Choppy video**
- Lower frame rate
- Check client CPU/GPU usage
- Ensure hardware video decoding is enabled in browser

## Security

- All WebRTC streams are encrypted with DTLS
- PIN-based authentication with rate limiting (3 attempts)
- Sessions are isolated per connection
- Designed for trusted local networks

## Building Installer

```batch
build_installer.bat
```

Creates either:
- NSIS installer (.exe) if NSIS is installed
- Portable ZIP package otherwise

## License

Business Source License (Personal-Online / No-Company) v1.1

See [LICENSE.txt](LICENSE.txt) for full terms.

## Copyright

© 2025-2026 Daniel Chrobak
