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
#include <QProcess>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSGRendererInterface>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <grp.h>
#include <memory>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
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
// The guest-agent flags are only valid for the current guest image. Store a
// small fingerprint so changing the system disk, its format or the OS metadata
// makes Karton re-evaluate the hint instead of trusting a stale "agent seen".
QString guestAgentPrefix(const Domain *domain)
{
    return QStringLiteral("guestAgent/%1/").arg(domain->config()->uuid());
}

QString guestAgentDiskPath(const Domain *domain)
{
    if (!domain) {
        return {};
    }
    // While a VM is running, its inactive configuration can already point at a
    // replacement image that will only be used after the next start. Attribute
    // the current agent connection to the live image instead.
    if (domain->isActive() && domain->domainPtr()) {
        char *xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
        if (xml) {
            QString livePath;
            QDomDocument document;
            if (document.setContent(QString::fromUtf8(xml))) {
                const QDomElement devices = document.documentElement().firstChildElement(QStringLiteral("devices"));
                const QDomNodeList disks = devices.elementsByTagName(QStringLiteral("disk"));
                for (int i = 0; i < disks.count(); ++i) {
                    const QDomElement disk = disks.at(i).toElement();
                    if (disk.attribute(QStringLiteral("device")) != QStringLiteral("disk")) {
                        continue;
                    }
                    const QString source = disk.firstChildElement(QStringLiteral("source")).attribute(QStringLiteral("file"));
                    if (!source.isEmpty()) {
                        livePath = source;
                        break;
                    }
                }
            }
            free(xml);
            if (!livePath.isEmpty()) {
                return livePath;
            }
        }
    }
    return domain->config()->virtualDiskPath();
}

QString guestAgentFingerprint(const Domain *domain)
{
    const DomainConfig *config = domain->config();
    const QStringList parts{
        config->uuid(),
        config->osId(),
        guestAgentDiskPath(domain),
        config->diskFormat(),
        config->firmware(),
    };
    return parts.join(QLatin1Char('\n'));
}

void prepareGuestAgentState(QSettings &settings, const QString &prefix, const QString &fingerprint)
{
    if (settings.value(prefix + QStringLiteral("fingerprint")).toString() == fingerprint) {
        return;
    }
    settings.remove(prefix + QStringLiteral("seen"));
    settings.remove(prefix + QStringLiteral("warningShown"));
    settings.setValue(prefix + QStringLiteral("fingerprint"), fingerprint);
}

int aclBits(const QString &permissions)
{
    int bits = 0;
    if (permissions.contains(QLatin1Char('r'))) {
        bits |= R_OK;
    }
    if (permissions.contains(QLatin1Char('w'))) {
        bits |= W_OK;
    }
    if (permissions.contains(QLatin1Char('x'))) {
        bits |= X_OK;
    }
    return bits;
}

int modeBitsFor(const struct stat &info, uid_t uid, const QVector<gid_t> &groups)
{
    if (info.st_uid == uid) {
        return (info.st_mode >> 6) & 7;
    }
    if (groups.contains(info.st_gid)) {
        return (info.st_mode >> 3) & 7;
    }
    return info.st_mode & 7;
}

