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
#include <QFileInfo>
#include <QImage>
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

namespace
{
QString kartonConfigPath()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/karton");
    QDir().mkpath(directory);
    const QString path = directory + QStringLiteral("/karton.conf");
    const QString legacyPath = directory + QStringLiteral("/karton.ini");
    if (!QFile::exists(path) && QFile::exists(legacyPath)) {
        QFile::copy(legacyPath, path);
    }
    return path;
}
} // namespace

Karton::Karton(QObject *parent)
    : QObject(parent)
    , m_conn(nullptr)
    , m_domains()
    , m_monitor(nullptr)
    , m_commandRunner(new CommandRunner(this))
{
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installEventFilter(this);
    }
    init();
}

bool Karton::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::NonClientAreaMouseButtonPress || event->type() == QEvent::ApplicationDeactivate) {
        Q_EMIT closePopupsRequested();
    }
    return QObject::eventFilter(watched, event);
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

    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString storedUri = settings.value(QStringLiteral("connection/uri")).toString();
    return storedUri.isEmpty() ? QStringLiteral("qemu:///system") : storedUri;
}

void Karton::saveConnectionUri(const QString &uri)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
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
        QSettings settings(kartonConfigPath(), QSettings::IniFormat);
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
    domain->setPaused(domInfo.state == VIR_DOMAIN_PAUSED);
    domain->setHasSavedState(virDomainHasManagedSaveImage(domainPtr, 0) == 1);
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
            .isPaused = domInfo.state == VIR_DOMAIN_PAUSED,
            .hasSavedState = virDomainHasManagedSaveImage(domainPtr, 0) == 1,
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

bool Karton::pauseDomain(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainSuspend(domainPtr) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to pause domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    refreshDomain(domainPtr);
    qCInfo(KARTON_DEBUG) << "Successfully paused domain:" << domain->config()->name();
    return true;
}

bool Karton::resumeDomain(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainResume(domainPtr) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to resume domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    refreshDomain(domainPtr);
    qCInfo(KARTON_DEBUG) << "Successfully resumed domain:" << domain->config()->name();
    return true;
}

bool Karton::rebootDomain(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainReboot(domainPtr, VIR_DOMAIN_REBOOT_DEFAULT) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to reboot domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    qCInfo(KARTON_DEBUG) << "Successfully rebooted domain:" << domain->config()->name();
    return true;
}

bool Karton::resetDomain(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainReset(domainPtr, 0) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to reset domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    qCInfo(KARTON_DEBUG) << "Successfully reset domain:" << domain->config()->name();
    return true;
}

bool Karton::saveDomainState(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainManagedSave(domainPtr, 0) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to save the state of domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    refreshDomain(domainPtr);
    qCInfo(KARTON_DEBUG) << "Successfully saved the state of domain:" << domain->config()->name();
    return true;
}

bool Karton::deleteSavedState(const Domain *domain)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainManagedSaveRemove(domainPtr, 0) < 0) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to delete the saved state of domain %1: %2",
                                       domain->config()->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    refreshDomain(domainPtr);
    qCInfo(KARTON_DEBUG) << "Successfully deleted the saved state of domain:" << domain->config()->name();
    return true;
}

void Karton::setGuestAgentWarningDismissed(bool dismissed)
{
    if (m_guestAgentWarningDismissed == dismissed) {
        return;
    }
    m_guestAgentWarningDismissed = dismissed;
    Q_EMIT guestAgentWarningDismissedChanged();
}

void Karton::viewerOpened()
{
    ++m_viewerCount;
    Q_EMIT viewerCountChanged();
}

void Karton::viewerClosed()
{
    if (m_viewerCount > 0) {
        --m_viewerCount;
    }
    Q_EMIT viewerCountChanged();
}

QObject *Karton::viewerForDomain(Domain *domain) const
{
    if (!domain) {
        return nullptr;
    }
    const auto it = m_viewers.constFind(domain->config()->uuid());
    if (it == m_viewers.constEnd() || it.value().isNull()) {
        return nullptr;
    }
    return it.value();
}

void Karton::registerViewer(Domain *domain, QObject *window)
{
    if (!domain || !window) {
        return;
    }
    m_viewers.insert(domain->config()->uuid(), window);
}

void Karton::unregisterViewer(QObject *window)
{
    for (auto it = m_viewers.begin(); it != m_viewers.end();) {
        if (it.value().isNull() || it.value() == window) {
            it = m_viewers.erase(it);
        } else {
            ++it;
        }
    }
}

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
    info.insert(QStringLiteral("videoModel"), videoModel.attribute(QStringLiteral("type"), QStringLiteral("qxl")));
    info.insert(QStringLiteral("videoHeads"), videoModel.attribute(QStringLiteral("heads"), QStringLiteral("1")).toInt());
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
    return info;
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
        model.setAttribute(QStringLiteral("type"), settings.value(QStringLiteral("videoModel")).toString());
        model.setAttribute(QStringLiteral("heads"), QString::number(qMax(1, settings.value(QStringLiteral("videoHeads")).toInt())));
        model.setAttribute(QStringLiteral("primary"), QStringLiteral("yes"));
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

