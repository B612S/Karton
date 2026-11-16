// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#pragma once

#include <linux/input-event-codes.h>
#include <spice-client.h>

#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QQuickItem>
#include <QSGNode>
#include <QSize>

#include <memory>

class QSGSimpleTextureNode;

#include "domain/domain.h"
#include "viewer/spicedisplayrenderer.h"

class DomainViewer : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(Domain *domain READ domain WRITE setDomain NOTIFY domainChanged REQUIRED)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(bool pointerGrabbed READ pointerGrabbed NOTIFY pointerGrabbedChanged)

public:
    explicit DomainViewer(QQuickItem *parent = nullptr);
    ~DomainViewer() override;

    Domain *domain() const
    {
        return m_domain;
    }
    void setDomain(Domain *domain);

    QString errorMessage() const
    {
        return m_errorMessage;
    }
    void setErrorMessage(const QString &message);

    bool pointerGrabbed() const
    {
        return m_pointerGrabbed;
    }
    Q_INVOKABLE void grabPointer();
    Q_INVOKABLE void releasePointerGrab();

    void componentComplete() override;

    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    bool setupSpiceSession();
    bool connectToSpice();
    Q_INVOKABLE void disconnectFromSpice();

    void stopAudio();

    void checkChannelStatus();

    void mouseMoveEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

    Q_INVOKABLE void saveFrameToDomain();
    Q_INVOKABLE void updateDisplayResolution();

Q_SIGNALS:
    void domainChanged();
    void errorMessageChanged();
    void pointerGrabbedChanged();

private Q_SLOTS:
    void handleRendererFrameSizeChanged();

private:
    enum class RenderMode {
        GlScanout,
        VulkanScanout,
        PrimarySurface,
    };

    static void channel_new_callback(SpiceSession *session, SpiceChannel *channel, gpointer user_data);
    void attachDisplayChannel(SpiceChannel *channel);
    void attachCursorChannel(SpiceChannel *channel);
    static uint8_t evdevToPcXt(uint32_t evdev_scancode);

    static void cursorSetCallback(SpiceCursorChannel *channel, gint width, gint height, gint hotX, gint hotY, gpointer rgba, gpointer userData);
    static void cursorMoveCallback(SpiceCursorChannel *channel, gint x, gint y, gpointer userData);
    static void mainAgentUpdateCallback(SpiceMainChannel *channel, gpointer userData);
    static void cursorHideCallback(SpiceCursorChannel *channel, gpointer userData);
    static void cursorResetCallback(SpiceCursorChannel *channel, gpointer userData);
    void clearCursor();

    static void playback_start_callback(SpicePlaybackChannel *channel, gint format, gint channels, gint rate, gpointer user_data);
    static void playback_data_callback(SpicePlaybackChannel *channel, gpointer data, gint size, gpointer user_data);
    static void playback_stop_callback(SpicePlaybackChannel *channel, gpointer user_data);

    void updatePointer(const QPointF &position, int buttonMask);
    void updateCursorPaintPosition();
    QPointF guestPositionForLocal(const QPointF &position) const;
    int mouseMode() const;

    Domain *m_domain;
    QString m_spiceUri;
    QString m_errorMessage;
    bool m_connected = false;
    RenderMode m_renderMode = RenderMode::GlScanout;

    std::unique_ptr<SpiceDisplayRenderer> m_renderer;

    SpiceSession *m_session = nullptr;
    SpiceMainChannel *m_main_channel = nullptr;
    SpiceInputsChannel *m_inputs_channel = nullptr;

    int m_current_button_mask = 0;
    bool m_pointerGrabbed = false;
    QPointF m_lastPointerPosition;
    bool m_hasLastPointerPosition = false;
    QPointF m_localPointerPosition;
    bool m_hasLocalPointerPosition = false;
    QPointF m_virtualPointerPosition;
    bool m_hasVirtualPointerPosition = false;

    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_audioDevice = nullptr;
    QAudioFormat m_audioFormat;

    QMutex m_cursorMutex;
    QImage m_cursorImage;
    QPoint m_cursorPosition;
    QPoint m_cursorHotspot;
    bool m_cursorVisible = false;
    bool m_cursorTextureDirty = true;
    QPointF m_cursorPaintPosition;
    bool m_hasCursorPaintPosition = false;
    QSize m_lastDisplayResolution;

    QSGNode *m_displayNode = nullptr;
    QSGSimpleTextureNode *m_cursorNode = nullptr;

    static constexpr quint32 x11_wayland_evdev_offset = 8;
    // difference of 8 between x11 wayland.
    // see: https://wayland-devel.freedesktop.narkive.com/6dOtsFGc/gtk-hardware-scancodes-for-wayland-detecting-xwayland
};
