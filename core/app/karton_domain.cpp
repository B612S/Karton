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

// ---- Hardware configuration ----

QVariantMap Karton::domainHardwareInfo(const Domain *domain) const
{
    QVariantMap info;
    if (!domain || !domain->domainPtr()) {
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

    const QDomElement root = document.documentElement();
    const QDomElement os = root.firstChildElement(QStringLiteral("os"));
    info.insert(QStringLiteral("firmware"),
                os.attribute(QStringLiteral("firmware")) == QStringLiteral("efi") ? QStringLiteral("uefi") : QStringLiteral("bios"));
    const QDomElement loader = os.firstChildElement(QStringLiteral("loader"));
    info.insert(QStringLiteral("secureBoot"), loader.attribute(QStringLiteral("secure")) == QStringLiteral("yes"));

    QString bootDev;
    const QDomNodeList bootNodes = os.elementsByTagName(QStringLiteral("boot"));
    if (!bootNodes.isEmpty()) {
        bootDev = bootNodes.at(0).toElement().attribute(QStringLiteral("dev"));
    }
    info.insert(QStringLiteral("bootDev"), bootDev.isEmpty() ? QStringLiteral("hd") : bootDev);

    const QDomElement devices = root.firstChildElement(QStringLiteral("devices"));
    const QDomElement video = devices.firstChildElement(QStringLiteral("video"));
    const QDomElement videoModel = video.firstChildElement(QStringLiteral("model"));
    const QString videoType = videoModel.attribute(QStringLiteral("type"), QStringLiteral("qxl"));
    const bool qxlVideo = videoType == QStringLiteral("qxl");
    info.insert(QStringLiteral("videoModel"), videoType);
    info.insert(QStringLiteral("videoHeads"), videoModel.attribute(QStringLiteral("heads"), QStringLiteral("1")).toInt());
    // Sizes are stored in KiB in the domain XML. QXL exposes its guest
    // framebuffer through vgamem; vga/virtio use the generic vram attribute.
    const int videoVgamem = videoModel.attribute(QStringLiteral("vgamem"), QStringLiteral("16384")).toInt();
    const int videoVram = videoModel.attribute(QStringLiteral("vram"), qxlVideo ? QStringLiteral("65536") : QStringLiteral("16384")).toInt();
    info.insert(QStringLiteral("videoVgamem"), videoVgamem);
    info.insert(QStringLiteral("videoVram"), videoVram);
    info.insert(QStringLiteral("videoRam"), videoModel.attribute(QStringLiteral("ram"), QStringLiteral("65536")).toInt());
    info.insert(QStringLiteral("videoMemoryKiB"), qxlVideo ? videoVgamem : videoVram);
    const QDomElement acceleration = videoModel.firstChildElement(QStringLiteral("acceleration"));
    info.insert(QStringLiteral("accel3d"), acceleration.attribute(QStringLiteral("accel3d")) == QStringLiteral("yes"));

    const QDomElement sound = devices.firstChildElement(QStringLiteral("sound"));
    info.insert(QStringLiteral("soundModel"), sound.isNull() ? QStringLiteral("none") : sound.attribute(QStringLiteral("model"), QStringLiteral("ich9")));

    const QDomElement tpm = devices.firstChildElement(QStringLiteral("tpm"));
    const QDomElement tpmBackend = tpm.firstChildElement(QStringLiteral("backend"));
    info.insert(QStringLiteral("tpmEnabled"), !tpm.isNull());
    info.insert(QStringLiteral("tpmModel"), tpm.attribute(QStringLiteral("model"), QStringLiteral("tpm-crb")));
    info.insert(QStringLiteral("tpmVersion"), tpmBackend.attribute(QStringLiteral("version"), QStringLiteral("2.0")));
    info.insert(QStringLiteral("description"), root.firstChildElement(QStringLiteral("description")).text());
    const QDomElement memoryBacking = root.firstChildElement(QStringLiteral("memoryBacking"));
    const QDomElement memoryAccess = memoryBacking.firstChildElement(QStringLiteral("access"));
    const QDomElement memorySource = memoryBacking.firstChildElement(QStringLiteral("source"));
    info.insert(QStringLiteral("sharedMemoryBacking"),
                memoryAccess.attribute(QStringLiteral("mode")) == QStringLiteral("shared")
                    || memorySource.attribute(QStringLiteral("type")) == QStringLiteral("memfd"));
    return info;
}

bool Karton::setSharedMemoryBacking(const Domain *domain, bool enabled)
{
    if (!domain || !domain->domainPtr() || !m_conn) {
        return false;
    }
    char *xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xmlDesc))) {
        free(xmlDesc);
        return false;
    }
    free(xmlDesc);

    QDomElement root = document.documentElement();
    QDomElement memoryBacking = root.firstChildElement(QStringLiteral("memoryBacking"));
    if (enabled) {
        if (memoryBacking.isNull()) {
            memoryBacking = document.createElement(QStringLiteral("memoryBacking"));
            const QDomElement currentMemory = root.firstChildElement(QStringLiteral("currentMemory"));
            if (!currentMemory.isNull()) {
                root.insertAfter(memoryBacking, currentMemory);
            } else {
                root.insertBefore(memoryBacking, root.firstChildElement(QStringLiteral("vcpu")));
            }
        }
        QDomElement source = memoryBacking.firstChildElement(QStringLiteral("source"));
        if (source.isNull()) {
            source = document.createElement(QStringLiteral("source"));
            memoryBacking.appendChild(source);
        }
        source.setAttribute(QStringLiteral("type"), QStringLiteral("memfd"));
        QDomElement access = memoryBacking.firstChildElement(QStringLiteral("access"));
        if (access.isNull()) {
            access = document.createElement(QStringLiteral("access"));
            memoryBacking.appendChild(access);
        }
        access.setAttribute(QStringLiteral("mode"), QStringLiteral("shared"));
    } else if (!memoryBacking.isNull()) {
        const QDomElement access = memoryBacking.firstChildElement(QStringLiteral("access"));
        if (!access.isNull()) {
            memoryBacking.removeChild(access);
        }
        const QDomElement source = memoryBacking.firstChildElement(QStringLiteral("source"));
        if (!source.isNull() && source.attribute(QStringLiteral("type")) == QStringLiteral("memfd")) {
            memoryBacking.removeChild(source);
        }
        if (!memoryBacking.hasChildNodes()) {
            root.removeChild(memoryBacking);
        }
    }

    virDomainPtr redefined = virDomainDefineXMLFlags(m_conn, document.toString().toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!redefined) {
        Q_EMIT errorOccurred(i18nc("%1 is the error message", "Failed to update shared memory backing: %1", QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    refreshDomainList();
    return true;
}

bool Karton::editDomainHardware(const Domain *domain, const QVariantMap &settings)
{
    if (!domain || !domain->domainPtr() || !m_conn) {
        return false;
    }

    char *xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        Q_EMIT errorOccurred(i18nc("%1 is the name of the domain, %2 is the error message",
                                   "Failed to read configuration for domain %1: %2",
                                   domain->config()->name(),
                                   QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }

    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xmlDesc))) {
        free(xmlDesc);
        Q_EMIT errorOccurred(i18n("Failed to parse the virtual machine configuration."));
        return false;
    }
    free(xmlDesc);

    QDomElement root = document.documentElement();
    QDomElement os = root.firstChildElement(QStringLiteral("os"));
    if (settings.contains(QStringLiteral("firmware"))) {
        const bool uefi = settings.value(QStringLiteral("firmware")).toString() == QStringLiteral("uefi");
        if (uefi) {
            os.setAttribute(QStringLiteral("firmware"), QStringLiteral("efi"));
            QDomElement loader = os.firstChildElement(QStringLiteral("loader"));
            if (loader.isNull()) {
                loader = document.createElement(QStringLiteral("loader"));
                const QDomElement typeElement = os.firstChildElement(QStringLiteral("type"));
                os.insertAfter(loader, typeElement);
            }
            loader.setAttribute(QStringLiteral("secure"), settings.value(QStringLiteral("secureBoot")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"));
        } else {
            os.removeAttribute(QStringLiteral("firmware"));
            const QDomElement loader = os.firstChildElement(QStringLiteral("loader"));
            if (!loader.isNull()) {
                os.removeChild(loader);
            }
            const QDomElement nvram = root.firstChildElement(QStringLiteral("nvram"));
            if (!nvram.isNull()) {
                root.removeChild(nvram);
            }
        }
    } else if (settings.contains(QStringLiteral("secureBoot")) && os.attribute(QStringLiteral("firmware")) == QStringLiteral("efi")) {
        QDomElement loader = os.firstChildElement(QStringLiteral("loader"));
        if (!loader.isNull()) {
            loader.setAttribute(QStringLiteral("secure"), settings.value(QStringLiteral("secureBoot")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"));
        }
    }

    if (settings.contains(QStringLiteral("bootDev"))) {
        QDomNodeList bootNodes = os.elementsByTagName(QStringLiteral("boot"));
        for (int i = bootNodes.count() - 1; i >= 0; --i) {
            const QDomElement boot = bootNodes.at(i).toElement();
            if (boot.parentNode() == os) {
                os.removeChild(boot);
            }
        }
        const QString first = settings.value(QStringLiteral("bootDev")).toString();
        QDomElement boot = document.createElement(QStringLiteral("boot"));
        boot.setAttribute(QStringLiteral("dev"), first);
        os.appendChild(boot);
        if (first != QStringLiteral("hd")) {
            QDomElement fallback = document.createElement(QStringLiteral("boot"));
            fallback.setAttribute(QStringLiteral("dev"), QStringLiteral("hd"));
            os.appendChild(fallback);
        }
    }

    QDomElement devices = root.firstChildElement(QStringLiteral("devices"));
    if (devices.isNull()) {
        devices = document.createElement(QStringLiteral("devices"));
        root.appendChild(devices);
    }

    if (settings.contains(QStringLiteral("videoModel"))) {
        QDomElement video = devices.firstChildElement(QStringLiteral("video"));
        if (video.isNull()) {
            video = document.createElement(QStringLiteral("video"));
            devices.appendChild(video);
        }
        QDomElement model = video.firstChildElement(QStringLiteral("model"));
        if (model.isNull()) {
            model = document.createElement(QStringLiteral("model"));
            video.appendChild(model);
        }
        const QString videoModelType = settings.value(QStringLiteral("videoModel")).toString();
        model.setAttribute(QStringLiteral("type"), videoModelType);
        model.setAttribute(QStringLiteral("heads"), QString::number(qMax(1, settings.value(QStringLiteral("videoHeads")).toInt())));
        model.setAttribute(QStringLiteral("primary"), QStringLiteral("yes"));
        if (settings.contains(QStringLiteral("videoMemory"))) {
            const int memoryMiB = qBound(16, settings.value(QStringLiteral("videoMemory")).toInt(), 512);
            if (videoModelType == QStringLiteral("qxl")) {
                // QXL's guest framebuffer is vgamem.
                model.setAttribute(QStringLiteral("vgamem"), QString::number(memoryMiB * 1024));
            } else {
                // VGA and virtio have no vgamem; their video memory is the
                // generic vram attribute (in KiB).
                model.setAttribute(QStringLiteral("vram"), QString::number(memoryMiB * 1024));
            }
        }
        if (videoModelType != QStringLiteral("qxl")) {
            // ram, vram64 and vgamem are QXL-only attributes. Leaving any of
            // them behind after switching to VirtIO/VGA makes libvirt reject
            // the domain with "ram attribute only supported for video type
            // qxl" (it validates ram first). The QXL video memory control must
            // not leak into the other video models.
            model.removeAttribute(QStringLiteral("ram"));
            model.removeAttribute(QStringLiteral("vram64"));
            model.removeAttribute(QStringLiteral("vgamem"));
        }
        const QDomElement oldAcceleration = model.firstChildElement(QStringLiteral("acceleration"));
        if (!oldAcceleration.isNull()) {
            model.removeChild(oldAcceleration);
        }
        if (settings.value(QStringLiteral("accel3d")).toBool()) {
            QDomElement acceleration = document.createElement(QStringLiteral("acceleration"));
            acceleration.setAttribute(QStringLiteral("accel3d"), QStringLiteral("yes"));
            model.appendChild(acceleration);
        }
    }

    if (settings.contains(QStringLiteral("soundModel"))) {
        const QString soundModel = settings.value(QStringLiteral("soundModel")).toString();
        QDomElement sound = devices.firstChildElement(QStringLiteral("sound"));
        if (soundModel == QStringLiteral("none")) {
            if (!sound.isNull()) {
                devices.removeChild(sound);
            }
        } else if (sound.isNull()) {
            sound = document.createElement(QStringLiteral("sound"));
            sound.setAttribute(QStringLiteral("model"), soundModel);
            devices.appendChild(sound);
        } else {
            sound.setAttribute(QStringLiteral("model"), soundModel);
        }
    }

    if (settings.contains(QStringLiteral("tpmEnabled"))) {
        QDomElement tpm = devices.firstChildElement(QStringLiteral("tpm"));
        if (!settings.value(QStringLiteral("tpmEnabled")).toBool()) {
            if (!tpm.isNull()) {
                devices.removeChild(tpm);
            }
        } else {
            if (tpm.isNull()) {
                tpm = document.createElement(QStringLiteral("tpm"));
                devices.appendChild(tpm);
            }
            tpm.setAttribute(QStringLiteral("model"), settings.value(QStringLiteral("tpmModel"), QStringLiteral("tpm-crb")).toString());
            QDomElement backend = tpm.firstChildElement(QStringLiteral("backend"));
            if (backend.isNull()) {
                backend = document.createElement(QStringLiteral("backend"));
                tpm.appendChild(backend);
            }
            backend.setAttribute(QStringLiteral("type"), QStringLiteral("emulator"));
            backend.setAttribute(QStringLiteral("version"), settings.value(QStringLiteral("tpmVersion"), QStringLiteral("2.0")).toString());
        }
    }

    if (settings.contains(QStringLiteral("description"))) {
        QDomElement description = root.firstChildElement(QStringLiteral("description"));
        const QString text = settings.value(QStringLiteral("description")).toString();
        if (text.trimmed().isEmpty()) {
            if (!description.isNull()) {
                root.removeChild(description);
            }
        } else {
            if (description.isNull()) {
                description = document.createElement(QStringLiteral("description"));
                root.insertAfter(description, root.firstChildElement(QStringLiteral("name")));
            }
            while (!description.firstChild().isNull()) {
                description.removeChild(description.firstChild());
            }
            description.appendChild(document.createTextNode(text));
        }
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

// ---- Cloning ----

bool Karton::cloneDomain(const Domain *domain, const QString &requestedName)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    const QString sourceName = domain->config()->name();
    const QString newName = requestedName.trimmed();
    if (!isValidDomainName(newName)) {
        Q_EMIT errorOccurred(i18n("A VM name may only contain letters, digits, dots, underscores and dashes."));
        return false;
    }

    virDomainPtr existing = virDomainLookupByName(m_conn, newName.toUtf8().constData());
    if (existing) {
        virDomainFree(existing);
        Q_EMIT errorOccurred(i18nc("%1 is a VM name", "A VM named %1 already exists.", newName));
        return false;
    }

    char *sourceXml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    if (!sourceXml) {
        Q_EMIT errorOccurred(
            i18nc("VM cloning failed, %1 is the libvirt error", "Could not read the source VM configuration: %1", QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }

    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(sourceXml))) {
        free(sourceXml);
        Q_EMIT errorOccurred(i18n("Could not parse the source VM configuration."));
        return false;
    }
    free(sourceXml);

    QDomElement root = document.documentElement();
    QDomElement nameElement = root.firstChildElement(QStringLiteral("name"));
    if (nameElement.isNull()) {
        Q_EMIT errorOccurred(i18n("The source VM configuration has no name."));
        return false;
    }
    if (nameElement.firstChild().isNull()) {
        nameElement.appendChild(document.createTextNode(newName));
    } else {
        nameElement.firstChild().setNodeValue(newName);
    }

    const QDomElement uuidElement = root.firstChildElement(QStringLiteral("uuid"));
    if (!uuidElement.isNull()) {
        root.removeChild(uuidElement);
    }
    const QDomElement metadataElement = root.firstChildElement(QStringLiteral("metadata"));
    if (!metadataElement.isNull()) {
        root.removeChild(metadataElement);
    }

    QDomElement osElement = root.firstChildElement(QStringLiteral("os"));
    if (!osElement.isNull()) {
        const QDomElement nvramElement = osElement.firstChildElement(QStringLiteral("nvram"));
        if (!nvramElement.isNull()) {
            osElement.removeChild(nvramElement);
        }
    }

    const QDomNodeList graphics = root.elementsByTagName(QStringLiteral("graphics"));
    for (int i = 0; i < graphics.count(); ++i) {
        QDomElement element = graphics.at(i).toElement();
        element.setAttribute(QStringLiteral("autoport"), QStringLiteral("yes"));
        element.removeAttribute(QStringLiteral("port"));
    }

    const QDomNodeList interfaces = root.elementsByTagName(QStringLiteral("interface"));
    for (int i = 0; i < interfaces.count(); ++i) {
        QDomElement element = interfaces.at(i).toElement();
        const QDomElement macElement = element.firstChildElement(QStringLiteral("mac"));
        if (!macElement.isNull()) {
            element.removeChild(macElement);
        }
    }

    const QDomNodeList hostdevs = root.elementsByTagName(QStringLiteral("hostdev"));
    for (int i = hostdevs.count() - 1; i >= 0; --i) {
        QDomElement element = hostdevs.at(i).toElement();
        if (!element.parentNode().isNull()) {
            element.parentNode().removeChild(element);
        }
    }

    struct DiskCopy {
        QString source;
        QString destination;
        QString format;
    };

    QVector<DiskCopy> diskCopies;
    const QDomNodeList diskNodes = root.elementsByTagName(QStringLiteral("disk"));
    int dataDiskIndex = 0;
    for (int i = 0; i < diskNodes.count(); ++i) {
        QDomElement disk = diskNodes.at(i).toElement();
        if (disk.attribute(QStringLiteral("device")) != QStringLiteral("disk")) {
            continue;
        }

        QDomElement source = disk.firstChildElement(QStringLiteral("source"));
        const QString sourcePath = source.attribute(QStringLiteral("file"));
        if (sourcePath.isEmpty()) {
            Q_EMIT errorOccurred(i18n("This VM has a disk that is not backed by a local file; cloning it is not supported."));
            return false;
        }

        const QDomElement driver = disk.firstChildElement(QStringLiteral("driver"));
        QString format = driver.attribute(QStringLiteral("type"));
        if (format.isEmpty()) {
            format = QStringLiteral("qcow2");
        }

        const QString diskName = dataDiskIndex == 0 ? newName : newName + QStringLiteral("-disk%1").arg(dataDiskIndex + 1);
        const QString destination = getVirtualDiskPath(diskName, format);
        if (QFile::exists(destination)) {
            Q_EMIT errorOccurred(i18nc("%1 is a file path", "The clone destination already exists: %1", destination));
            return false;
        }

        QDir().mkpath(QFileInfo(destination).absolutePath());
        source.setAttribute(QStringLiteral("file"), destination);
        diskCopies.append({sourcePath, destination, format});
        ++dataDiskIndex;
    }

    const QString cloneXml = document.toString();
    const auto createdFiles = std::make_shared<QStringList>();
    const auto diskList = std::make_shared<QVector<DiskCopy>>(diskCopies);

    auto defineClone = [this, cloneXml, createdFiles, sourceName, newName]() {
        virDomainPtr clone = virDomainDefineXML(m_conn, cloneXml.toUtf8().constData());
        if (!clone) {
            qCWarning(KARTON_DEBUG) << "Define clone failed:" << virGetLastErrorMessage();
            for (const QString &path : std::as_const(*createdFiles)) {
                QFile::remove(path);
            }
            const QString errorMsg =
                i18nc("%1 is a VM name, %2 is an error", "Failed to define cloned VM %1: %2", newName, QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            Q_EMIT domainCloned(false, sourceName, newName);
            return;
        }

        virDomainFree(clone);
        refreshDomainList();
        Q_EMIT domainCloned(true, sourceName, newName);
    };

    auto nextDisk = std::make_shared<std::function<void(int)>>();
    *nextDisk = [this, diskList, createdFiles, nextDisk, defineClone, sourceName, newName](int index) {
        if (index >= diskList->size()) {
            defineClone();
            return;
        }

        const DiskCopy &disk = diskList->at(index);
        m_commandRunner->runCommand(QStringLiteral("qemu-img"),
                                    {QStringLiteral("convert"), QStringLiteral("-U"), QStringLiteral("-O"), disk.format, disk.source, disk.destination},
                                    [this, disk, createdFiles, nextDisk, sourceName, newName, index](bool success, const QString &output) {
                                        if (!success) {
                                            for (const QString &path : std::as_const(*createdFiles)) {
                                                QFile::remove(path);
                                            }
                                            QFile::remove(disk.destination);
                                            Q_EMIT errorOccurred(
                                                i18nc("%1 is a disk path, %2 is the error", "Failed to copy disk %1: %2", disk.source, output));
                                            Q_EMIT domainCloned(false, sourceName, newName);
                                            return;
                                        }
                                        createdFiles->append(disk.destination);
                                        (*nextDisk)(index + 1);
                                    });
    };

    if (diskList->isEmpty()) {
        defineClone();
    } else {
        (*nextDisk)(0);
    }
    return true;
}

// ---- Creating, editing and deleting domains ----

bool Karton::deleteDomain(const Domain *domain, const bool deleteDisk)
{
    if (!domain) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainUndefine(domainPtr) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to undefine domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    qCInfo(KARTON_DEBUG) << "Successfully undefined domain:" << domain->config()->name();

    bool success = true;

    if (deleteDisk) {
        QStringList diskPaths = domain->config()->virtualDiskPaths();
        if (diskPaths.isEmpty() && !domain->config()->virtualDiskPath().isEmpty()) {
            diskPaths.append(domain->config()->virtualDiskPath());
        }
        diskPaths.removeDuplicates();

        for (const QString &diskPath : std::as_const(diskPaths)) {
            if (diskPath.isEmpty() || !QFile::exists(diskPath)) {
                continue;
            }
            if (!QFile::remove(diskPath)) {
                const QString errorMsg = i18nc("%1 is the path of the disk file", "Failed to delete disk file: %1", diskPath);
                qCWarning(KARTON_DEBUG) << errorMsg;
                Q_EMIT errorOccurred(errorMsg);
                success = false;
            } else {
                qCInfo(KARTON_DEBUG) << "Successfully deleted disk image:" << diskPath;
            }
        }
    }

    const QString previewPath = domain->config()->screenshotPreviewPath();
    if (!previewPath.isEmpty() && QFile::exists(previewPath) && !QFile::remove(previewPath)) {
        const QString errorMsg = i18nc("%1 is the path of the preview image", "Failed to delete preview image: %1", previewPath);
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        success = false;
    }

    auto *removableDomain = const_cast<Domain *>(domain);
    if (m_domains.removeOne(removableDomain)) {
        saveDomainOrder(m_domains, m_activeUri.isEmpty() ? connectionUri() : m_activeUri);
        Q_EMIT domainRemoved(removableDomain);
        removableDomain->deleteLater();
    }

    return success;
}

bool Karton::createDomain(const QVariantMap &config)
{
    if (!m_conn) {
        Q_EMIT errorOccurred(i18n("Not connected to libvirt"));
        return false;
    }

    const QString name = config.value(QStringLiteral("name")).toString().trimmed();
    const int memoryGB = config.value(QStringLiteral("memoryGB")).toInt();
    const int cpus = config.value(QStringLiteral("cpus")).toInt();
    const int storageGB = config.value(QStringLiteral("storageGB")).toInt();

    if (!isValidDomainName(name)) {
        Q_EMIT errorOccurred(i18nc("%1 is the requested name of the virtual machine", "Invalid virtual machine name: %1", name));
        return false;
    }
    if (memoryGB <= 0 || cpus <= 0 || storageGB <= 0) {
        Q_EMIT errorOccurred(i18n("Invalid virtual machine configuration"));
        return false;
    }
    const QString diskFormat = config.value(QStringLiteral("diskFormat"), QStringLiteral("qcow2")).toString();
    if (diskFormat != QStringLiteral("qcow2") && diskFormat != QStringLiteral("raw") && diskFormat != QStringLiteral("vmdk")) {
        Q_EMIT errorOccurred(i18n("Unsupported disk format"));
        return false;
    }
    const QString firmware = config.value(QStringLiteral("firmware"), QStringLiteral("bios")).toString();
    if (firmware != QStringLiteral("bios") && firmware != QStringLiteral("efi")) {
        Q_EMIT errorOccurred(i18n("Unsupported firmware type"));
        return false;
    }
    const QString networkModel = config.value(QStringLiteral("networkModel"), QStringLiteral("virtio")).toString();
    if (networkModel != QStringLiteral("virtio") && networkModel != QStringLiteral("e1000") && networkModel != QStringLiteral("none")) {
        Q_EMIT errorOccurred(i18n("Unsupported network model"));
        return false;
    }

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString basePath = QStringLiteral("%1/libvirt").arg(dataDir);
    QDir baseDir(basePath);
    if (!baseDir.mkpath(QStringLiteral("images"))) {
        qCCritical(KARTON_DEBUG) << "Failed to create directory for images:" << basePath;
        Q_EMIT errorOccurred(i18nc("%1 is a directory path", "Failed to create image directory: %1", basePath));
        return false;
    }

    DomainConfig::DomainConfigData configData;
    configData.name = name;
    configData.shortOsId = config.value(QStringLiteral("shortOsId")).toString();
    configData.maxRam = memoryGB;
    configData.ramUsage = memoryGB;
    configData.cpus = cpus;
    configData.maxDiskStorage = storageGB;
    configData.xmlConfigPath = getXmlConfigPath(name);
    configData.isoDiskPath = config.value(QStringLiteral("isoDiskPath")).toString();
    configData.virtualDiskPath = getVirtualDiskPath(name, diskFormat);
    configData.virtualDiskPaths = {configData.virtualDiskPath};
    configData.diskFormat = diskFormat;
    configData.firmware = firmware;
    configData.networkModel = networkModel;
    configData.secureBoot = config.value(QStringLiteral("secureBoot"), false).toBool();
    configData.enableAccel3d = config.value(QStringLiteral("enableAccel3d"), true).toBool();

    const QString diskPath = configData.virtualDiskPath;

    m_commandRunner->runCommand(
        QStringLiteral("qemu-img"),
        {QStringLiteral("create"), QStringLiteral("-f"), diskFormat, diskPath, QStringLiteral("%1G").arg(storageGB)},
        [this, name, diskPath, configData](bool success, const QString &output) {
            if (!success) {
                QFile::remove(diskPath);
                const QString errorMsg =
                    i18nc("%1 is the name of the domain, %2 is the error message", "Failed to create disk image for domain %1: %2", name, output);
                qCWarning(KARTON_DEBUG) << errorMsg;
                Q_EMIT errorOccurred(errorMsg);
                Q_EMIT domainCreated(false, name);
                return;
            }

            DomainConfig domainConfig(configData);
            DomainXmlBuilder builder;
            if (!builder.setupDomain(m_conn, &domainConfig)) {
                QFile::remove(diskPath);
                const QString errorMsg = i18nc("%1 is the name of the domain", "Failed to create domain: %1", name);
                qCWarning(KARTON_DEBUG) << errorMsg;
                Q_EMIT errorOccurred(errorMsg);
                Q_EMIT domainCreated(false, name);
                return;
            }

            qCInfo(KARTON_DEBUG) << "Successfully created domain:" << name;
            refreshDomainList();
            Q_EMIT domainCreated(true, name);
        });

    return true;
}

bool Karton::editDomain(const Domain *domain, const QVariantMap &config)
{
    if (!domain || !m_conn) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    DomainConfig *domainConfig = domain->config();

    if (virDomainIsActive(domainPtr) == 1) {
        const QString errorMsg = i18nc("%1 is the name of the domain", "Cannot edit configuration while domain is running: %1", domainConfig->name());
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const QString newName = config.value(QStringLiteral("name")).toString().trimmed();
    const int newMemoryGB = config.value(QStringLiteral("memoryGB")).toInt();
    const int newCpus = config.value(QStringLiteral("cpus")).toInt();
    const int newStorageGB = config.value(QStringLiteral("storageGB")).toInt();

    if (!isValidDomainName(newName)) {
        const QString errorMsg = i18nc("%1 is the requested name of the virtual machine", "Invalid virtual machine name: %1", newName);
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }
    if (newMemoryGB <= 0 || newCpus <= 0 || newStorageGB <= 0) {
        const QString errorMsg = i18n("Invalid virtual machine configuration");
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    // Karton stores the disk size in domain metadata. Domains defined by other
    // tools do not have it; in that case the size is unknown and must not be
    // touched.
    const int oldStorageGB = domainConfig->maxDiskStorage();
    const bool storageKnown = oldStorageGB > 0;
    if (storageKnown && newStorageGB < oldStorageGB) {
        const QString errorMsg = i18nc("%1 is the name of the domain", "Disk storage cannot be shrunk for domain: %1", domainConfig->name());
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    char *xmlDesc = virDomainGetXMLDesc(domainPtr, VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to read configuration for domain %1: %2",
                                       domainConfig->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    DomainXmlBuilder builder;
    const QString xmlString = builder.updateXML(QString::fromUtf8(xmlDesc), {.maxRam = newMemoryGB, .cpus = newCpus, .maxDiskStorage = newStorageGB});
    free(xmlDesc);

    if (xmlString.isEmpty()) {
        const QString errorMsg = i18nc("%1 is the name of the domain", "Failed to update configuration for domain: %1", domainConfig->name());
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const QString oldName = domainConfig->name();

    // Redefine the persistent XML, then rename if needed. Renaming must happen
    // separately: redefining the XML with a different <name> but the same UUID
    // is rejected by libvirt as a UUID conflict.
    const auto applyXml = [this, xmlString, newName, oldName](Domain *target) -> bool {
        if (!target) {
            Q_EMIT errorOccurred(i18n("The virtual machine was removed"));
            return false;
        }

        virDomainPtr domainPtr = target->domainPtr();
        virDomainPtr redefined = virDomainDefineXML(m_conn, xmlString.toUtf8().constData());
        if (!redefined) {
            const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                           "Failed to update configuration for domain %1: %2",
                                           oldName,
                                           QString::fromUtf8(virGetLastErrorMessage()));
            qCWarning(KARTON_DEBUG) << errorMsg;
            Q_EMIT errorOccurred(errorMsg);
            return false;
        }
        virDomainFree(redefined);

        if (newName != oldName && virDomainRename(domainPtr, newName.toUtf8().constData(), 0) < 0) {
            const QString errorMsg = i18nc("%1 is the old name, %2 is the new name, %3 is the error message",
                                           "Failed to rename domain %1 to %2: %3",
                                           oldName,
                                           newName,
                                           QString::fromUtf8(virGetLastErrorMessage()));
            qCWarning(KARTON_DEBUG) << errorMsg;
            Q_EMIT errorOccurred(errorMsg);
            refreshDomainList();
            return false;
        }

        qCInfo(KARTON_DEBUG) << "Successfully updated configuration for domain:" << newName;

        // Reload the paths (a rename also changes the XML config file path).
        refreshDomainList();
        return true;
    };

    // Grow the disk before redefining the domain. Growing is safe even if the
    // subsequent define fails, while the opposite order would leave the config
    // and the disk out of sync. The resize runs asynchronously so the GUI stays
    // responsive.
    if (storageKnown && newStorageGB > oldStorageGB) {
        const QString diskPath = domainConfig->virtualDiskPath();
        QPointer<Domain> guard(const_cast<Domain *>(domain));

        m_commandRunner->runCommand(
            QStringLiteral("qemu-img"),
            {QStringLiteral("resize"), diskPath, QStringLiteral("%1G").arg(newStorageGB)},
            [this, guard, applyXml, newName, diskPath](bool success, const QString &output) {
                if (!success) {
                    const QString errorMsg =
                        i18nc("%1 is the name of the domain, %2 is the error message", "Failed to resize disk for domain %1: %2", newName, output);
                    qCWarning(KARTON_DEBUG) << errorMsg;
                    Q_EMIT errorOccurred(errorMsg);
                    Q_EMIT domainEdited(false, newName);
                    return;
                }

                m_diskSizeCache.remove(diskPath);
                const bool applied = applyXml(guard.data());
                Q_EMIT domainEdited(applied, newName);
            });
        return true;
    }

    const bool success = applyXml(const_cast<Domain *>(domain));
    Q_EMIT domainEdited(success, newName);
    return success;
}
