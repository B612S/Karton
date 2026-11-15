// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "os/osinfoconfig.h"

#include <algorithm>

#include <QCollator>
#include <QFileInfo>

#include "karton_debug.h"

OsinfoConfig::OsinfoConfig()
    : m_loader(osinfo_loader_new())
{
    if (!m_loader) {
        qCCritical(KARTON_DEBUG) << "failed to create osinfo loader";
        return;
    }

    g_autoptr(GError) error = nullptr;
    osinfo_loader_process_default_path(m_loader.get(), &error);
    if (error) {
        qCCritical(KARTON_DEBUG) << "failed to process default path:" << error->message;
        m_loader.reset();
        return;
    }

    // osinfo_loader_get_db() returns a borrowed pointer owned by the loader.
    m_db = osinfo_loader_get_db(m_loader.get());
    if (!m_db) {
        qCCritical(KARTON_DEBUG) << "OS database not initialized";
        return;
    }

    GObjectPtr<OsinfoOsList> list(osinfo_db_get_os_list(m_db));
    const gint len = osinfo_list_get_length(OSINFO_LIST(list.get()));

    m_osList.reserve(len);
    for (gint i = 0; i < len; i++) {
        OsinfoOs *os = OSINFO_OS(osinfo_list_get_nth(OSINFO_LIST(list.get()), i));
        const gchar *id = osinfo_product_get_short_id(OSINFO_PRODUCT(os));
        if (id) {
            m_osList.append(QString::fromUtf8(id));
        }
    }

    QCollator order;
    std::sort(m_osList.begin(), m_osList.end(), order);
}

OsinfoConfig::~OsinfoConfig() = default;

QString OsinfoConfig::getOsIdFromShortId(const QString &short_id)
{
    if (!m_db) {
        qCCritical(KARTON_DEBUG) << "OS database not initialized";
        return QString();
    }

    GObjectPtr<OsinfoOsList> osList(osinfo_db_get_os_list(m_db));
    const gint len = osinfo_list_get_length(OSINFO_LIST(osList.get()));

    for (gint i = 0; i < len; i++) {
        OsinfoOs *os = OSINFO_OS(osinfo_list_get_nth(OSINFO_LIST(osList.get()), i));
        const gchar *id = osinfo_product_get_short_id(OSINFO_PRODUCT(os));
        if (id && short_id == QString::fromUtf8(id)) {
            qCInfo(KARTON_DEBUG) << "Found OS by short id:" << id;
            return QString::fromUtf8(osinfo_entity_get_id(OSINFO_ENTITY(os)));
        }
    }

    qCWarning(KARTON_DEBUG) << "Could not find OS by short id:" << short_id;
    return QString();
}

QString OsinfoConfig::getShortIdFromId(const QString &id)
{
    if (!m_db || id.isEmpty()) {
        return QString();
    }

    const QByteArray idUtf8 = id.toUtf8();
    OsinfoOs *os = OSINFO_OS(osinfo_db_get_os(m_db, idUtf8.constData()));
    if (!os) {
        return QString();
    }

    const gchar *shortId = osinfo_product_get_short_id(OSINFO_PRODUCT(os));
    return shortId ? QString::fromUtf8(shortId) : QString();
}

QString OsinfoConfig::getOsIdFromDisk(const QString &isoDiskPath)
{
    if (!m_db) {
        qCCritical(KARTON_DEBUG) << "OS database not initialized";
        return QString();
    }

    QFileInfo fileInfo(isoDiskPath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        qCCritical(KARTON_DEBUG) << "file path does not exist:" << isoDiskPath;
        return QString();
    }

    // Keep the byte array alive for the whole call: qUtf8Printable() would
    // return a pointer into a temporary that is destroyed at the end of the
    // statement.
    const QByteArray location = fileInfo.absoluteFilePath().toUtf8();

    g_autoptr(GError) error = nullptr;
    GObjectPtr<OsinfoMedia> osMedia(osinfo_media_create_from_location(location.constData(), nullptr, &error));

    if (error) {
        qCCritical(KARTON_DEBUG) << "os_media creation error:" << error->message;
        return QString();
    }
    if (!osMedia) {
        qCWarning(KARTON_DEBUG) << "could not create media for file:" << isoDiskPath;
        return QString();
    }

    if (!osinfo_db_identify_media(m_db, osMedia.get())) {
        qCWarning(KARTON_DEBUG) << "could not identify media from disk:" << isoDiskPath;
        return QString();
    }

    GObjectPtr<OsinfoOs> os(osinfo_media_get_os(osMedia.get()));
    if (!os) {
        qCWarning(KARTON_DEBUG) << "could not get OS from media:" << isoDiskPath;
        return QString();
    }

    const QString id = QString::fromUtf8(osinfo_entity_get_id(OSINFO_ENTITY(os.get())));
    qCInfo(KARTON_DEBUG) << "OS id from disk image:" << id;
    return id;
}

QString OsinfoConfig::getOsArchitecture(const QString &osId)
{
    if (!m_db) {
        qCCritical(KARTON_DEBUG) << "OS database not initialized";
        return QString();
    }

    const QByteArray osIdUtf8 = osId.toUtf8();
    OsinfoOs *libosinfoOs = osinfo_db_get_os(m_db, osIdUtf8.constData());
    if (!libosinfoOs) {
        qCWarning(KARTON_DEBUG) << "could not find OS with ID:" << osId;
        return QString();
    }

    GObjectPtr<OsinfoImageList> images(osinfo_os_get_image_list(libosinfoOs));
    if (!images || osinfo_list_get_length(OSINFO_LIST(images.get())) == 0) {
        qCWarning(KARTON_DEBUG) << "OS has no image info, defaulting to x86_64:" << osId;
        return QString::fromUtf8("x86_64");
    }

    const gchar *osArch = osinfo_image_get_architecture(OSINFO_IMAGE(osinfo_list_get_nth(OSINFO_LIST(images.get()), 0)));
    if (!osArch) {
        qCWarning(KARTON_DEBUG) << "could not get architecture, default to x86_64.";
        return QString::fromUtf8("x86_64");
    }

    return QString::fromUtf8(osArch);
}