bool isUnsafeImageDirectory(const QString &path)
{
    const QString home = QDir::cleanPath(QDir::homePath());
    if (path == QDir::rootPath() || path == home || path == home + QStringLiteral("/.local") || path == home + QStringLiteral("/.local/share")
        || path == home + QStringLiteral("/.cache") || path == home + QStringLiteral("/.config")) {
        return true;
    }
    if (path == QStringLiteral("/home")) {
        return true;
    }
    if (path.startsWith(QStringLiteral("/home/")) && !path.startsWith(home + QLatin1Char('/'))) {
        return true;
    }
    static const QStringList systemPrefixes = {QStringLiteral("/etc"),
                                               QStringLiteral("/usr"),
                                               QStringLiteral("/bin"),
                                               QStringLiteral("/sbin"),
                                               QStringLiteral("/lib"),
                                               QStringLiteral("/lib64"),
                                               QStringLiteral("/boot"),
                                               QStringLiteral("/dev"),
                                               QStringLiteral("/proc"),
                                               QStringLiteral("/sys"),
                                               QStringLiteral("/root"),
                                               QStringLiteral("/run")};
    for (const QString &prefix : systemPrefixes) {
        if (path == prefix || path.startsWith(prefix + QLatin1Char('/'))) {
            return true;
        }
    }
    static const QStringList sensitiveHomeDirectories = {QStringLiteral(".ssh"), QStringLiteral(".gnupg")};
    for (const QString &name : sensitiveHomeDirectories) {
        if (path == home + QLatin1Char('/') + name || path.startsWith(home + QLatin1Char('/') + name + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

bool qemuCanAccess(const QString &path, uid_t uid, const QVector<gid_t> &groups, int required)
{
    struct stat info = {};
    const QByteArray nativePath = path.toLocal8Bit();
    if (::stat(nativePath.constData(), &info) != 0) {
        return false;
    }

    QProcess process;
    process.start(QStringLiteral("getfacl"), {QStringLiteral("-n"), QStringLiteral("-c"), QStringLiteral("-p"), path});
    if (!process.waitForFinished(1500) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        // No getfacl available: fall back to the traditional mode bits.
        return (modeBitsFor(info, uid, groups) & required) == required;
    }

    int ownerBits = 0;
    int namedUserBits = -1;
    int groupBits = -1;
    int namedGroupBits = 0;
    bool groupMatched = false;
    int mask = 7;
    int otherBits = 0;

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList fields = line.split(QLatin1Char(':'));
        if (fields.size() < 3) {
            continue;
        }
        const QString &tag = fields.at(0);
        const QString &qualifier = fields.at(1);
        const int bits = aclBits(fields.at(2));
        if (tag == QStringLiteral("user")) {
            if (qualifier.isEmpty()) {
                ownerBits = bits;
            } else if (qualifier.toUInt() == uid) {
                namedUserBits = bits;
            }
        } else if (tag == QStringLiteral("group")) {
            if (qualifier.isEmpty()) {
                groupBits = bits;
            } else if (groups.contains(qualifier.toUInt())) {
                groupMatched = true;
                namedGroupBits |= bits;
            }
        } else if (tag == QStringLiteral("mask")) {
            mask = bits;
        } else if (tag == QStringLiteral("other")) {
            otherBits = bits;
        }
    }

    const bool owningGroup = groups.contains(info.st_gid) && groupBits >= 0;
    groupMatched = groupMatched || owningGroup;
    const int groupClass = (owningGroup ? groupBits : 0) | namedGroupBits;
    int effective = otherBits;
    if (uid == info.st_uid) {
        effective = ownerBits;
    } else if (namedUserBits >= 0) {
        effective = namedUserBits & mask;
    } else if (groupMatched) {
        effective = groupClass & mask;
    }
    return (effective & required) == required;
}
} // namespace

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

int Karton::clipboardTextMode() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return qBound(0, settings.value(QStringLiteral("clipboard/textMode"), 3).toInt(), 3);
}

void Karton::saveClipboardTextMode(int mode)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("clipboard/textMode"), qBound(0, mode, 3));
    Q_EMIT clipboardSettingsChanged();
}

int Karton::clipboardFilesMode() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return qBound(0, settings.value(QStringLiteral("clipboard/filesMode"), 0).toInt(), 3);
}

void Karton::saveClipboardFilesMode(int mode)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("clipboard/filesMode"), qBound(0, mode, 3));
    Q_EMIT clipboardSettingsChanged();
}

int Karton::globalConsoleFontSize() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return qBound(8, settings.value(QStringLiteral("console/fontSize"), 12).toInt(), 96);
}

void Karton::saveGlobalConsoleFontSize(int pixels)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("console/fontSize"), qBound(8, pixels, 96));
    Q_EMIT consoleFontSizeChanged();
}

int Karton::domainConsoleFontSize(const Domain *domain) const
{
    if (!domain) {
        return 0;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const int value = settings.value(QStringLiteral("console/%1/fontSize").arg(domain->config()->uuid()), 0).toInt();
    return value > 0 ? qBound(8, value, 96) : 0;
}

void Karton::saveDomainConsoleFontSize(const Domain *domain, int pixels)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString key = QStringLiteral("console/%1/fontSize").arg(domain->config()->uuid());
    if (pixels <= 0) {
        settings.remove(key);
    } else {
        settings.setValue(key, qBound(8, pixels, 96));
    }
    Q_EMIT consoleFontSizeChanged();
}

int Karton::consoleFontSize(const Domain *domain) const
{
    const int overrideSize = domainConsoleFontSize(domain);
    return overrideSize > 0 ? overrideSize : globalConsoleFontSize();
}

QString Karton::consoleCursorShape() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString shape = settings.value(QStringLiteral("console/cursorShape"), QStringLiteral("block")).toString();
    return shape == QStringLiteral("bar") || shape == QStringLiteral("underline") ? shape : QStringLiteral("block");
}

