// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "libvirteventloop.h"

#include <QSocketNotifier>
#include <QTimer>

#include "karton_debug.h"

namespace
{
LibvirtEventLoop *s_eventLoop = nullptr;
}

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
    auto *handle = new Handle;
    handle->fd = fd;
    handle->callback = callback;
    handle->opaque = opaque;
    handle->freeCallback = freeCallback;

    const int watch = m_nextHandle++;
    m_handles.insert(watch, handle);
    updateHandle(watch, events);
    return watch;
}

void LibvirtEventLoop::updateHandle(int watch, int events)
{
    const auto it = m_handles.find(watch);
    if (it == m_handles.end()) {
        return;
    }

    Handle *handle = *it;
    handle->events = events;

    if (events & VIR_EVENT_HANDLE_READABLE) {
        if (!handle->readNotifier) {
            handle->readNotifier = new QSocketNotifier(handle->fd, QSocketNotifier::Read, this);
            connect(handle->readNotifier, &QSocketNotifier::activated, this, [this, watch](QSocketDescriptor, QSocketNotifier::Type) {
                const auto it = m_handles.constFind(watch);
                if (it == m_handles.cend()) {
                    return;
                }
                Handle *handle = *it;
                handle->callback(watch, handle->fd, VIR_EVENT_HANDLE_READABLE, handle->opaque);
            });
        }
    } else if (handle->readNotifier) {
        delete handle->readNotifier;
        handle->readNotifier = nullptr;
    }

    if (events & VIR_EVENT_HANDLE_WRITABLE) {
        if (!handle->writeNotifier) {
            handle->writeNotifier = new QSocketNotifier(handle->fd, QSocketNotifier::Write, this);
            connect(handle->writeNotifier, &QSocketNotifier::activated, this, [this, watch](QSocketDescriptor, QSocketNotifier::Type) {
                const auto it = m_handles.constFind(watch);
                if (it == m_handles.cend()) {
                    return;
                }
                Handle *handle = *it;
                handle->callback(watch, handle->fd, VIR_EVENT_HANDLE_WRITABLE, handle->opaque);
            });
        }
    } else if (handle->writeNotifier) {
        delete handle->writeNotifier;
        handle->writeNotifier = nullptr;
    }
}

int LibvirtEventLoop::removeHandle(int watch)
{
    Handle *handle = m_handles.take(watch);
    if (!handle) {
        return -1;
    }

    delete handle->readNotifier;
    delete handle->writeNotifier;
    if (handle->freeCallback) {
        handle->freeCallback(handle->opaque);
    }
    delete handle;
    return 0;
}

int LibvirtEventLoop::addTimeout(int timeout, virEventTimeoutCallback callback, void *opaque, virFreeCallback freeCallback)
{
    auto *entry = new Timeout;
    entry->callback = callback;
    entry->opaque = opaque;
    entry->freeCallback = freeCallback;
    entry->timer = new QTimer(this);
    entry->timer->setSingleShot(false);

    const int id = m_nextTimeout++;
    m_timeouts.insert(id, entry);

    connect(entry->timer, &QTimer::timeout, this, [this, id] {
        const auto it = m_timeouts.constFind(id);
        if (it == m_timeouts.cend()) {
            return;
        }
        Timeout *entry = *it;
        entry->callback(id, entry->opaque);
    });

    updateTimeout(id, timeout);
    return id;
}

void LibvirtEventLoop::updateTimeout(int timer, int timeout)
{
    const auto it = m_timeouts.find(timer);
    if (it == m_timeouts.end()) {
        return;
    }

    Timeout *entry = *it;
    if (timeout < 0) {
        entry->timer->stop();
        return;
    }

    // A zero-interval repeating timer would spin the event loop; one
    // millisecond is the closest safe approximation.
    entry->timer->setInterval(qMax(1, timeout));
    if (!entry->timer->isActive()) {
        entry->timer->start();
    }
}

int LibvirtEventLoop::removeTimeout(int timer)
{
    Timeout *entry = m_timeouts.take(timer);
    if (!entry) {
        return -1;
    }

    entry->timer->stop();
    entry->timer->deleteLater();
    if (entry->freeCallback) {
        entry->freeCallback(entry->opaque);
    }
    delete entry;
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
