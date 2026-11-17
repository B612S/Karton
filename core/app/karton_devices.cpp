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

QString Karton::domainXml(const Domain *domain) const
{
    if (!domain || !domain->domainPtr()) {
        return {};
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xml) {
        return {};
    }
    const QString result = QString::fromUtf8(xml);
    free(xml);
    return result;
}

bool Karton::attachXmlDevice(const Domain *domain, const QString &xml)
{
    if (!domain || !domain->domainPtr() || xml.trimmed().isEmpty()) {
        return false;
    }
    unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
    if (domain->isActive()) {
        flags |= VIR_DOMAIN_AFFECT_LIVE;
    }
    int result = virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), flags);
    if (result < 0 && domain->isActive()) {
        result = virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG);
    }
    if (result < 0) {
        Q_EMIT errorOccurred(i18nc("The error message is supplied by libvirt", "Failed to add device: %1", QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    refreshDomainList();
    return true;
}

bool Karton::addController(const Domain *domain, const QString &type)
{
    if (!domain || !domain->domainPtr() || type.isEmpty()) {
        return false;
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return false;
    }
    free(xml);
    int nextIndex = 0;
    const QDomNodeList controllers = document.elementsByTagName(QStringLiteral("controller"));
    for (int i = 0; i < controllers.count(); ++i) {
        const QDomElement controller = controllers.at(i).toElement();
        if (controller.attribute(QStringLiteral("type")) == type) {
            nextIndex = qMax(nextIndex, controller.attribute(QStringLiteral("index")).toInt() + 1);
        }
    }
    QString xmlDevice = QStringLiteral("<controller type='%1' index='%2'").arg(type).arg(nextIndex);
    if (type == QStringLiteral("usb")) {
        xmlDevice += QStringLiteral(" model='qemu-xhci'");
    }
    xmlDevice += QStringLiteral("/>");
    return attachXmlDevice(domain, xmlDevice);
}

bool Karton::addSerialPort(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    int port = 0;
    const QVariantList serials = domainXmlDevices(domain);
    for (const QVariant &entry : serials) {
        if (entry.toMap().value(QStringLiteral("tag")).toString() == QStringLiteral("serial")) {
            ++port;
        }
    }
    return attachXmlDevice(domain, QStringLiteral("<serial type='pty'><target port='%1'/></serial>").arg(port));
}

bool Karton::addTextConsole(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    int port = 0;
    const QVariantList devices = domainXmlDevices(domain);
    for (const QVariant &entry : devices) {
        if (entry.toMap().value(QStringLiteral("tag")).toString() == QStringLiteral("console")) {
            ++port;
        }
    }
    return attachXmlDevice(domain, QStringLiteral("<console type='pty'><target type='serial' port='%1'/></console>").arg(port));
}

bool Karton::addChannel(const Domain *domain, const QString &name)
{
    if (!domain || name.isEmpty()) {
        return false;
    }
    if (name == QStringLiteral("com.redhat.spice.0")) {
        return attachXmlDevice(domain, QStringLiteral("<channel type='spicevmc'><target type='virtio' name='com.redhat.spice.0'/></channel>"));
    }
    if (name == QStringLiteral("org.spice-space.webdav.0")) {
        return attachXmlDevice(domain,
                               QStringLiteral("<channel type='spiceport'>"
                                              "<source channel='org.spice-space.webdav.0'/>"
                                              "<target type='virtio' name='org.spice-space.webdav.0'/>"
                                              "</channel>"));
    }
    return attachXmlDevice(domain, QStringLiteral("<channel type='unix'><target type='virtio' name='%1'/></channel>").arg(name));
}

QVariantList Karton::domainInputDevices(const Domain *domain) const
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
        const QDomNodeList inputs = document.elementsByTagName(QStringLiteral("input"));
        for (int i = 0; i < inputs.count(); ++i) {
            const QDomElement element = inputs.at(i).toElement();
            QVariantMap info;
            info.insert(QStringLiteral("type"), element.attribute(QStringLiteral("type")));
            info.insert(QStringLiteral("bus"), element.attribute(QStringLiteral("bus")));
            result.append(info);
        }
    }
    free(xml);
    return result;
}