bool Karton::refreshDomainPreview(const Domain *domain)
{
    if (!domain || !domain->domainPtr() || m_previewCapturing || domain->config()->screenshotPreviewPath().isEmpty()) {
        return false;
    }

    const QString previewPath = domain->config()->screenshotPreviewPath();
    QDir().mkpath(QFileInfo(previewPath).absolutePath());
    const QString tempPath = previewPath + QStringLiteral(".tmp.png");
    QFile::remove(tempPath);

    QPointer<Karton> guard(this);
    QPointer<Domain> domainGuard(const_cast<Domain *>(domain));
    m_previewCapturing = true;
    m_commandRunner->runCommand(QStringLiteral("virsh"),
                                {QStringLiteral("-c"), m_activeUri, QStringLiteral("screenshot"), domain->config()->name(), tempPath},
                                [guard, domainGuard, tempPath](bool success, const QString &output) {
                                    Q_UNUSED(output);
                                    if (!guard) {
                                        return;
                                    }
                                    guard->m_previewCapturing = false;
                                    if (!success) {
                                        QFile::remove(tempPath);
                                        return;
                                    }
                                    const QImage image(tempPath);
                                    QFile::remove(tempPath);
                                    if (image.isNull()) {
                                        return;
                                    }
                                    if (domainGuard) {
                                        domainGuard->savePreviewFrame(image);
                                    }
                                });
    return true;
}

QString Karton::domainOsIcon(const Domain *domain) const
{
    if (!domain) {
        return QStringLiteral("computer-symbolic");
    }
    return OsIconMapper::iconName(domain->config()->shortOsId(), domain->config()->osId());
}

QVariantMap Karton::mainWindowSize() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    QVariantMap size;
    const int width = settings.value(QStringLiteral("window/mainWidth"), 0).toInt();
    const int height = settings.value(QStringLiteral("window/mainHeight"), 0).toInt();
    if (width > 0 && height > 0) {
        size.insert(QStringLiteral("width"), width);
        size.insert(QStringLiteral("height"), height);
    }
    return size;
}

void Karton::saveMainWindowSize(int width, int height)
{
    if (width < 320 || height < 240) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("window/mainWidth"), width);
    settings.setValue(QStringLiteral("window/mainHeight"), height);
}

QVariantMap Karton::viewerWindowSize(const Domain *domain) const
{
    QVariantMap size;
    if (!domain) {
        return size;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("window/viewer/%1/").arg(domain->config()->uuid());
    const int width = settings.value(prefix + QStringLiteral("width"), 0).toInt();
    const int height = settings.value(prefix + QStringLiteral("height"), 0).toInt();
    if (width > 0 && height > 0) {
        size.insert(QStringLiteral("width"), width);
        size.insert(QStringLiteral("height"), height);
    }
    return size;
}

void Karton::saveViewerWindowSize(const Domain *domain, int width, int height)
{
    if (!domain || width < 320 || height < 240) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("window/viewer/%1/").arg(domain->config()->uuid());
    settings.setValue(prefix + QStringLiteral("width"), width);
    settings.setValue(prefix + QStringLiteral("height"), height);
}

bool Karton::audioPlaybackEnabled(const Domain *domain) const
{
    if (!domain) {
        return true;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("audio/%1/playback").arg(domain->config()->uuid()), true).toBool();
}

void Karton::saveAudioPlaybackEnabled(const Domain *domain, bool enabled)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("audio/%1/playback").arg(domain->config()->uuid()), enabled);
}

bool Karton::microphoneEnabled(const Domain *domain) const
{
    if (!domain) {
        return false;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("audio/%1/microphone").arg(domain->config()->uuid()), false).toBool();
}

void Karton::saveMicrophoneEnabled(const Domain *domain, bool enabled)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("audio/%1/microphone").arg(domain->config()->uuid()), enabled);
}

QVariantMap Karton::domainDisplayResolution(const Domain *domain) const
{
    QVariantMap resolution;
    if (!domain) {
        return resolution;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("display/%1/").arg(domain->config()->uuid());
    const int width = settings.value(prefix + QStringLiteral("width"), 0).toInt();
    const int height = settings.value(prefix + QStringLiteral("height"), 0).toInt();
    if (width > 0 && height > 0) {
        resolution.insert(QStringLiteral("width"), width);
        resolution.insert(QStringLiteral("height"), height);
    }
    return resolution;
}

void Karton::saveDomainDisplayResolution(const Domain *domain, int width, int height)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("display/%1/").arg(domain->config()->uuid());
    if (width <= 0 || height <= 0) {
        settings.remove(prefix + QStringLiteral("width"));
        settings.remove(prefix + QStringLiteral("height"));
    } else {
        settings.setValue(prefix + QStringLiteral("width"), width);
        settings.setValue(prefix + QStringLiteral("height"), height);
    }
}

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

