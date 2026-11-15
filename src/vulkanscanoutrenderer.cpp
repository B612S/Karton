// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#include "vulkanscanoutrenderer.h"

#include <KLocalizedString>

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QTimer>
#include <QVulkanFunctions>
#include <QVulkanInstance>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <unistd.h>

#include "karton_debug.h"

namespace
{
constexpr quint32 fourcc(char a, char b, char c, char d)
{
    return quint32(quint8(a)) | (quint32(quint8(b)) << 8) | (quint32(quint8(c)) << 16) | (quint32(quint8(d)) << 24);
}

bool toVulkanFormat(quint32 format, VkFormat *vkFormat, QRhiTexture::Format *rhiFormat)
{
    constexpr quint32 argb8888 = fourcc('A', 'R', '2', '4');
    constexpr quint32 xrgb8888 = fourcc('X', 'R', '2', '4');
    constexpr quint32 abgr8888 = fourcc('A', 'B', '2', '4');
    constexpr quint32 xbgr8888 = fourcc('X', 'B', '2', '4');
    constexpr quint32 rgb565 = fourcc('R', 'G', '1', '6');

    switch (format) {
    case argb8888:
    case xrgb8888:
        *vkFormat = VK_FORMAT_B8G8R8A8_UNORM;
        *rhiFormat = QRhiTexture::BGRA8;
        return true;
    case abgr8888:
    case xbgr8888:
        *vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
        *rhiFormat = QRhiTexture::RGBA8;
        return true;
    case rgb565:
        *vkFormat = VK_FORMAT_R5G6B5_UNORM_PACK16;
        *rhiFormat = QRhiTexture::UnknownFormat;
        return true;
    default:
        return false;
    }
}

bool copyImageToBuffer(VkDevice device,
                       QVulkanDeviceFunctions *functions,
                       VkQueue queue,
                       quint32 queueFamily,
                       VkImage image,
                       VkBuffer buffer,
                       quint32 width,
                       quint32 height)
{
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queueFamily;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (functions->vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (functions->vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
        functions->vkDestroyCommandPool(device, pool, nullptr);
        return false;
    }

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (functions->vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        functions->vkDestroyCommandPool(device, pool, nullptr);
        return false;
    }

    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};

    functions->vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

    if (functions->vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        functions->vkDestroyCommandPool(device, pool, nullptr);
        return false;
    }

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    const bool ok = functions->vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS && functions->vkQueueWaitIdle(queue) == VK_SUCCESS;
    functions->vkDestroyCommandPool(device, pool, nullptr);
    return ok;
}

bool transitionImage(VkDevice device,
                     QVulkanDeviceFunctions *functions,
                     VkQueue queue,
                     quint32 queueFamily,
                     VkImage image,
                     VkImageLayout oldLayout,
                     VkImageLayout newLayout,
                     VkPipelineStageFlags srcStage,
                     VkPipelineStageFlags dstStage,
                     VkAccessFlags dstAccess)
{
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queueFamily;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (functions->vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (functions->vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
        functions->vkDestroyCommandPool(device, pool, nullptr);
        return false;
    }

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (functions->vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        functions->vkDestroyCommandPool(device, pool, nullptr);
        return false;
    }

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = dstAccess;

    functions->vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    if (functions->vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        functions->vkDestroyCommandPool(device, pool, nullptr);
        return false;
    }

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    const bool ok = functions->vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS && functions->vkQueueWaitIdle(queue) == VK_SUCCESS;
    functions->vkDestroyCommandPool(device, pool, nullptr);
    return ok;
}
} // namespace

VulkanScanoutRenderer::~VulkanScanoutRenderer()
{
    detach();
}

void VulkanScanoutRenderer::attach(SpiceDisplayChannel *channel)
{
    detach();
    m_channel = channel;
    m_errorReported = false;
    m_glDrawHandlerId = g_signal_connect(channel, "gl-draw", G_CALLBACK(glDrawCallback), this);
}

void VulkanScanoutRenderer::detach()
{
    if (m_channel && m_glDrawHandlerId) {
        g_signal_handler_disconnect(m_channel, m_glDrawHandlerId);
    }
    m_glDrawHandlerId = 0;
    m_channel = nullptr;

    clearPending();

    QMutexLocker locker(&m_imagesMutex);
    if (m_device && m_functions) {
        m_functions->vkDeviceWaitIdle(m_device);
        for (const Image &image : std::as_const(m_images)) {
            if (image.memory) {
                m_functions->vkFreeMemory(m_device, image.memory, nullptr);
            }
            if (image.image) {
                m_functions->vkDestroyImage(m_device, image.image, nullptr);
            }
        }
    }
    if (m_hasCurrent && !m_current.cpuImage.isNull()) {
        if (m_device && m_functions) {
            if (m_current.memory) {
                m_functions->vkFreeMemory(m_device, m_current.memory, nullptr);
            }
            if (m_current.image) {
                m_functions->vkDestroyImage(m_device, m_current.image, nullptr);
            }
        }
    }
    m_images.clear();
    m_hasCurrent = false;
    m_current = Image();
    m_textureImage = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_functions = nullptr;
}

void VulkanScanoutRenderer::glDrawCallback(SpiceDisplayChannel *channel, guint x, guint y, guint width, guint height, gpointer userData)
{
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(width);
    Q_UNUSED(height);

    auto *self = static_cast<VulkanScanoutRenderer *>(userData);
    const SpiceGlScanout2 *scanout = spice_display_channel_get_gl_scanout2(channel);
    if (scanout) {
        self->queueScanout(scanout);
    }
    spice_display_channel_gl_draw_done(channel);
}

void VulkanScanoutRenderer::queueScanout(const SpiceGlScanout2 *scanout)
{
    Scanout pending;
    pending.width = scanout->width;
    pending.height = scanout->height;
    pending.format = scanout->format;
    pending.modifier = scanout->modifier;
    pending.numPlanes = scanout->num_planes;
    pending.sourceFd = scanout->fd[0];

    if (scanout->num_planes == 0 || scanout->num_planes > 4 || scanout->fd[0] < 0) {
        return;
    }

    for (quint32 i = 0; i < scanout->num_planes; ++i) {
        pending.fds[i] = dup(scanout->fd[i]);
        pending.offsets[i] = scanout->offset[i];
        pending.strides[i] = scanout->stride[i];
        if (pending.fds[i] < 0) {
            closeScanout(pending);
            return;
        }
    }

    {
        QMutexLocker locker(&m_pendingMutex);
        closeScanout(m_pending);
        m_pending = pending;
        m_hasPending = true;
    }

    const QSize size(static_cast<int>(scanout->width), static_cast<int>(scanout->height));
    if (size != m_size) {
        m_size = size;
        Q_EMIT frameSizeChanged();
    }
    Q_EMIT frameReady();
}

void VulkanScanoutRenderer::clearPending()
{
    QMutexLocker locker(&m_pendingMutex);
    closeScanout(m_pending);
    m_hasPending = false;
}

void VulkanScanoutRenderer::closeScanout(Scanout &scanout)
{
    for (int &fd : scanout.fds) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
    scanout.sourceFd = -1;
}

bool VulkanScanoutRenderer::matches(const Scanout &scanout, const Image &image)
{
    // Imported images are sampled in place on the GPU, but CPU-converted
    // formats keep a snapshot of the buffer content and must be refreshed.
    if (!image.cpuImage.isNull()) {
        return false;
    }
    return image.sourceFd == scanout.sourceFd && image.size.width() == static_cast<int>(scanout.width)
        && image.size.height() == static_cast<int>(scanout.height) && image.format == scanout.format && image.modifier == scanout.modifier;
}

bool VulkanScanoutRenderer::importScanout(QQuickWindow *window, const Scanout &scanout, Image &result)
{
    QVulkanDeviceFunctions *functions = m_functions;
    const VkDevice device = m_device;
    if (!functions || device == VK_NULL_HANDLE) {
        return false;
    }

    QRhi *rhi = window->rhi();
    if (!rhi || rhi->backend() != QRhi::Vulkan) {
        return false;
    }

    const auto *native = static_cast<const QRhiVulkanNativeHandles *>(rhi->nativeHandles());
    if (!native || !native->physDev || !native->gfxQueue) {
        return false;
    }

    qCDebug(KARTON_DEBUG) << "Vulkan scanout format" << Qt::hex << scanout.format << "modifier" << scanout.modifier << "planes" << scanout.numPlanes << "size"
                          << scanout.width << scanout.height;

    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    QRhiTexture::Format rhiFormat = QRhiTexture::UnknownFormat;
    if (!toVulkanFormat(scanout.format, &vkFormat, &rhiFormat)) {
        reportError(i18n("Vulkan cannot import this SPICE scanout format. Restart Karton with KARTON_GRAPHICS_API=opengl."));
        return false;
    }

    if (scanout.numPlanes != 1) {
        reportError(i18n("Vulkan cannot import multi-plane SPICE scanouts yet. Restart Karton with KARTON_GRAPHICS_API=opengl."));
        return false;
    }

    const int fd = dup(scanout.fds[0]);
    if (fd < 0) {
        return false;
    }

    VkSubresourceLayout planeLayout = {};
    planeLayout.offset = scanout.offsets[0];
    planeLayout.rowPitch = scanout.strides[0];
    planeLayout.size = 0;

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo = {};
    modifierInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifierInfo.drmFormatModifier = scanout.modifier;
    modifierInfo.drmFormatModifierPlaneCount = 1;
    modifierInfo.pPlaneLayouts = &planeLayout;

    VkExternalMemoryImageCreateInfo externalInfo = {};
    externalInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    externalInfo.pNext = &modifierInfo;

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = vkFormat;
    imageInfo.extent = {scanout.width, scanout.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (rhiFormat == QRhiTexture::UnknownFormat ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (functions->vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        close(fd);
        reportError(i18n("Vulkan could not create an image for the SPICE scanout. Restart Karton with KARTON_GRAPHICS_API=opengl."));
        return false;
    }

    VkMemoryRequirements requirements = {};
    functions->vkGetImageMemoryRequirements(device, image, &requirements);

    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    QVulkanFunctions *instanceFunctions = native->inst->functions();
    if (!instanceFunctions) {
        functions->vkDestroyImage(device, image, nullptr);
        close(fd);
        return false;
    }
    instanceFunctions->vkGetPhysicalDeviceMemoryProperties(native->physDev, &memoryProperties);

    quint32 memoryTypeIndex = UINT32_MAX;
    for (quint32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if (!(requirements.memoryTypeBits & (1u << i))) {
            continue;
        }
        if (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memoryTypeIndex = i;
            break;
        }
        if (memoryTypeIndex == UINT32_MAX) {
            memoryTypeIndex = i;
        }
    }

    if (memoryTypeIndex == UINT32_MAX) {
        functions->vkDestroyImage(device, image, nullptr);
        close(fd);
        return false;
    }

    VkImportMemoryFdInfoKHR importInfo = {};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importInfo.fd = fd;

    VkMemoryDedicatedAllocateInfo dedicatedInfo = {};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.image = image;

    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryTypeIndex;
    dedicatedInfo.pNext = &importInfo;
    allocateInfo.pNext = &dedicatedInfo;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (functions->vkAllocateMemory(device, &allocateInfo, nullptr, &memory) != VK_SUCCESS) {
        functions->vkDestroyImage(device, image, nullptr);
        close(fd);
        return false;
    }

    if (functions->vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        functions->vkFreeMemory(device, memory, nullptr);
        functions->vkDestroyImage(device, image, nullptr);
        return false;
    }

    const bool cpuConverted = rhiFormat == QRhiTexture::UnknownFormat;
    if (cpuConverted) {
        if (!transitionImage(device,
                             functions,
                             native->gfxQueue,
                             native->gfxQueueFamilyIdx,
                             image,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT)) {
            functions->vkFreeMemory(device, memory, nullptr);
            functions->vkDestroyImage(device, image, nullptr);
            return false;
        }

        const VkDeviceSize bufferSize = VkDeviceSize(scanout.width) * scanout.height * 2;
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer = VK_NULL_HANDLE;
        if (functions->vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
            functions->vkFreeMemory(device, memory, nullptr);
            functions->vkDestroyImage(device, image, nullptr);
            return false;
        }

        VkMemoryRequirements bufferRequirements = {};
        functions->vkGetBufferMemoryRequirements(device, buffer, &bufferRequirements);
        quint32 bufferTypeIndex = UINT32_MAX;
        for (quint32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if (!(bufferRequirements.memoryTypeBits & (1u << i))) {
                continue;
            }
            const VkMemoryPropertyFlags flags = memoryProperties.memoryTypes[i].propertyFlags;
            if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                bufferTypeIndex = i;
                break;
            }
        }
        if (bufferTypeIndex == UINT32_MAX) {
            functions->vkDestroyBuffer(device, buffer, nullptr);
            functions->vkFreeMemory(device, memory, nullptr);
            functions->vkDestroyImage(device, image, nullptr);
            return false;
        }

        VkMemoryAllocateInfo bufferAllocateInfo = {};
        bufferAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bufferAllocateInfo.allocationSize = bufferRequirements.size;
        bufferAllocateInfo.memoryTypeIndex = bufferTypeIndex;

        VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
        if (functions->vkAllocateMemory(device, &bufferAllocateInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
            functions->vkDestroyBuffer(device, buffer, nullptr);
            functions->vkFreeMemory(device, memory, nullptr);
            functions->vkDestroyImage(device, image, nullptr);
            return false;
        }
        if (functions->vkBindBufferMemory(device, buffer, bufferMemory, 0) != VK_SUCCESS) {
            functions->vkFreeMemory(device, bufferMemory, nullptr);
            functions->vkDestroyBuffer(device, buffer, nullptr);
            functions->vkFreeMemory(device, memory, nullptr);
            functions->vkDestroyImage(device, image, nullptr);
            return false;
        }

        if (!copyImageToBuffer(device, functions, native->gfxQueue, native->gfxQueueFamilyIdx, image, buffer, scanout.width, scanout.height)) {
            functions->vkFreeMemory(device, bufferMemory, nullptr);
            functions->vkDestroyBuffer(device, buffer, nullptr);
            functions->vkFreeMemory(device, memory, nullptr);
            functions->vkDestroyImage(device, image, nullptr);
            return false;
        }

        void *mapped = nullptr;
        if (functions->vkMapMemory(device, bufferMemory, 0, bufferSize, 0, &mapped) != VK_SUCCESS) {
            functions->vkFreeMemory(device, bufferMemory, nullptr);
            functions->vkDestroyBuffer(device, buffer, nullptr);
            functions->vkFreeMemory(device, memory, nullptr);
            functions->vkDestroyImage(device, image, nullptr);
            return false;
        }

        QImage converted(static_cast<int>(scanout.width), static_cast<int>(scanout.height), QImage::Format_RGBA8888);
        const auto *source = static_cast<const uint16_t *>(mapped);
        for (quint32 y = 0; y < scanout.height; ++y) {
            auto *destination = reinterpret_cast<uchar *>(converted.scanLine(static_cast<int>(y)));
            for (quint32 x = 0; x < scanout.width; ++x) {
                const uint16_t pixel = source[y * scanout.width + x];
                destination[x * 4 + 0] = static_cast<uchar>(((pixel >> 11) & 0x1f) * 255 / 31);
                destination[x * 4 + 1] = static_cast<uchar>(((pixel >> 5) & 0x3f) * 255 / 63);
                destination[x * 4 + 2] = static_cast<uchar>((pixel & 0x1f) * 255 / 31);
                destination[x * 4 + 3] = 0xff;
            }
        }

        functions->vkUnmapMemory(device, bufferMemory);
        functions->vkFreeMemory(device, bufferMemory, nullptr);
        functions->vkDestroyBuffer(device, buffer, nullptr);
        result.cpuImage = converted;
    } else {
        if (!transitionImage(device,
                             functions,
                             native->gfxQueue,
                             native->gfxQueueFamilyIdx,
                             image,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT)) {
            functions->vkFreeMemory(device, memory, nullptr);
            functions->vkDestroyImage(device, image, nullptr);
            return false;
        }
    }

    result.image = image;
    result.memory = memory;
    result.size = QSize(static_cast<int>(scanout.width), static_cast<int>(scanout.height));
    result.format = scanout.format;
    result.modifier = scanout.modifier;
    result.sourceFd = scanout.sourceFd;
    return true;
}

void VulkanScanoutRenderer::reportError(const QString &message)
{
    if (m_errorReported) {
        return;
    }
    m_errorReported = true;
    qCWarning(KARTON_DEBUG) << message;
    Q_EMIT errorOccurred(message);
}

QSGNode *VulkanScanoutRenderer::updatePaintNode(QQuickWindow *window, QSGNode *oldNode, const QRectF &bounds)
{
    if (!window) {
        return oldNode;
    }

    Scanout scanout;
    bool hasPending = false;
    {
        QMutexLocker locker(&m_pendingMutex);
        if (m_hasPending) {
            scanout = m_pending;
            m_pending = Scanout();
            m_hasPending = false;
            hasPending = true;
        }
    }

    if (!hasPending) {
        return oldNode;
    }

    QRhi *rhi = window->rhi();
    if (!rhi || rhi->backend() != QRhi::Vulkan) {
        closeScanout(scanout);
        return oldNode;
    }

    const auto *native = static_cast<const QRhiVulkanNativeHandles *>(rhi->nativeHandles());
    if (!native || !native->dev || !native->inst) {
        closeScanout(scanout);
        reportError(i18n("Vulkan native handles are unavailable. Restart Karton with KARTON_GRAPHICS_API=opengl."));
        return oldNode;
    }

    QMutexLocker imagesLocker(&m_imagesMutex);
    if (m_device == VK_NULL_HANDLE) {
        m_device = native->dev;
        m_functions = native->inst->deviceFunctions(native->dev);
    }

    if (!m_hasCurrent || !matches(scanout, m_current)) {
        if (m_hasCurrent && !m_current.cpuImage.isNull()) {
            // The previous CPU-converted image is no longer needed; its
            // content has already been uploaded to the scene graph texture.
            m_functions->vkDeviceWaitIdle(m_device);
            if (m_current.memory) {
                m_functions->vkFreeMemory(m_device, m_current.memory, nullptr);
            }
            if (m_current.image) {
                m_functions->vkDestroyImage(m_device, m_current.image, nullptr);
            }
            m_hasCurrent = false;
            m_current = Image();
        }

        Image image;
        if (!importScanout(window, scanout, image)) {
            closeScanout(scanout);
            return oldNode;
        }
        if (image.cpuImage.isNull()) {
            m_images.push_back(image);
        }
        m_current = image;
        m_hasCurrent = true;
        m_textureImage = VK_NULL_HANDLE;
    }

    closeScanout(scanout);

    QRhiTexture::Format rhiFormat = QRhiTexture::UnknownFormat;
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    if (!toVulkanFormat(m_current.format, &vkFormat, &rhiFormat)) {
        reportError(i18n("Unsupported SPICE scanout format. Restart Karton with KARTON_GRAPHICS_API=opengl."));
        return oldNode;
    }

    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
    }

    if (!node->texture() || m_textureImage != m_current.image) {
        if (!m_current.cpuImage.isNull()) {
            QSGTexture *texture = window->createTextureFromImage(m_current.cpuImage);
            if (!texture) {
                return node;
            }
            node->setTexture(texture);
            m_textureImage = m_current.image;
        } else {
            QRhiTexture *rhiTexture = rhi->newTexture(rhiFormat, m_current.size);
            if (!rhiTexture) {
                return node;
            }

            QRhiTexture::NativeTexture nativeTexture;
            nativeTexture.object = reinterpret_cast<quint64>(m_current.image);
            nativeTexture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (!rhiTexture->createFrom(nativeTexture)) {
                delete rhiTexture;
                reportError(i18n("Vulkan could not wrap the SPICE scanout image. Restart Karton with KARTON_GRAPHICS_API=opengl."));
                return node;
            }

            QSGTexture *texture = window->createTextureFromRhiTexture(rhiTexture);
            if (!texture) {
                delete rhiTexture;
                return node;
            }
            node->setTexture(texture);
            m_textureImage = m_current.image;
        }
    }

    node->setRect(bounds);
    node->setFiltering(QSGTexture::Linear);
    return node;
}
