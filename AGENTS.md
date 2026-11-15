<!--
SPDX-License-Identifier: CC0-1.0
SPDX-FileCopyrightText: none
-->

# AGENTS.md

Guidance for AI coding agents working on Karton.

## What Karton is

Karton is a Libvirt-based virtual machine manager for KDE. It is a C++20 / Qt 6 /
KF6 application with a Kirigami UI. It lists and manages local libvirt domains,
defines new domains from OS installation media, and embeds a SPICE display for
the VM console.

The application connects to **`qemu:///session`** (see `Karton::init()`). There
is currently no UI for changing the libvirt connection URI. Any `virsh` command
used during development or testing must therefore pass
`-c qemu:///session`.

## Repository layout

| Path | Purpose |
| --- | --- |
| `src/main.cpp` | Entry point; sets up `KAboutData` and loads the QML module. |
| `src/karton.{h,cpp}` | `Karton` QML singleton: libvirt connection, domain list, VM lifecycle operations, paths. |
| `src/domain.{h,cpp}` | QML-exposed `Domain` wrapper around a `virDomainPtr`. |
| `src/domainconfig.{h,cpp}` | QML-exposed per-domain configuration/state model. |
| `src/vmlistmodel.{h,cpp}` | `VMModel`, the QML singleton list model shown on the main page. |
| `src/domainxmlbuilder.{h,cpp}` | Generates and edits libvirt domain XML. |
| `src/domainxmlreader.{h,cpp}` | Reads libvirt's saved XML config files for Karton metadata. |
| `src/osinfoconfig.{h,cpp}` | libosinfo wrapper (OS list, media detection, architecture). |
| `src/domainviewer.{h,cpp}` | SPICE client `QQuickItem` used by the VM viewer window. |
| `src/glscanoutrenderer.*`, `src/primarysurfacerenderer.*` | SPICE display rendering backends. |
| `src/libvirtmonitor.*`, `src/libvirteventloop.*` | libvirt lifecycle-event monitoring. |
| `src/commandrunner.*` | Small `QProcess` helper used for `qemu-img` / `virsh`. |
| `src/qml/` | Kirigami QML pages and dialogs. |

QML sources are compiled into the binary by `ecm_add_qml_module()` under the URI
`org.kde.karton`. When adding a QML file, add it to the `ecm_target_qml_sources()`
list in `src/CMakeLists.txt`, otherwise it will not be bundled.

## Build

Required: CMake >= 3.20, a C++20 compiler, Qt 6, KF6 (Kirigami, I18n,
CoreAddons, QQC2DesktopStyle, IconThemes), Kirigami Addons >= 1.11, libvirt,
libosinfo, GLib, spice-client-glib, OpenGL/EGL, and Extra CMake Modules (ECM)
>= 6.0.

On Arch Linux, the relevant packages are roughly:

```sh
sudo pacman -S --needed base-devel cmake ninja extra-cmake-modules \
    qt6-base qt6-declarative qt6-multimedia qt6-tools \
    kirigami kirigami-addons kcoreaddons ki18n kiconthemes qqc2-desktop-style \
    libvirt libosinfo spice-gtk mesa
```

