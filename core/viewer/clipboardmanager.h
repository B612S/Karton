// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#pragma once

#include <QMetaObject>
#include <QObject>
#include <QString>

#include <spice-client.h>

// Bridges the Qt clipboard and the SPICE agent clipboard. Keeping this state
// outside DomainViewer mirrors the ownership bookkeeping done by spice-gtk's
// SpiceGtkSession while still using Qt's clipboard on the host side.
class ClipboardManager : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardManager(QObject *parent = nullptr);

    void attach(SpiceMainChannel *channel);
    void detach();

    void setTextMode(int mode);
    int textMode() const
    {
        return m_textMode;
    }
    void setFilesMode(int mode);
    int filesMode() const
    {
        return m_filesMode;
    }

    void syncHostClipboard();

private:
    static gboolean clipboardSelectionGrabCallback(SpiceMainChannel *channel, guint selection, guint32 *types, guint ntypes, gpointer userData);
    static gboolean clipboardSelectionRequestCallback(SpiceMainChannel *channel, guint selection, guint32 type, gpointer userData);
    static void clipboardSelectionCallback(SpiceMainChannel *channel, guint selection, guint32 type, gconstpointer data, guint size, gpointer userData);

    void hostClipboardChanged();

    SpiceMainChannel *m_channel = nullptr;
    QMetaObject::Connection m_clipboardConnection;
    int m_textMode = 3;
    int m_filesMode = 0;
    QString m_lastGuestClipboardText;
    QString m_lastSentHostClipboard;
    bool m_settingClipboard = false;
    bool m_pendingHostGrab = false;
};
