// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
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
#include <QUrl>
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
#include "util/calltimer.h"

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
    ScopedCallTimer timer("Karton::domainDisks");
    QVariantList result;
    if (!domain || !domain->domainPtr()) {
        return result;
    }

    char *xml = cachedDomainXml(domain, true);
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

namespace
{
QVariantList collectDiskStats(virDomainPtr domainPtr)
{
    QVariantList result;
    if (!domainPtr) {
        return result;
    }

    const bool running = virDomainIsActive(domainPtr) == 1;
    char *xml = virDomainGetXMLDesc(domainPtr, running ? 0 : VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domainPtr, VIR_DOMAIN_XML_INACTIVE);
    }
    if (!xml) {
        return result;
    }

    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return result;
    }
    free(xml);

    const QDomNodeList disks = document.elementsByTagName(QStringLiteral("disk"));
    for (int i = 0; i < disks.count(); ++i) {
        const QDomElement element = disks.at(i).toElement();
        const QDomElement source = element.firstChildElement(QStringLiteral("source"));
        const QDomElement target = element.firstChildElement(QStringLiteral("target"));
        const QDomElement driver = element.firstChildElement(QStringLiteral("driver"));
        const QString device = element.attribute(QStringLiteral("device"));
        const QString path = source.attribute(QStringLiteral("file"));
        const bool isCdrom = device == QStringLiteral("cdrom");

        qulonglong capacity = 0;
        qulonglong allocation = 0;
        virDomainBlockInfo blockInfo = {};
        if (!path.isEmpty() && virDomainGetBlockInfo(domainPtr, path.toUtf8().constData(), &blockInfo, 0) == 0) {
            capacity = blockInfo.capacity;
            allocation = blockInfo.allocation;
        }
        const QFileInfo fileInfo(path);
        if (fileInfo.exists()) {
            if (allocation == 0) {
                allocation = static_cast<qulonglong>(fileInfo.size());
            }
            if (capacity == 0) {
                capacity = static_cast<qulonglong>(fileInfo.size());
            }
        }

        QVariantMap info;
        info.insert(QStringLiteral("device"), device);
        info.insert(QStringLiteral("type"), element.attribute(QStringLiteral("type")));
        info.insert(QStringLiteral("target"), target.attribute(QStringLiteral("dev")));
        info.insert(QStringLiteral("bus"), target.attribute(QStringLiteral("bus")));
        info.insert(QStringLiteral("source"), path.isEmpty() ? source.attribute(QStringLiteral("dev")) : path);
        info.insert(QStringLiteral("format"), driver.attribute(QStringLiteral("type")));
        info.insert(QStringLiteral("readonly"), !element.firstChildElement(QStringLiteral("readonly")).isNull());
        info.insert(QStringLiteral("isCdrom"), isCdrom);
        info.insert(QStringLiteral("hasMedia"), !path.isEmpty());
        info.insert(QStringLiteral("capacityBytes"), capacity);
        info.insert(QStringLiteral("allocationBytes"), allocation);
        info.insert(QStringLiteral("usagePercent"),
                    capacity > 0 ? qBound(0.0, static_cast<double>(allocation) * 100.0 / static_cast<double>(capacity), 100.0) : 0.0);
        result.append(info);
    }

    return result;
}
} // namespace

