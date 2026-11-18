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

static QString pciHexValue(const QString &value, int width)
{
    bool ok = false;
    const uint number = value.toUInt(&ok, 0);
    if (!ok) {
        return {};
    }
    return QStringLiteral("0x") + QString::number(number, 16).rightJustified(width, QLatin1Char('0'));
}

static QString pciAddressString(const QString &pciDomain, const QString &bus, const QString &slot, const QString &function)
{
    return pciDomain + QStringLiteral(":") + bus + QStringLiteral(":") + slot + QStringLiteral(".") + function;
}

static QString pciHostdevXml(const QString &pciDomain, const QString &bus, const QString &slot, const QString &function)
{
    return QStringLiteral(
               "<hostdev mode='subsystem' type='pci' managed='yes'>"
               "<source>"
               "<address domain='%1' bus='%2' slot='%3' function='%4'/>"
               "</source>"
               "</hostdev>")
        .arg(pciDomain, bus, slot, function);
}

static QString
pciHostdevXmlForAddress(virDomainPtr domain, const QString &pciDomain, const QString &bus, const QString &slot, const QString &function, unsigned int xmlFlags)
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
            if (element.attribute(QStringLiteral("type")) != QStringLiteral("pci")) {
                continue;
            }

            const QDomElement source = element.firstChildElement(QStringLiteral("source"));
            const QDomElement address = source.firstChildElement(QStringLiteral("address"));
            const QString addressDomain = pciHexValue(address.attribute(QStringLiteral("domain")), 4);
            const QString addressBus = pciHexValue(address.attribute(QStringLiteral("bus")), 2);
            const QString addressSlot = pciHexValue(address.attribute(QStringLiteral("slot")), 2);
            const QString addressFunction = pciHexValue(address.attribute(QStringLiteral("function")), 1);
            if (addressDomain.compare(pciDomain, Qt::CaseInsensitive) != 0 || addressBus.compare(bus, Qt::CaseInsensitive) != 0
                || addressSlot.compare(slot, Qt::CaseInsensitive) != 0 || addressFunction.compare(function, Qt::CaseInsensitive) != 0) {
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

QVariantList Karton::pciDevices() const
{
    QVariantList result;
    if (!m_conn) {
        return result;
    }

    virNodeDevicePtr *nodeDevices = nullptr;
    const int count = virConnectListAllNodeDevices(m_conn, &nodeDevices, VIR_CONNECT_LIST_NODE_DEVICES_CAP_PCI_DEV);
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
            if (capability.attribute(QStringLiteral("type")) == QStringLiteral("pci")) {
                const QDomElement vendor = capability.firstChildElement(QStringLiteral("vendor"));
                const QDomElement product = capability.firstChildElement(QStringLiteral("product"));
                const QDomElement classElement = capability.firstChildElement(QStringLiteral("class"));
                const QDomElement iommuGroup = capability.firstChildElement(QStringLiteral("iommuGroup"));
                const QDomElement driver = rootElement.firstChildElement(QStringLiteral("driver"));

                const QString pciDomain = pciHexValue(capability.firstChildElement(QStringLiteral("domain")).text(), 4);
                const QString bus = pciHexValue(capability.firstChildElement(QStringLiteral("bus")).text(), 2);
                const QString slot = pciHexValue(capability.firstChildElement(QStringLiteral("slot")).text(), 2);
                const QString function = pciHexValue(capability.firstChildElement(QStringLiteral("function")).text(), 1);
                if (pciDomain.isEmpty() || bus.isEmpty() || slot.isEmpty() || function.isEmpty()) {
                    free(xml);
                    virNodeDeviceFree(nodeDevices[i]);
                    continue;
                }

                QString label = vendor.text();
                if (!product.text().isEmpty()) {
                    label += QStringLiteral(" ") + product.text();
                }
                if (label.trimmed().isEmpty()) {
                    label = nameElement.text();
                }

                QVariantMap info;
                info.insert(QStringLiteral("name"), nameElement.text());
                info.insert(QStringLiteral("label"), label);
                info.insert(QStringLiteral("vendor"), vendor.attribute(QStringLiteral("id")));
                info.insert(QStringLiteral("product"), product.attribute(QStringLiteral("id")));
                info.insert(QStringLiteral("class"), classElement.text());
                info.insert(QStringLiteral("pciDomain"), pciDomain);
                info.insert(QStringLiteral("bus"), bus);
                info.insert(QStringLiteral("slot"), slot);
                info.insert(QStringLiteral("function"), function);
                info.insert(QStringLiteral("address"), pciAddressString(pciDomain, bus, slot, function));
                info.insert(QStringLiteral("iommuGroup"), iommuGroup.attribute(QStringLiteral("number")));
                info.insert(QStringLiteral("driver"), driver.firstChildElement(QStringLiteral("name")).text());
                result.append(info);
            }
        }

        free(xml);
        virNodeDeviceFree(nodeDevices[i]);
    }
    free(nodeDevices);
    return result;
}