void Karton::saveConsoleCursorShape(const QString &shape)
{
    const QString normalized = shape == QStringLiteral("bar") || shape == QStringLiteral("underline") ? shape : QStringLiteral("block");
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("console/cursorShape"), normalized);
    Q_EMIT consoleCursorShapeChanged();
}

int Karton::cleanupStaleCaches()
{
    if (!m_conn) {
        return 0;
    }

    QSet<QString> knownUuids;
    for (const Domain *domain : std::as_const(m_domains)) {
        knownUuids.insert(domain->config()->uuid());
    }

    int removed = 0;
    const QString previewDirectory = QStandardPaths::writableLocation(QStandardPaths::StateLocation) + QStringLiteral("/previews");
    QDir previewDir(previewDirectory);
    if (previewDir.exists()) {
        const QFileInfoList files = previewDir.entryInfoList({QStringLiteral("*.png")}, QDir::Files);
        for (const QFileInfo &file : files) {
            const QString name = file.fileName();
            QString uuid = name;
            const bool temporary = name.endsWith(QStringLiteral(".tmp.png"));
            if (temporary) {
                uuid.chop(8);
            } else {
                uuid.chop(4);
            }
            if (temporary || !knownUuids.contains(uuid)) {
                if (QFile::remove(file.absoluteFilePath())) {
                    ++removed;
                }
            }
        }
    }

    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QStringList prefixes = {QStringLiteral("window/viewer"), QStringLiteral("audio"), QStringLiteral("display"), QStringLiteral("console")};
    for (const QString &prefix : prefixes) {
        settings.beginGroup(prefix);
        const QStringList groups = settings.childGroups();
        settings.endGroup();
        for (const QString &group : groups) {
            if (!knownUuids.contains(group)) {
                settings.remove(prefix + QLatin1Char('/') + group);
                ++removed;
            }
        }
    }
    settings.sync();
    return removed;
}

int Karton::clearPreviewCache()
{
    QStandardPaths::StandardLocation location = QStandardPaths::StateLocation;
    const QString previewDirectory = QStandardPaths::writableLocation(location) + QStringLiteral("/previews");
    int removed = 0;
    for (Domain *domain : std::as_const(m_domains)) {
        domain->clearPreviewFrame();
    }
    QDir previewDir(previewDirectory);
    if (previewDir.exists()) {
        const QFileInfoList files = previewDir.entryInfoList({QStringLiteral("*.png")}, QDir::Files);
        for (const QFileInfo &file : files) {
            if (QFile::remove(file.absoluteFilePath())) {
                ++removed;
            }
        }
    }
    return removed;
}

void Karton::removeDomainCache(const QString &uuid, const QString &previewPath)
{
    if (uuid.isEmpty()) {
        return;
    }
    if (!previewPath.isEmpty()) {
        QFile::remove(previewPath);
    }
    const QString previewDirectory = QStandardPaths::writableLocation(QStandardPaths::StateLocation) + QStringLiteral("/previews");
    QFile::remove(previewDirectory + QLatin1Char('/') + uuid + QStringLiteral(".png"));
    QFile::remove(previewDirectory + QLatin1Char('/') + uuid + QStringLiteral(".png.tmp.png"));
    QFile::remove(previewDirectory + QLatin1Char('/') + uuid + QStringLiteral(".tmp.png"));

    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QStringList prefixes = {QStringLiteral("window/viewer"), QStringLiteral("audio"), QStringLiteral("display"), QStringLiteral("console")};
    for (const QString &prefix : prefixes) {
        settings.remove(prefix + QLatin1Char('/') + uuid);
    }
    settings.sync();
}

QString Karton::defaultImagesDirectory() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/libvirt/images");
}

QString Karton::imagesDirectory() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString configured = settings.value(QStringLiteral("storage/imagesDirectory")).toString().trimmed();
    return configured.isEmpty() ? defaultImagesDirectory() : QDir::cleanPath(configured);
}

void Karton::saveImagesDirectory(const QString &path)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty() || QDir::cleanPath(trimmed) == defaultImagesDirectory()) {
        settings.remove(QStringLiteral("storage/imagesDirectory"));
    } else {
        settings.setValue(QStringLiteral("storage/imagesDirectory"), QDir::cleanPath(trimmed));
    }
    Q_EMIT imagesDirectoryChanged();
}

