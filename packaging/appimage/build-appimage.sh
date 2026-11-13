#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="${1:-0.1.1}"
OUT_DIR="${2:-${ROOT_DIR}/dist-appimage}"
WORK_DIR="${ROOT_DIR}/build-appimage"
APPDIR="${WORK_DIR}/AppDir"
TOOLS_DIR="${WORK_DIR}/tools"

rm -rf "${WORK_DIR}"
mkdir -p "${APPDIR}" "${TOOLS_DIR}" "${OUT_DIR}"

export APPIMAGE_EXTRACT_AND_RUN=1

# 1. Build and stage into AppDir
cmake -S "${ROOT_DIR}" -B "${WORK_DIR}/cmake-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "${WORK_DIR}/cmake-build" -j"$(nproc)"
DESTDIR="${APPDIR}" cmake --install "${WORK_DIR}/cmake-build"

# 2. Fetch linuxdeploy + Qt plugin
LINUXDEPLOY="${TOOLS_DIR}/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT="${TOOLS_DIR}/linuxdeploy-plugin-qt-x86_64.AppImage"

curl -fsSL -o "${LINUXDEPLOY}" \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
curl -fsSL -o "${LINUXDEPLOY_QT}" \
  "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
chmod +x "${LINUXDEPLOY}" "${LINUXDEPLOY_QT}"

# 3. Bundle Qt6, QML, and core binary dependencies
export QML_SOURCES_PATHS="${ROOT_DIR}/qml"
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so;libqxcb.so"
export EXTRA_QT_PLUGINS="iconengines;imageformats;multimedia;wayland-shell-integration;wayland-graphics-integration-client;wayland-decoration-client"

"${LINUXDEPLOY}" \
  --appdir "${APPDIR}" \
  --executable "${APPDIR}/usr/bin/v4l2-passthru" \
  --desktop-file "${APPDIR}/usr/share/applications/v4l2-passthru.desktop" \
  --icon-file "${APPDIR}/usr/share/icons/hicolor/scalable/apps/v4l2-passthru.svg" \
  --plugin qt

# 4. Bundle required GStreamer plugins and scanner
GST_PLUGIN_DIR="$(pkg-config --variable=pluginsdir gstreamer-1.0)"
GST_EXEC_DIR="$(pkg-config --variable=libexecdir gstreamer-1.0 2>/dev/null || echo /usr/libexec)"
GST_SCANNER="$(find "${GST_EXEC_DIR}" /usr/lib* -name 'gst-plugin-scanner' -type f 2>/dev/null | head -n 1 || true)"

mkdir -p "${APPDIR}/usr/lib/gstreamer-1.0" "${APPDIR}/usr/libexec/gstreamer-1.0"

REQUIRED_GST_PLUGINS=(
  libgstcoreelements.so
  libgstapp.so
  libgstvideo4linux2.so
  libgstjpeg.so
  libgstaudioconvert.so
  libgstaudioresample.so
  libgstvolume.so
  libgstpulseaudio.so
  libgstpipewire.so
)

for plugin in "${REQUIRED_GST_PLUGINS[@]}"; do
  if [[ -f "${GST_PLUGIN_DIR}/${plugin}" ]]; then
    cp -L "${GST_PLUGIN_DIR}/${plugin}" "${APPDIR}/usr/lib/gstreamer-1.0/"
  else
    echo "Warning: GStreamer plugin not found: ${GST_PLUGIN_DIR}/${plugin}" >&2
  fi
done

if [[ -n "${GST_SCANNER}" && -f "${GST_SCANNER}" ]]; then
  cp -L "${GST_SCANNER}" "${APPDIR}/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
  chmod +x "${APPDIR}/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
  "${LINUXDEPLOY}" --appdir "${APPDIR}" --deploy-deps-only "${APPDIR}/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
fi

"${LINUXDEPLOY}" --appdir "${APPDIR}" --deploy-deps-only "${APPDIR}/usr/lib/gstreamer-1.0"

# 5. Add AppRun hook so GStreamer uses bundled plugins and an isolated registry cache
mkdir -p "${APPDIR}/apprun-hooks"
cat > "${APPDIR}/apprun-hooks/gstreamer-env.sh" <<'EOF'
#!/bin/sh
export GST_PLUGIN_SYSTEM_PATH_1_0="$APPDIR/usr/lib/gstreamer-1.0"
export GST_PLUGIN_PATH_1_0="$APPDIR/usr/lib/gstreamer-1.0"
if [ -x "$APPDIR/usr/libexec/gstreamer-1.0/gst-plugin-scanner" ]; then
  export GST_PLUGIN_SCANNER_1_0="$APPDIR/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
fi
export GST_REGISTRY_1_0="${XDG_CACHE_HOME:-$HOME/.cache}/v4l2-passthru-gst-registry.bin"
EOF
chmod +x "${APPDIR}/apprun-hooks/gstreamer-env.sh"

# 6. Produce final AppImage
export LDAI_OUTPUT="${OUT_DIR}/v4l2-passthru-${VERSION}-x86_64.AppImage"
export OUTPUT="${LDAI_OUTPUT}"
export VERSION="${VERSION}"
(cd "${OUT_DIR}" && "${LINUXDEPLOY}" --appdir "${APPDIR}" --output appimage)

if [[ ! -f "${LDAI_OUTPUT}" ]]; then
  FOUND_APPIMAGE="$(find "${OUT_DIR}" -maxdepth 1 -name '*.AppImage' | head -n 1 || true)"
  if [[ -n "${FOUND_APPIMAGE}" ]]; then
    mv -f "${FOUND_APPIMAGE}" "${LDAI_OUTPUT}"
  fi
fi

(cd "${OUT_DIR}" && sha256sum "v4l2-passthru-${VERSION}-x86_64.AppImage" > "v4l2-passthru-${VERSION}-x86_64.AppImage.sha256")
echo "Created ${LDAI_OUTPUT}"
