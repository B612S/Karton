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

static QString normaliseDiskFormat(const QString &format)
{
    QString lower = format.trimmed().toLower();
    if (lower == QStringLiteral("raw") || lower == QStringLiteral("vmdk") || lower == QStringLiteral("qcow2")) {
        return lower;
    }
    return QStringLiteral("qcow2");
}

static QString nextDiskTarget(const Domain *domain, const QString &prefix)
{
    if (!domain || !domain->domainPtr()) {
        return {};
    }

    QSet<QString> usedTargets;
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (xml) {
        QDomDocument document;
        if (document.setContent(QString::fromUtf8(xml))) {
            const QDomNodeList disks = document.elementsByTagName(QStringLiteral("disk"));
            for (int i = 0; i < disks.count(); ++i) {
                const QDomElement target = disks.at(i).toElement().firstChildElement(QStringLiteral("target"));
                if (!target.isNull()) {
                    usedTargets.insert(target.attribute(QStringLiteral("dev")));
                }
            }
        }
        free(xml);
    }

    for (char letter = 'b'; letter <= 'z'; ++letter) {
        const QString candidate = prefix + QLatin1Char(letter);
        if (!usedTargets.contains(candidate)) {
            return candidate;
        }
    }
    return {};
}

static QString diskXmlForTarget(virDomainPtr domain, const QString &targetDev, unsigned int xmlFlags)
{
    if (!domain || targetDev.isEmpty()) {
        return {};
    }

    char *xml = virDomainGetXMLDesc(domain, xmlFlags);
    if (!xml) {
        return {};
    }

    QString deviceXml;
    QDomDocument document;
    if (document.setContent(QString::fromUtf8(xml))) {
        const QDomNodeList disks = document.elementsByTagName(QStringLiteral("disk"));
        for (int i = 0; i < disks.count(); ++i) {
            const QDomElement element = disks.at(i).toElement();
            const QDomElement target = element.firstChildElement(QStringLiteral("target"));
            if (target.attribute(QStringLiteral("dev")) == targetDev) {
                QDomDocument deviceDocument;
                deviceDocument.appendChild(deviceDocument.importNode(element, true));
                deviceXml = deviceDocument.toString();
                break;
            }
        }
    }

    free(xml);
    return deviceXml;
}

QVariantList Karton::domainDisks(const Domain *domain) const
{
    QVariantList result;
    if (!domain || !domain->domainPtr()) {
        return result;
    }

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xml) {
        return result;
    }

    QDomDocument document;
    if (document.setContent(QString::fromUtf8(xml))) {
        const QDomNodeList disks = document.elementsByTagName(QStringLiteral("disk"));
        for (int i = 0; i < disks.count(); ++i) {
            const QDomElement element = disks.at(i).toElement();
            const QDomElement source = element.firstChildElement(QStringLiteral("source"));
            const QDomElement target = element.firstChildElement(QStringLiteral("target"));
            const QDomElement driver = element.firstChildElement(QStringLiteral("driver"));
            const QString sourcePath = source.attribute(QStringLiteral("file"));

            QVariantMap info;
            info.insert(QStringLiteral("device"), element.attribute(QStringLiteral("device")));
            info.insert(QStringLiteral("type"), element.attribute(QStringLiteral("type")));
            info.insert(QStringLiteral("target"), target.attribute(QStringLiteral("dev")));
            info.insert(QStringLiteral("bus"), target.attribute(QStringLiteral("bus")));
            info.insert(QStringLiteral("source"), sourcePath.isEmpty() ? source.attribute(QStringLiteral("dev")) : sourcePath);
            info.insert(QStringLiteral("format"), driver.attribute(QStringLiteral("type")));
            info.insert(QStringLiteral("readonly"), !element.firstChildElement(QStringLiteral("readonly")).isNull());
            result.append(info);
        }
    }

    free(xml);
    return result;
}