bool Karton::attachInputDevice(const Domain *domain, const QString &type, const QString &bus)
{
    if (!domain || !domain->domainPtr() || type.isEmpty() || bus.isEmpty()) {
        return false;
    }
    const QString xml = QStringLiteral("<input type='%1' bus='%2'/>").arg(type, bus);
    unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
    if (domain->isActive()) {
        flags |= VIR_DOMAIN_AFFECT_LIVE;
    }
    int result = virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), flags);
    if (result < 0 && domain->isActive()) {
        result = virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG);
    }
    if (result < 0) {
        Q_EMIT errorOccurred(
            i18nc("%1 is the device type, %2 is the error message", "Failed to add input device %1: %2", type, QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    refreshDomainList();
    return true;
}

bool Karton::detachInputDevice(const Domain *domain, const QString &type, const QString &bus)
{
    if (!domain || !domain->domainPtr() || type.isEmpty()) {
        return false;
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return false;
    }
    free(xml);
    const QDomNodeList inputs = document.elementsByTagName(QStringLiteral("input"));
    for (int i = 0; i < inputs.count(); ++i) {
        const QDomElement element = inputs.at(i).toElement();
        if (element.attribute(QStringLiteral("type")) != type) {
            continue;
        }
        if (!bus.isEmpty() && element.attribute(QStringLiteral("bus")) != bus) {
            continue;
        }
        QDomDocument deviceDocument;
        deviceDocument.appendChild(deviceDocument.importNode(element, true));
        unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
        if (domain->isActive()) {
            flags |= VIR_DOMAIN_AFFECT_LIVE;
        }
        int result = virDomainDetachDeviceFlags(domain->domainPtr(), deviceDocument.toString().toUtf8().constData(), flags);
        if (result < 0 && domain->isActive()) {
            result = virDomainDetachDeviceFlags(domain->domainPtr(), deviceDocument.toString().toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG);
        }
        if (result < 0) {
            Q_EMIT errorOccurred(i18nc("%1 is the device type, %2 is the error message",
                                       "Failed to remove input device %1: %2",
                                       type,
                                       QString::fromUtf8(virGetLastErrorMessage())));
            return false;
        }
        refreshDomainList();
        return true;
    }
    return false;
}

bool Karton::detachXmlDevice(const Domain *domain, int index)
{
    if (!domain || !domain->domainPtr() || index < 0) {
        return false;
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return false;
    }
    free(xml);
    const QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    QDomElement device;
    int current = 0;
    for (QDomElement candidate = devices.firstChildElement(); !candidate.isNull(); candidate = candidate.nextSiblingElement()) {
        if (current++ == index) {
            device = candidate;
            break;
        }
    }
    if (device.isNull()) {
        return false;
    }
    QDomDocument deviceDocument;
    deviceDocument.appendChild(deviceDocument.importNode(device, true));
    unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
    if (domain->isActive()) {
        flags |= VIR_DOMAIN_AFFECT_LIVE;
    }
    int result = virDomainDetachDeviceFlags(domain->domainPtr(), deviceDocument.toString().toUtf8().constData(), flags);
    if (result < 0 && domain->isActive()) {
        result = virDomainDetachDeviceFlags(domain->domainPtr(), deviceDocument.toString().toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG);
    }
    if (result < 0) {
        Q_EMIT errorOccurred(i18nc("The error message is supplied by libvirt", "Failed to remove device: %1", QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    refreshDomainList();
    return true;
}

bool Karton::setXmlDevice(const Domain *domain, const QString &tag, const QString &xml)
{
    if (!domain || !domain->domainPtr() || !m_conn || tag.isEmpty() || xml.trimmed().isEmpty()) {
        return false;
    }
    char *xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xmlDesc) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xmlDesc))) {
        free(xmlDesc);
        return false;
    }
    free(xmlDesc);

    QDomDocument deviceDocument;
    if (!deviceDocument.setContent(xml)) {
        Q_EMIT errorOccurred(i18n("Invalid device XML."));
        return false;
    }
    QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    if (devices.isNull()) {
        devices = document.createElement(QStringLiteral("devices"));
        document.documentElement().appendChild(devices);
    }
    QDomElement existing;
    for (QDomElement candidate = devices.firstChildElement(); !candidate.isNull(); candidate = candidate.nextSiblingElement()) {
        if (candidate.tagName() == tag) {
            existing = candidate;
            break;
        }
    }
    if (existing.isNull()) {
        // Adding through libvirt's device API avoids rewriting the complete
        // domain XML and works for shmem/watchdog/RNG hotplug as well.
        return attachXmlDevice(domain, xml);
    }
    const QDomElement replacement = deviceDocument.documentElement();
    devices.replaceChild(document.importNode(replacement, true), existing);

    virDomainPtr redefined = virDomainDefineXMLFlags(m_conn, document.toString().toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!redefined) {
        Q_EMIT errorOccurred(
            i18nc("%1 is the device tag, %2 is the error message", "Failed to update %1: %2", tag, QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    refreshDomainList();
    return true;
}

bool Karton::replaceXmlDevice(const Domain *domain, int index, const QString &xml)
{
    if (!domain || !domain->domainPtr() || !m_conn || index < 0 || xml.trimmed().isEmpty()) {
        return false;
    }
    char *xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        xmlDesc = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xmlDesc) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xmlDesc))) {
        free(xmlDesc);
        return false;
    }
    free(xmlDesc);

    QDomDocument deviceDocument;
    if (!deviceDocument.setContent(xml)) {
        Q_EMIT errorOccurred(i18n("Invalid device XML."));
        return false;
    }
    QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    if (devices.isNull()) {
        return false;
    }
    QDomElement target;
    int current = 0;
    for (QDomElement candidate = devices.firstChildElement(); !candidate.isNull(); candidate = candidate.nextSiblingElement()) {
        if (current == index) {
            target = candidate;
            break;
        }
        ++current;
    }
    if (target.isNull()) {
        return false;
    }
    devices.replaceChild(document.importNode(deviceDocument.documentElement(), true), target);

    virDomainPtr redefined = virDomainDefineXMLFlags(m_conn, document.toString().toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!redefined) {
        Q_EMIT errorOccurred(i18nc("%1 is the error message", "Failed to update device: %1", QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    refreshDomainList();
    return true;
}

bool Karton::domainHasDevice(const Domain *domain, const QString &tag) const
{
    if (!domain || !domain->domainPtr() || tag.isEmpty()) {
        return false;
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xml) {
        return false;
    }
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return false;
    }
    free(xml);
    const QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    for (QDomElement device = devices.firstChildElement(); !device.isNull(); device = device.nextSiblingElement()) {
        if (device.tagName() == tag) {
            return true;
        }
    }
    return false;
}

bool Karton::ensureWebdavChannel(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xml) {
        return false;
    }
    const QByteArray current(xml);
    free(xml);
    if (current.contains("org.spice-space.webdav.0")) {
        return true;
    }
    return attachXmlDevice(domain,
                           QStringLiteral("<channel type='spiceport'>"
                                          "<source channel='org.spice-space.webdav.0'/>"
                                          "<target type='virtio' name='org.spice-space.webdav.0'/>"
                                          "</channel>"));
}

bool Karton::disableWebdavChannel(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    const QVariantList devices = domainXmlDevices(domain);
    for (const QVariant &entry : devices) {
        const QVariantMap info = entry.toMap();
        if (info.value(QStringLiteral("tag")).toString() == QStringLiteral("channel")
            && info.value(QStringLiteral("description")).toString() == QStringLiteral("org.spice-space.webdav.0")) {
            return detachXmlDevice(domain, info.value(QStringLiteral("index")).toInt());
        }
    }
    return true;
}

QVariantList Karton::domainXmlDevices(const Domain *domain) const
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
    if (!document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        return result;
    }
    free(xml);

    const QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
    for (QDomElement device = devices.firstChildElement(); !device.isNull(); device = device.nextSiblingElement()) {
        const QString tag = device.tagName();
        QString detail = device.attribute(QStringLiteral("type"));
        QString label = tag;
        QString icon = QStringLiteral("device-symbolic");
        const auto childText = [&device](const QString &name, const QString &attribute) {
            const QDomElement child = device.firstChildElement(name);
            return child.isNull() ? QString() : child.attribute(attribute);
        };
        if (tag == QStringLiteral("disk")) {
            detail = QStringLiteral("%1 %2").arg(childText(QStringLiteral("target"), QStringLiteral("dev")),
                                                 childText(QStringLiteral("source"), QStringLiteral("file")));
        } else if (tag == QStringLiteral("interface")) {
            detail = QStringLiteral("%1 %2").arg(childText(QStringLiteral("mac"), QStringLiteral("address")),
                                                 childText(QStringLiteral("model"), QStringLiteral("type")));
        } else if (tag == QStringLiteral("input")) {
            detail = QStringLiteral("%1 %2").arg(device.attribute(QStringLiteral("type")), device.attribute(QStringLiteral("bus")));
        } else if (tag == QStringLiteral("hostdev")) {
            const QString vendor = childText(QStringLiteral("vendor"), QStringLiteral("id"));
            const QString product = childText(QStringLiteral("product"), QStringLiteral("id"));
            detail = vendor.isEmpty() ? device.attribute(QStringLiteral("type")) : QStringLiteral("%1:%2").arg(vendor, product);
        } else if (tag == QStringLiteral("video")) {
            detail = childText(QStringLiteral("model"), QStringLiteral("type"));
        } else if (tag == QStringLiteral("sound") || tag == QStringLiteral("tpm")) {
            detail = device.attribute(QStringLiteral("model"));
        } else if (tag == QStringLiteral("controller")) {
            detail = QStringLiteral("%1 %2").arg(device.attribute(QStringLiteral("type")), device.attribute(QStringLiteral("index")));
        } else if (tag == QStringLiteral("serial") || tag == QStringLiteral("channel") || tag == QStringLiteral("console")) {
            detail = childText(QStringLiteral("target"), QStringLiteral("name"));
        } else if (tag == QStringLiteral("graphics")) {
            detail = device.attribute(QStringLiteral("type"));
        }

        if (tag == QStringLiteral("disk")) {
            label = i18n("Disk");
            icon = QStringLiteral("drive-harddisk-symbolic");
        } else if (tag == QStringLiteral("interface")) {
            label = i18n("Network interface");
            icon = QStringLiteral("network-wired-symbolic");
        } else if (tag == QStringLiteral("input")) {
            const QString inputType = device.attribute(QStringLiteral("type"));
            label = inputType == QStringLiteral("tablet") ? i18n("Tablet")
                : inputType == QStringLiteral("mouse")    ? i18n("Mouse")
                : inputType == QStringLiteral("keyboard") ? i18n("Keyboard")
                                                          : i18n("Input device");
            icon = inputType == QStringLiteral("keyboard") ? QStringLiteral("input-keyboard-symbolic")
                : inputType == QStringLiteral("mouse")     ? QStringLiteral("input-mouse-symbolic")
                                                           : QStringLiteral("input-tablet-symbolic");
        } else if (tag == QStringLiteral("video")) {
            label = i18n("Video");
            icon = QStringLiteral("video-display-symbolic");
        } else if (tag == QStringLiteral("sound")) {
            label = i18n("Sound");
            icon = QStringLiteral("audio-card-symbolic");
        } else if (tag == QStringLiteral("hostdev")) {
            label = device.attribute(QStringLiteral("type")) == QStringLiteral("usb") ? i18n("USB passthrough") : i18n("PCI passthrough");
            icon = QStringLiteral("drive-removable-media-usb-symbolic");
        } else if (tag == QStringLiteral("tpm")) {
            label = i18n("TPM");
            icon = QStringLiteral("security-high-symbolic");
        } else if (tag == QStringLiteral("controller")) {
            const QString controllerType = device.attribute(QStringLiteral("type"));
            label = controllerType == QStringLiteral("usb")                                           ? i18n("USB Controller")
                : controllerType == QStringLiteral("pci") || controllerType == QStringLiteral("pcie") ? i18n("PCI Express Controller")
                : controllerType == QStringLiteral("sata")                                            ? i18n("SATA Controller")
                : controllerType == QStringLiteral("virtio-serial")                                   ? i18n("VirtIO Serial Controller")
                : controllerType == QStringLiteral("scsi")                                            ? i18n("SCSI Controller")
                : controllerType == QStringLiteral("ide")                                             ? i18n("IDE Controller")
                                                                                                      : i18n("%1 Controller", controllerType);
            const QString controllerIndex = device.attribute(QStringLiteral("index"));
            if (!controllerIndex.isEmpty()) {
                label += QStringLiteral(" %1").arg(controllerIndex.toInt() + 1);
            }
            detail.clear();
            icon = QStringLiteral("cpu-symbolic");
        } else if (tag == QStringLiteral("serial")) {
            label = i18n("Serial Port");
            icon = QStringLiteral("network-wired-symbolic");
        } else if (tag == QStringLiteral("console")) {
            label = i18n("Text Console");
            icon = QStringLiteral("utilities-terminal-symbolic");
        } else if (tag == QStringLiteral("channel")) {
            const QString name = childText(QStringLiteral("target"), QStringLiteral("name"));
            if (name == QStringLiteral("org.qemu.guest_agent.0")) {
                label = i18n("QEMU Guest Agent");
            } else if (name == QStringLiteral("com.redhat.spice.0")) {
                label = i18n("SPICE Agent");
            } else if (name == QStringLiteral("org.spice-space.webdav.0")) {
                label = i18n("SPICE WebDAV");
            } else {
                label = i18n("VirtIO Channel (%1)", name);
            }
            icon = QStringLiteral("network-connect-symbolic");
        } else if (tag == QStringLiteral("redirdev")) {
            label = i18n("USB Redirection (SPICE)");
            icon = QStringLiteral("drive-removable-media-usb-symbolic");
        } else if (tag == QStringLiteral("watchdog")) {
            label = i18n("Watchdog");
            icon = QStringLiteral("security-medium-symbolic");
        } else if (tag == QStringLiteral("rng")) {
            label = i18n("Random Number Generator");
            icon = QStringLiteral("security-high-symbolic");
        } else if (tag == QStringLiteral("memballoon")) {
            label = i18n("Memory Balloon");
            icon = QStringLiteral("memory-symbolic");
        } else if (tag == QStringLiteral("panic")) {
            label = i18n("Panic Device");
            icon = QStringLiteral("dialog-warning-symbolic");
        } else if (tag == QStringLiteral("shmem")) {
            label = i18n("Shared Memory");
            icon = QStringLiteral("memory-symbolic");
        } else if (tag == QStringLiteral("iommu")) {
            label = i18n("IOMMU");
            icon = QStringLiteral("cpu-symbolic");
        } else if (tag == QStringLiteral("vsock")) {
            label = i18n("VSOCK");
            icon = QStringLiteral("network-connect-symbolic");
        } else if (tag == QStringLiteral("filesystem")) {
            label = i18n("Filesystem");
            icon = QStringLiteral("folder-symbolic");
        } else if (tag == QStringLiteral("smartcard")) {
            label = i18n("Smart Card");
            icon = QStringLiteral("security-high-symbolic");
        } else if (tag == QStringLiteral("hub")) {
            label = i18n("USB Hub");
            icon = QStringLiteral("drive-removable-media-usb-symbolic");
        } else if (tag == QStringLiteral("emulator")) {
            label = i18n("QEMU Emulator");
            icon = QStringLiteral("computer-symbolic");
        } else if (tag == QStringLiteral("graphics")) {
            label = i18n("Display");
            icon = QStringLiteral("video-display-symbolic");
        } else if (tag == QStringLiteral("audio")) {
            label = i18n("Audio backend");
            icon = QStringLiteral("audio-card-symbolic");
        }
        QVariantMap info;
        info.insert(QStringLiteral("tag"), tag);
        if (tag == QStringLiteral("watchdog")) {
            info.insert(QStringLiteral("model"), device.attribute(QStringLiteral("model")));
            info.insert(QStringLiteral("action"), device.attribute(QStringLiteral("action")));
        } else if (tag == QStringLiteral("rng")) {
            info.insert(QStringLiteral("model"), device.attribute(QStringLiteral("model")));
            const QDomElement backend = device.firstChildElement(QStringLiteral("backend"));
            info.insert(QStringLiteral("backend"), backend.attribute(QStringLiteral("model")));
        } else if (tag == QStringLiteral("panic")) {
            info.insert(QStringLiteral("model"), device.attribute(QStringLiteral("model")));
        } else if (tag == QStringLiteral("vsock")) {
            const QDomElement cid = device.firstChildElement(QStringLiteral("cid"));
            info.insert(QStringLiteral("cid"),
                        cid.attribute(QStringLiteral("auto")) == QStringLiteral("yes") ? QStringLiteral("auto") : cid.attribute(QStringLiteral("id")));
        } else if (tag == QStringLiteral("shmem")) {
            info.insert(QStringLiteral("name"), device.attribute(QStringLiteral("name")));
            info.insert(QStringLiteral("size"), device.attribute(QStringLiteral("size")));
            const QDomElement model = device.firstChildElement(QStringLiteral("model"));
            info.insert(QStringLiteral("model"), model.attribute(QStringLiteral("type")));
        } else if (tag == QStringLiteral("filesystem")) {
            const QDomElement source = device.firstChildElement(QStringLiteral("source"));
            const QDomElement target = device.firstChildElement(QStringLiteral("target"));
            const QDomElement driver = device.firstChildElement(QStringLiteral("driver"));
            info.insert(QStringLiteral("source"), source.attribute(QStringLiteral("dir")));
            info.insert(QStringLiteral("target"), target.attribute(QStringLiteral("dir")));
            info.insert(QStringLiteral("accessmode"), device.attribute(QStringLiteral("accessmode")));
            info.insert(QStringLiteral("driver"), driver.attribute(QStringLiteral("type")));
            info.insert(QStringLiteral("readonly"), !device.firstChildElement(QStringLiteral("readonly")).isNull());
        }
        info.insert(QStringLiteral("label"), label);
        info.insert(QStringLiteral("icon"), icon);
        info.insert(QStringLiteral("description"), detail.trimmed());
        info.insert(QStringLiteral("index"), result.size());
        result.append(info);
    }
    return result;
}

