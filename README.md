<p align="left">
  <img src="assets/v4l2-passthru.svg" width="72" alt="V4L2 Passthru icon" />
</p>

# V4L2 Passthru

**V4L2 Passthru** is a low-latency C++20 / Qt6 / QML desktop passthrough viewer for USB V4L2/UVC video capture devices and their accompanying USB audio inputs on Linux.

It lets you play or monitor external video sources directly on your desktop with minimal buffering while maintaining a **persistent Wayland/X11 window** and a **named PipeWire/PulseAudio audio stream**, making it easy to share via screen-sharing apps or capture cleanly in broadcasting tools.

## Example Screenshot

![V4L2 Passthru live window with auto-hiding HUD](assets/screenshot.png)

*(Shown above over a standard SMPTE test signal with the floating control bar visible.)*

## What It Does

* **Low-latency video passthrough**: Captures via GStreamer `v4l2src` (`io-mode=mmap`) into a single-frame `appsink` (`max-buffers=1`, `drop=true`, `sync=false`) and uploads directly to Qt6's GPU-accelerated `VideoOutput` (`Format_YUYV` for raw `YUYV 4:2:2`, or `jpegdec` → `Format_YUV420P` for `MJPG`).
* **Low-latency audio passthrough**: Routes `48 kHz` stereo USB capture audio through PipeWire/PulseAudio (`10 ms` latency-time / `25 ms` buffer-time) under the client/stream name **`V4L2 Passthru`**, automatically unmuting hardware ALSA capture switches (such as `Digital In` on MacroSilicon MS213x cards) and providing live mute & `0–150%` volume control.
* **Watchdog & automatic USB recovery**: Monitors incoming frame timestamps (`> 1.2s` stall threshold) and GStreamer bus errors. On a stall or USB disconnect/reset, it immediately closes `/dev/video*` so the kernel can re-enumerate cleanly, re-discovers the device via `libudev` + `VIDIOC_QUERYCAP`, and reconnects with backoff—**without closing or recreating the host window**.
* **Clean viewing & sharing**: The top HUD bar and cursor auto-hide after 2.5 seconds of inactivity during playback so viewers only see the video feed.

---

## How to Use

1. Plug in your USB capture device and launch `v4l2-passthru`.
2. By default (`Auto`), the app selects your USB capture device (prioritizing MacroSilicon `VC-003` / `345f:2131` if present) and starts streaming `1080p60` `YUYV` video + audio passthrough.
3. Move your mouse to reveal the floating top HUD bar:
   * **Format (`YUYV` / `MJPG`)**: `YUYV` gives zero decode latency and crisp uncompressed 4:2:2 color; switch to `MJPG` to reduce USB bandwidth if your port/cable drops frames.
   * **FPS (`60` / `30`)**: Switch capture frame rate on the fly.
   * **Mute / Volume slider**: Live-adjust passthrough audio without restarting video.
   * **Reconnect**: Cleanly tear down and reopen video & audio pipelines.
   * **Setup**: Select specific V4L2 video devices or PipeWire/PulseAudio audio inputs, toggle audio passthrough or auto-reconnect, and view live audio status and recovery counts.
4. **Screen-sharing / Broadcasting**:
   * Select the **`V4L2 Passthru`** window in PipeWire/Wayland Window Capture and capture the **`V4L2 Passthru`** PipeWire audio stream (or Desktop Audio).
   * Avoid keeping another V4L2 application open on the same `/dev/video*` node simultaneously, as Linux V4L2 allows only one active capture consumer at a time.

### Keyboard Shortcuts

| Shortcut | Action |
| --- | --- |
| `F11` or Double-click | Toggle fullscreen (`Esc` exits fullscreen) |
| `M` | Mute / unmute audio passthrough |
| `R` | Reconnect capture pipelines |
| `S` | Toggle device & recovery Setup drawer |

---

## Installation

### Option 1: Arch Linux Prebuilt Package (`pacman`)

Prebuilt `.pkg.tar.zst` packages are published on the **[GitHub Releases](../../releases)** page whenever a version tag (`v*.*.*`) is pushed:

```bash
# Download the package and checksum from the latest GitHub Release, then verify:
sha256sum -c v4l2-passthru-*-x86_64.pkg.tar.zst.sha256

# Install with pacman (automatically installs Qt6, GStreamer, PipeWire, and ALSA deps):
sudo pacman -U ./v4l2-passthru-*-x86_64.pkg.tar.zst
```

### Option 2: Build an Arch Package Locally (`makepkg`)

```bash
git clone https://github.com/covarianttensor/v4l2-passthru.git
cd v4l2-passthru
makepkg -si
```

### Option 3: Build from Source (CMake)

Install dependencies on Arch Linux:

```bash
sudo pacman -S --needed \
  base-devel cmake pkgconf \
  qt6-base qt6-declarative qt6-multimedia qt6-multimedia-ffmpeg qt6-svg \
  gstreamer gst-plugins-base-libs gst-plugins-good gst-plugin-pipewire \
  alsa-lib systemd-libs hicolor-icon-theme
```

Configure, build, and install:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

Or run directly from the build directory without installing:

```bash
./build/v4l2-passthru
```

Headless smoke test (verifies device discovery, QML loading, and live frame rate):

```bash
QT_QPA_PLATFORM=offscreen ./build/v4l2-passthru --smoke-test 5
```

---

## Releasing (GitHub Actions)

The workflow in [`.github/workflows/release.yml`](.github/workflows/release.yml) runs **only when a semantic version tag is pushed** (e.g., `v0.1.0`):

```bash
git tag -a v0.1.0 -m "Release v0.1.0"
git push origin v0.1.0
```

It builds the Arch `pacman` `.pkg.tar.zst` package inside `archlinux:latest`, generates a `.sha256` file, and creates a GitHub Release with install instructions and attached binaries.

## GitHub Topics (Discoverability)

Recommended GitHub repository topics:

`v4l2` `uvc` `capture-card` `video-capture` `passthrough` `qt6` `qml` `cpp20` `gstreamer` `pipewire` `wayland` `linux` `archlinux` `low-latency`

## License

This project is licensed under the **GNU General Public License v3.0 (GPLv3)** — see [`LICENSE`](LICENSE) for the full text.