QVariantList Karton::domainDiskStats(const Domain *domain)
{
    ScopedCallTimer timer("Karton::domainDiskStats");
    if (!domain || !domain->domainPtr()) {
        return {};
    }
    const QString cacheKey = Domain::uuidString(domain->domainPtr());
    if (cacheKey.isEmpty()) {
        return {};
    }
    if (!readerConnectionForUuid(cacheKey)) {
        return collectDiskStats(domain->domainPtr());
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto it = m_diskStatsCache.constFind(cacheKey);
    if (it == m_diskStatsCache.cend() || now - m_diskStatsTimes.value(cacheKey, 0) > 2000) {
        requestDomainDiskStats(cacheKey);
    }
    return it == m_diskStatsCache.cend() ? QVariantList{} : *it;
}

void Karton::requestDomainDiskStats(const QString &uuid)
{
    virConnectPtr readerConn = readerConnectionForUuid(uuid);
    if (!m_reader || !readerConn || m_diskStatsPending.contains(uuid)) {
        return;
    }
    m_diskStatsPending.insert(uuid);
    postReaderJob([this, uuid, readerConn]() {
        QVariantList result;
        if (virConnectPtr conn = readerConn) {
            virDomainPtr dom = virDomainLookupByUUIDString(conn, uuid.toUtf8().constData());
            if (dom) {
                result = collectDiskStats(dom);
                virDomainFree(dom);
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, uuid, result]() {
                m_diskStatsPending.remove(uuid);
                if (domainByUuid(uuid)) {
                    m_diskStatsCache.insert(uuid, result);
                    m_diskStatsTimes.insert(uuid, QDateTime::currentMSecsSinceEpoch());
                    Q_EMIT domainDiskStatsUpdated(uuid);
                }
            },
            Qt::QueuedConnection);
    });
}

bool Karton::addDisk(const Domain *domain, int sizeGB, const QString &format, const QVariantMap &options)
{
    ScopedCallTimer timer("Karton::addDisk");
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (sizeGB < 1 || sizeGB > 2048) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "disk_size_must_be_between_1_and_2048_gb"));
        return false;
    }
    if (connectionIsRemote(domainConnectionUri(domain))) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "remote_disk_creation_is_not_supported"));
        return false;
    }

    const QString diskFormat = normaliseDiskFormat(format);

    const QString bus = options.value(QStringLiteral("bus"), QStringLiteral("virtio")).toString();
    static const QStringList supportedBuses = {QStringLiteral("virtio"), QStringLiteral("sata"), QStringLiteral("scsi"), QStringLiteral("ide")};
    if (!supportedBuses.contains(bus)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "unsupported_disk_bus_1").arg(bus));
        return false;
    }
    const QString cache = options.value(QStringLiteral("cache")).toString();
    static const QStringList cacheModes = {QStringLiteral("none"),
                                           QStringLiteral("writethrough"),
                                           QStringLiteral("writeback"),
                                           QStringLiteral("directsync"),
                                           QStringLiteral("unsafe")};
    if (!cache.isEmpty() && !cacheModes.contains(cache)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "unsupported_disk_cache_mode_1").arg(cache));
        return false;
    }
    const QString discard = options.value(QStringLiteral("discard")).toString();
    static const QStringList discardModes = {QStringLiteral("unmap"), QStringLiteral("ignore")};
    if (!discard.isEmpty() && !discardModes.contains(discard)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "unsupported_disk_discard_mode_1").arg(discard));
        return false;
    }

    QString targetPrefix = QStringLiteral("vd");
    if (bus == QStringLiteral("sata") || bus == QStringLiteral("scsi")) {
        targetPrefix = QStringLiteral("sd");
    } else if (bus == QStringLiteral("ide")) {
        targetPrefix = QStringLiteral("hd");
    }
    const QString targetDev = nextDiskTarget(domain, targetPrefix);
    if (targetDev.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "no_free_1_disk_target_is_available").arg(bus));
        return false;
    }

    const QString domainName = domain->config()->name();
    QString path;
    const QString explicitPath = options.value(QStringLiteral("path")).toString().trimmed();
    const QString directoryPath = options.value(QStringLiteral("directory")).toString().trimmed();
    const QString requestedFileName = options.value(QStringLiteral("filename")).toString().trimmed();
    if (!explicitPath.isEmpty()) {
        path = QDir::cleanPath(explicitPath);
        if (QFileInfo(path).isRelative()) {
            path = QDir::current().absoluteFilePath(path);
        }
    } else if (!directoryPath.isEmpty()) {
        if (requestedFileName.contains(QLatin1Char('/'))) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_disk_file_name_must_not_contain_a_path"));
            return false;
        }
        QDir directory(directoryPath);
        if (!directory.mkpath(QStringLiteral("."))) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_image_directory_1").arg(directoryPath));
            return false;
        }
        QString fileName = requestedFileName;
        if (fileName.isEmpty()) {
            fileName = domainName + QLatin1Char('-') + targetDev;
        }
        if (!fileName.endsWith(QLatin1Char('.') + diskFormat)) {
            fileName += QLatin1Char('.') + diskFormat;
        }
        path = directory.filePath(fileName);
    } else {
        path = getVirtualDiskPath(domainName + QLatin1Char('-') + targetDev, diskFormat);
    }
    const QFileInfo pathInfo(path);
    if (!QDir().mkpath(pathInfo.absolutePath())) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_image_directory_1").arg(pathInfo.absolutePath()));
        return false;
    }
    if (QFile::exists(path)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_disk_path_already_exists_1").arg(path));
        return false;
    }
    QString driverAttributes;
    if (!cache.isEmpty()) {
        driverAttributes += QStringLiteral(" cache='%1'").arg(cache);
    }
    if (!discard.isEmpty()) {
        driverAttributes += QStringLiteral(" discard='%1'").arg(discard);
    }

    QString xml = QStringLiteral(
                      "<disk type='file' device='disk'>"
                      "<driver name='qemu' type='%1'%2/>"
                      "<source file='%3'/>"
                      "<target dev='%4' bus='%5'/>")
                      .arg(diskFormat)
                      .arg(driverAttributes, path.toHtmlEscaped(), targetDev, bus);
    if (options.value(QStringLiteral("readonly")).toBool()) {
        xml += QStringLiteral("<readonly/>");
    }
    if (options.value(QStringLiteral("shareable")).toBool()) {
        xml += QStringLiteral("<shareable/>");
    }
    const QString serial = options.value(QStringLiteral("serial")).toString().trimmed();
    if (!serial.isEmpty()) {
        xml += QStringLiteral("<serial>%1</serial>").arg(serial.toHtmlEscaped());
    }
    xml += QStringLiteral("</disk>");

    // qemu-img create runs asynchronously: a slow image (network filesystem,
    // thinning on a huge sparse file) must not freeze the window. The dialog
    // stays open and waits for diskCreated().
    const QString connectionUri = domainConnectionUri(domain);
    const bool readonlyDisk = options.value(QStringLiteral("readonly")).toBool();
    QPointer<Domain> guard(const_cast<Domain *>(domain));
    m_commandRunner->runCommand(
        QStringLiteral("qemu-img"),
        {QStringLiteral("create"), QStringLiteral("-f"), diskFormat, path, QStringLiteral("%1G").arg(sizeGB)},
        [this, guard, path, xml, connectionUri, readonlyDisk](bool created, const QString &output) {
            if (!guard) {
                // The domain disappeared while the image was being created;
                // do not leave an orphan file behind, but still answer the
                // dialog so it cannot stay in its "creating" state forever.
                QFile::remove(path);
                const QString message = QCoreApplication::translate("Karton", "the_virtual_machine_was_removed");
                Q_EMIT diskCreated(false, path, message);
                return;
            }
            if (!created) {
                QFile::remove(path);
                const QString message = QCoreApplication::translate("Karton", "failed_to_create_disk_1_2").arg(path, output);
                qCWarning(KARTON_DEBUG) << message;
                Q_EMIT errorOccurred(message);
                Q_EMIT diskCreated(false, path, message);
                return;
            }

            const QString aclError = ensureDiskAccess(path, connectionUri, readonlyDisk);
            if (!aclError.isEmpty()) {
                QFile::remove(path);
                qCWarning(KARTON_DEBUG) << aclError;
                Q_EMIT errorOccurred(aclError);
                Q_EMIT diskCreated(false, path, aclError);
                return;
            }

            bool success = false;
            QString message;
            if (guard->isActive()) {
                if (virDomainAttachDeviceFlags(guard->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
                    message = QCoreApplication::translate("Karton", "failed_to_attach_disk_1_2").arg(path, QString::fromUtf8(virGetLastErrorMessage()));
                } else {
                    success = true;
                }
                if (virDomainAttachDeviceFlags(guard->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
                    qCWarning(KARTON_DEBUG) << "Failed to persist disk:" << virGetLastErrorMessage();
                }
            } else if (virDomainAttachDeviceFlags(guard->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) == 0) {
                success = true;
            } else {
                message = QCoreApplication::translate("Karton", "failed_to_attach_disk_1_2").arg(path, QString::fromUtf8(virGetLastErrorMessage()));
            }

            if (!success) {
                QFile::remove(path);
                qCWarning(KARTON_DEBUG) << message;
                Q_EMIT errorOccurred(message);
                Q_EMIT diskCreated(false, path, message);
                return;
            }

            refreshDomain(guard->domainPtr());
            Q_EMIT diskCreated(true, path, QString());
        },
        60000);
    return true;
}

bool Karton::attachDiskImage(const Domain *domain, const QString &path, const QString &format)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    const QString connectionUri = domainConnectionUri(domain);
    const bool remote = connectionIsRemote(connectionUri);
    const bool volumePath = !remote && isStorageVolumePath(path, connectionUri);
    if (!remote && !volumePath && !QFileInfo::exists(path)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "disk_image_does_not_exist_1").arg(path));
        return false;
    }
    const QString aclError = ensureDiskAccess(path, connectionUri);
    if (!aclError.isEmpty()) {
        Q_EMIT errorOccurred(aclError);
        return false;
    }

    const QString diskFormat = normaliseDiskFormat(format);
    const QString targetDev = nextDiskTarget(domain, QStringLiteral("vd"));
    if (targetDev.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "no_free_virtio_disk_target_is_available"));
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
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_attach_disk_1_2").arg(path).arg(QString::fromUtf8(virGetLastErrorMessage())));
        } else {
            success = true;
        }
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to persist disk:" << virGetLastErrorMessage();
        }
    } else if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) == 0) {
        success = true;
    } else {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_attach_disk_1_2").arg(path).arg(QString::fromUtf8(virGetLastErrorMessage())));
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

bool Karton::setDiskMedia(const Domain *domain, const QString &targetDev, const QString &path)
{
    if (!domain || !domain->domainPtr() || targetDev.isEmpty()) {
        return false;
    }
    const QString connectionUri = domainConnectionUri(domain);
    const bool remote = connectionIsRemote(connectionUri);
    const bool volumePath = !remote && isStorageVolumePath(path, connectionUri);
    if (!remote && !volumePath && !path.isEmpty() && !QFileInfo::exists(path)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_media_file_does_not_exist_1").arg(path));
        return false;
    }
    const QString aclError = ensureDiskAccess(path, connectionUri, true);
    if (!aclError.isEmpty()) {
        Q_EMIT errorOccurred(aclError);
        return false;
    }

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_read_configuration_for_domain_1").arg(domain->config()->name()));
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_parse_the_virtual_machine_configuration"));
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
    if (selected.isNull() || selected.attribute(QStringLiteral("device")) != QStringLiteral("cdrom")) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "cd_rom_1_was_not_found").arg(targetDev));
        return false;
    }

    QDomElement driver = selected.firstChildElement(QStringLiteral("driver"));
    if (driver.isNull()) {
        driver = document.createElement(QStringLiteral("driver"));
        selected.insertBefore(driver, selected.firstChild());
    }
    driver.setAttribute(QStringLiteral("name"), QStringLiteral("qemu"));
    driver.setAttribute(QStringLiteral("type"), QStringLiteral("raw"));

    QDomElement source = selected.firstChildElement(QStringLiteral("source"));
    while (!source.isNull()) {
        selected.removeChild(source);
        source = selected.firstChildElement(QStringLiteral("source"));
    }
    if (!path.isEmpty()) {
        source = document.createElement(QStringLiteral("source"));
        source.setAttribute(QStringLiteral("file"), QDir::cleanPath(path));
        selected.insertAfter(source, driver);
    }
    if (selected.firstChildElement(QStringLiteral("readonly")).isNull()) {
        selected.appendChild(document.createElement(QStringLiteral("readonly")));
    }

    if (domain->isActive()) {
        QDomDocument liveDocument;
        liveDocument.appendChild(liveDocument.importNode(selected, true));
        const QByteArray liveXml = liveDocument.toString().toUtf8();
        if (virDomainUpdateDeviceFlags(domain->domainPtr(), liveXml.constData(), VIR_DOMAIN_DEVICE_MODIFY_LIVE) < 0) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_change_cd_rom_media_1").arg(QString::fromUtf8(virGetLastErrorMessage())));
            return false;
        }
    }

    virDomainPtr redefined = virDomainDefineXML(virDomainGetConnect(domain->domainPtr()), document.toString().toUtf8().constData());
    if (!redefined) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_update_configuration_for_domain_1_2")
                                 .arg(domain->config()->name())
                                 .arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    refreshDomain(domain->domainPtr());
    return true;
}

