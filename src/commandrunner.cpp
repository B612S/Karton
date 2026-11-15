// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "commandrunner.h"

#include <QProcess>

#include "karton_debug.h"

bool CommandRunner::runCommand(const QString &program, const QStringList &arguments)
{
    qCDebug(KARTON_DEBUG) << "Running command:" << program << arguments;

    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString output = QString::fromLocal8Bit(process->readAllStandardOutput());
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            const QString error = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
            if (!error.isEmpty()) {
                qCWarning(KARTON_DEBUG) << "Command failed:" << process->program() << error;
            }
        }
        process->deleteLater();
        Q_EMIT commandFinished(exitCode, output);
    });

    process->start(program, arguments);
    if (!process->waitForStarted()) {
        qCWarning(KARTON_DEBUG) << "Failed to start command:" << program;
        process->deleteLater();
        return false;
    }
    return true;
}

bool CommandRunner::runCommandAndWait(const QString &program, const QStringList &arguments, QString *output)
{
    qCDebug(KARTON_DEBUG) << "Running command (blocking):" << program << arguments;

    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted() || !process.waitForFinished(-1)) {
        qCWarning(KARTON_DEBUG) << "Failed to run command:" << program;
        return false;
    }

    if (output) {
        *output = QString::fromLocal8Bit(process.readAllStandardOutput());
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        qCWarning(KARTON_DEBUG) << "Command" << program << "failed with exit code" << process.exitCode() << error;
        return false;
    }

    return true;
}
