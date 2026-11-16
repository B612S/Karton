// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>

#include <memory>

// Small PipeWire playback sink for the interleaved S16 audio sent by SPICE.
// The stream is created on a PipeWire thread loop; SPICE callbacks append PCM
// data from the GUI thread and the process callback drains it into PipeWire.
class PipeWireAudioSink : public QObject
{
    Q_OBJECT

public:
    explicit PipeWireAudioSink(QObject *parent = nullptr);
    ~PipeWireAudioSink() override;

    bool start(int sampleRate, int channels);
    void write(const char *data, qint64 size);
    void stop();

    bool isRunning() const;

    struct Private;

private:
    std::unique_ptr<Private> d;
};
