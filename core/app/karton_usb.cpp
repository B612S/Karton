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

static bool hasUsbHostdev(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device)
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
    bool found = false;
    QDomDocument document;
    if (document.setContent(QString::fromUtf8(xml))) {
        const QDomNodeList hostdevs = document.elementsByTagName(QStringLiteral("hostdev"));
        for (int i = 0; i < hostdevs.count(); ++i) {
            const QDomElement element = hostdevs.at(i).toElement();
            if (element.attribute(QStringLiteral("type")) != QStringLiteral("usb")) {
                continue;
            }
            const QDomElement source = element.firstChildElement(QStringLiteral("source"));
            const QDomElement vendorElement = source.firstChildElement(QStringLiteral("vendor"));
            const QDomElement productElement = source.firstChildElement(QStringLiteral("product"));
            const QDomElement address = source.firstChildElement(QStringLiteral("address"));
            if (vendorElement.attribute(QStringLiteral("id")) == vendor && productElement.attribute(QStringLiteral("id")) == product
                && address.attribute(QStringLiteral("bus")) == bus && address.attribute(QStringLiteral("device")) == device) {
                found = true;
                break;
            }
        }
    }
    free(xml);
    return found;
}

static QString usbHostdevXml(const QString &vendor, const QString &product, const QString &bus, const QString &device)
{
    return QStringLiteral(
               "<hostdev mode='subsystem' type='usb' managed='yes'>"
               "<source>"
               "<vendor id='%1'/>"
               "<product id='%2'/>"
               "<address bus='%3' device='%4'/>"
               "</source>"
               "</hostdev>")
        .arg(vendor, product, bus, device);
}

static QString
usbHostdevXmlForDevice(virDomainPtr domain, const QString &vendor, const QString &product, const QString &bus, const QString &device, unsigned int xmlFlags)
{
    if (!domain) {
        return {};
    }

    char *xml = virDomainGetXMLDesc(domain, xmlFlags);
    if (!xml) {
        return {};
    }

    QString deviceXml;
    QDomDocument document;
    if (document.setContent(QString::fromUtf8(xml))) {
        const QDomNodeList hostdevs = document.elementsByTagName(QStringLiteral("hostdev"));
        for (int i = 0; i < hostdevs.count(); ++i) {
            const QDomElement element = hostdevs.at(i).toElement();
            if (element.attribute(QStringLiteral("type")) != QStringLiteral("usb")) {
                continue;
            }

            const QDomElement source = element.firstChildElement(QStringLiteral("source"));
            const QDomElement vendorElement = source.firstChildElement(QStringLiteral("vendor"));
            const QDomElement productElement = source.firstChildElement(QStringLiteral("product"));
            const QDomElement addressElement = source.firstChildElement(QStringLiteral("address"));

            const bool vendorMatches = vendorElement.attribute(QStringLiteral("id")).compare(vendor, Qt::CaseInsensitive) == 0;
            const bool productMatches = productElement.attribute(QStringLiteral("id")).compare(product, Qt::CaseInsensitive) == 0;
            const bool busMatches = bus.isEmpty() || addressElement.attribute(QStringLiteral("bus")) == bus;
            const bool deviceMatches = device.isEmpty() || addressElement.attribute(QStringLiteral("device")) == device;
            if (!vendorMatches || !productMatches || !busMatches || !deviceMatches) {
                continue;
            }

            QDomDocument deviceDocument;
            deviceDocument.appendChild(deviceDocument.importNode(element, true));
            deviceXml = deviceDocument.toString();
            break;
        }
    }

    free(xml);
    return deviceXml;
}

QVariantList Karton::usbDevices() const
{
    QVariantList result;
    if (!m_conn) {
        return result;
    }

    virNodeDevicePtr *nodeDevices = nullptr;
    const int count = virConnectListAllNodeDevices(m_conn, &nodeDevices, VIR_CONNECT_LIST_NODE_DEVICES_CAP_USB_DEV);
    if (count < 0) {
        return result;
    }

    for (int i = 0; i < count; ++i) {
        char *xml = virNodeDeviceGetXMLDesc(nodeDevices[i], 0);
        if (!xml) {
            virNodeDeviceFree(nodeDevices[i]);
            continue;
        }

        QDomDocument document;
        if (document.setContent(QString::fromUtf8(xml))) {
            const QDomElement rootElement = document.documentElement();
            const QDomElement nameElement = rootElement.firstChildElement(QStringLiteral("name"));
            const QDomElement capability = rootElement.firstChildElement(QStringLiteral("capability"));
            if (capability.attribute(QStringLiteral("type")) == QStringLiteral("usb_device")) {
                const QDomElement vendor = capability.firstChildElement(QStringLiteral("vendor"));
                const QDomElement product = capability.firstChildElement(QStringLiteral("product"));
                const QDomElement bus = capability.firstChildElement(QStringLiteral("bus"));
                const QDomElement device = capability.firstChildElement(QStringLiteral("device"));
                const QDomElement port = capability.firstChildElement(QStringLiteral("port"));
                const QDomElement devnode = rootElement.firstChildElement(QStringLiteral("devnode"));

                const QString vendorName = vendor.text();
                const QString productName = product.text();
                QString label = vendorName;
                if (!productName.isEmpty()) {
                    label += QStringLiteral(" ") + productName;
                }
                if (label.trimmed().isEmpty()) {
                    label = nameElement.text();
                }

                QVariantMap info;
                info.insert(QStringLiteral("name"), nameElement.text());
                info.insert(QStringLiteral("label"), label);
                info.insert(QStringLiteral("vendor"), vendor.attribute(QStringLiteral("id")));
                info.insert(QStringLiteral("product"), product.attribute(QStringLiteral("id")));
                info.insert(QStringLiteral("bus"), bus.text());
                info.insert(QStringLiteral("device"), device.text());
                info.insert(QStringLiteral("port"), port.text());
                info.insert(QStringLiteral("devnode"), devnode.text());
                info.insert(QStringLiteral("hub"),
                            productName.contains(QStringLiteral("hub"), Qt::CaseInsensitive)
                                || vendorName.contains(QStringLiteral("hub"), Qt::CaseInsensitive));
                result.append(info);
            }
        }

        free(xml);
        virNodeDeviceFree(nodeDevices[i]);
    }
    free(nodeDevices);
    return result;
}