bool Karton::saveDomainXml(const Domain *domain, const QString &xml)
{
    if (!domain || !m_conn || xml.trimmed().isEmpty()) {
        return false;
    }

    QDomDocument document;
    const QDomDocument::ParseResult parseResult = document.setContent(xml);
    if (!parseResult || document.documentElement().tagName() != QStringLiteral("domain")) {
        const QString parseError = parseResult.errorMessage.isEmpty() ? i18n("the root element must be <domain>") : parseResult.errorMessage;
        Q_EMIT errorOccurred(
            i18nc("XML parse error with line and column", "Invalid domain XML at %1:%2: %3", parseResult.errorLine, parseResult.errorColumn, parseError));
        return false;
    }
    if (document.documentElement().firstChildElement(QStringLiteral("name")).text().trimmed().isEmpty()) {
        Q_EMIT errorOccurred(i18n("Invalid domain XML: a domain name is required."));
        return false;
    }

    // Validate the complete document against libvirt's domain schema, not only
    // the root element. This is the same schema validation virt-manager uses.
    virDomainPtr redefined = virDomainDefineXMLFlags(m_conn, xml.toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!redefined) {
        Q_EMIT errorOccurred(i18nc("%1 is the name of the domain, %2 is the error message",
                                   "Failed to save XML for domain %1: %2",
                                   domain->config()->name(),
                                   QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    virDomainFree(redefined);
    refreshDomainList();
    return true;
}
