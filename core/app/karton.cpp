// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "app/karton.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <KLocalizedString>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSGRendererInterface>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <utility>

#include "domain/domain.h"
#include "domain/domainconfig.h"
#include "domain/domainxmlbuilder.h"
#include "domain/domainxmlreader.h"
#include "karton_debug.h"
#include "libvirt/libvirteventloop.h"
#include "libvirt/libvirtmonitor.h"

Karton::Karton(QObject *parent)
    : QObject(parent)
    , m_conn(nullptr)
    , m_domains()
    , m_monitor(nullptr)
    , m_commandRunner(new CommandRunner(this))
{
    init();
}

Karton::~Karton()
{
    if (m_conn) {
        virConnectClose(m_conn);
        m_conn = nullptr;
    }
}

QString Karton::connectionUri() const
{
    if (!m_activeUri.isEmpty()) {
        return m_activeUri;
    }

    const QByteArray environmentUri = qgetenv("KARTON_LIBVIRT_URI");
    if (!environmentUri.isEmpty()) {
        return QString::fromUtf8(environmentUri);
    }

    QSettings settings;
    const QString storedUri = settings.value(QStringLiteral("connection/uri")).toString();
    return storedUri.isEmpty() ? QStringLiteral("qemu:///system") : storedUri;
}

void Karton::saveConnectionUri(const QString &uri)
{
    QSettings settings;
    settings.setValue(QStringLiteral("connection/uri"), uri.trimmed());
}

Karton *Karton::self()
{
    static Karton *k = new Karton(nullptr);
    return k;
}

Karton *Karton::create(QQmlEngine *qmlEngine, QJSEngine *)
{
    Q_UNUSED(qmlEngine);
    return Karton::self();
}

void Karton::onDomainStateChanged(virDomainPtr domainPtr, int event, int detail)
{
    const char *domainName = virDomainGetName(domainPtr);
    qCInfo(KARTON_DEBUG) << "Domain state changed:" << domainName << "Event:" << event << "Detail:" << detail;
    // Reference for event/details: https://libvirt.org/html/libvirt-libvirt-domain.html#virDomainEventType

    Q_EMIT domainsChanged(domainPtr, event, detail);
}

bool Karton::init()
{
    // Register the Qt-based event loop. Note: must be done before connecting
    // to the hypervisor.
    LibvirtEventLoop::registerImpl();

    // Karton may be started by a user who only has domains on the system
    // connection, or by one who only uses the per-user session connection.
    // Prefer the explicitly configured URI, but otherwise pick the connection
    // that actually has domains.
    QStringList candidates;
    const QByteArray environmentUri = qgetenv("KARTON_LIBVIRT_URI");
    if (!environmentUri.isEmpty()) {
        candidates.append(QString::fromUtf8(environmentUri));
    } else {
        QSettings settings;
        const QString storedUri = settings.value(QStringLiteral("connection/uri")).toString();
        if (!storedUri.isEmpty()) {
            candidates.append(storedUri);
            if (storedUri == QStringLiteral("qemu:///system")) {
                candidates.append(QStringLiteral("qemu:///session"));
            } else if (storedUri == QStringLiteral("qemu:///session")) {
                candidates.append(QStringLiteral("qemu:///system"));
            }
        } else {
            candidates.append(QStringLiteral("qemu:///system"));
            candidates.append(QStringLiteral("qemu:///session"));
        }
    }

    struct Attempt {
        virConnectPtr conn = nullptr;
        QString uri;
        int domainCount = 0;
    };

    auto tryUri = [](const QString &uri, Attempt &attempt) {
        attempt.uri = uri;
        attempt.conn = virConnectOpen(uri.toUtf8().constData());
        if (!attempt.conn) {
            qCWarning(KARTON_DEBUG) << "Could not connect to" << uri << virGetLastErrorMessage();
            return false;
        }

        virDomainPtr *domains = nullptr;
        const int count = virConnectListAllDomains(attempt.conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE | VIR_CONNECT_LIST_DOMAINS_INACTIVE);
        if (count > 0 && domains) {
            for (int i = 0; i < count; ++i) {
                virDomainFree(domains[i]);
            }
            free(domains);
        }
        attempt.domainCount = count > 0 ? count : 0;
        return true;
    };

    Attempt chosen;
    Attempt fallback;
    for (const QString &uri : std::as_const(candidates)) {
        Attempt attempt;
        if (!tryUri(uri, attempt)) {
            continue;
        }

        qCInfo(KARTON_DEBUG) << "Connected to hypervisor:" << uri << "with" << attempt.domainCount << "domains";
        if (attempt.domainCount > 0) {
            chosen = attempt;
            break;
        }
        if (!fallback.conn) {
            fallback = attempt;
        } else {
            virConnectClose(attempt.conn);
        }
    }

    if (!chosen.conn) {
        chosen = fallback;
    } else if (fallback.conn) {
        virConnectClose(fallback.conn);
    }

    if (!chosen.conn) {
        qCCritical(KARTON_DEBUG) << "Failed to connect to hypervisor:" << virGetLastErrorMessage();
        return false;
    }

    m_conn = chosen.conn;
    m_activeUri = chosen.uri;

    m_monitor = new LibvirtMonitor(this, m_conn);
    connect(m_monitor, &LibvirtMonitor::domainStateChanged, this, &Karton::onDomainStateChanged);

    refreshDomainList();

    return true;
}

