// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;

class CommandRunner : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    using CommandCallback = std::function<void(bool success, const QString &output)>;
    using CommandId = quintptr;

    // Starts a command without blocking. The callback runs on the object's
    // thread with the exit status and stdout (or stderr on failure).
    // A positive timeoutMs kills the process and reports a failure; 0 waits
    // without a limit (long conversions, image compaction).
    // Returns an id that cancelCommand() accepts.
    CommandId runCommand(const QString &program, const QStringList &arguments, const CommandCallback &callback, int timeoutMs = 0);

    // Kills a running command started by runCommand(). Its callback is invoked
    // with success == false and a "cancelled" message, so a long operation can
    // be aborted without freezing the GUI.
    void cancelCommand(CommandId id);

    // Runs a command and waits for it to finish. Returns true only when the
    // process started and exited with code 0. timeoutMs has the same meaning
    // as in runCommand().
    bool runCommandAndWait(const QString &program, const QStringList &arguments, QString *output = nullptr, int timeoutMs = 0);

private:
    QHash<CommandId, QProcess *> m_processes;
    QSet<CommandId> m_cancelled;
    CommandId m_nextCommandId = 1;
};
