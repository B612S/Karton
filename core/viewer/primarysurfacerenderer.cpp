// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "viewer/primarysurfacerenderer.h"

#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

#include "karton_debug.h"

void PrimarySurfaceRenderer::attach(SpiceDisplayChannel *channel)
{
    detach();
    m_channel = channel;
    m_primaryCreateHandlerId = g_signal_connect(channel, "display-primary-create", G_CALLBACK(display_primary_create_callback), this);
    m_invalidateHandlerId = g_signal_connect(channel, "display-invalidate", G_CALLBACK(display_invalidate_callback), this);
    m_primaryDestroyHandlerId = g_signal_connect(channel, "display-primary-destroy", G_CALLBACK(display_primary_destroy_callback), this);
}

void PrimarySurfaceRenderer::detach()
{
    if (m_channel) {
        if (m_primaryCreateHandlerId) {
            g_signal_handler_disconnect(m_channel, m_primaryCreateHandlerId);
        }
        if (m_invalidateHandlerId) {
            g_signal_handler_disconnect(m_channel, m_invalidateHandlerId);
        }
        if (m_primaryDestroyHandlerId) {
            g_signal_handler_disconnect(m_channel, m_primaryDestroyHandlerId);
        }
    }
    m_primaryCreateHandlerId = 0;
    m_invalidateHandlerId = 0;
    m_primaryDestroyHandlerId = 0;
    m_channel = nullptr;

    QMutexLocker locker(&m_frameLock);
    m_frameSource = nullptr;
    m_frameStride = 0;
    m_frame = QImage();
    m_frameUpdated = false;
}

void PrimarySurfaceRenderer::display_primary_create_callback(SpiceChannel *channel,
                                                             gint format,
                                                             gint width,
                                                             gint height,
                                                             gint stride,
                                                             gint shmid,
                                                             gpointer imgdata,
                                                             gpointer user_data)
{
    Q_UNUSED(channel);
    Q_UNUSED(format);
    Q_UNUSED(shmid);

    auto *self = static_cast<PrimarySurfaceRenderer *>(user_data);
    qCInfo(KARTON_DEBUG) << "SPICE: primary surface received! size:" << width << "x" << height;
    qCInfo(KARTON_DEBUG) << "SPICE: format is:" << format;

    {
        QMutexLocker locker(&self->m_frameLock);
        self->m_frameSource = static_cast<const uchar *>(imgdata);
        self->m_frameStride = stride;
        self->m_imageWidth = width;
        self->m_imageHeight = height;
        // Copy out of SPICE's framebuffer. The original memory is owned by
        // SPICE and can be unmapped while the render thread is still creating
        // a texture from it (Windows resizes the primary surface while
        // booting), which used to crash QSGBatchRenderer.
        self->m_frame = QImage(width, height, QImage::Format_RGB32);
        if (!self->m_frame.isNull() && self->m_frameSource) {
            const int bytesPerLine = qMin(stride, self->m_frame.bytesPerLine());
            for (int y = 0; y < height; ++y) {
                memcpy(self->m_frame.scanLine(y), self->m_frameSource + y * stride, bytesPerLine);
            }
        }
        self->m_frameUpdated = true;
    }

    Q_EMIT self->frameSizeChanged();
    Q_EMIT self->frameReady();
}

void PrimarySurfaceRenderer::display_invalidate_callback(SpiceDisplayChannel *channel, gint x, gint y, gint width, gint height, gpointer user_data)
{
    Q_UNUSED(channel);

    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(width);
    Q_UNUSED(height);

    auto *self = static_cast<PrimarySurfaceRenderer *>(user_data);
    {
        QMutexLocker locker(&self->m_frameLock);
        if (self->m_frameSource && !self->m_frame.isNull()) {
            const int left = qBound(0, x, self->m_frame.width());
            const int top = qBound(0, y, self->m_frame.height());
            const int right = qBound(0, x + width, self->m_frame.width());
            const int bottom = qBound(0, y + height, self->m_frame.height());
            const int bytes = (right - left) * 4;
            if (bytes > 0) {
                for (int row = top; row < bottom; ++row) {
                    memcpy(self->m_frame.scanLine(row) + left * 4, self->m_frameSource + row * self->m_frameStride + left * 4, bytes);
                }
            }
        }
        self->m_frameUpdated = true;
    }

    Q_EMIT self->frameReady();
}

void PrimarySurfaceRenderer::display_primary_destroy_callback(SpiceDisplayChannel *channel, gpointer user_data)
{
    Q_UNUSED(channel);
    auto *self = static_cast<PrimarySurfaceRenderer *>(user_data);
    QMutexLocker locker(&self->m_frameLock);
    self->m_frameSource = nullptr;
    self->m_frameStride = 0;
    self->m_frame = QImage();
    self->m_frameUpdated = false;
}

QImage PrimarySurfaceRenderer::currentFrame() const
{
    QMutexLocker locker(&m_frameLock);
    return m_frame.copy();
}

QSGNode *PrimarySurfaceRenderer::updatePaintNode(QQuickWindow *window, QSGNode *oldNode, const QRectF &bounds)
{
    QMutexLocker locker(&m_frameLock);

    if (m_frame.isNull() || m_frame.width() <= 0 || m_frame.height() <= 0) {
        delete oldNode;
        return nullptr;
    }

    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
    }

    node->setRect(bounds);
    node->setFiltering(QSGTexture::Linear);

    // Keep the existing texture when the frame did not change; deleting the
    // node here used to make the VM display go black on unrelated repaints.
    if (!m_frameUpdated && node->texture()) {
        return node;
    }

    QSGTexture *texture = window->createTextureFromImage(m_frame, QQuickWindow::TextureIsOpaque);
    if (texture) {
        node->setTexture(texture);
        m_frameUpdated = false;
    }

    return node;
}
