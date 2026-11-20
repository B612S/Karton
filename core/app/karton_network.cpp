// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"
#include "app/libvirtconnection.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
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
#include "util/calltimer.h"

using namespace KartonCommon;

namespace
{
// ---- Network address helpers ------------------------------------------------

bool parseIpv4Address(const QString &text, quint32 *address)
{
    QHostAddress candidate;
    if (!candidate.setAddress(text.trimmed()) || candidate.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }
    *address = candidate.toIPv4Address();
    return true;
}

bool parseIpv4Cidr(const QString &text, quint32 *network, int *prefix)
{
    const QStringList parts = text.trimmed().split(QLatin1Char('/'));
    if (parts.size() != 2) {
        return false;
    }
    quint32 address = 0;
    if (!parseIpv4Address(parts.at(0), &address)) {
        return false;
    }
    bool ok = false;
    const int parsedPrefix = parts.at(1).toInt(&ok);
    if (!ok || parsedPrefix < 1 || parsedPrefix > 30) {
        return false;
    }
    const quint32 mask = 0xFFFFFFFFu << (32 - parsedPrefix);
    *network = address & mask;
    *prefix = parsedPrefix;
    return true;
}

QString ipv4CidrString(quint32 network, int prefix)
{
    return QHostAddress(network).toString() + QLatin1Char('/') + QString::number(prefix);
}

QString ipv4AddressString(quint32 address)
{
    return QHostAddress(address).toString();
}

bool ipv4InNetwork(quint32 address, quint32 network, int prefix)
{
    const quint32 mask = 0xFFFFFFFFu << (32 - prefix);
    return (address & mask) == network;
}

quint32 ipv4DefaultGateway(quint32 network, int prefix, const QString &preferred)
{
    quint32 candidate = 0;
    if (!preferred.isEmpty() && parseIpv4Address(preferred, &candidate) && ipv4InNetwork(candidate, network, prefix)) {
        return candidate;
    }
    return network + 1;
}

quint32 ipv4DefaultDhcpStart(quint32 network, int prefix)
{
    const quint32 size = 1u << (32 - prefix);
    return network + qMax<quint32>(2, size / 2);
}

quint32 ipv4DefaultDhcpEnd(quint32 network, int prefix)
{
    const quint32 size = 1u << (32 - prefix);
    return network + size - 2;
}

bool parseIpv6Address(const QString &text, Q_IPV6ADDR *address)
{
    QHostAddress candidate;
    if (!candidate.setAddress(text.trimmed()) || candidate.protocol() != QAbstractSocket::IPv6Protocol) {
        return false;
    }
    *address = candidate.toIPv6Address();
    return true;
}

void maskIpv6(Q_IPV6ADDR *address, int prefix)
{
    for (int byte = 0; byte < 16; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            if (byte * 8 + bit >= prefix) {
                (*address)[byte] &= static_cast<quint8>(~(0x80 >> bit));
            }
        }
    }
}

bool parseIpv6Cidr(const QString &text, Q_IPV6ADDR *network, int *prefix)
{
    const QStringList parts = text.trimmed().split(QLatin1Char('/'));
    if (parts.size() != 2) {
        return false;
    }
    Q_IPV6ADDR address = {};
    if (!parseIpv6Address(parts.at(0), &address)) {
        return false;
    }
    bool ok = false;
    const int parsedPrefix = parts.at(1).toInt(&ok);
    if (!ok || parsedPrefix < 1 || parsedPrefix > 126) {
        return false;
    }
    maskIpv6(&address, parsedPrefix);
    *network = address;
    *prefix = parsedPrefix;
    return true;
}

QString ipv6AddressString(const Q_IPV6ADDR &address)
{
    return QHostAddress(address).toString();
}

QString ipv6CidrString(const Q_IPV6ADDR &network, int prefix)
{
    return ipv6AddressString(network) + QLatin1Char('/') + QString::number(prefix);
}

Q_IPV6ADDR ipv6Increment(Q_IPV6ADDR address, quint32 amount = 1)
{
    for (int byte = 15; byte >= 0 && amount > 0; --byte) {
        const quint32 sum = static_cast<quint32>(address[byte]) + (amount & 0xFFu);
        address[byte] = static_cast<quint8>(sum & 0xFFu);
        amount = (amount >> 8) + (sum >> 8);
    }
    return address;
}

QString ipv6DefaultGateway(const Q_IPV6ADDR &network, int prefix, const QString &preferred)
{
    Q_IPV6ADDR candidate = {};
    if (!preferred.isEmpty() && parseIpv6Address(preferred, &candidate)) {
        Q_IPV6ADDR candidateNetwork = candidate;
        maskIpv6(&candidateNetwork, prefix);
        if (std::memcmp(candidateNetwork.c, network.c, 16) == 0) {
            return ipv6AddressString(candidate);
        }
    }
    return ipv6AddressString(ipv6Increment(network));
}

QString ipv6DefaultDhcpStart(const Q_IPV6ADDR &network, int prefix)
{
    if (prefix != 64) {
        return {};
    }
    return ipv6AddressString(ipv6Increment(network, 0x100));
}

QString ipv6DefaultDhcpEnd(const Q_IPV6ADDR &network, int prefix)
{
    if (prefix != 64) {
        return {};
    }
    return ipv6AddressString(ipv6Increment(network, 0x1ff));
}

int ipv4PrefixFromNetmask(quint32 netmask)
{
    int prefix = 0;
    while (prefix < 32 && (netmask & 0x80000000u)) {
        ++prefix;
        netmask <<= 1;
    }
    return prefix;
}

// Reservations keep the time they were added in karton.conf because libvirt
// does not record it. The network name is used as a settings group.
QString dhcpReservationKey(const QString &connectionUri, const QString &networkName, const QString &mac)
{
    QString networkKey = networkName;
    networkKey.replace(QLatin1Char('/'), QLatin1Char('_'));
    const QString normalizedMac = mac.trimmed().toLower();
    if (connectionUri.isEmpty()) {
        // Primary connection keeps the historical key so existing
        // reservations survive the upgrade.
        return QStringLiteral("dhcpReservation/%1/%2").arg(networkKey, normalizedMac);
    }
    const QString scope = QString::fromLatin1(QCryptographicHash::hash(connectionUri.toUtf8(), QCryptographicHash::Sha1).toHex());
    return QStringLiteral("dhcpReservation/%1/%2/%3").arg(scope, networkKey, normalizedMac);
}

} // namespace

