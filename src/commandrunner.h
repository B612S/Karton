// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class CommandRunner : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // Starts a command without blocking. commandFinished() is emitted when it
    // exits and reports whether the process could be started.
    bool runCommand(const QString &program, const QStringList &arguments);

    // Runs a command and waits for it to finish. Returns true only when the
    // process started and exited with code 0.
    bool runCommandAndWait(const QString &program, const QStringList &arguments, QString *output = nullptr);

Q_SIGNALS:
    void commandFinished(int exitCode, const QString &output);
};
