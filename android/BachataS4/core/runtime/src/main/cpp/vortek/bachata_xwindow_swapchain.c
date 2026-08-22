/*
 * Bachata Vortek WSI synchronization adapter.
 *
 * Upstream Vortek calls updateWindowContent immediately from vkQueuePresentKHR.
 * Bachata's Canvas compositor then CPU-locks and copies the swapchain AHB, so the
 * host queue must finish the preceding render submission before that read.
 *
 * Product present ownership:
 *   1) Consume present wait semaphores (binary sem must not be re-signaled while
 *      still signaled — host previously ignored pWaitSemaphores).
 *   2) QueueWaitIdle = render complete only (NOT present complete).
 *   3) updateWindowContent = compositor/AHB CPU copy (sync) = present complete.
 *   4) Per-image state AVAILABLE→ACQUIRED→PRESENT_PENDING→AVAILABLE.
 */
#include <android/log.h>
#include <stdint.h>

#include "vulkan_helper.h"

#ifdef BACHATA_VORTEK_SERVER
#include "vortek_gpu_track.h"
#endif

#define XWindowSwapchain_presentImage BachataUpstreamXWindowSwapchain_presentImage
#include "xwindow_swapchain.c"
#undef XWindowSwapchain_presentImage

#ifdef BACHATA_VORTEK_SERVER
static uint64_t g_active_present_id = 0;
static int g_present_id_override = 0;
#endif

/* Consumes guest present wait semaphores so binary present_ready[i] is unsignaled. */
static VkResult bachata_consume_present_wait_semaphores(VkQueue queue,
                                                        const VkSemaphore* waitSemaphores,
                                                        uint32_t waitSemaphoreCount) {
    if (!queue || !waitSemaphores || waitSemaphoreCount == 0) {
        return VK_SUCCESS;
    }
    enum { kMaxWait = 16 };
    VkPipelineStageFlags stages[kMaxWait];
    uint32_t n = waitSemaphoreCount;
    if (n > kMaxWait) {
        n = kMaxWait;
    }
    for (uint32_t i = 0; i < n; ++i) {
        stages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = n;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = stages;
    return vulkanWrapper.vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
}

void XWindowSwapchain_presentImage(XWindowSwapchain* swapchain) {
#ifdef BACHATA_VORTEK_SERVER
    static uint64_t s_present_id = 0;
    uint64_t present_id;
    if (g_present_id_override) {
        present_id = g_active_present_id;
    } else {
        s_present_id++;
        present_id = s_present_id;
    }
    const uint32_t imageIndex =
        swapchain && swapchain->currentImageIndex >= 0
            ? (uint32_t)swapchain->currentImageIndex
            : 0u;
    VortekGpuTrack_noteQueue(swapchain->queue);
    VortekGpuTrack_notePresent(swapchain->queue, present_id);

    if (swapchain && imageIndex < (uint32_t)swapchain->imageCount) {
        XWindowSwapchain_Image* img = &swapchain->images[imageIndex];
        img->life = XW_IMG_PRESENT_PENDING;
        img->present_pending = true;
        img->present_id = present_id;
    }
#else
    const uint32_t imageIndex = 0u;
    const uint64_t present_id = 0;
    (void)present_id;
#endif

    /* Render-side drain only. Does not prove compositor/AHB release. */
    VkResult result = vulkanWrapper.vkQueueWaitIdle(swapchain->queue);
    if (result != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "Bachata.Vortek",
                            "present_sync_failed result=%d imageIndex=%u", (int)result,
                            imageIndex);
#ifdef BACHATA_VORTEK_SERVER
        VortekGpuTrack_onPresentSyncFailed((int)result);
#endif
        return;
    }
#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_noteQueueWaitIdle(swapchain->queue, (int)result);
    /* RENDER_WAIT_COMPLETE + PRESENT_ACCEPTED (distinct from compositor). */
    VortekGpuTrack_notePresentAccepted(present_id, imageIndex);
    VortekGpuTrack_freeflightEvent("RENDER_WAIT_COMPLETE", swapchain->queue, present_id, NULL, 0,
                                   0, 0, 0, 0, 0, 0, 0);
#endif

    /* Compositor path: blit (if private AHB) + sync AHB CPU copy + unlock. */
    BachataUpstreamXWindowSwapchain_presentImage(swapchain);

#ifdef BACHATA_VORTEK_SERVER
    if (swapchain && imageIndex < (uint32_t)swapchain->imageCount) {
        XWindowSwapchain_Image* img = &swapchain->images[imageIndex];
        img->present_pending = false;
        img->life = XW_IMG_AVAILABLE;
    }
    VortekGpuTrack_notePresentComplete(present_id, imageIndex, "compositor_sync");
#endif
}