// searchDomain(domain) returns index position of the domain in m_domains
int Karton::searchDomain(const virDomainPtr domainPtr)
{
    const QString searchUuid = Domain::uuidString(domainPtr);

    for (int i = 0; i < m_domains.size(); i++) {
        if (searchUuid == m_domains[i]->config()->uuid()) {
            return i;
        }
    }
    return -1;
}

static bool isValidDomainName(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9._-]+$"));
    return expression.match(name).hasMatch();
}

// get string from state
static QString domainStateString(unsigned int state)
{
    switch (state) {
    case VIR_DOMAIN_NOSTATE:
        return i18n("no state");
    case VIR_DOMAIN_RUNNING:
        return i18n("running");
    case VIR_DOMAIN_BLOCKED:
        return i18n("blocked");
    case VIR_DOMAIN_PAUSED:
        return i18n("paused");
    case VIR_DOMAIN_SHUTDOWN:
        return i18n("shutting down");
    case VIR_DOMAIN_SHUTOFF:
        return i18n("shutoff");
    case VIR_DOMAIN_CRASHED:
        return i18n("crashed");
    case VIR_DOMAIN_PMSUSPENDED:
        return i18n("suspended");
    default:
        return i18n("unknown");
    }
}

// refresh a singular domain, used to update list
void Karton::refreshDomain(const virDomainPtr domainPtr)
{
    if (!domainPtr) {
        return;
    }

    const int index = searchDomain(domainPtr);
    if (index == -1) {
        qCWarning(KARTON_DEBUG) << "Domain not found in list.";
        return;
    }

    Domain *domain = m_domains[index];

    virDomainInfo domInfo = {};
    if (virDomainGetInfo(domainPtr, &domInfo) < 0) {
        qCWarning(KARTON_DEBUG) << "Failed to get domain info:" << virGetLastErrorMessage();
        return;
    }

    int autoFlag = 0;
    if (virDomainGetAutostart(domainPtr, &autoFlag) < 0) {
        autoFlag = domain->autostart() ? 1 : 0;
    }

    // updates only mutable fields
    domain->setActive(virDomainIsActive(domainPtr) == 1);
    domain->setState(domainStateString(domInfo.state));
    domain->setRamUsage(domInfo.memory / (1024 * 1024));
    domain->setAutostart(autoFlag != 0);
}

