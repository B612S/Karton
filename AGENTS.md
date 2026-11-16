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

Karton auto-detects the libvirt connection in `Karton::init()`: without an
explicit setting it tries `qemu:///system` first, then `qemu:///session`, and
selects the first connection that actually has domains. The active URI is shown
in the empty-state message and can be overridden in the Connection Settings
dialog (requires a restart) or with the `KARTON_LIBVIRT_URI` environment
variable, which takes precedence. A stored URI is used first; if it has no
domains and is one of the two local URIs, the other one is tried. The URI is
stored in QSettings under `connection/uri`. Any `virsh` command used during
development or testing must pass `-c <same URI>` (usually
`-c qemu:///system`).

## Repository layout

| Path | Purpose |
| --- | --- |
| `core/app/main.cpp` | Entry point; sets up `KAboutData` and loads the QML module. |
| `core/app/karton.{h,cpp}` | `Karton` QML singleton: libvirt connection, domain list, VM lifecycle operations, paths. |
| `core/domain/domain.{h,cpp}` | QML-exposed `Domain` wrapper around a `virDomainPtr`. |
| `core/domain/domainconfig.{h,cpp}` | QML-exposed per-domain configuration/state model. |
| `core/app/vmlistmodel.{h,cpp}` | `VMModel`, the QML singleton list model shown on the main page. |
| `core/domain/domainxmlbuilder.{h,cpp}` | Generates and edits libvirt domain XML. |
| `core/domain/domainxmlreader.{h,cpp}` | Parses libvirt's domain XML for Karton metadata and disk paths. |
| `core/os/osinfoconfig.{h,cpp}` | libosinfo wrapper (OS list, media detection, architecture). |
| `core/viewer/domainviewer.{h,cpp}` | SPICE client `QQuickItem` used by the VM viewer window. |
| `core/viewer/glscanoutrenderer.*`, `core/viewer/primarysurfacerenderer.*`, `core/viewer/vulkanscanoutrenderer.*` | SPICE display rendering backends. |
| `core/libvirt/libvirtmonitor.*`, `core/libvirt/libvirteventloop.*` | libvirt lifecycle-event monitoring. |
| `core/util/commandrunner.*` | Small `QProcess` helper used for `qemu-img`. |
| `qml/` | Kirigami QML pages and dialogs. |
| `tests/` | QtTest-based unit tests for the XML parser/builder and command runner. |
| `src/` | Reserved for makepkg's `$srcdir`; it is not used for source files. |

C++ sources are grouped by responsibility under `core/`:

- `core/app/` — application entry point, `Karton` singleton and the VM list model.
- `core/domain/` — domain state, configuration, XML builder and reader.
- `core/libvirt/` — libvirt event loop and lifecycle monitor.
- `core/viewer/` — SPICE viewer and display renderers.
- `core/os/` — libosinfo integration.
- `core/util/` — small helpers such as the command runner and DPI/keyboard utilities.

New C++ files must go into the matching subsystem directory; create a new
subsystem only when none of the existing groups fits. Include headers with the
subsystem prefix, for example `#include "domain/domain.h"`; `core/` is an
include root. QML stays in the top-level `qml/` directory.

QML sources are compiled into the binary by `ecm_add_qml_module()` under the URI
`org.kde.karton`. When adding a QML file, add it to the `ecm_target_qml_sources()`
list in the root `CMakeLists.txt`, otherwise it will not be bundled.

## Build

Required: CMake >= 3.20, a C++20 compiler, Qt 6, KF6 (Kirigami, I18n,
CoreAddons, QQC2DesktopStyle, IconThemes), Kirigami Addons >= 1.11, libvirt,
libosinfo, GLib, spice-client-glib, Vulkan (default scene graph and SPICE
scanout import), OpenGL/EGL (fallback renderer), and Extra CMake Modules (ECM)
>= 6.0.

On Arch Linux, the relevant packages are roughly:

```sh
sudo pacman -S --needed base-devel cmake ninja extra-cmake-modules \
    qt6-base qt6-declarative qt6-multimedia qt6-tools \
    kirigami kirigami-addons kcoreaddons ki18n kiconthemes qqc2-desktop-style \
    libvirt libosinfo spice-gtk mesa vulkan-headers vulkan-icd-loader
```