bool Karton::addDisk(const Domain *domain, int sizeGB, const QString &format)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (sizeGB < 1 || sizeGB > 2048) {
        Q_EMIT errorOccurred(i18n("Disk size must be between 1 and 2048 GB."));
        return false;
    }

    const QString diskFormat = normaliseDiskFormat(format);
    const QString targetDev = nextDiskTarget(domain, QStringLiteral("vd"));
    if (targetDev.isEmpty()) {
        Q_EMIT errorOccurred(i18n("No free virtio disk target is available."));
        return false;
    }

    const QString domainName = domain->config()->name();
    const QString path = getVirtualDiskPath(domainName + QLatin1Char('-') + targetDev, diskFormat);
    if (QFile::exists(path)) {
        Q_EMIT errorOccurred(i18nc("%1 is a file path", "The disk path already exists: %1", path));
        return false;
    }

    QString output;
    if (!m_commandRunner->runCommandAndWait(QStringLiteral("qemu-img"),
                                            {QStringLiteral("create"), QStringLiteral("-f"), diskFormat, path, QStringLiteral("%1G").arg(sizeGB)},
                                            &output)) {
        Q_EMIT errorOccurred(i18nc("%1 is a disk path, %2 is the error", "Failed to create disk %1: %2", path, output));
        return false;
    }

    const QString xml = QStringLiteral(
                            "<disk type='file' device='disk'>"
                            "<driver name='qemu' type='%1'/>"
                            "<source file='%2'/>"
                            "<target dev='%3' bus='virtio'/>"
                            "</disk>")
                            .arg(diskFormat, path, targetDev);

    bool success = false;
    if (domain->isActive()) {
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            Q_EMIT errorOccurred(
                i18nc("%1 is a disk path, %2 is the error", "Failed to attach disk %1: %2", path, QString::fromUtf8(virGetLastErrorMessage())));
        } else {
            success = true;
        }
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to persist disk:" << virGetLastErrorMessage();
        }
    } else if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) == 0) {
        success = true;
    } else {
        Q_EMIT errorOccurred(i18nc("%1 is a disk path, %2 is the error", "Failed to attach disk %1: %2", path, QString::fromUtf8(virGetLastErrorMessage())));
    }

    if (!success) {
        QFile::remove(path);
        return false;
    }

    refreshDomain(domain->domainPtr());
    return true;
}