bool Karton::attachIso(const Domain *domain, const QString &path)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    const QString connectionUri = domainConnectionUri(domain);
    const bool remote = connectionIsRemote(connectionUri);
    const bool volumePath = !remote && isStorageVolumePath(path, connectionUri);
    if (!remote && !volumePath && !QFileInfo::exists(path)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "iso_image_does_not_exist_1").arg(path));
        return false;
    }
    const QString aclError = ensureDiskAccess(path, connectionUri, true);
    if (!aclError.isEmpty()) {
        Q_EMIT errorOccurred(aclError);
        return false;
    }

    const QVariantList disks = domainDisks(domain);
    for (const QVariant &entry : disks) {
        const QVariantMap disk = entry.toMap();
        if (disk.value(QStringLiteral("device")).toString() == QStringLiteral("cdrom") && disk.value(QStringLiteral("source")).toString().isEmpty()) {
            return setDiskMedia(domain, disk.value(QStringLiteral("target")).toString(), path);
        }
    }

    const QString targetDev = nextDiskTarget(domain, QStringLiteral("sd"));
    if (targetDev.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "no_free_sata_target_is_available_for_the_iso"));
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
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_attach_iso_1_2").arg(path).arg(QString::fromUtf8(virGetLastErrorMessage())));
        } else {
            success = true;
        }
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to persist ISO:" << virGetLastErrorMessage();
        }
    } else if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) == 0) {
        success = true;
    } else {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_attach_iso_1_2").arg(path).arg(QString::fromUtf8(virGetLastErrorMessage())));
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
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "stop_the_vm_before_deleting_a_disk_file"));
        return false;
    }

    const QString liveXml = diskXmlForTarget(domain->domainPtr(), targetDev, 0);
    const QString configXml = diskXmlForTarget(domain->domainPtr(), targetDev, VIR_DOMAIN_XML_INACTIVE);
    if (liveXml.isEmpty() && configXml.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "no_disk_with_target_1_was_found").arg(targetDev));
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
                QCoreApplication::translate("Karton", "failed_to_detach_disk_1_2").arg(targetDev).arg(QString::fromUtf8(virGetLastErrorMessage())));
            success = false;
        }
    }

    if (!configXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), configXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            Q_EMIT errorOccurred(
                QCoreApplication::translate("Karton", "failed_to_detach_persistent_disk_1_2").arg(targetDev).arg(QString::fromUtf8(virGetLastErrorMessage())));
            success = false;
        }
    }

    if (success) {
        if (deleteFile && diskDevice == QStringLiteral("disk") && !diskSource.isEmpty()) {
            if (connectionIsRemote(domainConnectionUri(domain))) {
                virConnectPtr conn = connectionHandleForUri(domainConnectionUri(domain));
                virStorageVolPtr volume = conn ? virStorageVolLookupByPath(conn, diskSource.toUtf8().constData()) : nullptr;
                if (volume) {
                    if (virStorageVolDelete(volume, 0) != 0) {
                        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_delete_storage_volume_1_2")
                                                 .arg(diskSource, QString::fromUtf8(virGetLastErrorMessage())));
                        success = false;
                    }
                    virStorageVolFree(volume);
                }
            } else if (QFile::exists(diskSource)) {
                if (!QFile::remove(diskSource)) {
                    Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_disk_was_detached_but_its_file_could_not_be_removed_1").arg(diskSource));
                    success = false;
                }
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
        const QDomElement target = disk.firstChildElement(QStringLiteral("target"));
        info.insert(QStringLiteral("device"), disk.attribute(QStringLiteral("device")));
        info.insert(QStringLiteral("bus"), target.attribute(QStringLiteral("bus")));
        info.insert(QStringLiteral("source"), disk.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file")));
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
    if (!domain || !domain->domainPtr() || targetDev.isEmpty()) {
        return false;
    }
    const QString connectionUri = domainConnectionUri(domain);
    const bool remote = connectionIsRemote(connectionUri);

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_read_configuration_for_domain_1").arg(domain->config()->name()));
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_parse_the_virtual_machine_configuration"));
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
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "disk_1_was_not_found").arg(targetDev));
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
    const bool isCdrom = selected.attribute(QStringLiteral("device")) == QStringLiteral("cdrom");
    bool sourceChanged = false;
    if (settings.contains(QStringLiteral("source"))) {
        QString requestedSource = settings.value(QStringLiteral("source")).toString().trimmed();
        if (!requestedSource.isEmpty()) {
            requestedSource = QDir::cleanPath(requestedSource);
            const bool volumeSource = !remote && isStorageVolumePath(requestedSource, connectionUri);
            if (!remote && !volumeSource && QFileInfo(requestedSource).isRelative()) {
                requestedSource = QDir::current().absoluteFilePath(requestedSource);
            }
            if (!remote && !volumeSource && !QFileInfo::exists(requestedSource)) {
                Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_disk_image_does_not_exist_1").arg(requestedSource));
                return false;
            }
            const bool readOnlyDisk = isCdrom || settings.value(QStringLiteral("readonly")).toBool();
            const QString aclError = ensureDiskAccess(requestedSource, connectionUri, readOnlyDisk);
            if (!aclError.isEmpty()) {
                Q_EMIT errorOccurred(aclError);
                return false;
            }
        }
        const QString oldSource = selected.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file"));
        if (requestedSource != oldSource) {
            if (!isCdrom && requestedSource.isEmpty()) {
                Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "a_disk_path_cannot_be_empty"));
                return false;
            }
            QDomElement oldSourceElement = selected.firstChildElement(QStringLiteral("source"));
            while (!oldSourceElement.isNull()) {
                selected.removeChild(oldSourceElement);
                oldSourceElement = selected.firstChildElement(QStringLiteral("source"));
            }
            if (!requestedSource.isEmpty()) {
                QDomElement source = document.createElement(QStringLiteral("source"));
                source.setAttribute(QStringLiteral("file"), requestedSource);
                QDomElement driver = selected.firstChildElement(QStringLiteral("driver"));
                if (driver.isNull()) {
                    selected.appendChild(source);
                } else {
                    selected.insertAfter(source, driver);
                }
            }
            sourceChanged = true;
        }
    }

    if (isCdrom) {
        // CD-ROM media is always read-only; shareable, serial, cache and
        // discard settings do not apply to an optical drive.
        setFlag(QStringLiteral("readonly"), true);
    } else {
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
    }

    if (isCdrom && sourceChanged && domain->isActive()) {
        QDomDocument liveDocument;
        liveDocument.appendChild(liveDocument.importNode(selected, true));
        if (virDomainUpdateDeviceFlags(domain->domainPtr(), liveDocument.toString().toUtf8().constData(), VIR_DOMAIN_DEVICE_MODIFY_LIVE) < 0) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_change_cd_rom_media_1").arg(QString::fromUtf8(virGetLastErrorMessage())));
            return false;
        }
    }

    virDomainPtr redefined = virDomainDefineXML(virDomainGetConnect(domain->domainPtr()), document.toString().toUtf8().constData());
    if (!redefined) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_update_configuration_for_domain_1_2")
                                 .arg(domain->config()->name())
                                 .arg(QString::fromUtf8(virGetLastErrorMessage())));
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
    QString suffix = QStringLiteral("qcow2");
    if (format == QStringLiteral("raw")) {
        suffix = QStringLiteral("raw");
    } else if (format == QStringLiteral("vmdk")) {
        suffix = QStringLiteral("vmdk");
    }
    QDir directory(imagesDirectory());
    directory.mkpath(QStringLiteral("."));
    return directory.filePath(domainName + QLatin1Char('.') + suffix);
}

