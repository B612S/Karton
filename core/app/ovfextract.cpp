// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#include "app/ovfextract.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStringList>
#include <QUuid>

namespace
{
// Runs tar and polls the cancel flag, so extracting a multi-gigabyte archive
// can be stopped without freezing the GUI or leaving a child process behind.
bool runTar(const QStringList &arguments,
            const std::atomic<bool> &cancelRequested,
            QByteArray *output,
            QByteArray *errorOutput,
            bool *cancelled,
            const OvfProgressCallback &progress = {})
{
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // The listing size column and the checkpoint messages are parsed below;
    // keep tar's output in the POSIX locale regardless of the user's locale.
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral("tar"), arguments);
    if (!process.waitForStarted(10000)) {
        *errorOutput = process.errorString().toUtf8();
        return false;
    }

    while (process.state() != QProcess::NotRunning) {
        if (cancelRequested.load()) {
            process.kill();
            process.waitForFinished(2000);
            *cancelled = true;
            return false;
        }
        // Read both channels while tar runs: a large `-tf` listing fills the
        // pipe, and the child would block forever if nobody drained it.
        process.waitForReadyRead(50);
        output->append(process.readAllStandardOutput());
        errorOutput->append(process.readAllStandardError());

        if (progress) {
            // GNU tar prints "R: <bytes>" for --checkpoint-action=echo=%T.
            static const QRegularExpression progressExpression(QStringLiteral("R: (\\d+)"));
            auto matches = progressExpression.globalMatch(QString::fromUtf8(*errorOutput));
            qint64 processed = -1;
            while (matches.hasNext()) {
                processed = matches.next().captured(1).toLongLong();
            }
            if (processed >= 0) {
                progress(processed, 0);
            }
        }
    }
    output->append(process.readAllStandardOutput());
    errorOutput->append(process.readAllStandardError());
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
} // namespace

OvfExtractionResult extractOvaArchive(const QString &path, const std::atomic<bool> &cancelRequested, const OvfProgressCallback &progress)
{
    OvfExtractionResult result;
    if (cancelRequested.load()) {
        result.cancelled = true;
        result.error = QStringLiteral("cancelled");
        return result;
    }
    QByteArray output;
    QByteArray errorOutput;
    bool wasCancelled = false;

    if (!runTar({QStringLiteral("-tf"), path}, cancelRequested, &output, &errorOutput, &wasCancelled)) {
        if (wasCancelled) {
            result.cancelled = true;
            result.error = QStringLiteral("cancelled");
            return result;
        }
        result.error = QString::fromUtf8(errorOutput).trimmed();
        if (result.error.isEmpty()) {
            result.error = QStringLiteral("tar -tf failed");
        }
        return result;
    }

    const QStringList entries = QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const QString simplified = QDir::cleanPath(entry);
        if (entry.startsWith(QLatin1Char('/')) || simplified.startsWith(QStringLiteral("../")) || simplified == QStringLiteral("..")
            || entry.contains(QLatin1Char('\\'))) {
            result.error = QStringLiteral("The OVA archive contains an unsafe path: %1").arg(entry);
            return result;
        }
    }

    // Reject links and device nodes: a symlink inside the archive could make
    // a later entry escape the temporary directory.
    output.clear();
    errorOutput.clear();
    if (!runTar({QStringLiteral("-tvf"), path}, cancelRequested, &output, &errorOutput, &wasCancelled)) {
        if (wasCancelled) {
            result.cancelled = true;
            result.error = QStringLiteral("cancelled");
            return result;
        }
        result.error = QStringLiteral("Could not inspect the OVA archive");
        return result;
    }
    const QStringList typeLines = QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    qint64 totalBytes = 0;
    static const QRegularExpression sizeExpression(QStringLiteral("^\\S+\\s+\\S+\\s+(\\d+)\\s"));
    for (const QString &line : typeLines) {
        const auto sizeMatch = sizeExpression.match(line);
        if (sizeMatch.hasMatch()) {
            totalBytes += sizeMatch.captured(1).toLongLong();
        }
        if (line.isEmpty()) {
            continue;
        }
        const QChar type = line.at(0);
        if (type != QLatin1Char('-') && type != QLatin1Char('d')) {
            result.error = QStringLiteral("The OVA archive contains a link or a special file");
            return result;
        }
    }

    const QString directory = QDir::tempPath() + QStringLiteral("/karton-ovf-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(directory)) {
        result.error = QStringLiteral("Could not create the temporary directory");
        return result;
    }

    output.clear();
    errorOutput.clear();
    const OvfProgressCallback progressCallback = progress ? OvfProgressCallback([progress, totalBytes](qint64 processed, qint64) {
        progress(processed, totalBytes);
    })
                                                          : OvfProgressCallback();
    if (!runTar({QStringLiteral("--no-same-owner"),
                 QStringLiteral("-xf"),
                 path,
                 QStringLiteral("-C"),
                 directory,
                 QStringLiteral("--checkpoint=100"),
                 QStringLiteral("--checkpoint-action=echo=%T")},
                cancelRequested,
                &output,
                &errorOutput,
                &wasCancelled,
                progressCallback)) {
        QDir(directory).removeRecursively();
        if (wasCancelled) {
            result.cancelled = true;
            result.error = QStringLiteral("cancelled");
            return result;
        }
        result.error = QString::fromUtf8(errorOutput).trimmed();
        return result;
    }

    QDirIterator iterator(directory, {QStringLiteral("*.ovf")}, QDir::Files, QDirIterator::Subdirectories);
    if (!iterator.hasNext()) {
        result.error = QStringLiteral("The OVA archive does not contain an OVF descriptor");
        QDir(directory).removeRecursively();
        return result;
    }

    result.success = true;
    result.ovfPath = iterator.next();
    result.temporaryDirectory = directory;
    return result;
}