bool Karton::attachDiskImage(const Domain *domain, const QString &path, const QString &format)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (!QFileInfo::exists(path)) {
        Q_EMIT errorOccurred(i18nc("%1 is a file path", "Disk image does not exist: %1", path));
        return false;
    }

    const QString diskFormat = normaliseDiskFormat(format);
    const QString targetDev = nextDiskTarget(domain, QStringLiteral("vd"));
    if (targetDev.isEmpty()) {
        Q_EMIT errorOccurred(i18n("No free virtio disk target is available."));
        return false;
    }

    const QString xml = QStringLiteral(
                            "<disk type='file' device='disk'>"
                            "<driver name='qemu' type='%1'/>"
                            "<source file='%2'/>"
                            "<target dev='%3' bus='virtio'/>"
                            "</disk>")
                            .arg(diskFormat, path, targetDev);

    bool success = false;
    if (domain->isActive()) {
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            Q_EMIT errorOccurred(
                i18nc("%1 is a disk path, %2 is the error", "Failed to attach disk %1: %2", path, QString::fromUtf8(virGetLastErrorMessage())));
        } else {
            success = true;
        }
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to persist disk:" << virGetLastErrorMessage();
        }
    } else if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) == 0) {
        success = true;
    } else {
        Q_EMIT errorOccurred(i18nc("%1 is a disk path, %2 is the error", "Failed to attach disk %1: %2", path, QString::fromUtf8(virGetLastErrorMessage())));
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

bool Karton::attachIso(const Domain *domain, const QString &path)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (!QFileInfo::exists(path)) {
        Q_EMIT errorOccurred(i18nc("%1 is a file path", "ISO image does not exist: %1", path));
        return false;
    }

    const QString targetDev = nextDiskTarget(domain, QStringLiteral("sd"));
    if (targetDev.isEmpty()) {
        Q_EMIT errorOccurred(i18n("No free SATA target is available for the ISO."));
        return false;
    }

    const QString xml = QStringLiteral(
                            "<disk type='file' device='cdrom'>"
                            "<driver name='qemu' type='raw'/>"
                            "<source file='%1'/>"
                            "<target dev='%2' bus='sata'/>"
                            "<readonly/>"
                            "</disk>")
                            .arg(path, targetDev);

    bool success = false;
    if (domain->isActive()) {
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            Q_EMIT errorOccurred(i18nc("%1 is an ISO path, %2 is the error", "Failed to attach ISO %1: %2", path, QString::fromUtf8(virGetLastErrorMessage())));
        } else {
            success = true;
        }
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to persist ISO:" << virGetLastErrorMessage();
        }
    } else if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) == 0) {
        success = true;
    } else {
        Q_EMIT errorOccurred(i18nc("%1 is an ISO path, %2 is the error", "Failed to attach ISO %1: %2", path, QString::fromUtf8(virGetLastErrorMessage())));
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

bool Karton::detachDisk(const Domain *domain, const QString &targetDev, bool deleteFile)
{
    if (!domain || !domain->domainPtr() || targetDev.isEmpty()) {
        return false;
    }
    if (deleteFile && domain->isActive()) {
        Q_EMIT errorOccurred(i18n("Stop the VM before deleting a disk file."));
        return false;
    }

    const QString liveXml = diskXmlForTarget(domain->domainPtr(), targetDev, 0);
    const QString configXml = diskXmlForTarget(domain->domainPtr(), targetDev, VIR_DOMAIN_XML_INACTIVE);
    if (liveXml.isEmpty() && configXml.isEmpty()) {
        Q_EMIT errorOccurred(i18nc("%1 is a disk target", "No disk with target %1 was found.", targetDev));
        return false;
    }

    QString diskSource;
    QString diskDevice;
    QDomDocument diskDocument;
    if (diskDocument.setContent(liveXml.isEmpty() ? configXml : liveXml)) {
        const QDomElement element = diskDocument.documentElement();
        diskDevice = element.attribute(QStringLiteral("device"));
        diskSource = element.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file"));
    }

    bool success = true;
    if (domain->isActive() && !liveXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), liveXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            Q_EMIT errorOccurred(
                i18nc("%1 is a disk target, %2 is the error", "Failed to detach disk %1: %2", targetDev, QString::fromUtf8(virGetLastErrorMessage())));
            success = false;
        }
    }

    if (!configXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), configXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            Q_EMIT errorOccurred(i18nc("%1 is a disk target, %2 is the error",
                                       "Failed to detach persistent disk %1: %2",
                                       targetDev,
                                       QString::fromUtf8(virGetLastErrorMessage())));
            success = false;
        }
    }

    if (success) {
        if (deleteFile && diskDevice == QStringLiteral("disk") && !diskSource.isEmpty() && QFile::exists(diskSource)) {
            if (!QFile::remove(diskSource)) {
                Q_EMIT errorOccurred(i18nc("%1 is a file path", "The disk was detached, but its file could not be removed: %1", diskSource));
                success = false;
            }
        }
        refreshDomain(domain->domainPtr());
    }
    return success;
}

QVariantMap Karton::domainDiskAdvanced(const Domain *domain, const QString &targetDev) const
{
    QVariantMap info;
    if (!domain || !domain->domainPtr() || targetDev.isEmpty()) {
        return info;
    }

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xml) {
        return info;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return info;
    }
    free(xml);

    const QDomNodeList disks = document.elementsByTagName(QStringLiteral("disk"));
    for (int i = 0; i < disks.count(); ++i) {
        const QDomElement disk = disks.at(i).toElement();
        if (disk.firstChildElement(QStringLiteral("target")).attribute(QStringLiteral("dev")) != targetDev) {
            continue;
        }
        const QDomElement driver = disk.firstChildElement(QStringLiteral("driver"));
        info.insert(QStringLiteral("readonly"), !disk.firstChildElement(QStringLiteral("readonly")).isNull());
        info.insert(QStringLiteral("shareable"), !disk.firstChildElement(QStringLiteral("shareable")).isNull());
        info.insert(QStringLiteral("serial"), disk.firstChildElement(QStringLiteral("serial")).text());
        info.insert(QStringLiteral("cache"), driver.attribute(QStringLiteral("cache")));
        info.insert(QStringLiteral("discard"), driver.attribute(QStringLiteral("discard")));
        break;
    }
    return info;
}