namespace
{
struct NetworkConfigData {
    QString name;
    QString mode;
    QString forwardInterface;
    QString bridgeName;
    QString domainName;
    bool ipv4Enabled = false;
    int ipv4Prefix = 0;
    quint32 ipv4Network = 0;
    quint32 ipv4Gateway = 0;
    bool ipv4Dhcp = false;
    quint32 ipv4DhcpStart = 0;
    quint32 ipv4DhcpEnd = 0;
    bool ipv6Enabled = false;
    int ipv6Prefix = 0;
    Q_IPV6ADDR ipv6Network = {};
    Q_IPV6ADDR ipv6Gateway = {};
    bool ipv6Dhcp = false;
    Q_IPV6ADDR ipv6DhcpStart = {};
    Q_IPV6ADDR ipv6DhcpEnd = {};
};

bool parseNetworkConfig(const QVariantMap &config, NetworkConfigData *data, QString *error)
{
    data->name = config.value(QStringLiteral("name")).toString().trimmed();
    if (data->name.isEmpty()) {
        *error = QCoreApplication::translate("Karton", "the_network_name_cannot_be_empty");
        return false;
    }
    if (!isValidDomainName(data->name)) {
        *error = QCoreApplication::translate("Karton", "a_network_name_may_only_contain_letters_digits_dots_underscores_and_dashes");
        return false;
    }

    data->mode = config.value(QStringLiteral("mode"), QStringLiteral("nat")).toString();
    static const QStringList modes = {QStringLiteral("nat"),
                                      QStringLiteral("route"),
                                      QStringLiteral("isolated"),
                                      QStringLiteral("open"),
                                      QStringLiteral("bridge")};
    if (!modes.contains(data->mode)) {
        *error = QCoreApplication::translate("Karton", "unsupported_network_mode");
        return false;
    }

    data->forwardInterface = config.value(QStringLiteral("forwardInterface")).toString().trimmed();
    data->bridgeName = config.value(QStringLiteral("bridgeName")).toString().trimmed();
    data->domainName = config.value(QStringLiteral("domainName")).toString().trimmed();
    if (!data->domainName.isEmpty() && (data->domainName.size() > 253 || data->domainName.contains(QRegularExpression(QStringLiteral("\\s"))))) {
        *error = QCoreApplication::translate("Karton", "the_dns_domain_name_is_invalid");
        return false;
    }

    if (data->mode == QStringLiteral("bridge")) {
        if (data->bridgeName.isEmpty()) {
            *error = QCoreApplication::translate("Karton", "a_bridge_name_is_required_for_bridge_networks");
            return false;
        }
        if (!isValidDomainName(data->bridgeName)) {
            *error = QCoreApplication::translate("Karton", "the_bridge_name_is_invalid");
            return false;
        }
        // Bridge networks get their addressing from the host bridge.
        data->forwardInterface.clear();
        data->ipv4Enabled = false;
        data->ipv6Enabled = false;
        return true;
    }

    data->ipv4Enabled = config.value(QStringLiteral("ipv4Enabled"), false).toBool();
    if (data->ipv4Enabled) {
        if (!parseIpv4Cidr(config.value(QStringLiteral("ipv4")).toString(), &data->ipv4Network, &data->ipv4Prefix)) {
            *error = QCoreApplication::translate("Karton", "the_ipv4_network_must_be_a_valid_cidr_such_as_192_168_100_0_24");
            return false;
        }
        data->ipv4Gateway = ipv4DefaultGateway(data->ipv4Network, data->ipv4Prefix, config.value(QStringLiteral("ipv4Gateway")).toString());
        data->ipv4Dhcp = config.value(QStringLiteral("ipv4Dhcp"), false).toBool();
        if (data->ipv4Dhcp) {
            QString startText = config.value(QStringLiteral("ipv4DhcpStart")).toString();
            QString endText = config.value(QStringLiteral("ipv4DhcpEnd")).toString();
            if (startText.isEmpty()) {
                startText = ipv4AddressString(ipv4DefaultDhcpStart(data->ipv4Network, data->ipv4Prefix));
            }
            if (endText.isEmpty()) {
                endText = ipv4AddressString(ipv4DefaultDhcpEnd(data->ipv4Network, data->ipv4Prefix));
            }
            if (!parseIpv4Address(startText, &data->ipv4DhcpStart) || !parseIpv4Address(endText, &data->ipv4DhcpEnd)
                || !ipv4InNetwork(data->ipv4DhcpStart, data->ipv4Network, data->ipv4Prefix)
                || !ipv4InNetwork(data->ipv4DhcpEnd, data->ipv4Network, data->ipv4Prefix) || data->ipv4DhcpStart > data->ipv4DhcpEnd
                || data->ipv4DhcpStart == data->ipv4Gateway || data->ipv4DhcpEnd == data->ipv4Gateway) {
                *error = QCoreApplication::translate("Karton", "the_dhcpv4_range_is_invalid_for_this_network");
                return false;
            }
        }
    }

    data->ipv6Enabled = config.value(QStringLiteral("ipv6Enabled"), false).toBool();
    if (data->ipv6Enabled) {
        if (!parseIpv6Cidr(config.value(QStringLiteral("ipv6")).toString(), &data->ipv6Network, &data->ipv6Prefix)) {
            *error = QCoreApplication::translate("Karton", "the_ipv6_network_must_be_a_valid_cidr_such_as_fd00_64");
            return false;
        }
        const QString gateway = ipv6DefaultGateway(data->ipv6Network, data->ipv6Prefix, config.value(QStringLiteral("ipv6Gateway")).toString());
        if (!parseIpv6Address(gateway, &data->ipv6Gateway)) {
            *error = QCoreApplication::translate("Karton", "the_ipv6_gateway_is_invalid");
            return false;
        }
        data->ipv6Dhcp = config.value(QStringLiteral("ipv6Dhcp"), false).toBool();
        if (data->ipv6Dhcp) {
            QString startText = config.value(QStringLiteral("ipv6DhcpStart")).toString();
            QString endText = config.value(QStringLiteral("ipv6DhcpEnd")).toString();
            if (startText.isEmpty()) {
                startText = ipv6DefaultDhcpStart(data->ipv6Network, data->ipv6Prefix);
            }
            if (endText.isEmpty()) {
                endText = ipv6DefaultDhcpEnd(data->ipv6Network, data->ipv6Prefix);
            }
            if (startText.isEmpty() || endText.isEmpty() || !parseIpv6Address(startText, &data->ipv6DhcpStart)
                || !parseIpv6Address(endText, &data->ipv6DhcpEnd)) {
                *error = QCoreApplication::translate("Karton", "the_dhcpv6_range_is_invalid_for_this_network");
                return false;
            }
        }
    }

    return true;
}

void removeDirectChildren(QDomElement &root, const QStringList &tags)
{
    for (QDomNode node = root.firstChild(); !node.isNull();) {
        QDomNode next = node.nextSibling();
        if (node.isElement() && tags.contains(node.toElement().tagName())) {
            root.removeChild(node);
        }
        node = next;
    }
}

bool applyNetworkConfig(QDomDocument &document, const NetworkConfigData &data, bool create, QString *error)
{
    QDomElement root;
    if (create) {
        root = document.createElement(QStringLiteral("network"));
        document.appendChild(root);
        QDomElement name = document.createElement(QStringLiteral("name"));
        name.appendChild(document.createTextNode(data.name));
        root.appendChild(name);
    } else {
        root = document.documentElement();
    }

    const QString existingBridgeName = root.firstChildElement(QStringLiteral("bridge")).attribute(QStringLiteral("name"));
    const QString existingForwardMode = root.firstChildElement(QStringLiteral("forward")).attribute(QStringLiteral("mode"), QStringLiteral("isolated"));
    removeDirectChildren(root, {QStringLiteral("forward"), QStringLiteral("bridge"), QStringLiteral("domain"), QStringLiteral("ip")});

    if (data.mode != QStringLiteral("isolated")) {
        QDomElement forward = document.createElement(QStringLiteral("forward"));
        forward.setAttribute(QStringLiteral("mode"), data.mode);
        if (data.mode != QStringLiteral("bridge") && !data.forwardInterface.isEmpty()) {
            QDomElement interface = document.createElement(QStringLiteral("interface"));
            interface.setAttribute(QStringLiteral("dev"), data.forwardInterface);
            forward.appendChild(interface);
        }
        root.appendChild(forward);
    }

    if (data.mode == QStringLiteral("bridge")) {
        QDomElement bridge = document.createElement(QStringLiteral("bridge"));
        bridge.setAttribute(QStringLiteral("name"), data.bridgeName);
        root.appendChild(bridge);
    } else {
        QDomElement bridge = document.createElement(QStringLiteral("bridge"));
        bridge.setAttribute(QStringLiteral("stp"), QStringLiteral("on"));
        bridge.setAttribute(QStringLiteral("delay"), QStringLiteral("0"));
        // Keep the existing bridge device when editing a non-bridge network;
        // letting libvirt allocate a new one would change the interface name
        // (virbr1 -> virbr2) and disrupt running guests.
        if (existingForwardMode != QStringLiteral("bridge") && !existingBridgeName.isEmpty()) {
            bridge.setAttribute(QStringLiteral("name"), existingBridgeName);
        }
        root.appendChild(bridge);
    }

    if (!data.domainName.isEmpty()) {
        QDomElement domain = document.createElement(QStringLiteral("domain"));
        domain.setAttribute(QStringLiteral("name"), data.domainName);
        domain.setAttribute(QStringLiteral("localOnly"), QStringLiteral("yes"));
        root.appendChild(domain);
    }

    if (data.ipv4Enabled) {
        QDomElement ip = document.createElement(QStringLiteral("ip"));
        ip.setAttribute(QStringLiteral("address"), ipv4AddressString(data.ipv4Gateway));
        ip.setAttribute(QStringLiteral("prefix"), QString::number(data.ipv4Prefix));
        if (data.ipv4Dhcp) {
            QDomElement dhcp = document.createElement(QStringLiteral("dhcp"));
            QDomElement range = document.createElement(QStringLiteral("range"));
            range.setAttribute(QStringLiteral("start"), ipv4AddressString(data.ipv4DhcpStart));
            range.setAttribute(QStringLiteral("end"), ipv4AddressString(data.ipv4DhcpEnd));
            dhcp.appendChild(range);
            ip.appendChild(dhcp);
        }
        root.appendChild(ip);
    }

    if (data.ipv6Enabled) {
        QDomElement ip = document.createElement(QStringLiteral("ip"));
        ip.setAttribute(QStringLiteral("family"), QStringLiteral("ipv6"));
        ip.setAttribute(QStringLiteral("address"), ipv6AddressString(data.ipv6Gateway));
        ip.setAttribute(QStringLiteral("prefix"), QString::number(data.ipv6Prefix));
        if (data.ipv6Dhcp) {
            QDomElement dhcp = document.createElement(QStringLiteral("dhcp"));
            QDomElement range = document.createElement(QStringLiteral("range"));
            range.setAttribute(QStringLiteral("start"), ipv6AddressString(data.ipv6DhcpStart));
            range.setAttribute(QStringLiteral("end"), ipv6AddressString(data.ipv6DhcpEnd));
            dhcp.appendChild(range);
            ip.appendChild(dhcp);
        }
        root.appendChild(ip);
    }

    Q_UNUSED(error);
    return true;
}
} // namespace

