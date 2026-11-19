// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"
#include "app/shortcutinhibitor.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
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
#include <QWindow>
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

// Karton is one QML singleton, but its implementation is split by
// responsibility across this directory: this file owns the connection and VM
// lifecycle, while karton_network.cpp, karton_devices.cpp, karton_disk.cpp,
// karton_domain.cpp, karton_usb.cpp, karton_pci.cpp, karton_snapshots.cpp,
// karton_settings.cpp and karton_os.cpp contain the
// corresponding feature groups.

Karton::Karton(QObject *parent)
    : QObject(parent)
    , m_conn(nullptr)
    , m_monitor(nullptr)
    , m_commandRunner(new CommandRunner(this))
{
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installEventFilter(this);
    }
    updateHolidayMode();
    m_holidayTimer.setInterval(3600000);
    connect(&m_holidayTimer, &QTimer::timeout, this, &Karton::updateHolidayMode);
    m_holidayTimer.start();
    init();
}

void Karton::updateHolidayMode()
{
    // A week before and after New Year: 25 December through 7 January.
    // KARTON_HOLIDAY_MODE=1/0 forces the easter egg on/off for testing.
    const QByteArray forced = qgetenv("KARTON_HOLIDAY_MODE").trimmed().toLower();
    const QDate today = QDate::currentDate();
    const int newYear = today.month() == 12 ? today.year() + 1 : today.year();
    const QDate start(newYear - 1, 12, 25);
    const QDate end(newYear, 1, 7);
    const bool active = forced == "1" || forced == "true" ? true : forced == "0" || forced == "false" ? false : today >= start && today <= end;
    if (active == m_holidayMode) {
        return;
    }
    m_holidayMode = active;
    Q_EMIT holidayModeChanged();
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
    if (m_shortcutInhibitor) {
        m_shortcutInhibitor->release();
    }
    m_shortcutBlockedViewers.clear();
    updateGlobalShortcutsBlock();
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
    // Clear a KGlobalAccel block left behind by a previous crashed viewer.
    if (QDBusConnection::sessionBus().isConnected()) {
        QDBusInterface iface(QStringLiteral("org.kde.kglobalaccel"),
                             QStringLiteral("/kglobalaccel"),
                             QStringLiteral("org.kde.KGlobalAccel"),
                             QDBusConnection::sessionBus());
        iface.call(QStringLiteral("blockGlobalShortcuts"), false);
    }
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

// get string from state
static QString domainStateString(unsigned int state)
{
    switch (state) {
    case VIR_DOMAIN_NOSTATE:
        return QCoreApplication::translate("Karton", "no_state");
    case VIR_DOMAIN_RUNNING:
        return QCoreApplication::translate("Karton", "running");
    case VIR_DOMAIN_BLOCKED:
        return QCoreApplication::translate("Karton", "blocked");
    case VIR_DOMAIN_PAUSED:
        return QCoreApplication::translate("Karton", "paused");
    case VIR_DOMAIN_SHUTDOWN:
        return QCoreApplication::translate("Karton", "shutting_down");
    case VIR_DOMAIN_SHUTOFF:
        return QCoreApplication::translate("Karton", "shutoff");
    case VIR_DOMAIN_CRASHED:
        return QCoreApplication::translate("Karton", "crashed");
    case VIR_DOMAIN_PMSUSPENDED:
        return QCoreApplication::translate("Karton", "suspended");
    default:
        return QCoreApplication::translate("Karton", "unknown");
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

// Capacity is available through libvirt for stopped domains too, and unlike
// qemu-img it keeps working when libvirt's dynamic ownership made the image
// unreadable for the current user.
static int domainDiskSizeGB(virDomainPtr domainPtr, const QString &path)
{
    if (!domainPtr || path.isEmpty()) {
        return 0;
    }
    virDomainBlockInfo blockInfo = {};
    if (virDomainGetBlockInfo(domainPtr, path.toUtf8().constData(), &blockInfo, 0) < 0 || blockInfo.capacity <= 0) {
        return 0;
    }
    return static_cast<int>((blockInfo.capacity + (1LL << 30) - 1) / (1LL << 30));
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
        // Previews are memory-only now. Remove screenshots written by older
        // versions instead of leaving them in the state directory.
        QFile::remove(screenshotPreviewPath);
        QFile::remove(screenshotPreviewPath + QStringLiteral(".tmp.png"));

        int autoFlag = 0;
        virDomainGetAutostart(domainPtr, &autoFlag);

        int maxDiskStorage = reader.m_xmlInfo.maxDiskStorage > 0 ? reader.m_xmlInfo.maxDiskStorage / 1024 : 0; // MiB -> GiB
        if (maxDiskStorage <= 0) {
            maxDiskStorage = domainDiskSizeGB(domainPtr, reader.m_xmlInfo.virtualDiskPath);
        }
        if (maxDiskStorage <= 0) {
            // Fallback for disks libvirt cannot describe (for example a path
            // that is not part of the persistent definition).
            maxDiskStorage = diskSizeGB(reader.m_xmlInfo.virtualDiskPath);
        }

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
            // Round up so a sub-GiB VM (for example 512 MiB) is not shown as 0 GB.
            .maxRam = static_cast<int>((domInfo.maxMem + 1024ULL * 1024ULL - 1) / (1024ULL * 1024ULL)),
            .ramUsage = static_cast<int>((domInfo.memory + 1024ULL * 1024ULL - 1) / (1024ULL * 1024ULL)),
            .cpus = static_cast<int>(domInfo.nrVirtCpu),
            .maxDiskStorage = maxDiskStorage,
            .xmlConfigPath = xmlConfigPath,
            .isoDiskPath = reader.m_xmlInfo.isoDiskPath,
            .virtualDiskPath = reader.m_xmlInfo.virtualDiskPath,
            .virtualDiskPaths = reader.m_xmlInfo.virtualDiskPaths,
            .diskFormat = reader.m_xmlInfo.diskFormat,
            .firmware = reader.m_xmlInfo.firmware,
            .networkModel = reader.m_xmlInfo.networkModel,
            .networkName = reader.m_xmlInfo.networkName,
            .secureBoot = reader.m_xmlInfo.secureBoot,
            .hasGraphics = reader.m_xmlInfo.hasGraphics,
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

    // Restore the order the user arranged in the VM list. Domains that are not
    // in the saved order (newly defined ones) stay at the end in libvirt order.
    const QString orderUri = m_activeUri.isEmpty() ? connectionUri() : m_activeUri;
    const QStringList savedOrder = storedDomainOrder(orderUri);
    if (!savedOrder.isEmpty()) {
        QHash<QString, Domain *> domainsByUuid;
        domainsByUuid.reserve(updated.size());
        for (Domain *domain : std::as_const(updated)) {
            domainsByUuid.insert(domain->config()->uuid(), domain);
        }

        QVector<Domain *> ordered;
        ordered.reserve(updated.size());
        for (const QString &uuid : savedOrder) {
            if (Domain *domain = domainsByUuid.take(uuid)) {
                ordered.append(domain);
            }
        }
        for (Domain *domain : std::as_const(updated)) {
            const QString uuid = domain->config()->uuid();
            if (domainsByUuid.contains(uuid)) {
                ordered.append(domain);
                domainsByUuid.remove(uuid);
            }
        }
        updated = ordered;
    }

    // Drop domains that disappeared from libvirt. deleteLater() keeps QML pages
    // that are still being torn down from touching a destroyed object.
    const QVector<Domain *> previousDomains = m_domains;
    m_domains = updated;

    QStringList currentOrder;
    currentOrder.reserve(m_domains.size());
    for (const Domain *domain : std::as_const(m_domains)) {
        currentOrder.append(domain->config()->uuid());
    }
    if (currentOrder != savedOrder) {
        saveDomainOrder(m_domains, orderUri);
    }

    for (Domain *domain : std::as_const(added)) {
        Q_EMIT domainAdded(domain);
    }
    for (Domain *domain : previousDomains) {
        if (!m_domains.contains(domain)) {
            removeDomainCache(domain->config()->uuid(), domain->config()->screenshotPreviewPath());
            Q_EMIT domainRemoved(domain);
            domain->deleteLater();
        }
    }

    // The runtime path above cleans VMs removed while Karton is running. Use a
    // per-connection list of known UUIDs so VMs deleted while Karton was closed
    // are cleaned up on the next start, without touching data that belongs to
    // another libvirt connection.
    QSet<QString> currentUuids;
    for (const Domain *domain : std::as_const(m_domains)) {
        currentUuids.insert(domain->config()->uuid());
    }
    const QString connectionKey =
        QString::fromLatin1(QCryptographicHash::hash((m_activeUri.isEmpty() ? connectionUri() : m_activeUri).toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString knownKey = QStringLiteral("knownDomains/%1").arg(connectionKey);
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QStringList previousKnown = settings.value(knownKey).toStringList();
    for (const QString &uuid : previousKnown) {
        if (!currentUuids.contains(uuid)) {
            removeDomainCache(uuid, QString());
        }
    }
    QStringList currentKnown = currentUuids.values();
    currentKnown.sort();
    if (currentKnown != previousKnown) {
        settings.setValue(knownKey, currentKnown);
        settings.sync();
    }
}

QVector<Domain *> Karton::domains()
{
    return m_domains;
}

void Karton::moveDomain(int fromRow, int toRow)
{
    if (fromRow == toRow || fromRow < 0 || toRow < 0 || fromRow >= m_domains.size() || toRow >= m_domains.size()) {
        return;
    }

    m_domains.move(fromRow, toRow);
    saveDomainOrder(m_domains, m_activeUri.isEmpty() ? connectionUri() : m_activeUri);
}

bool Karton::startDomain(const Domain *domain)
{
    if (!domain) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainCreate(domainPtr) < 0) {
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_start_domain_1_2").arg(domain->config()->name()).arg(QString::fromUtf8(virGetLastErrorMessage()));
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
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_get_state_of_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    if (info.state == VIR_DOMAIN_RUNNING || info.state == VIR_DOMAIN_PAUSED) {
        if (virDomainShutdown(domainPtr) < 0) {
            const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_stop_domain_1_2")
                                         .arg(domain->config()->name())
                                         .arg(QString::fromUtf8(virGetLastErrorMessage()));
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
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_force_stop_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
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
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_pause_domain_1_2").arg(domain->config()->name()).arg(QString::fromUtf8(virGetLastErrorMessage()));
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
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_resume_domain_1_2").arg(domain->config()->name()).arg(QString::fromUtf8(virGetLastErrorMessage()));
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
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_reboot_domain_1_2").arg(domain->config()->name()).arg(QString::fromUtf8(virGetLastErrorMessage()));
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
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_reset_domain_1_2").arg(domain->config()->name()).arg(QString::fromUtf8(virGetLastErrorMessage()));
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
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_save_the_state_of_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
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
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_delete_the_saved_state_of_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    refreshDomain(domainPtr);
    qCInfo(KARTON_DEBUG) << "Successfully deleted the saved state of domain:" << domain->config()->name();
    return true;
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

bool Karton::reserveViewer(const Domain *domain)
{
    if (!domain || !domain->config()) {
        return false;
    }
    const QString uuid = domain->config()->uuid();
    if (uuid.isEmpty() || m_viewerReservations.contains(uuid)) {
        return false;
    }
    // A registered or top-level window always wins over a new reservation.
    if (viewerForDomain(const_cast<Domain *>(domain))) {
        return false;
    }
    m_viewerReservations.insert(uuid);
    return true;
}

void Karton::releaseViewerReservation(const Domain *domain)
{
    if (domain && domain->config()) {
        m_viewerReservations.remove(domain->config()->uuid());
    }
}

QObject *Karton::viewerForDomain(Domain *domain) const
{
    if (!domain) {
        return nullptr;
    }
    const QString uuid = domain->config()->uuid();
    const auto known = m_knownViewers.constFind(uuid);
    if (known != m_knownViewers.constEnd() && !known.value().isNull()) {
        return known.value();
    }
    const auto it = m_viewers.constFind(uuid);
    if (it != m_viewers.constEnd() && !it.value().isNull()) {
        return it.value();
    }
    const auto preview = m_previewViewers.constFind(uuid);
    if (preview != m_previewViewers.constEnd() && !preview.value().isNull()) {
        return preview.value();
    }

    // Last-resort lookup by the window's domain property. This keeps opening
    // an existing VM window idempotent even if a QML registration was lost.
    const QWindowList windows = QGuiApplication::topLevelWindows();
    for (QWindow *window : windows) {
        const QVariant value = window->property("domain");
        auto *candidate = value.value<Domain *>();
        if (!candidate) {
            candidate = qobject_cast<Domain *>(value.value<QObject *>());
        }
        if (candidate && candidate->config()->uuid() == uuid) {
            return window;
        }
    }
    return nullptr;
}

void Karton::registerViewer(Domain *domain, QObject *window)
{
    if (!domain || !window) {
        return;
    }
    // Karton owns the viewer window. Keep the QWindow out of the QML engine's
    // garbage collector so it cannot be destroyed while the SPICE session and
    // the scene graph still use it; such a teardown aborts inside
    // QWaylandWindow::reset() on Wayland.
    QQmlEngine::setObjectOwnership(window, QQmlEngine::CppOwnership);
    m_viewerReservations.remove(domain->config()->uuid());
    m_viewers.insert(domain->config()->uuid(), window);
    m_knownViewers.insert(domain->config()->uuid(), window);
}

void Karton::closeAllViewers()
{
    const auto viewers = m_viewers.values();
    for (const QPointer<QObject> &viewer : viewers) {
        if (viewer) {
            QMetaObject::invokeMethod(viewer, "close", Qt::DirectConnection);
        }
    }
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

void Karton::registerPreviewViewer(Domain *domain, QObject *window)
{
    if (!domain || !window) {
        return;
    }
    QQmlEngine::setObjectOwnership(window, QQmlEngine::CppOwnership);
    m_viewerReservations.remove(domain->config()->uuid());
    m_previewViewers.insert(domain->config()->uuid(), window);
    m_knownViewers.insert(domain->config()->uuid(), window);
}

void Karton::unregisterPreviewViewer(QObject *window)
{
    for (auto it = m_previewViewers.begin(); it != m_previewViewers.end();) {
        if (it.value().isNull() || it.value() == window) {
            it = m_previewViewers.erase(it);
        } else {
            ++it;
        }
    }
}

void Karton::setViewerGlobalShortcutsBlocked(QObject *viewer, bool blocked)
{
    if (!viewer) {
        return;
    }
    if (blocked) {
        if (m_shortcutBlockedViewers.contains(viewer)) {
            return;
        }
        m_shortcutBlockedViewers.insert(viewer);
        if (auto *window = qobject_cast<QWindow *>(viewer)) {
            if (!m_shortcutInhibitor) {
                m_shortcutInhibitor = std::make_unique<ShortcutInhibitor>(this);
            }
            m_shortcutInhibitor->setWindow(window);
            m_shortcutInhibitor->setInhibited(true);
        }
        connect(viewer, &QObject::destroyed, this, [this, viewer]() {
            m_shortcutBlockedViewers.remove(viewer);
            if (m_shortcutBlockedViewers.isEmpty() && m_shortcutInhibitor) {
                m_shortcutInhibitor->release();
            }
            updateGlobalShortcutsBlock();
        });
    } else {
        m_shortcutBlockedViewers.remove(viewer);
        if (m_shortcutBlockedViewers.isEmpty() && m_shortcutInhibitor) {
            m_shortcutInhibitor->release();
        }
    }
    updateGlobalShortcutsBlock();
}

void Karton::updateGlobalShortcutsBlock()
{
    const bool block = !m_shortcutBlockedViewers.isEmpty();
    if (block == m_kglobalShortcutsBlocked) {
        return;
    }
    if (!QDBusConnection::sessionBus().isConnected()) {
        return;
    }
    // KWin still consumes some window shortcuts such as Alt+F4 even with the
    // keyboard-shortcuts-inhibit protocol. Keep this KDE-only fallback so a
    // running guest really receives every key. Other desktops ignore it.
    QDBusInterface iface(QStringLiteral("org.kde.kglobalaccel"),
                         QStringLiteral("/kglobalaccel"),
                         QStringLiteral("org.kde.KGlobalAccel"),
                         QDBusConnection::sessionBus());
    if (iface.isValid() && iface.call(QStringLiteral("blockGlobalShortcuts"), block).type() == QDBusMessage::ReplyMessage) {
        m_kglobalShortcutsBlocked = block;
    }
}

Domain *Karton::domainByUuid(const QString &uuid) const
{
    if (uuid.isEmpty()) {
        return nullptr;
    }
    for (Domain *candidate : std::as_const(m_domains)) {
        if (candidate->config()->uuid() == uuid) {
            return candidate;
        }
    }
    return nullptr;
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
    bool haveCpuPercent = false;
    const auto previous = m_cpuSamples.constFind(cacheKey);
    if (info.state == VIR_DOMAIN_RUNNING) {
        if (previous == m_cpuSamples.cend()) {
            m_cpuSamples.insert(cacheKey, {cpuTime, now, 0.0, false});
        } else if (now > previous->timestamp) {
            const qint64 elapsedMs = now - previous->timestamp;
            if (elapsedMs < 250) {
                // The VM page and an open Settings dialog can both poll. Reuse
                // the last value without resetting the sample timestamp.
                if (previous->valid) {
                    cpuPercent = previous->percent;
                    haveCpuPercent = true;
                }
            } else {
                const qint64 usedNs = qMax<qint64>(0, cpuTime - previous->cpuTime);
                cpuPercent = static_cast<double>(usedNs) * 100.0 / (static_cast<double>(elapsedMs) * 1000000.0 * vcpus);
                cpuPercent = qBound(0.0, cpuPercent, 100.0);
                haveCpuPercent = true;
                m_cpuSamples.insert(cacheKey, {cpuTime, now, cpuPercent, true});
            }
        } else if (previous->valid) {
            cpuPercent = previous->percent;
            haveCpuPercent = true;
        }
    }

    const unsigned long maxMemory = virDomainGetMaxMemory(domainPtr);

    // Do not call virDomainMemoryStats() here: it asks QEMU for memory
    // statistics over the monitor and can block for a long time (for example
    // while the guest is rebooting). This function runs from the VM page's
    // 2-second timer on the GUI thread, so such a call freezes the whole
    // window, including an open SPICE viewer. virDomainGetInfo() is answered
    // from libvirt's cached domain state and is safe to poll.
    if (haveCpuPercent) {
        stats.insert(QStringLiteral("cpuPercent"), cpuPercent);
    }
    stats.insert(QStringLiteral("cpuTime"), cpuTime);
    stats.insert(QStringLiteral("memoryKiB"), static_cast<qulonglong>(info.memory));
    stats.insert(QStringLiteral("maxMemoryKiB"), static_cast<qulonglong>(maxMemory));
    stats.insert(QStringLiteral("vcpus"), vcpus);

    const auto memorySample = m_guestMemory.constFind(cacheKey);
    if (memorySample != m_guestMemory.cend()) {
        stats.insert(QStringLiteral("memoryUsedKiB"), memorySample->usedKiB);
        stats.insert(QStringLiteral("memoryTotalKiB"), memorySample->totalKiB);
    }

    if (info.state == VIR_DOMAIN_RUNNING && !m_memoryRequests.contains(cacheKey) && now - m_memoryRequestTimes.value(cacheKey, 0) >= 2000) {
        const QString virsh = QStandardPaths::findExecutable(QStringLiteral("virsh"));
        if (!virsh.isEmpty() && !m_activeUri.isEmpty()) {
            m_memoryRequests.insert(cacheKey);
            m_memoryRequestTimes.insert(cacheKey, now);
            const QString name = QString::fromUtf8(virDomainGetName(domainPtr));
            m_commandRunner->runCommand(
                virsh,
                {QStringLiteral("-c"), m_activeUri, QStringLiteral("dommemstat"), name},
                [this, cacheKey, virsh, name](bool success, const QString &output) {
                    m_memoryRequests.remove(cacheKey);
                    if (!success) {
                        return;
                    }
                    qulonglong actual = 0;
                    qulonglong unused = 0;
                    qulonglong available = 0;
                    qulonglong usable = 0;
                    bool hasUnused = false;
                    bool hasAvailable = false;
                    const QStringList lines = output.split(QLatin1Char('\n'));
                    for (const QString &line : lines) {
                        const QStringList parts = line.simplified().split(QLatin1Char(' '));
                        if (parts.size() < 2) {
                            continue;
                        }
                        bool ok = false;
                        const qulonglong value = parts.at(1).toULongLong(&ok);
                        if (!ok) {
                            continue;
                        }
                        if (parts.at(0) == QStringLiteral("actual")) {
                            actual = value;
                        } else if (parts.at(0) == QStringLiteral("unused")) {
                            unused = value;
                            hasUnused = true;
                        } else if (parts.at(0) == QStringLiteral("available")) {
                            available = value;
                            hasAvailable = true;
                        } else if (parts.at(0) == QStringLiteral("usable")) {
                            usable = value;
                        }
                    }
                    const qulonglong total = actual > 0 ? actual : usable;
                    qulonglong used = 0;
                    if (hasUnused && total >= unused) {
                        used = total - unused;
                    } else if (hasAvailable && total >= available) {
                        used = total - available;
                    } else {
                        // Balloon polling is off. virsh's --period command
                        // prints no statistics; enable it and fetch on the
                        // next two-second tick.
                        m_commandRunner->runCommand(
                            virsh,
                            {QStringLiteral("-c"), m_activeUri, QStringLiteral("dommemstat"), QStringLiteral("--period"), QStringLiteral("2"), name},
                            [](bool, const QString &) { });
                        return;
                    }
                    applyGuestMemoryStats(cacheKey, used, total);
                });
        }
    }

    return stats;
}

void Karton::applyGuestMemoryStats(const QString &uuid, qulonglong usedKiB, qulonglong totalKiB)
{
    if (totalKiB == 0) {
        return;
    }
    m_guestMemory.insert(uuid, {usedKiB, totalKiB});
    Q_EMIT domainStatsUpdated(uuid);
}

bool Karton::setAutostart(const Domain *domain, bool autostart)
{
    if (!domain) {
        return false;
    }

    virDomainPtr domainPtr = domain->domainPtr();
    if (virDomainSetAutostart(domainPtr, autostart ? 1 : 0) < 0) {
        const QString errorMsg = QCoreApplication::translate("Karton", "failed_to_change_autostart_for_domain_1_2")
                                     .arg(domain->config()->name())
                                     .arg(QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const_cast<Domain *>(domain)->setAutostart(autostart);
    return true;
}