int Karton::diskSizeGB(const QString &path, bool queryIfMissing)
{
    if (path.isEmpty()) {
        return 0;
    }

    const auto cached = m_diskSizeCache.constFind(path);
    if (cached != m_diskSizeCache.cend()) {
        return *cached;
    }
    if (!queryIfMissing) {
        return 0;
    }

    QString output;
    // -U / --force-share lets us inspect a disk image that a running VM has
    // locked for writing.
    if (!m_commandRunner->runCommandAndWait(QStringLiteral("qemu-img"),
                                            {QStringLiteral("info"), QStringLiteral("-U"), QStringLiteral("--output=json"), path},
                                            &output,
                                            10000)) {
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

void Karton::scheduleDiskSizeQuery(const QString &path)
{
    if (path.isEmpty() || m_diskSizeCache.contains(path) || m_diskSizeLookups.contains(path)) {
        return;
    }
    m_diskSizeLookups.insert(path);
    // Never spawn qemu-img from the synchronous refresh: a stuck image
    // (network filesystem, stale lock) would freeze the GUI thread. The size
    // is filled in when the answer arrives and the model updates itself.
    m_commandRunner->runCommand(
        QStringLiteral("qemu-img"),
        {QStringLiteral("info"), QStringLiteral("-U"), QStringLiteral("--output=json"), path},
        [this, path](bool success, const QString &output) {
            m_diskSizeLookups.remove(path);
            if (!success) {
                return;
            }
            const QJsonDocument document = QJsonDocument::fromJson(output.toUtf8());
            const qint64 bytes = document.object().value(QStringLiteral("virtual-size")).toVariant().toLongLong();
            if (bytes <= 0) {
                return;
            }
            const int sizeGB = static_cast<int>((bytes + (1LL << 30) - 1) / (1LL << 30));
            m_diskSizeCache.insert(path, sizeGB);
            for (Domain *domain : std::as_const(m_domains)) {
                if (domain->config()->virtualDiskPaths().contains(path)) {
                    domain->config()->setMaxDiskStorage(sizeGB);
                }
            }
        },
        10000);
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
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_read_configuration_for_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    QDomDocument document;
    const auto result = document.setContent(QString::fromUtf8(xmlDesc));
    free(xmlDesc);

    if (!result) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_parse_configuration_for_domain_1").arg(domain->config()->name());
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
        const QString errorMsg = QCoreApplication::translate("Karton", "no_cd_rom_device_found_in_domain_1").arg(domain->config()->name());
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
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_eject_iso_disk_from_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    qCInfo(KARTON_DEBUG) << "Successfully ejected ISO disk from domain:" << domain->config()->name();
    refreshDomainList();
    return true;
}

bool Karton::checkDiskImage(const Domain *domain, const QString &path)
{
    Q_UNUSED(domain);
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "disk_image_does_not_exist_1").arg(path));
        return false;
    }

    Q_EMIT diskMaintenanceStarted(QStringLiteral("check"), path);
    // -U lets qemu-img inspect an image whose lock is held by a running VM;
    // check does not modify the image.
    m_commandRunner->runCommand(QStringLiteral("qemu-img"),
                                {QStringLiteral("check"), QStringLiteral("-U"), path},
                                [this, path](bool success, const QString &output) {
                                    Q_EMIT diskMaintenanceFinished(QStringLiteral("check"), path, success, output.trimmed());
                                });
    return true;
}