// Refresh the complete list of domains. Existing Domain objects are updated in
// place so that QML pages holding them keep working.
void Karton::refreshDomainList()
{
    if (!m_conn) {
        return;
    }

    virDomainPtr *domains = nullptr;
    const int numDomains = virConnectListAllDomains(m_conn, &domains, 0);
    if (numDomains < 0) {
        qCWarning(KARTON_DEBUG) << "Failed to list domains:" << virGetLastErrorMessage();
        free(domains);
        return;
    }

    QVector<Domain *> updated;
    updated.reserve(numDomains);
    QVector<Domain *> added;
    for (int i = 0; i < numDomains; ++i) {
        virDomainPtr domainPtr = domains[i];
        const QString uuid = Domain::uuidString(domainPtr);
        if (uuid.isEmpty()) {
            qCWarning(KARTON_DEBUG) << "Skipping domain without a UUID";
            virDomainFree(domainPtr);
            continue;
        }
        virDomainInfo domInfo = {};
        if (virDomainGetInfo(domainPtr, &domInfo) < 0) {
            qCWarning(KARTON_DEBUG) << "Failed to get domain info:" << virGetLastErrorMessage();
            virDomainFree(domainPtr);
            continue;
        }

        const QString name = QString::fromUtf8(virDomainGetName(domainPtr));
        const QString xmlConfigPath = getXmlConfigPath(name);

        DomainXmlReader reader(domainPtr);

        QString screenshotPreviewPath = reader.m_xmlInfo.screenshotPreviewPath;
        if (screenshotPreviewPath.isEmpty()) { // set default path if not found
            screenshotPreviewPath = QStringLiteral("%1/previews/%2.png").arg(QStandardPaths::writableLocation(QStandardPaths::StateLocation), uuid);
        }

        int autoFlag = 0;
        virDomainGetAutostart(domainPtr, &autoFlag);

        const DomainConfig::DomainConfigData configData = {
            .hypervisorType = reader.m_xmlInfo.hypervisorType,
            .indexId = reader.m_xmlInfo.indexId,
            .name = name,
            .uuid = uuid,
            .shortOsId = reader.m_xmlInfo.shortOsId,
            .osId = reader.m_xmlInfo.osId,
            .isActive = virDomainIsActive(domainPtr) == 1,
            .state = domainStateString(domInfo.state),
            .maxRam = static_cast<int>(domInfo.maxMem / (1024 * 1024)), // KiB -> GiB
            .ramUsage = static_cast<int>(domInfo.memory / (1024 * 1024)), // KiB -> GiB
            .cpus = static_cast<int>(domInfo.nrVirtCpu),
            .maxDiskStorage = reader.m_xmlInfo.maxDiskStorage > 0 ? reader.m_xmlInfo.maxDiskStorage / 1024 // MiB -> GiB
                                                                  : diskSizeGB(reader.m_xmlInfo.virtualDiskPath),
            .xmlConfigPath = xmlConfigPath,
            .isoDiskPath = reader.m_xmlInfo.isoDiskPath,
            .virtualDiskPath = reader.m_xmlInfo.virtualDiskPath,
            .virtualDiskPaths = reader.m_xmlInfo.virtualDiskPaths,
            .diskFormat = reader.m_xmlInfo.diskFormat,
            .firmware = reader.m_xmlInfo.firmware,
            .networkModel = reader.m_xmlInfo.networkModel,
            .secureBoot = reader.m_xmlInfo.secureBoot,
            .screenshotPreviewPath = screenshotPreviewPath,
            .autostart = autoFlag != 0,
        };

        Domain *domain = nullptr;
        for (Domain *candidate : std::as_const(m_domains)) {
            if (candidate->config()->uuid() == uuid) {
                domain = candidate;
                break;
            }
        }

        if (domain) {
            domain->setDomainPtr(domainPtr);
            domain->apply(configData);
        } else {
            auto *config = new DomainConfig(configData);
            domain = new Domain(domainPtr, config, this);
            added.append(domain);
        }

        updated.append(domain);
    }

    free(domains);

    // Drop domains that disappeared from libvirt. deleteLater() keeps QML pages
    // that are still being torn down from touching a destroyed object.
    const QVector<Domain *> previousDomains = m_domains;
    m_domains = updated;

    for (Domain *domain : std::as_const(added)) {
        Q_EMIT domainAdded(domain);
    }
    for (Domain *domain : previousDomains) {
        if (!m_domains.contains(domain)) {
            Q_EMIT domainRemoved(domain);
            domain->deleteLater();
        }
    }
}

