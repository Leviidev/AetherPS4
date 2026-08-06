#include "xwindow_swapchain.h"
#include "vulkan_helper.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#ifdef BACHATA_VORTEK_SERVER
#include "vortek_gpu_track.h"
#include <android/log.h>
#define XW_LOG(...) __android_log_print(ANDROID_LOG_WARN, "Bachata.Vortek.GpuTrack", __VA_ARGS__)
#define XW_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "Bachata.Vortek", __VA_ARGS__)
#else
#define XW_LOG(...) ((void)0)
#define XW_ERR(...) ((void)0)
#endif

void getWindowExtent(JMethods* jmethods, int windowId, VkExtent2D* extent) {
    extent->width =
        (*jmethods->env)->CallIntMethod(jmethods->env, jmethods->obj, jmethods->getWindowWidth, windowId);
    extent->height =
        (*jmethods->env)->CallIntMethod(jmethods->env, jmethods->obj, jmethods->getWindowHeight, windowId);
}

static AHardwareBuffer* getWindowHardwareBuffer(JMethods* jmethods, int windowId,
                                                 jboolean useHALPixelFormatBGRA8888) {
    jlong hardwareBufferPtr = (*jmethods->env)
                                  ->CallLongMethod(jmethods->env, jmethods->obj,
                                                   jmethods->getWindowHardwareBuffer, windowId,
                                                   useHALPixelFormatBGRA8888);
    return (AHardwareBuffer*)hardwareBufferPtr;
}

/* NDK public enum lacks BGRA; Android HAL value is 5 (matches window GPUImage path). */
#ifndef AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
#endif

static uint32_t vk_format_to_ahb_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM;
        default:
            return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    }
}

/* Stable system-wide AHB identity (API 31+). Falls back to pointer if query fails. */
static uint64_t ahb_system_id(AHardwareBuffer* ahb) {
    if (!ahb) {
        return 0;
    }
    uint64_t id = 0;
    if (AHardwareBuffer_getId(ahb, &id) == 0 && id != 0) {
        return id;
    }
    return (uint64_t)(uintptr_t)ahb;
}

#ifdef BACHATA_VORTEK_SERVER
static int ahb_usage_has_gpu(uint64_t u) {
    const uint64_t gpu_bits = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                              AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                              AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    return (u & gpu_bits) != 0;
}

/* One probe of GetPhysicalDeviceImageFormatProperties2 + AHB usage chain. */
static VkResult probe_ahb_usage(VkPhysicalDevice physicalDevice, VkFormat format,
                                VkImageUsageFlags usage, VkImageCreateFlags flags,
                                uint64_t* out_ahb_usage) {
    *out_ahb_usage = 0;
    if (!physicalDevice || !vulkanWrapper.vkGetPhysicalDeviceImageFormatProperties2) {
        XW_LOG("PRIVATE_AHB_CAPS vkFormat=%u vkUsage=0x%x vkFlags=0x%x vkTiling=%u "
               "result=-1 recommendedAhbUsage=0x0 note=no_physical_or_fn",
               (unsigned)format, (unsigned)usage, (unsigned)flags, (unsigned)VK_IMAGE_TILING_OPTIMAL);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    VkPhysicalDeviceExternalImageFormatInfo externalInfo = {0};
    externalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkPhysicalDeviceImageFormatInfo2 formatInfo = {0};
    formatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    formatInfo.pNext = &externalInfo;
    formatInfo.format = format;
    formatInfo.type = VK_IMAGE_TYPE_2D;
    formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    formatInfo.usage = usage;
    formatInfo.flags = flags;

    VkAndroidHardwareBufferUsageANDROID ahbUsage = {0};
    ahbUsage.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID;

    VkImageFormatProperties2 properties = {0};
    properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    properties.pNext = &ahbUsage;

    VkResult result =
        vulkanWrapper.vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, &formatInfo, &properties);
    *out_ahb_usage = ahbUsage.androidHardwareBufferUsage;
    XW_LOG("PRIVATE_AHB_CAPS vkFormat=%u vkUsage=0x%x vkFlags=0x%x vkTiling=%u result=%d "
           "recommendedAhbUsage=0x%" PRIx64,
           (unsigned)format, (unsigned)usage, (unsigned)flags, (unsigned)VK_IMAGE_TILING_OPTIMAL,
           (int)result, (uint64_t)ahbUsage.androidHardwareBufferUsage);
    return result;
}

