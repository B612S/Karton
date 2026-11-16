<!--
SPDX-License-Identifier: CC0-1.0
SPDX-FileCopyrightText: 2024 Aaron Rainbolt <arraybolt3@gmail.com>
SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
-->

# Karton <img src="img/karton_logo.svg" width="40"/>
A Libvirt-based Virtual Machine Manager for KDE.

## WARNING
This project is still in early development and will likely break existing virtual machines.

## Features
- Displays list of existing VMs
- VM lifecycle: start, stop, pause/resume, reboot, force stop and open the console
- SPICE viewer with pointer/keyboard capture, a large initial window, guest
  resolution auto-fitting, live VM previews and a categorized viewer menu
  (display, devices and machine actions including VM settings)
- Double-click a VM in the list to open its viewer directly
- Managed save: store a running VM's state on disk and restore it on the next start
- Live CPU and memory usage on the VM page
- Installing and deleting VMs
- qcow2, raw and vmdk virtual disks
- BIOS and UEFI firmware, optional Secure Boot
- Optional autostart on boot
- User-mode networking with virtio or e1000, or no network

## Dependencies
- Kirigami
- Kirigami Addons
- Qt Quick Controls
- libvirt
- spice-client-glib
- libosinfo
- Vulkan with DMA-BUF scanout import (or OpenGL as a fallback)
- Automatic system/session libvirt connection detection
- Libvirt network management: list, start/stop, autostart, NAT creation and VM NIC attach/detach
- Runtime USB passthrough from host to running VMs, managed from the viewer window
- VM snapshots: internal snapshots (with optional memory) and external disk-only
  snapshots for running VMs, with revert and delete
- Runtime PCI passthrough with IOMMU group information and warnings
- VM cloning with copied disk images and regenerated UUID/MAC
- Runtime disk and ISO hotplug, with optional disk-file deletion on detach

## Configuration
The connection URI can be changed in the Connection Settings dialog (restart
required) or with `KARTON_LIBVIRT_URI`, which overrides the stored value (for
example `KARTON_LIBVIRT_URI=qemu:///system`). Set
`KARTON_GRAPHICS_API=opengl` if Vulkan is unavailable and the automatic
fallback is not desired.

## Installation
```
git clone https://github.com/B612S/karton.git
cd karton
mkdir build
cd build
cmake ..
make -j$(nproc)
sudo make install
```
Check your app menu for "Karton", it should be hiding under the "System" section.

## Gallery
<img src="https://cdn.kde.org/screenshots/karton/karton_viewer.png" width="800"/>
<img src="https://cdn.kde.org/screenshots/karton/karton_homepage.png" width="370"/>
<img src="https://cdn.kde.org/screenshots/karton/karton_installer.png" width="370"/>


## Arch Linux
A `PKGBUILD` for the git version is included in the repository root; build and
install it with `makepkg -si`.

## Development
```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```
Use `-DKARTON_SANITIZE=ON` for an ASAN/UBSAN build.

## We hope you find Karton useful!

Feel free to join our matrix at [#karton:kde.org](https://matrix.to/#/#karton:kde.org).