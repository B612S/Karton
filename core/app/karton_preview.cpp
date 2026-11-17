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

bool Karton::refreshDomainPreview(const Domain *domain)
{
    // virsh screenshot briefly pauses QEMU. Never capture while a SPICE
    // viewer is open, even if a stale QML timer still asks for a preview.
    if (m_viewerCount > 0) {
        return false;
    }
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