QVector<Domain *> Karton::domains()
{
    return m_domains;
}

bool Karton::startDomain(const Domain *domain)
{
    if (!domain) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainCreate(domainPtr) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to start domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }
    qCInfo(KARTON_DEBUG) << "Successfully started domain:" << domain->config()->name();
    return true;
}

bool Karton::stopDomain(const Domain *domain)
{
    if (!domain) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    virDomainInfo info = {};
    if (virDomainGetInfo(domainPtr, &info) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to get state of domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    if (info.state == VIR_DOMAIN_RUNNING || info.state == VIR_DOMAIN_PAUSED) {
        if (virDomainShutdown(domainPtr) < 0) {
            const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                           "Failed to stop domain %1: %2",
                                           domain->config()->name(),
                                           QString::fromUtf8(virGetLastErrorMessage()));
            qCWarning(KARTON_DEBUG) << errorMsg;
            Q_EMIT errorOccurred(errorMsg);
            return false;
        }
    }

    qCInfo(KARTON_DEBUG) << "Successfully stopped domain:" << domain->config()->name();
    return true;
}

bool Karton::forceStopDomain(const Domain *domain)
{
    if (!domain) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainDestroy(domainPtr) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to force-stop domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }
    qCInfo(KARTON_DEBUG) << "Successfully force-stopped domain:" << domain->config()->name();
    return true;
}

