// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#pragma once

#include <linux/input-event-codes.h>
#include <spice-client.h>

#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
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
    Q_PROPERTY(bool agentConnected READ agentConnected NOTIFY agentConnectedChanged)
    Q_PROPERTY(bool audioPlaybackEnabled READ audioPlaybackEnabled WRITE setAudioPlaybackEnabled NOTIFY audioPlaybackEnabledChanged)
    Q_PROPERTY(bool microphoneEnabled READ microphoneEnabled WRITE setMicrophoneEnabled NOTIFY microphoneEnabledChanged)
    Q_PROPERTY(bool spiceTlsEnabled READ spiceTlsEnabled WRITE setSpiceTlsEnabled NOTIFY spiceTlsEnabledChanged)
    Q_PROPERTY(QString spiceCaCertificate READ spiceCaCertificate WRITE setSpiceCaCertificate NOTIFY spiceCaCertificateChanged)
    Q_PROPERTY(int clipboardTextMode READ clipboardTextMode WRITE setClipboardTextMode NOTIFY clipboardTextModeChanged)
    Q_PROPERTY(int clipboardFilesMode READ clipboardFilesMode WRITE setClipboardFilesMode NOTIFY clipboardFilesModeChanged)

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
    Q_INVOKABLE void grabPointer();
    Q_INVOKABLE void releasePointerGrab();
    Q_INVOKABLE void syncHostClipboard();

    void componentComplete() override;

    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
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
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
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
    void agentConnectedChanged();
    void audioPlaybackEnabledChanged();
    void microphoneEnabledChanged();
    void spiceTlsEnabledChanged();
    void spiceCaCertificateChanged();
    void clipboardTextModeChanged();
    void clipboardFilesModeChanged();

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
    static gboolean clipboardSelectionGrabCallback(SpiceMainChannel *channel, guint selection, guint32 *types, guint ntypes, gpointer userData);
    static gboolean clipboardSelectionRequestCallback(SpiceMainChannel *channel, guint selection, guint32 type, gpointer userData);
    static void clipboardSelectionCallback(SpiceMainChannel *channel, guint selection, guint32 type, gconstpointer data, guint size, gpointer userData);
    void hostClipboardChanged();
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

    SpiceSession *m_session = nullptr;
    SpiceMainChannel *m_main_channel = nullptr;
    SpiceInputsChannel *m_inputs_channel = nullptr;
    QMetaObject::Connection m_clipboardConnection;
    bool m_settingClipboard = false;

    int m_current_button_mask = 0;
    bool m_pointerGrabbed = false;
    bool m_agentConnected = false;
    QPointF m_lastPointerPosition;
    bool m_hasLastPointerPosition = false;
    QPointF m_localPointerPosition;
    bool m_hasLocalPointerPosition = false;
    QPointF m_virtualPointerPosition;
    bool m_hasVirtualPointerPosition = false;

    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_audioDevice = nullptr;
    QAudioFormat m_audioFormat;
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
    QString m_lastGuestClipboardText;
    QElapsedTimer m_hostClipboardAge;
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
    int m_fixedDisplayWidth = 0;
    int m_fixedDisplayHeight = 0;

    QSGNode *m_displayNode = nullptr;
    QSGSimpleTextureNode *m_cursorNode = nullptr;
    bool m_previewGrabPending = false;

    static constexpr quint32 x11_wayland_evdev_offset = 8;
    // difference of 8 between x11 wayland.
    // see: https://wayland-devel.freedesktop.narkive.com/6dOtsFGc/gtk-hardware-scancodes-for-wayland-detecting-xwayland
};
