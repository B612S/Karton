// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "util/commandrunner.h"

#include <QProcess>
#include <QTimer>

#include "karton_debug.h"

void CommandRunner::runCommand(const QString &program, const QStringList &arguments, CommandCallback callback)
{
    qCDebug(KARTON_DEBUG) << "Running command:" << program << arguments;

    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [process, callback](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString output = QString::fromLocal8Bit(process->readAllStandardOutput());
        const QString error = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
        const bool success = exitStatus == QProcess::NormalExit && exitCode == 0;

        if (!success && !error.isEmpty()) {
            qCWarning(KARTON_DEBUG) << "Command failed:" << process->program() << error;
        }

        process->deleteLater();
        callback(success, success ? output : (error.isEmpty() ? output : error));
    });

    process->start(program, arguments);
    if (!process->waitForStarted()) {
        const QString error = process->errorString();
        qCWarning(KARTON_DEBUG) << "Failed to start command:" << program << error;
        process->deleteLater();

        // Report the failure after the caller has returned and had a chance to
        // set up its own pending state.
        QTimer::singleShot(0, this, [callback, error] {
            callback(false, error);
        });
    }
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
