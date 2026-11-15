# Maintainer: Karton contributors
# SPDX-License-Identifier: CC0-1.0

pkgname=karton-git
pkgver=r0.0000000
pkgrel=1
pkgdesc="Libvirt-based virtual machine manager for KDE (git version)"
arch=('x86_64' 'aarch64')
url="https://github.com/B612S/Karton"
license=('GPL-3.0-or-later')
depends=(
    'qt6-base'
    'qt6-declarative'
    'qt6-multimedia'
    'kirigami'
    'kirigami-addons'
    'kcoreaddons'
    'ki18n'
    'kiconthemes'
    'qqc2-desktop-style'
    'libvirt'
    'libosinfo'
    'spice-gtk'
    'mesa'
    'vulkan-icd-loader'
    'hicolor-icon-theme'
)
checkdepends=('appstream')
makedepends=(
    'cmake'
    'ninja'
    'extra-cmake-modules'
    'vulkan-headers'
    'git'
)
provides=('karton')
conflicts=('karton')
source=("git+${url}.git")
sha256sums=('SKIP')

pkgver() {
    cd "${srcdir}/Karton"
    printf 'r%s.%s' "$(git rev-list --count HEAD)" "$(git rev-parse --short=7 HEAD)"
}

build() {
    cmake -S "${srcdir}/Karton" -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DBUILD_TESTING=ON
    cmake --build build
}

check() {
    ctest --test-dir build --output-on-failure
}

package() {
    DESTDIR="${pkgdir}" cmake --install build
    install -Dm644 "${srcdir}/Karton/LICENSES/GPL-3.0-or-later.txt" \
        "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
}
