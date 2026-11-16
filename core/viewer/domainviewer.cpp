// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "viewer/domainviewer.h"

#include "viewer/clipboardmanager.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <spice-client.h>
#include <spice/vd_agent.h>

#include <cstdlib>

#include <KLocalizedString>

#include <QCursor>
#include <QDomDocument>
#include <QDomElement>
#include <QGuiApplication>
#include <QMutexLocker>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QString>
#include <QTimer>

#include "domain/domain.h"
#include "glib.h"
#include "karton_debug.h"
#include "viewer/glscanoutrenderer.h"
#include "viewer/primarysurfacerenderer.h"
#include "viewer/vulkanscanoutrenderer.h"

#include "util/evdev_to_xtkbd_map.h"

DomainViewer::DomainViewer(QQuickItem *parent)
    : QQuickItem(parent)
    , m_domain(nullptr)
    , m_spiceUri(QString())
    , m_connected(false)
    , m_session(nullptr)
    , m_inputs_channel(nullptr)
    , m_current_button_mask(0)
    , m_audioPipeWire(nullptr)
{
    setFlag(ItemHasContents, true);
    m_clipboard = new ClipboardManager(this);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFlag(ItemIsFocusScope, true);
}

DomainViewer::~DomainViewer()
{
    disconnectFromSpice();
}

void DomainViewer::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    Q_EMIT errorMessageChanged();
}

void DomainViewer::setDomain(Domain *domain)
{
    if (m_domain == domain) {
        return;
    }
    if (m_domain) {
        disconnectFromSpice();
    }

    m_domain = domain;
    Q_EMIT domainChanged();

    if (isComponentComplete() && m_domain) {
        if (m_domain) {
            setupSpiceSession();
        } else {
            qCDebug(KARTON_DEBUG) << "setDomain(): null domain assigned";
        }
    }
}

void DomainViewer::componentComplete()
{
    QQuickItem::componentComplete();
    if (m_domain) {
        setupSpiceSession();
    }
}

bool DomainViewer::setupSpiceSession()
{
    setErrorMessage(QString());

    if (!m_domain || !m_domain->domainPtr()) {
        qCWarning(KARTON_DEBUG) << "setupSpiceSession(): no valid domain";
        setErrorMessage(i18n("No virtual machine is selected."));
        return false;
    }

    // The live XML contains the actual SPICE port for autoport domains and the
    // socket path for socket-listening domains.
    char *xmlDesc = virDomainGetXMLDesc(m_domain->domainPtr(), 0);
    if (!xmlDesc) {
        xmlDesc = virDomainGetXMLDesc(m_domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    }
    if (!xmlDesc) {
        qCWarning(KARTON_DEBUG) << "Failed to read domain XML:" << virGetLastErrorMessage();
        setErrorMessage(i18n("Failed to read the virtual machine configuration."));
        return false;
    }

    const QByteArray xml(xmlDesc);
    free(xmlDesc);

    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(xml))) {
        qCWarning(KARTON_DEBUG) << "Failed to parse domain XML";
        setErrorMessage(i18n("Failed to parse the virtual machine configuration."));
        return false;
    }

    QString uri;
    bool glEnabled = false;
    m_spiceTlsPort = 0;
    m_spicePassword.clear();

    const QDomNodeList graphicsNodes = document.elementsByTagName(QStringLiteral("graphics"));
    for (int i = 0; i < graphicsNodes.count(); ++i) {
        const QDomElement graphics = graphicsNodes.at(i).toElement();
        if (graphics.attribute(QStringLiteral("type")) != QStringLiteral("spice")) {
            continue;
        }

        const QDomElement gl = graphics.firstChildElement(QStringLiteral("gl"));
        glEnabled = gl.attribute(QStringLiteral("enable")) == QStringLiteral("yes");

        const QDomElement listen = graphics.firstChildElement(QStringLiteral("listen"));
        QString listenType = listen.attribute(QStringLiteral("type"));
        if (listenType.isEmpty()) {
            listenType = graphics.attribute(QStringLiteral("listen"));
        }

        if (listenType == QStringLiteral("socket")) {
            QString socketPath = listen.attribute(QStringLiteral("socket"));
            if (socketPath.isEmpty()) {
                socketPath = graphics.attribute(QStringLiteral("socket"));
            }
            if (!socketPath.isEmpty()) {
                uri = QStringLiteral("spice+unix://%1").arg(socketPath);
            }
        } else if (listenType != QStringLiteral("none")) {
            QString host = listen.attribute(QStringLiteral("address"));
            if (host.isEmpty()) {
                host = graphics.attribute(QStringLiteral("listen"));
            }
            if (host.isEmpty() || host == QStringLiteral("0.0.0.0") || host == QStringLiteral("::")) {
                host = QStringLiteral("127.0.0.1");
            }
            const QString port = graphics.attribute(QStringLiteral("port"));
            const QString tlsPort = graphics.attribute(QStringLiteral("tlsPort"));
            const bool useTls = m_spiceTlsEnabled && !tlsPort.isEmpty() && tlsPort != QStringLiteral("-1");
            if (useTls) {
                m_spiceTlsPort = tlsPort.toInt();
                m_spicePassword = graphics.attribute(QStringLiteral("passwd"));
                uri = QStringLiteral("spice://%1?tls_port=%2").arg(host, tlsPort);
            } else if (!port.isEmpty() && port != QStringLiteral("-1")) {
                uri = QStringLiteral("spice://%1:%2").arg(host, port);
            }
        }

        if (!uri.isEmpty()) {
            break;
        }
    }

    if (uri.isEmpty()) {
        qCWarning(KARTON_DEBUG) << "No usable SPICE graphics device found for domain:" << m_domain->config()->name();
        setErrorMessage(i18n("This virtual machine has no usable SPICE display."));
        return false;
    }

    m_spiceUri = uri;

    const bool vulkan = QQuickWindow::graphicsApi() == QSGRendererInterface::Vulkan;
    if (vulkan) {
        m_renderMode = glEnabled ? RenderMode::VulkanScanout : RenderMode::PrimarySurface;
        qCInfo(KARTON_DEBUG) << "Using Vulkan;" << (glEnabled ? "scanout" : "primary surface") << "rendering selected";
    } else if (uri.startsWith(QStringLiteral("spice+unix://"))) {
        qCInfo(KARTON_DEBUG) << "Detected UNIX socket. Using GL scanout rendering.";
        m_renderMode = RenderMode::GlScanout;
    } else {
        qCInfo(KARTON_DEBUG) << "Detected network TCP socket. Using primary surface rendering.";
        m_renderMode = RenderMode::PrimarySurface;
    }

    if (!connectToSpice()) {
        setErrorMessage(i18n("Could not connect to the SPICE display."));
        return false;
    }
    return true;
}