bool Karton::compactDiskImage(const Domain *domain, const QString &path)
{
    ScopedCallTimer timer("Karton::compactDiskImage");
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (domain->isActive()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "stop_the_vm_before_compacting_its_disk"));
        return false;
    }
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "disk_image_does_not_exist_1").arg(path));
        return false;
    }

    // Compact rewrites the image as qcow2. Converting a raw/vmdk image in
    // place would make the format in the domain XML wrong, so only qcow2 is
    // accepted.
    QString infoOutput;
    if (!m_commandRunner->runCommandAndWait(QStringLiteral("qemu-img"),
                                            {QStringLiteral("info"), QStringLiteral("-U"), QStringLiteral("--output=json"), path},
                                            &infoOutput,
                                            10000)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_read_disk_image_information_1").arg(path));
        return false;
    }
    const QString format = QJsonDocument::fromJson(infoOutput.toUtf8()).object().value(QStringLiteral("format")).toString();
    if (format != QStringLiteral("qcow2")) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "only_qcow2_images_can_be_compacted"));
        return false;
    }

    const QFileInfo sourceInfo(path);
    const QString temporaryPath = path + QStringLiteral(".karton-compact");
    QFile::remove(temporaryPath);
    Q_EMIT diskMaintenanceStarted(QStringLiteral("compact"), path);

    m_commandRunner->runCommand(QStringLiteral("qemu-img"),
                                {QStringLiteral("convert"), QStringLiteral("-O"), QStringLiteral("qcow2"), QStringLiteral("-U"), path, temporaryPath},
                                [this, path, temporaryPath, permissions = sourceInfo.permissions()](bool success, const QString &output) {
                                    if (!success) {
                                        QFile::remove(temporaryPath);
                                        Q_EMIT diskMaintenanceFinished(QStringLiteral("compact"), path, false, output.trimmed());
                                        return;
                                    }

                                    const QString backupPath = path + QStringLiteral(".karton-backup");
                                    QFile::remove(backupPath);
                                    if (!QFile::rename(path, backupPath)) {
                                        QFile::remove(temporaryPath);
                                        Q_EMIT diskMaintenanceFinished(QStringLiteral("compact"),
                                                                       path,
                                                                       false,
                                                                       QCoreApplication::translate("Karton", "failed_to_replace_disk_image_1").arg(path));
                                        return;
                                    }
                                    if (!QFile::rename(temporaryPath, path)) {
                                        QFile::rename(backupPath, path);
                                        QFile::remove(temporaryPath);
                                        Q_EMIT diskMaintenanceFinished(QStringLiteral("compact"),
                                                                       path,
                                                                       false,
                                                                       QCoreApplication::translate("Karton", "failed_to_replace_disk_image_1").arg(path));
                                        return;
                                    }
                                    QFile::remove(backupPath);
                                    QFile::setPermissions(path, permissions);
                                    Q_EMIT diskMaintenanceFinished(QStringLiteral("compact"), path, true, output.trimmed());
                                });
    return true;
}

