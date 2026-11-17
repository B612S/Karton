// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include <QtTest>

#include "os/osiconmapper.h"

class OsIconMapperTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void windowsModern();
    void windowsLegacy();
    void windowsServerModern();
    void linuxFamily();
    void bsdFamily();
    void macAndDos();
    void fallback();
};

void OsIconMapperTest::windowsModern()
{
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("win11")), QStringLiteral("karton-os-windows"));
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("win10")), QStringLiteral("karton-os-windows"));
    QCOMPARE(OsIconMapper::displayName(QStringLiteral("win11")), QStringLiteral("Windows 11"));
    QCOMPARE(OsIconMapper::displayName(QStringLiteral("win10")), QStringLiteral("Windows 10"));
    QCOMPARE(OsIconMapper::displayName(QString(), QStringLiteral("http://microsoft.com/win/11")), QStringLiteral("Windows 11"));
}

void OsIconMapperTest::windowsLegacy()
{
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("winxp")), QStringLiteral("karton-os-windows-legacy"));
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("win7")), QStringLiteral("karton-os-windows-legacy"));
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("win8.1")), QStringLiteral("karton-os-windows-legacy"));
    QCOMPARE(OsIconMapper::displayName(QStringLiteral("winxp")), QStringLiteral("Windows XP"));
    QCOMPARE(OsIconMapper::displayName(QStringLiteral("win7")), QStringLiteral("Windows 7"));
}

void OsIconMapperTest::windowsServerModern()
{
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("win2k19")), QStringLiteral("karton-os-windows"));
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("win2k25")), QStringLiteral("karton-os-windows"));
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("win2k8")), QStringLiteral("karton-os-windows"));
}

void OsIconMapperTest::linuxFamily()
{
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("archlinux")), QStringLiteral("karton-os-linux"));
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("ubuntu24.04")), QStringLiteral("karton-os-linux"));
    QCOMPARE(OsIconMapper::displayName(QStringLiteral("archlinux")), QStringLiteral("Arch Linux"));
    QCOMPARE(OsIconMapper::displayName(QStringLiteral("ubuntu24.04")), QStringLiteral("Ubuntu"));
    QCOMPARE(OsIconMapper::displayName(QStringLiteral("astra-linux")), QStringLiteral("Astra Linux"));
}

void OsIconMapperTest::bsdFamily()
{
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("freebsd")), QStringLiteral("karton-os-freebsd"));
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("openbsd")), QStringLiteral("karton-os-openbsd"));
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("netbsd")), QStringLiteral("karton-os-netbsd"));
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("dragonflybsd")), QStringLiteral("karton-os-dragonflybsd"));
    QCOMPARE(OsIconMapper::displayName(QStringLiteral("freebsd")), QStringLiteral("FreeBSD"));
}

void OsIconMapperTest::macAndDos()
{
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("darwin")), QStringLiteral("karton-os-macos"));
    QCOMPARE(OsIconMapper::displayName(QStringLiteral("darwin")), QStringLiteral("macOS"));
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("freedos")), QStringLiteral("karton-os-dos"));
    QCOMPARE(OsIconMapper::displayName(QStringLiteral("msdos")), QStringLiteral("MS-DOS"));
}

void OsIconMapperTest::fallback()
{
    QCOMPARE(OsIconMapper::iconName(QString()), QStringLiteral("computer-symbolic"));
    QCOMPARE(OsIconMapper::displayName(QString()), QString());
    QCOMPARE(OsIconMapper::iconName(QStringLiteral("unknown-os")), QStringLiteral("computer-symbolic"));
}

QTEST_GUILESS_MAIN(OsIconMapperTest)

#include "tst_osiconmapper.moc"