bool DomainViewer::connectToSpice()
{
    qCInfo(KARTON_DEBUG) << "Connecting to SPICE...";
    qCInfo(KARTON_DEBUG) << "   SPICE URI:" << m_spiceUri;

    if (!m_domain) {
        qCCritical(KARTON_DEBUG) << "connectToSpice() called but domain is null!";
        return false;
    }

    disconnectFromSpice();
    m_session = spice_session_new();
    if (m_clipboard) {
        m_clipboard->prepare(m_session);
    }

    qCInfo(KARTON_DEBUG) << "Connecting to URI! -" << m_spiceUri;
    g_object_set(m_session, "uri", m_spiceUri.toUtf8().constData(), NULL);
    if (m_spiceTlsPort > 0) {
        g_object_set(m_session, "tls-port", m_spiceTlsPort, NULL);
    }
    if (!m_spiceCaCertificate.isEmpty()) {
        g_object_set(m_session, "ca-file", m_spiceCaCertificate.toUtf8().constData(), NULL);
    }
    if (!m_spicePassword.isEmpty()) {
        g_object_set(m_session, "password", m_spicePassword.toUtf8().constData(), NULL);
    }

    g_signal_connect(m_session, "channel-new", G_CALLBACK(DomainViewer::channel_new_callback), this);

    if (!spice_session_connect(m_session)) {
        g_object_unref(m_session);
        m_session = nullptr;
        m_connected = false;
        if (m_agentConnected) {
            m_agentConnected = false;
            Q_EMIT agentConnectedChanged();
        }
        return false;
    }
    qCInfo(KARTON_DEBUG) << "Established connection! connected to " << domain()->config()->name();
    m_connected = true;

    return true;
}

void DomainViewer::disconnectFromSpice()
{
    stopAudio();

    if (m_renderer) {
        m_renderer->detach();
        m_renderer.reset();
    }

    clearCursor();
    update();

    if (m_session) {
        spice_session_disconnect(m_session);
        g_object_unref(m_session);
        m_session = nullptr;
    }

    stopRecording();
    if (m_clipboard) {
        m_clipboard->detach();
    }
    m_main_channel = nullptr;
    m_inputs_channel = nullptr;
    m_recordChannel = nullptr;
    m_connected = false;
    m_lastDisplayResolution = QSize();
    releasePointerGrab();
}

void DomainViewer::channel_new_callback(SpiceSession *session, SpiceChannel *channel, gpointer user_data)
{
    Q_UNUSED(session);

    DomainViewer *item = static_cast<DomainViewer *>(user_data);

    item->checkChannelStatus();
    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE display connected";
        item->attachDisplayChannel(channel);
    } else if (SPICE_IS_MAIN_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE main channel connected";
        spice_channel_connect(channel);
        item->m_main_channel = SPICE_MAIN_CHANNEL(channel);
        g_signal_connect(channel, "main-agent-update", G_CALLBACK(mainAgentUpdateCallback), item);
        item->m_clipboard->attach(item->m_session, SPICE_MAIN_CHANNEL(channel));
        QTimer::singleShot(1500, item, &DomainViewer::updateDisplayResolution);
    } else if (SPICE_IS_INPUTS_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE: Inputs connected";
        spice_channel_connect(channel);
        item->m_inputs_channel = SPICE_INPUTS_CHANNEL(channel);
    } else if (SPICE_IS_PLAYBACK_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE: Audio playback connected";
        spice_channel_connect(channel);

        g_signal_connect(channel, "playback-start", G_CALLBACK(playback_start_callback), item);
        g_signal_connect(channel, "playback-data", G_CALLBACK(playback_data_callback), item);
        g_signal_connect(channel, "playback-stop", G_CALLBACK(playback_stop_callback), item);
    } else if (SPICE_IS_RECORD_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE: Audio record connected";
        spice_channel_connect(channel);
        item->m_recordChannel = SPICE_RECORD_CHANNEL(channel);
        g_signal_connect(channel, "record-start", G_CALLBACK(record_start_callback), item);
        g_signal_connect(channel, "record-stop", G_CALLBACK(record_stop_callback), item);
    } else if (SPICE_IS_CURSOR_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE: Cursor connected";
        item->attachCursorChannel(channel);
    } else {
        gint type = 0;
        g_object_get(channel, "channel-type", &type, NULL);
        qCDebug(KARTON_DEBUG) << "Unhandled SPICE channel type:" << QString::fromUtf8(spice_channel_type_to_string(type));
    }
}

void DomainViewer::mainAgentUpdateCallback(SpiceMainChannel *channel, gpointer userData)
{
    Q_UNUSED(channel);
    auto *item = static_cast<DomainViewer *>(userData);
    // The agent can connect after the initial resize request. Force a new
    // request even if the target size did not change in the meantime.
    item->m_lastDisplayResolution = QSize();
    item->updateDisplayResolution();
}

void DomainViewer::syncHostClipboard()
{
    if (m_clipboard) {
        m_clipboard->syncHostClipboard();
    }
}

// ========================== Display rendering  ========================

