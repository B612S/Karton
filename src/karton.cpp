// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "karton.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <KLocalizedString>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <utility>

#include "domain.h"
#include "domainconfig.h"
#include "domainxmlbuilder.h"
#include "domainxmlreader.h"
#include "karton_debug.h"
#include "libvirtmonitor.h"

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
    // Register event loop. Note: must be done before connection to hypervisor.
    virEventRegisterDefaultImpl();

    // Currently set to session, but could also do system for root..
    m_conn = virConnectOpen("qemu:///session");
    if (!m_conn) {
        qCCritical(KARTON_DEBUG) << "Failed to connect to hypervisor:" << virGetLastErrorMessage();
        return false;
    }

    qCInfo(KARTON_DEBUG) << "Connected to hypervisor";

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

        DomainXmlReader reader(xmlConfigPath);

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
            .maxDiskStorage = reader.m_xmlInfo.maxDiskStorage / 1024, // MiB -> GiB
            .xmlConfigPath = xmlConfigPath,
            .isoDiskPath = reader.m_xmlInfo.isoDiskPath,
            .virtualDiskPath = reader.m_xmlInfo.virtualDiskPath,
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

    const QString diskPath = domain->config()->virtualDiskPath();
    if (deleteDisk && !diskPath.isEmpty() && QFile::exists(diskPath) && !QFile::remove(diskPath)) {
        const QString errorMsg = i18nc("%1 is the path of the disk file", "Failed to delete disk file: %1", diskPath);
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        success = false;
    } else if (deleteDisk && !diskPath.isEmpty()) {
        qCInfo(KARTON_DEBUG) << "Successfully deleted disk image of" << domain->config()->name();
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
    configData.virtualDiskPath = getVirtualDiskPath(name);
    configData.enableAccel3d = config.value(QStringLiteral("enableAccel3d"), true).toBool();

    DomainConfig domainConfig(configData);

    if (!m_commandRunner->runCommandAndWait(QStringLiteral("qemu-img"),
                                            {QStringLiteral("create"),
                                             QStringLiteral("-f"),
                                             QStringLiteral("qcow2"),
                                             domainConfig.virtualDiskPath(),
                                             QStringLiteral("%1G").arg(domainConfig.maxDiskStorage())})) {
        Q_EMIT errorOccurred(i18nc("%1 is the name of the domain", "Failed to create disk image for domain: %1", name));
        return false;
    }

    DomainXmlBuilder builder;
    if (!builder.setupDomain(m_conn, &domainConfig)) {
        QFile::remove(domainConfig.virtualDiskPath());
        qCWarning(KARTON_DEBUG) << "Failed to define domain:" << name;
        Q_EMIT errorOccurred(i18nc("%1 is the name of the domain", "Failed to create domain: %1", name));
        return false;
    }

    qCInfo(KARTON_DEBUG) << "Successfully created domain:" << name;
    refreshDomainList();
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

    // Grow the disk before redefining the domain. Growing is safe even if the
    // subsequent define fails, while the opposite order would leave the config
    // and the disk out of sync.
    if (storageKnown && newStorageGB > oldStorageGB) {
        if (!m_commandRunner->runCommandAndWait(QStringLiteral("qemu-img"),
                                                {QStringLiteral("resize"), domainConfig->virtualDiskPath(), QStringLiteral("%1G").arg(newStorageGB)})) {
            const QString errorMsg = i18nc("%1 is the name of the domain", "Failed to resize disk for domain: %1", domainConfig->name());
            qCWarning(KARTON_DEBUG) << errorMsg;
            Q_EMIT errorOccurred(errorMsg);
            return false;
        }
    }

    virDomainPtr redefined = virDomainDefineXML(m_conn, xmlString.toUtf8().constData());
    if (!redefined) {
        const QString errorMsg = i18nc("%1 is the name of the domain, %2 is the error message",
                                       "Failed to update configuration for domain %1: %2",
                                       domainConfig->name(),
                                       QString::fromUtf8(virGetLastErrorMessage()));
        qCWarning(KARTON_DEBUG) << errorMsg;
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }
    virDomainFree(redefined);

    // Renaming must happen separately: redefining the XML with a different
    // <name> but the same UUID is rejected by libvirt as a UUID conflict.
    if (newName != domainConfig->name()) {
        if (virDomainRename(domainPtr, newName.toUtf8().constData(), 0) < 0) {
            const QString errorMsg = i18nc("%1 is the old name, %2 is the new name, %3 is the error message",
                                           "Failed to rename domain %1 to %2: %3",
                                           domainConfig->name(),
                                           newName,
                                           QString::fromUtf8(virGetLastErrorMessage()));
            qCWarning(KARTON_DEBUG) << errorMsg;
            Q_EMIT errorOccurred(errorMsg);
            refreshDomainList();
            return false;
        }
    }

    qCInfo(KARTON_DEBUG) << "Successfully updated configuration for domain:" << newName;

    // Reload the paths (a rename also changes the XML config file path).
    refreshDomainList();
    return true;
}

QString Karton::getXmlConfigPath(const QString &domainName)
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return QStringLiteral("%1/libvirt/qemu/%2.xml").arg(configDir, domainName);
}

QString Karton::getVirtualDiskPath(const QString &domainName)
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QStringLiteral("%1/libvirt/images/%2.qcow2").arg(dataDir, domainName);
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
