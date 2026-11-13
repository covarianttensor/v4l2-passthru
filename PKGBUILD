# Maintainer: covarianttensor <covarianttensor@users.noreply.github.com>
pkgname=v4l2-passthru
pkgver=0.1.1
pkgrel=1
pkgdesc='Low-latency V4L2 video and PipeWire/PulseAudio audio passthrough viewer'
arch=('x86_64')
url='https://github.com/covarianttensor/v4l2-passthru'
license=('GPL-3.0-only')
depends=(
    'alsa-lib'
    'gst-plugin-pipewire'
    'gst-plugins-base-libs'
    'gst-plugins-good'
    'gstreamer'
    'hicolor-icon-theme'
    'qt6-base'
    'qt6-declarative'
    'qt6-multimedia'
    'qt6-multimedia-ffmpeg'
    'qt6-svg'
    'systemd-libs'
)
makedepends=(
    'cmake'
    'gcc'
    'pkgconf'
)
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
    cmake -B build -S "$pkgname-$pkgver" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -Wno-dev
    cmake --build build
}

package() {
    DESTDIR="$pkgdir" cmake --install build
    install -Dm644 "$pkgname-$pkgver/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
