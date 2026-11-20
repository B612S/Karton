// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#pragma once

#include <QElapsedTimer>
#include <QLoggingCategory>

#include "karton_debug.h"

// Measures an operation that runs on the GUI thread and logs it when it blocks
// for too long. Attach one to every libvirt, process or filesystem call that is
// reachable from QML or from a timer: these warnings are how a freeze in the
// field is traced back to the call that caused it.
class ScopedCallTimer
{
public:
    explicit ScopedCallTimer(const char *label, qint64 warnAfterMs = 100)
        : m_label(label)
        , m_warnAfterMs(warnAfterMs)
    {
        m_timer.start();
    }

    ~ScopedCallTimer()
    {
        const qint64 elapsed = m_timer.elapsed();
        if (elapsed >= m_warnAfterMs) {
            qCWarning(KARTON_DEBUG) << "Slow GUI call:" << m_label << elapsed << "ms";
        } else {
            qCDebug(KARTON_DEBUG) << "GUI call:" << m_label << elapsed << "ms";
        }
    }

    Q_DISABLE_COPY_MOVE(ScopedCallTimer)

private:
    const char *m_label;
    qint64 m_warnAfterMs;
    QElapsedTimer m_timer;
};