QVariantList collectNetworks(virConnectPtr conn)
{
    ScopedCallTimer timer("Karton::networks");
    QVariantList result;
    if (!conn) {
        return result;
    }

    virNetworkPtr *networkPtrs = nullptr;
    const int count = virConnectListAllNetworks(conn, &networkPtrs, VIR_CONNECT_LIST_NETWORKS_ACTIVE | VIR_CONNECT_LIST_NETWORKS_INACTIVE);
    if (count < 0) {
        return result;
    }

    for (int i = 0; i < count; ++i) {
        virNetworkPtr network = networkPtrs[i];
        const QString name = QString::fromUtf8(virNetworkGetName(network));

        int autostart = 0;
        virNetworkGetAutostart(network, &autostart);

        QString forwardMode = QStringLiteral("isolated");
        QString forwardInterface;
        QString bridge;
        QString ipv4Network;
        QString ipv4Gateway;
        QString ipv4DhcpStart;
        QString ipv4DhcpEnd;
        QString ipv6Network;
        QString ipv6Gateway;
        QString ipv6DhcpStart;
        QString ipv6DhcpEnd;
        QString domainName;
        bool ipv4Dhcp = false;
        bool ipv6Dhcp = false;

        char *xml = virNetworkGetXMLDesc(network, VIR_NETWORK_XML_INACTIVE);
        if (!xml) {
            xml = virNetworkGetXMLDesc(network, 0);
        }
        if (xml) {
            QDomDocument document;
            if (document.setContent(QString::fromUtf8(xml))) {
                const QDomElement root = document.documentElement();
                const QDomElement forward = root.firstChildElement(QStringLiteral("forward"));
                if (!forward.isNull()) {
                    forwardMode = forward.attribute(QStringLiteral("mode"), QStringLiteral("nat"));
                    const QDomElement interface = forward.firstChildElement(QStringLiteral("interface"));
                    forwardInterface = interface.attribute(QStringLiteral("dev"));
                }
                bridge = root.firstChildElement(QStringLiteral("bridge")).attribute(QStringLiteral("name"));
                domainName = root.firstChildElement(QStringLiteral("domain")).attribute(QStringLiteral("name"));

                const QDomNodeList addresses = root.elementsByTagName(QStringLiteral("ip"));
                for (int j = 0; j < addresses.count(); ++j) {
                    const QDomElement ip = addresses.at(j).toElement();
                    const QString family = ip.attribute(QStringLiteral("family"), QStringLiteral("ipv4"));
                    const QString address = ip.attribute(QStringLiteral("address"));
                    if (address.isEmpty()) {
                        continue;
                    }
                    const QDomElement dhcp = ip.firstChildElement(QStringLiteral("dhcp"));
                    const QDomElement range = dhcp.firstChildElement(QStringLiteral("range"));
                    const QString dhcpStart = range.attribute(QStringLiteral("start"));
                    const QString dhcpEnd = range.attribute(QStringLiteral("end"));

                    if (family == QStringLiteral("ipv6")) {
                        int prefix = ip.attribute(QStringLiteral("prefix")).toInt();
                        Q_IPV6ADDR networkAddress = {};
                        if (prefix > 0 && parseIpv6Address(address, &networkAddress)) {
                            maskIpv6(&networkAddress, prefix);
                            ipv6Network = ipv6CidrString(networkAddress, prefix);
                            ipv6Gateway = address;
                            ipv6Dhcp = !dhcp.isNull();
                            ipv6DhcpStart = dhcpStart;
                            ipv6DhcpEnd = dhcpEnd;
                        }
                    } else {
                        int prefix = ip.attribute(QStringLiteral("prefix")).toInt();
                        if (prefix <= 0) {
                            quint32 netmask = 0;
                            if (parseIpv4Address(ip.attribute(QStringLiteral("netmask")), &netmask)) {
                                prefix = ipv4PrefixFromNetmask(netmask);
                            }
                        }
                        quint32 networkAddress = 0;
                        if (prefix > 0 && parseIpv4Address(address, &networkAddress)) {
                            const quint32 mask = 0xFFFFFFFFu << (32 - prefix);
                            networkAddress &= mask;
                            ipv4Network = ipv4CidrString(networkAddress, prefix);
                            ipv4Gateway = address;
                            ipv4Dhcp = !dhcp.isNull();
                            ipv4DhcpStart = dhcpStart;
                            ipv4DhcpEnd = dhcpEnd;
                        }
                    }
                }
            }
            free(xml);
        }

        if (bridge.isEmpty()) {
            char *bridgeName = virNetworkGetBridgeName(network);
            if (bridgeName) {
                bridge = QString::fromUtf8(bridgeName);
                free(bridgeName);
            }
        }

        QVariantMap networkInfo;
        networkInfo.insert(QStringLiteral("name"), name);
        networkInfo.insert(QStringLiteral("active"), virNetworkIsActive(network) == 1);
        networkInfo.insert(QStringLiteral("autostart"), autostart == 1);
        networkInfo.insert(QStringLiteral("forward"), forwardMode);
        networkInfo.insert(QStringLiteral("mode"), forwardMode);
        networkInfo.insert(QStringLiteral("forwardInterface"), forwardInterface);
        networkInfo.insert(QStringLiteral("bridge"), bridge);
        networkInfo.insert(QStringLiteral("ipv4"), ipv4Network);
        networkInfo.insert(QStringLiteral("ipv4Gateway"), ipv4Gateway);
        networkInfo.insert(QStringLiteral("ipv4Dhcp"), ipv4Dhcp);
        networkInfo.insert(QStringLiteral("ipv4DhcpStart"), ipv4DhcpStart);
        networkInfo.insert(QStringLiteral("ipv4DhcpEnd"), ipv4DhcpEnd);
        networkInfo.insert(QStringLiteral("ipv6"), ipv6Network);
        networkInfo.insert(QStringLiteral("ipv6Gateway"), ipv6Gateway);
        networkInfo.insert(QStringLiteral("ipv6Dhcp"), ipv6Dhcp);
        networkInfo.insert(QStringLiteral("ipv6DhcpStart"), ipv6DhcpStart);
        networkInfo.insert(QStringLiteral("ipv6DhcpEnd"), ipv6DhcpEnd);
        networkInfo.insert(QStringLiteral("domainName"), domainName);
        result.append(networkInfo);

        virNetworkFree(network);
    }
    free(networkPtrs);
    return result;
}

QVariantMap Karton::defaultNetworkConfig()
{
    QSet<int> usedSubnets;
    const QVariantList existingNetworks = networks();
    for (const QVariant &entry : existingNetworks) {
        const QVariantMap info = entry.toMap();
        quint32 network = 0;
        int prefix = 0;
        if (!parseIpv4Cidr(info.value(QStringLiteral("ipv4")).toString(), &network, &prefix) || prefix < 24) {
            continue;
        }
        const QStringList parts = ipv4AddressString(network).split(QLatin1Char('.'));
        if (parts.size() == 4 && parts.at(0) == QStringLiteral("192") && parts.at(1) == QStringLiteral("168")) {
            usedSubnets.insert(parts.at(2).toInt());
        }
    }
    for (const QHostAddress &address : QNetworkInterface::allAddresses()) {
        const QStringList parts = address.toString().split(QLatin1Char('.'));
        if (parts.size() == 4 && parts.at(0) == QStringLiteral("192") && parts.at(1) == QStringLiteral("168")) {
            usedSubnets.insert(parts.at(2).toInt());
        }
    }

    int subnet = 100;
    for (int candidate = 100; candidate <= 250; ++candidate) {
        if (!usedSubnets.contains(candidate)) {
            subnet = candidate;
            break;
        }
    }

    const QString prefix = QStringLiteral("192.168.%1.").arg(subnet);
    const QString ipv6Prefix = QStringLiteral("fd00:%1::").arg(QString::number(subnet, 16));
    QVariantMap config;
    config.insert(QStringLiteral("mode"), QStringLiteral("nat"));
    config.insert(QStringLiteral("forwardInterface"), QString());
    config.insert(QStringLiteral("bridgeName"), QString());
    config.insert(QStringLiteral("ipv4Enabled"), true);
    config.insert(QStringLiteral("ipv4"), QString(prefix + QStringLiteral("0/24")));
    config.insert(QStringLiteral("ipv4Gateway"), QString(prefix + QStringLiteral("1")));
    config.insert(QStringLiteral("ipv4Dhcp"), true);
    config.insert(QStringLiteral("ipv4DhcpStart"), QString(prefix + QStringLiteral("128")));
    config.insert(QStringLiteral("ipv4DhcpEnd"), QString(prefix + QStringLiteral("254")));
    config.insert(QStringLiteral("ipv6Enabled"), false);
    config.insert(QStringLiteral("ipv6"), QString(ipv6Prefix + QStringLiteral("/64")));
    config.insert(QStringLiteral("ipv6Gateway"), QString(ipv6Prefix + QStringLiteral("1")));
    config.insert(QStringLiteral("ipv6Dhcp"), false);
    config.insert(QStringLiteral("ipv6DhcpStart"), QString(ipv6Prefix + QStringLiteral("100")));
    config.insert(QStringLiteral("ipv6DhcpEnd"), QString(ipv6Prefix + QStringLiteral("1ff")));
    config.insert(QStringLiteral("domainName"), QString());
    return config;
}

