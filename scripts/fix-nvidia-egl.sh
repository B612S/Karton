#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>
#
# Add the proprietary NVIDIA device nodes to libvirt's cgroup_device_acl.
#
# QEMU is started by libvirt inside a device cgroup. Its default allowlist
# contains the DRI render node but not the NVIDIA control nodes, so NVIDIA's
# EGL driver cannot initialize the render node and SPICE GL / virtio-vga-gl
# domains fail at startup with:
#
#   egl: eglInitialize failed: EGL_NOT_INITIALIZED
#   egl: render node init failed
#
# Usage:
#   sudo scripts/fix-nvidia-egl.sh [--dry-run] [--no-restart]
#   sudo scripts/fix-nvidia-egl.sh --config /path/to/qemu.conf
set -euo pipefail

default_config=/etc/libvirt/qemu.conf
config=$default_config
dry_run=0
restart=1

usage() {
    cat <<'EOF'
Usage: sudo fix-nvidia-egl.sh [options]

Add /dev/nvidiactl, /dev/nvidia0 and /dev/nvidia-modeset to the libvirt
qemu.conf cgroup_device_acl so QEMU can use NVIDIA EGL (SPICE GL,
virtio-vga-gl). The file is backed up before it is changed.

Options:
  --config PATH   edit PATH instead of /etc/libvirt/qemu.conf
  -n, --dry-run   print the diff and do not write anything
      --no-restart  do not restart libvirtd after updating the file
  -h, --help      show this help
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --config)
            if [ "$#" -lt 2 ]; then
                echo "error: --config needs a path" >&2
                exit 2
            fi
            config=$2
            shift 2
            ;;
        -n | --dry-run)
            dry_run=1
            shift
            ;;
        --no-restart)
            restart=0
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ "$config" != "$default_config" ]; then
    # A custom file is useful for testing and for non-default libvirt
    # instances; never restart the system daemon behind the user's back.
    restart=0
fi

if [ "$(id -u)" -ne 0 ] && [ "$dry_run" -eq 0 ] && [ "$config" = "$default_config" ]; then
    echo "error: this script must be run as root; use: sudo $0" >&2
    exit 1
fi

if [ ! -f "$config" ]; then
    echo "error: $config not found; is libvirt installed?" >&2
    exit 1
fi

for node in /dev/nvidiactl /dev/nvidia0 /dev/nvidia-modeset; do
    if [ ! -e "$node" ]; then
        echo "warning: $node not found; is the proprietary NVIDIA driver loaded?" >&2
    fi
done

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
cp "$config" "$tmp"