QString Karton::grantDiskDirectoryAccess(const QString &path)
{
    if (path.trimmed().isEmpty()) {
        return QCoreApplication::translate("Karton", "the_disk_folder_is_empty");
    }
    const QString directoryPath = QDir::cleanPath(QDir(path).absolutePath());
    if (isUnsafeImageDirectory(directoryPath)) {
        return QCoreApplication::translate("Karton", "choose_a_dedicated_folder_for_vm_images_not_a_system_home_or_sensitive_directory");
    }
    if (!QDir().mkpath(directoryPath)) {
        return QCoreApplication::translate("Karton", "failed_to_create_the_disk_folder_1").arg(directoryPath);
    }
    if (::access(directoryPath.toLocal8Bit().constData(), R_OK | W_OK | X_OK) != 0) {
        return QCoreApplication::translate("Karton", "karton_has_no_write_access_to_the_disk_folder_1").arg(directoryPath);
    }

    if (!m_activeUri.startsWith(QStringLiteral("qemu:///system"))) {
        return {};
    }
    struct passwd *qemuUser = getpwnam("libvirt-qemu");
    if (!qemuUser) {
        qemuUser = getpwnam("qemu");
    }
    if (!qemuUser) {
        return QCoreApplication::translate("Karton", "could_not_find_the_libvirt_qemu_user_grant_access_manually");
    }

    gid_t groupBuffer[256];
    int groupCount = 256;
    if (getgrouplist(qemuUser->pw_name, qemuUser->pw_gid, groupBuffer, &groupCount) < 0) {
        groupCount = 1;
    }
    QVector<gid_t> groups;
    groups.reserve(groupCount);
    for (int i = 0; i < groupCount; ++i) {
        groups.append(groupBuffer[i]);
    }

    QString current = QStringLiteral("/");
    bool changed = false;
    const QStringList parts = directoryPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        current = QDir(current).filePath(parts.at(i));
        const bool isTarget = (i == parts.size() - 1);
        const int required = isTarget ? (R_OK | W_OK | X_OK) : X_OK;
        if (qemuCanAccess(current, qemuUser->pw_uid, groups, required)) {
            continue;
        }
        const QFileInfo info(current);
        if (info.ownerId() != static_cast<uint>(::getuid())) {
            return QCoreApplication::translate("Karton", "cannot_grant_access_to_1_because_it_is_owned_by_another_user").arg(current);
        }
        const QString permissions = isTarget ? QStringLiteral("rwx") : QStringLiteral("x");
        const QString acl = QStringLiteral("u:%1:%2").arg(QString::fromUtf8(qemuUser->pw_name), permissions);
        if (QProcess::execute(QStringLiteral("setfacl"), {QStringLiteral("-m"), acl, current}) != 0) {
            return QCoreApplication::translate("Karton", "failed_to_grant_libvirt_access_to_1").arg(current);
        }
        changed = true;
        {
            QSettings settings(kartonConfigPath(), QSettings::IniFormat);
            QStringList aclPaths = settings.value(QStringLiteral("storage/grantedAclPaths")).toStringList();
            if (!aclPaths.contains(current)) {
                aclPaths.append(current);
                settings.setValue(QStringLiteral("storage/grantedAclPaths"), aclPaths);
            }
            QStringList directories = settings.value(QStringLiteral("storage/grantedDirectories")).toStringList();
            if (!directories.contains(directoryPath)) {
                directories.append(directoryPath);
                settings.setValue(QStringLiteral("storage/grantedDirectories"), directories);
            }
        }
        if (!qemuCanAccess(current, qemuUser->pw_uid, groups, required)) {
            return QCoreApplication::translate("Karton", "libvirt_still_cannot_access_1_after_granting_permissions").arg(current);
        }
    }
    if (changed) {
        QSettings settings(kartonConfigPath(), QSettings::IniFormat);
        QStringList directories = settings.value(QStringLiteral("storage/grantedDirectories")).toStringList();
        if (!directories.contains(directoryPath)) {
            directories.append(directoryPath);
            settings.setValue(QStringLiteral("storage/grantedDirectories"), directories);
        }
    }
    return {};
}

QStringList Karton::grantedDiskDirectories() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("storage/grantedDirectories")).toStringList();
}

