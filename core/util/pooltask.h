// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#pragma once

#include <QRunnable>

#include <functional>
#include <utility>

// Runs a callable on QThreadPool. The project builds with -fno-exceptions, so a
// plain QRunnable is used instead of QtConcurrent or std::thread.
class PoolTask : public QRunnable
{
public:
    explicit PoolTask(std::function<void()> function)
        : m_function(std::move(function))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        m_function();
    }

private:
    std::function<void()> m_function;
};
