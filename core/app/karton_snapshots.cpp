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

            // External disk-only snapshots keep one overlay per disk; their
            // total file size is a useful indicator of how much space the
            // snapshot uses. Internal snapshots live inside the disk image and
            // have no separate size to report.
            bool external = false;
            bool hasMemory = false;
            qint64 sizeBytes = 0;
            QSet<QString> countedFiles;
            const QDomElement disksElement = rootElement.firstChildElement(QStringLiteral("disks"));
            const QDomNodeList diskNodes = disksElement.elementsByTagName(QStringLiteral("disk"));
            for (int d = 0; d < diskNodes.count(); ++d) {
                const QDomElement disk = diskNodes.at(d).toElement();
                if (disk.attribute(QStringLiteral("snapshot")) != QStringLiteral("external")) {
                    continue;
                }
                external = true;
                const QString file = disk.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file"));
                if (!file.isEmpty() && !countedFiles.contains(file)) {
                    countedFiles.insert(file);
                    const QFileInfo info(file);
                    if (info.exists()) {
                        sizeBytes += info.size();
                    }
                }
            }
            const QDomElement memoryElement = rootElement.firstChildElement(QStringLiteral("memory"));
            if (!memoryElement.isNull() && memoryElement.attribute(QStringLiteral("snapshot")) != QStringLiteral("no")) {
                hasMemory = true;
            }

            QVariantMap info;
            info.insert(QStringLiteral("name"), nameElement.text());
            info.insert(QStringLiteral("description"), descriptionElement.text());
            info.insert(QStringLiteral("creationTime"), creationTime);
            info.insert(QStringLiteral("state"), stateElement.text());
            info.insert(QStringLiteral("parent"), parentNameElement.text());
            info.insert(QStringLiteral("current"), virDomainSnapshotIsCurrent(snapshots[i], 0) == 1);
            info.insert(QStringLiteral("type"), external ? QStringLiteral("external") : QStringLiteral("internal"));
            info.insert(QStringLiteral("hasMemory"), hasMemory);
            info.insert(QStringLiteral("sizeBytes"), sizeBytes);
            result.append(info);
        }

        free(xml);
        virDomainSnapshotFree(snapshots[i]);
    }
    free(snapshots);

    // Newest first, so the list reads as a timeline regardless of libvirt's
    // enumeration order.
    std::sort(result.begin(), result.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("creationTime")).toString() > b.toMap().value(QStringLiteral("creationTime")).toString();
    });
    return result;
}

bool Karton::createSnapshot(const Domain *domain, const QString &requestedName, const QString &description, bool includeMemory)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    const QString name = requestedName.trimmed();
    if (!isValidDomainName(name)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "a_snapshot_name_may_only_contain_letters_digits_dots_underscores_and_dashes"));
        return false;
    }
    if (domain->isActive() && !includeMemory) {
        // A running VM cannot use an internal snapshot without memory, so use
        // an external disk-only snapshot: libvirt creates one qcow2 overlay
        // per data disk and switches the live domain to the overlay.
        char *domainXml = virDomainGetXMLDesc(domain->domainPtr(), 0);
        if (!domainXml) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "could_not_read_the_vm_configuration_for_the_snapshot"));
            return false;
        }

        QDomDocument domainDocument;
        if (!domainDocument.setContent(QString::fromUtf8(domainXml))) {
            free(domainXml);
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "could_not_parse_the_vm_configuration_for_the_snapshot"));
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
                Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "snapshot_overlay_already_exists_1").arg(overlayPath));
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
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_vm_has_no_local_data_disks_to_snapshot"));
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
                QCoreApplication::translate("Karton", "failed_to_create_snapshot_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage())));
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
            QCoreApplication::translate("Karton", "failed_to_create_snapshot_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
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
            QCoreApplication::translate("Karton", "failed_to_find_snapshot_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
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
            QCoreApplication::translate("Karton", "failed_to_delete_snapshot_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
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
            QCoreApplication::translate("Karton", "failed_to_find_snapshot_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
        return false;
    }

    const bool success = virDomainRevertToSnapshot(snapshot, 0) == 0;
    if (!success) {
        const QString errorMsg =
            QCoreApplication::translate("Karton", "failed_to_revert_to_snapshot_1_2").arg(name).arg(QString::fromUtf8(virGetLastErrorMessage()));
        Q_EMIT errorOccurred(errorMsg);
    }
    virDomainSnapshotFree(snapshot);
    if (success) {
        refreshDomain(domain->domainPtr());
    }
    return success;
}
