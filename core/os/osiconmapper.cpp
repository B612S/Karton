// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "os/osiconmapper.h"

namespace
{
QString normalized(const QString &shortOsId, const QString &osId)
{
    return (shortOsId + QLatin1Char(' ') + osId).toLower();
}

bool isOldWindows(const QString &id)
{
    return id.contains(QStringLiteral("winxp")) || id.contains(QStringLiteral("win2000")) || id.contains(QStringLiteral("win2k3"))
        || id.contains(QStringLiteral("win2003")) || id.contains(QStringLiteral("winvista")) || id.contains(QStringLiteral("win7"))
        || id.contains(QStringLiteral("win8")) || id.contains(QStringLiteral("winnt")) || id.contains(QStringLiteral("windows xp"))
        || id.contains(QStringLiteral("windows 7")) || id.contains(QStringLiteral("windows vista")) || id.contains(QStringLiteral("win/7"))
        || id.contains(QStringLiteral("win/xp")) || id.contains(QStringLiteral("win/vista")) || id.contains(QStringLiteral("win/8"))
        || id.contains(QStringLiteral("win/2000")) || id.contains(QStringLiteral("win/2003")) || id.trimmed() == QStringLiteral("win2k");
}

QString windowsName(const QString &id)
{
    if (id.contains(QStringLiteral("win11")) || id.contains(QStringLiteral("win/11")) || id.contains(QStringLiteral("windows 11"))) {
        return QStringLiteral("Windows 11");
    }
    if (id.contains(QStringLiteral("win10")) || id.contains(QStringLiteral("win/10")) || id.contains(QStringLiteral("windows 10"))) {
        return QStringLiteral("Windows 10");
    }
    if (id.contains(QStringLiteral("win81")) || id.contains(QStringLiteral("win/8.1")) || id.contains(QStringLiteral("windows 8.1"))) {
        return QStringLiteral("Windows 8.1");
    }
    if (id.contains(QStringLiteral("win8")) || id.contains(QStringLiteral("win/8")) || id.contains(QStringLiteral("windows 8"))) {
        return QStringLiteral("Windows 8");
    }
    if (id.contains(QStringLiteral("win7")) || id.contains(QStringLiteral("win/7")) || id.contains(QStringLiteral("windows 7"))) {
        return QStringLiteral("Windows 7");
    }
    if (id.contains(QStringLiteral("winxp")) || id.contains(QStringLiteral("win/xp")) || id.contains(QStringLiteral("windows xp"))) {
        return QStringLiteral("Windows XP");
    }
    if (id.contains(QStringLiteral("winvista")) || id.contains(QStringLiteral("win/vista")) || id.contains(QStringLiteral("windows vista"))) {
        return QStringLiteral("Windows Vista");
    }
    if (id.contains(QStringLiteral("win2003"))) {
        return QStringLiteral("Windows Server 2003");
    }
    if (id.contains(QStringLiteral("win2000")) || id.contains(QStringLiteral("win2k"))) {
        return QStringLiteral("Windows 2000");
    }
    return QStringLiteral("Windows");
}

QString linuxDistroName(const QString &id)
{
    struct Entry {
        const char *needle;
        const char *name;
    };
    static const Entry entries[] = {
        {"astra", "Astra Linux"},
        {"archlinux", "Arch Linux"},
        {"arch", "Arch Linux"},
        {"ubuntu", "Ubuntu"},
        {"debian", "Debian"},
        {"fedora", "Fedora"},
        {"opensuse", "openSUSE"},
        {"suse", "SUSE"},
        {"rhel", "Red Hat Enterprise Linux"},
        {"centos", "CentOS"},
        {"gentoo", "Gentoo"},
        {"nixos", "NixOS"},
        {"void", "Void Linux"},
        {"alpine", "Alpine Linux"},
        {"manjaro", "Manjaro"},
        {"linuxmint", "Linux Mint"},
        {"mint", "Linux Mint"},
        {"kali", "Kali Linux"},
        {"elementary", "elementary OS"},
        {"popos", "Pop!_OS"},
        {"android", "Android"},
    };
    for (const auto &entry : entries) {
        if (id.contains(QString::fromLatin1(entry.needle))) {
            return QString::fromUtf8(entry.name);
        }
    }
    return QStringLiteral("Linux");
}
} // namespace

