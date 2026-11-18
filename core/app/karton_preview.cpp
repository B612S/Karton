// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

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

    // A text-only VM (serial console) has no framebuffer, so virsh would fail
    // with "no screens to take screenshot from" on every poll. Do not even
    // start the command for such domains, and drop a preview left over from an
    // earlier configuration so the VM page cannot show a stale frame.
    const QString uuid = domain->config()->uuid();
    if (!domain->config()->hasGraphics() || m_previewUnsupportedDomains.contains(uuid)) {
        if (!domain->previewFrame().isNull()) {
            const_cast<Domain *>(domain)->clearPreviewFrame();
        }
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
                                    if (!guard) {
                                        return;
                                    }
                                    guard->m_previewCapturing = false;
                                    if (!success) {
                                        QFile::remove(tempPath);
                                        // QEMU can still report no screen when the
                                        // XML does contain a video device. Remember
                                        // that so the error is not repeated every
                                        // second; refreshDomainList() clears it.
                                        if (output.contains(QStringLiteral("no screens"), Qt::CaseInsensitive)) {
                                            if (domainGuard) {
                                                guard->m_previewUnsupportedDomains.insert(domainGuard->config()->uuid());
                                                domainGuard->clearPreviewFrame();
                                            }
                                        }
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