QString Karton::domainOsName(const Domain *domain) const
{
    if (!domain) {
        return {};
    }
    return OsIconMapper::displayName(domain->config()->shortOsId(), domain->config()->osId());
}

QVariantMap Karton::domainStats(const Domain *domain)
{
    QVariantMap stats;
    if (!domain || !domain->domainPtr()) {
        return stats;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    virDomainInfo info = {};
    if (virDomainGetInfo(domainPtr, &info) < 0) {
        return stats;
    }

    const int vcpus = qMax(1, static_cast<int>(info.nrVirtCpu));
    const qint64 cpuTime = static_cast<qint64>(info.cpuTime);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString cacheKey = Domain::uuidString(domainPtr);
    if (cacheKey.isEmpty()) {
        cacheKey = QString::fromUtf8(virDomainGetName(domainPtr));
    }

    double cpuPercent = 0.0;
    const auto previous = m_cpuSamples.constFind(cacheKey);
    if (info.state == VIR_DOMAIN_RUNNING && previous != m_cpuSamples.cend() && now > previous->timestamp) {
        const qint64 elapsedMs = now - previous->timestamp;
        const qint64 usedNs = cpuTime - previous->cpuTime;
        if (elapsedMs > 0 && usedNs > 0) {
            cpuPercent = static_cast<double>(usedNs) * 100.0 / (static_cast<double>(elapsedMs) * 1000000.0 * vcpus);
            cpuPercent = qBound(0.0, cpuPercent, 100.0);
        }
    }
    m_cpuSamples.insert(cacheKey, {cpuTime, now});

    const unsigned long maxMemory = virDomainGetMaxMemory(domainPtr);
    stats.insert(QStringLiteral("cpuPercent"), cpuPercent);
    stats.insert(QStringLiteral("cpuTime"), cpuTime);
    stats.insert(QStringLiteral("memoryKiB"), static_cast<qulonglong>(info.memory));
    stats.insert(QStringLiteral("maxMemoryKiB"), static_cast<qulonglong>(maxMemory));
    stats.insert(QStringLiteral("vcpus"), vcpus);
    return stats;
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

bool Karton::attachUsbDevicePersistent(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device)
{
    if (!domain || !domain->domainPtr() || vendor.isEmpty() || product.isEmpty() || bus.isEmpty() || device.isEmpty()) {
        return false;
    }
    const QString xml = usbHostdevXml(vendor, product, bus, device);
    if (virDomainAttachDeviceFlags(domain->domainPtr(), xml.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) {
        Q_EMIT errorOccurred(i18nc("%1 is a USB device, %2 is an error",
                                   "Failed to persist USB device %1: %2",
                                   vendor + QLatin1Char(':') + product,
                                   QString::fromUtf8(virGetLastErrorMessage())));
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
        Q_EMIT errorOccurred(i18nc("%1 is a USB device, %2 is an error",
                                   "Failed to remove persisted USB device %1: %2",
                                   vendor + QLatin1Char(':') + product,
                                   QString::fromUtf8(virGetLastErrorMessage())));
        return false;
    }
    refreshDomain(domain->domainPtr());
    return true;
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
        // A running VM cannot use an internal snapshot without memory, so use
        // an external disk-only snapshot: libvirt creates one qcow2 overlay
        // per data disk and switches the live domain to the overlay.
        char *domainXml = virDomainGetXMLDesc(domain->domainPtr(), 0);
        if (!domainXml) {
            Q_EMIT errorOccurred(i18n("Could not read the VM configuration for the snapshot."));
            return false;
        }

        QDomDocument domainDocument;
        if (!domainDocument.setContent(QString::fromUtf8(domainXml))) {
            free(domainXml);
            Q_EMIT errorOccurred(i18n("Could not parse the VM configuration for the snapshot."));
            return false;
        }
        free(domainXml);

        QDomDocument snapshotDocument;
        QDomElement snapshotRoot = snapshotDocument.createElement(QStringLiteral("domainsnapshot"));
        snapshotDocument.appendChild(snapshotRoot);

        QDomElement snapshotName = snapshotDocument.createElement(QStringLiteral("name"));
        snapshotName.appendChild(snapshotDocument.createTextNode(name));
        snapshotRoot.appendChild(snapshotName);

        if (!description.trimmed().isEmpty()) {
            QDomElement descriptionElement = snapshotDocument.createElement(QStringLiteral("description"));
            descriptionElement.appendChild(snapshotDocument.createTextNode(description));
            snapshotRoot.appendChild(descriptionElement);
        }

        QDomElement memoryElement = snapshotDocument.createElement(QStringLiteral("memory"));
        memoryElement.setAttribute(QStringLiteral("snapshot"), QStringLiteral("no"));
        snapshotRoot.appendChild(memoryElement);

        QDomElement disksElement = snapshotDocument.createElement(QStringLiteral("disks"));
        snapshotRoot.appendChild(disksElement);

        QStringList overlayPaths;
        const QString domainName = domain->config()->name();
        const QDomNodeList disks = domainDocument.elementsByTagName(QStringLiteral("disk"));
        for (int i = 0; i < disks.count(); ++i) {
            const QDomElement disk = disks.at(i).toElement();
            if (disk.attribute(QStringLiteral("device")) != QStringLiteral("disk") || disk.attribute(QStringLiteral("type")) != QStringLiteral("file")) {
                continue;
            }
            const QDomElement source = disk.firstChildElement(QStringLiteral("source"));
            const QString sourceFile = source.attribute(QStringLiteral("file"));
            if (sourceFile.isEmpty()) {
                continue;
            }
            const QDomElement target = disk.firstChildElement(QStringLiteral("target"));
            const QString targetDev = target.attribute(QStringLiteral("dev"));
            if (targetDev.isEmpty()) {
                continue;
            }

            // Keep the overlay next to its backing file: for a qemu:///system
            // domain only the image directory is writable by the qemu user.
            const QString overlayPath = QFileInfo(sourceFile).absolutePath() + QLatin1Char('/') + domainName + QLatin1Char('-') + name + QLatin1Char('-')
                + targetDev + QStringLiteral(".qcow2");
            if (QFile::exists(overlayPath)) {
                for (const QString &path : std::as_const(overlayPaths)) {
                    QFile::remove(path);
                }
                Q_EMIT errorOccurred(i18nc("%1 is a file path", "Snapshot overlay already exists: %1", overlayPath));
                return false;
            }

            QDomElement diskElement = snapshotDocument.createElement(QStringLiteral("disk"));
            diskElement.setAttribute(QStringLiteral("name"), targetDev);
            diskElement.setAttribute(QStringLiteral("snapshot"), QStringLiteral("external"));
            diskElement.setAttribute(QStringLiteral("type"), QStringLiteral("file"));

            QDomElement sourceElement = snapshotDocument.createElement(QStringLiteral("source"));
            sourceElement.setAttribute(QStringLiteral("file"), overlayPath);
            diskElement.appendChild(sourceElement);

            QDomElement driverElement = snapshotDocument.createElement(QStringLiteral("driver"));
            driverElement.setAttribute(QStringLiteral("type"), QStringLiteral("qcow2"));
            diskElement.appendChild(driverElement);

            disksElement.appendChild(diskElement);
            overlayPaths.append(overlayPath);
        }

        if (overlayPaths.isEmpty()) {
            Q_EMIT errorOccurred(i18n("The VM has no local data disks to snapshot."));
            return false;
        }

        virDomainSnapshotPtr snapshot = virDomainSnapshotCreateXML(domain->domainPtr(),
                                                                   snapshotDocument.toString().toUtf8().constData(),
                                                                   VIR_DOMAIN_SNAPSHOT_CREATE_DISK_ONLY | VIR_DOMAIN_SNAPSHOT_CREATE_ATOMIC);
        if (!snapshot) {
            for (const QString &path : std::as_const(overlayPaths)) {
                QFile::remove(path);
            }
            Q_EMIT errorOccurred(
                i18nc("%1 is a snapshot name, %2 is an error", "Failed to create snapshot %1: %2", name, QString::fromUtf8(virGetLastErrorMessage())));
            return false;
        }

        virDomainSnapshotFree(snapshot);
        refreshDomain(domain->domainPtr());
        return true;
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

    // External disk snapshots cannot be deleted together with their children;
    // libvirt merges the overlay back into its backing file when the snapshot
    // metadata is removed on its own. Internal snapshots keep the old
    // delete-the-subtree behaviour.
    bool external = false;
    char *snapshotXml = virDomainSnapshotGetXMLDesc(snapshot, 0);
    if (snapshotXml) {
        external = QString::fromUtf8(snapshotXml).contains(QStringLiteral("snapshot='external'"));
        free(snapshotXml);
    }

    const bool success = virDomainSnapshotDelete(snapshot, external ? 0 : VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN) == 0;
    if (!success) {
        const QString errorMsg =
            i18nc("%1 is a snapshot name, %2 is an error", "Failed to delete snapshot %1: %2", name, QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }
    virDomainSnapshotFree(snapshot);
    if (success) {
        refreshDomain(domain->domainPtr());
    }
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

static QString normaliseDiskFormat(const QString &format)
{
    const QString lower = format.trimmed().toLower();
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
