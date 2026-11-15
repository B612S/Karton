// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "domain/domain.h"

#include <array>

#include <QDir>
#include <QFileInfo>
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
    connect(m_config, &DomainConfig::stateChanged, this, &Domain::stateChanged);
    connect(m_config, &DomainConfig::ramUsageChanged, this, &Domain::ramUsageChanged);
    connect(m_config, &DomainConfig::autostartChanged, this, &Domain::autostartChanged);
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
    connect(m_config, &DomainConfig::stateChanged, this, &Domain::stateChanged);
    connect(m_config, &DomainConfig::ramUsageChanged, this, &Domain::ramUsageChanged);
    connect(m_config, &DomainConfig::autostartChanged, this, &Domain::autostartChanged);
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

    if (!frame.save(previewPath)) {
        qCWarning(KARTON_DEBUG) << "Failed to save preview image:" << previewPath;
        return;
    }

    Q_EMIT previewChanged();
}
