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
        *error = i18n("The network name cannot be empty.");
        return false;
    }
    if (!isValidDomainName(data->name)) {
        *error = i18n("A network name may only contain letters, digits, dots, underscores and dashes.");
        return false;
    }

    data->mode = config.value(QStringLiteral("mode"), QStringLiteral("nat")).toString();
    static const QStringList modes = {QStringLiteral("nat"),
                                      QStringLiteral("route"),
                                      QStringLiteral("isolated"),
                                      QStringLiteral("open"),
                                      QStringLiteral("bridge")};
    if (!modes.contains(data->mode)) {
        *error = i18n("Unsupported network mode.");
        return false;
    }

    data->forwardInterface = config.value(QStringLiteral("forwardInterface")).toString().trimmed();
    data->bridgeName = config.value(QStringLiteral("bridgeName")).toString().trimmed();
    data->domainName = config.value(QStringLiteral("domainName")).toString().trimmed();
    if (!data->domainName.isEmpty() && (data->domainName.size() > 253 || data->domainName.contains(QRegularExpression(QStringLiteral("\\s"))))) {
        *error = i18n("The DNS domain name is invalid.");
        return false;
    }

    if (data->mode == QStringLiteral("bridge")) {
        if (data->bridgeName.isEmpty()) {
            *error = i18n("A bridge name is required for bridge networks.");
            return false;
        }
        if (!isValidDomainName(data->bridgeName)) {
            *error = i18n("The bridge name is invalid.");
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
            *error = i18n("The IPv4 network must be a valid CIDR such as 192.168.100.0/24.");
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
                *error = i18n("The DHCPv4 range is invalid for this network.");
                return false;
            }
        }
    }

    data->ipv6Enabled = config.value(QStringLiteral("ipv6Enabled"), false).toBool();
    if (data->ipv6Enabled) {
        if (!parseIpv6Cidr(config.value(QStringLiteral("ipv6")).toString(), &data->ipv6Network, &data->ipv6Prefix)) {
            *error = i18n("The IPv6 network must be a valid CIDR such as fd00::/64.");
            return false;
        }
        const QString gateway = ipv6DefaultGateway(data->ipv6Network, data->ipv6Prefix, config.value(QStringLiteral("ipv6Gateway")).toString());
        if (!parseIpv6Address(gateway, &data->ipv6Gateway)) {
            *error = i18n("The IPv6 gateway is invalid.");
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
                *error = i18n("The DHCPv6 range is invalid for this network.");
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

QVariantList Karton::networks() const
{
    QVariantList result;
    if (!m_conn) {
        return result;
    }

    virNetworkPtr *networkPtrs = nullptr;
    const int count = virConnectListAllNetworks(m_conn, &networkPtrs, VIR_CONNECT_LIST_NETWORKS_ACTIVE | VIR_CONNECT_LIST_NETWORKS_INACTIVE);
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

QVariantMap Karton::defaultNetworkConfig() const
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

bool Karton::createNetwork(const QVariantMap &config)
{
    if (!m_conn) {
        return false;
    }

    NetworkConfigData data;
    QString error;
    if (!parseNetworkConfig(config, &data, &error)) {
        Q_EMIT errorOccurred(error);
        return false;
    }

    virNetworkPtr existing = virNetworkLookupByName(m_conn, data.name.toUtf8().constData());
    if (existing) {
        virNetworkFree(existing);
        Q_EMIT errorOccurred(i18nc("%1 is a network name", "Network %1 already exists.", data.name));
        return false;
    }

    QDomDocument document;
    if (!applyNetworkConfig(document, data, true, &error)) {
        Q_EMIT errorOccurred(error);
        return false;
    }

    virNetworkPtr network = virNetworkDefineXML(m_conn, document.toString().toUtf8().constData());
    if (!network) {
        Q_EMIT errorOccurred(
            i18nc("%1 is a network name, %2 is an error", "Failed to define network %1: %2", data.name, QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }

    if (virNetworkCreate(network) != 0) {
        Q_EMIT errorOccurred(
            i18nc("%1 is a network name, %2 is an error", "Failed to start network %1: %2", data.name, QString::fromUtf8(virGetLastErrorMessage())));
        virNetworkUndefine(network);
        virNetworkFree(network);
        return false;
    }

    const bool autostart = config.value(QStringLiteral("autostart"), false).toBool();
    if (virNetworkSetAutostart(network, autostart ? 1 : 0) != 0) {
        Q_EMIT errorOccurred(i18nc("%1 is a network name, %2 is an error",
                                   "Failed to change autostart for network %1: %2",
                                   data.name,
                                   QString::fromUtf8(virGetLastErrorMessage())));
    }

    virNetworkFree(network);
    Q_EMIT networksChanged();
    return true;
}

bool Karton::updateNetwork(const QVariantMap &config)
{
    if (!m_conn) {
        return false;
    }

    NetworkConfigData data;
    QString error;
    if (!parseNetworkConfig(config, &data, &error)) {
        Q_EMIT errorOccurred(error);
        return false;
    }

    virNetworkPtr oldNetwork = virNetworkLookupByName(m_conn, data.name.toUtf8().constData());
    if (!oldNetwork) {
        Q_EMIT errorOccurred(i18nc("%1 is a network name", "Network %1 was not found.", data.name));
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
        Q_EMIT errorOccurred(i18nc("%1 is a network name", "Failed to read the configuration of network %1.", data.name));
        return false;
    }
    free(xml);

    if (!applyNetworkConfig(document, data, false, &error)) {
        virNetworkFree(oldNetwork);
        Q_EMIT errorOccurred(error);
        return false;
    }

    virNetworkPtr newNetwork = virNetworkDefineXML(m_conn, document.toString().toUtf8().constData());
    virNetworkFree(oldNetwork);
    if (!newNetwork) {
        Q_EMIT errorOccurred(
            i18nc("%1 is a network name, %2 is an error", "Failed to update network %1: %2", data.name, QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }

    if (wasActive) {
        // Apply the new definition to the running network as well. A brief
        // restart is required because libvirt cannot change a live network's
        // addressing in place.
        if (virNetworkDestroy(newNetwork) != 0 || virNetworkCreate(newNetwork) != 0) {
            Q_EMIT errorOccurred(i18nc("%1 is a network name, %2 is an error",
                                       "Network %1 was updated but could not be restarted: %2",
                                       data.name,
                                       QString::fromUtf8(virGetLastErrorMessage())));
            virNetworkFree(newNetwork);
            return false;
        }
    }

    const bool autostart = config.value(QStringLiteral("autostart"), false).toBool();
    if (virNetworkSetAutostart(newNetwork, autostart ? 1 : 0) != 0) {
        Q_EMIT errorOccurred(i18nc("%1 is a network name, %2 is an error",
                                   "Failed to change autostart for network %1: %2",
                                   data.name,
                                   QString::fromUtf8(virGetLastErrorMessage())));
    }

    virNetworkFree(newNetwork);
    Q_EMIT networksChanged();
    return true;
}

bool Karton::startNetwork(const QString &name)
{
    if (!m_conn) {
        return false;
    }

    virNetworkPtr network = virNetworkLookupByName(m_conn, name.toUtf8().constData());
    if (!network) {
        const QString errorMsg =
            i18nc("%1 is a network name, %2 is an error", "Failed to find network %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const bool success = virNetworkCreate(network) == 0;
    if (!success) {
        const QString errorMsg =
            i18nc("%1 is a network name, %2 is an error", "Failed to start network %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }
    virNetworkFree(network);
    if (success) {
        Q_EMIT networksChanged();
    }
    return success;
}

bool Karton::stopNetwork(const QString &name)
{
    if (!m_conn) {
        return false;
    }

    virNetworkPtr network = virNetworkLookupByName(m_conn, name.toUtf8().constData());
    if (!network) {
        const QString errorMsg =
            i18nc("%1 is a network name, %2 is an error", "Failed to find network %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const bool success = virNetworkDestroy(network) == 0;
    if (!success) {
        const QString errorMsg =
            i18nc("%1 is a network name, %2 is an error", "Failed to stop network %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }
    virNetworkFree(network);
    if (success) {
        Q_EMIT networksChanged();
    }
    return success;
}

bool Karton::setNetworkAutostart(const QString &name, bool autostart)
{
    virNetworkPtr network = virNetworkLookupByName(m_conn, name.toUtf8().constData());
    if (!network) {
        const QString errorMsg =
            i18nc("%1 is a network name, %2 is an error", "Failed to find network %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const bool success = virNetworkSetAutostart(network, autostart ? 1 : 0) == 0;
    if (!success) {
        const QString errorMsg =
            i18nc("%1 is a network name, %2 is an error", "Failed to change autostart for network %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }
    virNetworkFree(network);
    if (success) {
        Q_EMIT networksChanged();
    }
    return success;
}

bool Karton::createNatNetwork(const QString &requestedName)
{
    if (!m_conn) {
        return false;
    }

    QString name = requestedName.trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("karton-nat");
    }
    if (!isValidDomainName(name)) {
        Q_EMIT errorOccurred(i18n("A network name may only contain letters, digits, dots, underscores and dashes."));
        return false;
    }

    virNetworkPtr existing = virNetworkLookupByName(m_conn, name.toUtf8().constData());
    if (existing) {
        virNetworkFree(existing);
        Q_EMIT errorOccurred(i18nc("%1 is a network name", "Network %1 already exists.", name));
        return false;
    }

    // Find a 192.168.X.0/24 subnet that is not used by another libvirt
    // network or by a host interface.
    QSet<int> usedSubnets;
    virNetworkPtr *networkPtrs = nullptr;
    const int count = virConnectListAllNetworks(m_conn, &networkPtrs, VIR_CONNECT_LIST_NETWORKS_ACTIVE | VIR_CONNECT_LIST_NETWORKS_INACTIVE);
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
        Q_EMIT errorOccurred(i18n("Could not find a free 192.168.x.0/24 subnet for the new network."));
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

    virNetworkPtr network = virNetworkDefineXML(m_conn, xml.toUtf8().constData());
    if (!network) {
        const QString errorMsg =
            i18nc("%1 is a network name, %2 is an error", "Failed to define network %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    if (virNetworkCreate(network) != 0) {
        const QString errorMsg =
            i18nc("%1 is a network name, %2 is an error", "Failed to start network %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        virNetworkUndefine(network);
        virNetworkFree(network);
        return false;
    }

    virNetworkFree(network);
    Q_EMIT networksChanged();
    return true;
}

QVariantList Karton::domainInterfaces(const Domain *domain) const
{
    QVariantList result;
    if (!domain || !domain->domainPtr()) {
        return result;
    }

    QHash<QString, QStringList> addresses;
    virDomainInterfacePtr *interfaceAddresses = nullptr;
    int addressCount = 0;
    if (domain->isActive()) {
        addressCount = virDomainInterfaceAddresses(domain->domainPtr(), &interfaceAddresses, VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT, 0);
        if (addressCount <= 0) {
            addressCount = virDomainInterfaceAddresses(domain->domainPtr(), &interfaceAddresses, VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_LEASE, 0);
        }
        if (addressCount <= 0) {
            addressCount = virDomainInterfaceAddresses(domain->domainPtr(), &interfaceAddresses, VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_ARP, 0);
        }
    }
    for (int i = 0; i < addressCount; ++i) {
        const virDomainInterface *interface = interfaceAddresses[i];
        QStringList ips;
        for (unsigned int j = 0; j < interface->naddrs; ++j) {
            const QString address = QString::fromUtf8(interface->addrs[j].addr);
            if (!address.isEmpty()) {
                ips.append(address);
            }
        }
        if (!interface->hwaddr || ips.isEmpty()) {
            continue;
        }
        addresses.insert(QString::fromUtf8(interface->hwaddr).toLower(), ips);
    }
    for (int i = 0; i < addressCount; ++i) {
        virDomainInterfaceFree(interfaceAddresses[i]);
    }
    free(interfaceAddresses);

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xml) {
        return result;
    }

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
            interfaceInfo.insert(QStringLiteral("ip"), addresses.value(macElement.attribute(QStringLiteral("address")).toLower()).join(QStringLiteral(", ")));
            result.append(interfaceInfo);
        }
    }

    free(xml);
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

bool Karton::deleteNetwork(const QString &name)
{
    if (!m_conn || name.isEmpty()) {
        return false;
    }
    virNetworkPtr network = virNetworkLookupByName(m_conn, name.toUtf8().constData());
    if (!network) {
        Q_EMIT errorOccurred(i18nc("%1 is a network name", "Network %1 was not found.", name));
        return false;
    }
    const int active = virNetworkIsActive(network);
    if (active == 1) {
        virNetworkDestroy(network);
    }
    const bool ok = virNetworkUndefine(network) == 0;
    if (!ok) {
        Q_EMIT errorOccurred(
            i18nc("%1 is a network name, %2 is the error message", "Failed to delete network %1: %2", name, QString::fromUtf8(virGetLastErrorMessage())));
    }
    virNetworkFree(network);
    if (ok) {
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
        Q_EMIT errorOccurred(i18n("Invalid network name."));
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
                i18nc("%1 is a network name, %2 is an error", "Failed to attach network %1: %2", network, QString::fromUtf8(virGetLastErrorMessage()));
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
            i18nc("%1 is a network name, %2 is an error", "Failed to attach network %1: %2", network, QString::fromUtf8(virGetLastErrorMessage()));
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
        Q_EMIT errorOccurred(i18nc("%1 is a MAC address", "No interface with MAC address %1 was found.", mac));
        return false;
    }

    bool success = true;
    if (domain->isActive() && !liveXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), liveXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg =
                i18nc("%1 is a MAC address, %2 is an error", "Failed to detach interface %1: %2", mac, QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (!configXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), configXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            const QString errorMsg =
                i18nc("%1 is a MAC address, %2 is an error", "Failed to detach persistent interface %1: %2", mac, QString::fromUtf8(virGetLastErrorMessage()));
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
    if (!domain || !domain->domainPtr() || mac.isEmpty() || newMac.isEmpty() || !m_conn) {
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

        virDomainPtr redefined = virDomainDefineXML(m_conn, document.toString().toUtf8().constData());
        if (!redefined) {
            Q_EMIT errorOccurred(i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to update interface for domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage())));
            return false;
        }
        virDomainFree(redefined);
        refreshDomainList();
        return true;
    }

    Q_EMIT errorOccurred(i18nc("%1 is a MAC address", "No interface with MAC address %1 was found.", mac));
    return false;
}
