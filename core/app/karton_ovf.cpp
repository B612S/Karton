// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QDomAttr>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>
#include <utility>

namespace
{
void collectElements(const QDomElement &element, const QString &localName, QList<QDomElement> *result)
{
    if (element.localName() == localName || element.tagName() == localName) {
        result->append(element);
    }
    for (QDomNode node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
        if (node.isElement()) {
            collectElements(node.toElement(), localName, result);
        }
    }
}

QList<QDomElement> elementsByLocalName(QDomDocument &document, const QString &localName)
{
    QList<QDomElement> result;
    if (!document.documentElement().isNull()) {
        collectElements(document.documentElement(), localName, &result);
    }
    return result;
}

QString attributeByLocalName(const QDomElement &element, const QString &localName)
{
    const QDomNamedNodeMap attributes = element.attributes();
    for (int i = 0; i < attributes.count(); ++i) {
        const QDomAttr attribute = attributes.item(i).toAttr();
        if (attribute.localName() == localName || attribute.name() == localName) {
            return attribute.value();
        }
    }
    return element.attribute(localName);
}

QString childText(const QDomElement &parent, const QString &localName)
{
    const QDomNodeList children = parent.childNodes();
    for (int i = 0; i < children.count(); ++i) {
        const QDomElement child = children.at(i).toElement();
        if (child.isNull()) {
            continue;
        }
        if (child.localName() == localName || child.tagName() == localName) {
            return child.text().trimmed();
        }
    }
    return {};
}

QString friendlyName(const QString &source)
{
    QString name = source.trimmed();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
    name.replace(QRegularExpression(QStringLiteral("^-+|-+$")), QString());
    if (name.isEmpty()) {
        name = QStringLiteral("imported-vm");
    }
    return name;
}

qint64 capacityBytes(const QDomElement &disk)
{
    bool ok = false;
    const qint64 quantity = attributeByLocalName(disk, QStringLiteral("capacity")).toLongLong(&ok);
    if (!ok || quantity <= 0) {
        return 0;
    }
    const QString units = attributeByLocalName(disk, QStringLiteral("capacityAllocationUnits")).toLower();
    if (units.contains(QStringLiteral("2^30"))) {
        return quantity * (1LL << 30);
    }
    if (units.contains(QStringLiteral("2^20"))) {
        return quantity * (1LL << 20);
    }
    if (units.contains(QStringLiteral("2^10"))) {
        return quantity * 1024;
    }
    // The OVF default is bytes when no allocation units are given.
    return quantity;
}

bool extractOva(const QString &path, QString *ovfPath, QString *temporaryDirectory, QString *error)
{
    QProcess list;
    list.start(QStringLiteral("tar"), {QStringLiteral("-tf"), path});
    if (!list.waitForFinished(-1) || list.exitCode() != 0) {
        *error = QString::fromUtf8(list.readAllStandardError()).trimmed();
        if (error->isEmpty()) {
            *error = QStringLiteral("tar -tf failed");
        }
        return false;
    }

    const QStringList entries = QString::fromUtf8(list.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const QString simplified = QDir::cleanPath(entry);
        if (entry.startsWith(QLatin1Char('/')) || simplified.startsWith(QStringLiteral("../")) || simplified == QStringLiteral("..")
            || entry.contains(QLatin1Char('\\'))) {
            *error = QStringLiteral("The OVA archive contains an unsafe path: %1").arg(entry);
            return false;
        }
    }

    // Reject links and device nodes: a symlink inside the archive could make
    // a later entry escape the temporary directory.
    QProcess types;
    types.start(QStringLiteral("tar"), {QStringLiteral("-tvf"), path});
    if (!types.waitForFinished(-1) || types.exitCode() != 0) {
        *error = QStringLiteral("Could not inspect the OVA archive");
        return false;
    }
    const QStringList typeLines = QString::fromUtf8(types.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : typeLines) {
        if (line.isEmpty()) {
            continue;
        }
        const QChar type = line.at(0);
        if (type != QLatin1Char('-') && type != QLatin1Char('d')) {
            *error = QStringLiteral("The OVA archive contains a link or a special file");
            return false;
        }
    }

    const QString directory = QDir::tempPath() + QStringLiteral("/karton-ovf-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(directory)) {
        *error = QStringLiteral("Could not create the temporary directory");
        return false;
    }

    QProcess extract;
    extract.start(QStringLiteral("tar"), {QStringLiteral("--no-same-owner"), QStringLiteral("-xf"), path, QStringLiteral("-C"), directory});
    if (!extract.waitForFinished(-1) || extract.exitCode() != 0) {
        *error = QString::fromUtf8(extract.readAllStandardError()).trimmed();
        QDir(directory).removeRecursively();
        return false;
    }

    QDirIterator iterator(directory, {QStringLiteral("*.ovf")}, QDir::Files, QDirIterator::Subdirectories);
    if (!iterator.hasNext()) {
        *error = QStringLiteral("The OVA archive does not contain an OVF descriptor");
        QDir(directory).removeRecursively();
        return false;
    }
    *ovfPath = iterator.next();
    *temporaryDirectory = directory;
    return true;
}

QString resolvedHref(const QString &ovfDirectory, const QString &href)
{
    QString value = href.trimmed();
    if (value.startsWith(QStringLiteral("file://"))) {
        value = QUrl(value).toLocalFile();
    } else {
        value = QUrl::fromPercentEncoding(value.toUtf8());
    }
    QFileInfo info(value);
    if (info.isAbsolute()) {
        return value;
    }
    return QDir(ovfDirectory).filePath(value);
}

QDomElement addElement(QDomDocument &document, QDomElement parent, const QString &tag, const QString &text = {}, const QMap<QString, QString> &attributes = {})
{
    QDomElement element = document.createElement(tag);
    if (!text.isEmpty()) {
        element.appendChild(document.createTextNode(text));
    }
    for (auto it = attributes.cbegin(); it != attributes.cend(); ++it) {
        element.setAttribute(it.key(), it.value());
    }
    parent.appendChild(element);
    return element;
}

QString buildOvfDomainXml(const QString &name, int cpus, int memoryMiB, const QString &networkName, const QStringList &diskPaths)
{
    QString architecture = QSysInfo::currentCpuArchitecture();
    if (architecture == QStringLiteral("arm64")) {
        architecture = QStringLiteral("aarch64");
    } else if (architecture == QStringLiteral("i386")) {
        architecture = QStringLiteral("i686");
    } else if (architecture.isEmpty()) {
        architecture = QStringLiteral("x86_64");
    }

    QDomDocument document;
    QDomElement domain = document.createElement(QStringLiteral("domain"));
    domain.setAttribute(QStringLiteral("type"), QStringLiteral("kvm"));
    document.appendChild(domain);

    addElement(document, domain, QStringLiteral("name"), name);
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    addElement(document, domain, QStringLiteral("uuid"), uuid);

    addElement(document, domain, QStringLiteral("memory"), QString::number(memoryMiB), {{QStringLiteral("unit"), QStringLiteral("MiB")}});
    addElement(document, domain, QStringLiteral("currentMemory"), QString::number(memoryMiB), {{QStringLiteral("unit"), QStringLiteral("MiB")}});
    addElement(document, domain, QStringLiteral("vcpu"), QString::number(cpus));

    const QString machine = architecture == QStringLiteral("aarch64") ? QStringLiteral("virt") : QStringLiteral("q35");
    QDomElement os = addElement(document, domain, QStringLiteral("os"));
    addElement(document, os, QStringLiteral("type"), QStringLiteral("hvm"), {{QStringLiteral("arch"), architecture}, {QStringLiteral("machine"), machine}});
    addElement(document, os, QStringLiteral("boot"), QString(), {{QStringLiteral("dev"), QStringLiteral("hd")}});
    QDomElement features = addElement(document, domain, QStringLiteral("features"));
    addElement(document, features, QStringLiteral("acpi"));
    addElement(document, features, QStringLiteral("apic"));
    addElement(document, domain, QStringLiteral("cpu"), QString(), {{QStringLiteral("mode"), QStringLiteral("host-passthrough")}});
    addElement(document, domain, QStringLiteral("clock"), QString(), {{QStringLiteral("offset"), QStringLiteral("utc")}});

    QDomElement devices = addElement(document, domain, QStringLiteral("devices"));
    for (int i = 0; i < diskPaths.size(); ++i) {
        if (i >= 26) {
            break;
        }
        QDomElement disk = addElement(document,
                                      devices,
                                      QStringLiteral("disk"),
                                      QString(),
                                      {{QStringLiteral("type"), QStringLiteral("file")}, {QStringLiteral("device"), QStringLiteral("disk")}});
        addElement(document,
                   disk,
                   QStringLiteral("driver"),
                   QString(),
                   {{QStringLiteral("name"), QStringLiteral("qemu")}, {QStringLiteral("type"), QStringLiteral("qcow2")}});
        addElement(document, disk, QStringLiteral("source"), QString(), {{QStringLiteral("file"), diskPaths.at(i)}});
        addElement(document,
                   disk,
                   QStringLiteral("target"),
                   QString(),
                   {{QStringLiteral("dev"), QStringLiteral("vd") + QChar(QLatin1Char('a' + i))}, {QStringLiteral("bus"), QStringLiteral("virtio")}});
    }

    QDomElement graphics = addElement(document,
                                      devices,
                                      QStringLiteral("graphics"),
                                      QString(),
                                      {{QStringLiteral("type"), QStringLiteral("spice")}, {QStringLiteral("autoport"), QStringLiteral("yes")}});
    addElement(document,
               graphics,
               QStringLiteral("listen"),
               QString(),
               {{QStringLiteral("type"), QStringLiteral("address")}, {QStringLiteral("address"), QStringLiteral("127.0.0.1")}});
    QDomElement video = addElement(document, devices, QStringLiteral("video"));
    addElement(
        document,
        video,
        QStringLiteral("model"),
        QString(),
        {{QStringLiteral("type"), QStringLiteral("qxl")}, {QStringLiteral("heads"), QStringLiteral("1")}, {QStringLiteral("primary"), QStringLiteral("yes")}});

    QDomElement spiceChannel = addElement(document, devices, QStringLiteral("channel"), QString(), {{QStringLiteral("type"), QStringLiteral("spicevmc")}});
    addElement(document,
               spiceChannel,
               QStringLiteral("target"),
               QString(),
               {{QStringLiteral("type"), QStringLiteral("virtio")}, {QStringLiteral("name"), QStringLiteral("com.redhat.spice.0")}});
    QDomElement agentChannel = addElement(document, devices, QStringLiteral("channel"), QString(), {{QStringLiteral("type"), QStringLiteral("unix")}});
    addElement(document,
               agentChannel,
               QStringLiteral("target"),
               QString(),
               {{QStringLiteral("type"), QStringLiteral("virtio")}, {QStringLiteral("name"), QStringLiteral("org.qemu.guest_agent.0")}});

    addElement(document,
               devices,
               QStringLiteral("input"),
               QString(),
               {{QStringLiteral("type"), QStringLiteral("tablet")}, {QStringLiteral("bus"), QStringLiteral("usb")}});
    addElement(document,
               devices,
               QStringLiteral("input"),
               QString(),
               {{QStringLiteral("type"), QStringLiteral("keyboard")}, {QStringLiteral("bus"), QStringLiteral("usb")}});
    addElement(document, devices, QStringLiteral("memballoon"), QString(), {{QStringLiteral("model"), QStringLiteral("virtio")}});

    if (!networkName.isEmpty()) {
        QDomElement interface = addElement(document, devices, QStringLiteral("interface"), QString(), {{QStringLiteral("type"), QStringLiteral("network")}});
        addElement(document, interface, QStringLiteral("source"), QString(), {{QStringLiteral("network"), networkName}});
        addElement(document, interface, QStringLiteral("model"), QString(), {{QStringLiteral("type"), QStringLiteral("virtio")}});
    }

    return document.toString(-1);
}
} // namespace