bool Karton::editDomainDiskAdvanced(const Domain *domain, const QString &targetDev, const QVariantMap &settings)
{
    if (!domain || !domain->domainPtr() || !m_conn || targetDev.isEmpty()) {
        return false;
    }

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        Q_EMIT errorOccurred(i18nc("%1 is the name of the domain", "Failed to read configuration for domain: %1", domain->config()->name()));
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        Q_EMIT errorOccurred(i18n("Failed to parse the virtual machine configuration."));
        return false;
    }
    free(xml);

    QDomElement selected;
    const QDomNodeList disks = document.elementsByTagName(QStringLiteral("disk"));
    for (int i = 0; i < disks.count(); ++i) {
        const QDomElement disk = disks.at(i).toElement();
        if (disk.firstChildElement(QStringLiteral("target")).attribute(QStringLiteral("dev")) == targetDev) {
            selected = disk;
            break;
        }
    }
    if (selected.isNull()) {
        Q_EMIT errorOccurred(i18nc("%1 is a disk target", "Disk %1 was not found.", targetDev));
        return false;
    }

    auto setFlag = [&document, &selected](const QString &name, bool enabled) {
        QDomElement element = selected.firstChildElement(name);
        if (enabled && element.isNull()) {
            selected.appendChild(document.createElement(name));
        } else if (!enabled && !element.isNull()) {
            selected.removeChild(element);
        }
    };
    setFlag(QStringLiteral("readonly"), settings.value(QStringLiteral("readonly")).toBool());
    setFlag(QStringLiteral("shareable"), settings.value(QStringLiteral("shareable")).toBool());

    QDomElement serial = selected.firstChildElement(QStringLiteral("serial"));
    const QString serialText = settings.value(QStringLiteral("serial")).toString();
    if (serialText.isEmpty()) {
        if (!serial.isNull()) {
            selected.removeChild(serial);
        }
    } else {
        if (serial.isNull()) {
            serial = document.createElement(QStringLiteral("serial"));
            selected.appendChild(serial);
        }
        serial.clear();
        serial.appendChild(document.createTextNode(serialText));
    }

    QDomElement driver = selected.firstChildElement(QStringLiteral("driver"));
    if (driver.isNull()) {
        driver = document.createElement(QStringLiteral("driver"));
        selected.insertBefore(driver, selected.firstChild());
    }
    const QString cache = settings.value(QStringLiteral("cache")).toString();
    if (cache.isEmpty()) {
        driver.removeAttribute(QStringLiteral("cache"));
    } else {
        driver.setAttribute(QStringLiteral("cache"), cache);
    }
    const QString discard = settings.value(QStringLiteral("discard")).toString();
    if (discard.isEmpty()) {
        driver.removeAttribute(QStringLiteral("discard"));
    } else {
        driver.setAttribute(QStringLiteral("discard"), discard);
    }

    virDomainPtr redefined = virDomainDefineXML(m_conn, document.toString().toUtf8().constData());
    if (!redefined) {
        Q_EMIT errorOccurred(i18nc("%1 is the name of the domain, %2 is the error message",
                                   "Failed to update configuration for domain %1: %2",
                                   domain->config()->name(),
                                   QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    refreshDomainList();
    return true;
}

QString Karton::getXmlConfigPath(const QString &domainName)
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return QStringLiteral("%1/libvirt/qemu/%2.xml").arg(configDir, domainName);
}

QString Karton::getVirtualDiskPath(const QString &domainName, const QString &format)
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString suffix = QStringLiteral("qcow2");
    if (format == QStringLiteral("raw")) {
        suffix = QStringLiteral("raw");
    } else if (format == QStringLiteral("vmdk")) {
        suffix = QStringLiteral("vmdk");
    }
    return QStringLiteral("%1/libvirt/images/%2.%3").arg(dataDir, domainName, suffix);
}