QVariantList Karton::hostNetworkInterfaces() const
{
    QVariantList result;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        const auto flags = interface.flags();
        if (!(flags & QNetworkInterface::IsUp) || (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        QVariantMap info;
        info.insert(QStringLiteral("name"), interface.name());
        const QString label = interface.humanReadableName();
        info.insert(QStringLiteral("label"), label.isEmpty() ? interface.name() : label);
        result.append(info);
    }
    std::sort(result.begin(), result.end(), [](const QVariant &left, const QVariant &right) {
        return left.toMap().value(QStringLiteral("name")).toString() < right.toMap().value(QStringLiteral("name")).toString();
    });
    return result;
}

virConnectPtr Karton::connectionHandleForUri(const QString &connectionUri) const
{
    if (connectionUri.isEmpty() || connectionUri == this->connectionUri()) {
        return m_conn;
    }
    if (LibvirtConnection *connection = connectionForUri(connectionUri)) {
        return connection->connection();
    }
    return nullptr;
}

virConnectPtr Karton::networkConnectionForUri(const QString &connectionUri) const
{
    return connectionHandleForUri(connectionUri);
}

virConnectPtr Karton::readerConnectionForUri(const QString &connectionUri) const
{
    if (connectionUri.isEmpty() || connectionUri == this->connectionUri()) {
        return m_readerEnabled.load() ? m_readerConn.load() : nullptr;
    }
    if (LibvirtConnection *connection = connectionForUri(connectionUri)) {
        return connection->readerConnection();
    }
    return nullptr;
}

QString Karton::domainConnectionUri(const Domain *domain) const
{
    if (!domain) {
        return connectionUri();
    }
    return connectionUriForUuid(domain->config()->uuid());
}

bool Karton::connectionIsRemote(const QString &connectionUri) const
{
    // Development/testing switch: exercise the remote code paths on a local
    // connection without a remote libvirt.
    if (qEnvironmentVariableIsSet("KARTON_FORCE_REMOTE")) {
        return true;
    }
    const QString uri = connectionUri.isEmpty() ? this->connectionUri() : connectionUri;
    return !QUrl(uri).host().isEmpty();
}

QVariantList Karton::networks(const QString &connectionUri)
{
    ScopedCallTimer timer("Karton::networks");
    const QString uri = connectionUri.isEmpty() ? this->connectionUri() : connectionUri;
    if (!m_reader || !readerConnectionForUri(uri)) {
        virConnectPtr conn = networkConnectionForUri(uri);
        return conn ? collectNetworks(conn) : QVariantList{};
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_networksTimes.contains(uri) || m_networksTimes.value(uri) == 0 || now - m_networksTimes.value(uri) > 5000) {
        requestNetworks(uri);
    }
    return m_networksCache.value(uri);
}

void Karton::requestNetworks(const QString &connectionUri)
{
    const QString uri = connectionUri.isEmpty() ? this->connectionUri() : connectionUri;
    virConnectPtr readerConn = readerConnectionForUri(uri);
    if (!m_reader || !readerConn || m_networksPending.contains(uri)) {
        return;
    }
    m_networksPending.insert(uri);
    postReaderJob([this, uri, readerConn]() {
        const QVariantList result = collectNetworks(readerConn);
        QMetaObject::invokeMethod(
            this,
            [this, uri, result]() {
                m_networksPending.remove(uri);
                m_networksCache.insert(uri, result);
                m_networksTimes.insert(uri, QDateTime::currentMSecsSinceEpoch());
                Q_EMIT networksUpdated();
            },
            Qt::QueuedConnection);
    });
}

bool Karton::createNetwork(const QVariantMap &config)
{
    const QString connectionUri = config.value(QStringLiteral("connectionUri")).toString();
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    if (!conn) {
        return false;
    }

    NetworkConfigData data;
    QString error;
    if (!parseNetworkConfig(config, &data, &error)) {
        Q_EMIT errorOccurred(error);
        return false;
    }

    virNetworkPtr existing = virNetworkLookupByName(conn, data.name.toUtf8().constData());
    if (existing) {
        virNetworkFree(existing);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_already_exists").arg(data.name));
        return false;
    }

    QDomDocument document;
    if (!applyNetworkConfig(document, data, true, &error)) {
        Q_EMIT errorOccurred(error);
        return false;
    }

    virNetworkPtr network = virNetworkDefineXML(conn, document.toString().toUtf8().constData());
    if (!network) {
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "failed_to_define_network_1_2").arg(data.name).arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }

    if (virNetworkCreate(network) != 0) {
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "failed_to_start_network_1_2").arg(data.name).arg(QString::fromUtf8(virGetLastErrorMessage())));
        virNetworkUndefine(network);
        virNetworkFree(network);
        return false;
    }

    const bool autostart = config.value(QStringLiteral("autostart"), false).toBool();
    if (virNetworkSetAutostart(network, autostart ? 1 : 0) != 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_change_autostart_for_network_1_2")
                                 .arg(data.name)
                                 .arg(QString::fromUtf8(virGetLastErrorMessage())));
    }

    virNetworkFree(network);
    m_networksTimes[connectionUri] = 0;
    Q_EMIT networksChanged();
    return true;
}

bool Karton::updateNetwork(const QVariantMap &config)
{
    const QString connectionUri = config.value(QStringLiteral("connectionUri")).toString();
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    if (!conn) {
        return false;
    }

    NetworkConfigData data;
    QString error;
    if (!parseNetworkConfig(config, &data, &error)) {
        Q_EMIT errorOccurred(error);
        return false;
    }

    virNetworkPtr oldNetwork = virNetworkLookupByName(conn, data.name.toUtf8().constData());
    if (!oldNetwork) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_was_not_found").arg(data.name));
        return false;
    }
    const bool wasActive = virNetworkIsActive(oldNetwork) == 1;

    char *xml = virNetworkGetXMLDesc(oldNetwork, VIR_NETWORK_XML_INACTIVE);
    if (!xml) {
        xml = virNetworkGetXMLDesc(oldNetwork, 0);
    }
    QDomDocument document;
    if (!xml || !document.setContent(QString::fromUtf8(xml))) {
        free(xml);
        virNetworkFree(oldNetwork);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_read_the_configuration_of_network_1").arg(data.name));
        return false;
    }
    free(xml);

    if (!applyNetworkConfig(document, data, false, &error)) {
        virNetworkFree(oldNetwork);
        Q_EMIT errorOccurred(error);
        return false;
    }

    virNetworkPtr newNetwork = virNetworkDefineXML(conn, document.toString().toUtf8().constData());
    virNetworkFree(oldNetwork);
    if (!newNetwork) {
        Q_EMIT errorOccurred(
            QCoreApplication::translate("Karton", "failed_to_update_network_1_2").arg(data.name).arg(QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }

    if (wasActive) {
        // Apply the new definition to the running network as well. A brief
        // restart is required because libvirt cannot change a live network's
        // addressing in place.
        if (virNetworkDestroy(newNetwork) != 0 || virNetworkCreate(newNetwork) != 0) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_was_updated_but_could_not_be_restarted_2")
                                     .arg(data.name)
                                     .arg(QString::fromUtf8(virGetLastErrorMessage())));
            virNetworkFree(newNetwork);
            return false;
        }
    }

    const bool autostart = config.value(QStringLiteral("autostart"), false).toBool();
    if (virNetworkSetAutostart(newNetwork, autostart ? 1 : 0) != 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_change_autostart_for_network_1_2")
                                 .arg(data.name)
                                 .arg(QString::fromUtf8(virGetLastErrorMessage())));
    }

    virNetworkFree(newNetwork);
    m_networksTimes[connectionUri] = 0;
    Q_EMIT networksChanged();
    return true;
}