void DomainViewer::attachDisplayChannel(SpiceChannel *channel)
{
    spice_channel_connect(channel);

    switch (m_renderMode) {
    case RenderMode::GlScanout:
        m_renderer = std::make_unique<GlScanoutRenderer>();
        break;
    case RenderMode::VulkanScanout:
        m_renderer = std::make_unique<VulkanScanoutRenderer>();
        break;
    case RenderMode::PrimarySurface:
        m_renderer = std::make_unique<PrimarySurfaceRenderer>();
        break;
    }

    connect(m_renderer.get(), &SpiceDisplayRenderer::frameReady, this, &QQuickItem::update);
    connect(m_renderer.get(), &SpiceDisplayRenderer::frameReady, this, &DomainViewer::updateLivePreview);
    connect(m_renderer.get(), &SpiceDisplayRenderer::frameSizeChanged, this, &DomainViewer::handleRendererFrameSizeChanged);
    connect(m_renderer.get(), &SpiceDisplayRenderer::errorOccurred, this, [this](const QString &message) {
        setErrorMessage(message);
    });

    m_renderer->attach(SPICE_DISPLAY_CHANNEL(channel));
}

void DomainViewer::attachCursorChannel(SpiceChannel *channel)
{
    spice_channel_connect(channel);
    g_signal_connect(channel, "cursor-set", G_CALLBACK(cursorSetCallback), this);
    g_signal_connect(channel, "cursor-move", G_CALLBACK(cursorMoveCallback), this);
    g_signal_connect(channel, "cursor-hide", G_CALLBACK(cursorHideCallback), this);
    g_signal_connect(channel, "cursor-reset", G_CALLBACK(cursorResetCallback), this);
}

void DomainViewer::cursorSetCallback(SpiceCursorChannel *channel, gint width, gint height, gint hotX, gint hotY, gpointer rgba, gpointer userData)
{
    Q_UNUSED(channel);

    auto *item = static_cast<DomainViewer *>(userData);
    if (!rgba || width <= 0 || height <= 0) {
        item->clearCursor();
        return;
    }

    qCDebug(KARTON_DEBUG) << "SPICE cursor-set:" << width << "x" << height << "hotspot" << hotX << hotY;
    const QImage image(static_cast<const uchar *>(rgba), width, height, QImage::Format_RGBA8888);
    {
        QMutexLocker locker(&item->m_cursorMutex);
        item->m_cursorImage = image.copy();
        item->m_cursorHotspot = QPoint(hotX, hotY);
        item->m_cursorVisible = true;
        item->m_cursorTextureDirty = true;
    }
    item->setCursor(QCursor(Qt::BlankCursor));
    item->update();
}

void DomainViewer::cursorMoveCallback(SpiceCursorChannel *channel, gint x, gint y, gpointer userData)
{
    Q_UNUSED(channel);

    auto *item = static_cast<DomainViewer *>(userData);
    qCDebug(KARTON_DEBUG) << "SPICE cursor-move:" << x << y;
    {
        QMutexLocker locker(&item->m_cursorMutex);
        item->m_cursorPosition = QPoint(x, y);
    }
    item->update();
}

void DomainViewer::cursorHideCallback(SpiceCursorChannel *channel, gpointer userData)
{
    Q_UNUSED(channel);

    auto *item = static_cast<DomainViewer *>(userData);
    qCDebug(KARTON_DEBUG) << "SPICE cursor-hide";
    bool pointerInside = false;
    bool haveCursor = false;
    {
        QMutexLocker locker(&item->m_cursorMutex);
        pointerInside = item->m_pointerInside;
        haveCursor = item->m_cursorVisible && !item->m_cursorImage.isNull();
        if (!pointerInside) {
            item->m_cursorVisible = false;
        }
    }
    if (pointerInside && haveCursor) {
        // Keep the last guest cursor while the pointer is over the display;
        // otherwise the host arrow flashes in during guest window dragging.
        item->setCursor(Qt::BlankCursor);
    } else if (!item->m_pointerGrabbed) {
        // No guest cursor is available (for example after clearing it while
        // entering fullscreen). Do not leave the host cursor blank.
        item->unsetCursor();
    }
    item->update();
}

void DomainViewer::cursorResetCallback(SpiceCursorChannel *channel, gpointer userData)
{
    Q_UNUSED(channel);

    qCDebug(KARTON_DEBUG) << "SPICE cursor-reset";
    static_cast<DomainViewer *>(userData)->clearCursor();
}

void DomainViewer::clearCursor()
{
    {
        QMutexLocker locker(&m_cursorMutex);
        m_cursorImage = QImage();
        m_cursorVisible = false;
        m_cursorTextureDirty = true;
        m_hasCursorPaintPosition = false;
    }
    if (!m_pointerGrabbed) {
        unsetCursor();
    }
    update();
}

void DomainViewer::updateLivePreview()
{
    if (!m_domain || !m_renderer || (m_livePreviewTimer.isValid() && m_livePreviewTimer.elapsed() < 200)) {
        return;
    }
    m_livePreviewTimer.restart();
    const QImage frame = m_renderer->currentFrame();
    if (!frame.isNull()) {
        m_domain->updateLivePreviewFrame(frame);
    }
}

void DomainViewer::handleRendererFrameSizeChanged()
{
    if (!m_renderer) {
        return;
    }
    const QSize size = m_renderer->frameSize();
    setImplicitWidth(size.width());
    setImplicitHeight(size.height());
}

void DomainViewer::setFixedDisplayResolution(int width, int height)
{
    m_fixedDisplayWidth = width;
    m_fixedDisplayHeight = height;
    m_lastDisplayResolution = QSize();
    updateDisplayResolution();
}