bool Karton::convertDiskImage(const QString &sourcePath, const QString &destinationPath, const QString &format)
{
    if (sourcePath.isEmpty() || destinationPath.isEmpty() || sourcePath == destinationPath) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "source_and_destination_must_differ"));
        return false;
    }
    if (!QFileInfo::exists(sourcePath)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "disk_image_does_not_exist_1").arg(sourcePath));
        return false;
    }
    const QString targetFormat = format.trimmed().toLower();
    if (targetFormat != QStringLiteral("qcow2") && targetFormat != QStringLiteral("raw") && targetFormat != QStringLiteral("vmdk")) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "unsupported_disk_format_1").arg(format));
        return false;
    }

    // The destination comes from a save dialog, so an existing file there has
    // already been confirmed by the user. Remove it so qemu-img cannot fail on
    // an existing target.
    if (QFileInfo::exists(destinationPath)) {
        QFile::remove(destinationPath);
    }

    Q_EMIT diskMaintenanceStarted(QStringLiteral("convert"), destinationPath);
    m_commandRunner->runCommand(QStringLiteral("qemu-img"),
                                {QStringLiteral("convert"), QStringLiteral("-O"), targetFormat, QStringLiteral("-U"), sourcePath, destinationPath},
                                [this, destinationPath](bool success, const QString &output) {
                                    if (!success) {
                                        QFile::remove(destinationPath);
                                    }
                                    Q_EMIT diskMaintenanceFinished(QStringLiteral("convert"), destinationPath, success, output.trimmed());
                                });
    return true;
}

bool Karton::openDiskFolder(const QString &path)
{
    const QFileInfo info(path);
    const QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    if (folder.isEmpty()) {
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}
