// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "util/commandrunner.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <memory>

#include "karton_debug.h"

CommandRunner::CommandId CommandRunner::runCommand(const QString &program, const QStringList &arguments, const CommandCallback &callback, int timeoutMs)
{
    qCDebug(KARTON_DEBUG) << "Running command:" << program << arguments;

    if (qEnvironmentVariableIsSet("KARTON_TIMING")) {
        qWarning("KARTON_TIMING runner start");
    }
    auto *process = new QProcess(this);
    const CommandId id = m_nextCommandId++;
    m_processes.insert(id, process);
    auto reported = std::make_shared<bool>(false);
    connect(process, &QProcess::errorOccurred, this, [this, id, process, callback, reported](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || *reported) {
            return;
        }
        *reported = true;
        m_processes.remove(id);
        m_cancelled.remove(id);
        const QString errorString = process->errorString();
        process->deleteLater();
        QTimer::singleShot(0, this, [callback, errorString] {
            callback(false, errorString);
        });
    });
    connect(process, &QProcess::finished, this, [this, id, process, callback, reported](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString output = QString::fromLocal8Bit(process->readAllStandardOutput());
        const QString error = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
        const bool wasCancelled = m_cancelled.remove(id);
        m_processes.remove(id);
        const bool success = !wasCancelled && exitStatus == QProcess::NormalExit && exitCode == 0;

        if (!success && !error.isEmpty() && !wasCancelled) {
            qCWarning(KARTON_DEBUG) << "Command failed:" << process->program() << error;
        }

        process->deleteLater();
        if (*reported) {
            return;
        }
        *reported = true;
        if (wasCancelled) {
            callback(false, QCoreApplication::translate("CommandRunner", "command_cancelled"));
            return;
        }
        callback(success, success ? output : (error.isEmpty() ? output : error));
    });

    if (timeoutMs > 0) {
        auto *timer = new QTimer(process);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, id, process, callback, reported, timeoutMs] {
            if (*reported) {
                return;
            }
            *reported = true;
            m_processes.remove(id);
            m_cancelled.remove(id);
            // A stuck child (hung network filesystem, dead NFS mount) must not
            // keep a dialog waiting forever. Kill it and report the timeout
            // immediately; `finished` still arrives to reap the process.
            process->kill();
            qCWarning(KARTON_DEBUG) << "Command timed out after" << timeoutMs << "ms:" << process->program();
            const QString message = QCoreApplication::translate("CommandRunner", "command_timed_out_after_1_s").arg(qMax(1, timeoutMs / 1000));
            QTimer::singleShot(0, this, [callback, message] {
                callback(false, message);
            });
        });
        timer->start(timeoutMs);
    }

    // QProcess::start() is asynchronous. Do not call waitForStarted() here:
    // it blocks the GUI thread while the child process is forked/executed,
    // which froze the window for several seconds on slower systems.
    process->start(program, arguments);
    if (qEnvironmentVariableIsSet("KARTON_TIMING")) {
        qWarning("KARTON_TIMING runner start returned");
    }
    return id;
}

void CommandRunner::cancelCommand(CommandId id)
{
    QProcess *process = m_processes.value(id, nullptr);
    if (!process) {
        return;
    }
    m_cancelled.insert(id);
    process->kill();
}

bool CommandRunner::runCommandAndWait(const QString &program, const QStringList &arguments, QString *output, int timeoutMs)
{
    qCDebug(KARTON_DEBUG) << "Running command (blocking):" << program << arguments;

    QProcess process;
    process.start(program, arguments);
    const int startTimeout = timeoutMs > 0 ? timeoutMs : 30000;
    if (!process.waitForStarted(startTimeout)) {
        qCWarning(KARTON_DEBUG) << "Failed to run command:" << program;
        return false;
    }
    const bool finished = timeoutMs > 0 ? process.waitForFinished(timeoutMs) : process.waitForFinished(-1);
    if (!finished) {
        process.kill();
        process.waitForFinished(2000);
        const QString message = QCoreApplication::translate("CommandRunner", "command_timed_out_after_1_s").arg(qMax(1, timeoutMs / 1000));
        qCWarning(KARTON_DEBUG) << "Command timed out:" << program << arguments;
        if (output) {
            *output = message;
        }
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