void DomainViewer::updateDisplayResolution()
{
    if (!m_main_channel || !m_connected || width() < 320 || height() < 240) {
        return;
    }

    // The SPICE agent resizes the guest desktop to the monitor configuration.
    // update=TRUE lets spice-gtk debounce resize storms while the user drags
    // the window, and send the final configuration to the agent.
    // Use the parent page size, not the aspect-fitted display item: the item
    // shrinks/grows when the guest resolution changes, and using it here made
    // the auto-fit request oscillate between two resolutions while Windows
    // was booting.
    QQuickItem *container = parentItem();
    const qreal logicalWidth = container ? container->width() : width();
    const qreal logicalHeight = container ? container->height() : height();
    const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
    const qreal availableWidth = logicalWidth * dpr;
    const qreal availableHeight = logicalHeight * dpr;

    // Fit the monitor rectangle to the current guest aspect ratio instead of
    // stretching it to the page size. A request that is wider than the guest's
    // supported mode (for example 2160x1178 on a 1920x1200 guest) is rejected
    // by spice-vdagent and looked like auto-resize did nothing.
    // Ask the guest for the complete viewer area. If we instead preserve the
    // old guest aspect ratio here, the aspect-fitted DomainViewer item keeps
    // leaving black bars on the sides even after the agent applies the resize.
    int targetWidth = qMax(640, qRound(availableWidth));
    int targetHeight = qMax(480, qRound(availableHeight));
    if (m_fixedDisplayWidth >= 320 && m_fixedDisplayHeight >= 240) {
        targetWidth = m_fixedDisplayWidth;
        targetHeight = m_fixedDisplayHeight;
    }
    targetWidth &= ~1;
    targetHeight &= ~1;

    const QSize target(targetWidth, targetHeight);
    if (target == m_lastDisplayResolution) {
        return;
    }
    m_lastDisplayResolution = target;
    // Some agents only react when the display is explicitly enabled and the
    // monitor configuration is sent immediately; update=TRUE alone can leave
    // the request waiting for the next agent event.
    spice_main_channel_update_display_enabled(m_main_channel, 0, TRUE, TRUE);
    spice_main_channel_update_display(m_main_channel, 0, 0, 0, targetWidth, targetHeight, TRUE);
    gboolean agentConnected = FALSE;
    g_object_get(m_main_channel, "agent-connected", &agentConnected, nullptr);
    if (agentConnected) {
        spice_main_channel_send_monitor_config(m_main_channel);
    }
    if (m_agentConnected != agentConnected) {
        m_agentConnected = agentConnected;
        Q_EMIT agentConnectedChanged();
    }
    qCInfo(KARTON_DEBUG) << "Requested guest display resolution:" << target << "agent:" << agentConnected;
}

void DomainViewer::fitGuestToWindow()
{
    m_lastDisplayResolution = QSize();
    updateDisplayResolution();
}

// triggered by update()
QSGNode *DomainViewer::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    QSGNode *root = oldNode;
    if (!root) {
        // The scene graph owns the old node tree and may delete it when the
        // window is hidden, resized or the graphics context is reset. The
        // member pointers must not be reused after that, otherwise the next
        // update appends a dangling node and crashes in QSGBatchRenderer.
        m_displayNode = nullptr;
        m_cursorNode = nullptr;
        root = new QSGNode;
    }

    if (m_renderer) {
        QSGNode *display = m_renderer->updatePaintNode(window(), m_displayNode, boundingRect());
        if (display != m_displayNode) {
            m_displayNode = display;
            if (display) {
                root->appendChildNode(display);
            }
        } else if (display && display->parent() != root) {
            root->appendChildNode(display);
        }
    } else if (m_displayNode) {
        if (m_displayNode->parent() == root) {
            root->removeChildNode(m_displayNode);
        }
        delete m_displayNode;
        m_displayNode = nullptr;
    }

    QImage cursorImage;
    QPoint cursorPosition;
    QPoint hotspot;
    bool cursorVisible = false;
    bool textureDirty = false;
    QPointF cursorPaintPosition;
    bool hasCursorPaintPosition = false;
    {
        QMutexLocker locker(&m_cursorMutex);
        cursorImage = m_cursorImage;
        cursorPosition = m_cursorPosition;
        hotspot = m_cursorHotspot;
        cursorVisible = m_cursorVisible;
        textureDirty = m_cursorTextureDirty;
        m_cursorTextureDirty = false;
        cursorPaintPosition = m_cursorPaintPosition;
        hasCursorPaintPosition = m_hasCursorPaintPosition;
    }

    if (!cursorVisible || cursorImage.isNull()) {
        if (m_cursorNode) {
            root->removeChildNode(m_cursorNode);
            delete m_cursorNode;
            m_cursorNode = nullptr;
        }
        return root;
    }

    if (!m_cursorNode) {
        m_cursorNode = new QSGSimpleTextureNode;
        m_cursorNode->setOwnsTexture(true);
        root->appendChildNode(m_cursorNode);
    } else {
        // Keep the cursor above the display node.
        root->removeChildNode(m_cursorNode);
        root->appendChildNode(m_cursorNode);
    }

    if (textureDirty || !m_cursorNode->texture()) {
        QSGTexture *texture = window()->createTextureFromImage(cursorImage);
        if (!texture) {
            root->removeChildNode(m_cursorNode);
            delete m_cursorNode;
            m_cursorNode = nullptr;
            return root;
        }
        m_cursorNode->setTexture(texture);
    }

    const QSize frameSize = m_renderer ? m_renderer->frameSize() : QSize();
    const qreal scaleX = (frameSize.width() > 0 && width() > 0) ? width() / frameSize.width() : 1.0;
    const qreal scaleY = (frameSize.height() > 0 && height() > 0) ? height() / frameSize.height() : 1.0;

    // SPICE only sends cursor-move in server mouse mode. In client mode there
    // is no cursor-move at all, so fall back to the local pointer position (or
    // the center of the frame) instead of leaving the cursor in the top-left
    // corner.
    if (hasCursorPaintPosition) {
        cursorPosition = cursorPaintPosition.toPoint();
    } else if (cursorPosition.isNull() && frameSize.isValid()) {
        cursorPosition = QPoint(frameSize.width() / 2, frameSize.height() / 2);
    }

    const qreal x = (cursorPosition.x() - hotspot.x()) * scaleX;
    const qreal y = (cursorPosition.y() - hotspot.y()) * scaleY;
    m_cursorNode->setRect(QRectF(x, y, cursorImage.width() * scaleX, cursorImage.height() * scaleY));
    m_cursorNode->setFiltering(QSGTexture::Linear);

    return root;
}

