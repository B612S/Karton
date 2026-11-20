// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "libvirt/libvirteventloop.h"

#include <QAbstractEventDispatcher>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>

#include "karton_debug.h"

namespace
{
LibvirtEventLoop *s_eventLoop = nullptr;

// libvirt calls removeHandle() from inside its own teardown
// (virConnectClose -> virNetSocketDispose) while holding object locks.
// Invoking the handle's free callback in that same call stack re-enters the
// object and deadlocks on a non-recursive mutex, so run it on the next
// event-loop turn of the thread that registered the handle instead. The object
// stays alive until the callback releases the reference libvirt handed to the
// event loop, so deferring cannot free it too early.
void deferHandleFreeCallback(virFreeCallback freeCallback, void *opaque)
{
    if (!freeCallback) {
        return;
    }
    if (QAbstractEventDispatcher::instance()) {
        QTimer::singleShot(0, [freeCallback, opaque]() {
            freeCallback(opaque);
        });
    } else {
        // No event loop in this thread to defer to; keep the old behaviour.
        freeCallback(opaque);
    }
}
} // namespace

LibvirtEventLoop::LibvirtEventLoop(QObject *parent)
    : QObject(parent)
{
}

void LibvirtEventLoop::registerImpl()
{
    if (s_eventLoop) {
        return;
    }

    s_eventLoop = new LibvirtEventLoop;
    virEventRegisterImpl(addHandleCallback, updateHandleCallback, removeHandleCallback, addTimeoutCallback, updateTimeoutCallback, removeTimeoutCallback);
}

LibvirtEventLoop *LibvirtEventLoop::instance()
{
    Q_ASSERT(s_eventLoop);
    return s_eventLoop;
}

int LibvirtEventLoop::addHandle(int fd, int events, virEventHandleCallback callback, void *opaque, virFreeCallback freeCallback)
{
    auto handle = std::make_shared<Handle>();
    handle->fd = fd;
    handle->callback = callback;
    handle->opaque = opaque;
    handle->freeCallback = freeCallback;

    int watch = 0;
    {
        QMutexLocker locker(&m_mutex);
        watch = m_nextHandle++;
        m_handles.insert(watch, handle);
    }
    updateHandle(watch, events);
    return watch;
}

void LibvirtEventLoop::updateHandle(int watch, int events)
{
    std::shared_ptr<Handle> handle;
    {
        QMutexLocker locker(&m_mutex);
        const auto it = m_handles.constFind(watch);
        if (it == m_handles.cend()) {
            return;
        }
        handle = *it;
    }
    if (handle->removed) {
        return;
    }

    handle->events = events;

    if (events & VIR_EVENT_HANDLE_READABLE) {
        if (!handle->readNotifier) {
            handle->readNotifier = new QSocketNotifier(handle->fd, QSocketNotifier::Read);
            connect(handle->readNotifier, &QSocketNotifier::activated, handle->readNotifier, [this, watch](QSocketDescriptor, QSocketNotifier::Type) {
                invokeHandle(watch, VIR_EVENT_HANDLE_READABLE);
            });
        }
    } else if (handle->readNotifier) {
        handle->readNotifier->deleteLater();
        handle->readNotifier = nullptr;
    }

    if (events & VIR_EVENT_HANDLE_WRITABLE) {
        if (!handle->writeNotifier) {
            handle->writeNotifier = new QSocketNotifier(handle->fd, QSocketNotifier::Write);
            connect(handle->writeNotifier, &QSocketNotifier::activated, handle->writeNotifier, [this, watch](QSocketDescriptor, QSocketNotifier::Type) {
                invokeHandle(watch, VIR_EVENT_HANDLE_WRITABLE);
            });
        }
    } else if (handle->writeNotifier) {
        handle->writeNotifier->deleteLater();
        handle->writeNotifier = nullptr;
    }
}

void LibvirtEventLoop::invokeHandle(int watch, int events)
{
    std::shared_ptr<Handle> handle;
    {
        QMutexLocker locker(&m_mutex);
        const auto it = m_handles.constFind(watch);
        if (it == m_handles.cend()) {
            return;
        }
        handle = *it;
    }
    if (handle->removed || !handle->callback) {
        return;
    }
    handle->callback(watch, handle->fd, events, handle->opaque);
}