bool Karton::importOvf(const QString &path, const QString &requestedName, const QString &destinationDirectory, const QString &networkName)
{
    if (m_ovfActive) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "another_ovf_import_is_already_running"));
        return false;
    }
    if (!m_conn) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "not_connected_to_libvirt"));
        return false;
    }
    const QFileInfo packageInfo(path);
    if (!packageInfo.exists() || !packageInfo.isFile()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_open_1_2").arg(path, QStringLiteral("No such file")));
        return false;
    }

    QString ovfPath;
    QString temporaryDirectory;
    QString extractionError;
    const QString suffix = packageInfo.suffix().toLower();
    if (suffix == QStringLiteral("ova")) {
        if (!extractOva(packageInfo.absoluteFilePath(), &ovfPath, &temporaryDirectory, &extractionError)) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_import_the_ova_1").arg(extractionError));
            return false;
        }
    } else if (suffix == QStringLiteral("ovf")) {
        ovfPath = packageInfo.absoluteFilePath();
    } else {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_file_is_not_an_ovf_or_ova_package"));
        return false;
    }
    const QString ovfDirectory = QFileInfo(ovfPath).absolutePath();

    QFile ovfFile(ovfPath);
    if (!ovfFile.open(QIODevice::ReadOnly)) {
        if (!temporaryDirectory.isEmpty()) {
            QDir(temporaryDirectory).removeRecursively();
        }
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_open_1_2").arg(ovfPath, ovfFile.errorString()));
        return false;
    }
    QDomDocument document;
    const QDomDocument::ParseResult parseResult = document.setContent(ovfFile.readAll());
    if (!parseResult) {
        if (!temporaryDirectory.isEmpty()) {
            QDir(temporaryDirectory).removeRecursively();
        }
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_parse_the_ovf_1").arg(parseResult.errorMessage));
        return false;
    }

    const QList<QDomElement> systems = elementsByLocalName(document, QStringLiteral("VirtualSystem"));
    if (systems.isEmpty()) {
        if (!temporaryDirectory.isEmpty()) {
            QDir(temporaryDirectory).removeRecursively();
        }
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_ovf_contains_no_virtual_system"));
        return false;
    }
    const QDomElement system = systems.first();

    QString name = requestedName.trimmed();
    if (name.isEmpty()) {
        name = childText(system, QStringLiteral("Name"));
    }
    name = friendlyName(name);
    if (!KartonCommon::isValidDomainName(name)) {
        if (!temporaryDirectory.isEmpty()) {
            QDir(temporaryDirectory).removeRecursively();
        }
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_virtual_machine_name_1").arg(name));
        return false;
    }
    // Avoid colliding with an existing domain; keep the import repeatable.
    QString uniqueName = name;
    for (int suffixIndex = 2; suffixIndex < 100; ++suffixIndex) {
        bool found = false;
        for (const Domain *domain : std::as_const(m_domains)) {
            if (domain->config()->name() == uniqueName) {
                found = true;
                break;
            }
        }
        if (!found) {
            break;
        }
        uniqueName = name + QLatin1Char('-') + QString::number(suffixIndex);
    }
    name = uniqueName;

    int cpus = 0;
    int memoryMiB = 0;
    QList<QDomElement> items;
    collectElements(system, QStringLiteral("Item"), &items);
    for (int i = 0; i < items.count(); ++i) {
        const QDomElement item = items.at(i);
        const QString resourceType = childText(item, QStringLiteral("ResourceType"));
        if (resourceType == QStringLiteral("3") && cpus == 0) {
            cpus = childText(item, QStringLiteral("VirtualQuantity")).toInt();
        } else if (resourceType == QStringLiteral("4") && memoryMiB == 0) {
            bool ok = false;
            const double quantity = childText(item, QStringLiteral("VirtualQuantity")).toDouble(&ok);
            if (ok && quantity > 0) {
                const QString units = childText(item, QStringLiteral("AllocationUnits")).toLower();
                double value = quantity;
                if (units.contains(QStringLiteral("2^30"))) {
                    value *= 1024;
                } else if (units.contains(QStringLiteral("2^20"))) {
                    // Already MiB.
                } else if (units.contains(QStringLiteral("2^10"))) {
                    value /= 1024;
                } else if (units.contains(QStringLiteral("byte"))) {
                    value /= (1024 * 1024);
                }
                memoryMiB = qMax(128, qRound(value));
            }
        }
    }
    if (cpus <= 0) {
        cpus = 2;
    }
    if (memoryMiB <= 0) {
        memoryMiB = 2048;
    }

    QHash<QString, QString> hrefs;
    const QList<QDomElement> files = elementsByLocalName(document, QStringLiteral("File"));
    for (int i = 0; i < files.count(); ++i) {
        const QDomElement file = files.at(i);
        const QString id = attributeByLocalName(file, QStringLiteral("id"));
        const QString href = attributeByLocalName(file, QStringLiteral("href"));
        if (!id.isEmpty() && !href.isEmpty()) {
            hrefs.insert(id, href);
        }
    }

    QStringList sources;
    QStringList createCapacities;
    const QList<QDomElement> disks = elementsByLocalName(document, QStringLiteral("Disk"));
    for (int i = 0; i < disks.count(); ++i) {
        const QDomElement disk = disks.at(i);
        const QString fileRef = attributeByLocalName(disk, QStringLiteral("fileRef"));
        const QString href = hrefs.value(fileRef);
        if (!fileRef.isEmpty() && href.isEmpty()) {
            if (!temporaryDirectory.isEmpty()) {
                QDir(temporaryDirectory).removeRecursively();
            }
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_ovf_references_an_unknown_file_1").arg(fileRef));
            return false;
        }
        if (!href.isEmpty()) {
            const QString sourcePath = resolvedHref(ovfDirectory, href);
            if (!QFileInfo::exists(sourcePath)) {
                if (!temporaryDirectory.isEmpty()) {
                    QDir(temporaryDirectory).removeRecursively();
                }
                Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_ovf_disk_1_does_not_exist").arg(sourcePath));
                return false;
            }
            sources.append(sourcePath);
            createCapacities.append(QString());
        } else {
            const qint64 capacity = capacityBytes(disk);
            if (capacity <= 0) {
                if (!temporaryDirectory.isEmpty()) {
                    QDir(temporaryDirectory).removeRecursively();
                }
                Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_ovf_disk_1_has_no_source_or_capacity")
                                         .arg(attributeByLocalName(disk, QStringLiteral("diskId"))));
                return false;
            }
            sources.append(QString());
            createCapacities.append(QString::number(capacity));
        }
    }
    if (sources.size() > 26) {
        if (!temporaryDirectory.isEmpty()) {
            QDir(temporaryDirectory).removeRecursively();
        }
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_ovf_contains_too_many_disks"));
        return false;
    }
    if (sources.isEmpty()) {
        if (!temporaryDirectory.isEmpty()) {
            QDir(temporaryDirectory).removeRecursively();
        }
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_ovf_contains_no_disks"));
        return false;
    }

    if (!networkName.isEmpty()) {
        virNetworkPtr network = virNetworkLookupByName(m_conn, networkName.toUtf8().constData());
        if (!network) {
            if (!temporaryDirectory.isEmpty()) {
                QDir(temporaryDirectory).removeRecursively();
            }
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "network_1_was_not_found").arg(networkName));
            return false;
        }
        virNetworkFree(network);
    }

    QString destination = destinationDirectory.trimmed();
    if (destination.isEmpty()) {
        destination = imagesDirectory();
    }
    if (!QDir().mkpath(destination)) {
        if (!temporaryDirectory.isEmpty()) {
            QDir(temporaryDirectory).removeRecursively();
        }
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_create_the_folder_1").arg(destination));
        return false;
    }

    QStringList destinations;
    for (int i = 0; i < sources.size(); ++i) {
        const QString target = QDir(destination).filePath(name + QStringLiteral("-disk") + QString::number(i + 1) + QStringLiteral(".qcow2"));
        if (QFileInfo::exists(target)) {
            if (!temporaryDirectory.isEmpty()) {
                QDir(temporaryDirectory).removeRecursively();
            }
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_destination_disk_1_already_exists").arg(target));
            return false;
        }
        destinations.append(target);
    }

    m_ovfActive = true;
    m_ovfName = name;
    m_ovfDestinationDirectory = destination;
    m_ovfNetwork = networkName;
    m_ovfTemporaryDirectory = temporaryDirectory;
    m_ovfDiskSources = sources;
    m_ovfDiskDestinations = destinations;
    m_ovfDiskCreateCapacities = createCapacities;
    m_ovfCreatedFiles.clear();
    m_ovfCpus = cpus;
    m_ovfMemoryMiB = memoryMiB;
    m_ovfNextDisk = 0;

    Q_EMIT ovfImportStarted(name);
    convertNextOvfDisk();
    return true;
}