void DomainViewer::updateDomainPreview()
{
    if (!m_domain || !window() || !isVisible() || width() <= 0 || height() <= 0) {
        return;
    }

    // Primary-surface mode already keeps an owned CPU image. Use it directly:
    // no GPU readback, no blocking and no colour conversion.
    if (m_renderer) {
        const QImage frame = m_renderer->currentFrame();
        if (!frame.isNull()) {
            m_domain->savePreviewFrame(frame);
            return;
        }
    }

    if (m_previewGrabPending) {
        return;
    }

    // GPU scanout fallback. Asynchronous item grab only reads the display
    // item, not the whole window, and does not block the GUI.
    const int targetWidth = 480;
    const QSize targetSize(targetWidth, qMax(1, qRound(targetWidth * height() / width())));
    const QSharedPointer<QQuickItemGrabResult> result = grabToImage(targetSize);
    if (!result) {
        return;
    }
    m_previewGrabPending = true;
    connect(result.data(), &QQuickItemGrabResult::ready, this, [this, result]() {
        m_previewGrabPending = false;
        const QImage image = result->image();
        if (m_domain && !image.isNull()) {
            qCInfo(KARTON_DEBUG) << "preview grab:" << image.size() << image.format() << "item:" << size()
                                 << "frame:" << (m_renderer ? m_renderer->frameSize() : QSize());
            image.save(QStringLiteral("/tmp/karton-grab.png"));
            m_domain->savePreviewFrame(image);
        }
    });
}

void DomainViewer::saveFrameToDomain()
{
    qCDebug(KARTON_DEBUG) << "saveFrameToDomain called";

    if (!window()) {
        qCDebug(KARTON_DEBUG) << "saveFrameToDomain: No window available";
        return;
    }

    // Do not require isVisible() here: the window is already hidden when
    // onClosing runs, but the scene graph can still be grabbed.
    if (width() <= 0 || height() <= 0) {
        qCDebug(KARTON_DEBUG) << "saveFrameToDomain: Item not ready for grabbing";
        return;
    }

    // returns the image of the full window
    QImage fullImage = window()->grabWindow();

    if (!fullImage.isNull()) {
        // grabWindow() returns device pixels, while mapRectToScene() works in
        // logical coordinates.
        const qreal dpr = window()->devicePixelRatio();
        const QRectF itemBounds = mapRectToScene(boundingRect());
        const QRect cropRect =
            QRectF(itemBounds.x() * dpr, itemBounds.y() * dpr, itemBounds.width() * dpr, itemBounds.height() * dpr).toRect().intersected(fullImage.rect());
        const QImage finalImage = cropRect.isEmpty() ? fullImage : fullImage.copy(cropRect);

        qCDebug(KARTON_DEBUG) << "Sending QImage frame to save... - " << finalImage.size() << ", " << m_domain->config()->name();
        m_domain->savePreviewFrame(finalImage.convertToFormat(QImage::Format_RGB32));
    } else {
        qCDebug(KARTON_DEBUG) << "saveFrameToDomain: grabWindow returned null image";
    }
}
// ========================== Audio callbacks =====================

void DomainViewer::playback_start_callback(SpicePlaybackChannel *channel, gint format, gint channels, gint rate, gpointer user_data)
{
    Q_UNUSED(channel);

    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    qCInfo(KARTON_DEBUG) << "Audio playback starting - Format:" << format << "Channels:" << channels << "Rate:" << rate;

    item->stopAudio();
    if (!item->m_audioPlaybackEnabled) {
        return;
    }

    item->m_audioPipeWire = new PipeWireAudioSink(item);
    if (!item->m_audioPipeWire->start(rate, channels)) {
        qCWarning(KARTON_DEBUG) << "Failed to start PipeWire audio output";
        item->stopAudio();
        return;
    }

    qCInfo(KARTON_DEBUG) << "Audio output started through PipeWire";
}

void DomainViewer::playback_data_callback(SpicePlaybackChannel *channel, gpointer data, gint size, gpointer user_data)
{
    Q_UNUSED(channel);

    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    if (item->m_audioPipeWire) {
        item->m_audioPipeWire->write(static_cast<const char *>(data), size);
    }
}

void DomainViewer::playback_stop_callback(SpicePlaybackChannel *channel, gpointer user_data)
{
    Q_UNUSED(channel);

    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    qCInfo(KARTON_DEBUG) << "Audio playback stopping";
    item->stopAudio();
}

void DomainViewer::stopAudio()
{
    if (m_audioPipeWire) {
        m_audioPipeWire->stop();
        delete m_audioPipeWire;
        m_audioPipeWire = nullptr;
    }
}

void DomainViewer::setAudioPlaybackEnabled(bool enabled)
{
    if (m_audioPlaybackEnabled == enabled) {
        return;
    }
    m_audioPlaybackEnabled = enabled;
    if (!enabled) {
        stopAudio();
    }
    Q_EMIT audioPlaybackEnabledChanged();
}

void DomainViewer::setMicrophoneEnabled(bool enabled)
{
    if (m_microphoneEnabled == enabled) {
        return;
    }
    m_microphoneEnabled = enabled;
    if (enabled) {
        if (m_recording) {
            startRecording();
        }
    } else {
        stopRecording();
    }
    Q_EMIT microphoneEnabledChanged();
}

void DomainViewer::setSpiceTlsEnabled(bool enabled)
{
    if (m_spiceTlsEnabled == enabled) {
        return;
    }
    m_spiceTlsEnabled = enabled;
    Q_EMIT spiceTlsEnabledChanged();
}

