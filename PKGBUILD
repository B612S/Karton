# Maintainer: Karton contributors
# SPDX-License-Identifier: CC0-1.0
# SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

pkgname=karton
# The version is taken from the local CMake project; override with
# KARTON_PKGVER for release builds.
pkgver="${KARTON_PKGVER:-$(sed -n 's/^project(karton[[:space:]]\+VERSION[[:space:]]\+\([^)]*\)).*/\1/p' "${startdir:-$PWD}/CMakeLists.txt" 2>/dev/null | head -n1)}"
pkgver="${pkgver:-1.0.0}"
pkgrel=1
pkgdesc="Libvirt-based virtual machine manager for KDE (local build)"
arch=('x86_64' 'aarch64')
url="https://github.com/B612S/karton"
license=('GPL-3.0-or-later')
options=('strip' '!debug')

depends=(
    'qt6-base'
    'qt6-declarative'
    'qt6-multimedia'
    'kirigami'
    'kirigami-addons'
    'kcoreaddons'
    'kiconthemes'
    'qqc2-desktop-style'
    'libvirt'
    'acl'
    'libosinfo'
    'spice-gtk'
    'phodav'
    'pipewire'
    'mesa'
    'hicolor-icon-theme'
)
makedepends=(
    'cmake'
    'ninja'
    'extra-cmake-modules'
    'qt6-tools'
)
checkdepends=('appstream')
conflicts=('karton-git')

# This PKGBUILD packages the working tree, so there is no remote source.
source=()
sha256sums=()

prepare() {
    # Copy the checkout into $srcdir without local build, VCS or packaging data.
    rm -rf "$srcdir/karton"
    mkdir -p "$srcdir/karton"
    tar -C "${startdir:-$PWD}" \
        --exclude='./.git' \
        --exclude='./build' \
        --exclude='./build-*' \
        --exclude='./pkg' \
        --exclude='./src' \
        --exclude='*.pkg.tar.*' \
        -cf - . | tar -C "$srcdir/karton" -xf -
}

build() {
    cd "$srcdir/karton"
    cmake -B build -S . -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DBUILD_TESTING=ON
    cmake --build build
}

check() {
    cd "$srcdir/karton"
    ctest --test-dir build --output-on-failure
}

package() {
    cd "$srcdir/karton"
    DESTDIR="$pkgdir" cmake --install build
    install -Dm644 LICENSES/GPL-3.0-or-later.txt \
        "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
