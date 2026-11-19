#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>
#
# Karton guest tools installer for Linux guests.
#
# Run this script inside the guest (copy it there and execute it):
#
#     sh install.sh
#
# It downloads and installs the QEMU guest agent, the SPICE agent, file
# clipboard support, the Mesa virtio-gpu drivers and a serial console getty
# with the distribution's package manager, so the guest needs internet access.
# Karton itself does not download or install anything in the guest.
#
# Package manager output goes to /tmp/karton-guest-tools.log; the terminal
# only shows a progress line.

set -eu

LOG=/tmp/karton-guest-tools.log
: >"$LOG" 2>/dev/null || true

SUDO=''
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO='sudo'
    else
        echo 'Run this script as root (for example: su -c "sh install.sh").' >&2
        exit 1
    fi
fi

say() {
    printf '%s\n' "$*"
}

warn() {
    printf 'warning: %s\n' "$*" >&2
}

# Run a package manager command with a moving progress line instead of its
# full output; the output itself goes to the log file.
run_quiet() {
    label=$1
    shift
    "$@" >>"$LOG" 2>&1 &
    pid=$!
    n=0
    while kill -0 "$pid" 2>/dev/null; do
        n=$((n + 1))
        case $((n % 4)) in
            0) dots='' ;;
            1) dots='.' ;;
            2) dots='..' ;;
            3) dots='...' ;;
        esac
        printf '\r\033[K%s%s' "$label" "$dots"
        sleep 0.3
    done
    rc=0
    wait "$pid" || rc=$?
    printf '\r\033[K'
    if [ "$rc" -ne 0 ]; then
        printf '%s failed. Last log lines:\n' "$label" >&2
        tail -n 20 "$LOG" >&2
        exit "$rc"
    fi
}

# Desktop sessions on the guest start these services through systemd or
# OpenRC; both are best effort because some guests use another init.
enable_service() {
    name=$1
    if command -v systemctl >/dev/null 2>&1; then
        $SUDO systemctl enable --now "$name" >>"$LOG" 2>&1 || true
    elif command -v rc-update >/dev/null 2>&1; then
        $SUDO rc-update add "${name%.service}" default >>"$LOG" 2>&1 || true
        $SUDO rc-service "${name%.service}" start >>"$LOG" 2>&1 || true
    fi
}

# Optional packages differ between distributions and releases. Install them
# one by one so a missing package does not abort the whole installation.
install_optional() {
    for package in "$@"; do
        if ! install_one "$package" >>"$LOG" 2>&1; then
            warn "$package is not available in the configured repositories; skipping it."
        fi
    done
}

if command -v apt-get >/dev/null 2>&1; then
    install_one() {
        $SUDO env DEBIAN_FRONTEND=noninteractive apt-get -qq -y install --no-install-recommends "$1"
    }
    run_quiet "Updating package lists" $SUDO apt-get -qq update
    run_quiet "Installing guest packages" $SUDO env DEBIAN_FRONTEND=noninteractive apt-get -qq -y install \
        --no-install-recommends qemu-guest-agent spice-vdagent mesa-utils libgl1-mesa-dri
    install_optional spice-webdavd mesa-vulkan-drivers
    enable_service spice-webdavd.service
elif command -v dnf >/dev/null 2>&1; then
    install_one() {
        $SUDO dnf -q -y install "$1"
    }
    run_quiet "Installing guest packages" $SUDO dnf -q -y install \
        qemu-guest-agent spice-vdagent mesa-dri-drivers glx-utils
    install_optional spice-webdavd mesa-vulkan-drivers
    enable_service spice-webdavd.service
elif command -v yum >/dev/null 2>&1; then
    install_one() {
        $SUDO yum -q -y install "$1"
    }
    run_quiet "Installing guest packages" $SUDO yum -q -y install \
        qemu-guest-agent spice-vdagent mesa-dri-drivers glx-utils
    install_optional spice-webdavd mesa-vulkan-drivers
    enable_service spice-webdavd.service
elif command -v pacman >/dev/null 2>&1; then
    install_one() {
        $SUDO pacman -S --needed --noconfirm "$1"
    }
    run_quiet "Installing guest packages" $SUDO pacman -S --needed --noconfirm \
        qemu-guest-agent spice-vdagent mesa mesa-utils vulkan-icd-loader
    if pacman -Si spice-webdavd >/dev/null 2>&1; then
        run_quiet "Installing spice-webdavd" $SUDO pacman -S --needed --noconfirm spice-webdavd
    else
        warn 'spice-webdavd is not in the configured Arch repositories; install it from the AUR for file clipboard support.'
    fi
    enable_service spice-webdavd.service
elif command -v zypper >/dev/null 2>&1; then
    install_one() {
        $SUDO zypper --non-interactive --quiet install "$1"
    }
    run_quiet "Installing guest packages" $SUDO zypper --non-interactive --quiet install \
        qemu-guest-agent spice-vdagent Mesa Mesa-dri
    install_optional spice-webdavd Mesa-libvulkan1
    enable_service spice-webdavd.service
elif command -v apk >/dev/null 2>&1; then
    install_one() {
        $SUDO apk add --quiet "$1"
    }
    run_quiet "Installing guest packages" $SUDO apk add --quiet \
        qemu-guest-agent spice-vdagent mesa mesa-utils
    install_optional spice-webdavd
    enable_service spice-webdavd
else
    say 'No supported package manager found.' >&2
    exit 2
fi

enable_service qemu-guest-agent
enable_service spice-vdagentd
enable_service spice-vdagent.service
enable_service serial-getty@ttyS0.service

say ''
say 'Guest tools installed.'
say 'Reboot, or log out and back in, to start the agents.'
say "Package manager log: $LOG"