void DomainViewer::setSpiceCaCertificate(const QString &path)
{
    if (m_spiceCaCertificate == path) {
        return;
    }
    m_spiceCaCertificate = path;
    Q_EMIT spiceCaCertificateChanged();
}

void DomainViewer::setClipboardTextMode(int mode)
{
    mode = qBound(0, mode, 3);
    if (m_clipboardTextMode == mode) {
        return;
    }
    m_clipboardTextMode = mode;
    if (m_clipboard) {
        m_clipboard->setTextMode(mode);
    }
    Q_EMIT clipboardTextModeChanged();
}

void DomainViewer::setClipboardFilesMode(int mode)
{
    mode = qBound(0, mode, 3);
    if (m_clipboardFilesMode == mode) {
        return;
    }
    m_clipboardFilesMode = mode;
    if (m_clipboard) {
        m_clipboard->setFilesMode(mode);
    }
    Q_EMIT clipboardFilesModeChanged();
}

void DomainViewer::record_start_callback(SpiceRecordChannel *channel, gint format, gint channels, gint rate, gpointer user_data)
{
    Q_UNUSED(channel);
    Q_UNUSED(format);
    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    item->m_recording = true;
    item->m_recordFormat.setSampleRate(rate);
    item->m_recordFormat.setChannelCount(channels);
    item->m_recordFormat.setSampleFormat(QAudioFormat::Int16);
    if (item->m_microphoneEnabled) {
        item->startRecording();
    }
}

void DomainViewer::record_stop_callback(SpiceRecordChannel *channel, gpointer user_data)
{
    Q_UNUSED(channel);
    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    item->m_recording = false;
    item->stopRecording();
}

void DomainViewer::startRecording()
{
    stopRecording();
    if (!m_recordChannel || !m_microphoneEnabled) {
        return;
    }
    m_audioSource = new QAudioSource(m_recordFormat, this);
    m_recordDevice = m_audioSource->start();
    if (!m_recordDevice) {
        delete m_audioSource;
        m_audioSource = nullptr;
        return;
    }
    connect(m_recordDevice, &QIODevice::readyRead, this, &DomainViewer::readRecordingData);
}

void DomainViewer::stopRecording()
{
    if (m_audioSource) {
        m_audioSource->stop();
        delete m_audioSource;
        m_audioSource = nullptr;
    }
    m_recordDevice = nullptr;
}

void DomainViewer::readRecordingData()
{
    if (!m_recordDevice || !m_recordChannel || !m_microphoneEnabled) {
        return;
    }
    const QByteArray data = m_recordDevice->readAll();
    if (!data.isEmpty()) {
        spice_record_channel_send_data(m_recordChannel, const_cast<char *>(data.constData()), data.size(), static_cast<guint32>(g_get_monotonic_time() / 1000));
    }
}

// ========================== Input handling =============================

// maps qt provided scancode to pcxt
uint8_t DomainViewer::evdevToPcXt(uint32_t evdev_scancode)
{
    if (evdev_scancode >= CODE_MAP_LINUX_TO_XTKBD_LEN) {
        qCWarning(KARTON_DEBUG) << "Scancode out of range:" << evdev_scancode;
        return 0;
    }

    uint8_t result = code_map_linux_to_xtkbd[evdev_scancode] & 0xFF;
    if (result == 0) {
        qCWarning(KARTON_DEBUG) << "Unknown evdev scancode:" << evdev_scancode;
        return 0;
    }

    qCDebug(KARTON_DEBUG) << "Mapped evdev" << evdev_scancode << "to PC XT" << QString::number(result, 16);
    return result;
}

void DomainViewer::grabPointer()
{
    if (m_pointerGrabbed || !window()) {
        return;
    }

    // Set the guard before calling into the window system: grabbing can
    // re-enter the event handlers on some platforms.
    m_pointerGrabbed = true;
    if (!window()->setMouseGrabEnabled(true)) {
        m_pointerGrabbed = false;
        qCWarning(KARTON_DEBUG) << "The windowing system does not support mouse grab";
        return;
    }

    if (!window()->setKeyboardGrabEnabled(true)) {
        qCWarning(KARTON_DEBUG) << "The windowing system does not support keyboard grab";
    }
    qCInfo(KARTON_DEBUG) << "Pointer grabbed; SPICE mouse mode:" << mouseMode();
    m_hasLastPointerPosition = false;

    if (!m_hasVirtualPointerPosition) {
        if (m_hasLocalPointerPosition) {
            m_virtualPointerPosition = guestPositionForLocal(m_localPointerPosition);
        } else if (m_renderer && m_renderer->frameSize().isValid()) {
            const QSize frameSize = m_renderer->frameSize();
            m_virtualPointerPosition = QPointF(frameSize.width() / 2.0, frameSize.height() / 2.0);
        }
        m_hasVirtualPointerPosition = true;
    }
    updateCursorPaintPosition();

    setCursor(Qt::BlankCursor);
    forceActiveFocus();
    Q_EMIT pointerGrabbedChanged();
}

void DomainViewer::releasePointerGrab()
{
    if (!m_pointerGrabbed) {
        return;
    }

    m_pointerGrabbed = false;
    m_hasLastPointerPosition = false;
    qCInfo(KARTON_DEBUG) << "Pointer released";
    if (window()) {
        window()->setMouseGrabEnabled(false);
        window()->setKeyboardGrabEnabled(false);
    }
    bool keepBlank = false;
    {
        QMutexLocker locker(&m_cursorMutex);
        keepBlank = m_pointerInside && m_cursorVisible;
    }
    if (keepBlank) {
        setCursor(Qt::BlankCursor);
    } else {
        unsetCursor();
    }
    Q_EMIT pointerGrabbedChanged();
}

int DomainViewer::mouseMode() const
{
    if (!m_main_channel) {
        return SPICE_MOUSE_MODE_CLIENT;
    }

    int mode = SPICE_MOUSE_MODE_CLIENT;
    g_object_get(m_main_channel, "mouse-mode", &mode, nullptr);
    return mode;
}