bool Karton::startNetwork(const QString &name, const QString &connectionUri)
{
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    if (!conn) {
        return false;
    }

    virNetworkPtr network = virNetworkLookupByName(conn, name.toUtf8().constData());
    if (!network) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_find_network_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const bool success = virNetworkCreate(network) == 0;
    if (!success) {
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_start_network_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }
    virNetworkFree(network);
    if (success) {
        m_networksTimes[connectionUri] = 0;
        Q_EMIT networksChanged();
    }
    return success;
}

bool Karton::stopNetwork(const QString &name, const QString &connectionUri)
{
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    if (!conn) {
        return false;
    }

    virNetworkPtr network = virNetworkLookupByName(conn, name.toUtf8().constData());
    if (!network) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_find_network_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const bool success = virNetworkDestroy(network) == 0;
    if (!success) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_stop_network_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }
    virNetworkFree(network);
    if (success) {
        m_networksTimes[connectionUri] = 0;
        Q_EMIT networksChanged();
    }
    return success;
}

bool Karton::setNetworkAutostart(const QString &name, bool autostart, const QString &connectionUri)
{
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    virNetworkPtr network = virNetworkLookupByName(conn, name.toUtf8().constData());
    if (!network) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_find_network_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const bool success = virNetworkSetAutostart(network, autostart ? 1 : 0) == 0;
    if (!success) {
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_change_autostart_for_network_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }
    virNetworkFree(network);
    if (success) {
        m_networksTimes[connectionUri] = 0;
        Q_EMIT networksChanged();
    }
    return success;
}

bool Karton::createNatNetwork(const QString &requestedName, const QString &connectionUri)
{
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    if (!conn) {
        return false;
    }

    QString name = requestedName.trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("karton-nat");
    }
    if (!isValidDomainName(name)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "a_network_name_may_only_contain_letters_digits_dots_underscores_and_dashes"));
        return false;
    }

    virNetworkPtr existing = virNetworkLookupByName(conn, name.toUtf8().constData());
    if (existing) {
        virNetworkFree(existing);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_already_exists").arg(name));
        return false;
    }

    // Find a 192.168.X.0/24 subnet that is not used by another libvirt
    // network or by a host interface.
    QSet<int> usedSubnets;
    virNetworkPtr *networkPtrs = nullptr;
    const int count = virConnectListAllNetworks(conn, &networkPtrs, VIR_CONNECT_LIST_NETWORKS_ACTIVE | VIR_CONNECT_LIST_NETWORKS_INACTIVE);
    if (count > 0 && networkPtrs) {
        for (int i = 0; i < count; ++i) {
            char *xml = virNetworkGetXMLDesc(networkPtrs[i], 0);
            if (!xml) {
                virNetworkFree(networkPtrs[i]);
                continue;
            }
            QDomDocument document;
            if (document.setContent(QString::fromUtf8(xml))) {
                const QDomNodeList addresses = document.elementsByTagName(QStringLiteral("ip"));
                for (int j = 0; j < addresses.count(); ++j) {
                    const QString address = addresses.at(j).toElement().attribute(QStringLiteral("address"));
                    const QStringList parts = address.split(QLatin1Char('.'));
                    if (parts.size() == 4 && parts.at(0) == QStringLiteral("192") && parts.at(1) == QStringLiteral("168")) {
                        usedSubnets.insert(parts.at(2).toInt());
                    }
                }
            }
            free(xml);
            virNetworkFree(networkPtrs[i]);
        }
        free(networkPtrs);
    }
    for (const QHostAddress &address : QNetworkInterface::allAddresses()) {
        const QStringList parts = address.toString().split(QLatin1Char('.'));
        if (parts.size() == 4 && parts.at(0) == QStringLiteral("192") && parts.at(1) == QStringLiteral("168")) {
            usedSubnets.insert(parts.at(2).toInt());
        }
    }

    int subnet = 0;
    for (int candidate = 100; candidate <= 250; ++candidate) {
        if (!usedSubnets.contains(candidate)) {
            subnet = candidate;
            break;
        }
    }
    if (subnet == 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "could_not_find_a_free_192_168_x_0_24_subnet_for_the_new_network"));
        return false;
    }

    const QString xml = QStringLiteral(
                            "<network>"
                            "<name>%1</name>"
                            "<forward mode='nat'/>"
                            "<bridge stp='on' delay='0'/>"
                            "<ip address='192.168.%2.1' netmask='255.255.255.0'>"
                            "<dhcp><range start='192.168.%2.2' end='192.168.%2.254'/></dhcp>"
                            "</ip>"
                            "</network>")
                            .arg(name)
                            .arg(subnet);

    virNetworkPtr network = virNetworkDefineXML(conn, xml.toUtf8().constData());
    if (!network) {
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_define_network_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    if (virNetworkCreate(network) != 0) {
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_start_network_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        virNetworkUndefine(network);
        virNetworkFree(network);
        return false;
    }

    virNetworkFree(network);
    m_networksTimes[connectionUri] = 0;
    Q_EMIT networksChanged();
    return true;
}

namespace
{
struct GuestInterfaceData {
    QString name;
    QStringList addresses;
};

// Loopback, IPv4 link-local (APIPA) and IPv6 link-local addresses are real
// addresses of the guest but cannot be reached from the host, so they only
// add noise to the VM page and to the port forwarding address list.
QStringList usableInterfaceAddresses(const QStringList &addresses)
{
    QStringList usable;
    for (const QString &address : addresses) {
        if (address == QLatin1String("127.0.0.1") || address == QLatin1String("::1") || address.startsWith(QLatin1String("169.254."))
            || address.startsWith(QLatin1String("fe80:"), Qt::CaseInsensitive)) {
            continue;
        }
        usable.append(address);
    }
    // Show IPv4 first, which is what a user normally wants.
    std::stable_sort(usable.begin(), usable.end(), [](const QString &left, const QString &right) {
        return !left.contains(QLatin1Char(':')) && right.contains(QLatin1Char(':'));
    });
    return usable;
}

bool interfaceAddressMissing(const QHash<QString, GuestInterfaceData> &interfaces, const QSet<QString> &macs)
{
    for (const QString &mac : macs) {
        if (usableInterfaceAddresses(interfaces.value(mac).addresses).isEmpty()) {
            return true;
        }
    }
    return false;
}

void collectInterfaceAddresses(virDomainPtr domain,
                               unsigned int source,
                               bool readName,
                               const QSet<QString> &wantedMacs,
                               QHash<QString, GuestInterfaceData> *interfaces)
{
    virDomainInterfacePtr *addresses = nullptr;
    const int count = virDomainInterfaceAddresses(domain, &addresses, source, 0);
    if (count <= 0) {
        // A missing lease/ARP entry/agent reply is an expected state, not an
        // error worth leaving behind for the next unrelated libvirt call.
        free(addresses);
        virResetLastError();
        return;
    }

    for (int i = 0; i < count; ++i) {
        const virDomainInterface *interface = addresses[i];
        const QString mac = interface->hwaddr ? QString::fromUtf8(interface->hwaddr).toLower() : QString();
        if (mac.isEmpty() || !wantedMacs.contains(mac)) {
            virDomainInterfaceFree(addresses[i]);
            continue;
        }

        GuestInterfaceData &data = (*interfaces)[mac];
        // The lease and ARP sources report the host-side interface name
        // (vnet0), so only the guest agent is allowed to name the interface.
        if (readName && data.name.isEmpty() && interface->name && *interface->name) {
            data.name = QString::fromUtf8(interface->name);
        }
        for (unsigned int j = 0; j < interface->naddrs; ++j) {
            const QString address = QString::fromUtf8(interface->addrs[j].addr).trimmed();
            if (!address.isEmpty() && !data.addresses.contains(address)) {
                data.addresses.append(address);
            }
        }
        virDomainInterfaceFree(addresses[i]);
    }
    free(addresses);
}
} // namespace

