// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#pragma once

#include <atomic>
#include <functional>

#include <QString>

struct OvfExtractionResult {
    bool success = false;
    bool cancelled = false;
    QString ovfPath;
    QString temporaryDirectory;
    QString error;
};

// Called from the extraction worker with the number of archive bytes read so
// far and the total uncompressed entry size. `totalBytes` is 0 when the
// archive listing did not expose sizes; the UI then stays indeterminate.
using OvfProgressCallback = std::function<void(qint64 processedBytes, qint64 totalBytes)>;

// Extracts an OVA archive into a temporary directory and validates its paths
// and entry types. Safe to call from a worker thread: it only touches its own
// QProcess objects and polls `cancelRequested` (killing tar when it is set).
OvfExtractionResult extractOvaArchive(const QString &path, const std::atomic<bool> &cancelRequested, const OvfProgressCallback &progress = {});