set +e
result=$(python3 - "$tmp" <<'PY'
import re
import sys

path = sys.argv[1]
nvidia_nodes = ["/dev/nvidiactl", "/dev/nvidia0", "/dev/nvidia-modeset"]
default_nodes = [
    "/dev/null",
    "/dev/full",
    "/dev/zero",
    "/dev/random",
    "/dev/urandom",
    "/dev/ptmx",
    "/dev/userfaultfd",
]

with open(path, encoding="utf-8") as handle:
    original = handle.read()
lines = original.splitlines(keepends=True)

active_re = re.compile(r"^\s*cgroup_device_acl\s*=\s*\[")
commented_re = re.compile(r"^\s*#\s*cgroup_device_acl\s*=\s*\[")


def find_start(pattern):
    for index, line in enumerate(lines):
        if pattern.match(line):
            return index
    return None


def find_end(start):
    for index in range(start, len(lines)):
        if "]" in lines[index]:
            return index
    return None


def quoted_entries(start, end):
    entries = set()
    for line in lines[start : end + 1]:
        # Ignore commented-out entries inside an otherwise active block.
        code = line.split("#", 1)[0]
        entries.update(re.findall(r'"([^"]+)"', code))
    return entries


def add_missing(start, end):
    present = quoted_entries(start, end)
    if present:
        missing = [node for node in nvidia_nodes if node not in present]
    else:
        # An empty active block would otherwise drop the devices QEMU needs
        # for normal operation.
        missing = default_nodes + nvidia_nodes
    if not missing:
        return []

    indent = None
    for line in lines[start : end + 1]:
        match = re.match(r'^(\s*)"', line)
        if match:
            indent = match.group(1)
            break
    if indent is None:
        indent = "    "

    if start == end:
        # Single-line form: cgroup_device_acl = [ "...", "..." ]
        line = lines[start]
        closing = line.index("]")
        before = line[:closing].rstrip()
        after = line[closing:]
        if before and not before.endswith((",", "[")):
            before += ","
        entries = ", ".join(f'"{node}"' for node in missing)
        separator = "" if before.endswith("[") else " "
        if after.startswith("]"):
            after = " " + after
        lines[start] = f"{before}{separator}{entries}{after}"
        return missing

    # Multiline form: add a comma to the last entry if needed, then insert
    # the new entries just before the closing bracket.
    previous = end - 1
    while previous > start and not lines[previous].strip():
        previous -= 1
    if previous >= start:
        stripped = lines[previous].rstrip("\n").rstrip()
        if stripped and not stripped.endswith(","):
            lines[previous] = stripped + ",\n"

    inserted = "".join(f'{indent}"{node}",\n' for node in missing)
    lines.insert(end, inserted)
    return missing


start = find_start(active_re)
if start is None:
    commented = find_start(commented_re)
    if commented is not None:
        end = find_end(commented)
        if end is None:
            print("error: unterminated cgroup_device_acl block", file=sys.stderr)
            sys.exit(1)
        for index in range(commented, end + 1):
            lines[index] = lines[index].replace("#", "", 1)
        start = commented

if start is not None:
    end = find_end(start)
    if end is None:
        print("error: unterminated cgroup_device_acl block", file=sys.stderr)
        sys.exit(1)
    added = add_missing(start, end)
else:
    added = list(nvidia_nodes)
    if lines and not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    lines.append("\n# Added by scripts/fix-nvidia-egl.sh\n")
    lines.append("cgroup_device_acl = [\n")
    for node in default_nodes + nvidia_nodes:
        lines.append(f'    "{node}",\n')
    lines.append("]\n")

updated = "".join(lines)
if updated == original:
    print("UNCHANGED")
    sys.exit(0)

with open(path, "w", encoding="utf-8") as handle:
    handle.write(updated)

if added:
    print("added: " + " ".join(added), file=sys.stderr)
print("CHANGED")
PY
)
status=$?
set -e

if [ "$status" -ne 0 ]; then
    echo "error: failed to update $config" >&2
    exit "$status"
fi

if [ "$result" = "UNCHANGED" ]; then
    echo "$config already contains the NVIDIA device nodes; nothing to do."
    exit 0
fi

if [ "$result" != "CHANGED" ]; then
    echo "error: unexpected helper output: $result" >&2
    exit 1
fi

if [ "$dry_run" -eq 1 ]; then
    diff -u "$config" "$tmp" || true
    echo
    echo "Dry run: no changes written."
    exit 0
fi

backup="${config}.bak-$(date +%Y%m%d-%H%M%S)"
if [ -e "$backup" ]; then
    backup="${backup}-$$"
fi
cp -a "$config" "$backup"
# cat keeps the original inode, owner and mode instead of replacing the file.
cat "$tmp" >"$config"
echo "Updated $config"
echo "Backup:  $backup"

if [ "$restart" -eq 0 ]; then
    echo "Skipped the libvirtd restart; run 'sudo systemctl restart libvirtd' to apply."
    exit 0
fi

if command -v systemctl >/dev/null 2>&1 && systemctl cat libvirtd.service >/dev/null 2>&1; then
    if ! systemctl restart libvirtd; then
        echo "error: libvirtd failed to start with the new configuration; restoring the backup" >&2
        cat "$backup" >"$config"
        systemctl restart libvirtd || true
        exit 1
    fi
    echo "Restarted libvirtd."
else
    echo "warning: could not find the libvirtd systemd unit; restart libvirtd manually." >&2
fi

if command -v virsh >/dev/null 2>&1; then
    if virsh -c qemu:///system version >/dev/null 2>&1; then
        echo "libvirt is reachable."
    else
        echo "warning: could not connect to libvirtd; check 'systemctl status libvirtd'." >&2
    fi
fi

cat <<'EOF'

Done. Start the VM and verify the renderer inside the guest:

    glxinfo -B | grep -i renderer

It should report a virgl (NVIDIA ...) renderer.
EOF
