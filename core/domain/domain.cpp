// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "domain/domain.h"

#include <array>

#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QSaveFile>
#include <QString>

#include "domain/domainconfig.h"
#include "karton_debug.h"

Domain::Domain(QObject *parent)
    : QObject(parent)
    , m_domainPtr(nullptr)
    , m_config(new DomainConfig(this))
{
    connect(m_config, &DomainConfig::isActiveChanged, this, &Domain::isActiveChanged);
    connect(m_config, &DomainConfig::isPausedChanged, this, &Domain::isPausedChanged);
    connect(m_config, &DomainConfig::hasSavedStateChanged, this, &Domain::hasSavedStateChanged);
    connect(m_config, &DomainConfig::stateChanged, this, &Domain::stateChanged);
    connect(m_config, &DomainConfig::ramUsageChanged, this, &Domain::ramUsageChanged);
    connect(m_config, &DomainConfig::autostartChanged, this, &Domain::autostartChanged);
    m_previewFrame.load(m_config->screenshotPreviewPath());
}

Domain::Domain(const virDomainPtr domainPtr, DomainConfig *config, QObject *parent)
    : QObject(parent)
    , m_domainPtr(domainPtr)
    , m_config(config)
{
    Q_ASSERT(m_config);
    m_config->setParent(this);

    connect(m_config, &DomainConfig::isActiveChanged, this, &Domain::isActiveChanged);
    connect(m_config, &DomainConfig::isPausedChanged, this, &Domain::isPausedChanged);
    connect(m_config, &DomainConfig::hasSavedStateChanged, this, &Domain::hasSavedStateChanged);
    connect(m_config, &DomainConfig::stateChanged, this, &Domain::stateChanged);
    connect(m_config, &DomainConfig::ramUsageChanged, this, &Domain::ramUsageChanged);
    connect(m_config, &DomainConfig::autostartChanged, this, &Domain::autostartChanged);
    m_previewFrame.load(m_config->screenshotPreviewPath());
}

Domain::~Domain()
{
    if (m_domainPtr) {
        virDomainFree(m_domainPtr);
    }
}

void Domain::setDomainPtr(virDomainPtr domainPtr)
{
    if (m_domainPtr == domainPtr) {
        return;
    }
    if (m_domainPtr) {
        virDomainFree(m_domainPtr);
    }
    m_domainPtr = domainPtr;
}

void Domain::apply(const DomainConfig::DomainConfigData &data)
{
    m_config->apply(data);
}

void Domain::setActive(bool active)
{
    m_config->setActive(active);
}

void Domain::setPaused(bool paused)
{
    m_config->setPaused(paused);
}

void Domain::setHasSavedState(bool hasSavedState)
{
    m_config->setHasSavedState(hasSavedState);
}

void Domain::setState(const QString &state)
{
    m_config->setState(state);
}

void Domain::setRamUsage(int ramUsage)
{
    m_config->setRamUsage(ramUsage);
}

void Domain::setAutostart(bool autostart)
{
    m_config->setAutostart(autostart);
}

QString Domain::uuidString(virDomainPtr domainPtr)
{
    if (!domainPtr) {
        return {};
    }

    std::array<char, VIR_UUID_STRING_BUFLEN> uuid = {};
    if (virDomainGetUUIDString(domainPtr, uuid.data()) == -1) {
        qCWarning(KARTON_DEBUG) << "Failed to get UUID string for" << domainPtr;
        return {};
    }
    return QString::fromUtf8(uuid.data());
}

void Domain::savePreviewFrame(QImage frame)
{
    qCDebug(KARTON_DEBUG) << "Saving preview to:" << m_config->screenshotPreviewPath();

    const QString previewPath = m_config->screenshotPreviewPath();
    QDir previewDir = QFileInfo(previewPath).dir();
    if (!previewDir.exists() && !previewDir.mkpath(QStringLiteral("."))) {
        qCWarning(KARTON_DEBUG) << "Failed to create preview directory:" << previewDir.absolutePath();
        return;
    }

    // QQuickItemGrabResult can be premultiplied/have alpha; compositing it
    // onto black first keeps its colours identical to the old virsh preview.
    if (frame.format() != QImage::Format_RGB32) {
        QImage opaque(frame.size(), QImage::Format_RGB32);
        opaque.fill(Qt::black);
        QPainter painter(&opaque);
        painter.drawImage(0, 0, frame);
        painter.end();
        frame = opaque;
    }

    if (frame.width() > 640) {
        frame = frame.scaledToWidth(640, Qt::SmoothTransformation);
    }

    QSaveFile previewFile(previewPath);
    if (!previewFile.open(QIODevice::WriteOnly) || !frame.save(&previewFile, "PNG") || !previewFile.commit()) {
        qCWarning(KARTON_DEBUG) << "Failed to save preview image:" << previewPath;
        return;
    }

    m_previewFrame = frame;
    Q_EMIT previewFrameChanged();
    Q_EMIT previewChanged();
}