int LibvirtEventLoop::removeHandle(int watch)
{
    std::shared_ptr<Handle> handle;
    {
        QMutexLocker locker(&m_mutex);
        handle = m_handles.take(watch);
    }
    if (!handle) {
        return -1;
    }

    handle->removed = true;
    if (handle->readNotifier) {
        handle->readNotifier->deleteLater();
        handle->readNotifier = nullptr;
    }
    if (handle->writeNotifier) {
        handle->writeNotifier->deleteLater();
        handle->writeNotifier = nullptr;
    }
    if (handle->freeCallback) {
        deferHandleFreeCallback(handle->freeCallback, handle->opaque);
    }
    return 0;
}

int LibvirtEventLoop::addTimeout(int timeout, virEventTimeoutCallback callback, void *opaque, virFreeCallback freeCallback)
{
    auto entry = std::make_shared<Timeout>();
    entry->callback = callback;
    entry->opaque = opaque;
    entry->freeCallback = freeCallback;
    entry->timer = new QTimer;
    // libvirt timeouts are one-shot: the callback re-arms the timer through
    // updateTimeout(). A repeating timer here fires again when a callback
    // intentionally stops re-arming, which can spin the event loop.
    entry->timer->setSingleShot(true);

    int id = 0;
    {
        QMutexLocker locker(&m_mutex);
        id = m_nextTimeout++;
        m_timeouts.insert(id, entry);
    }

    connect(entry->timer, &QTimer::timeout, entry->timer, [this, id] {
        invokeTimeout(id);
    });

    updateTimeout(id, timeout);
    return id;
}

void LibvirtEventLoop::updateTimeout(int timer, int timeout)
{
    std::shared_ptr<Timeout> entry;
    {
        QMutexLocker locker(&m_mutex);
        const auto it = m_timeouts.constFind(timer);
        if (it == m_timeouts.cend()) {
            return;
        }
        entry = *it;
    }
    if (entry->removed || !entry->timer) {
        return;
    }

    auto apply = [entry, timeout]() {
        if (entry->removed || !entry->timer) {
            return;
        }
        if (timeout < 0) {
            entry->timer->stop();
            return;
        }
        // Restart even when the timer is running: libvirt re-arms its timeouts
        // from inside the callback, and the interval may have changed. A single
        // shot timer with a 0 ms interval runs on the next event loop iteration,
        // so it cannot spin the loop the way a 0 ms repeating timer would.
        entry->timer->setInterval(qMax(0, timeout));
        entry->timer->start();
    };

    if (QThread::currentThread() == entry->timer->thread()) {
        apply();
    } else {
        QMetaObject::invokeMethod(entry->timer, apply, Qt::QueuedConnection);
    }
}

void LibvirtEventLoop::invokeTimeout(int timer)
{
    std::shared_ptr<Timeout> entry;
    {
        QMutexLocker locker(&m_mutex);
        const auto it = m_timeouts.constFind(timer);
        if (it == m_timeouts.cend()) {
            return;
        }
        entry = *it;
    }
    if (entry->removed || !entry->callback) {
        return;
    }
    entry->callback(timer, entry->opaque);
}

int LibvirtEventLoop::removeTimeout(int timer)
{
    std::shared_ptr<Timeout> entry;
    {
        QMutexLocker locker(&m_mutex);
        entry = m_timeouts.take(timer);
    }
    if (!entry) {
        return -1;
    }

    entry->removed = true;
    if (entry->timer) {
        auto stop = [entry]() {
            entry->timer->stop();
            entry->timer->deleteLater();
        };
        if (QThread::currentThread() == entry->timer->thread()) {
            stop();
        } else {
            QMetaObject::invokeMethod(entry->timer, stop, Qt::QueuedConnection);
        }
    }
    if (entry->freeCallback) {
        entry->freeCallback(entry->opaque);
    }
    return 0;
}

int LibvirtEventLoop::addHandleCallback(int fd, int events, virEventHandleCallback callback, void *opaque, virFreeCallback freeCallback)
{
    return instance()->addHandle(fd, events, callback, opaque, freeCallback);
}

void LibvirtEventLoop::updateHandleCallback(int watch, int events)
{
    instance()->updateHandle(watch, events);
}

int LibvirtEventLoop::removeHandleCallback(int watch)
{
    return instance()->removeHandle(watch);
}

int LibvirtEventLoop::addTimeoutCallback(int timeout, virEventTimeoutCallback callback, void *opaque, virFreeCallback freeCallback)
{
    return instance()->addTimeout(timeout, callback, opaque, freeCallback);
}

void LibvirtEventLoop::updateTimeoutCallback(int timer, int timeout)
{
    instance()->updateTimeout(timer, timeout);
}

int LibvirtEventLoop::removeTimeoutCallback(int timer)
{
    return instance()->removeTimeout(timer);
}