QVariantList collectInterfaces(virDomainPtr domainPtr)
{
    ScopedCallTimer timer("Karton::domainInterfaces");
    QVariantList result;
    if (!domainPtr) {
        return result;
    }

    char *xml = virDomainGetXMLDesc(domainPtr, VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domainPtr, 0);
    }
    if (!xml) {
        return result;
    }

    struct ParsedInterface {
        QString mac;
        QVariantMap info;
    };
    QList<ParsedInterface> parsedInterfaces;
    QSet<QString> macs;

    QDomDocument document;
    if (document.setContent(QString::fromUtf8(xml))) {
        const QDomNodeList interfaces = document.elementsByTagName(QStringLiteral("interface"));
        for (int i = 0; i < interfaces.count(); ++i) {
            const QDomElement element = interfaces.at(i).toElement();
            const QDomElement macElement = element.firstChildElement(QStringLiteral("mac"));
            const QDomElement sourceElement = element.firstChildElement(QStringLiteral("source"));
            const QDomElement modelElement = element.firstChildElement(QStringLiteral("model"));

            QString source;
            if (!sourceElement.isNull()) {
                source = sourceElement.attribute(QStringLiteral("network"));
                if (source.isEmpty()) {
                    source = sourceElement.attribute(QStringLiteral("bridge"));
                }
                if (source.isEmpty()) {
                    source = sourceElement.attribute(QStringLiteral("dev"));
                }
            }

            QVariantMap interfaceInfo;
            interfaceInfo.insert(QStringLiteral("type"), element.attribute(QStringLiteral("type")));
            interfaceInfo.insert(QStringLiteral("mac"), macElement.attribute(QStringLiteral("address")));
            interfaceInfo.insert(QStringLiteral("source"), source);
            interfaceInfo.insert(QStringLiteral("model"), modelElement.attribute(QStringLiteral("type")));

            const QString mac = macElement.attribute(QStringLiteral("address")).toLower();
            if (!mac.isEmpty()) {
                macs.insert(mac);
            }
            parsedInterfaces.append({mac, interfaceInfo});
        }
    }

    free(xml);

    QHash<QString, GuestInterfaceData> guestInterfaces;
    if (virDomainIsActive(domainPtr) == 1 && !macs.isEmpty()) {
        // The agent is the only source that knows the guest-side interface
        // name, so query it first. Its reply is not proof that an address is
        // known yet: the Windows agent reports every adapter (including the
        // NIC right after boot, before DHCP has completed and while it only
        // has an APIPA/link-local address). Always fill the gaps from the
        // DHCP lease and then from the host ARP table instead of stopping at
        // the first source that returned any interface at all.
        collectInterfaceAddresses(domainPtr, VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT, true, macs, &guestInterfaces);
        if (interfaceAddressMissing(guestInterfaces, macs)) {
            collectInterfaceAddresses(domainPtr, VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_LEASE, false, macs, &guestInterfaces);
        }
        if (interfaceAddressMissing(guestInterfaces, macs)) {
            collectInterfaceAddresses(domainPtr, VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_ARP, false, macs, &guestInterfaces);
        }
    }

    for (const ParsedInterface &parsed : parsedInterfaces) {
        const GuestInterfaceData data = guestInterfaces.value(parsed.mac);
        QVariantMap interfaceInfo = parsed.info;
        interfaceInfo.insert(QStringLiteral("name"), data.name);
        interfaceInfo.insert(QStringLiteral("ip"), usableInterfaceAddresses(data.addresses).join(QStringLiteral(", ")));
        result.append(interfaceInfo);
    }

    return result;
}

static QString interfaceXmlForMac(virDomainPtr domain, const QString &mac, unsigned int xmlFlags)
{
    if (!domain || mac.isEmpty()) {
        return {};
    }

    char *xml = virDomainGetXMLDesc(domain, xmlFlags);
    if (!xml) {
        return {};
    }

    QString deviceXml;
    QDomDocument document;
    if (document.setContent(QString::fromUtf8(xml))) {
        const QDomNodeList interfaces = document.elementsByTagName(QStringLiteral("interface"));
        for (int i = 0; i < interfaces.count(); ++i) {
            const QDomElement element = interfaces.at(i).toElement();
            const QDomElement macElement = element.firstChildElement(QStringLiteral("mac"));
            if (macElement.attribute(QStringLiteral("address")).compare(mac, Qt::CaseInsensitive) == 0) {
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

bool Karton::deleteNetwork(const QString &name, const QString &connectionUri)
{
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    if (!conn || name.isEmpty()) {
        return false;
    }
    virNetworkPtr network = virNetworkLookupByName(conn, name.toUtf8().constData());
    if (!network) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_was_not_found").arg(name));
        return false;
    }
    const int active = virNetworkIsActive(network);
    if (active == 1) {
        virNetworkDestroy(network);
    }
    const bool ok = virNetworkUndefine(network) == 0;
    if (!ok) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_delete_network_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage())));
    }
    virNetworkFree(network);
    if (ok) {
        m_networksTimes[connectionUri] = 0;
        Q_EMIT networksChanged();
    }
    return ok;
}

bool Karton::attachNetwork(const Domain *domain, const QString &network, const QString &model)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }
    if (!isValidDomainName(network)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_network_name"));
        return false;
    }

    const QString networkModel = (model == QStringLiteral("e1000")) ? QStringLiteral("e1000") : QStringLiteral("virtio");
    const quint32 random = QRandomGenerator::global()->generate();
    const QString mac = QStringLiteral("52:54:00:%1:%2:%3")
                            .arg((random >> 16) & 0xff, 2, 16, QLatin1Char('0'))
                            .arg((random >> 8) & 0xff, 2, 16, QLatin1Char('0'))
                            .arg(random & 0xff, 2, 16, QLatin1Char('0'));

    const QString xml = QStringLiteral(
                            "<interface type='network'>"
                            "<mac address='%1'/>"
                            "<source network='%2'/>"
                            "<model type='%3'/>"
                            "</interface>")
                            .arg(mac, network, networkModel);

    bool success = false;
    if (domain->isActive()) {
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg =
                QCoreApplication::translate("Karton", "failed_to_attach_network_1_2").arg(network).arg(QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
        } else {
            success = true;
        }

        // Keep the interface after shutdown too. This is a separate call: some
        // libvirt versions reject LIVE|CONFIG when the live device is already
        // present.
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to persist attached network interface:" << virGetLastErrorMessage();
        }
    } else if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) == 0) {
        success = true;
    } else {
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_attach_network_1_2").arg(network).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

bool Karton::detachNetwork(const Domain *domain, const QString &mac)
{
    if (!domain || !domain->domainPtr() || mac.isEmpty()) {
        return false;
    }

    const QString liveXml = interfaceXmlForMac(domain->domainPtr(), mac, 0);
    const QString configXml = interfaceXmlForMac(domain->domainPtr(), mac, VIR_DOMAIN_XML_INACTIVE);
    if (liveXml.isEmpty() && configXml.isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "no_interface_with_mac_address_1_was_found").arg(mac));
        return false;
    }

    bool success = true;
    if (domain->isActive() && !liveXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), liveXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg =
                QCoreApplication::translate("Karton", "failed_to_detach_interface_1_2").arg(mac).arg(QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (!configXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), configXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            const QString errorMsg =
                QCoreApplication::translate("Karton", "failed_to_detach_persistent_interface_1_2").arg(mac).arg(QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

bool Karton::editNetworkInterface(const Domain *domain, const QString &mac, const QString &newMac, const QString &model)
{
    if (!domain || !domain->domainPtr() || mac.isEmpty() || newMac.isEmpty()) {
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

    const QDomNodeList interfaces = document.elementsByTagName(QStringLiteral("interface"));
    for (int i = 0; i < interfaces.count(); ++i) {
        QDomElement interface = interfaces.at(i).toElement();
        QDomElement macElement = interface.firstChildElement(QStringLiteral("mac"));
        if (macElement.attribute(QStringLiteral("address")).compare(mac, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (macElement.isNull()) {
            macElement = document.createElement(QStringLiteral("mac"));
            interface.insertBefore(macElement, interface.firstChild());
        }
        macElement.setAttribute(QStringLiteral("address"), newMac);
        QDomElement modelElement = interface.firstChildElement(QStringLiteral("model"));
        if (modelElement.isNull()) {
            modelElement = document.createElement(QStringLiteral("model"));
            interface.appendChild(modelElement);
        }
        modelElement.setAttribute(QStringLiteral("type"), model);

        virDomainPtr redefined = virDomainDefineXML(virDomainGetConnect(domain->domainPtr()), document.toString().toUtf8().constData());
        if (!redefined) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_update_interface_for_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage())));
            return false;
        }
        virDomainFree(redefined);
        refreshDomainList();
        return true;
    }

    Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "no_interface_with_mac_address_1_was_found").arg(mac));
    return false;
}

QVariantList Karton::networkLeases(const QString &name, const QString &connectionUri) const
{
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    ScopedCallTimer timer("Karton::networkLeases");
    QVariantList result;
    if (!conn || name.isEmpty()) {
        return result;
    }
    virNetworkPtr network = virNetworkLookupByName(conn, name.toUtf8().constData());
    if (!network) {
        return result;
    }

    virNetworkDHCPLeasePtr *leases = nullptr;
    const int count = virNetworkGetDHCPLeases(network, nullptr, &leases, 0);
    for (int i = 0; i < count; ++i) {
        const virNetworkDHCPLeasePtr lease = leases[i];
        QVariantMap info;
        info.insert(QStringLiteral("mac"), QString::fromUtf8(lease->mac ? lease->mac : ""));
        info.insert(QStringLiteral("ip"), QString::fromUtf8(lease->ipaddr ? lease->ipaddr : ""));
        info.insert(QStringLiteral("hostname"), QString::fromUtf8(lease->hostname ? lease->hostname : ""));
        info.insert(QStringLiteral("clientId"), QString::fromUtf8(lease->clientid ? lease->clientid : ""));
        info.insert(QStringLiteral("expiryTime"), QDateTime::fromSecsSinceEpoch(lease->expirytime).toString(Qt::ISODate));
        info.insert(QStringLiteral("type"), lease->type == VIR_IP_ADDR_TYPE_IPV6 ? QStringLiteral("ipv6") : QStringLiteral("ipv4"));
        result.append(info);
        virNetworkDHCPLeaseFree(lease);
    }
    free(leases);
    virNetworkFree(network);
    return result;
}