QString Karton::revokeGrantedDiskAccess()
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QStringList aclPaths = settings.value(QStringLiteral("storage/grantedAclPaths")).toStringList();
    if (aclPaths.isEmpty()) {
        settings.remove(QStringLiteral("storage/grantedDirectories"));
        return {};
    }

    struct passwd *qemuUser = getpwnam("libvirt-qemu");
    if (!qemuUser) {
        qemuUser = getpwnam("qemu");
    }
    if (!qemuUser) {
        return QCoreApplication::translate("Karton", "could_not_find_the_libvirt_qemu_user");
    }
    const QString user = QString::fromUtf8(qemuUser->pw_name);

    QStringList failed;
    for (const QString &path : aclPaths) {
        if (!QFileInfo::exists(path)) {
            continue;
        }
        if (QProcess::execute(QStringLiteral("setfacl"), {QStringLiteral("-x"), QStringLiteral("u:%1").arg(user), path}) != 0) {
            failed.append(path);
        }
    }
    if (failed.isEmpty()) {
        settings.remove(QStringLiteral("storage/grantedAclPaths"));
    } else {
        settings.setValue(QStringLiteral("storage/grantedAclPaths"), failed);
    }
    settings.remove(QStringLiteral("storage/grantedDirectories"));
    if (!failed.isEmpty()) {
        return QCoreApplication::translate("Karton", "failed_to_remove_access_from_1").arg(failed.join(QStringLiteral(", ")));
    }
    return {};
}

QString Karton::checkDiskDirectoryAccess(const QString &path) const
{
    if (path.trimmed().isEmpty()) {
        return QCoreApplication::translate("Karton", "the_disk_folder_is_empty");
    }
    const QString directoryPath = QDir::cleanPath(QDir(path).absolutePath());
    if (isUnsafeImageDirectory(directoryPath)) {
        return QCoreApplication::translate("Karton", "choose_a_dedicated_folder_for_vm_images_not_a_system_home_or_sensitive_directory");
    }
    if (!QDir().mkpath(directoryPath)) {
        return QCoreApplication::translate("Karton", "failed_to_create_the_disk_folder_1").arg(directoryPath);
    }
    const QByteArray directoryBytes = directoryPath.toLocal8Bit();
    if (::access(directoryBytes.constData(), R_OK | W_OK | X_OK) != 0) {
        return QCoreApplication::translate("Karton", "karton_has_no_write_access_to_the_disk_folder_1").arg(directoryPath);
    }

    // With qemu:///session the guest process runs as the same user. For a
    // system connection libvirt's QEMU user needs to traverse every parent
    // directory and read/write the image folder. This function only checks
    // permissions; it never changes ownership, modes or ACLs.
    if (!m_activeUri.startsWith(QStringLiteral("qemu:///system"))) {
        return {};
    }
    struct passwd *qemuUser = getpwnam("libvirt-qemu");
    if (!qemuUser) {
        qemuUser = getpwnam("qemu");
    }
    if (!qemuUser) {
        return {};
    }

    gid_t groupBuffer[256];
    int groupCount = 256;
    if (getgrouplist(qemuUser->pw_name, qemuUser->pw_gid, groupBuffer, &groupCount) < 0) {
        groupCount = 1;
    }
    QVector<gid_t> groups;
    groups.reserve(groupCount);
    for (int i = 0; i < groupCount; ++i) {
        groups.append(groupBuffer[i]);
    }

    QString current = QStringLiteral("/");
    const QStringList parts = directoryPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        current = QDir(current).filePath(parts.at(i));
        const bool isTarget = (i == parts.size() - 1);
        const int required = isTarget ? (R_OK | W_OK | X_OK) : X_OK;
        if (!qemuCanAccess(current, qemuUser->pw_uid, groups, required)) {
            if (isTarget) {
                return QCoreApplication::translate("Karton", "libvirt_qemu_user_cannot_read_write_1_grant_that_user_access_to_the_folder").arg(current);
            }
            return QCoreApplication::translate("Karton", "libvirt_qemu_user_cannot_traverse_1_grant_that_user_execute_permission_on_the_directory")
                .arg(current);
        }
    }
    return {};
}

QString Karton::suggestedDiskPath(const QString &name, const QString &format) const
{
    const QString safeName = name.trimmed().isEmpty() ? QStringLiteral("vm") : name.trimmed();
    return imagesDirectory() + QLatin1Char('/') + safeName + QLatin1Char('.')
        + (format.trimmed().isEmpty() ? QStringLiteral("qcow2") : format.trimmed().toLower());
}

bool Karton::spiceTlsEnabled() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("spice/tls"), true).toBool();
}

void Karton::saveSpiceTlsEnabled(bool enabled)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("spice/tls"), enabled);
}

QString Karton::spiceCaCertificate() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("spice/caCertificate")).toString();
}

