// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <linux/input-event-codes.h>
#include <spice-client.h>

#include <QAudioFormat>
#include <QAudioSource>
#include <QByteArray>
#include <QElapsedTimer>
#include <QIODevice>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QQuickItem>
#include <QSGNode>
#include <QSize>
#include <QStringConverter>
#include <QTimer>

#include <memory>

class QSGSimpleTextureNode;
class ClipboardManager;

#include "domain/domain.h"
#include "viewer/pipewireaudio.h"
#include "viewer/spicedisplayrenderer.h"

class DomainViewer : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(Domain *domain READ domain WRITE setDomain NOTIFY domainChanged REQUIRED)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(bool pointerGrabbed READ pointerGrabbed NOTIFY pointerGrabbedChanged)
    Q_PROPERTY(bool pointerInside READ pointerInside NOTIFY pointerInsideChanged)
    Q_PROPERTY(bool agentConnected READ agentConnected NOTIFY agentConnectedChanged)
    Q_PROPERTY(bool audioPlaybackEnabled READ audioPlaybackEnabled WRITE setAudioPlaybackEnabled NOTIFY audioPlaybackEnabledChanged)
    Q_PROPERTY(bool microphoneEnabled READ microphoneEnabled WRITE setMicrophoneEnabled NOTIFY microphoneEnabledChanged)
    Q_PROPERTY(bool spiceTlsEnabled READ spiceTlsEnabled WRITE setSpiceTlsEnabled NOTIFY spiceTlsEnabledChanged)
    Q_PROPERTY(QString spiceCaCertificate READ spiceCaCertificate WRITE setSpiceCaCertificate NOTIFY spiceCaCertificateChanged)
    Q_PROPERTY(int clipboardTextMode READ clipboardTextMode WRITE setClipboardTextMode NOTIFY clipboardTextModeChanged)
    Q_PROPERTY(int clipboardFilesMode READ clipboardFilesMode WRITE setClipboardFilesMode NOTIFY clipboardFilesModeChanged)
    Q_PROPERTY(bool serialConsoleActive READ serialConsoleActive NOTIFY serialConsoleActiveChanged)

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
    bool pointerInside() const
    {
        return m_pointerInside;
    }
    bool agentConnected() const
    {
        return m_agentConnected;
    }
    bool audioPlaybackEnabled() const
    {
        return m_audioPlaybackEnabled;
    }
    void setAudioPlaybackEnabled(bool enabled);
    bool microphoneEnabled() const
    {
        return m_microphoneEnabled;
    }
    void setMicrophoneEnabled(bool enabled);
    bool spiceTlsEnabled() const
    {
        return m_spiceTlsEnabled;
    }
    void setSpiceTlsEnabled(bool enabled);
    QString spiceCaCertificate() const
    {
        return m_spiceCaCertificate;
    }
    void setSpiceCaCertificate(const QString &path);
    int clipboardTextMode() const
    {
        return m_clipboardTextMode;
    }
    void setClipboardTextMode(int mode);
    int clipboardFilesMode() const
    {
        return m_clipboardFilesMode;
    }
    void setClipboardFilesMode(int mode);
    bool serialConsoleActive() const
    {
        return m_serialConsoleActive;
    }
    Q_INVOKABLE void sendConsoleText(const QString &text);
    Q_INVOKABLE void grabPointer();
    Q_INVOKABLE void releasePointerGrab();

    void componentComplete() override;

    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void releaseResources() override;
    Q_INVOKABLE bool setupSpiceSession();
    bool connectToSpice();
    Q_INVOKABLE void disconnectFromSpice();

    void stopAudio();

    void checkChannelStatus();

    void mouseMoveEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

    Q_INVOKABLE void saveFrameToDomain();
    Q_INVOKABLE void updateDomainPreview();
    Q_INVOKABLE void setFixedDisplayResolution(int width, int height);
    Q_INVOKABLE void updateDisplayResolution();
    Q_INVOKABLE void fitGuestToWindow();

Q_SIGNALS:
    void domainChanged();
    void errorMessageChanged();
    void pointerGrabbedChanged();
    void pointerInsideChanged();
    void agentConnectedChanged();
    void audioPlaybackEnabledChanged();
    void microphoneEnabledChanged();
    void spiceTlsEnabledChanged();
    void spiceCaCertificateChanged();
    void clipboardTextModeChanged();
    void clipboardFilesModeChanged();
    void serialConsoleActiveChanged();
    void consoleDataReceived(const QString &text);

private Q_SLOTS:
    void handleRendererFrameSizeChanged();
    void updateLivePreview();
    void applyGuestCursor();
    void applyDisplayResolution();