QVariantList Karton::networkDhcpHosts(const QString &name, const QString &connectionUri) const
{
    const QString scopeUri = (connectionUri.isEmpty() || connectionUri == this->connectionUri()) ? QString() : connectionUri;
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    QVariantList result;
    if (!conn || name.isEmpty()) {
        return result;
    }
    virNetworkPtr network = virNetworkLookupByName(conn, name.toUtf8().constData());
    if (!network) {
        return result;
    }

    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    char *xml = virNetworkGetXMLDesc(network, VIR_NETWORK_XML_INACTIVE);
    if (xml) {
        QDomDocument document;
        if (document.setContent(QString::fromUtf8(xml))) {
            const QDomNodeList hosts = document.elementsByTagName(QStringLiteral("host"));
            for (int i = 0; i < hosts.count(); ++i) {
                const QDomElement host = hosts.at(i).toElement();
                const QString mac = host.attribute(QStringLiteral("mac"));
                QVariantMap info;
                info.insert(QStringLiteral("mac"), mac);
                info.insert(QStringLiteral("ip"), host.attribute(QStringLiteral("ip")));
                info.insert(QStringLiteral("hostname"), host.attribute(QStringLiteral("name")));
                info.insert(QStringLiteral("addedAt"), settings.value(dhcpReservationKey(scopeUri, name, mac) + QStringLiteral("/addedAt")).toString());
                result.append(info);
            }
        }
        free(xml);
    }
    virNetworkFree(network);
    return result;
}

QVariantList Karton::networkDhcpCandidates(const QString &name, const QString &connectionUri) const
{
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    QVariantList result;
    if (!conn || name.isEmpty()) {
        return result;
    }

    QSet<QString> reservedMacs;
    for (const QVariant &entry : networkDhcpHosts(name)) {
        reservedMacs.insert(entry.toMap().value(QStringLiteral("mac")).toString().toLower());
    }

    for (const Domain *domain : std::as_const(m_domains)) {
        if (!domain || !domain->domainPtr()) {
            continue;
        }
        char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
        if (!xml) {
            continue;
        }
        QDomDocument document;
        if (document.setContent(QString::fromUtf8(xml))) {
            const QDomNodeList interfaces = document.elementsByTagName(QStringLiteral("interface"));
            for (int i = 0; i < interfaces.count(); ++i) {
                const QDomElement element = interfaces.at(i).toElement();
                if (element.attribute(QStringLiteral("type")) != QStringLiteral("network")) {
                    continue;
                }
                const QDomElement source = element.firstChildElement(QStringLiteral("source"));
                if (source.attribute(QStringLiteral("network")) != name) {
                    continue;
                }
                const QString mac = element.firstChildElement(QStringLiteral("mac")).attribute(QStringLiteral("address")).toLower();
                if (mac.isEmpty()) {
                    continue;
                }
                const QString model = element.firstChildElement(QStringLiteral("model")).attribute(QStringLiteral("type"));
                QVariantMap info;
                info.insert(QStringLiteral("vmName"), domain->config()->name());
                info.insert(QStringLiteral("uuid"), domain->config()->uuid());
                info.insert(QStringLiteral("mac"), mac);
                info.insert(QStringLiteral("model"), model);
                info.insert(QStringLiteral("label"), model.isEmpty() ? mac : mac + QStringLiteral(" · ") + model);
                info.insert(QStringLiteral("reserved"), reservedMacs.contains(mac));
                result.append(info);
            }
        }
        free(xml);
    }

    // Stable order: VM name first, then MAC, so both combo boxes and the
    // reservation list keep their order between refreshes.
    std::sort(result.begin(), result.end(), [](const QVariant &left, const QVariant &right) {
        const QVariantMap leftMap = left.toMap();
        const QVariantMap rightMap = right.toMap();
        const QString leftName = leftMap.value(QStringLiteral("vmName")).toString();
        const QString rightName = rightMap.value(QStringLiteral("vmName")).toString();
        if (leftName != rightName) {
            return leftName.localeAwareCompare(rightName) < 0;
        }
        return leftMap.value(QStringLiteral("mac")).toString() < rightMap.value(QStringLiteral("mac")).toString();
    });
    return result;
}