QVariantList Karton::domainPciDevices(const Domain *domain) const
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
            if (element.attribute(QStringLiteral("type")) != QStringLiteral("pci")) {
                continue;
            }

            const QDomElement source = element.firstChildElement(QStringLiteral("source"));
            const QDomElement address = source.firstChildElement(QStringLiteral("address"));
            const QString pciDomain = pciHexValue(address.attribute(QStringLiteral("domain")), 4);
            const QString bus = pciHexValue(address.attribute(QStringLiteral("bus")), 2);
            const QString slot = pciHexValue(address.attribute(QStringLiteral("slot")), 2);
            const QString function = pciHexValue(address.attribute(QStringLiteral("function")), 1);

            QVariantMap info;
            info.insert(QStringLiteral("pciDomain"), pciDomain);
            info.insert(QStringLiteral("bus"), bus);
            info.insert(QStringLiteral("slot"), slot);
            info.insert(QStringLiteral("function"), function);
            info.insert(QStringLiteral("address"), pciAddressString(pciDomain, bus, slot, function));
            result.append(info);
        }
    }

    free(xml);
    return result;
}

bool Karton::attachPciDevice(const Domain *domain, const QString &pciDomain, const QString &bus, const QString &slot, const QString &function)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    const QString normalizedDomain = pciHexValue(pciDomain, 4);
    const QString normalizedBus = pciHexValue(bus, 2);
    const QString normalizedSlot = pciHexValue(slot, 2);
    const QString normalizedFunction = pciHexValue(function, 1);
    if (normalizedDomain.isEmpty() || normalizedBus.isEmpty() || normalizedSlot.isEmpty() || normalizedFunction.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_pci_address"));
        return false;
    }
    for (const QVariant &entry : domainPciDevices(domain)) {
        const QVariantMap info = entry.toMap();
        if (info.value(QStringLiteral("pciDomain")).toString().compare(normalizedDomain, Qt::CaseInsensitive) == 0
            && info.value(QStringLiteral("bus")).toString().compare(normalizedBus, Qt::CaseInsensitive) == 0
            && info.value(QStringLiteral("slot")).toString().compare(normalizedSlot, Qt::CaseInsensitive) == 0
            && info.value(QStringLiteral("function")).toString().compare(normalizedFunction, Qt::CaseInsensitive) == 0) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "this_pci_device_is_already_attached"));
            return false;
        }
    }

    const QString xml = pciHostdevXml(normalizedDomain, normalizedBus, normalizedSlot, normalizedFunction);
    const QString label = pciAddressString(normalizedDomain, normalizedBus, normalizedSlot, normalizedFunction);
    bool success = false;
    if (domain->isActive()) {
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg =
                QCoreApplication::translate("Karton", "failed_to_attach_pci_device_1_2").arg(label).arg(QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
        } else {
            success = true;
        }

        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to persist PCI device:" << virGetLastErrorMessage();
        }
    } else if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) == 0) {
        success = true;
    } else {
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_attach_pci_device_1_2").arg(label).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

bool Karton::detachPciDevice(const Domain *domain, const QString &pciDomain, const QString &bus, const QString &slot, const QString &function)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    const QString normalizedDomain = pciHexValue(pciDomain, 4);
    const QString normalizedBus = pciHexValue(bus, 2);
    const QString normalizedSlot = pciHexValue(slot, 2);
    const QString normalizedFunction = pciHexValue(function, 1);
    const QString label = pciAddressString(normalizedDomain, normalizedBus, normalizedSlot, normalizedFunction);

    const QString liveXml = pciHostdevXmlForAddress(domain->domainPtr(), normalizedDomain, normalizedBus, normalizedSlot, normalizedFunction, 0);
    const QString configXml =
        pciHostdevXmlForAddress(domain->domainPtr(), normalizedDomain, normalizedBus, normalizedSlot, normalizedFunction, VIR_DOMAIN_XML_INACTIVE);
    if (liveXml.isEmpty() && configXml.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "no_matching_pci_device_1_was_found").arg(label));
        return false;
    }

    bool success = true;
    if (domain->isActive() && !liveXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), liveXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg =
                QCoreApplication::translate("Karton", "failed_to_detach_pci_device_1_2").arg(label).arg(QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (!configXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), configXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            const QString errorMsg =
                QCoreApplication::translate("Karton", "failed_to_detach_persistent_pci_device_1_2").arg(label).arg(QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}