void Karton::saveSpiceCaCertificate(const QString &path)
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("spice/caCertificate"), path);
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

bool Karton::guestAgentSeen(const Domain *domain) const
{
    if (!domain) {
        return false;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = guestAgentPrefix(domain);
    if (settings.value(prefix + QStringLiteral("fingerprint")).toString() != guestAgentFingerprint(domain)) {
        return false;
    }
    return settings.value(prefix + QStringLiteral("seen"), false).toBool();
}

void Karton::markGuestAgentSeen(const Domain *domain)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = guestAgentPrefix(domain);
    prepareGuestAgentState(settings, prefix, guestAgentFingerprint(domain));
    if (!settings.value(prefix + QStringLiteral("seen"), false).toBool()) {
        settings.setValue(prefix + QStringLiteral("seen"), true);
    }
}

bool Karton::guestAgentWarningShown(const Domain *domain) const
{
    if (!domain) {
        return false;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = guestAgentPrefix(domain);
    if (settings.value(prefix + QStringLiteral("fingerprint")).toString() != guestAgentFingerprint(domain)) {
        return false;
    }
    return settings.value(prefix + QStringLiteral("warningShown"), false).toBool();
}

void Karton::markGuestAgentWarningShown(const Domain *domain)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = guestAgentPrefix(domain);
    prepareGuestAgentState(settings, prefix, guestAgentFingerprint(domain));
    if (!settings.value(prefix + QStringLiteral("warningShown"), false).toBool()) {
        settings.setValue(prefix + QStringLiteral("warningShown"), true);
    }
}

void Karton::clearGuestAgentState(const Domain *domain)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = guestAgentPrefix(domain);
    settings.remove(prefix + QStringLiteral("seen"));
    settings.remove(prefix + QStringLiteral("warningShown"));
}

QString Karton::language() const
{
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString language = settings.value(QStringLiteral("ui/language"), QStringLiteral("auto")).toString();
    if (language == QStringLiteral("auto")) {
        return language;
    }
    return language == QStringLiteral("ru") ? QStringLiteral("ru") : QStringLiteral("en");
}

void Karton::setLanguage(const QString &language)
{
    const QString value = language == QStringLiteral("auto") || language == QStringLiteral("ru") ? language : QStringLiteral("en");
    if (this->language() == value) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ui/language"), value);
    Q_EMIT languageChanged();
}

QString Karton::resolveLanguage(const QString &requested, const QString &systemLocale)
{
    if (requested == QStringLiteral("en") || requested == QStringLiteral("ru")) {
        return requested;
    }
    if (requested != QStringLiteral("auto")) {
        return QStringLiteral("en");
    }
    return systemLocale.left(2) == QStringLiteral("ru") ? QStringLiteral("ru") : QStringLiteral("en");
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

bool Karton::domainDisplayFullscreen(const Domain *domain) const
{
    if (!domain) {
        return false;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("display/%1/fullscreen").arg(domain->config()->uuid()), false).toBool();
}

void Karton::saveDomainDisplayFullscreen(const Domain *domain, bool fullscreen)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("display/%1/").arg(domain->config()->uuid());
    settings.setValue(prefix + QStringLiteral("fullscreen"), fullscreen);
    if (fullscreen) {
        settings.remove(prefix + QStringLiteral("width"));
        settings.remove(prefix + QStringLiteral("height"));
    }
    Q_EMIT displayResolutionChanged(domain);
}

void Karton::saveDomainDisplayResolution(const Domain *domain, int width, int height)
{
    if (!domain) {
        return;
    }
    QSettings settings(kartonConfigPath(), QSettings::IniFormat);
    const QString prefix = QStringLiteral("display/%1/").arg(domain->config()->uuid());
    const int oldWidth = settings.value(prefix + QStringLiteral("width"), 0).toInt();
    const int oldHeight = settings.value(prefix + QStringLiteral("height"), 0).toInt();
    if (width <= 0 || height <= 0) {
        if (oldWidth == 0 && oldHeight == 0) {
            return;
        }
        settings.remove(prefix + QStringLiteral("width"));
        settings.remove(prefix + QStringLiteral("height"));
    } else {
        if (oldWidth == width && oldHeight == height) {
            return;
        }
        settings.setValue(prefix + QStringLiteral("width"), width);
        settings.setValue(prefix + QStringLiteral("height"), height);
    }
    // An open viewer must pick the new mode up without being reopened.
    Q_EMIT displayResolutionChanged(domain);
}
