// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <KLocalizedString>
#include <QAbstractSocket>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSGRendererInterface>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

#include "domain/domain.h"
#include "domain/domainconfig.h"
#include "domain/domainxmlbuilder.h"
#include "domain/domainxmlreader.h"
#include "karton_debug.h"
#include "libvirt/libvirteventloop.h"
#include "libvirt/libvirtmonitor.h"
#include "os/osiconmapper.h"

using namespace KartonCommon;

QVariantMap Karton::mainWindowSize() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    QVariantMap size;
    const int width = settings.value(QStringLiteral("window/mainWidth"), 0).toInt();
    const int height = settings.value(QStringLiteral("window/mainHeight"), 0).toInt();
    if (width > 0 && height > 0) {
        size.insert(QStringLiteral("width"), width);
        size.insert(QStringLiteral("height"), height);
    }
    return size;
}

void Karton::saveMainWindowSize(int width, int height)
{
    if (width < 320 || height < 240) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("window/mainWidth"), width);
    settings.setValue(QStringLiteral("window/mainHeight"), height);
}

QVariantMap Karton::viewerWindowSize(const Domain *domain) const
{
    QVariantMap size;
    if (!domain) {
        return size;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("window/viewer/%1/").arg(domain->config()->uuid());
    const int width = settings.value(prefix + QStringLiteral("width"), 0).toInt();
    const int height = settings.value(prefix + QStringLiteral("height"), 0).toInt();
    if (width > 0 && height > 0) {
        size.insert(QStringLiteral("width"), width);
        size.insert(QStringLiteral("height"), height);
    }
    return size;
}

void Karton::saveViewerWindowSize(const Domain *domain, int width, int height)
{
    if (!domain || width < 320 || height < 240) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("window/viewer/%1/").arg(domain->config()->uuid());
    settings.setValue(prefix + QStringLiteral("width"), width);
    settings.setValue(prefix + QStringLiteral("height"), height);
}

int Karton::clipboardTextMode() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return qBound(0, settings.value(QStringLiteral("clipboard/textMode"), 3).toInt(), 3);
}

void Karton::saveClipboardTextMode(int mode)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("clipboard/textMode"), qBound(0, mode, 3));
    Q_EMIT clipboardSettingsChanged();
}

int Karton::clipboardFilesMode() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return qBound(0, settings.value(QStringLiteral("clipboard/filesMode"), 0).toInt(), 3);
}

void Karton::saveClipboardFilesMode(int mode)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("clipboard/filesMode"), qBound(0, mode, 3));
    Q_EMIT clipboardSettingsChanged();
}

bool Karton::spiceTlsEnabled() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("spice/tls"), true).toBool();
}

void Karton::saveSpiceTlsEnabled(bool enabled)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("spice/tls"), enabled);
}

QString Karton::spiceCaCertificate() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("spice/caCertificate")).toString();
}

void Karton::saveSpiceCaCertificate(const QString &path)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("spice/caCertificate"), path);
}

bool Karton::audioPlaybackEnabled(const Domain *domain) const
{
    if (!domain) {
        return true;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("audio/%1/playback").arg(domain->config()->uuid()), true).toBool();
}

void Karton::saveAudioPlaybackEnabled(const Domain *domain, bool enabled)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("audio/%1/playback").arg(domain->config()->uuid()), enabled);
}

bool Karton::microphoneEnabled(const Domain *domain) const
{
    if (!domain) {
        return false;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("audio/%1/microphone").arg(domain->config()->uuid()), false).toBool();
}

void Karton::saveMicrophoneEnabled(const Domain *domain, bool enabled)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("audio/%1/microphone").arg(domain->config()->uuid()), enabled);
}

QVariantMap Karton::domainDisplayResolution(const Domain *domain) const
{
    QVariantMap resolution;
    if (!domain) {
        return resolution;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("display/%1/").arg(domain->config()->uuid());
    const int width = settings.value(prefix + QStringLiteral("width"), 0).toInt();
    const int height = settings.value(prefix + QStringLiteral("height"), 0).toInt();
    if (width > 0 && height > 0) {
        resolution.insert(QStringLiteral("width"), width);
        resolution.insert(QStringLiteral("height"), height);
    }
    return resolution;
}

void Karton::saveDomainDisplayResolution(const Domain *domain, int width, int height)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("display/%1/").arg(domain->config()->uuid());
    const int oldWidth = settings.value(prefix + QStringLiteral("width"), 0).toInt();
    const int oldHeight = settings.value(prefix + QStringLiteral("height"), 0).toInt();
    if (width <= 0 || height <= 0) {
        if (oldWidth == 0 && oldHeight == 0) {
            return;
        }
        settings.remove(prefix + QStringLiteral("width"));
        settings.remove(prefix + QStringLiteral("height"));
    } else {
        if (oldWidth == width && oldHeight == height) {
            return;
        }
        settings.setValue(prefix + QStringLiteral("width"), width);
        settings.setValue(prefix + QStringLiteral("height"), height);
    }
    // An open viewer must pick the new mode up without being reopened.
    Q_EMIT displayResolutionChanged(domain);
}