int Karton::diskSizeGB(const QString &path)
{
    if (path.isEmpty()) {
        return 0;
    }

    const auto cached = m_diskSizeCache.constFind(path);
    if (cached != m_diskSizeCache.cend()) {
        return *cached;
    }

    QString output;
    // -U / --force-share lets us inspect a disk image that a running VM has
    // locked for writing.
    if (!m_commandRunner->runCommandAndWait(QStringLiteral("qemu-img"),
                                            {QStringLiteral("info"), QStringLiteral("-U"), QStringLiteral("--output=json"), path},
                                            &output)) {
        return 0;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(output.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        qCWarning(KARTON_DEBUG) << "Failed to parse qemu-img output for" << path << error.errorString();
        return 0;
    }

    const qint64 bytes = document.object().value(QStringLiteral("virtual-size")).toVariant().toLongLong();
    if (bytes <= 0) {
        return 0;
    }

    const int sizeGB = static_cast<int>((bytes + (1LL << 30) - 1) / (1LL << 30));
    m_diskSizeCache.insert(path, sizeGB);
    return sizeGB;
}

bool Karton::ejectDisk(const Domain *domain)
{
    if (!domain) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (!domainPtr) {
        return false;
    }

    const bool isActive = virDomainIsActive(domainPtr) == 1;

    char *xmlDesc = virDomainGetXMLDesc(domainPtr, VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to read configuration for domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    QDomDocument document;
    const auto result = document.setContent(QString::fromUtf8(xmlDesc));
    free(xmlDesc);

    if (!result) {
        const QString errorMsg = i18nc("%1 is the name of the domain", "Failed to parse configuration for domain: %1", domain->config()->name());
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    QString targetDev;
    QString targetBus;
    QString diskType = QStringLiteral("file");

    const QDomNodeList disks = document.elementsByTagName(QStringLiteral("disk"));
    for (int i = 0; i < disks.count(); ++i) {
        const QDomElement disk = disks.at(i).toElement();
        if (disk.attribute(QStringLiteral("device")) != QStringLiteral("cdrom")) {
            continue;
        }
        diskType = disk.attribute(QStringLiteral("type"), QStringLiteral("file"));
        const QDomElement target = disk.firstChildElement(QStringLiteral("target"));
        targetDev = target.attribute(QStringLiteral("dev"));
        targetBus = target.attribute(QStringLiteral("bus"));
        break;
    }

    if (targetDev.isEmpty()) {
        const QString errorMsg = i18nc("%1 is the name of the domain", "No CD-ROM device found in domain: %1", domain->config()->name());
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    // Replaces the current device with the same drive minus its source, i.e. empty.
    DomainXmlBuilder builder;
    const QString ejectXml = builder.generateDiskXML({.diskType = diskType,
                                                      .device = QStringLiteral("cdrom"),
                                                      .driver = QStringLiteral("qemu"),
                                                      .format = QStringLiteral("raw"),
                                                      .sourcePath = QString(),
                                                      .target = targetDev,
                                                      .bus = targetBus,
                                                      .readonly = true});

    // Only affect the live domain if it's running; otherwise just update the
    // persistent config.
    unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
    if (isActive) {
        flags |= VIR_DOMAIN_AFFECT_LIVE;
    }

    if (virDomainUpdateDeviceFlags(domainPtr, ejectXml.toUtf8().constData(), flags) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to eject ISO disk from domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    qCInfo(KARTON_DEBUG) << "Successfully ejected ISO disk from domain:" << domain->config()->name();
    refreshDomainList();
    return true;
}