QPointF DomainViewer::guestPositionForLocal(const QPointF &position) const
{
    const QSize frameSize = m_renderer ? m_renderer->frameSize() : QSize();
    if (frameSize.isEmpty() || width() <= 0 || height() <= 0) {
        return position;
    }
    return QPointF(position.x() * frameSize.width() / width(), position.y() * frameSize.height() / height());
}

void DomainViewer::updateCursorPaintPosition()
{
    QMutexLocker locker(&m_cursorMutex);
    if (m_hasVirtualPointerPosition) {
        m_cursorPaintPosition = m_virtualPointerPosition;
        m_hasCursorPaintPosition = true;
    } else if (m_hasLocalPointerPosition) {
        m_cursorPaintPosition = guestPositionForLocal(m_localPointerPosition);
        m_hasCursorPaintPosition = true;
    } else {
        m_hasCursorPaintPosition = false;
    }
}

void DomainViewer::updatePointer(const QPointF &position, int buttonMask)
{
    if (!m_inputs_channel || !m_connected || width() <= 0 || height() <= 0) {
        return;
    }

    const QSize frameSize = m_renderer ? m_renderer->frameSize() : QSize();
    const qreal scaleX = frameSize.width() > 0 ? frameSize.width() / width() : 1.0;
    const qreal scaleY = frameSize.height() > 0 ? frameSize.height() / height() : 1.0;

    if (!m_hasLastPointerPosition) {
        m_lastPointerPosition = position;
        m_hasLastPointerPosition = true;
    }

    const QPointF delta = position - m_lastPointerPosition;
    m_lastPointerPosition = position;
    m_localPointerPosition = position;
    m_hasLocalPointerPosition = true;

    QPointF guestPosition = guestPositionForLocal(position);
    if (frameSize.isValid()) {
        guestPosition.setX(qBound<qreal>(0, guestPosition.x(), frameSize.width()));
        guestPosition.setY(qBound<qreal>(0, guestPosition.y(), frameSize.height()));
    }

    if (m_pointerGrabbed) {
        if (!m_hasVirtualPointerPosition) {
            m_virtualPointerPosition = guestPosition;
            m_hasVirtualPointerPosition = true;
        } else {
            m_virtualPointerPosition += QPointF(delta.x() * scaleX, delta.y() * scaleY);
            if (frameSize.isValid()) {
                m_virtualPointerPosition.setX(qBound<qreal>(0, m_virtualPointerPosition.x(), frameSize.width()));
                m_virtualPointerPosition.setY(qBound<qreal>(0, m_virtualPointerPosition.y(), frameSize.height()));
            }
        }
    } else {
        m_virtualPointerPosition = guestPosition;
        m_hasVirtualPointerPosition = true;
    }
    updateCursorPaintPosition();

    if (mouseMode() == SPICE_MOUSE_MODE_SERVER) {
        // PS/2-style guests only understand relative motion. This also works
        // while the pointer is grabbed and the host cursor is outside the item.
        if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
            spice_inputs_channel_motion(m_inputs_channel, qRound(delta.x() * scaleX), qRound(delta.y() * scaleY), buttonMask);
            update();
        }
        return;
    }

    const QPointF absolute = m_pointerGrabbed ? m_virtualPointerPosition : guestPosition;
    spice_inputs_channel_position(m_inputs_channel, qRound(absolute.x()), qRound(absolute.y()), 0, buttonMask);
    update();
}

static quint32 evdevScancodeForEvent(const QKeyEvent *event)
{
    // X11/XWayland report X keycodes and Wayland reports xkb keycodes; both
    // are evdev scancodes shifted by 8. The old code only adjusted Wayland,
    // which made keyboard input off by eight keys under XWayland (and mapped
    // many keys to unrelated scancodes).
    quint32 scancode = event->nativeScanCode();
    const QString platform = QGuiApplication::platformName();
    if ((platform == QStringLiteral("xcb") || platform == QStringLiteral("wayland")) && scancode >= 8) {
        scancode -= 8;
    }
    return scancode;
}

void DomainViewer::keyPressEvent(QKeyEvent *event)
{
    if (m_pointerGrabbed && event->key() == Qt::Key_Escape) {
        releasePointerGrab();
        event->accept();
        return;
    }

    event->accept();
    const quint32 evdev_scancode = evdevScancodeForEvent(event);

    uint8_t pcxt_scancode = DomainViewer::evdevToPcXt(evdev_scancode); // SPICE accepts PC XT: see inputs channel docs
    qCDebug(KARTON_DEBUG) << "key press: " << event->text() << evdev_scancode << pcxt_scancode;

    if (m_inputs_channel && m_connected && pcxt_scancode != 0) {
        spice_inputs_channel_key_press(m_inputs_channel, pcxt_scancode);
    }
}

void DomainViewer::keyReleaseEvent(QKeyEvent *event)
{
    event->accept();

    const quint32 evdev_scancode = evdevScancodeForEvent(event);
    uint8_t pcxt_scancode = DomainViewer::evdevToPcXt(evdev_scancode);

    if (m_inputs_channel && m_connected && pcxt_scancode != 0) {
        spice_inputs_channel_key_release(m_inputs_channel, pcxt_scancode);
    }
}

void DomainViewer::wheelEvent(QWheelEvent *event)
{
    event->accept();

    if (!m_inputs_channel || !m_connected) {
        return;
    }
    updatePointer(event->position(), m_current_button_mask);

    QPoint angleDelta = event->angleDelta();
    // for more info on constants see: (Inputs channel definition) https://www.spice-space.org/spice-protocol.html
    if (angleDelta.y() > 0) { // scroll up
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_UP, 0);
        spice_inputs_channel_button_release(m_inputs_channel, SPICE_MOUSE_BUTTON_UP, 0);
    } else if (angleDelta.y() < 0) { // scroll down
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_DOWN, 0);
        spice_inputs_channel_button_release(m_inputs_channel, SPICE_MOUSE_BUTTON_DOWN, 0);
    }

    // Horizontal scrolling is not part of the SPICE mouse-wheel protocol.

    qCDebug(KARTON_DEBUG) << "wheel event at" << event->position() << "delta:" << angleDelta;
}

