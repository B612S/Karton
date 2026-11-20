// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>
#pragma once

#include <libvirt/libvirt.h>

#include <QHash>
#include <QMutex>
#include <QObject>

#include <memory>

class QSocketNotifier;
class QTimer;

// Qt-based implementation of the libvirt event loop. Registering it lets
// libvirt drive its file-descriptor watches and timers directly through the Qt
// event loop instead of polling virEventRunDefaultImpl() on a timer.
//
// Handles and timeouts may be added from any thread: libvirt registers them
// from the thread that opens the connection. The notifier/timer is created in
// that thread and its callback runs there, so a worker connection is driven by
// the worker's event loop while the GUI connection stays on the GUI thread.
class LibvirtEventLoop : public QObject
{
    Q_OBJECT

public:
    // Must be called before virConnectOpen().
    static void registerImpl();

private:
    explicit LibvirtEventLoop(QObject *parent = nullptr);

    struct Handle {
        int fd = -1;
        int events = 0;
        virEventHandleCallback callback = nullptr;
        void *opaque = nullptr;
        virFreeCallback freeCallback = nullptr;
        QSocketNotifier *readNotifier = nullptr;
        QSocketNotifier *writeNotifier = nullptr;
        bool removed = false;
    };

    struct Timeout {
        virEventTimeoutCallback callback = nullptr;
        void *opaque = nullptr;
        virFreeCallback freeCallback = nullptr;
        QTimer *timer = nullptr;
        bool removed = false;
    };

    static LibvirtEventLoop *instance();

    int addHandle(int fd, int events, virEventHandleCallback callback, void *opaque, virFreeCallback freeCallback);
    void updateHandle(int watch, int events);
    int removeHandle(int watch);
    void invokeHandle(int watch, int events);

    int addTimeout(int timeout, virEventTimeoutCallback callback, void *opaque, virFreeCallback freeCallback);
    void updateTimeout(int timer, int timeout);
    int removeTimeout(int timer);
    void invokeTimeout(int timer);

    static int addHandleCallback(int fd, int events, virEventHandleCallback callback, void *opaque, virFreeCallback freeCallback);
    static void updateHandleCallback(int watch, int events);
    static int removeHandleCallback(int watch);
    static int addTimeoutCallback(int timeout, virEventTimeoutCallback callback, void *opaque, virFreeCallback freeCallback);
    static void updateTimeoutCallback(int timer, int timeout);
    static int removeTimeoutCallback(int timer);

    mutable QMutex m_mutex;
    QHash<int, std::shared_ptr<Handle>> m_handles;
    QHash<int, std::shared_ptr<Timeout>> m_timeouts;
    int m_nextHandle = 1;
    int m_nextTimeout = 1;
};