QVariantList Karton::domainUsbDevices(const Domain *domain) const
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
        const QDomNodeList hostdevs = document.elementsByTagName(QStringLiteral("hostdev"));
        for (int i = 0; i < hostdevs.count(); ++i) {
            const QDomElement element = hostdevs.at(i).toElement();
            if (element.attribute(QStringLiteral("type")) != QStringLiteral("usb")) {
                continue;
            }

            const QDomElement source = element.firstChildElement(QStringLiteral("source"));
            const QDomElement vendor = source.firstChildElement(QStringLiteral("vendor"));
            const QDomElement product = source.firstChildElement(QStringLiteral("product"));
            const QDomElement address = source.firstChildElement(QStringLiteral("address"));

            QVariantMap info;
            info.insert(QStringLiteral("vendor"), vendor.attribute(QStringLiteral("id")));
            info.insert(QStringLiteral("product"), product.attribute(QStringLiteral("id")));
            info.insert(QStringLiteral("bus"), address.attribute(QStringLiteral("bus")));
            info.insert(QStringLiteral("device"), address.attribute(QStringLiteral("device")));
            info.insert(QStringLiteral("label"), QStringLiteral("%1:%2").arg(vendor.attribute(QStringLiteral("id")), product.attribute(QStringLiteral("id"))));
            result.append(info);
        }
    }

    free(xml);
    return result;
}

bool Karton::attachUsbDevice(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (vendor.isEmpty() || product.isEmpty() || bus.isEmpty() || device.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "usb_vendor_product_bus_and_device_are_required"));
        return false;
    }
    if (hasUsbHostdev(domain, vendor, product, bus, device)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "this_usb_device_is_already_attached"));
        return false;
    }

    const QString xml = usbHostdevXml(vendor, product, bus, device);
    bool success = false;
    if (domain->isActive()) {
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_attach_usb_device_1_2")
                                         .arg(vendor + QLatin1Char(':') + product)
                                         .arg(QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
        } else {
            success = true;
        }

        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to persist USB device:" << virGetLastErrorMessage();
        }
    } else if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) == 0) {
        success = true;
    } else {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_attach_usb_device_1_2")
                                     .arg(vendor + QLatin1Char(':') + product)
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

bool Karton::detachUsbDevice(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    const QString liveXml = usbHostdevXmlForDevice(domain->domainPtr(), vendor, product, bus, device, 0);
    const QString configXml = usbHostdevXmlForDevice(domain->domainPtr(), vendor, product, bus, device, VIR_DOMAIN_XML_INACTIVE);
    if (liveXml.isEmpty() && configXml.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "no_matching_usb_device_1_was_found").arg(vendor + QLatin1Char(':') + product));
        return false;
    }

    bool success = true;
    if (domain->isActive() && !liveXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), liveXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_detach_usb_device_1_2")
                                         .arg(vendor + QLatin1Char(':') + product)
                                         .arg(QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (!configXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), configXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_detach_persistent_usb_device_1_2")
                                         .arg(vendor + QLatin1Char(':') + product)
                                         .arg(QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

bool Karton::attachUsbDevicePersistent(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device)
{
    if (!domain || !domain->domainPtr() || vendor.isEmpty() || product.isEmpty() || bus.isEmpty() || device.isEmpty()) {
        return false;
    }
    if (hasUsbHostdev(domain, vendor, product, bus, device)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "this_usb_device_is_already_attached"));
        return false;
    }
    const QString xml = usbHostdevXml(vendor, product, bus, device);
    if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_persist_usb_device_1_2")
                                 .arg(vendor + QLatin1Char(':') + product)
                                 .arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    refreshDomain(domain->domainPtr());
    return true;
}

bool Karton::detachUsbDevicePersistent(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device)
{
    if (!domain || !domain->domainPtr() || vendor.isEmpty() || product.isEmpty() || bus.isEmpty() || device.isEmpty()) {
        return false;
    }
    const QString xml = usbHostdevXml(vendor, product, bus, device);
    if (virDomainDetachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_remove_persisted_usb_device_1_2")
                                 .arg(vendor + QLatin1Char(':') + product)
                                 .arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    refreshDomain(domain->domainPtr());
    return true;
}