Configure and build out-of-tree:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
./build/bin/karton
```

The `build/` directory is git-ignored. If CMake cannot find ECM, either install
`extra-cmake-modules` or point CMake at a local installation, e.g.
`-DCMAKE_PREFIX_PATH="$HOME/.local"`.

Run the app in a graphical session. For automated GUI testing under Wayland it
is often easier to start it on XWayland so that `xdotool` can drive it:

```sh
QT_QPA_PLATFORM=xcb ./build/bin/karton
```

Screenshots can be taken with `spectacle -b -n -f -o /tmp/karton.png`.

## Manual smoke test

There are no automated tests yet. A build plus a manual smoke test is the
current baseline. Use a throwaway domain so nothing important is touched:

```sh
qemu-img create -f qcow2 ~/.local/share/libvirt/images/agent-test.qcow2 2G
virsh -c qemu:///session define /tmp/agent-test.xml   # minimal domain XML
virsh -c qemu:///session list --all
```

Start Karton, confirm the VM appears, open it, and exercise start, stop, config
edit, eject, and delete. Clean up afterwards:

```sh
virsh -c qemu:///session undefine agent-test
rm -f ~/.local/share/libvirt/images/agent-test.qcow2
```

When testing destructive operations, prefer domains whose names start with
`agent-` and never operate on the user's real VMs.

## Code conventions

- C++20, Qt 6 / KF6 APIs. Prefer Qt/KF6 classes and `QStringLiteral` over
  `const char *` where practical.
- Every source file starts with SPDX headers. Most C++/QML sources are
  `GPL-3.0-or-later`; CMake files are `BSD-2-Clause`; documentation is
  `CC0-1.0`. A few files (e.g. `devicepixelratiohelper.*`) use
  `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL`. Match the
  header of the file you are editing, and do not remove existing copyright
  lines.
- Formatting is enforced by the KDE clang-format configuration. Run
  `clang-format -i` on touched C++ files (the build wires up
  `kde_clang_format`).
- User-visible strings must be translatable: use `i18n()` / `i18nc()` in C++
  and QML, and add translation contexts as existing code does. Keep
  `src/Messages.sh` and `src/CMakeLists.txt` in sync when adding files.
- Use the `KARTON_DEBUG` logging category (`qCInfo(KARTON_DEBUG)`,
  `qCWarning(KARTON_DEBUG)`, ...) rather than raw `qDebug()`.
- Keep changes focused. Do not reformat unrelated code or check in `build/`,
  editor files, or generated artifacts.

## Architecture notes and pitfalls

- **`Domain` objects are owned by `Karton`** (QObject children). They own their
  `virDomainPtr` and free it in `~Domain()`. QML pages keep raw `Domain *`
  pointers, so do not delete `Domain` objects while a page may still reference
  them; use `deleteLater()` and clear model/page references first.
- **Use `Domain::isActive` for running/stopped logic, not the `state` string.**
  `DomainConfig::state` is a localized display string (e.g. "running" in
  English, "работает" in Russian), so comparing it to an English literal breaks
  in other locales.
- `virConnectListAllDomains()` returns referenced `virDomainPtr`s. Karton's
  `Domain` objects take ownership of those references; don't add extra refs or
  frees without checking the surrounding code.
- `virDomainGetXMLDesc()` returns a `char *` that must be freed with `free()`.
- libvirt lifecycle events are delivered through `LibvirtEventLoop`, which polls
  `virEventRunDefaultImpl()` from a 1-second `QTimer` on the GUI thread. The
  event callback emits synchronously, so handlers run on the GUI thread; keep
  them cheap and do not block.
- `CommandRunner::runCommand()` starts a program with an explicit argument list
  (no shell), so it does not support pipes, globbing, or shell escape
  sequences. It only reports whether the process *started*; use
  `runCommandAndWait()` when you need the exit code, or `commandFinished` for
  the asynchronous result.
- External `virsh` calls (e.g. in `DomainViewer`) must use
  `-c qemu:///session` to match the application's connection.
- Karton stores its own data inside domain XML metadata:
  `<karton:data maxDiskStorage="…" screenshotPreviewPath="…"/>`.
  `maxDiskStorage` is stored in MiB. Preserve unknown metadata when rewriting
  XML — `DomainXmlBuilder::updateXML()` intentionally edits only specific
  fields.
- Disk shrinking is not supported; `qemu-img resize` is only safe to grow.
- The screenshot preview path defaults to
  `$XDG_STATE_HOME/previews/<uuid>.png` (via `QStandardPaths::StateLocation`).
- Suspend/undefine/define events can arrive while a QML page is open; check for
  valid domain pointers and handle missing files gracefully.

## Git workflow

- `origin` is the personal fork `https://github.com/B612S/Karton` and `main` is
  the working branch. `upstream` is `https://github.com/kenoi1/Karton`.
- Push work to `origin/main` only. Do not push to `upstream`.
- Write imperative, descriptive commit messages (e.g. "Fix storage check when
  editing a domain").
- Before committing, build the project and, for UI changes, run the manual smoke
  test above.