Configure and build out-of-tree:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
./build/bin/karton
```

For an AddressSanitizer/UndefinedBehaviorSanitizer build, configure a
separate tree with `-DKARTON_SANITIZE=ON` (for example `build-asan`) and run
the tests with `ASAN_OPTIONS=detect_leaks=0` (Qt and libvirt produce a lot of
intentional process-lifetime allocations). The `build-*` trees are ignored.

The `build/` directory is git-ignored. If CMake cannot find ECM, either install
`extra-cmake-modules` or point CMake at a local installation, e.g.
`-DCMAKE_PREFIX_PATH="$HOME/.local"`.

Run the app in a graphical session. For automated GUI testing under Wayland it
is often easier to start it on XWayland so that `xdotool` can drive it:

```sh
QT_QPA_PLATFORM=xcb ./build/bin/karton
```

Screenshots can be taken with `spectacle -b -n -f -o /tmp/karton.png`.

## Tests and manual smoke test

Run the automated tests with:

```sh
ctest --test-dir build --output-on-failure
```

They currently cover the domain XML parser/builder and the asynchronous command
runner. A build plus a manual smoke test is still the baseline for UI changes.
Use a throwaway domain so nothing important is touched:

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
  `core/Messages.sh` and the root `CMakeLists.txt` in sync when adding files.
- Use the `KARTON_DEBUG` logging category (`qCInfo(KARTON_DEBUG)`,
  `qCWarning(KARTON_DEBUG)`, ...) rather than raw `qDebug()`.
- Use monochrome `*-symbolic` icon names in QML actions and buttons so the UI
  follows the theme; avoid colored icon names and duplicate icons for different
  actions.
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
- libvirt lifecycle events are delivered through `LibvirtEventLoop`, which
  registers a Qt implementation of the libvirt event API (`QSocketNotifier` for
  handles, `QTimer` for timeouts) before the hypervisor connection is opened.
  The event callback emits synchronously on the GUI thread; keep handlers cheap
  and do not block.
- `CommandRunner::runCommand()` starts a program with an explicit argument list
  (no shell) and invokes a callback with the exit status and output. It does not
  support pipes, globbing, or shell escape sequences. Use
  `runCommandAndWait()` only for short commands where blocking is acceptable
  (currently cached `qemu-img info` queries). VM creation and disk resizing use
  the asynchronous path and report completion through `domainCreated` /
  `domainEdited`.
- The QML dialogs keep themselves open while an asynchronous create/edit is in
  flight; do not close them optimistically after calling `Karton`.
- Libvirt network management lives on `Karton` (`networks()`, `startNetwork()`,
  `stopNetwork()`, `createNatNetwork()`, `attachNetwork()`, `detachNetwork()`)
  and is exposed inline by the Network page of `qml/SettingsDialog.qml`. NIC
  attach uses separate LIVE and CONFIG libvirt calls so a running VM keeps the
  interface after shutdown; NIC detach removes the CONFIG device even if the
  guest cannot hot-unplug the live device.
- Runtime USB passthrough is available through `Karton::usbDevices()`,
  `domainUsbDevices()`, `attachUsbDevice()` and `detachUsbDevice()`, with the
  UI in `qml/UsbDevicesDialog.qml`. Host devices come from libvirt node devices
  (`usb_device` capability) and are attached as USB hostdevs.
- Snapshots are exposed through `Karton::domainSnapshots()`,
  `createSnapshot()`, `revertSnapshot()` and `deleteSnapshot()`, with the UI in
  `qml/SnapshotsDialog.qml`. A running VM with "Include memory" uses an internal
  snapshot with memory; without it, the backend creates an external disk-only
  snapshot with one qcow2 overlay per local data disk. External snapshots must
  be deleted newest-first: `deleteSnapshot()` omits the children flag for them so
  libvirt merges the overlay back instead of failing with "deletion of external
  disk snapshots with children not supported".
- Runtime PCI passthrough is exposed through `Karton::pciDevices()`,
  `domainPciDevices()`, `attachPciDevice()` and `detachPciDevice()`, with the
  UI inline in the USB & PCI page of `qml/SettingsDialog.qml`. Attach is only
  enabled for devices with an IOMMU group and the page warns that the device is
  detached from the host.
- VM cloning is implemented by `Karton::cloneDomain()`, with the UI in
  `qml/CloneDialog.qml`. It copies local file-backed disks with `qemu-img
  convert -U`, strips the UUID, MAC addresses, hostdevs, NVRAM and Karton
  metadata, and defines a stopped clone. The copy runs asynchronously through
  `CommandRunner`; partial destination files are removed on failure.
- Disk/ISO hotplug is exposed through `Karton::domainDisks()`, `addDisk()`,
  `attachDiskImage()`, `attachIso()` and `detachDisk()`, with the UI inline in
  the Disks page of `qml/SettingsDialog.qml`. New disks are created with
  `qemu-img create` and attached as virtio devices; ISO images are attached as
  read-only SATA cdroms. The page uses FileDialog pickers for images/ISOs.
- The viewer discovers the SPICE URI from the domain XML
  (`virDomainGetXMLDesc()`), not by spawning `virsh`. If an external `virsh`
  call is ever needed, pass `-c <the same URI>` to match the application's
  active connection.
- Karton stores its own data inside domain XML metadata:
  `<karton:data maxDiskStorage="…" screenshotPreviewPath="…"/>`.
  `maxDiskStorage` is stored in MiB. `DomainXmlReader` gets the XML from
  libvirt (`virDomainGetXMLDesc()`) rather than reading the config file on
  disk. Preserve unknown metadata when rewriting XML —
  `DomainXmlBuilder::updateXML()` intentionally edits only specific fields.
- Disk shrinking is not supported; `qemu-img resize` is only safe to grow.
- `DomainConfig::virtualDiskPaths` contains every `<disk device='disk'>` path
  found in the domain XML; deleting a VM with "Delete Disk" removes all of
  them. Creating a new VM still creates a single data disk, and size editing
  operates on the first disk (`virtualDiskPath`).
- New virtual disks can be created as `qcow2` (default), `raw` or `vmdk`.
  The format is stored in the domain XML `<driver type="…"/>` and exposed as
  `DomainConfig::diskFormat`; editing an existing domain does not convert its
  format.
- New domains can use BIOS or UEFI firmware (`DomainConfig::firmware`).
  UEFI sets `<os firmware='efi'>` and selects the non-Secure-Boot firmware
  variant via `<loader secure='no'/>` by default, because the default firmware
  descriptor otherwise rejects unsigned installers and guest drivers. An
  optional Secure Boot checkbox in the installer enables
  `<loader secure='yes'/>` instead (`DomainConfig::secureBoot`).
- For domains without Karton metadata, the disk size is queried with
  `qemu-img info -U --output=json` (cached per path; `-U` is required while a
  running VM holds the image lock) and written into the Karton metadata on the
  next configuration save.
- Autostart is toggled from the General configuration tab through
  `Karton::setAutostart()`; it is also read back into `DomainConfig::autostart`.
- Pause/resume/reboot are `Karton::pauseDomain()`, `resumeDomain()` and
  `rebootDomain()`. `Domain::isPaused` mirrors `VIR_DOMAIN_PAUSED`; never infer
  paused state from the localized `DomainConfig::state` string.
- `Karton::domainStats()` returns CPU percentage, CPU time, current/max memory
  and vCPU count for the VM page. `VMPage.qml` polls it with a 2-second `Timer`
  while the VM is running; the percentage is computed from the `virDomainInfo`
  CPU-time delta since the previous call and averaged over the vCPUs.
- Managed save is exposed through `Karton::saveDomainState()` and
  `deleteSavedState()`, with `Domain::hasSavedState` driving the VM page
  actions. `startDomain()` restores a saved state automatically through
  `virDomainCreate()`; never call `virDomainManagedSaveRemove()` while the
  domain is running.
- Application icons are installed under the reverse-DNS name
  `org.kde.karton`, matching `org.kde.karton.desktop` and `QGuiApplication`'s
  window icon lookup.
- `InstallationDialog.qml` is a three-step wizard: installation source,
  resources (memory/storage/format) and system (firmware/network/CPU/
  acceleration). Keep the content of each step short; the previous single long
  form triggered a `Kirigami.Dialog` binding loop on `y`.
- `SettingsDialog.qml` is the virt-manager-style hardware settings dialog:
  Overview, CPUs & Memory, Boot Options, Disks, Network, Display, Sound, USB,
  PCI and TPM pages. Disks, networks, USB and PCI devices are managed inline
  (lists, add/attach/detach controls) instead of separate dialogs. Settings USB
  attach/detach is persistent (`attachUsbDevicePersistent` /
  `detachUsbDevicePersistent`); the viewer's USB dialog remains the runtime
  path. Disks expose advanced options (read-only, shareable, serial, cache and
  discard). It reads `Karton::domainHardwareInfo()` and writes firmware, boot
  order, video, sound, TPM and description through `editDomainHardware()`.
  "Add Hardware" navigates to the relevant Settings page. There are no separate
  Storage/Networks/PCI/USB menu actions.
- New domains use user-mode networking with a `virtio` (default) or `e1000`
  interface, or no network at all. The model is stored in
  `DomainConfig::networkModel` and read back from the domain XML.
- The screenshot preview path defaults to
  `$XDG_STATE_HOME/previews/<uuid>.png` (via `QStandardPaths::StateLocation`).
  `VMPage.qml` polls `Karton::refreshDomainPreview()` every 5 seconds while the
  VM is running, the page is visible and no viewer is open. The capture runs in
  a detached worker thread with a temporary libvirt connection, so
  `virDomainScreenshot()` does not block the GUI or stall mouse input. The
  image is handed back with a queued callback and written through
  `Domain::savePreviewFrame()`.
- SPICE cursor shapes are rendered by `DomainViewer` as a separate scene-graph
  overlay, above the display node; the local cursor is hidden while the remote
  cursor is visible. There is one display node per renderer and one root node
  that owns both. Client mouse mode sends no SPICE `cursor-move` events, so the
  overlay follows the local pointer (and a grabbed virtual pointer) instead of
  the initial `(0, 0)` cursor position.
- The viewer's `DomainViewer` must receive keyboard focus after the window
  opens (`forceActiveFocus` in `VMViewerWindow.qml` and on click). Scancodes are
  normalized to evdev by subtracting the X11/xkb keycode offset of 8 on both
  `xcb` and `wayland`; subtracting it only on Wayland made keys map to unrelated
  PC XT scancodes under XWayland.
- `DomainViewer::updateDisplayResolution()` calls
  `spice_main_channel_update_display()` with the **page/content size** in device
  pixels on page resize and when the SPICE agent connects; it enables display 0
  and sends the monitor configuration immediately when the agent is present.
  It deliberately does not use the aspect-fitted `DomainViewer` item size: the
  item changes size when the guest resolution changes, which caused a resize
  feedback loop while Windows was booting. The guest adapts its resolution
  through the agent (spice-vdagent / Windows guest tools); without an agent the
  request is a no-op. `fitGuestToWindow()` forces a fresh request even if the
  target size did not change.
- Pointer motion calls `QQuickItem::update()` so the local SPICE cursor overlay
  is repainted on every motion event; without it the cursor looked laggy even
  though input was sent at full rate. While the pointer is inside the display,
  a guest `cursor-hide` keeps the last guest cursor and leaves the host cursor
  blank, so dragging a guest window does not flash the host arrow.
- `PrimarySurfaceRenderer` copies SPICE's primary framebuffer into an owned
  `QImage`. The original buffer is unmapped by spice-gtk when the guest changes
  resolution (notably while Windows boots); creating/using a texture from that
  pointer on the render thread used to crash `QSGBatchRenderer::prepareRenderPass`.
  `updatePaintNode()` must also clear its cached `m_displayNode`/`m_cursorNode`
  pointers when Qt passes a null root after deleting the scene graph. Display
  textures use `QSGTexture::Linear` filtering so fonts stay readable when the
  guest resolution does not exactly match the viewer size.
- `VMViewerWindow.qml` starts at 85% of the screen. Its toolbar has labeled
  runtime actions: Start when stopped, Power (menu with Shut Down and Force
  Off), Reboot (menu with Reboot and Reset), fullscreen and a three-dot menu
  containing Actual Size, Fit Guest to Window, USB Devices, Settings and
  Snapshots. "Fit to Window" was deliberately removed. A stopped VM opens the
  viewer without starting it and shows only a large clickable start triangle;
  it is started only when the user asks. When a running VM stops, the viewer
  closes automatically. Opening the menu releases the pointer grab first. There
  is no explicit grab/release-pointer action: clicking the display grabs the
  pointer and Esc releases it. A double-click on a VM list row opens that VM's
  viewer, whether it is running or stopped. UI icons use the monochrome
  `*-symbolic` variants. PCI and Storage have no separate menu actions; they are
  managed inline from SettingsDialog.
- The scene graph uses Vulkan by default (`QQuickWindow::setGraphicsApi()` in
  `main.cpp`), with automatic OpenGL fallback when Vulkan is unavailable.
  `KARTON_GRAPHICS_API=opengl|vulkan|software` overrides the choice.
  `VulkanScanoutRenderer` imports the SPICE GL scanout DMA-BUF into a
  `VkImage` using `VK_EXT_external_memory_dma_buf` and
  `VK_EXT_image_drm_format_modifier`; `main.cpp` enables those device
  extensions before the scene graph is created. ARGB/XRGB scanouts are sampled
  directly, RGB565 scanouts are copied to a host buffer and converted to RGBA8.
  The OpenGL renderer remains available with `KARTON_GRAPHICS_API=opengl`.
  New domains can use 3D acceleration under Vulkan.
- Suspend/undefine/define events can arrive while a QML page is open; check for
  valid domain pointers and handle missing files gracefully.

## Git workflow

- `origin` is the personal fork `https://github.com/B612S/karton` and `main` is
  the working branch. `upstream` is `https://github.com/kenoi1/Karton`.
- Push work to `origin/main` only. Do not push to `upstream`.
- Write imperative, descriptive commit messages (e.g. "Fix storage check when
  editing a domain").
- Before committing, build the project and, for UI changes, run the manual smoke
  test above.