#ifdef BACHATA_VORTEK_SERVER
/*
 * Full present entry used by request_handler: wait-sem consume + ownership + present.
 */
VkResult BachataXWindowSwapchain_presentWithWaits(XWindowSwapchain* swapchain,
                                                  uint32_t imageIndex,
                                                  const VkSemaphore* waitSemaphores,
                                                  uint32_t waitSemaphoreCount,
                                                  uint64_t presentId) {
    if (!swapchain) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if ((int)imageIndex < swapchain->imageCount) {
        swapchain->currentImageIndex = (int)imageIndex;
    }

    void* waitSem = (waitSemaphoreCount > 0 && waitSemaphores)
                        ? (void*)(uintptr_t)waitSemaphores[0]
                        : NULL;

    if (!VortekGpuTrack_beginPresent(presentId, imageIndex, waitSem)) {
        /* present_pending on this semaphore/image — product: drain + force complete. */
        __android_log_print(ANDROID_LOG_WARN, "Bachata.Vortek",
                            "PRESENT_SEMAPHORE_REUSE_BLOCKED imageIndex=%u presentId=%llu "
                            "waitSemaphore=%p — draining queue before reuse",
                            imageIndex, (unsigned long long)presentId, waitSem);
        VkResult idle = vulkanWrapper.vkQueueWaitIdle(swapchain->queue);
        VortekGpuTrack_noteQueueWaitIdle(swapchain->queue, (int)idle);
        VortekGpuTrack_notePresentComplete(0, imageIndex, "diag_sem_reuse_wait");
        if (swapchain->imageCount > 0 && (int)imageIndex < swapchain->imageCount) {
            swapchain->images[imageIndex].present_pending = false;
            swapchain->images[imageIndex].life = XW_IMG_AVAILABLE;
        }
        (void)VortekGpuTrack_beginPresent(presentId, imageIndex, waitSem);
    }

    /* Shared AHB: serialize if another image still claims present ownership. */
    if (swapchain->sharedAhb) {
        for (int i = 0; i < swapchain->imageCount; ++i) {
            if (i == (int)imageIndex) {
                continue;
            }
            if (swapchain->images[i].present_pending) {
                VortekGpuTrack_logBusyAhb((void*)swapchain->images[i].ahb, imageIndex,
                                          (uint32_t)i);
                VkResult idle = vulkanWrapper.vkQueueWaitIdle(swapchain->queue);
                VortekGpuTrack_noteQueueWaitIdle(swapchain->queue, (int)idle);
                VortekGpuTrack_forcePresentCompleteForAhb((void*)swapchain->images[i].ahb,
                                                         "diag_same_ahb_wait");
                swapchain->images[i].present_pending = false;
                swapchain->images[i].life = XW_IMG_AVAILABLE;
            }
        }
    }

    /* Product fix: wait on guest present_ready[imageIndex] (consume binary sem). */
    if (waitSemaphoreCount > 0 && waitSemaphores) {
        __android_log_print(ANDROID_LOG_WARN, "Bachata.Vortek.GpuTrack",
                            "SEMAPHORE_WAIT presentId=%llu imageIndex=%u count=%u",
                            (unsigned long long)presentId, imageIndex, waitSemaphoreCount);
        VortekGpuTrack_freeflightEvent("SEMAPHORE_WAIT", swapchain->queue, presentId,
                                       (void*)(uintptr_t)waitSemaphores[0], 0, 0, 0, 0, 0, 0, 0,
                                       0);
        VkResult wr =
            bachata_consume_present_wait_semaphores(swapchain->queue, waitSemaphores,
                                                    waitSemaphoreCount);
        if (wr != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "Bachata.Vortek",
                                "present_wait_sem_submit_failed result=%d imageIndex=%u",
                                (int)wr, imageIndex);
            if (wr == VK_ERROR_DEVICE_LOST) {
                VortekGpuTrack_onPresentSyncFailed((int)wr);
            }
            return wr;
        }
    }

    g_active_present_id = presentId;
    g_present_id_override = 1;
    XWindowSwapchain_presentImage(swapchain);
    g_present_id_override = 0;
    return VK_SUCCESS;
}
#endif
