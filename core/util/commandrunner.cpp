// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "util/commandrunner.h"

#include <QProcess>
#include <QTimer>
#include <memory>

#include "karton_debug.h"

void CommandRunner::runCommand(const QString &program, const QStringList &arguments, const CommandCallback &callback)
{
    qCDebug(KARTON_DEBUG) << "Running command:" << program << arguments;

    if (qEnvironmentVariableIsSet("KARTON_TIMING")) {
        qWarning("KARTON_TIMING runner start");
    }
    auto *process = new QProcess(this);
    auto reported = std::make_shared<bool>(false);
    connect(process, &QProcess::errorOccurred, this, [this, process, callback, reported](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || *reported) {
            return;
        }
        *reported = true;
        const QString errorString = process->errorString();
        process->deleteLater();
        QTimer::singleShot(0, this, [callback, errorString] {
            callback(false, errorString);
        });
    });
    connect(process, &QProcess::finished, this, [process, callback, reported](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString output = QString::fromLocal8Bit(process->readAllStandardOutput());
        const QString error = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
        const bool success = exitStatus == QProcess::NormalExit && exitCode == 0;

        if (!success && !error.isEmpty()) {
            qCWarning(KARTON_DEBUG) << "Command failed:" << process->program() << error;
        }

        process->deleteLater();
        if (*reported) {
            return;
        }
        *reported = true;
        callback(success, success ? output : (error.isEmpty() ? output : error));
    });

    // QProcess::start() is asynchronous. Do not call waitForStarted() here:
    // it blocks the GUI thread while the child process is forked/executed,
    // which froze the window for several seconds on slower systems.
    process->start(program, arguments);
    if (qEnvironmentVariableIsSet("KARTON_TIMING")) {
        qWarning("KARTON_TIMING runner start returned");
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