bool Karton::addNetworkDhcpHost(const QString &name, const QString &mac, const QString &ip, const QString &connectionUri)
{
    const QString scopeUri = (connectionUri.isEmpty() || connectionUri == this->connectionUri()) ? QString() : connectionUri;
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    if (!conn) {
        return false;
    }
    static const QRegularExpression macPattern(QStringLiteral("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$"));
    const QString normalizedMac = mac.trimmed().toLower();
    if (!macPattern.match(normalizedMac).hasMatch()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_mac_address_1").arg(mac));
        return false;
    }
    QHostAddress address;
    if (!address.setAddress(ip.trimmed()) || address.protocol() != QAbstractSocket::IPv4Protocol) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_ipv4_address_1").arg(ip));
        return false;
    }

    virNetworkPtr network = virNetworkLookupByName(conn, name.toUtf8().constData());
    if (!network) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_was_not_found").arg(name));
        return false;
    }

    char *networkXml = virNetworkGetXMLDesc(network, VIR_NETWORK_XML_INACTIVE);
    int parentIndex = -1;
    QString ipv4Address;
    QString ipv4Netmask;
    int ipv4Prefix = 0;
    bool macAlreadyReserved = false;
    bool ipAlreadyReserved = false;
    QSet<QString> hostNames;
    if (networkXml) {
        QDomDocument document;
        if (document.setContent(QString::fromUtf8(networkXml))) {
            const QDomNodeList addresses = document.documentElement().elementsByTagName(QStringLiteral("ip"));
            for (int i = 0; i < addresses.count(); ++i) {
                const QDomElement candidate = addresses.at(i).toElement();
                if (candidate.attribute(QStringLiteral("family"), QStringLiteral("ipv4")) == QStringLiteral("ipv4")) {
                    parentIndex = i;
                    ipv4Address = candidate.attribute(QStringLiteral("address"));
                    ipv4Netmask = candidate.attribute(QStringLiteral("netmask"));
                    ipv4Prefix = candidate.attribute(QStringLiteral("prefix")).toInt();
                    break;
                }
            }
            const QDomNodeList hosts = document.elementsByTagName(QStringLiteral("host"));
            for (int i = 0; i < hosts.count(); ++i) {
                const QDomElement host = hosts.at(i).toElement();
                if (host.attribute(QStringLiteral("mac")).compare(normalizedMac, Qt::CaseInsensitive) == 0) {
                    macAlreadyReserved = true;
                }
                if (host.attribute(QStringLiteral("ip")) == address.toString()) {
                    ipAlreadyReserved = true;
                }
                const QString hostName = host.attribute(QStringLiteral("name")).toLower();
                if (!hostName.isEmpty()) {
                    hostNames.insert(hostName);
                }
            }
        }
        free(networkXml);
    }
    if (parentIndex < 0) {
        virNetworkFree(network);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_has_no_ipv4_dhcp_configuration").arg(name));
        return false;
    }
    if (ipv4Prefix <= 0) {
        quint32 netmask = 0;
        if (parseIpv4Address(ipv4Netmask, &netmask)) {
            ipv4Prefix = ipv4PrefixFromNetmask(netmask);
        }
    }
    quint32 gatewayAddress = 0;
    if (ipv4Prefix <= 0 || !parseIpv4Address(ipv4Address, &gatewayAddress)) {
        virNetworkFree(network);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_has_no_ipv4_dhcp_configuration").arg(name));
        return false;
    }

    const quint32 mask = 0xFFFFFFFFu << (32 - ipv4Prefix);
    const quint32 networkAddress = gatewayAddress & mask;
    const quint32 broadcastAddress = networkAddress | ~mask;
    const quint32 requestedAddress = address.toIPv4Address();
    if (!ipv4InNetwork(requestedAddress, networkAddress, ipv4Prefix)) {
        virNetworkFree(network);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_ip_address_1_is_not_inside_the_network_2")
                                 .arg(address.toString(), ipv4CidrString(networkAddress, ipv4Prefix)));
        return false;
    }
    if (requestedAddress == networkAddress || requestedAddress == broadcastAddress || requestedAddress == gatewayAddress) {
        virNetworkFree(network);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_ip_address_1_cannot_be_used_for_a_reservation").arg(address.toString()));
        return false;
    }
    if (macAlreadyReserved) {
        virNetworkFree(network);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_mac_address_1_already_has_a_reservation").arg(normalizedMac));
        return false;
    }
    if (ipAlreadyReserved) {
        virNetworkFree(network);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_ip_address_1_is_already_reserved").arg(address.toString()));
        return false;
    }

    // A reservation belongs to a VM interface in this network. Deriving the
    // host name from that VM keeps the libvirt entry and the UI in sync.
    QString vmName;
    for (const Domain *domain : std::as_const(m_domains)) {
        if (!domain || !domain->domainPtr()) {
            continue;
        }
        char *domainXml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
        if (!domainXml) {
            continue;
        }
        QDomDocument document;
        if (document.setContent(QString::fromUtf8(domainXml))) {
            const QDomNodeList interfaces = document.elementsByTagName(QStringLiteral("interface"));
            for (int i = 0; i < interfaces.count(); ++i) {
                const QDomElement element = interfaces.at(i).toElement();
                if (element.attribute(QStringLiteral("type")) != QStringLiteral("network")) {
                    continue;
                }
                if (element.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("network")) != name) {
                    continue;
                }
                if (element.firstChildElement(QStringLiteral("mac")).attribute(QStringLiteral("address")).compare(normalizedMac, Qt::CaseInsensitive) != 0) {
                    continue;
                }
                vmName = domain->config()->name();
                break;
            }
        }
        free(domainXml);
        if (!vmName.isEmpty()) {
            break;
        }
    }
    if (vmName.isEmpty()) {
        virNetworkFree(network);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_mac_address_1_is_not_connected_to_network_2").arg(normalizedMac, name));
        return false;
    }

    // The DHCP host name ends up in dnsmasq's configuration, so it must be a
    // plain ASCII host name. The UI resolves the VM name from the MAC anyway.
    QString baseHostname = vmName;
    baseHostname.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9-]")), QStringLiteral("-"));
    while (baseHostname.startsWith(QLatin1Char('-'))) {
        baseHostname.remove(0, 1);
    }
    while (baseHostname.endsWith(QLatin1Char('-'))) {
        baseHostname.chop(1);
    }
    if (baseHostname.isEmpty()) {
        baseHostname = QStringLiteral("vm");
    }
    if (baseHostname.size() > 63) {
        baseHostname.truncate(63);
        while (baseHostname.endsWith(QLatin1Char('-'))) {
            baseHostname.chop(1);
        }
    }

    // libvirt rejects a second host entry with the same name, but one VM can
    // have several interfaces in the same network. Keep the VM name for the
    // first reservation and add a numeric suffix for the rest.
    QString hostname = baseHostname;
    if (hostNames.contains(hostname.toLower())) {
        for (int suffix = 2; suffix < 1000; ++suffix) {
            const QString candidate = QStringLiteral("%1-%2").arg(baseHostname).arg(suffix);
            if (!hostNames.contains(candidate.toLower())) {
                hostname = candidate;
                break;
            }
        }
    }
    QString escapedHostname = hostname;
    escapedHostname.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    escapedHostname.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    escapedHostname.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    escapedHostname.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    QString hostXml = QStringLiteral("<host mac='%1' ip='%2'").arg(normalizedMac, address.toString());
    hostXml += QStringLiteral(" name='%1'").arg(escapedHostname);
    hostXml += QStringLiteral("/>");

    unsigned int flags = VIR_NETWORK_UPDATE_AFFECT_CONFIG;
    if (virNetworkIsActive(network) == 1) {
        flags |= VIR_NETWORK_UPDATE_AFFECT_LIVE;
    }
    // virNetworkUpdate takes the command before the section (see
    // libvirt-network.h); passing the section first updates the wrong element.
    const int result =
        virNetworkUpdate(network, VIR_NETWORK_UPDATE_COMMAND_ADD_LAST, VIR_NETWORK_SECTION_IP_DHCP_HOST, parentIndex, hostXml.toUtf8().constData(), flags);
    if (result < 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_add_the_dhcp_reservation_1").arg(QString::fromUtf8(virGetLastErrorMessage())));
    }
    virNetworkFree(network);
    if (result == 0) {
        QSettings settings(kartonConfigPath(), QSettings::IniFormat);
        settings.setValue(dhcpReservationKey(scopeUri, name, normalizedMac) + QStringLiteral("/addedAt"),
                          QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        settings.sync();
        m_networksTimes[connectionUri] = 0;
        Q_EMIT networksChanged();
    }
    return result == 0;
}

bool Karton::removeNetworkDhcpHost(const QString &name, const QString &mac, const QString &connectionUri)
{
    const QString scopeUri = (connectionUri.isEmpty() || connectionUri == this->connectionUri()) ? QString() : connectionUri;
    virConnectPtr conn = networkConnectionForUri(connectionUri);

    if (!conn) {
        return false;
    }
    virNetworkPtr network = virNetworkLookupByName(conn, name.toUtf8().constData());
    if (!network) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_was_not_found").arg(name));
        return false;
    }

    char *networkXml = virNetworkGetXMLDesc(network, VIR_NETWORK_XML_INACTIVE);
    int parentIndex = -1;
    if (networkXml) {
        QDomDocument document;
        if (document.setContent(QString::fromUtf8(networkXml))) {
            const QDomNodeList addresses = document.documentElement().elementsByTagName(QStringLiteral("ip"));
            for (int i = 0; i < addresses.count(); ++i) {
                if (addresses.at(i).toElement().attribute(QStringLiteral("family"), QStringLiteral("ipv4")) == QStringLiteral("ipv4")) {
                    parentIndex = i;
                    break;
                }
            }
        }
        free(networkXml);
    }
    if (parentIndex < 0) {
        virNetworkFree(network);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_has_no_ipv4_dhcp_configuration").arg(name));
        return false;
    }

    const QString hostXml = QStringLiteral("<host mac='%1'/>").arg(mac.trimmed());
    unsigned int flags = VIR_NETWORK_UPDATE_AFFECT_CONFIG;
    if (virNetworkIsActive(network) == 1) {
        flags |= VIR_NETWORK_UPDATE_AFFECT_LIVE;
    }
    const int result =
        virNetworkUpdate(network, VIR_NETWORK_UPDATE_COMMAND_DELETE, VIR_NETWORK_SECTION_IP_DHCP_HOST, parentIndex, hostXml.toUtf8().constData(), flags);
    if (result < 0) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_remove_the_dhcp_reservation_1").arg(QString::fromUtf8(virGetLastErrorMessage())));
    }
    virNetworkFree(network);
    if (result == 0) {
        QSettings settings(kartonConfigPath(), QSettings::IniFormat);
        settings.remove(dhcpReservationKey(scopeUri, name, mac));
        settings.sync();
        m_networksTimes[connectionUri] = 0;
        Q_EMIT networksChanged();
    }
    return result == 0;
}

QVariantList Karton::domainInterfaces(const Domain *domain)
{
    ScopedCallTimer timer("Karton::domainInterfaces");
    if (!domain || !domain->domainPtr()) {
        return {};
    }
    const QString cacheKey = Domain::uuidString(domain->domainPtr());
    if (cacheKey.isEmpty()) {
        return {};
    }
    if (!readerConnectionForUuid(cacheKey)) {
        return collectInterfaces(domain->domainPtr());
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto it = m_interfaceCache.constFind(cacheKey);
    if (it == m_interfaceCache.cend() || now - m_interfaceTimes.value(cacheKey, 0) > 2000) {
        requestDomainInterfaces(cacheKey);
    }
    return it == m_interfaceCache.cend() ? QVariantList{} : *it;
}

void Karton::requestDomainInterfaces(const QString &uuid)
{
    virConnectPtr readerConn = readerConnectionForUuid(uuid);
    if (!m_reader || !readerConn || m_interfacePending.contains(uuid)) {
        return;
    }
    m_interfacePending.insert(uuid);
    postReaderJob([this, uuid, readerConn]() {
        QVariantList result;
        if (virConnectPtr conn = readerConn) {
            virDomainPtr dom = virDomainLookupByUUIDString(conn, uuid.toUtf8().constData());
            if (dom) {
                result = collectInterfaces(dom);
                virDomainFree(dom);
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, uuid, result]() {
                m_interfacePending.remove(uuid);
                if (domainByUuid(uuid)) {
                    m_interfaceCache.insert(uuid, result);
                    m_interfaceTimes.insert(uuid, QDateTime::currentMSecsSinceEpoch());
                    Q_EMIT domainInterfacesUpdated(uuid);
                }
            },
            Qt::QueuedConnection);
    });
}