void Karton::convertNextOvfDisk()
{
    if (!m_ovfActive) {
        return;
    }
    if (m_ovfNextDisk >= m_ovfDiskDestinations.size()) {
        finishOvfImport(true, QString());
        return;
    }

    const int index = m_ovfNextDisk;
    const QString destination = m_ovfDiskDestinations.at(index);
    QStringList arguments;
    if (m_ovfDiskSources.at(index).isEmpty()) {
        arguments = {QStringLiteral("create"), QStringLiteral("-f"), QStringLiteral("qcow2"), destination, m_ovfDiskCreateCapacities.at(index)};
    } else {
        arguments = {QStringLiteral("convert"), QStringLiteral("-O"), QStringLiteral("qcow2"), QStringLiteral("-U"), m_ovfDiskSources.at(index), destination};
    }

    m_commandRunner->runCommand(QStringLiteral("qemu-img"), arguments, [this, destination](bool success, const QString &output) {
        if (!success) {
            QFile::remove(destination);
            finishOvfImport(false, output.trimmed());
            return;
        }
        m_ovfCreatedFiles.append(destination);
        ++m_ovfNextDisk;
        convertNextOvfDisk();
    });
}

void Karton::finishOvfImport(bool success, const QString &message)
{
    const QString name = m_ovfName;
    QString failure = message;
    if (success) {
        const QString xml = buildOvfDomainXml(name, m_ovfCpus, m_ovfMemoryMiB, m_ovfNetwork, m_ovfDiskDestinations);
        virDomainPtr domain = virDomainDefineXMLFlags(m_conn, xml.toUtf8().constData(), VIR_DOMAIN_DEFINE_VALIDATE);
        if (!domain) {
            domain = virDomainDefineXML(m_conn, xml.toUtf8().constData());
        }
        if (!domain) {
            success = false;
            failure = QCoreApplication::translate("Karton", "failed_to_define_the_domain_1").arg(QString::fromUtf8(virGetLastErrorMessage()));
        } else {
            virDomainFree(domain);
        }
    }

    if (!success) {
        for (const QString &file : std::as_const(m_ovfCreatedFiles)) {
            QFile::remove(file);
        }
        if (!failure.isEmpty()) {
            Q_EMIT errorOccurred(failure);
        }
    } else {
        refreshDomainList();
        Q_EMIT domainImported(true, name);
    }

    if (!m_ovfTemporaryDirectory.isEmpty()) {
        QDir(m_ovfTemporaryDirectory).removeRecursively();
    }
    m_ovfTemporaryDirectory.clear();
    m_ovfCreatedFiles.clear();
    m_ovfDiskSources.clear();
    m_ovfDiskDestinations.clear();
    m_ovfDiskCreateCapacities.clear();
    m_ovfActive = false;
    m_ovfNextDisk = 0;

    Q_EMIT ovfImportFinished(success, name, failure);
}