bool Karton::setAutostart(const Domain *domain, bool autostart)
{
    if (!domain) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainSetAutostart(domainPtr, autostart ? 1 : 0) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to change autostart for domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const_cast<Domain *>(domain)->setAutostart(autostart);
    return true;
}

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

        QString forwardMode = QStringLiteral("none");
        char *xml = virNetworkGetXMLDesc(network, 0);
        if (xml) {
            QDomDocument document;
            if (document.setContent(QString::fromUtf8(xml))) {
                const QDomElement root = document.documentElement();
                const QDomElement forward = root.firstChildElement(QStringLiteral("forward"));
                if (!forward.isNull()) {
                    forwardMode = forward.attribute(QStringLiteral("mode"), QStringLiteral("nat"));
                }
            }
            free(xml);
        }

        QString bridge;
        char *bridgeName = virNetworkGetBridgeName(network);
        if (bridgeName) {
            bridge = QString::fromUtf8(bridgeName);
            free(bridgeName);
        }

        QVariantMap networkInfo;
        networkInfo.insert(QStringLiteral("name"), name);
        networkInfo.insert(QStringLiteral("active"), virNetworkIsActive(network) == 1);
        networkInfo.insert(QStringLiteral("autostart"), autostart == 1);
        networkInfo.insert(QStringLiteral("forward"), forwardMode);
        networkInfo.insert(QStringLiteral("bridge"), bridge);
        result.append(networkInfo);

        virNetworkFree(network);
    }
    free(networkPtrs);
    return result;
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

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
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

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
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
        Q_EMIT errorOccurred(i18n("USB vendor, product, bus and device are required."));
        return false;
    }

    const QString xml = usbHostdevXml(vendor, product, bus, device);
    bool success = false;
    if (domain->isActive()) {
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg = i18nc("%1 is a USB device, %2 is an error",
                                           "Failed to attach USB device %1: %2",
                                           vendor + QLatin1Char(':') + product,
                                           QString::fromUtf8(virGetLastErrorMessage()));
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
        const QString errorMsg = i18nc("%1 is a USB device, %2 is an error",
                                       "Failed to attach USB device %1: %2",
                                       vendor + QLatin1Char(':') + product,
                                       QString::fromUtf8(virGetLastErrorMessage()));
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
        Q_EMIT errorOccurred(i18nc("%1 is a USB device", "No matching USB device %1 was found.", vendor + QLatin1Char(':') + product));
        return false;
    }

    bool success = true;
    if (domain->isActive() && !liveXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), liveXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg = i18nc("%1 is a USB device, %2 is an error",
                                           "Failed to detach USB device %1: %2",
                                           vendor + QLatin1Char(':') + product,
                                           QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (!configXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), configXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            const QString errorMsg = i18nc("%1 is a USB device, %2 is an error",
                                           "Failed to detach persistent USB device %1: %2",
                                           vendor + QLatin1Char(':') + product,
                                           QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

QVariantList Karton::domainSnapshots(const Domain *domain) const
{
    QVariantList result;
    if (!domain || !domain->domainPtr()) {
        return result;
    }

    virDomainSnapshotPtr *snapshots = nullptr;
    const int count = virDomainListAllSnapshots(domain->domainPtr(), &snapshots, 0);
    if (count < 0) {
        return result;
    }

    for (int i = 0; i < count; ++i) {
        char *xml = virDomainSnapshotGetXMLDesc(snapshots[i], 0);
        if (!xml) {
            virDomainSnapshotFree(snapshots[i]);
            continue;
        }

        QDomDocument document;
        if (document.setContent(QString::fromUtf8(xml))) {
            const QDomElement rootElement = document.documentElement();
            const QDomElement nameElement = rootElement.firstChildElement(QStringLiteral("name"));
            const QDomElement descriptionElement = rootElement.firstChildElement(QStringLiteral("description"));
            const QDomElement creationElement = rootElement.firstChildElement(QStringLiteral("creationTime"));
            const QDomElement stateElement = rootElement.firstChildElement(QStringLiteral("state"));
            const QDomElement parentElement = rootElement.firstChildElement(QStringLiteral("parent"));
            const QDomElement parentNameElement = parentElement.firstChildElement(QStringLiteral("name"));

            QString creationTime;
            if (!creationElement.text().isEmpty()) {
                creationTime = QDateTime::fromSecsSinceEpoch(creationElement.text().toLongLong()).toString(Qt::ISODate);
            }

            QVariantMap info;
            info.insert(QStringLiteral("name"), nameElement.text());
            info.insert(QStringLiteral("description"), descriptionElement.text());
            info.insert(QStringLiteral("creationTime"), creationTime);
            info.insert(QStringLiteral("state"), stateElement.text());
            info.insert(QStringLiteral("parent"), parentNameElement.text());
            info.insert(QStringLiteral("current"), virDomainSnapshotIsCurrent(snapshots[i], 0) == 1);
            result.append(info);
        }

        free(xml);
        virDomainSnapshotFree(snapshots[i]);
    }
    free(snapshots);
    return result;
}

bool Karton::createSnapshot(const Domain *domain, const QString &requestedName, const QString &description, bool includeMemory)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    const QString name = requestedName.trimmed();
    if (!isValidDomainName(name)) {
        Q_EMIT errorOccurred(i18n("A snapshot name may only contain letters, digits, dots, underscores and dashes."));
        return false;
    }
    if (domain->isActive() && !includeMemory) {
        Q_EMIT errorOccurred(i18n("A running VM needs a memory-inclusive internal snapshot. Enable Include memory."));
        return false;
    }

    QDomDocument document;
    QDomElement root = document.createElement(QStringLiteral("domainsnapshot"));
    document.appendChild(root);

    QDomElement nameElement = document.createElement(QStringLiteral("name"));
    nameElement.appendChild(document.createTextNode(name));
    root.appendChild(nameElement);

    if (!description.trimmed().isEmpty()) {
        QDomElement descriptionElement = document.createElement(QStringLiteral("description"));
        descriptionElement.appendChild(document.createTextNode(description));
        root.appendChild(descriptionElement);
    }

    QDomElement memoryElement = document.createElement(QStringLiteral("memory"));
    memoryElement.setAttribute(QStringLiteral("snapshot"), (includeMemory && domain->isActive()) ? QStringLiteral("internal") : QStringLiteral("no"));
    root.appendChild(memoryElement);

    virDomainSnapshotPtr snapshot = virDomainSnapshotCreateXML(domain->domainPtr(), document.toString().toUtf8().constData(), 0);
    if (!snapshot) {
        const QString errorMsg =
            i18nc("%1 is a snapshot name, %2 is an error", "Failed to create snapshot %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    virDomainSnapshotFree(snapshot);
    return true;
}

bool Karton::deleteSnapshot(const Domain *domain, const QString &name)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virDomainSnapshotPtr snapshot = virDomainSnapshotLookupByName(domain->domainPtr(), name.toUtf8().constData(), 0);
    if (!snapshot) {
        const QString errorMsg =
            i18nc("%1 is a snapshot name, %2 is an error", "Failed to find snapshot %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const bool success = virDomainSnapshotDelete(snapshot, VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN) == 0;
    if (!success) {
        const QString errorMsg =
            i18nc("%1 is a snapshot name, %2 is an error", "Failed to delete snapshot %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }
    virDomainSnapshotFree(snapshot);
    return success;
}

bool Karton::revertSnapshot(const Domain *domain, const QString &name)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virDomainSnapshotPtr snapshot = virDomainSnapshotLookupByName(domain->domainPtr(), name.toUtf8().constData(), 0);
    if (!snapshot) {
        const QString errorMsg =
            i18nc("%1 is a snapshot name, %2 is an error", "Failed to find snapshot %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const bool success = virDomainRevertToSnapshot(snapshot, 0) == 0;
    if (!success) {
        const QString errorMsg =
            i18nc("%1 is a snapshot name, %2 is an error", "Failed to revert to snapshot %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }
    virDomainSnapshotFree(snapshot);
    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

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

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
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
        Q_EMIT errorOccurred(i18n("Invalid PCI address."));
        return false;
    }

    const QString xml = pciHostdevXml(normalizedDomain, normalizedBus, normalizedSlot, normalizedFunction);
    const QString label = pciAddressString(normalizedDomain, normalizedBus, normalizedSlot, normalizedFunction);
    bool success = false;
    if (domain->isActive()) {
        if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg =
                i18nc("%1 is a PCI address, %2 is an error", "Failed to attach PCI device %1: %2", label, QString::fromUtf8(virGetLastErrorMessage()));
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
            i18nc("%1 is a PCI address, %2 is an error", "Failed to attach PCI device %1: %2", label, QString::fromUtf8(virGetLastErrorMessage()));
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
        Q_EMIT errorOccurred(i18nc("%1 is a PCI address", "No matching PCI device %1 was found.", label));
        return false;
    }

    bool success = true;
    if (domain->isActive() && !liveXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), liveXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) {
            const QString errorMsg =
                i18nc("%1 is a PCI address, %2 is an error", "Failed to detach PCI device %1: %2", label, QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (!configXml.isEmpty()) {
        if (virDomainDetachDeviceFlags(domain->domainPtr(), configXml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            const QString errorMsg = i18nc("%1 is a PCI address, %2 is an error",
                                           "Failed to detach persistent PCI device %1: %2",
                                           label,
                                           QString::fromUtf8(virGetLastErrorMessage()));
            Q_EMIT errorOccurred(errorMsg);
            success = false;
        }
    }

    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}

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
