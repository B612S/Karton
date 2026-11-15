// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "domainviewer.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <spice-client.h>

#include <cstdlib>

#include <KLocalizedString>

#include <QCursor>
#include <QDomDocument>
#include <QDomElement>
#include <QGuiApplication>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QString>

#include "domain.h"
#include "glib.h"
#include "glscanoutrenderer.h"
#include "karton_debug.h"
#include "primarysurfacerenderer.h"
#include "vulkanscanoutrenderer.h"

#include "evdev_to_xtkbd_map.h"

DomainViewer::DomainViewer(QQuickItem *parent)
    : QQuickItem(parent)
    , m_domain(nullptr)
    , m_spiceUri(QString())
    , m_connected(false)
    , m_session(nullptr)
    , m_inputs_channel(nullptr)
    , m_current_button_mask(0)
    , m_audioSink(nullptr)
    , m_audioDevice(nullptr)
    , m_audioFormat()
{
    setFlag(ItemHasContents, true);
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
            if (!port.isEmpty() && port != QStringLiteral("-1")) {
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

    qCInfo(KARTON_DEBUG) << "Connecting to URI! -" << m_spiceUri;
    g_object_set(m_session, "uri", m_spiceUri.toUtf8().constData(), NULL);

    g_signal_connect(m_session, "channel-new", G_CALLBACK(DomainViewer::channel_new_callback), this);

    if (!spice_session_connect(m_session)) {
        g_object_unref(m_session);
        m_session = nullptr;
        m_connected = false;
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

    m_inputs_channel = nullptr;
    m_connected = false;
}

void DomainViewer::channel_new_callback(SpiceSession *session, SpiceChannel *channel, gpointer user_data)
{
    Q_UNUSED(session);

    DomainViewer *item = static_cast<DomainViewer *>(user_data);

    item->checkChannelStatus();
    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE display connected";
        item->attachDisplayChannel(channel);
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
    } else if (SPICE_IS_CURSOR_CHANNEL(channel)) {
        qCInfo(KARTON_DEBUG) << "SPICE: Cursor connected";
        item->attachCursorChannel(channel);
    } else {
        gint type = 0;
        g_object_get(channel, "channel-type", &type, NULL);
        qCDebug(KARTON_DEBUG) << "Unhandled SPICE channel type:" << QString::fromUtf8(spice_channel_type_to_string(type));
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
    {
        QMutexLocker locker(&item->m_cursorMutex);
        item->m_cursorVisible = false;
    }
    item->unsetCursor();
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
    }
    unsetCursor();
    update();
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

// triggered by update()
QSGNode *DomainViewer::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    QSGNode *root = oldNode;
    if (!root) {
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
    {
        QMutexLocker locker(&m_cursorMutex);
        cursorImage = m_cursorImage;
        cursorPosition = m_cursorPosition;
        hotspot = m_cursorHotspot;
        cursorVisible = m_cursorVisible;
        textureDirty = m_cursorTextureDirty;
        m_cursorTextureDirty = false;
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

    const qreal x = (cursorPosition.x() - hotspot.x()) * scaleX;
    const qreal y = (cursorPosition.y() - hotspot.y()) * scaleY;
    m_cursorNode->setRect(QRectF(x, y, cursorImage.width() * scaleX, cursorImage.height() * scaleY));
    m_cursorNode->setFiltering(QSGTexture::Linear);

    return root;
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

    item->m_audioFormat.setSampleRate(rate);
    item->m_audioFormat.setChannelCount(channels);
    item->m_audioFormat.setSampleFormat(QAudioFormat::Int16);

    item->m_audioSink = new QAudioSink(item->m_audioFormat, item);
    item->m_audioDevice = item->m_audioSink->start();

    qCInfo(KARTON_DEBUG) << "Audio output started successfully";
}

void DomainViewer::playback_data_callback(SpicePlaybackChannel *channel, gpointer data, gint size, gpointer user_data)
{
    Q_UNUSED(channel);

    DomainViewer *item = static_cast<DomainViewer *>(user_data);
    if (item->m_audioDevice) {
        item->m_audioDevice->write(static_cast<const char *>(data), size);
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
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }

    m_audioDevice = nullptr;
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

void DomainViewer::keyPressEvent(QKeyEvent *event)
{
    event->accept();
    quint32 evdev_scancode;
    if (QGuiApplication::platformName() == QStringLiteral("xcb")) { // check if X11
        evdev_scancode = event->nativeScanCode();
    } else { // wayland probably
        evdev_scancode = event->nativeScanCode() - x11_wayland_evdev_offset;
    }

    uint8_t pcxt_scancode = DomainViewer::evdevToPcXt(evdev_scancode); // SPICE accepts PC XT: see inputs channel docs
    qCDebug(KARTON_DEBUG) << "key press: " << event->text() << evdev_scancode << pcxt_scancode;

    if (m_inputs_channel && m_connected && pcxt_scancode != 0) {
        spice_inputs_channel_key_press(m_inputs_channel, pcxt_scancode);
    }
}

void DomainViewer::keyReleaseEvent(QKeyEvent *event)
{
    event->accept();

    quint32 evdev_scancode;
    if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
        evdev_scancode = event->nativeScanCode();
    } else {
        evdev_scancode = event->nativeScanCode() - x11_wayland_evdev_offset;
    }

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
    qreal x = event->position().x();
    qreal y = event->position().y();

    const QSize frameSize = m_renderer ? m_renderer->frameSize() : QSize();
    if (frameSize.width() > 0 && frameSize.height() > 0 && width() > 0 && height() > 0) {
        x = (x * frameSize.width()) / width();
        y = (y * frameSize.height()) / height();
    }

    spice_inputs_channel_position(m_inputs_channel, x, y, 0, 0);

    QPoint angleDelta = event->angleDelta();
    // for more info on constants see: (Inputs channel definition) https://www.spice-space.org/spice-protocol.html
    if (angleDelta.y() > 0) { // scroll up
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_UP, 0);
        spice_inputs_channel_button_release(m_inputs_channel, SPICE_MOUSE_BUTTON_UP, 0);
    } else if (angleDelta.y() < 0) { // scroll down
        spice_inputs_channel_button_press(m_inputs_channel, SPICE_MOUSE_BUTTON_DOWN, 0);
        spice_inputs_channel_button_release(m_inputs_channel, SPICE_MOUSE_BUTTON_DOWN, 0);
    }

    if (angleDelta.x() > 0) { // scroll right
        // TODO: find horizontal in spice protocol
    } else if (angleDelta.x() < 0) { // scroll left
    }

    qCDebug(KARTON_DEBUG) << "wheel event at (" << x << "," << y << ") delta:" << angleDelta;
}

void DomainViewer::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_inputs_channel || !m_connected) {
        return;
    }

    int button_mask = 0;
    if (event->buttons() & Qt::LeftButton)
        button_mask |= SPICE_MOUSE_BUTTON_MASK_LEFT;
    if (event->buttons() & Qt::MiddleButton)
        button_mask |= SPICE_MOUSE_BUTTON_MASK_MIDDLE;
    if (event->buttons() & Qt::RightButton)
        button_mask |= SPICE_MOUSE_BUTTON_MASK_RIGHT;

    qreal x = event->position().x();
    qreal y = event->position().y();

    const QSize frameSize = m_renderer ? m_renderer->frameSize() : QSize();
    if (width() > 0 && height() > 0) {
        x = (x * frameSize.width()) / width();
        y = (y * frameSize.height()) / height();
    }

    // note: theres a warning that it's deprecated, but newer version has some bug with drag.
    spice_inputs_position(m_inputs_channel, x, y, 0, button_mask);
    static int moveCounter = 0;
    if (++moveCounter % 10 == 0) {
        qCDebug(KARTON_DEBUG) << "Mouse drag at (" << x << "," << y << "), mask: " << m_current_button_mask;
    }

    event->accept();
}
void DomainViewer::hoverMoveEvent(QHoverEvent *event)
{
    static int hoverCounter = 0;
    if (++hoverCounter % 20 == 0) {
        qCDebug(KARTON_DEBUG) << "Mouse hover at (" << event->position().x() << "," << event->position().y() << ")";
    }
    if (m_inputs_channel && m_connected) {
        qreal x = event->position().x();
        qreal y = event->position().y();

        const QSize frameSize = m_renderer ? m_renderer->frameSize() : QSize();
        if (frameSize.width() > 0 && frameSize.height() > 0 && width() > 0 && height() > 0) {
            x = (x * frameSize.width()) / width();
            y = (y * frameSize.height()) / height();
        }

        spice_inputs_channel_position(m_inputs_channel, x, y, 0, 0);
    }
}

void DomainViewer::mousePressEvent(QMouseEvent *event)
{
    qCDebug(KARTON_DEBUG) << "Mouse click at (" << event->position().x() << "," << event->position().y() << ") button:" << event->button();
    setFocus(true);

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
