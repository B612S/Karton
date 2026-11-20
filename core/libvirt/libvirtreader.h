// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#pragma once

#include <QObject>

// Lives in the libvirt reader thread and has no libvirt connection of its own:
// Karton opens one connection on the GUI thread, posts blocking read calls here
// through QMetaObject::invokeMethod(), and closes the connection on the GUI
// thread once no read is in flight. The thread only needs an event loop to
// receive those queued calls.
class LibvirtReader : public QObject
{
    Q_OBJECT
};
