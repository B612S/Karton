// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#include "viewer/clipboardmanager.h"

#include <spice/vd_agent.h>

#include <QClipboard>
#include <QGuiApplication>
#include <QTimer>

#include "karton_debug.h"

ClipboardManager::ClipboardManager(QObject *parent)
    : QObject(parent)
{
}

void ClipboardManager::attach(SpiceMainChannel *channel)
{
    detach();
    if (!channel) {
        return;
    }
    m_channel = channel;
    g_signal_connect(channel, "main-clipboard-selection-grab", G_CALLBACK(clipboardSelectionGrabCallback), this);
    g_signal_connect(channel, "main-clipboard-selection-request", G_CALLBACK(clipboardSelectionRequestCallback), this);
    g_signal_connect(channel, "main-clipboard-selection", G_CALLBACK(clipboardSelectionCallback), this);
    m_clipboardConnection = QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this]() {
        hostClipboardChanged();
    });
}

void ClipboardManager::detach()
{
    QObject::disconnect(m_clipboardConnection);
    m_clipboardConnection = {};
    m_channel = nullptr;
    m_lastGuestClipboardText.clear();
    m_lastSentHostClipboard.clear();
    m_settingClipboard = false;
    m_pendingHostGrab = false;
}

void ClipboardManager::setTextMode(int mode)
{
    m_textMode = qBound(0, mode, 3);
}

void ClipboardManager::setFilesMode(int mode)
{
    m_filesMode = qBound(0, mode, 3);
}

void ClipboardManager::syncHostClipboard()
{
    if (m_channel) {
        hostClipboardChanged();
    }
}

gboolean ClipboardManager::clipboardSelectionGrabCallback(SpiceMainChannel *channel, guint selection, guint32 *types, guint ntypes, gpointer userData)
{
    Q_UNUSED(types);
    Q_UNUSED(ntypes);
    auto *manager = static_cast<ClipboardManager *>(userData);
    if (manager->m_textMode != 2 && manager->m_textMode != 3) {
        return TRUE;
    }
    if (manager->m_pendingHostGrab) {
        qCInfo(KARTON_DEBUG) << "clipboard: guest grab ignored while host clipboard is pending";
        manager->m_pendingHostGrab = false;
        return TRUE;
    }
    qCInfo(KARTON_DEBUG) << "clipboard: guest has data, requesting text";
    spice_main_channel_clipboard_selection_request(channel, selection, VD_AGENT_CLIPBOARD_UTF8_TEXT);
    return TRUE;
}

gboolean ClipboardManager::clipboardSelectionRequestCallback(SpiceMainChannel *channel, guint selection, guint32 type, gpointer userData)
{
    auto *manager = static_cast<ClipboardManager *>(userData);
    if (manager->m_textMode != 1 && manager->m_textMode != 3) {
        return FALSE;
    }
    if (type != VD_AGENT_CLIPBOARD_UTF8_TEXT) {
        return FALSE;
    }
    const QByteArray text = QGuiApplication::clipboard()->text(QClipboard::Clipboard).toUtf8();
    manager->m_pendingHostGrab = false;
    qCInfo(KARTON_DEBUG) << "clipboard: guest requested text, sending" << text.size() << "bytes, selection" << selection << "by-demand"
                         << spice_main_channel_agent_test_capability(channel, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND) << "selection-cap"
                         << spice_main_channel_agent_test_capability(channel, VD_AGENT_CAP_CLIPBOARD_SELECTION);
    g_object_ref(channel);
    QTimer::singleShot(0, qApp, [channel, selection, text]() {
        spice_main_channel_clipboard_selection_notify(channel,
                                                      selection,
                                                      VD_AGENT_CLIPBOARD_UTF8_TEXT,
                                                      reinterpret_cast<const guchar *>(text.constData()),
                                                      static_cast<size_t>(text.size()));
        g_object_unref(channel);
    });
    return TRUE;
}

void ClipboardManager::clipboardSelectionCallback(SpiceMainChannel *channel, guint selection, guint32 type, gconstpointer data, guint size, gpointer userData)
{
    Q_UNUSED(channel);
    Q_UNUSED(selection);
    if (type != VD_AGENT_CLIPBOARD_UTF8_TEXT || !data) {
        return;
    }
    auto *manager = static_cast<ClipboardManager *>(userData);
    if (manager->m_textMode != 2 && manager->m_textMode != 3) {
        return;
    }
    const QString text = QString::fromUtf8(static_cast<const char *>(data), static_cast<int>(size));
    qCInfo(KARTON_DEBUG) << "clipboard: received text from guest" << text.size() << "chars";
    manager->m_lastGuestClipboardText = text;
    manager->m_lastSentHostClipboard = text;
    manager->m_settingClipboard = true;
    QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
    manager->m_settingClipboard = false;
}

void ClipboardManager::hostClipboardChanged()
{
    if (!m_channel || m_settingClipboard) {
        return;
    }
    if (m_textMode != 1 && m_textMode != 3) {
        return;
    }
    const QString text = QGuiApplication::clipboard()->text(QClipboard::Clipboard);
    qCInfo(KARTON_DEBUG) << "clipboard: host changed, mode" << m_textMode << "text" << text.size() << "chars";
    if (text.isEmpty()) {
        return;
    }
    if (!m_lastGuestClipboardText.isEmpty() && text == m_lastGuestClipboardText) {
        m_lastGuestClipboardText.clear();
        return;
    }
    if (text == m_lastSentHostClipboard) {
        return;
    }
    m_lastSentHostClipboard = text;
    guint32 types[] = {VD_AGENT_CLIPBOARD_UTF8_TEXT};
    m_pendingHostGrab = true;
    spice_main_channel_clipboard_selection_grab(m_channel, VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD, types, 1);
}
