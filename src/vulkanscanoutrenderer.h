// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#pragma once

#include "spicedisplayrenderer.h"

#include <QImage>
#include <QMutex>
#include <QSize>

#include <array>
#include <vector>

#include <vulkan/vulkan.h>

class QSGSimpleTextureNode;
class QQuickWindow;
class QVulkanDeviceFunctions;

// Hardware-accelerated renderer that imports the SPICE GL scanout DMA-BUF into
// a Vulkan image instead of relying on the OpenGL/EGL renderer.
class VulkanScanoutRenderer : public SpiceDisplayRenderer
{
    Q_OBJECT

public:
    using SpiceDisplayRenderer::SpiceDisplayRenderer;
    ~VulkanScanoutRenderer() override;

    void attach(SpiceDisplayChannel *channel) override;
    void detach() override;

    QSGNode *updatePaintNode(QQuickWindow *window, QSGNode *oldNode, const QRectF &bounds) override;

    QSize frameSize() const override
    {
        return m_size;
    }

private:
    struct Scanout {
        std::array<int, 4> fds = {-1, -1, -1, -1};
        std::array<quint32, 4> offsets = {};
        std::array<quint32, 4> strides = {};
        int sourceFd = -1;
        quint32 width = 0;
        quint32 height = 0;
        quint32 format = 0;
        quint64 modifier = 0;
        quint32 numPlanes = 0;
    };

    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        QSize size;
        quint32 format = 0;
        quint64 modifier = 0;
        int sourceFd = -1;
        QImage cpuImage;
    };

    static void glDrawCallback(SpiceDisplayChannel *channel, guint x, guint y, guint width, guint height, gpointer userData);

    void queueScanout(const SpiceGlScanout2 *scanout);
    void clearPending();
    static void closeScanout(Scanout &scanout);
    static bool matches(const Scanout &scanout, const Image &image);

    bool importScanout(QQuickWindow *window, const Scanout &scanout, Image &image);
    void reportError(const QString &message);

    SpiceDisplayChannel *m_channel = nullptr;
    gulong m_glDrawHandlerId = 0;

    QMutex m_pendingMutex;
    Scanout m_pending;
    bool m_hasPending = false;

    QMutex m_imagesMutex;
    std::vector<Image> m_images;
    Image m_current;
    bool m_hasCurrent = false;
    VkImage m_textureImage = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions *m_functions = nullptr;

    QSize m_size;
    bool m_errorReported = false;
};
