<!--
SPDX-License-Identifier: CC0-1.0
SPDX-FileCopyrightText: none
-->

# Karton TODO

## Planned

- [x] VM list search/filter: name, OS, UUID, running/stopped.
- [x] Drag and drop an ISO/image onto the VM page to insert or attach it.
- [x] Persistent error journal instead of short-lived passive notifications.
- [x] Import/export of domain XML.
- [x] Import OVF/OVA packages.
- [x] NAT networks: DHCP lease list/reservations and TCP port forwarding.
- [x] Disk tools: `qemu-img` check, compact and convert from the Disks page.
- [x] Snapshot UX: internal/external type, size, and a quick snapshot action.

## Later

- [ ] SPICE multi-monitor: one display channel per monitor and a layout picker.
- [ ] Multiple libvirt connections at the same time (system and session).

## Not planned

- Libvirt storage pool/volume management. Karton deliberately works with
  direct disk paths and does not manage storage pools.