void DomainViewer::mouseMoveEvent(QMouseEvent *event)
{
    int buttonMask = 0;
    if (event->buttons() & Qt::LeftButton) {
        buttonMask |= SPICE_MOUSE_BUTTON_MASK_LEFT;
    }
    if (event->buttons() & Qt::MiddleButton) {
        buttonMask |= SPICE_MOUSE_BUTTON_MASK_MIDDLE;
    }
    if (event->buttons() & Qt::RightButton) {
        buttonMask |= SPICE_MOUSE_BUTTON_MASK_RIGHT;
    }

    updatePointer(event->position(), buttonMask);
    event->accept();
}

void DomainViewer::hoverEnterEvent(QHoverEvent *event)
{
    bool haveCursor = false;
    {
        QMutexLocker locker(&m_cursorMutex);
        m_pointerInside = true;
        if (!m_cursorImage.isNull()) {
            m_cursorVisible = true;
            haveCursor = true;
        }
    }
    if (haveCursor) {
        setCursor(Qt::BlankCursor);
    }
    QQuickItem::hoverEnterEvent(event);
}

void DomainViewer::hoverLeaveEvent(QHoverEvent *event)
{
    {
        QMutexLocker locker(&m_cursorMutex);
        m_pointerInside = false;
        m_cursorVisible = false;
    }
    if (!m_pointerGrabbed) {
        unsetCursor();
    }
    update();
    QQuickItem::hoverLeaveEvent(event);
}

void DomainViewer::hoverMoveEvent(QHoverEvent *event)
{
    updatePointer(event->position(), 0);
}

void DomainViewer::mousePressEvent(QMouseEvent *event)
{
    qCDebug(KARTON_DEBUG) << "Mouse click at (" << event->position().x() << "," << event->position().y() << ") button:" << event->button();
    m_localPointerPosition = event->position();
    m_hasLocalPointerPosition = true;
    m_lastPointerPosition = event->position();
    m_hasLastPointerPosition = true;
    // On Wayland the viewer must be the active window before Qt can read the
    // host clipboard. Clicking the display should activate it explicitly,
    // rather than requiring a click on the window frame.
    if (QQuickWindow *viewerWindow = window()) {
        viewerWindow->raise();
        viewerWindow->requestActivate();
    }
    grabPointer();
    forceActiveFocus();

    if (!m_inputs_channel || !m_connected) {
        return;
    }

    switch (event->button()) {
    case Qt::LeftButton:
        m_current_button_mask |= SPICE_MOUSE_BUTTON_MASK_LEFT;
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_LEFT, m_current_button_mask);
        break;
    case Qt::MiddleButton:
        m_current_button_mask |= SPICE_MOUSE_BUTTON_MASK_MIDDLE;
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_MIDDLE, m_current_button_mask);
        break;
    case Qt::RightButton:
        m_current_button_mask |= SPICE_MOUSE_BUTTON_MASK_RIGHT;
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_RIGHT, m_current_button_mask);
        break;
    default:
        return;
    }

    event->accept();
}

void DomainViewer::mouseReleaseEvent(QMouseEvent *event)
{
    qCDebug(KARTON_DEBUG) << "Mouse release at (" << event->position().x() << "," << event->position().y() << ") button:" << event->button();

    if (!m_inputs_channel || !m_connected) {
        return;
    }

    int button = 0;
    switch (event->button()) {
    case Qt::LeftButton:
        button = SPICE_MOUSE_BUTTON_LEFT;
        m_current_button_mask &= ~SPICE_MOUSE_BUTTON_MASK_LEFT;
        break;
    case Qt::MiddleButton:
        button = SPICE_MOUSE_BUTTON_MIDDLE;
        m_current_button_mask &= ~SPICE_MOUSE_BUTTON_MASK_MIDDLE;
        break;
    case Qt::RightButton:
        button = SPICE_MOUSE_BUTTON_RIGHT;
        m_current_button_mask &= ~SPICE_MOUSE_BUTTON_MASK_RIGHT;
        break;
    default:
        return;
    }

    spice_inputs_channel_button_release(m_inputs_channel, button, m_current_button_mask);

    event->accept();
}

void DomainViewer::focusOutEvent(QFocusEvent *event)
{
    if (!window() || !window()->isActive()) {
        releasePointerGrab();
    }
    QQuickItem::focusOutEvent(event);
}

void DomainViewer::checkChannelStatus()
{
    if (!m_session) {
        qCDebug(KARTON_DEBUG) << "Channel check: No active session";
        return;
    }
    if (!SPICE_IS_SESSION(m_session)) {
        qCWarning(KARTON_DEBUG) << "Session pointer is invalid!";
        m_session = nullptr;
        return;
    }

    // Transfer container: the channels themselves stay owned by the session.
    GList *channels = spice_session_get_channels(m_session);
    qCDebug(KARTON_DEBUG) << "Channel check: Found" << (channels ? g_list_length(channels) : 0) << "channels";

    bool hasDisplayChannel = false;
    for (GList *iter = channels; iter; iter = iter->next) {
        SpiceChannel *channel = SPICE_CHANNEL(iter->data);
        gint type;
        g_object_get(channel, "channel-type", &type, NULL);
        const gchar *type_str = spice_channel_type_to_string(type);

        qCDebug(KARTON_DEBUG) << "  - Channel type:" << QString::fromUtf8(type_str);

        if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
            hasDisplayChannel = true;
            qCDebug(KARTON_DEBUG) << "    Found display channel";
        }
    }

    g_list_free(channels);

    if (!hasDisplayChannel) {
        qCDebug(KARTON_DEBUG) << "No display channel yet" << (m_connected ? "(connected)" : "(connecting)");
    }
}