private:
    enum class RenderMode {
        GlScanout,
        PrimarySurface,
    };

    static void channel_new_callback(SpiceSession *session, SpiceChannel *channel, gpointer user_data);
    void attachDisplayChannel(SpiceChannel *channel);
    void attachCursorChannel(SpiceChannel *channel);
    static uint32_t evdevToPcXt(uint32_t evdev_scancode);

    bool openSerialConsole();
    void closeSerialConsole();
    void scheduleConsoleClose();
    static void consoleEventCallback(virStreamPtr stream, int events, void *opaque);
    void flushConsoleWrites();
    void updateConsoleEvents();

    static void cursorSetCallback(SpiceCursorChannel *channel, gint width, gint height, gint hotX, gint hotY, gpointer rgba, gpointer userData);
    static void cursorMoveCallback(SpiceCursorChannel *channel, gint x, gint y, gpointer userData);
    static void mainAgentUpdateCallback(SpiceMainChannel *channel, gpointer userData);
    static void cursorHideCallback(SpiceCursorChannel *channel, gpointer userData);
    static void cursorResetCallback(SpiceCursorChannel *channel, gpointer userData);
    void clearCursor();

    static void playback_start_callback(SpicePlaybackChannel *channel, gint format, gint channels, gint rate, gpointer user_data);
    static void playback_data_callback(SpicePlaybackChannel *channel, gpointer data, gint size, gpointer user_data);
    static void playback_stop_callback(SpicePlaybackChannel *channel, gpointer user_data);
    static void record_start_callback(SpiceRecordChannel *channel, gint format, gint channels, gint rate, gpointer user_data);
    static void record_stop_callback(SpiceRecordChannel *channel, gpointer user_data);
    void startRecording();
    void stopRecording();
    void readRecordingData();

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
    // A detached renderer is kept alive until releaseResources() runs on the
    // render thread, so its EGL image and GL texture are not leaked when the
    // viewer is closed.
    std::unique_ptr<SpiceDisplayRenderer> m_detachedRenderer;

    SpiceSession *m_session = nullptr;
    SpiceMainChannel *m_main_channel = nullptr;
    SpiceInputsChannel *m_inputs_channel = nullptr;
    ClipboardManager *m_clipboard = nullptr;

    virStreamPtr m_consoleStream = nullptr;
    bool m_serialConsoleActive = false;
    bool m_consoleClosePending = false;
    quint64 m_consoleGeneration = 0;
    QByteArray m_consoleWriteBuffer;
    QStringDecoder m_consoleDecoder{QStringConverter::Utf8};

    int m_current_button_mask = 0;
    bool m_pointerGrabbed = false;
    bool m_agentConnected = false;
    QPointF m_lastPointerPosition;
    bool m_hasLastPointerPosition = false;
    QPointF m_localPointerPosition;
    bool m_hasLocalPointerPosition = false;
    QPointF m_virtualPointerPosition;
    bool m_hasVirtualPointerPosition = false;

    PipeWireAudioSink *m_audioPipeWire = nullptr;
    SpiceRecordChannel *m_recordChannel = nullptr;
    QAudioSource *m_audioSource = nullptr;
    QIODevice *m_recordDevice = nullptr;
    QAudioFormat m_recordFormat;
    bool m_audioPlaybackEnabled = true;
    bool m_microphoneEnabled = false;
    bool m_recording = false;
    bool m_spiceTlsEnabled = true;
    QString m_spiceCaCertificate;
    int m_clipboardTextMode = 3;
    int m_clipboardFilesMode = 0;
    int m_spiceTlsPort = 0;
    QString m_spicePassword;

    QMutex m_cursorMutex;
    QImage m_cursorImage;
    QPoint m_cursorPosition;
    QPoint m_cursorHotspot;
    bool m_cursorVisible = false;
    bool m_cursorTextureDirty = true;
    QPointF m_cursorPaintPosition;
    bool m_hasCursorPaintPosition = false;
    bool m_pointerInside = false;
    QSize m_lastDisplayResolution;
    // Coalesces the geometry burst while a window is restored from fullscreen:
    // the page first loses the toolbar, then the window shrinks, and each step
    // would otherwise ask the guest agent for a new resolution. Sending only
    // the final size avoids a visible double mode switch in the guest.
    QTimer m_displayResizeTimer;
    int m_fixedDisplayWidth = 0;
    int m_fixedDisplayHeight = 0;

    QSGNode *m_displayNode = nullptr;
    QSGSimpleTextureNode *m_cursorNode = nullptr;
    bool m_previewGrabPending = false;
    QElapsedTimer m_livePreviewTimer;

    static constexpr quint32 x11_wayland_evdev_offset = 8;
    // difference of 8 between x11 wayland.
    // see: https://wayland-devel.freedesktop.narkive.com/6dOtsFGc/gtk-hardware-scancodes-for-wayland-detecting-xwayland
};
