<!--
SPDX-License-Identifier: CC0-1.0
SPDX-FileCopyrightText: 2024 Aaron Rainbolt <arraybolt3@gmail.com>
SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
SPDX-FileCopyrightText: 2026 Karton contributors
-->

# Karton

A libvirt-based virtual machine manager for KDE.

Karton is built with C++23, Qt 6, KF6 and Kirigami. It manages local libvirt
domains, creates new VMs from installation media, and embeds a SPICE console
with a desktop-oriented viewer.

> **Early development:** Karton is still evolving and may change domain XML or
> break existing VMs. Back up important disks before using it with production
> machines.

## Features

### Virtual machines

- List local libvirt domains with their OS icon and human-readable OS name.
- Create VMs from an ISO or disk image.
- Start, shut down, power off, pause, resume, reboot and reset.
- Autostart on boot.
- Hibernate and restore a running VM through libvirt managed save.
- Clone a VM, copy its disks and regenerate UUID/MAC addresses.
- Take snapshots, revert to them and delete them.
- Delete a VM together with its local disks when requested.
- Live CPU and memory usage on the VM page.

### SPICE viewer

- Embedded SPICE display with keyboard and pointer capture.
- Split power actions: Shut Down/Start plus a separate menu for Power Off,
  Reboot plus Reset, and Pause as a direct action.
- Fullscreen mode and a More menu for display, USB, snapshots and Settings.
- Live VM previews on the VM page.
- Per-VM fixed display resolution, including custom resolutions.
- Optional guest resolution auto-fit while the VM is running.
- Separate per-VM sound output and microphone controls.
- Runtime USB and PCI passthrough from the viewer.
- Opening the same VM twice focuses the existing viewer instead of creating a
  duplicate.

### Settings

Settings is a Luma-style overlay attached to the window that owns the VM.

VM pages include:

- Overview
- CPUs & Memory
- Boot Options
- Disks and CD-ROMs
- Network interfaces
- Display and video
- Sound
- USB
- PCI
- TPM
- Input devices
- Controllers
- Serial & Console
- Remaining XML devices
- OS Information
- Performance
- Domain XML editor

The XML editor is read-only by default, has an explicit Edit switch, syntax
highlighting and scrolls independently of the page.

**Add…** opens a grouped Add Hardware dialog for:

- storage, disk images and ISO images;
- network interfaces;
- input devices;
- USB and PCI passthrough;
- controllers, serial ports and consoles;
- VirtIO channels;
- USB redirection, watchdog and RNG.

TPM is configured on the VM's TPM Settings page.

Global Application Settings provide:

- the libvirt connection URI;
- SPICE TLS: prefer TLS when the VM exposes a TLS port, and the CA certificate path;
- shared libvirt networks (start/stop, autostart, create NAT, delete);
- About information.

## Requirements

- Linux with KVM and libvirt
- C++23 compiler
- CMake >= 3.20
- Qt 6
- KF6: Kirigami, Kirigami Addons, I18n, CoreAddons, QQC2 Desktop Style,
  Icon Themes
- libvirt
- libosinfo
- GLib
- spice-client-glib
- Vulkan with external-memory/DMA-BUF support, or OpenGL as a fallback
- Extra CMake Modules

### Arch Linux

The repository contains a `PKGBUILD`:

```sh
cd ~/Projects/karton
makepkg -si
```

Development packages:

```sh
sudo pacman -S --needed base-devel cmake ninja extra-cmake-modules \
    qt6-base qt6-declarative qt6-multimedia qt6-tools \
    kirigami kirigami-addons kcoreaddons ki18n kiconthemes qqc2-desktop-style \
    libvirt libosinfo spice-gtk mesa vulkan-headers vulkan-icd-loader
```

## Build from source

```sh
git clone https://github.com/B612S/karton.git
cd karton
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Run the development build:

```sh
./build/bin/karton
```

Install system-wide:

```sh
sudo cmake --install build
```

The application icon is installed as `org.kde.karton.svg` under
`hicolor/scalable/apps`.

## Configuration

Karton auto-detects the libvirt connection:

1. a stored URI is tried first;
2. otherwise `qemu:///system` is tried;
3. then `qemu:///session`.

If the selected URI has no domains, Karton tries the other local URI. The active
URI is shown on the empty VM list.

The URI can be changed in **Application Settings**; a restart is required.
`KARTON_LIBVIRT_URI` overrides both the stored value and auto-detection:

```sh
KARTON_LIBVIRT_URI=qemu:///system ./build/bin/karton
```

Karton stores its settings in:

```text
$XDG_CONFIG_HOME/karton/karton.conf
```

That includes the connection URI, main-window size, per-VM viewer size,
per-VM display resolution and audio preferences.

### Rendering

Vulkan is used by default, with automatic OpenGL fallback. Force a renderer:

```sh
KARTON_GRAPHICS_API=opengl ./build/bin/karton
KARTON_GRAPHICS_API=vulkan ./build/bin/karton
KARTON_GRAPHICS_API=software ./build/bin/karton
```

## Testing

Tests are opt-in, modeled on Luma: they build as separate executables, are never
linked into the application and are never installed with the package.

```sh
cmake -S . -B build -G Ninja -DKARTON_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The unit tests run offscreen and do not require libvirt. For UI changes also run
the application manually and exercise the affected pages with a throwaway VM.

## Project layout

| Path | Purpose |
| --- | --- |
| `main.cpp` | Application entry point. |
| `core/app/` | `Karton` singleton, VM list model and application services. |
| `core/domain/` | Domain wrapper, configuration and XML read/write. |
| `core/viewer/` | SPICE viewer and display renderers. |
| `core/libvirt/` | libvirt event loop and lifecycle monitor. |
| `core/os/` | libosinfo integration and OS icon mapping. |
| `core/util/` | Small helpers, including XML syntax highlighting. |
| `qml/` | Kirigami pages and dialogs. |
| `img/os/` | Bundled flat OS family icons. |
| `tests/` | Optional QtTest unit tests. |

## Contributing

- Use C++23 and Qt/KF6 APIs.
- Keep UI strings translatable with `i18n()`/`i18nc()`.
- Run `clang-format` on touched C++ files.
- Keep user-visible changes tested with a disposable VM.
- Never test destructive operations on the user's production VMs.

See `AGENTS.md` for the full engineering conventions and architecture notes.

## Links

- Repository: <https://github.com/B612S/karton>
- Upstream: <https://github.com/kenoi1/Karton>
- Matrix: <https://matrix.to/#/#karton:kde.org>

## License

Karton is licensed under **GPL-3.0-or-later**. The icon and documentation are
covered by the licenses declared in their SPDX headers. Bundled OS family icons
are derived from Papirus and keep their original GPL-3.0-or-later licensing.