/*
 * Query driver-recommended AHB usage. Mali often rejects TRANSFER_* on external AHB
 * images — try guest usage, then COLOR+SAMPLED (no transfer). Prefer SUCCESS; if
 * result fails but usage has GPU bits, still accept (some drivers fill usage on error).
 */
static VkResult query_ahb_usage(VkPhysicalDevice physicalDevice, VkFormat format,
                                VkImageUsageFlags guest_usage, VkImageCreateFlags flags,
                                uint64_t* out_ahb_usage, VkImageUsageFlags* out_vk_usage) {
    if (!out_ahb_usage || !out_vk_usage) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *out_ahb_usage = 0;
    *out_vk_usage = guest_usage;

    const VkImageUsageFlags candidates[] = {
        guest_usage,
        /* Drop transfer: common Mali external-AHB failure mode for usage 0x13. */
        (VkImageUsageFlags)(guest_usage & ~(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    };

    VkResult last = VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        VkImageUsageFlags u = candidates[i];
        if (u == 0) {
            continue;
        }
        uint64_t ahb_u = 0;
        VkResult r = probe_ahb_usage(physicalDevice, format, u, flags, &ahb_u);
        last = r;
        if (r == VK_SUCCESS && ahb_usage_has_gpu(ahb_u)) {
            *out_ahb_usage = ahb_u;
            *out_vk_usage = u;
            /* Ensure CPU read for compositor path / diagnostics. */
            *out_ahb_usage |= AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
            XW_LOG("PRIVATE_AHB_CAPS_SELECTED vkUsage=0x%x recommendedAhbUsage=0x%" PRIx64,
                   (unsigned)u, *out_ahb_usage);
            return VK_SUCCESS;
        }
        if (r != VK_SUCCESS && ahb_usage_has_gpu(ahb_u)) {
            /* Partial fill: accept with warning. */
            *out_ahb_usage = ahb_u | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
            *out_vk_usage = u;
            XW_LOG("PRIVATE_AHB_CAPS_SELECTED_PARTIAL result=%d vkUsage=0x%x "
                   "recommendedAhbUsage=0x%" PRIx64,
                   (int)r, (unsigned)u, *out_ahb_usage);
            return VK_SUCCESS;
        }
    }
    return last;
}
#endif

static VkResult createImageMemory(VkDevice device, VkImage image, AHardwareBuffer* hardwareBuffer,
                                  VkDeviceMemory* pMemory, uint64_t* out_alloc_size) {
    VkAndroidHardwareBufferPropertiesANDROID ahbProperties = {0};
    ahbProperties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    VkResult result =
        vulkanWrapper.vkGetAndroidHardwareBufferPropertiesANDROID(device, hardwareBuffer, &ahbProperties);
    if (result != VK_SUCCESS) {
        return result;
    }

    /* Import chain: alloc → import → dedicated (user-spec order). */
    VkMemoryDedicatedAllocateInfo dedicated = {0};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    dedicated.buffer = VK_NULL_HANDLE;

    VkImportAndroidHardwareBufferInfoANDROID importAhb = {0};
    importAhb.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    importAhb.pNext = &dedicated;
    importAhb.buffer = hardwareBuffer;

    VkMemoryAllocateInfo memoryInfo = {0};
    memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryInfo.pNext = &importAhb;
    memoryInfo.allocationSize = ahbProperties.allocationSize;
    memoryInfo.memoryTypeIndex =
        getMemoryTypeIndex(ahbProperties.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory memory;
    result = vulkanWrapper.vkAllocateMemory(device, &memoryInfo, NULL, &memory);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = vulkanWrapper.vkBindImageMemory(device, image, memory, 0);
    if (result != VK_SUCCESS) {
        vulkanWrapper.vkFreeMemory(device, memory, NULL);
        return result;
    }

    *pMemory = memory;
    if (out_alloc_size) {
        *out_alloc_size = ahbProperties.allocationSize;
    }
    return VK_SUCCESS;
}

static VkResult create_vk_image_from_ahb(VkDevice device, XWindowSwapchain* swapchain,
                                         AHardwareBuffer* hardwareBuffer, VkImage* outImage,
                                         VkDeviceMemory* outMemory, uint64_t* outAllocSize) {
    AHardwareBuffer_Desc ahbDesc = {0};
    AHardwareBuffer_describe(hardwareBuffer, &ahbDesc);

    VkExternalMemoryImageCreateInfo externalMemoryImageInfo = {0};
    externalMemoryImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalMemoryImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo imageInfo = {0};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalMemoryImageInfo;
    imageInfo.flags = swapchain->imageFlags;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = swapchain->imageFormat;
    imageInfo.extent.width = ahbDesc.width;
    imageInfo.extent.height = ahbDesc.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = swapchain->imageUsage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image;
    VkResult result = vulkanWrapper.vkCreateImage(device, &imageInfo, NULL, &image);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkDeviceMemory memory;
    uint64_t allocSize = 0;
    result = createImageMemory(device, image, hardwareBuffer, &memory, &allocSize);
    if (result != VK_SUCCESS) {
        vulkanWrapper.vkDestroyImage(device, image, NULL);
        return result;
    }

    *outImage = image;
    *outMemory = memory;
    if (outAllocSize) {
        *outAllocSize = allocSize;
    }
    return VK_SUCCESS;
}

static VkResult create_private_swapchain_image(VkDevice device, XWindowSwapchain* swapchain,
                                               XWindowSwapchain_Image* swapchainImage,
                                               uint32_t imageIndex) {
    AHardwareBuffer_Desc desc = {0};
    desc.width = swapchain->imageExtent.width;
    desc.height = swapchain->imageExtent.height;
    desc.layers = 1;
    desc.format = vk_format_to_ahb_format(swapchain->imageFormat);
    desc.usage = (uint32_t)swapchain->recommended_ahb_usage;

    AHardwareBuffer* ahb = NULL;
    int rc = AHardwareBuffer_allocate(&desc, &ahb);
    if (rc != 0 || !ahb) {
        XW_ERR("PRIVATE_AHB_ALLOC_FAILED imageIndex=%u rc=%d usage=0x%" PRIx64 " format=%u %ux%u",
               imageIndex, rc, swapchain->recommended_ahb_usage, desc.format, desc.width, desc.height);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    AHardwareBuffer_Desc got = {0};
    AHardwareBuffer_describe(ahb, &got);

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t allocSize = 0;
    VkResult result = create_vk_image_from_ahb(device, swapchain, ahb, &image, &memory, &allocSize);
    if (result != VK_SUCCESS) {
        XW_ERR("PRIVATE_AHB_IMPORT_FAILED imageIndex=%u result=%d ahb=%p", imageIndex, (int)result,
               (void*)ahb);
        AHardwareBuffer_release(ahb);
        return result;
    }

    swapchainImage->image = image;
    swapchainImage->memory = memory;
    swapchainImage->ahb = ahb;
    swapchainImage->owns_ahb = 1;
    swapchainImage->allocation_size = allocSize;
    swapchainImage->ahb_usage = got.usage;
    swapchainImage->stride = got.stride;
    swapchainImage->native_handle = ahb_system_id(ahb);
    swapchainImage->image_index = imageIndex;
    swapchainImage->life = XW_IMG_AVAILABLE;
    swapchainImage->acquire_generation = 0;
    swapchainImage->present_id = 0;
    swapchainImage->present_pending = false;

#ifdef BACHATA_VORTEK_SERVER
    uint64_t ahbId = 0, mapId = 0, gpuVaId = 0;
    VortekGpuTrack_registerSwapchainBacking(
        imageIndex, (void*)(uintptr_t)image, (void*)(uintptr_t)memory, (void*)ahb,
        (intptr_t)swapchainImage->native_handle, allocSize, got.usage, got.stride, &ahbId, &mapId,
        &gpuVaId);
    swapchainImage->ahb_id = ahbId;
    XW_LOG("PRIVATE_AHB_CREATED imageIndex=%u ahb=%p nativeHandle=0x%" PRIx64, imageIndex,
           (void*)ahb, swapchainImage->native_handle);
    XW_LOG("SWAPCHAIN_IMAGE_BACKING imageIndex=%u ahbId=0x%" PRIx64 " mappingId=0x%" PRIx64
           " gpuVaId=0x%" PRIx64 " vkImage=%p vkMemory=%p usage=0x%x allocationSize=%" PRIu64
           " stride=%u ahb=%p nativeHandle=0x%" PRIx64,
           imageIndex, ahbId, mapId, gpuVaId, (void*)(uintptr_t)image, (void*)(uintptr_t)memory,
           (unsigned)got.usage, allocSize, (unsigned)got.stride, (void*)ahb,
           swapchainImage->native_handle);
#else
    (void)imageIndex;
#endif
    return VK_SUCCESS;
}

#ifdef BACHATA_VORTEK_SERVER
/* CPU fallback when VkImage lacks TRANSFER usage (common after AHB caps drop transfer). */
static VkResult cpu_copy_ahb(AHardwareBuffer* src, AHardwareBuffer* dst) {
    if (!src || !dst) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    AHardwareBuffer_Desc sd = {0}, dd = {0};
    AHardwareBuffer_describe(src, &sd);
    AHardwareBuffer_describe(dst, &dd);
    void* sp = NULL;
    void* dp = NULL;
    if (AHardwareBuffer_lock(src, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &sp) != 0) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    if (AHardwareBuffer_lock(dst, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &dp) != 0) {
        AHardwareBuffer_unlock(src, NULL);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    const uint32_t h = sd.height < dd.height ? sd.height : dd.height;
    const uint32_t row_bytes =
        (sd.width < dd.width ? sd.width : dd.width) * 4u; /* RGBA/BGRA 8-bit */
    const uint32_t sstride = sd.stride * 4u;
    const uint32_t dstride = dd.stride * 4u;
    for (uint32_t y = 0; y < h; ++y) {
        memcpy((uint8_t*)dp + (size_t)y * dstride, (uint8_t*)sp + (size_t)y * sstride, row_bytes);
    }
    AHardwareBuffer_unlock(dst, NULL);
    AHardwareBuffer_unlock(src, NULL);
    return VK_SUCCESS;
}

static VkResult blit_image_to_window(XWindowSwapchain* swapchain, uint32_t srcIndex) {
    if (!swapchain || srcIndex >= (uint32_t)swapchain->imageCount) {
        return VK_SUCCESS;
    }
    AHardwareBuffer* srcAhb = swapchain->images[srcIndex].ahb;
    if (!srcAhb || srcAhb == swapchain->windowAhb) {
        return VK_SUCCESS;
    }
    /* Prefer CPU copy when transfer bits missing or no blit cmd. */
    const int can_gpu =
        swapchain->windowImage && swapchain->blitCmd &&
        (swapchain->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        (swapchain->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!can_gpu) {
        return cpu_copy_ahb(srcAhb, swapchain->windowAhb);
    }
    if (!swapchain->blitCmd) {
        return cpu_copy_ahb(srcAhb, swapchain->windowAhb);
    }

    VkCommandBuffer cmd = swapchain->blitCmd;
    VkResult r = vulkanWrapper.vkResetCommandBuffer(cmd, 0);
    if (r != VK_SUCCESS) {
        return r;
    }

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vulkanWrapper.vkBeginCommandBuffer(cmd, &begin);
    if (r != VK_SUCCESS) {
        return r;
    }

    VkImage src = swapchain->images[srcIndex].image;
    VkImage dst = swapchain->windowImage;
    uint32_t w = swapchain->imageExtent.width ? swapchain->imageExtent.width : 1;
    uint32_t h = swapchain->imageExtent.height ? swapchain->imageExtent.height : 1;

    VkImageMemoryBarrier barriers[2] = {0};
    for (int i = 0; i < 2; ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[i].subresourceRange.levelCount = 1;
        barriers[i].subresourceRange.layerCount = 1;
    }
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].image = src;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].image = dst;

    vulkanWrapper.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2,
                                       barriers);

    VkImageCopy region = {0};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent.width = w;
    region.extent.height = h;
    region.extent.depth = 1;
    vulkanWrapper.vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier after = {0};
    after.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after.image = dst;
    after.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    after.subresourceRange.levelCount = 1;
    after.subresourceRange.layerCount = 1;
    vulkanWrapper.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                                       &after);

    r = vulkanWrapper.vkEndCommandBuffer(cmd);
    if (r != VK_SUCCESS) {
        return r;
    }

    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    r = vulkanWrapper.vkQueueSubmit(swapchain->queue, 1, &submit, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        return r;
    }
    return vulkanWrapper.vkQueueWaitIdle(swapchain->queue);
}
#endif

int getSurfaceMinImageCount() {
    return 1;
}

VkSurfaceFormatKHR* getSurfaceFormats(uint32_t* formatCount) {
    static const VkFormat supportedFormats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB,
                                                VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    int supportedFormatCount = ARRAY_SIZE(supportedFormats);
    VkSurfaceFormatKHR* surfaceFormats = calloc(supportedFormatCount, sizeof(VkSurfaceFormatKHR));

    if (formatCount) {
        *formatCount = supportedFormatCount;
    }

    for (int i = 0; i < supportedFormatCount; i++) {
        surfaceFormats[i].format = supportedFormats[i];
        surfaceFormats[i].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }

    return surfaceFormats;
}

XWindowSwapchain* XWindowSwapchain_create(VkDevice device, uint32_t graphicsQueueIndex,
                                          VkSwapchainCreateInfoKHR* swapchainInfo,
                                          JMethods* jmethods, int windowId) {
    XWindowSwapchain* swapchain = calloc(1, sizeof(XWindowSwapchain));
    if (!swapchain) {
        return NULL;
    }
    swapchain->windowId = windowId;
    swapchain->imageCount = swapchainInfo->minImageCount > 0 ? (int)swapchainInfo->minImageCount : 1;
    if (swapchain->imageCount < 1) {
        swapchain->imageCount = 1;
    }
    swapchain->nextAcquireIndex = 0;
    swapchain->currentImageIndex = 0;
    swapchain->sharedAhb = 0;
    swapchain->distinct_ahb_confirmed = 0;
    swapchain->windowAhb = NULL;
    swapchain->windowImage = VK_NULL_HANDLE;
    swapchain->windowMemory = VK_NULL_HANDLE;
    swapchain->device = device;
    swapchain->physicalDevice = bachataHostPhysicalDevice;
    swapchain->blitCmdPool = VK_NULL_HANDLE;
    swapchain->blitCmd = VK_NULL_HANDLE;
    swapchain->images = calloc(swapchain->imageCount, sizeof(XWindowSwapchain_Image));
    swapchain->imageFormat = swapchainInfo->imageFormat;
    /* Start from guest usage; capability query may drop TRANSFER for external AHB. */
    swapchain->imageUsage = swapchainInfo->imageUsage;
    /* Do NOT set ALIAS_BIT for private AHB path — each image has dedicated backing. */
    swapchain->imageFlags = 0;
    memcpy(&swapchain->imageExtent, &swapchainInfo->imageExtent, sizeof(VkExtent2D));
    swapchain->jmethods = jmethods;

#ifdef BACHATA_VORTEK_SERVER
    const bool require_distinct = VortekGpuTrack_requireDistinctAhb();
    VkImageUsageFlags selected_usage = swapchain->imageUsage;
    VkResult cap_result =
        query_ahb_usage(swapchain->physicalDevice, swapchain->imageFormat, swapchain->imageUsage,
                        swapchain->imageFlags, &swapchain->recommended_ahb_usage, &selected_usage);
    if (cap_result != VK_SUCCESS) {
        XW_ERR("PRIVATE_AHB_CAPS_FAILED result=%d require_distinct=%d", (int)cap_result,
               require_distinct ? 1 : 0);
        if (require_distinct) {
            goto error;
        }
        swapchain->recommended_ahb_usage =
            AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
            AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
        selected_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    swapchain->imageUsage = selected_usage;
    /* Prefer GPU blit when transfer bits present; else CPU path in present. */
    if ((swapchain->imageUsage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) !=
        (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        XW_LOG("PRIVATE_AHB_NO_TRANSFER_USAGE vkUsage=0x%x note=cpu_blit_to_window",
               (unsigned)swapchain->imageUsage);
    }
#else
    swapchain->recommended_ahb_usage =
        AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
#endif

    /* Compositor window AHB (not guest swapchain image). */
    jboolean useBGRA = swapchain->imageFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                       swapchain->imageFormat == VK_FORMAT_B8G8R8A8_SRGB;
    swapchain->windowAhb = getWindowHardwareBuffer(jmethods, windowId, useBGRA);
    if (!swapchain->windowAhb) {
        XW_ERR("WINDOW_AHB_MISSING windowId=%d", windowId);
        goto error;
    }
    {
        uint64_t winAlloc = 0;
        VkResult wr = create_vk_image_from_ahb(device, swapchain, swapchain->windowAhb,
                                               &swapchain->windowImage, &swapchain->windowMemory,
                                               &winAlloc);
        if (wr != VK_SUCCESS) {
            XW_ERR("WINDOW_AHB_IMPORT_FAILED result=%d", (int)wr);
#ifdef BACHATA_VORTEK_SERVER
            if (VortekGpuTrack_requireDistinctAhb()) {
                goto error;
            }
#endif
            /* Without window image, present blit fails — still try guest images. */
        }
        (void)winAlloc;
    }

    for (int i = 0; i < swapchain->imageCount; i++) {
        VkResult result =
            create_private_swapchain_image(device, swapchain, &swapchain->images[i], (uint32_t)i);
        if (result != VK_SUCCESS) {
#ifdef BACHATA_VORTEK_SERVER
            if (VortekGpuTrack_requireDistinctAhb()) {
                XW_ERR("REQUIRE_DISTINCT_AHB abort: private image %d failed result=%d", i,
                       (int)result);
                goto error;
            }
#endif
            /* Non-strict legacy fallback: share window AHB (cosmetic multi-image). */
            XW_ERR("PRIVATE_AHB_FALLBACK_SHARED imageIndex=%d", i);
            swapchain->images[i].ahb = swapchain->windowAhb;
            swapchain->images[i].owns_ahb = 0;
            swapchain->images[i].image = swapchain->windowImage;
            swapchain->images[i].memory = swapchain->windowMemory;
            swapchain->images[i].image_index = (uint32_t)i;
            swapchain->sharedAhb = 1;
        }
    }

#ifdef BACHATA_VORTEK_SERVER
    /* Validate distinctness for multi-image. */
    if (swapchain->imageCount > 1) {
        AHardwareBuffer* a0 = swapchain->images[0].ahb;
        AHardwareBuffer* a1 = swapchain->images[1].ahb;
        uint64_t h0 = swapchain->images[0].native_handle;
        uint64_t h1 = swapchain->images[1].native_handle;
        int same_ptr = (a0 == a1);
        int same_native = (h0 != 0 && h0 == h1);
        int same_vk = (swapchain->images[0].image == swapchain->images[1].image) ||
                      (swapchain->images[0].memory == swapchain->images[1].memory);

        if (same_ptr || same_native || same_vk || !swapchain->images[0].owns_ahb ||
            !swapchain->images[1].owns_ahb) {
            swapchain->sharedAhb = 1;
            XW_ERR("DISTINCT_AHB_FAILED same_ptr=%d same_native=%d same_vk=%d owns0=%d owns1=%d "
                   "ahb0=%p ahb1=%p nh0=0x%" PRIx64 " nh1=0x%" PRIx64,
                   same_ptr, same_native, same_vk, swapchain->images[0].owns_ahb,
                   swapchain->images[1].owns_ahb, (void*)a0, (void*)a1, h0, h1);
            if (VortekGpuTrack_requireDistinctAhb()) {
                goto error;
            }
            VortekGpuTrack_noteSharedAhb((void*)a0, (uint32_t)swapchain->imageCount);
        } else {
            swapchain->distinct_ahb_confirmed = 1;
            XW_LOG("DISTINCT_AHB_CONFIRMED image0=0x%" PRIx64 " image1=0x%" PRIx64,
                   swapchain->images[0].ahb_id, swapchain->images[1].ahb_id);
            XW_LOG("DISTINCT_AHB_CONFIRMED ahb0=%p ahb1=%p native0=0x%" PRIx64 " native1=0x%" PRIx64
                   " vkImage0=%p vkImage1=%p vkMemory0=%p vkMemory1=%p",
                   (void*)a0, (void*)a1, h0, h1, (void*)(uintptr_t)swapchain->images[0].image,
                   (void*)(uintptr_t)swapchain->images[1].image,
                   (void*)(uintptr_t)swapchain->images[0].memory,
                   (void*)(uintptr_t)swapchain->images[1].memory);
        }
    }

    /* Blit pool: private images → window compositor image. */
    if (swapchain->windowImage && swapchain->imageCount > 0) {
        VkCommandPoolCreateInfo poolInfo = {0};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphicsQueueIndex;
        if (vulkanWrapper.vkCreateCommandPool(device, &poolInfo, NULL, &swapchain->blitCmdPool) ==
            VK_SUCCESS) {
            VkCommandBufferAllocateInfo allocInfo = {0};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = swapchain->blitCmdPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            vulkanWrapper.vkAllocateCommandBuffers(device, &allocInfo, &swapchain->blitCmd);
        }
    }
#endif

    vulkanWrapper.vkGetDeviceQueue(device, graphicsQueueIndex, 0, &swapchain->queue);
#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_notePresenterConfig((uint32_t)swapchain->imageCount, (uint32_t)swapchain->imageCount,
                                       (uint32_t)swapchain->imageCount, graphicsQueueIndex,
                                       graphicsQueueIndex, swapchain);
#endif
    return swapchain;

error:
    if (swapchain) {
        if (swapchain->blitCmdPool) {
            vulkanWrapper.vkDestroyCommandPool(device, swapchain->blitCmdPool, NULL);
        }
        if (swapchain->images) {
            for (int i = 0; i < swapchain->imageCount; i++) {
                if (swapchain->images[i].owns_ahb) {
                    if (swapchain->images[i].image) {
                        vulkanWrapper.vkDestroyImage(device, swapchain->images[i].image, NULL);
                    }
                    if (swapchain->images[i].memory) {
                        vulkanWrapper.vkFreeMemory(device, swapchain->images[i].memory, NULL);
                    }
                    if (swapchain->images[i].ahb) {
                        AHardwareBuffer_release(swapchain->images[i].ahb);
                    }
                }
            }
            MEMFREE(swapchain->images);
        }
        if (swapchain->windowImage) {
            vulkanWrapper.vkDestroyImage(device, swapchain->windowImage, NULL);
        }
        if (swapchain->windowMemory) {
            vulkanWrapper.vkFreeMemory(device, swapchain->windowMemory, NULL);
        }
        MEMFREE(swapchain);
    }
    return NULL;
}

void XWindowSwapchain_destroy(VkDevice device, XWindowSwapchain* swapchain) {
    if (!swapchain) {
        return;
    }
#ifdef BACHATA_VORTEK_SERVER
    if (swapchain->blitCmdPool) {
        vulkanWrapper.vkDestroyCommandPool(device, swapchain->blitCmdPool, NULL);
        swapchain->blitCmdPool = VK_NULL_HANDLE;
        swapchain->blitCmd = VK_NULL_HANDLE;
    }
#endif
    for (int i = 0; i < swapchain->imageCount; i++) {
        if (swapchain->images[i].owns_ahb) {
            if (swapchain->images[i].image) {
                vulkanWrapper.vkDestroyImage(device, swapchain->images[i].image, NULL);
            }
            if (swapchain->images[i].memory) {
                vulkanWrapper.vkFreeMemory(device, swapchain->images[i].memory, NULL);
            }
            if (swapchain->images[i].ahb) {
                AHardwareBuffer_release(swapchain->images[i].ahb);
            }
        }
        /* Shared fallback may alias windowImage — do not double-destroy. */
    }
    if (swapchain->windowImage) {
        vulkanWrapper.vkDestroyImage(device, swapchain->windowImage, NULL);
    }
    if (swapchain->windowMemory) {
        vulkanWrapper.vkFreeMemory(device, swapchain->windowMemory, NULL);
    }

    MEMFREE(swapchain->images);
    MEMFREE(swapchain);
}

VkResult XWindowSwapchain_acquireNextImage(XWindowSwapchain* swapchain, uint64_t timeout,
                                           VkSemaphore signalSemaphore, VkFence fence,
                                           uint32_t* imageIndex) {
    (void)timeout;
    if (signalSemaphore || fence) {
        VkSubmitInfo submitInfo = {0};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (signalSemaphore) {
            submitInfo.pSignalSemaphores = &signalSemaphore;
            submitInfo.signalSemaphoreCount = 1;
        }

        VkResult result = vulkanWrapper.vkQueueSubmit(swapchain->queue, 1, &submitInfo, fence);
        if (result == VK_ERROR_DEVICE_LOST) {
            return result;
        }
    }

    VkExtent2D windowSize;
    getWindowExtent(swapchain->jmethods, swapchain->windowId, &windowSize);

    VkResult result = VK_SUCCESS;
    if (swapchain->imageExtent.width != windowSize.width ||
        swapchain->imageExtent.height != windowSize.height) {
        result = VK_ERROR_SURFACE_LOST_KHR;
    }

    uint32_t idx = 0;
    if (swapchain->imageCount > 0) {
        for (int attempt = 0; attempt < swapchain->imageCount; ++attempt) {
            uint32_t cand =
                (uint32_t)((swapchain->nextAcquireIndex + attempt) % swapchain->imageCount);
            XWindowSwapchain_Image* img = &swapchain->images[cand];

            if (img->present_pending || img->life == XW_IMG_PRESENT_PENDING) {
#ifdef BACHATA_VORTEK_SERVER
                VortekGpuTrack_notePresentComplete(img->present_id, cand, "reacquire");
#endif
                img->present_pending = false;
                img->life = XW_IMG_AVAILABLE;
            }

            idx = cand;
            swapchain->nextAcquireIndex = (int)((cand + 1) % (uint32_t)swapchain->imageCount);
            break;
        }
    }

    XWindowSwapchain_Image* acquired = &swapchain->images[idx];
    acquired->life = XW_IMG_ACQUIRED;
    acquired->acquire_generation++;
    acquired->present_pending = false;
    swapchain->currentImageIndex = (int)idx;
    *imageIndex = idx;
#ifdef BACHATA_VORTEK_SERVER
    static uint64_t s_acquire_call_id = 0;
    s_acquire_call_id++;
    VortekGpuTrack_noteAcquireResult(s_acquire_call_id, (int)result, idx,
                                     (void*)(uintptr_t)signalSemaphore, (void*)(uintptr_t)fence);
#endif
    return result;
}

void XWindowSwapchain_presentImage(XWindowSwapchain* swapchain) {
#ifdef BACHATA_VORTEK_SERVER
    /* Private guest image → window AHB so updateWindowContent reads compositor buffer. */
    if (swapchain && swapchain->currentImageIndex >= 0 && swapchain->windowImage &&
        swapchain->images[swapchain->currentImageIndex].ahb != swapchain->windowAhb) {
        VkResult br = blit_image_to_window(swapchain, (uint32_t)swapchain->currentImageIndex);
        if (br != VK_SUCCESS) {
            XW_ERR("present_blit_to_window_failed result=%d imageIndex=%d", (int)br,
                   swapchain->currentImageIndex);
        }
    }
#endif
    (*swapchain->jmethods->env)
        ->CallVoidMethod(swapchain->jmethods->env, swapchain->jmethods->obj,
                         swapchain->jmethods->updateWindowContent, swapchain->windowId);
}