QString OsIconMapper::iconName(const QString &shortOsId, const QString &osId)
{
    const QString id = normalized(shortOsId, osId);
    if (id.trimmed().isEmpty()) {
        return QStringLiteral("computer-symbolic");
    }
    if (id.contains(QStringLiteral("darwin")) || id.contains(QStringLiteral("macos")) || id.contains(QStringLiteral("mac os"))) {
        return QStringLiteral("karton-os-macos");
    }
    if (id.contains(QStringLiteral("win"))) {
        return isOldWindows(id) ? QStringLiteral("karton-os-windows-legacy") : QStringLiteral("karton-os-windows");
    }
    if (id.contains(QStringLiteral("freebsd"))) {
        return QStringLiteral("karton-os-freebsd");
    }
    if (id.contains(QStringLiteral("openbsd"))) {
        return QStringLiteral("karton-os-openbsd");
    }
    if (id.contains(QStringLiteral("netbsd"))) {
        return QStringLiteral("karton-os-netbsd");
    }
    if (id.contains(QStringLiteral("dragonfly"))) {
        return QStringLiteral("karton-os-dragonflybsd");
    }
    if (id.contains(QStringLiteral("freedos")) || id.contains(QStringLiteral("ms-dos")) || id.contains(QStringLiteral("msdos"))) {
        return QStringLiteral("karton-os-dos");
    }
    if (id.contains(QStringLiteral("linux")) || id.contains(QStringLiteral("ubuntu")) || id.contains(QStringLiteral("debian"))
        || id.contains(QStringLiteral("fedora")) || id.contains(QStringLiteral("arch")) || id.contains(QStringLiteral("suse"))
        || id.contains(QStringLiteral("centos")) || id.contains(QStringLiteral("rhel")) || id.contains(QStringLiteral("gentoo"))
        || id.contains(QStringLiteral("nixos")) || id.contains(QStringLiteral("alpine")) || id.contains(QStringLiteral("manjaro"))
        || id.contains(QStringLiteral("mint")) || id.contains(QStringLiteral("kali")) || id.contains(QStringLiteral("void"))) {
        return QStringLiteral("karton-os-linux");
    }
    return QStringLiteral("computer-symbolic");
}

QString OsIconMapper::displayName(const QString &shortOsId, const QString &osId)
{
    const QString id = normalized(shortOsId, osId);
    if (id.trimmed().isEmpty()) {
        return QString();
    }
    if (id.contains(QStringLiteral("darwin")) || id.contains(QStringLiteral("macos")) || id.contains(QStringLiteral("mac os"))) {
        return QStringLiteral("macOS");
    }
    if (id.contains(QStringLiteral("win"))) {
        return windowsName(id);
    }
    if (id.contains(QStringLiteral("freebsd"))) {
        return QStringLiteral("FreeBSD");
    }
    if (id.contains(QStringLiteral("openbsd"))) {
        return QStringLiteral("OpenBSD");
    }
    if (id.contains(QStringLiteral("netbsd"))) {
        return QStringLiteral("NetBSD");
    }
    if (id.contains(QStringLiteral("dragonfly"))) {
        return QStringLiteral("DragonFly BSD");
    }
    if (id.contains(QStringLiteral("freedos"))) {
        return QStringLiteral("FreeDOS");
    }
    if (id.contains(QStringLiteral("ms-dos")) || id.contains(QStringLiteral("msdos"))) {
        return QStringLiteral("MS-DOS");
    }
    if (id.contains(QStringLiteral("linux")) || id.contains(QStringLiteral("ubuntu")) || id.contains(QStringLiteral("debian"))
        || id.contains(QStringLiteral("fedora")) || id.contains(QStringLiteral("arch")) || id.contains(QStringLiteral("suse"))
        || id.contains(QStringLiteral("centos")) || id.contains(QStringLiteral("rhel")) || id.contains(QStringLiteral("gentoo"))
        || id.contains(QStringLiteral("nixos")) || id.contains(QStringLiteral("alpine")) || id.contains(QStringLiteral("manjaro"))
        || id.contains(QStringLiteral("mint")) || id.contains(QStringLiteral("kali")) || id.contains(QStringLiteral("void"))) {
        return linuxDistroName(id);
    }
    if (!shortOsId.isEmpty()) {
        return shortOsId;
    }
    return osId;
}
