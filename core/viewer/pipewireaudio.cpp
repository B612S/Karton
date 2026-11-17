// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "viewer/pipewireaudio.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <QMutexLocker>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "karton_debug.h"

struct PipeWireAudioSink::Private {
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook streamListener = {};
    pw_stream_events events = {};
    QByteArray queue;
    QMutex queueMutex;
    int sampleRate = 0;
    int channels = 0;
    std::atomic<bool> running = false;
    std::atomic<bool> stopping = false;
    bool loopStarted = false;
};

namespace
{
void onProcess(void *userData)
{
    auto *sink = static_cast<PipeWireAudioSink::Private *>(userData);
    if (sink->stopping) {
        return;
    }
    pw_buffer *buffer = pw_stream_dequeue_buffer(sink->stream);
    if (!buffer || !buffer->buffer || buffer->buffer->n_datas == 0) {
        if (buffer) {
            pw_stream_queue_buffer(sink->stream, buffer);
        }
        return;
    }

    spa_data &data = buffer->buffer->datas[0];
    if (!data.chunk) {
        pw_stream_queue_buffer(sink->stream, buffer);
        return;
    }
    auto *destination = static_cast<uchar *>(data.data);
    if (!destination || data.maxsize == 0) {
        pw_stream_queue_buffer(sink->stream, buffer);
        return;
    }

    qsizetype copied = 0;
    {
        QMutexLocker locker(&sink->queueMutex);
        copied = std::min<qsizetype>(sink->queue.size(), static_cast<qsizetype>(data.maxsize));
        if (copied > 0) {
            std::memcpy(destination, sink->queue.constData(), static_cast<size_t>(copied));
            sink->queue.remove(0, copied);
        }
    }
    if (copied < static_cast<qsizetype>(data.maxsize)) {
        std::memset(destination + copied, 0, static_cast<size_t>(data.maxsize) - static_cast<size_t>(copied));
    }
    data.chunk->offset = 0;
    data.chunk->stride = sink->channels * static_cast<int>(sizeof(qint16));
    data.chunk->size = static_cast<quint32>(data.maxsize);
    pw_stream_queue_buffer(sink->stream, buffer);
}

void onStateChanged(void *userData, pw_stream_state oldState, pw_stream_state state, const char *error)
{
    Q_UNUSED(userData);
    Q_UNUSED(oldState);
    if (state == PW_STREAM_STATE_ERROR) {
        qCWarning(KARTON_DEBUG) << "PipeWire audio stream error:" << (error ? error : "unknown");
    } else {
        qCDebug(KARTON_DEBUG) << "PipeWire audio stream state:" << pw_stream_state_as_string(state);
    }
}
} // namespace

PipeWireAudioSink::PipeWireAudioSink(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
}

PipeWireAudioSink::~PipeWireAudioSink()
{
    stop();
}

bool PipeWireAudioSink::start(int sampleRate, int channels)
{
    stop();
    if (sampleRate <= 0 || channels <= 0) {
        return false;
    }

    pw_init(nullptr, nullptr);
    d->sampleRate = sampleRate;
    d->channels = channels;
    d->stopping = false;
    d->loop = pw_thread_loop_new("karton-audio", nullptr);
    if (!d->loop) {
        return false;
    }
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (!d->context) {
        stop();
        return false;
    }
    d->core = pw_context_connect(d->context, nullptr, 0);
    if (!d->core) {
        stop();
        return false;
    }

    pw_properties *properties =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Movie", PW_KEY_NODE_NAME, "karton-audio", nullptr);
    d->stream = pw_stream_new(d->core, "karton-audio", properties);
    if (!d->stream) {
        stop();
        return false;
    }

    spa_audio_info_raw audioInfo = {};
    audioInfo.format = SPA_AUDIO_FORMAT_S16;
    audioInfo.rate = static_cast<uint32_t>(sampleRate);
    audioInfo.channels = static_cast<uint32_t>(channels);
    if (channels > 0) {
        audioInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
    }
    if (channels > 1) {
        audioInfo.position[1] = SPA_AUDIO_CHANNEL_FR;
    }

    uint8_t podBuffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audioInfo)};

    d->events.version = PW_VERSION_STREAM_EVENTS;
    d->events.state_changed = onStateChanged;
    d->events.process = onProcess;
    pw_stream_add_listener(d->stream, &d->streamListener, &d->events, d.get());

    if (pw_thread_loop_start(d->loop) != 0) {
        stop();
        return false;
    }
    d->loopStarted = true;
    pw_thread_loop_lock(d->loop);
    const int result = pw_stream_connect(d->stream,
                                         PW_DIRECTION_OUTPUT,
                                         PW_ID_ANY,
                                         static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                                         params,
                                         1);
    pw_thread_loop_unlock(d->loop);
    if (result < 0) {
        stop();
        return false;
    }

    d->running = true;
    qCInfo(KARTON_DEBUG) << "PipeWire audio started:" << sampleRate << "Hz," << channels << "channels";
    return true;
}

void PipeWireAudioSink::write(const char *data, qint64 size)
{
    if (!d || !d->running || !data || size <= 0) {
        return;
    }
    {
        QMutexLocker locker(&d->queueMutex);
        d->queue.append(data, static_cast<int>(size));
    }
    if (d->stream) {
        pw_stream_trigger_process(d->stream);
    }
}

void PipeWireAudioSink::stop()
{
    if (!d) {
        return;
    }
    d->stopping = true;
    d->running = false;
    if (d->loop && d->loopStarted) {
        pw_thread_loop_stop(d->loop);
        d->loopStarted = false;
    }
    if (d->stream) {
        pw_stream_destroy(d->stream);
        d->stream = nullptr;
    }
    if (d->core) {
        pw_core_disconnect(d->core);
        d->core = nullptr;
    }
    if (d->context) {
        pw_context_destroy(d->context);
        d->context = nullptr;
    }
    if (d->loop) {
        pw_thread_loop_destroy(d->loop);
        d->loop = nullptr;
    }
    QMutexLocker locker(&d->queueMutex);
    d->queue.clear();
}

bool PipeWireAudioSink::isRunning() const
{
    return d && d->running;
}
