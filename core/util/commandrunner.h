// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class CommandRunner : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    using CommandCallback = std::function<void(bool success, const QString &output)>;

    // Starts a command without blocking. The callback runs on the object's
    // thread with the exit status and stdout (or stderr on failure).
    void runCommand(const QString &program, const QStringList &arguments, const CommandCallback &callback);

    // Runs a command and waits for it to finish. Returns true only when the
    // process started and exited with code 0.
    bool runCommandAndWait(const QString &program, const QStringList &arguments, QString *output = nullptr);
};
