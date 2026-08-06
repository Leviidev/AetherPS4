#include "request_handler.h"
#include "vk_context.h"
#include "vulkan_helper.h"
#include "sysvshared_memory.h"
#ifdef BACHATA_VORTEK_SERVER
#include "vortek_gpu_track.h"
#include <android/log.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#endif

#ifdef BACHATA_VORTEK_SERVER
/* Internal completion fences: host proof for fence=0x0 submits.
 * Generation + activeWaiters: never reset/recycle while a serial still references. */
enum { kBachataInternalFencePool = 512 };
typedef struct BachataInternalFenceSlot {
    VkFence fence;
    uint64_t generation;
    uint64_t submission_serial; /* global submission id bound at QueueSubmit */
    uint32_t active_waiters;
    uint8_t used;
    uint8_t submitted;
    uint8_t signaled;
} BachataInternalFenceSlot;
static VkDevice g_bachata_track_device = VK_NULL_HANDLE;
static BachataInternalFenceSlot g_bachata_internal_fence_slots[kBachataInternalFencePool];
static int g_bachata_internal_fence_count = 0;

/* R2 physical multi-slot pool: alternate VkDeviceMemory when guest range busy. */
enum { kBachataSuballocPoolMax = 64 };
typedef struct BachataSuballocPoolSlot {
    int used;
    int busy;
    VkDeviceMemory memory;
    VkDeviceSize size;
    uint32_t memoryTypeIndex;
    uint64_t generation;
    void* owner_buffer;
} BachataSuballocPoolSlot;
static BachataSuballocPoolSlot g_bachata_pool_slots[kBachataSuballocPoolMax];
static int g_bachata_pool_slot_count = 0;
static uint64_t g_bachata_pool_bytes = 0;

static void bachata_note_track_device(VkDevice device) {
    if (device) {
        g_bachata_track_device = device;
    }
}

static BachataInternalFenceSlot* bachata_find_fence_slot(VkFence fence) {
    if (!fence) {
        return NULL;
    }
    for (int i = 0; i < g_bachata_internal_fence_count; ++i) {
        if (g_bachata_internal_fence_slots[i].fence == fence) {
            return &g_bachata_internal_fence_slots[i];
        }
    }
    return NULL;
}

/* Recycle only when: signaled, no waiters, submission map cleared. */
static int bachata_try_recycle_fence_slot(VkDevice device, BachataInternalFenceSlot* slot) {
    if (!slot || !slot->used || !slot->fence) {
        return 0;
    }
    if (slot->active_waiters > 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Bachata.Vortek.GpuTrack",
                            "FENCE_RECYCLED_WHILE_REFERENCED fence=%p generation=%" PRIu64
                            " waiters=%u serial=%" PRIu64,
                            (void*)slot->fence, slot->generation, slot->active_waiters,
                            slot->submission_serial);
        return 0;
    }
    if (!slot->signaled) {
        return 0;
    }
    VortekGpuTrack_releaseCompletionFence((void*)slot->fence);
    slot->used = 0;
    slot->submitted = 0;
    slot->signaled = 0;
    slot->submission_serial = 0;
    /* generation kept until next acquire (bumped there) so stale waiters can detect. */
    (void)device;
    return 1;
}

static VkFence bachata_acquire_internal_fence(VkDevice device) {
    if (!device) {
        return VK_NULL_HANDLE;
    }
    bachata_note_track_device(device);
    for (int i = 0; i < g_bachata_internal_fence_count; ++i) {
        BachataInternalFenceSlot* slot = &g_bachata_internal_fence_slots[i];
        if (slot->used || slot->fence == VK_NULL_HANDLE || slot->active_waiters > 0) {
            continue;
        }
        if (vulkanWrapper.vkResetFences(device, 1, &slot->fence) != VK_SUCCESS) {
            continue;
        }
        slot->used = 1;
        slot->submitted = 0;
        slot->signaled = 0;
        slot->submission_serial = 0;
        slot->generation += 1;
        if (slot->generation == 0) {
            slot->generation = 1;
        }
        return slot->fence;
    }
    if (g_bachata_internal_fence_count >= kBachataInternalFencePool) {
        /* Last-chance: poll signaled slots with zero waiters. */
        for (int i = 0; i < g_bachata_internal_fence_count; ++i) {
            BachataInternalFenceSlot* slot = &g_bachata_internal_fence_slots[i];
            if (!slot->used || slot->active_waiters > 0 || !slot->fence) {
                continue;
            }
            VkResult st = vulkanWrapper.vkGetFenceStatus(device, slot->fence);
            if (st == VK_SUCCESS) {
                slot->signaled = 1;
                VortekGpuTrack_noteFenceStatus((void*)slot->fence, (int)st);
                if (bachata_try_recycle_fence_slot(device, slot)) {
                    if (vulkanWrapper.vkResetFences(device, 1, &slot->fence) == VK_SUCCESS) {
                        slot->used = 1;
                        slot->submitted = 0;
                        slot->signaled = 0;
                        slot->submission_serial = 0;
                        slot->generation += 1;
                        if (slot->generation == 0) {
                            slot->generation = 1;
                        }
                        return slot->fence;
                    }
                }
            }
        }
        __android_log_print(ANDROID_LOG_WARN, "Bachata.Vortek.GpuTrack",
                            "INTERNAL_FENCE_POOL_EXHAUSTED count=%d",
                            g_bachata_internal_fence_count);
        return VK_NULL_HANDLE;
    }
    VkFenceCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vulkanWrapper.vkCreateFence(device, &ci, NULL, &fence) != VK_SUCCESS || !fence) {
        return VK_NULL_HANDLE;
    }
    int idx = g_bachata_internal_fence_count++;
    BachataInternalFenceSlot* slot = &g_bachata_internal_fence_slots[idx];
    memset(slot, 0, sizeof(*slot));
    slot->fence = fence;
    slot->used = 1;
    slot->generation = 1;
    return fence;
}

static void bachata_bind_internal_fence_serial(VkFence fence, uint64_t submission_serial) {
    BachataInternalFenceSlot* slot = bachata_find_fence_slot(fence);
    if (!slot) {
        return;
    }
    slot->submission_serial = submission_serial;
    slot->submitted = 1;
    slot->signaled = 0;
}

static void bachata_release_internal_fence(VkFence fence) {
    if (!fence) {
        return;
    }
    BachataInternalFenceSlot* slot = bachata_find_fence_slot(fence);
    if (!slot) {
        return;
    }
    if (slot->active_waiters > 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Bachata.Vortek.GpuTrack",
                            "FENCE_RECYCLED_WHILE_REFERENCED fence=%p generation=%" PRIu64
                            " waiters=%u (deferred)",
                            (void*)fence, slot->generation, slot->active_waiters);
        /* Mark signaled so waiter path can recycle after last waiter leaves. */
        slot->signaled = 1;
        return;
    }
    slot->signaled = 1;
    bachata_try_recycle_fence_slot(g_bachata_track_device, slot);
}

static void bachata_poll_completion_fences(VkDevice device) {
    if (!device && g_bachata_track_device) {
        device = g_bachata_track_device;
    }
    if (!device) {
        return;
    }
    void* pending[kBachataInternalFencePool];
    int n = VortekGpuTrack_fillPendingCompletionFences(pending, kBachataInternalFencePool);
    for (int i = 0; i < n; ++i) {
        VkFence fence = (VkFence)pending[i];
        if (!fence) {
            continue;
        }
        VkResult st = vulkanWrapper.vkGetFenceStatus(device, fence);
        VortekGpuTrack_noteFenceStatus((void*)fence, (int)st);
        if (st == VK_SUCCESS) {
            BachataInternalFenceSlot* slot = bachata_find_fence_slot(fence);
            if (slot) {
                slot->signaled = 1;
                bachata_try_recycle_fence_slot(device, slot);
            }
        }
    }
    /* Whole-pool scan: retire signaled internal fences with zero waiters. */
    for (int j = 0; j < g_bachata_internal_fence_count; ++j) {
        BachataInternalFenceSlot* slot = &g_bachata_internal_fence_slots[j];
        if (!slot->used || slot->fence == VK_NULL_HANDLE || slot->active_waiters > 0) {
            continue;
        }
        VkResult st = vulkanWrapper.vkGetFenceStatus(device, slot->fence);
        if (st == VK_SUCCESS) {
            VortekGpuTrack_noteFenceStatus((void*)slot->fence, (int)st);
            slot->signaled = 1;
            bachata_try_recycle_fence_slot(device, slot);
        }
    }
}

/* Exact wait with generation check + slow-wait log. Returns vkWait result. */
static VkResult bachata_wait_fences_exact(VkDevice device, VkFence* fences, int n,
                                          uint64_t timeout_ns) {
    if (!device || !fences || n <= 0) {
        return VK_SUCCESS;
    }
    uint64_t gens[32];
    int slot_n = n > 32 ? 32 : n;
    for (int i = 0; i < slot_n; ++i) {
        BachataInternalFenceSlot* slot = bachata_find_fence_slot(fences[i]);
        gens[i] = slot ? slot->generation : 0;
        if (slot) {
            slot->active_waiters++;
        }
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    VkResult waitResult =
        vulkanWrapper.vkWaitForFences(device, (uint32_t)n, fences, VK_TRUE, timeout_ns);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed_ms =
        (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    for (int i = 0; i < slot_n; ++i) {
        BachataInternalFenceSlot* slot = bachata_find_fence_slot(fences[i]);
        if (!slot) {
            continue;
        }
        if (slot->generation != gens[i] && gens[i] != 0) {
            __android_log_print(ANDROID_LOG_ERROR, "Bachata.Vortek.GpuTrack",
                                "SERIAL_FENCE_GENERATION_MISMATCH fence=%p waitGen=%" PRIu64
                                " nowGen=%" PRIu64 " serial=%" PRIu64,
                                (void*)fences[i], gens[i], slot->generation,
                                slot->submission_serial);
        }
        if (slot->active_waiters > 0) {
            slot->active_waiters--;
        }
        if (waitResult == VK_SUCCESS) {
            slot->signaled = 1;
            if (slot->active_waiters == 0) {
                bachata_try_recycle_fence_slot(device, slot);
            }
        }
        if (elapsed_ms > 100) {
            __android_log_print(ANDROID_LOG_WARN, "Bachata.Vortek.GpuTrack",
                                "TARGETED_FENCE_WAIT_SLOW fence=%p fenceGeneration=%" PRIu64
                                " serial=%" PRIu64 " status=%d elapsedMs=%ld",
                                (void*)fences[i], slot->generation, slot->submission_serial,
                                (int)waitResult, elapsed_ms);
        }
    }
    if (waitResult == VK_SUCCESS) {
        /* Detect fence signaled but serial not advanced — soft check via note. */
        for (int i = 0; i < n; ++i) {
            VortekGpuTrack_noteFenceStatus((void*)fences[i], (int)VK_SUCCESS);
        }
    }
    return waitResult;
}

static void bachata_drain_deferred_destroys(void) {
    bachata_poll_completion_fences(g_bachata_track_device);
    VortekGpuTrack_collectRetired();
    int kind = 0;
    void* handle = NULL;
    void* device = NULL;
    ResourceMemory* rm = NULL;
    while (VortekGpuTrack_takeDeferredDestroy(&kind, &handle, &device, &rm)) {
        if (kind == 1 && handle) {
            for (int i = 0; i < g_bachata_pool_slot_count; ++i) {
                if (g_bachata_pool_slots[i].used &&
                    g_bachata_pool_slots[i].owner_buffer == handle) {
                    g_bachata_pool_slots[i].busy = 0;
                    g_bachata_pool_slots[i].owner_buffer = NULL;
                    VortekGpuTrack_notePoolSlotFreed(i);
                }
            }
            vulkanWrapper.vkDestroyBuffer((VkDevice)device, (VkBuffer)handle, NULL);
        } else if (kind == 2 && handle) {
            if (/* texture decoder path not available here */ 0) {
            } else {
                vulkanWrapper.vkDestroyImage((VkDevice)device, (VkImage)handle, NULL);
            }
        } else if (kind == 4 && handle) {
            vulkanWrapper.vkDestroyImageView((VkDevice)device, (VkImageView)handle, NULL);
        } else if (kind == 3 && rm && device) {
            /* Memory already past lifetime gate; free without re-defer. */
            ResourceMemory_free(NULL, (VkDevice)device, rm);
        }
    }
}

/* Mode C / class-specific diagnostic wait. Never hold tracker lock across this. */
static void bachata_maybe_wait_idle_device(VkDevice device, VortekWaitIdleClass klass,
                                           const char* reason, void* resourceHint) {
    if (!device || !VortekGpuTrack_shouldWaitIdleBefore(klass)) {
        return;
    }
    __android_log_print(ANDROID_LOG_WARN, "Bachata.Vortek.GpuTrack",
                        "WAIT_IDLE_BEFORE_DESTROY_BEGIN resource=%p reason=%s class=%d",
                        resourceHint, reason ? reason : "?", (int)klass);
    VkResult waitResult = vulkanWrapper.vkDeviceWaitIdle(device);
    __android_log_print(ANDROID_LOG_WARN, "Bachata.Vortek.GpuTrack",
                        "WAIT_IDLE_BEFORE_DESTROY_END resource=%p result=%d reason=%s",
                        resourceHint, (int)waitResult, reason ? reason : "?");
    /* Tracker accuracy only — not the production fix. */
    if (waitResult == VK_SUCCESS) {
        VortekGpuTrack_markAllSubmittedWorkCompleted();
    }
}

/* Suballoc overlap: exact fence wait first (durable map); QueueWaitIdle last resort only. */
static void bachata_wait_suballoc_overlap(VkDevice device) {
    if (!device || !VortekGpuTrack_waitOnSuballocOverlapEnabled()) {
        return;
    }
    void* fence_ptrs[32];
    uint64_t max_sub = 0;
    int qi = -1;
    uint64_t qserial = 0;
    int n = VortekGpuTrack_fillSuballocOverlapWaitFences(fence_ptrs, 32, &max_sub, &qi, &qserial);
    /* No pending overlap stashed by onBindBuffer. */
    if (n <= 0 && max_sub == 0 && qserial == 0) {
        return;
    }

    const uint64_t kTimeoutNs = 5000000000ull;
    VkResult waitResult = VK_SUCCESS;
    int waited = 0;
    int used_queue_idle = 0;

    if (n > 0) {
        VkFence fences[32];
        for (int i = 0; i < n; ++i) {
            fences[i] = (VkFence)fence_ptrs[i];
        }
        waitResult = bachata_wait_fences_exact(device, fences, n, kTimeoutNs);
        VortekGpuTrack_noteFencesWait(fence_ptrs, (uint32_t)n, 1, (int)waitResult);
        VortekGpuTrack_noteExactFenceWait();
        waited = n;
    } else {
        /* Poll path: drain signaled fences until serial completes or retries end. */
        for (int attempt = 0; attempt < 80 && VortekGpuTrack_suballocOverlapStillIncomplete();
             ++attempt) {
            bachata_poll_completion_fences(device);
            n = VortekGpuTrack_fillSuballocOverlapWaitFences(fence_ptrs, 32, &max_sub, &qi,
                                                             &qserial);
            if (n > 0) {
                VkFence fences[32];
                for (int i = 0; i < n; ++i) {
                    fences[i] = (VkFence)fence_ptrs[i];
                }
                waitResult = bachata_wait_fences_exact(device, fences, n, kTimeoutNs);
                VortekGpuTrack_noteFencesWait(fence_ptrs, (uint32_t)n, 1, (int)waitResult);
                VortekGpuTrack_noteExactFenceWait();
                waited = n;
                break;
            }
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000L}; /* 2ms */
            nanosleep(&ts, NULL);
        }
    }

    /* Last-resort: queue wait-idle on owning queue only (never DeviceWaitIdle). */
    if (VortekGpuTrack_suballocOverlapStillIncomplete()) {
        VkQueue q = (VkQueue)VortekGpuTrack_suballocOverlapQueue();
        if (q && vulkanWrapper.vkQueueWaitIdle) {
            __android_log_print(ANDROID_LOG_WARN, "Bachata.Vortek.GpuTrack",
                                "SUBALLOC_TARGETED_WAIT_QUEUE_IDLE queue=%p serial=%" PRIu64,
                                (void*)q, qserial);
            VkResult qr = vulkanWrapper.vkQueueWaitIdle(q);
            VortekGpuTrack_noteQueueWaitIdle((void*)q, (int)qr);
            VortekGpuTrack_noteQueueIdleFallback();
            used_queue_idle = 1;
            waitResult = qr;
            /* Queue idle ⇒ all prior work done; recycle every signaled internal fence. */
            if (qr == VK_SUCCESS) {
                bachata_poll_completion_fences(device);
            }
        }
    }

    VortekGpuTrack_noteSuballocTargetedWait(max_sub, qi, qserial, waited, (int)waitResult);
    (void)used_queue_idle;
}

/* Try physical pool redirect when guest range busy (R2 multi-slot). Returns 1 if bound.
 * Disabled by default: rebinding away from guest VkDeviceMemory desyncs Map/Flush of the
 * guest allocation (CPU writes land on guest mem, GPU reads empty pool mem → freeze).
 * Enable only with debug.bachata.suballoc_physical_pool=1 for experiments. */
static int bachata_physical_pool_enabled(void) {
    char buf[92];
    buf[0] = '\0';
#ifdef __ANDROID__
    if (__system_property_get("debug.bachata.suballoc_physical_pool", buf) > 0) {
        return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T';
    }
#endif
    const char* e = getenv("BACHATA_SUBALLOC_PHYSICAL_POOL");
    return e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y');
}

static int bachata_try_pool_bind(VkDevice device, VkBuffer buffer, ResourceMemory* guestRm,
                                 VkDeviceSize guestOffset, VkDeviceSize size,
                                 uint32_t memoryTypeIndex, VkDeviceMemory* outMem,
                                 VkDeviceSize* outOffset) {
    if (!device || !buffer || !outMem || !outOffset) {
        return 0;
    }
    if (!VortekGpuTrack_suballocRangePoolEnabled() || !bachata_physical_pool_enabled()) {
        return 0;
    }
    /* Only when a wait was stashed (busy range). */
    void* dummy[1];
    uint64_t max_sub = 0;
    int qi = -1;
    uint64_t qserial = 0;
    int n = VortekGpuTrack_fillSuballocOverlapWaitFences(dummy, 0, &max_sub, &qi, &qserial);
    (void)n;
    if (max_sub == 0 && qserial == 0) {
        return 0;
    }

    /* Prefer free existing slot of sufficient size + matching type. */
    for (int i = 0; i < g_bachata_pool_slot_count; ++i) {
        BachataSuballocPoolSlot* s = &g_bachata_pool_slots[i];
        if (!s->used || s->busy) {
            continue;
        }
        if (s->memoryTypeIndex != memoryTypeIndex || s->size < size) {
            continue;
        }
        s->busy = 1;
        s->owner_buffer = (void*)buffer;
        s->generation++;
        *outMem = s->memory;
        *outOffset = 0;
        VortekGpuTrack_notePoolSlotBound(i, (void*)buffer, (void*)s->memory, (uint64_t)size,
                                         s->generation);
        /* Clear pending wait — redirected off the busy guest range. */
        VortekGpuTrack_noteSuballocTargetedWait(0, -1, 0, 0, 0);
        (void)guestRm;
        (void)guestOffset;
        return 1;
    }

    /* Grow pool within tracker policy. */
    if (!VortekGpuTrack_tryReservePoolSlot((uint64_t)size, memoryTypeIndex, NULL, NULL)) {
        return 0;
    }
    if (g_bachata_pool_slot_count >= kBachataSuballocPoolMax) {
        return 0;
    }

    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = size;
    ai.memoryTypeIndex = memoryTypeIndex;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vulkanWrapper.vkAllocateMemory(device, &ai, NULL, &mem) != VK_SUCCESS || !mem) {
        return 0;
    }
    int idx = g_bachata_pool_slot_count++;
    BachataSuballocPoolSlot* s = &g_bachata_pool_slots[idx];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->busy = 1;
    s->memory = mem;
    s->size = size;
    s->memoryTypeIndex = memoryTypeIndex;
    s->generation = 1;
    s->owner_buffer = (void*)buffer;
    g_bachata_pool_bytes += (uint64_t)size;
    VortekGpuTrack_notePoolGrown(g_bachata_pool_slot_count, g_bachata_pool_bytes);
    VortekGpuTrack_notePoolSlotBound(idx, (void*)buffer, (void*)mem, (uint64_t)size, s->generation);
    *outMem = mem;
    *outOffset = 0;
    VortekGpuTrack_noteSuballocTargetedWait(0, -1, 0, 0, 0);
    return 1;
}
#endif

#define MSG_DEBUG_UNIMPLEMENTED_FUNC "%s not implemented yet\n"

void vt_handle_vkCreateInstance(VkContext* context) {
    VkInstanceCreateInfo createInfo = {0};
    vt_unserialize_vkCreateInstance(&createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);

    const char* skipExtensions[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};

#if ENABLE_VALIDATION_LAYER
    createInfo.ppEnabledLayerNames = validationLayers;
    createInfo.enabledLayerCount = ARRAY_SIZE(validationLayers);

    const char* extraExtensions[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities", "VK_KHR_external_fence_capabilities", "VK_EXT_debug_report"};
#else
    const char* extraExtensions[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities", "VK_KHR_external_fence_capabilities"};
#endif

    injectExtensions(context, (char***)&createInfo.ppEnabledExtensionNames, &createInfo.enabledExtensionCount,
                     extraExtensions, ARRAY_SIZE(extraExtensions),
                     skipExtensions, ARRAY_SIZE(skipExtensions));

#ifdef BACHATA_VORTEK_SERVER
    /* debug_utils only when address-binding report dig enabled (default OFF). */
    if (VortekGpuTrack_gpuAddressBindingReportEnabled()) {
        bool hasDebugUtils = false;
        uint32_t instExtCount = 0;
        if (vulkanWrapper.vkEnumerateInstanceExtensionProperties(NULL, &instExtCount, NULL) ==
                VK_SUCCESS &&
            instExtCount > 0) {
            VkExtensionProperties* props = calloc(instExtCount, sizeof(VkExtensionProperties));
            if (props &&
                vulkanWrapper.vkEnumerateInstanceExtensionProperties(NULL, &instExtCount, props) ==
                    VK_SUCCESS) {
                for (uint32_t i = 0; i < instExtCount; ++i) {
                    if (strcmp(props[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                        hasDebugUtils = true;
                        break;
                    }
                }
            }
            free(props);
        }
        if (hasDebugUtils) {
            const char* digInst[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
            injectExtensions(context, (char***)&createInfo.ppEnabledExtensionNames,
                             &createInfo.enabledExtensionCount, digInst, 1, NULL, 0);
        }
    }
#endif

    VkInstance instance;
    VkResult result = vulkanWrapper.vkCreateInstance(&createInfo, NULL, &instance);
    if (result == VK_SUCCESS) initVulkanInstance(context, instance, createInfo.pApplicationInfo);

    VT_SERIALIZE_CMD(VkInstance, instance);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyInstance(VkContext* context) {
    uint64_t instanceId;
    vt_unserialize_vkDestroyInstance((VkInstance)&instanceId, NULL, context->inputBuffer, &context->memoryPool);
    VkInstance instance = VkObject_fromId(instanceId);

#if ENABLE_VALIDATION_LAYER
    if (context->debugReportCallback) {
        vulkanWrapper.vkDestroyDebugReportCallback(instance, context->debugReportCallback, NULL);
        context->debugReportCallback = VK_NULL_HANDLE;
    }
#endif
#ifdef BACHATA_VORTEK_SERVER
    if (context->debugUtilsMessenger && vulkanWrapper.vkDestroyDebugUtilsMessengerEXT) {
        vulkanWrapper.vkDestroyDebugUtilsMessengerEXT(instance, context->debugUtilsMessenger, NULL);
        context->debugUtilsMessenger = VK_NULL_HANDLE;
    }
#endif

    vulkanWrapper.vkDestroyInstance(instance, NULL);
}

void vt_handle_vkEnumeratePhysicalDevices(VkContext* context) {
    uint32_t physicalDeviceCount;
    uint64_t instanceId;
    vt_unserialize_vkEnumeratePhysicalDevices((VkInstance)&instanceId, &physicalDeviceCount, NULL, context->inputBuffer, &context->memoryPool);
    VkInstance instance = VkObject_fromId(instanceId);

    VkPhysicalDevice* physicalDevices = physicalDeviceCount > 0 ? calloc(physicalDeviceCount, sizeof(VkPhysicalDevice)) : NULL;
    VkResult result = vulkanWrapper.vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices);

    VT_SERIALIZE_CMD(vkEnumeratePhysicalDevices, VK_NULL_HANDLE, &physicalDeviceCount, physicalDevices);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);

    MEMFREE(physicalDevices);
}

void vt_handle_vkGetPhysicalDeviceProperties(VkContext* context) {
    uint64_t physicalDeviceId;
    vt_unserialize_VkPhysicalDevice((VkPhysicalDevice)&physicalDeviceId, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkPhysicalDeviceProperties properties = {0};
    vulkanWrapper.vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    checkDeviceProperties(context, &properties, NULL);

    VT_SERIALIZE_CMD(VkPhysicalDeviceProperties, &properties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceQueueFamilyProperties(VkContext* context) {
    uint64_t physicalDeviceId;
    uint32_t queueFamilyPropertyCount;

    vt_unserialize_vkGetPhysicalDeviceQueueFamilyProperties((VkPhysicalDevice)&physicalDeviceId, &queueFamilyPropertyCount, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkQueueFamilyProperties* queueFamilyProperties = queueFamilyPropertyCount > 0 ? calloc(queueFamilyPropertyCount, sizeof(VkQueueFamilyProperties)) : NULL;
    vulkanWrapper.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyPropertyCount, queueFamilyProperties);

    VT_SERIALIZE_CMD(vkGetPhysicalDeviceQueueFamilyProperties, NULL, &queueFamilyPropertyCount, queueFamilyProperties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);

    MEMFREE(queueFamilyProperties);
}

void vt_handle_vkGetPhysicalDeviceMemoryProperties(VkContext* context) {
    uint64_t physicalDeviceId;
    vt_unserialize_VkPhysicalDevice((VkPhysicalDevice)&physicalDeviceId, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkPhysicalDeviceMemoryProperties memoryProperties = {0};
    vulkanWrapper.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    checkDeviceMemoryProperties(context, &memoryProperties, NULL);

    VT_SERIALIZE_CMD(VkPhysicalDeviceMemoryProperties, &memoryProperties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceFeatures(VkContext* context) {
    uint64_t physicalDeviceId;
    vt_unserialize_VkPhysicalDevice((VkPhysicalDevice)&physicalDeviceId, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkPhysicalDeviceFeatures features = {0};
    vulkanWrapper.vkGetPhysicalDeviceFeatures(physicalDevice, &features);
    checkDeviceFeatures(&features, NULL);

    VT_SERIALIZE_CMD(VkPhysicalDeviceFeatures, &features);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceFormatProperties(VkContext* context) {
    uint64_t physicalDeviceId;
    VkFormat format;

    vt_unserialize_vkGetPhysicalDeviceFormatProperties((VkPhysicalDevice)&physicalDeviceId, &format, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkFormatProperties formatProperties = {0};
    vulkanWrapper.vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &formatProperties);
    checkFormatProperties(physicalDevice, format, &formatProperties);

    VT_SERIALIZE_CMD(VkFormatProperties, &formatProperties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceImageFormatProperties(VkContext* context) {
    uint64_t physicalDeviceId;
    VkFormat format;
    VkImageType type;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    VkImageCreateFlags flags;

    vt_unserialize_vkGetPhysicalDeviceImageFormatProperties((VkPhysicalDevice)&physicalDeviceId, &format, &type, &tiling, &usage, &flags, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkImageFormatProperties imageFormatProperties = {0};
    VkResult result = vulkanWrapper.vkGetPhysicalDeviceImageFormatProperties(physicalDevice, format, type, tiling, usage, flags, &imageFormatProperties);
    checkImageFormatProperties(format, type, tiling, usage, flags, &imageFormatProperties, &result);

    VT_SERIALIZE_CMD(VkImageFormatProperties, &imageFormatProperties);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkCreateDevice(VkContext* context) {
    uint64_t physicalDeviceId;
    VkDeviceCreateInfo createInfo = {0};
    vt_unserialize_vkCreateDevice((VkPhysicalDevice)&physicalDeviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    disableUnsupportedDeviceFeatures(physicalDevice, &createInfo);

    const char* extraExtensions[] = {"VK_KHR_get_memory_requirements2", "VK_KHR_dedicated_allocation", "VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_KHR_external_fence", "VK_KHR_external_fence_fd", "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign"};
    injectExtensions(context, (char***)&createInfo.ppEnabledExtensionNames, &createInfo.enabledExtensionCount,
                     extraExtensions, ARRAY_SIZE(extraExtensions),
                     globalImplementedDeviceExtensions, ARRAY_SIZE(globalImplementedDeviceExtensions));

#ifdef BACHATA_VORTEK_SERVER
    /* Probe optional mapping-lifetime dig extensions (never fail CreateDevice if missing). */
    const bool wantAddrBind = VortekGpuTrack_gpuAddressBindingReportEnabled();
    const bool wantDeviceFault = VortekGpuTrack_deviceFaultQueryWanted();
    bool hasAddrBind = false;
    bool hasDeviceFault = false;
    {
        uint32_t extCount = 0;
        if (vulkanWrapper.vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &extCount, NULL) ==
                VK_SUCCESS &&
            extCount > 0) {
            VkExtensionProperties* props = calloc(extCount, sizeof(VkExtensionProperties));
            if (props &&
                vulkanWrapper.vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &extCount,
                                                                   props) == VK_SUCCESS) {
                for (uint32_t i = 0; i < extCount; ++i) {
                    if (wantAddrBind &&
                        strcmp(props[i].extensionName,
                               VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME) == 0) {
                        hasAddrBind = true;
                    }
                    if (wantDeviceFault &&
                        strcmp(props[i].extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) == 0) {
                        hasDeviceFault = true;
                    }
                }
            }
            free(props);
        }
    }
    if (hasAddrBind) {
        const char* digExts[] = {VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME};
        injectExtensions(context, (char***)&createInfo.ppEnabledExtensionNames,
                         &createInfo.enabledExtensionCount, digExts, 1, NULL, 0);
    }
    if (hasDeviceFault) {
        const char* digExts[] = {VK_EXT_DEVICE_FAULT_EXTENSION_NAME};
        injectExtensions(context, (char***)&createInfo.ppEnabledExtensionNames,
                         &createInfo.enabledExtensionCount, digExts, 1, NULL, 0);
    }
    VkPhysicalDeviceAddressBindingReportFeaturesEXT addrBindFeatures = {0};
    if (hasAddrBind) {
        addrBindFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT;
        addrBindFeatures.reportAddressBinding = VK_TRUE;
        addrBindFeatures.pNext = (void*)createInfo.pNext;
        createInfo.pNext = &addrBindFeatures;
    }
    println("device dig extensions address_binding=%d device_fault=%d wantAddr=%d wantFault=%d",
            hasAddrBind ? 1 : 0, hasDeviceFault ? 1 : 0, wantAddrBind ? 1 : 0,
            wantDeviceFault ? 1 : 0);
    println("GPU_ADDRESS_BINDING_REPORT enabled=%d", wantAddrBind ? 1 : 0);
    println("DEVICE_FAULT_QUERY enabled=%d", wantDeviceFault ? 1 : 0);
#endif

    VkDevice device;
    VkResult result = vulkanWrapper.vkCreateDevice(physicalDevice, &createInfo, NULL, &device);
    if (result == VK_SUCCESS) {
        initVulkanDevice(context, physicalDevice, device);
#ifdef BACHATA_VORTEK_SERVER
        bachata_note_track_device(device);
        if (VortekGpuTrack_deviceFaultQueryWanted() && vulkanWrapper.vkGetDeviceFaultInfoEXT) {
            VortekGpuTrack_registerDeviceFaultQuery((void*)device,
                                                    (void*)vulkanWrapper.vkGetDeviceFaultInfoEXT);
        } else {
            VortekGpuTrack_registerDeviceFaultQuery((void*)device, NULL);
        }
#endif
    }

    VT_SERIALIZE_CMD(VkDevice, device);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyDevice(VkContext* context) {
    uint64_t deviceId;

    vt_unserialize_vkDestroyDevice((VkDevice)&deviceId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    vulkanWrapper.vkDestroyDevice(device, NULL);
}

void vt_handle_vkEnumerateInstanceVersion(VkContext* context) {
    vt_send(context->clientRing, VK_SUCCESS, &context->vkMaxVersion, 4);
}

void vt_handle_vkEnumerateInstanceExtensionProperties(VkContext* context) {
    uint32_t propertyCount;
    vt_unserialize_vkEnumerateInstanceExtensionProperties(NULL, &propertyCount, NULL, context->inputBuffer, &context->memoryPool);
    VkResult result;

    uint32_t exposedExtensionCount;
    result = vulkanWrapper.vkEnumerateInstanceExtensionProperties(NULL, &exposedExtensionCount, NULL);

    VkExtensionProperties* exposedExtensions = vt_alloc(&context->memoryPool, propertyCount * sizeof(VkExtensionProperties));
    result = vulkanWrapper.vkEnumerateInstanceExtensionProperties(NULL, &exposedExtensionCount, exposedExtensions);

    const char* extraExtensions[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    const char* skipExtensions[] = {"VK_KHR_android_surface"};
    injectExtensions2(context, &exposedExtensions, &exposedExtensionCount,
                      extraExtensions, ARRAY_SIZE(extraExtensions),
                      skipExtensions, ARRAY_SIZE(skipExtensions));

    bool nullProperties = propertyCount == 0;
    VkExtensionProperties* properties = !nullProperties ? exposedExtensions : NULL;
    if (nullProperties) propertyCount = exposedExtensionCount;

    VT_SERIALIZE_CMD(vkEnumerateInstanceExtensionProperties, NULL, &propertyCount, properties);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkEnumerateDeviceExtensionProperties(VkContext* context) {
    uint64_t physicalDeviceId;
    uint32_t propertyCount;

    vt_unserialize_vkEnumerateDeviceExtensionProperties((VkPhysicalDevice)&physicalDeviceId, NULL, &propertyCount, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    bool nullProperties = propertyCount == 0;
    VkExtensionProperties* properties = getExposedDeviceExtensionProperties(context, physicalDevice, &propertyCount);
    VkResult result = properties ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;

    VT_SERIALIZE_CMD(vkEnumerateDeviceExtensionProperties, NULL, NULL, &propertyCount, !nullProperties ? properties : NULL);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkGetDeviceQueue(VkContext* context) {
    uint64_t deviceId;
    uint32_t queueFamilyIndex;
    uint32_t queueIndex;

    vt_unserialize_vkGetDeviceQueue((VkDevice)&deviceId, &queueFamilyIndex, &queueIndex, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
#ifdef BACHATA_VORTEK_SERVER
    bachata_note_track_device(device);
#endif

    VkQueue queue;
    vulkanWrapper.vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, &queue);

    VT_SERIALIZE_CMD(VkQueue, queue);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkQueueSubmit(VkContext* context) {
    uint64_t queueId;
    uint32_t submitCount;
    uint64_t fenceId;

    vt_unserialize_vkQueueSubmit((VkQueue)&queueId, &submitCount, NULL, (VkFence)&fenceId, context->inputBuffer, &context->memoryPool);
    VkQueue queue = VkObject_fromId(queueId);
    VkFence fence = VkObject_fromId(fenceId);

    VkSubmitInfo submits[submitCount];
    vt_unserialize_vkQueueSubmit(VK_NULL_HANDLE, NULL, submits, VK_NULL_HANDLE, context->inputBuffer, &context->memoryPool);

    bool clientWaiting = RingBuffer_hasStatus(context->clientRing, RING_STATUS_WAIT);
    if (context->textureDecoder) TextureDecoder_decodeAll(context->textureDecoder);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_noteQueue(queue);
    uint64_t submission = VortekGpuTrack_beginSubmission();
    for (uint32_t si = 0; si < submitCount; si++) {
        for (uint32_t ci = 0; ci < submits[si].commandBufferCount; ci++) {
            VortekGpuTrack_noteSubmitCommandBuffer(submits[si].pCommandBuffers[ci]);
        }
    }
    /* Internal completion token when guest fence is null. */
    VkFence submitFence = fence;
    VkFence internalFence = VK_NULL_HANDLE;
    if (fence == VK_NULL_HANDLE && g_bachata_track_device) {
        internalFence = bachata_acquire_internal_fence(g_bachata_track_device);
        if (internalFence != VK_NULL_HANDLE) {
            submitFence = internalFence;
        } else {
            __android_log_print(ANDROID_LOG_ERROR, "Bachata.Vortek.GpuTrack",
                                "SUBMISSION_WITHOUT_COMPLETION_TOKEN submission=%" PRIu64
                                " queue=%p (internal fence pool exhausted)",
                                submission, (void*)queue);
        }
    }
#endif
    VkResult result = vulkanWrapper.vkQueueSubmit(queue, submitCount, submits,
#ifdef BACHATA_VORTEK_SERVER
                                                  submitFence
#else
                                                  fence
#endif
    );
    if (result == VK_ERROR_DEVICE_LOST) context->status = result;
#ifdef BACHATA_VORTEK_SERVER
    if (internalFence != VK_NULL_HANDLE && result == VK_SUCCESS) {
        bachata_bind_internal_fence_serial(internalFence, submission);
        VortekGpuTrack_bindSubmissionCompletion(submission, (void*)internalFence,
                                                VORTEK_COMPLETION_INTERNAL_FENCE);
    } else if (fence != VK_NULL_HANDLE) {
        VortekGpuTrack_bindSubmissionCompletion(submission, (void*)fence,
                                                VORTEK_COMPLETION_CALLER_FENCE);
    } else {
        VortekGpuTrack_bindSubmissionFence(submission, fence);
        if (internalFence != VK_NULL_HANDLE) {
            /* Submit failed — recycle internal fence. */
            BachataInternalFenceSlot* failed = bachata_find_fence_slot(internalFence);
            if (failed) {
                failed->signaled = 1; /* allow recycle without GetFenceStatus */
                failed->submitted = 0;
            }
            bachata_release_internal_fence(internalFence);
        } else if (result == VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "Bachata.Vortek.GpuTrack",
                                "SUBMISSION_WITHOUT_COMPLETION_TOKEN submission=%" PRIu64
                                " result=success fence=null",
                                submission);
        }
    }
    VortekGpuTrack_endSubmission(submission, (int)result, result == VK_ERROR_DEVICE_LOST);
    bachata_drain_deferred_destroys();
#endif

    if (clientWaiting) vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkQueueWaitIdle(VkContext* context) {
    uint64_t queueId;
    vt_unserialize_VkQueue((VkQueue)&queueId, context->inputBuffer, &context->memoryPool);
    VkQueue queue = VkObject_fromId(queueId);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_noteQueue(queue);
#endif
    VkResult result = vulkanWrapper.vkQueueWaitIdle(queue);
#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_noteQueueWaitIdle(queue, (int)result);
    bachata_drain_deferred_destroys();
#endif
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkDeviceWaitIdle(VkContext* context) {
    uint64_t deviceId;
    vt_unserialize_VkDevice((VkDevice)&deviceId, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkResult result = vulkanWrapper.vkDeviceWaitIdle(device);
#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_noteDeviceWaitIdle((int)result);
    bachata_drain_deferred_destroys();
#endif
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkAllocateMemory(VkContext* context) {
    uint64_t deviceId;
    VkMemoryAllocateInfo allocateInfo = {0};
    vt_unserialize_vkAllocateMemory((VkDevice)&deviceId, &allocateInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    ResourceMemory* resourceMemory = ResourceMemory_allocate(context, device, &allocateInfo);
    VkResult result = resourceMemory ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;

    if (result == VK_SUCCESS) {
        VT_SERIALIZE_CMD(VkDeviceMemory, (VkDeviceMemory)resourceMemory);
        vt_send(context->clientRing, result, outputBuffer, bufferSize);
    }
    else vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkFreeMemory(VkContext* context) {
    uint64_t deviceId;
    uint64_t memoryId;

    vt_unserialize_vkFreeMemory((VkDevice)&deviceId, (VkDeviceMemory)&memoryId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    ResourceMemory* resourceMemory = VkObject_fromId(memoryId);

#ifdef BACHATA_VORTEK_SERVER
    bachata_note_track_device(device);
    /* Prefer external-class wait when guest uses imported backing. */
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_EXTERNAL, "free_external_or_memory",
                                   resourceMemory);
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_MEMORY, "free_memory",
                                   resourceMemory);
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_MAPPING, "unmap_on_free",
                                   resourceMemory);
#endif
    ResourceMemory_free(context, device, resourceMemory);
#ifdef BACHATA_VORTEK_SERVER
    bachata_drain_deferred_destroys();
#endif
}

void vt_handle_vkMapMemory(VkContext* context) {
    uint64_t memoryId;
    vt_unserialize_VkDeviceMemory((VkDeviceMemory)&memoryId, context->inputBuffer, &context->memoryPool);
    ResourceMemory* resourceMemory = VkObject_fromId(memoryId);

    VkResult result = resourceMemory->fd != -1 ? VK_SUCCESS : VK_ERROR_MEMORY_MAP_FAILED;
#ifdef BACHATA_VORTEK_SERVER
    if (result == VK_SUCCESS) {
        /* Protect CPU write into GPU-owned suballoc ranges (R2).
         * Whole-map: wait if any in-flight lease on this allocation, but do not
         * pin cpu_write_owner for the entire heap (would block forever). */
        if (VortekGpuTrack_prepareCpuWrite(resourceMemory, 0, 0, NULL)) {
            bachata_wait_suballoc_overlap(g_bachata_track_device);
        }
        VortekGpuTrack_onMap(resourceMemory);
    }
#endif
    send_fds(context->clientFd, &resourceMemory->fd, 1, &result, sizeof(VkResult));
}

void vt_handle_vkUnmapMemory(VkContext* context) {
    /* Guest unmap not fully wired in Vortek protocol; still audit as untracked if
     * we cannot resolve ResourceMemory. Class wait props still usable for free path. */
#ifdef BACHATA_VORTEK_SERVER
    bachata_maybe_wait_idle_device(g_bachata_track_device, VORTEK_WAIT_IDLE_MAPPING,
                                   "unmap_memory", NULL);
    bachata_maybe_wait_idle_device(g_bachata_track_device, VORTEK_WAIT_IDLE_MEMORY,
                                   "unmap_memory", NULL);
    (void)VortekGpuTrack_onUnmap(NULL, g_bachata_track_device);
#endif
    println(MSG_DEBUG_UNIMPLEMENTED_FUNC, "vkUnmapMemory");
    (void)context;
}

void vt_handle_vkFlushMappedMemoryRanges(VkContext* context) {
    uint64_t deviceId;
    uint32_t memoryRangeCount;

    vt_unserialize_vkFlushMappedMemoryRanges((VkDevice)&deviceId, &memoryRangeCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkMappedMemoryRange memoryRanges[memoryRangeCount];
    vt_unserialize_vkFlushMappedMemoryRanges(VK_NULL_HANDLE, NULL, memoryRanges, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        /* memory field may be ResourceMemory* before cast in some paths; log host flush. */
        VortekGpuTrack_noteHostFlush(NULL, (uint64_t)memoryRanges[i].offset,
                                     (uint64_t)memoryRanges[i].size);
    }
#endif
    VkResult result = vulkanWrapper.vkFlushMappedMemoryRanges(device, memoryRangeCount, memoryRanges);
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkInvalidateMappedMemoryRanges(VkContext* context) {
    uint64_t deviceId;
    uint32_t memoryRangeCount;

    vt_unserialize_vkInvalidateMappedMemoryRanges((VkDevice)&deviceId, &memoryRangeCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkMappedMemoryRange memoryRanges[memoryRangeCount];
    vt_unserialize_vkInvalidateMappedMemoryRanges(VK_NULL_HANDLE, NULL, memoryRanges, context->inputBuffer, &context->memoryPool);

    VkResult result = vulkanWrapper.vkInvalidateMappedMemoryRanges(device, memoryRangeCount, memoryRanges);
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkGetDeviceMemoryCommitment(VkContext* context) {
    uint64_t deviceId;
    uint64_t memoryId;

    vt_unserialize_vkGetDeviceMemoryCommitment((VkDevice)&deviceId, (VkDeviceMemory)&memoryId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    ResourceMemory* resourceMemory = VkObject_fromId(memoryId);

    VkDeviceSize committedMemoryInBytes;
    vulkanWrapper.vkGetDeviceMemoryCommitment(device, resourceMemory->memory, &committedMemoryInBytes);

    vt_send(context->clientRing, VK_SUCCESS, &committedMemoryInBytes, sizeof(VkDeviceSize));
}

void vt_handle_vkGetBufferMemoryRequirements(VkContext* context) {
    uint64_t deviceId;
    uint64_t bufferId;

    vt_unserialize_vkGetBufferMemoryRequirements((VkDevice)&deviceId, (VkBuffer)&bufferId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkBuffer buffer = VkObject_fromId(bufferId);

    VkMemoryRequirements memoryRequirements = {0};
    vulkanWrapper.vkGetBufferMemoryRequirements(device, buffer, &memoryRequirements);

    VT_SERIALIZE_CMD(VkMemoryRequirements, &memoryRequirements);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkBindBufferMemory(VkContext* context) {
    uint64_t deviceId;
    uint64_t bufferId;
    uint64_t memoryId;
    VkDeviceSize memoryOffset;

    vt_unserialize_vkBindBufferMemory((VkDevice)&deviceId, (VkBuffer)&bufferId, (VkDeviceMemory)&memoryId, &memoryOffset, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkBuffer buffer = VkObject_fromId(bufferId);
    ResourceMemory* resourceMemory = VkObject_fromId(memoryId);

    VkDeviceMemory bindMemory = resourceMemory->memory;
    VkDeviceSize bindOffset = memoryOffset;

#ifdef BACHATA_VORTEK_SERVER
    VkMemoryRequirements memReqs = {0};
    vulkanWrapper.vkGetBufferMemoryRequirements(device, buffer, &memReqs);
    VortekGpuTrack_onBindBuffer(buffer, resourceMemory, (uint64_t)memoryOffset,
                                (uint64_t)memReqs.size, (uint64_t)memReqs.alignment);
    bachata_note_track_device(device);
    /* R2: try multi-slot pool redirect before waiting on busy guest range. */
    {
        uint32_t typeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if ((memReqs.memoryTypeBits & (1u << typeIndex)) == 0) {
            typeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        if ((memReqs.memoryTypeBits & (1u << typeIndex)) == 0) {
            for (uint32_t ti = 0; ti < 32; ++ti) {
                if (memReqs.memoryTypeBits & (1u << ti)) {
                    typeIndex = ti;
                    break;
                }
            }
        }
        VkDeviceMemory poolMem = VK_NULL_HANDLE;
        VkDeviceSize poolOff = 0;
        if (bachata_try_pool_bind(device, buffer, resourceMemory, memoryOffset, memReqs.size,
                                  typeIndex, &poolMem, &poolOff)) {
            bindMemory = poolMem;
            bindOffset = poolOff;
        } else {
            bachata_wait_suballoc_overlap(device);
        }
    }
    VortekGpuTrack_maybeLogSuballocPoolStats();
#endif

    VkResult result = vulkanWrapper.vkBindBufferMemory(device, buffer, bindMemory, bindOffset);

    if (result == VK_SUCCESS && context->textureDecoder) {
        TextureDecoder_addBoundBuffer(context->textureDecoder, resourceMemory, buffer, memoryOffset);
    }

    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkGetImageMemoryRequirements(VkContext* context) {
    uint64_t deviceId;
    uint64_t imageId;

    vt_unserialize_vkGetImageMemoryRequirements((VkDevice)&deviceId, (VkImage)&imageId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkImage image = VkObject_fromId(imageId);

    VkMemoryRequirements memoryRequirements = {0};
    vulkanWrapper.vkGetImageMemoryRequirements(device, image, &memoryRequirements);

    VT_SERIALIZE_CMD(VkMemoryRequirements, &memoryRequirements);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkBindImageMemory(VkContext* context) {
    uint64_t deviceId;
    uint64_t imageId;
    uint64_t memoryId;
    VkDeviceSize memoryOffset;

    vt_unserialize_vkBindImageMemory((VkDevice)&deviceId, (VkImage)&imageId, (VkDeviceMemory)&memoryId, &memoryOffset, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkImage image = VkObject_fromId(imageId);
    ResourceMemory* resourceMemory = VkObject_fromId(memoryId);

#ifdef BACHATA_VORTEK_SERVER
    VkMemoryRequirements memReqs = {0};
    vulkanWrapper.vkGetImageMemoryRequirements(device, image, &memReqs);
    VortekGpuTrack_onBindImage(image, resourceMemory, (uint64_t)memoryOffset,
                               (uint64_t)memReqs.size, (uint64_t)memReqs.alignment);
#endif

    VkResult result = vulkanWrapper.vkBindImageMemory(device, image, resourceMemory->memory, memoryOffset);
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkGetImageSparseMemoryRequirements(VkContext* context) {
    uint64_t deviceId;
    uint64_t imageId;
    uint32_t requirementCount;

    vt_unserialize_vkGetImageSparseMemoryRequirements((VkDevice)&deviceId, (VkImage)&imageId, &requirementCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkImage image = VkObject_fromId(imageId);

    VkSparseImageMemoryRequirements* requirements = requirementCount > 0 ? calloc(requirementCount, sizeof(VkSparseImageMemoryRequirements)) : NULL;
    vulkanWrapper.vkGetImageSparseMemoryRequirements(device, image, &requirementCount, requirements);

    VT_SERIALIZE_CMD(vkGetImageSparseMemoryRequirements, VK_NULL_HANDLE, VK_NULL_HANDLE, &requirementCount, requirements);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);

    MEMFREE(requirements);
}

void vt_handle_vkGetPhysicalDeviceSparseImageFormatProperties(VkContext* context) {
    uint64_t physicalDeviceId;
    VkFormat format;
    VkImageType type;
    VkSampleCountFlagBits samples;
    VkImageUsageFlags usage;
    VkImageTiling tiling;
    uint32_t propertyCount;

    vt_unserialize_vkGetPhysicalDeviceSparseImageFormatProperties((VkPhysicalDevice)&physicalDeviceId, &format, &type, &samples, &usage, &tiling, &propertyCount, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkSparseImageFormatProperties* properties = propertyCount > 0 ? calloc(propertyCount, sizeof(VkSparseImageFormatProperties)) : NULL;
    vulkanWrapper.vkGetPhysicalDeviceSparseImageFormatProperties(physicalDevice, format, type, samples, usage, tiling, &propertyCount, properties);

    VT_SERIALIZE_CMD(vkGetPhysicalDeviceSparseImageFormatProperties, VK_NULL_HANDLE, format, type, samples, usage, tiling, &propertyCount, properties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);

    MEMFREE(properties);
}

void vt_handle_vkQueueBindSparse(VkContext* context) {
    uint64_t queueId;
    uint32_t bindInfoCount;
    uint64_t fenceId;

    vt_unserialize_vkQueueBindSparse((VkQueue)&queueId, &bindInfoCount, NULL, (VkFence)&fenceId, context->inputBuffer, &context->memoryPool);
    VkQueue queue = VkObject_fromId(queueId);
    VkFence fence = VkObject_fromId(fenceId);

    VkBindSparseInfo bindInfo[bindInfoCount];
    vt_unserialize_vkQueueBindSparse(VK_NULL_HANDLE, NULL, bindInfo, VK_NULL_HANDLE, context->inputBuffer, &context->memoryPool);

    VkResult result = vulkanWrapper.vkQueueBindSparse(queue, bindInfoCount, bindInfo, fence);
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkCreateFence(VkContext* context) {
    uint64_t deviceId;
    VkFenceCreateInfo createInfo = {0};

    vt_unserialize_vkCreateFence((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    /* Do not force SYNC_FD export on every guest fence. Forced export made
     * create-signaled fences unusable on Mali (and some Adreno) drivers:
     * vkGetFenceFdKHR returned a never-ready FD that the client mapped to
     * VK_ERROR_DEVICE_LOST on the first Presenter GetRenderFrame wait.
     * Preserve the guest pNext chain as-is; WaitForFences host-waits. */
    VkFence fence = VK_NULL_HANDLE;
    VkResult result = vulkanWrapper.vkCreateFence(device, &createInfo, NULL, &fence);

    VT_SERIALIZE_CMD(VkFence, fence);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyFence(VkContext* context) {
    uint64_t deviceId;
    uint64_t fenceId;

    vt_unserialize_vkDestroyFence((VkDevice)&deviceId, (VkFence)&fenceId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkFence fence = VkObject_fromId(fenceId);

#ifdef BACHATA_VORTEK_SERVER
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_SYNC, "destroy_fence", fence);
#endif
    vulkanWrapper.vkDestroyFence(device, fence, NULL);
}

void vt_handle_vkResetFences(VkContext* context) {
    uint64_t deviceId;
    uint32_t fenceCount;

    vt_unserialize_vkResetFences((VkDevice)&deviceId, &fenceCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkFence fences[fenceCount];
    vt_unserialize_vkResetFences(VK_NULL_HANDLE, NULL, fences, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkResetFences(device, fenceCount, fences);
}

void vt_handle_vkGetFenceStatus(VkContext* context) {
    uint64_t deviceId;
    uint64_t fenceId;

    vt_unserialize_vkGetFenceStatus((VkDevice)&deviceId, (VkFence)&fenceId, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkFence fence = VkObject_fromId(fenceId);

    VkResult result = vulkanWrapper.vkGetFenceStatus(device, fence);
#ifdef BACHATA_VORTEK_SERVER
    bachata_note_track_device(device);
    VortekGpuTrack_noteFenceStatus((void*)fence, (int)result);
    if (result == VK_SUCCESS) {
        bachata_drain_deferred_destroys();
    }
#endif
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkWaitForFences(VkContext* context) {
    uint64_t deviceId;
    uint32_t fenceCount;
    VkBool32 waitAll;
    uint64_t timeout;
    /* APK gate marker for runtime/tests/verify-native-fixes.mjs — keep literal. */
    static const char kBachataVortekFenceHostWaitMarker[] = "bachata_vortek_fence_host_wait";
    (void)kBachataVortekFenceHostWaitMarker;

    vt_unserialize_vkWaitForFences((VkDevice)&deviceId, &fenceCount, NULL, &waitAll, &timeout, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkFence fences[fenceCount];
    vt_unserialize_vkWaitForFences(VK_NULL_HANDLE, NULL, fences, NULL, NULL, context->inputBuffer, &context->memoryPool);

    /* Always host-wait. Do not export SYNC_FD and hand FDs to the guest client:
     * on Mali-G615 (and previously some Adreno paths) create-signaled fences
     * produced never-ready FDs that the client mapped to VK_ERROR_DEVICE_LOST
     * on the first Presenter present_done wait (exit 133 / GetRenderFrame).
     *
     * Guest Vulkan calls are serialized by the client VT_CALL_LOCK, so blocking
     * the server request thread for the wait is safe. Client already treats
     * SUCCESS + 0 FDs as a completed wait (see vt_call_vkWaitForFences). */
    VkResult result = vulkanWrapper.vkWaitForFences(device, fenceCount, fences, waitAll, timeout);
#ifdef BACHATA_VORTEK_SERVER
    {
        void* fencePtrs[fenceCount > 0 ? fenceCount : 1];
        for (uint32_t fi = 0; fi < fenceCount; fi++) {
            fencePtrs[fi] = (void*)fences[fi];
        }
        VortekGpuTrack_noteFencesWait(fencePtrs, fenceCount, waitAll ? 1 : 0, (int)result);
    }
    bachata_drain_deferred_destroys();
    if (result == VK_ERROR_DEVICE_LOST) context->status = result;
#endif
    if (timeout != 0) {
        send_fds(context->clientFd, NULL, 0, &result, sizeof(VkResult));
    } else {
        vt_send(context->clientRing, result, NULL, 0);
    }
}

void vt_handle_vkCreateSemaphore(VkContext* context) {
    uint64_t deviceId;
    VkSemaphoreCreateInfo createInfo = {0};

    vt_unserialize_vkCreateSemaphore((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkSemaphore semaphore;
    VkResult result = vulkanWrapper.vkCreateSemaphore(device, &createInfo, NULL, &semaphore);

    VT_SERIALIZE_CMD(VkSemaphore, semaphore);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroySemaphore(VkContext* context) {
    uint64_t deviceId;
    uint64_t semaphoreId;

    vt_unserialize_vkDestroySemaphore((VkDevice)&deviceId, (VkSemaphore)&semaphoreId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkSemaphore semaphore = VkObject_fromId(semaphoreId);

#ifdef BACHATA_VORTEK_SERVER
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_SYNC, "destroy_semaphore",
                                   semaphore);
    VortekGpuTrack_notePresentSemaphoreDestroyed((void*)semaphore);
#endif
    vulkanWrapper.vkDestroySemaphore(device, semaphore, NULL);
}

void vt_handle_vkCreateEvent(VkContext* context) {
    uint64_t deviceId;
    VkEventCreateInfo createInfo = {0};

    vt_unserialize_vkCreateEvent((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkEvent event;
    VkResult result = vulkanWrapper.vkCreateEvent(device, &createInfo, NULL, &event);

    VT_SERIALIZE_CMD(VkEvent, event);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyEvent(VkContext* context) {
    uint64_t deviceId;
    uint64_t eventId;

    vt_unserialize_vkDestroyEvent((VkDevice)&deviceId, (VkEvent)&eventId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkEvent event = VkObject_fromId(eventId);

    vulkanWrapper.vkDestroyEvent(device, event, NULL);
}

void vt_handle_vkGetEventStatus(VkContext* context) {
    uint64_t deviceId;
    uint64_t eventId;

    vt_unserialize_vkGetEventStatus((VkDevice)&deviceId, (VkEvent)&eventId, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkEvent event = VkObject_fromId(eventId);

    VkResult result = vulkanWrapper.vkGetEventStatus(device, event);
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkSetEvent(VkContext* context) {
    uint64_t deviceId;
    uint64_t eventId;

    vt_unserialize_vkSetEvent((VkDevice)&deviceId, (VkEvent)&eventId, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkEvent event = VkObject_fromId(eventId);

    VkResult result = vulkanWrapper.vkSetEvent(device, event);
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkResetEvent(VkContext* context) {
    uint64_t deviceId;
    uint64_t eventId;

    vt_unserialize_vkResetEvent((VkDevice)&deviceId, (VkEvent)&eventId, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkEvent event = VkObject_fromId(eventId);

    VkResult result = vulkanWrapper.vkResetEvent(device, event);
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkCreateQueryPool(VkContext* context) {
    uint64_t deviceId;
    VkQueryPoolCreateInfo createInfo = {0};

    vt_unserialize_vkCreateQueryPool((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkQueryPool queryPool;
    VkResult result = vulkanWrapper.vkCreateQueryPool(device, &createInfo, NULL, &queryPool);

    VT_SERIALIZE_CMD(VkQueryPool, queryPool);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyQueryPool(VkContext* context) {
    uint64_t deviceId;
    uint64_t queryPoolId;

    vt_unserialize_vkDestroyQueryPool((VkDevice)&deviceId, (VkQueryPool)&queryPoolId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);

    vulkanWrapper.vkDestroyQueryPool(device, queryPool, NULL);
}

void vt_handle_vkGetQueryPoolResults(VkContext* context) {
    uint64_t deviceId;
    uint64_t queryPoolId;
    uint32_t firstQuery;
    uint32_t queryCount;
    size_t dataSize;
    VkDeviceSize stride;
    VkQueryResultFlags flags;

    vt_unserialize_vkGetQueryPoolResults((VkDevice)&deviceId, (VkQueryPool)&queryPoolId, &firstQuery, &queryCount, &dataSize, NULL, &stride, &flags, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);

    void* data = vt_alloc(&context->memoryPool, dataSize);
    VkResult result = vulkanWrapper.vkGetQueryPoolResults(device, queryPool, firstQuery, queryCount, dataSize, data, stride, flags);
    VT_SERIALIZE_CMD(vkGetQueryPoolResults, VK_NULL_HANDLE, VK_NULL_HANDLE, firstQuery, queryCount, dataSize, data, stride, flags);

    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkResetQueryPool(VkContext* context) {
    uint64_t deviceId;
    uint64_t queryPoolId;
    uint32_t firstQuery;
    uint32_t queryCount;

    vt_unserialize_vkResetQueryPool((VkDevice)&deviceId, (VkQueryPool)&queryPoolId, &firstQuery, &queryCount, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);

    vulkanWrapper.vkResetQueryPool(device, queryPool, firstQuery, queryCount);
}

void vt_handle_vkCreateBuffer(VkContext* context) {
    uint64_t deviceId;
    VkBufferCreateInfo createInfo = {0};

    vt_unserialize_vkCreateBuffer((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkBuffer buffer;
    VkResult result = vulkanWrapper.vkCreateBuffer(device, &createInfo, NULL, &buffer);
#ifdef BACHATA_VORTEK_SERVER
    if (result == VK_SUCCESS) {
        VortekGpuTrack_onCreateBuffer(buffer, (uint64_t)createInfo.size, (uint32_t)createInfo.usage);
    }
#endif

    VT_SERIALIZE_CMD(VkBuffer, buffer);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyBuffer(VkContext* context) {
    uint64_t deviceId;
    uint64_t bufferId;

    vt_unserialize_vkDestroyBuffer((VkDevice)&deviceId, (VkBuffer)&bufferId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkBuffer buffer = VkObject_fromId(bufferId);

    if (context->textureDecoder) TextureDecoder_removeBoundBuffer(context->textureDecoder, buffer);
#ifdef BACHATA_VORTEK_SERVER
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_BUFFER, "destroy_buffer", buffer);
    if (!VortekGpuTrack_onDestroyBuffer(buffer, device)) {
        return; /* deferred destroy — pool slot freed at physical release */
    }
    for (int i = 0; i < g_bachata_pool_slot_count; ++i) {
        if (g_bachata_pool_slots[i].used && g_bachata_pool_slots[i].owner_buffer == (void*)buffer) {
            g_bachata_pool_slots[i].busy = 0;
            g_bachata_pool_slots[i].owner_buffer = NULL;
            VortekGpuTrack_notePoolSlotFreed(i);
        }
    }
#endif
    vulkanWrapper.vkDestroyBuffer(device, buffer, NULL);
#ifdef BACHATA_VORTEK_SERVER
    bachata_drain_deferred_destroys();
#endif
}

void vt_handle_vkCreateBufferView(VkContext* context) {
    uint64_t deviceId;
    VkBufferViewCreateInfo createInfo = {0};

    vt_unserialize_vkCreateBufferView((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkBufferView view;
    VkResult result = vulkanWrapper.vkCreateBufferView(device, &createInfo, NULL, &view);

    VT_SERIALIZE_CMD(VkBufferView, view);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyBufferView(VkContext* context) {
    uint64_t deviceId;
    uint64_t bufferViewId;

    vt_unserialize_vkDestroyBufferView((VkDevice)&deviceId, (VkBufferView)&bufferViewId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkBufferView bufferView = VkObject_fromId(bufferViewId);

    vulkanWrapper.vkDestroyBufferView(device, bufferView, NULL);
}

void vt_handle_vkCreateImage(VkContext* context) {
    uint64_t deviceId;
    VkImageCreateInfo createInfo = {0};

    vt_unserialize_vkCreateImage((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkImage image;
    VkResult result;
    if (context->textureDecoder && isCompressedFormat(createInfo.format)) {
        result = TextureDecoder_createImage(context->textureDecoder, device, &createInfo, &image);
        if (result == VK_SUCCESS) RingBuffer_setStatus(context->clientRing, RING_STATUS_WAIT);
    }
    else result = vulkanWrapper.vkCreateImage(device, &createInfo, NULL, &image);
#ifdef BACHATA_VORTEK_SERVER
    if (result == VK_SUCCESS) {
        VortekGpuTrack_onCreateImage(image,
                                     createInfo.extent.width,
                                     createInfo.extent.height,
                                     (uint32_t)createInfo.format,
                                     (uint32_t)createInfo.usage);
    }
#endif

    VT_SERIALIZE_CMD(VkImage, image);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyImage(VkContext* context) {
    uint64_t deviceId;
    uint64_t imageId;

    vt_unserialize_vkDestroyImage((VkDevice)&deviceId, (VkImage)&imageId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkImage image = VkObject_fromId(imageId);

#ifdef BACHATA_VORTEK_SERVER
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_IMAGE, "destroy_image", image);
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_BUFFER, "destroy_image", image);
    if (!VortekGpuTrack_onDestroyImage(image, device)) {
        return; /* deferred destroy */
    }
#endif
    if (context->textureDecoder && TextureDecoder_containsImage(context->textureDecoder, image)) {
        TextureDecoder_destroyImage(context->textureDecoder, device, image);
    }
    else vulkanWrapper.vkDestroyImage(device, image, NULL);
#ifdef BACHATA_VORTEK_SERVER
    bachata_drain_deferred_destroys();
#endif
}

void vt_handle_vkGetImageSubresourceLayout(VkContext* context) {
    uint64_t deviceId;
    uint64_t imageId;
    VkImageSubresource subresource = {0};

    vt_unserialize_vkGetImageSubresourceLayout((VkDevice)&deviceId, (VkImage)&imageId, &subresource, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkImage image = VkObject_fromId(imageId);

    VkSubresourceLayout layout = {0};
    vulkanWrapper.vkGetImageSubresourceLayout(device, image, &subresource, &layout);

    VT_SERIALIZE_CMD(VkSubresourceLayout, &layout);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkCreateImageView(VkContext* context) {
    uint64_t deviceId;
    VkImageViewCreateInfo createInfo = {0};

    vt_unserialize_vkCreateImageView((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    if (context->textureDecoder && isCompressedFormat(createInfo.format)) {
        createInfo.format = DECOMPRESSED_FORMAT;
        createInfo.subresourceRange.levelCount = 1;
    }

    VkImageView view;
    VkResult result = vulkanWrapper.vkCreateImageView(device, &createInfo, NULL, &view);
#ifdef BACHATA_VORTEK_SERVER
    if (result == VK_SUCCESS) {
        VortekGpuTrack_onCreateImageView(view, createInfo.image);
    }
#endif

    VT_SERIALIZE_CMD(VkImageView, view);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyImageView(VkContext* context) {
    uint64_t deviceId;
    uint64_t imageViewId;

    vt_unserialize_vkDestroyImageView((VkDevice)&deviceId, (VkImageView)&imageViewId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkImageView imageView = VkObject_fromId(imageViewId);

#ifdef BACHATA_VORTEK_SERVER
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_IMAGE, "destroy_image_view",
                                   imageView);
    if (!VortekGpuTrack_onDestroyImageView(imageView, device)) {
        return;
    }
#endif
    vulkanWrapper.vkDestroyImageView(device, imageView, NULL);
}

void vt_handle_vkCreateShaderModule(VkContext* context) {
    uint64_t deviceId;
    VkShaderModuleCreateInfo createInfo = {0};

    vt_unserialize_vkCreateShaderModule((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    ShaderModule* shaderModule;
    VkResult result = ShaderInspector_createModule(context->shaderInspector, device, createInfo.pCode, createInfo.codeSize, &shaderModule);

    VT_SERIALIZE_CMD(VkShaderModule, (VkShaderModule)shaderModule);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyShaderModule(VkContext* context) {
    uint64_t deviceId;
    uint64_t shaderModuleId;

    vt_unserialize_vkDestroyShaderModule((VkDevice)&deviceId, (VkShaderModule)&shaderModuleId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    ShaderModule* shaderModule = VkObject_fromId(shaderModuleId);

    destroyVkObject(VK_OBJECT_TYPE_SHADER_MODULE, device, shaderModule);
}

void vt_handle_vkCreatePipelineCache(VkContext* context) {
    uint64_t deviceId;
    VkPipelineCacheCreateInfo createInfo = {0};

    vt_unserialize_vkCreatePipelineCache((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkPipelineCache pipelineCache;
    VkResult result = vulkanWrapper.vkCreatePipelineCache(device, &createInfo, NULL, &pipelineCache);

    VT_SERIALIZE_CMD(VkPipelineCache, pipelineCache);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyPipelineCache(VkContext* context) {
    uint64_t deviceId;
    uint64_t pipelineCacheId;

    vt_unserialize_vkDestroyPipelineCache((VkDevice)&deviceId, (VkPipelineCache)&pipelineCacheId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkPipelineCache pipelineCache = VkObject_fromId(pipelineCacheId);

    vulkanWrapper.vkDestroyPipelineCache(device, pipelineCache, NULL);
}

void vt_handle_vkGetPipelineCacheData(VkContext* context) {
    uint64_t deviceId;
    uint64_t pipelineCacheId;
    size_t dataSize;

    vt_unserialize_vkGetPipelineCacheData((VkDevice)&deviceId, (VkPipelineCache)&pipelineCacheId, &dataSize, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkPipelineCache pipelineCache = VkObject_fromId(pipelineCacheId);

    void* data = dataSize > 0 ? vt_alloc(&context->memoryPool, dataSize) : NULL;
    VkResult result = vulkanWrapper.vkGetPipelineCacheData(device, pipelineCache, &dataSize, data);

    VT_SERIALIZE_CMD(vkGetPipelineCacheData, VK_NULL_HANDLE, VK_NULL_HANDLE, &dataSize, data);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkMergePipelineCaches(VkContext* context) {
    uint64_t deviceId;
    uint64_t dstCacheId;
    uint32_t srcCacheCount;

    vt_unserialize_vkMergePipelineCaches((VkDevice)&deviceId, (VkPipelineCache)&dstCacheId, &srcCacheCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkPipelineCache dstCache = VkObject_fromId(dstCacheId);

    VkPipelineCache srcCaches[srcCacheCount];
    vt_unserialize_vkMergePipelineCaches(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, srcCaches, context->inputBuffer, &context->memoryPool);

    VkResult result = vulkanWrapper.vkMergePipelineCaches(device, dstCache, srcCacheCount, srcCaches);
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkCreateGraphicsPipelines(VkContext* context) {
    AsyncPipelineCreator_create(context, PIPELINE_TYPE_GRAPHICS);
}

void vt_handle_vkCreateComputePipelines(VkContext* context) {
    AsyncPipelineCreator_create(context, PIPELINE_TYPE_COMPUTE);
}

void vt_handle_vkDestroyPipeline(VkContext* context) {
    uint64_t deviceId;
    uint64_t pipelineId;

    vt_unserialize_vkDestroyPipeline((VkDevice)&deviceId, (VkPipeline)&pipelineId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkPipeline pipeline = VkObject_fromId(pipelineId);

    vulkanWrapper.vkDestroyPipeline(device, pipeline, NULL);
}

void vt_handle_vkCreatePipelineLayout(VkContext* context) {
    uint64_t deviceId;
    VkPipelineLayoutCreateInfo createInfo = {0};

    vt_unserialize_vkCreatePipelineLayout((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    if (createInfo.pPushConstantRanges) {
        for (int i = 0; i < createInfo.pushConstantRangeCount; i++) {
            VkPushConstantRange* pushConstantRange = (VkPushConstantRange*)&createInfo.pPushConstantRanges[i];
            if (pushConstantRange->stageFlags & VK_SHADER_STAGE_VERTEX_BIT) pushConstantRange->stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        }
    }

    VkPipelineLayout pipelineLayout;
    VkResult result = vulkanWrapper.vkCreatePipelineLayout(device, &createInfo, NULL, &pipelineLayout);

    VT_SERIALIZE_CMD(VkPipelineLayout, pipelineLayout);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyPipelineLayout(VkContext* context) {
    uint64_t deviceId;
    uint64_t pipelineLayoutId;

    vt_unserialize_vkDestroyPipelineLayout((VkDevice)&deviceId, (VkPipelineLayout)&pipelineLayoutId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkPipelineLayout pipelineLayout = VkObject_fromId(pipelineLayoutId);

    vulkanWrapper.vkDestroyPipelineLayout(device, pipelineLayout, NULL);
}

void vt_handle_vkCreateSampler(VkContext* context) {
    uint64_t deviceId;
    VkSamplerCreateInfo createInfo = {0};

    vt_unserialize_vkCreateSampler((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkSampler sampler;
    VkResult result = vulkanWrapper.vkCreateSampler(device, &createInfo, NULL, &sampler);

    VT_SERIALIZE_CMD(VkSampler, sampler);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroySampler(VkContext* context) {
    uint64_t deviceId;
    uint64_t samplerId;

    vt_unserialize_vkDestroySampler((VkDevice)&deviceId, (VkSampler)&samplerId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkSampler sampler = VkObject_fromId(samplerId);

    vulkanWrapper.vkDestroySampler(device, sampler, NULL);
}

void vt_handle_vkCreateDescriptorSetLayout(VkContext* context) {
    uint64_t deviceId;
    VkDescriptorSetLayoutCreateInfo createInfo = {0};

    vt_unserialize_vkCreateDescriptorSetLayout((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkDescriptorSetLayout setLayout;
    VkResult result = vulkanWrapper.vkCreateDescriptorSetLayout(device, &createInfo, NULL, &setLayout);

    VT_SERIALIZE_CMD(VkDescriptorSetLayout, setLayout);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyDescriptorSetLayout(VkContext* context) {
    uint64_t deviceId;
    uint64_t descriptorSetLayoutId;

    vt_unserialize_vkDestroyDescriptorSetLayout((VkDevice)&deviceId, (VkDescriptorSetLayout)&descriptorSetLayoutId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkDescriptorSetLayout descriptorSetLayout = VkObject_fromId(descriptorSetLayoutId);

    vulkanWrapper.vkDestroyDescriptorSetLayout(device, descriptorSetLayout, NULL);
}

void vt_handle_vkCreateDescriptorPool(VkContext* context) {
    uint64_t deviceId;
    VkDescriptorPoolCreateInfo createInfo = {0};

    vt_unserialize_vkCreateDescriptorPool((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkDescriptorPool descriptorPool;
    VkResult result = vulkanWrapper.vkCreateDescriptorPool(device, &createInfo, NULL, &descriptorPool);

    VT_SERIALIZE_CMD(VkDescriptorPool, descriptorPool);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyDescriptorPool(VkContext* context) {
    uint64_t deviceId;
    uint64_t descriptorPoolId;

    vt_unserialize_vkDestroyDescriptorPool((VkDevice)&deviceId, (VkDescriptorPool)&descriptorPoolId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkDescriptorPool descriptorPool = VkObject_fromId(descriptorPoolId);

    vulkanWrapper.vkDestroyDescriptorPool(device, descriptorPool, NULL);
}

void vt_handle_vkResetDescriptorPool(VkContext* context) {
    uint64_t deviceId;
    uint64_t descriptorPoolId;
    VkDescriptorPoolResetFlags flags;

    vt_unserialize_vkResetDescriptorPool((VkDevice)&deviceId, (VkDescriptorPool)&descriptorPoolId, &flags, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkDescriptorPool descriptorPool = VkObject_fromId(descriptorPoolId);

    vulkanWrapper.vkResetDescriptorPool(device, descriptorPool, flags);
}

void vt_handle_vkAllocateDescriptorSets(VkContext* context) {
    uint64_t deviceId;
    VkDescriptorSetAllocateInfo allocateInfo = {0};

    vt_unserialize_vkAllocateDescriptorSets((VkDevice)&deviceId, &allocateInfo, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkDescriptorSet descriptorSets[allocateInfo.descriptorSetCount];
    VkResult result = vulkanWrapper.vkAllocateDescriptorSets(device, &allocateInfo, descriptorSets);

    int bufferSize = allocateInfo.descriptorSetCount * VK_HANDLE_BYTE_COUNT;
    char* outputBuffer = vt_alloc(&context->memoryPool, bufferSize);
    for (int i = 0, j = 0; i < allocateInfo.descriptorSetCount; i++, j += VK_HANDLE_BYTE_COUNT) {
        vt_serialize_VkDescriptorSet(descriptorSets[i], outputBuffer + j);
    }
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkFreeDescriptorSets(VkContext* context) {
    uint64_t deviceId;
    uint64_t descriptorPoolId;
    uint32_t descriptorSetCount;

    vt_unserialize_vkFreeDescriptorSets((VkDevice)&deviceId, (VkDescriptorPool)&descriptorPoolId, &descriptorSetCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkDescriptorPool descriptorPool = VkObject_fromId(descriptorPoolId);

    VkDescriptorSet descriptorSets[descriptorSetCount];
    vt_unserialize_vkFreeDescriptorSets(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, descriptorSets, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkFreeDescriptorSets(device, descriptorPool, descriptorSetCount, descriptorSets);
}

void vt_handle_vkUpdateDescriptorSets(VkContext* context) {
    uint64_t deviceId;
    uint32_t descriptorWriteCount;
    uint32_t descriptorCopyCount;

    vt_unserialize_vkUpdateDescriptorSets((VkDevice)&deviceId, &descriptorWriteCount, NULL, &descriptorCopyCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkWriteDescriptorSet descriptorWrites[descriptorWriteCount];
    VkCopyDescriptorSet descriptorCopies[descriptorCopyCount];
    vt_unserialize_vkUpdateDescriptorSets(VK_NULL_HANDLE, NULL, descriptorWrites, NULL, descriptorCopies, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    /* Dig (detile_stamp=1): merge per-binding writes so detile 0/1/2 all stay.
     * Baseline (stamp=0): legacy replace-per-write — matches deep-run host tracking
     * (only last write, usually UBO, survives). Merging FHD src into every cmd
     * closure earlier regressed SEGA→stage reach. */
    if (VortekGpuTrack_detileStampEnabled()) {
        void* bufs[64];
        uint32_t bindings[64];
        uint32_t dtypes[64];
        uint64_t offs[64];
        uint64_t ranges[64];
        for (uint32_t wi = 0; wi < descriptorWriteCount; wi++) {
            const VkWriteDescriptorSet* w = &descriptorWrites[wi];
            if (!w->dstSet || !w->pBufferInfo || w->descriptorCount == 0) {
                continue;
            }
            uint32_t bc = 0;
            for (uint32_t bi = 0; bi < w->descriptorCount && bc < 64; bi++) {
                if (!w->pBufferInfo[bi].buffer) {
                    continue;
                }
                bufs[bc] = (void*)w->pBufferInfo[bi].buffer;
                bindings[bc] = w->dstBinding + bi;
                dtypes[bc] = (uint32_t)w->descriptorType;
                offs[bc] = (uint64_t)w->pBufferInfo[bi].offset;
                ranges[bc] = (uint64_t)w->pBufferInfo[bi].range;
                bc++;
            }
            if (bc > 0) {
                VortekGpuTrack_onUpdateDescriptorWrites((void*)w->dstSet, bindings, dtypes, bufs,
                                                        offs, ranges, bc);
            }
        }
    } else {
        for (uint32_t wi = 0; wi < descriptorWriteCount; wi++) {
            const VkWriteDescriptorSet* w = &descriptorWrites[wi];
            if (!w->dstSet || !w->pBufferInfo || w->descriptorCount == 0) {
                continue;
            }
            void* bufs[64];
            uint32_t bc = 0;
            for (uint32_t bi = 0; bi < w->descriptorCount && bc < 64; bi++) {
                if (w->pBufferInfo[bi].buffer) {
                    bufs[bc++] = (void*)w->pBufferInfo[bi].buffer;
                }
            }
            if (bc > 0) {
                VortekGpuTrack_onUpdateDescriptorBuffers((void*)w->dstSet, bufs, bc);
            }
        }
    }
#endif
    vulkanWrapper.vkUpdateDescriptorSets(device, descriptorWriteCount, descriptorWrites, descriptorCopyCount, descriptorCopies);
}

void vt_handle_vkCreateFramebuffer(VkContext* context) {
    uint64_t deviceId;
    VkFramebufferCreateInfo createInfo = {0};

    vt_unserialize_vkCreateFramebuffer((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkFramebuffer framebuffer;
    VkResult result = vulkanWrapper.vkCreateFramebuffer(device, &createInfo, NULL, &framebuffer);

    VT_SERIALIZE_CMD(VkFramebuffer, framebuffer);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyFramebuffer(VkContext* context) {
    uint64_t deviceId;
    uint64_t framebufferId;

    vt_unserialize_vkDestroyFramebuffer((VkDevice)&deviceId, (VkFramebuffer)&framebufferId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkFramebuffer framebuffer = VkObject_fromId(framebufferId);

    vulkanWrapper.vkDestroyFramebuffer(device, framebuffer, NULL);
}

void vt_handle_vkCreateRenderPass(VkContext* context) {
    uint64_t deviceId;
    VkRenderPassCreateInfo createInfo = {0};

    vt_unserialize_vkCreateRenderPass((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkRenderPass renderPass;
    VkResult result = vulkanWrapper.vkCreateRenderPass(device, &createInfo, NULL, &renderPass);

    VT_SERIALIZE_CMD(VkRenderPass, renderPass);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyRenderPass(VkContext* context) {
    uint64_t deviceId;
    uint64_t renderPassId;

    vt_unserialize_vkDestroyRenderPass((VkDevice)&deviceId, (VkRenderPass)&renderPassId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkRenderPass renderPass = VkObject_fromId(renderPassId);

    vulkanWrapper.vkDestroyRenderPass(device, renderPass, NULL);
}

void vt_handle_vkGetRenderAreaGranularity(VkContext* context) {
    uint64_t deviceId;
    uint64_t renderPassId;

    vt_unserialize_vkGetRenderAreaGranularity((VkDevice)&deviceId, (VkRenderPass)&renderPassId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkRenderPass renderPass = VkObject_fromId(renderPassId);

    VkExtent2D granularity = {0};
    vulkanWrapper.vkGetRenderAreaGranularity(device, renderPass, &granularity);

    VT_SERIALIZE_CMD(VkExtent2D, &granularity);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkCreateCommandPool(VkContext* context) {
    uint64_t deviceId;
    VkCommandPoolCreateInfo createInfo = {0};

    vt_unserialize_vkCreateCommandPool((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkCommandPool commandPool;
    VkResult result = vulkanWrapper.vkCreateCommandPool(device, &createInfo, NULL, &commandPool);

    VT_SERIALIZE_CMD(VkCommandPool, commandPool);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroyCommandPool(VkContext* context) {
    uint64_t deviceId;
    uint64_t commandPoolId;

    vt_unserialize_vkDestroyCommandPool((VkDevice)&deviceId, (VkCommandPool)&commandPoolId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkCommandPool commandPool = VkObject_fromId(commandPoolId);

#ifdef BACHATA_VORTEK_SERVER
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_CMDPOOL, "destroy_command_pool",
                                   commandPool);
    VortekGpuTrack_onCommandPoolDestroy(commandPool, device);
#endif
    vulkanWrapper.vkDestroyCommandPool(device, commandPool, NULL);
}

void vt_handle_vkResetCommandPool(VkContext* context) {
    uint64_t deviceId;
    uint64_t commandPoolId;
    VkCommandPoolResetFlags flags;

    vt_unserialize_vkResetCommandPool((VkDevice)&deviceId, (VkCommandPool)&commandPoolId, &flags, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkCommandPool commandPool = VkObject_fromId(commandPoolId);

#ifdef BACHATA_VORTEK_SERVER
    bachata_maybe_wait_idle_device(device, VORTEK_WAIT_IDLE_CMDPOOL, "reset_command_pool",
                                   commandPool);
    VortekGpuTrack_onCommandPoolReset(commandPool, device);
#endif
    vulkanWrapper.vkResetCommandPool(device, commandPool, flags);
}

void vt_handle_vkAllocateCommandBuffers(VkContext* context) {
    uint64_t deviceId;
    VkCommandBufferAllocateInfo allocateInfo = {0};

    vt_unserialize_vkAllocateCommandBuffers((VkDevice)&deviceId, &allocateInfo, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkCommandBuffer commandBuffers[allocateInfo.commandBufferCount];
    VkResult result = vulkanWrapper.vkAllocateCommandBuffers(device, &allocateInfo, commandBuffers);

    VT_SERIALIZE_CMD(vkAllocateCommandBuffers, VK_NULL_HANDLE, &allocateInfo, commandBuffers);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkFreeCommandBuffers(VkContext* context) {
    uint64_t deviceId;
    uint64_t commandPoolId;
    uint32_t commandBufferCount;

    vt_unserialize_vkFreeCommandBuffers((VkDevice)&deviceId, (VkCommandPool)&commandPoolId, &commandBufferCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkCommandPool commandPool = VkObject_fromId(commandPoolId);

    VkCommandBuffer commandBuffers[commandBufferCount];
    vt_unserialize_vkFreeCommandBuffers(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, commandBuffers, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkFreeCommandBuffers(device, commandPool, commandBufferCount, commandBuffers);
}

void vt_handle_vkBeginCommandBuffer(VkContext* context) {
    uint64_t commandBufferId;
    VkCommandBufferBeginInfo beginInfo = {0};
    vt_unserialize_vkBeginCommandBuffer((VkCommandBuffer)&commandBufferId, &beginInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    #ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onCmdReset(commandBuffer);
#endif
    vulkanWrapper.vkBeginCommandBuffer(commandBuffer, &beginInfo);
}

void vt_handle_vkEndCommandBuffer(VkContext* context) {
    int position = 0;
    char* inputBuffer = context->inputBuffer;
    uint64_t commandBufferId = *(uint64_t*)(inputBuffer);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    position += VK_HANDLE_BYTE_COUNT;

    while (position < context->inputBufferSize) {
        int requestCode = *(int*)(inputBuffer + position + 0);
        int size = *(int*)(inputBuffer + position + 4);

        context->inputBuffer = inputBuffer + position + HEADER_SIZE;
        HandleRequestFunc handleRequestFunc = getHandleRequestFunc(requestCode);

#if DEBUG_MODE
        println("handleRequest name=%s size=%d", requestCodeToString(requestCode), size);
#endif

        if (handleRequestFunc) handleRequestFunc(context);
        context->inputBuffer = inputBuffer;

        position += size + HEADER_SIZE;
    }

    vulkanWrapper.vkEndCommandBuffer(commandBuffer);
}

void vt_handle_vkResetCommandBuffer(VkContext* context) {
    uint64_t commandBufferId;
    VkCommandBufferResetFlags flags;

    vt_unserialize_vkResetCommandBuffer((VkCommandBuffer)&commandBufferId, &flags, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    #ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onCmdReset(commandBuffer);
#endif
    vulkanWrapper.vkResetCommandBuffer(commandBuffer, flags);
}

void vt_handle_vkCmdBindPipeline(VkContext* context) {
    uint64_t commandBufferId;
    VkPipelineBindPoint pipelineBindPoint;
    uint64_t pipelineId;

    vt_unserialize_vkCmdBindPipeline((VkCommandBuffer)&commandBufferId, &pipelineBindPoint, (VkPipeline)&pipelineId, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkPipeline pipeline = VkObject_fromId(pipelineId);

    vulkanWrapper.vkCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

void vt_handle_vkCmdSetViewport(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstViewport;
    uint32_t viewportCount;

    vt_unserialize_vkCmdSetViewport((VkCommandBuffer)&commandBufferId, &firstViewport, &viewportCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkViewport viewports[viewportCount];
    vt_unserialize_vkCmdSetViewport(VK_NULL_HANDLE, NULL, NULL, viewports, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetViewport(commandBuffer, firstViewport, viewportCount, viewports);
}

void vt_handle_vkCmdSetScissor(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstScissor;
    uint32_t scissorCount;

    vt_unserialize_vkCmdSetScissor((VkCommandBuffer)&commandBufferId, &firstScissor, &scissorCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkRect2D scissors[scissorCount];
    vt_unserialize_vkCmdSetScissor(VK_NULL_HANDLE, NULL, NULL, scissors, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetScissor(commandBuffer, firstScissor, scissorCount, scissors);
}

void vt_handle_vkCmdSetLineWidth(VkContext* context) {
    uint64_t commandBufferId;
    float lineWidth;

    vt_unserialize_vkCmdSetLineWidth((VkCommandBuffer)&commandBufferId, &lineWidth, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetLineWidth(commandBuffer, lineWidth);
}

void vt_handle_vkCmdSetDepthBias(VkContext* context) {
    uint64_t commandBufferId;
    float depthBiasConstantFactor;
    float depthBiasClamp;
    float depthBiasSlopeFactor;

    vt_unserialize_vkCmdSetDepthBias((VkCommandBuffer)&commandBufferId, &depthBiasConstantFactor, &depthBiasClamp, &depthBiasSlopeFactor, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDepthBias(commandBuffer, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

void vt_handle_vkCmdSetBlendConstants(VkContext* context) {
    uint64_t commandBufferId;

    vt_unserialize_vkCmdSetBlendConstants((VkCommandBuffer)&commandBufferId, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    float blendConstants[4];
    vt_unserialize_vkCmdSetBlendConstants(VK_NULL_HANDLE, blendConstants, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetBlendConstants(commandBuffer, blendConstants);
}

void vt_handle_vkCmdSetDepthBounds(VkContext* context) {
    uint64_t commandBufferId;
    float minDepthBounds;
    float maxDepthBounds;

    vt_unserialize_vkCmdSetDepthBounds((VkCommandBuffer)&commandBufferId, &minDepthBounds, &maxDepthBounds, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDepthBounds(commandBuffer, minDepthBounds, maxDepthBounds);
}

void vt_handle_vkCmdSetStencilCompareMask(VkContext* context) {
    uint64_t commandBufferId;
    VkStencilFaceFlags faceMask;
    uint32_t compareMask;

    vt_unserialize_vkCmdSetStencilCompareMask((VkCommandBuffer)&commandBufferId, &faceMask, &compareMask, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetStencilCompareMask(commandBuffer, faceMask, compareMask);
}

void vt_handle_vkCmdSetStencilWriteMask(VkContext* context) {
    uint64_t commandBufferId;
    VkStencilFaceFlags faceMask;
    uint32_t writeMask;

    vt_unserialize_vkCmdSetStencilWriteMask((VkCommandBuffer)&commandBufferId, &faceMask, &writeMask, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetStencilWriteMask(commandBuffer, faceMask, writeMask);
}

void vt_handle_vkCmdSetStencilReference(VkContext* context) {
    uint64_t commandBufferId;
    VkStencilFaceFlags faceMask;
    uint32_t reference;

    vt_unserialize_vkCmdSetStencilReference((VkCommandBuffer)&commandBufferId, &faceMask, &reference, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetStencilReference(commandBuffer, faceMask, reference);
}

void vt_handle_vkCmdBindDescriptorSets(VkContext* context) {
    uint64_t commandBufferId;
    VkPipelineBindPoint pipelineBindPoint;
    uint64_t layoutId;
    uint32_t firstSet;
    uint32_t descriptorSetCount;
    uint32_t dynamicOffsetCount;

    vt_unserialize_vkCmdBindDescriptorSets((VkCommandBuffer)&commandBufferId, &pipelineBindPoint, (VkPipelineLayout)&layoutId, &firstSet, &descriptorSetCount, VK_NULL_HANDLE, &dynamicOffsetCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkPipelineLayout layout = VkObject_fromId(layoutId);

    VkDescriptorSet descriptorSets[descriptorSetCount];
    uint32_t dynamicOffsets[dynamicOffsetCount];
    vt_unserialize_vkCmdBindDescriptorSets(VK_NULL_HANDLE, NULL, VK_NULL_HANDLE, NULL, NULL, descriptorSets, NULL, dynamicOffsets, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onBindDescriptorSets(commandBuffer, (void* const*)descriptorSets, descriptorSetCount);
#endif
    vulkanWrapper.vkCmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, descriptorSets, dynamicOffsetCount, dynamicOffsets);
}

void vt_handle_vkCmdBindIndexBuffer(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t bufferId;
    VkDeviceSize offset;
    VkIndexType indexType;

    vt_unserialize_vkCmdBindIndexBuffer((VkCommandBuffer)&commandBufferId, (VkBuffer)&bufferId, &offset, &indexType, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer buffer = VkObject_fromId(bufferId);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onCmdBufferRef(commandBuffer, buffer, "bind_index", 0);
#endif
    vulkanWrapper.vkCmdBindIndexBuffer(commandBuffer, buffer, offset, indexType);
}

void vt_handle_vkCmdBindVertexBuffers(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstBinding;
    uint32_t bindingCount;

    vt_unserialize_vkCmdBindVertexBuffers((VkCommandBuffer)&commandBufferId, &firstBinding, &bindingCount, VK_NULL_HANDLE, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkBuffer buffers[bindingCount];
    VkDeviceSize offsets[bindingCount];
    vt_unserialize_vkCmdBindVertexBuffers(VK_NULL_HANDLE, NULL, NULL, buffers, offsets, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    for (uint32_t i = 0; i < bindingCount; i++) {
        if (buffers[i]) {
            VortekGpuTrack_onCmdBufferRef(commandBuffer, buffers[i], "bind_vertex", 0);
        }
    }
#endif
    vulkanWrapper.vkCmdBindVertexBuffers(commandBuffer, firstBinding, bindingCount, buffers, offsets);
}

void vt_handle_vkCmdDraw(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;

    vt_unserialize_vkCmdDraw((VkCommandBuffer)&commandBufferId, &vertexCount, &instanceCount, &firstVertex, &firstInstance, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void vt_handle_vkCmdDrawIndexed(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;

    vt_unserialize_vkCmdDrawIndexed((VkCommandBuffer)&commandBufferId, &indexCount, &instanceCount, &firstIndex, &vertexOffset, &firstInstance, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void vt_handle_vkCmdDrawIndirect(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t bufferId;
    VkDeviceSize offset;
    uint32_t drawCount;
    uint32_t stride;

    vt_unserialize_vkCmdDrawIndirect((VkCommandBuffer)&commandBufferId, (VkBuffer)&bufferId, &offset, &drawCount, &stride, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer buffer = VkObject_fromId(bufferId);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onCmdBufferRef(commandBuffer, buffer, "draw_indirect", 0);
#endif
    vulkanWrapper.vkCmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
}

void vt_handle_vkCmdDrawIndexedIndirect(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t bufferId;
    VkDeviceSize offset;
    uint32_t drawCount;
    uint32_t stride;

    vt_unserialize_vkCmdDrawIndexedIndirect((VkCommandBuffer)&commandBufferId, (VkBuffer)&bufferId, &offset, &drawCount, &stride, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer buffer = VkObject_fromId(bufferId);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onCmdBufferRef(commandBuffer, buffer, "draw_indexed_indirect", 0);
#endif
    vulkanWrapper.vkCmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
}

void vt_handle_vkCmdDispatch(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;

    vt_unserialize_vkCmdDispatch((VkCommandBuffer)&commandBufferId, &groupCountX, &groupCountY, &groupCountZ, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

#ifdef BACHATA_VORTEK_SERVER
    /* Resolve push/bind descriptor buffers → real src/dst (detile path). */
    VortekGpuTrack_onCmdDispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
#endif
    vulkanWrapper.vkCmdDispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
}

void vt_handle_vkCmdDispatchIndirect(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t bufferId;
    VkDeviceSize offset;

    vt_unserialize_vkCmdDispatchIndirect((VkCommandBuffer)&commandBufferId, (VkBuffer)&bufferId, &offset, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer buffer = VkObject_fromId(bufferId);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onCmdBufferRef(commandBuffer, buffer, "dispatch_indirect", 0);
#endif
    vulkanWrapper.vkCmdDispatchIndirect(commandBuffer, buffer, offset);
}

void vt_handle_vkCmdCopyBuffer(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t srcBufferId;
    uint64_t dstBufferId;
    uint32_t regionCount;

    vt_unserialize_vkCmdCopyBuffer((VkCommandBuffer)&commandBufferId, (VkBuffer)&srcBufferId, (VkBuffer)&dstBufferId, &regionCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer srcBuffer = VkObject_fromId(srcBufferId);
    VkBuffer dstBuffer = VkObject_fromId(dstBufferId);

    VkBufferCopy regions[regionCount];
    vt_unserialize_vkCmdCopyBuffer(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, regions, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    /* Always log FHD_PREPARE_WRITE_CHECK on copy dest; wait only if exact_wait returns 1. */
    for (uint32_t i = 0; i < regionCount; i++) {
        if (VortekGpuTrack_prepareResourceWrite(dstBuffer, (uint64_t)regions[i].dstOffset,
                                                (uint64_t)regions[i].size, "Copy")) {
            bachata_wait_suballoc_overlap(g_bachata_track_device);
        }
    }
    bool allOk = true;
    for (uint32_t i = 0; i < regionCount; i++) {
        if (!VortekGpuTrack_onGpuAccess(commandBuffer, "copy_buffer", srcBuffer, (uint64_t)regions[i].srcOffset,
                                        (uint64_t)regions[i].size, dstBuffer,
                                        (uint64_t)regions[i].dstOffset, (uint64_t)regions[i].size,
                                        0, 0, 0, -1)) {
            allOk = false;
        }
    }
    if (!allOk) {
        /* Skip suspect copy during investigation; still records GPU_RANGE_INVALID. */
        return;
    }
#endif
    vulkanWrapper.vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, regionCount, regions);
}

void vt_handle_vkCmdCopyImage(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t srcImageId;
    VkImageLayout srcImageLayout;
    uint64_t dstImageId;
    VkImageLayout dstImageLayout;
    uint32_t regionCount;

    vt_unserialize_vkCmdCopyImage((VkCommandBuffer)&commandBufferId, (VkImage)&srcImageId, &srcImageLayout, (VkImage)&dstImageId, &dstImageLayout, &regionCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkImage srcImage = VkObject_fromId(srcImageId);
    VkImage dstImage = VkObject_fromId(dstImageId);

    VkImageCopy regions[regionCount];
    vt_unserialize_vkCmdCopyImage(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, VK_NULL_HANDLE, NULL, NULL, regions, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    for (uint32_t i = 0; i < regionCount; i++) {
        VortekGpuTrack_onGpuAccess(commandBuffer, "copy_image", srcImage, 0, 0, dstImage, 0, 0,
                                   regions[i].extent.width, regions[i].extent.height, 0, -1);
    }
#endif
    vulkanWrapper.vkCmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, regions);
}

void vt_handle_vkCmdBlitImage(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t srcImageId;
    VkImageLayout srcImageLayout;
    uint64_t dstImageId;
    VkImageLayout dstImageLayout;
    uint32_t regionCount;
    VkFilter filter;

    vt_unserialize_vkCmdBlitImage((VkCommandBuffer)&commandBufferId, (VkImage)&srcImageId, &srcImageLayout, (VkImage)&dstImageId, &dstImageLayout, &regionCount, NULL, &filter, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkImage srcImage = VkObject_fromId(srcImageId);
    VkImage dstImage = VkObject_fromId(dstImageId);

    VkImageBlit regions[regionCount];
    vt_unserialize_vkCmdBlitImage(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, VK_NULL_HANDLE, NULL, NULL, regions, NULL, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, regions, filter);
}

void vt_handle_vkCmdCopyBufferToImage(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t srcBufferId;
    uint64_t dstImageId;
    VkImageLayout dstImageLayout;
    uint32_t regionCount;

    vt_unserialize_vkCmdCopyBufferToImage((VkCommandBuffer)&commandBufferId, (VkBuffer)&srcBufferId, (VkImage)&dstImageId, &dstImageLayout, &regionCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer srcBuffer = VkObject_fromId(srcBufferId);
    VkImage dstImage = VkObject_fromId(dstImageId);

    VkBufferImageCopy regions[regionCount];
    vt_unserialize_vkCmdCopyBufferToImage(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, NULL, regions, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    bool allOk = true;
    for (uint32_t i = 0; i < regionCount; i++) {
        if (!VortekGpuTrack_onCopyBufferToImage(
                commandBuffer, srcBuffer, dstImage, (uint32_t)dstImageLayout,
                (uint64_t)regions[i].bufferOffset, regions[i].bufferRowLength,
                regions[i].bufferImageHeight, regions[i].imageExtent.width,
                regions[i].imageExtent.height, regions[i].imageExtent.depth)) {
            allOk = false;
        }
    }
    if (!allOk) return;
#endif

    if (context->textureDecoder && TextureDecoder_containsImage(context->textureDecoder, dstImage)) {
        if (regions[0].imageSubresource.mipLevel > 0) return;
        TextureDecoder_copyBufferToImage(context->textureDecoder, commandBuffer, srcBuffer, dstImage, dstImageLayout, regions[0].bufferOffset);
    }
    else vulkanWrapper.vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, regions);
}

void vt_handle_vkCmdCopyImageToBuffer(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t srcImageId;
    VkImageLayout srcImageLayout;
    uint64_t dstBufferId;
    uint32_t regionCount;

    vt_unserialize_vkCmdCopyImageToBuffer((VkCommandBuffer)&commandBufferId, (VkImage)&srcImageId, &srcImageLayout, (VkBuffer)&dstBufferId, &regionCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkImage srcImage = VkObject_fromId(srcImageId);
    VkBuffer dstBuffer = VkObject_fromId(dstBufferId);

    VkBufferImageCopy regions[regionCount];
    vt_unserialize_vkCmdCopyImageToBuffer(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, VK_NULL_HANDLE, NULL, regions, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    bool allOk = true;
    for (uint32_t i = 0; i < regionCount; i++) {
        if (!VortekGpuTrack_onGpuAccess(commandBuffer, "copy_image_to_buffer", srcImage, 0, 0, dstBuffer,
                                        (uint64_t)regions[i].bufferOffset, 0,
                                        regions[i].imageExtent.width,
                                        regions[i].imageExtent.height,
                                        regions[i].bufferRowLength, -1)) {
            allOk = false;
        }
    }
    if (!allOk) return;
#endif
    vulkanWrapper.vkCmdCopyImageToBuffer(commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, regions);
}

void vt_handle_vkCmdUpdateBuffer(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t dstBufferId;
    VkDeviceSize dstOffset;
    VkDeviceSize dataSize;

    vt_unserialize_vkCmdUpdateBuffer((VkCommandBuffer)&commandBufferId, (VkBuffer)&dstBufferId, &dstOffset, &dataSize, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer dstBuffer = VkObject_fromId(dstBufferId);

    char data[dataSize];
    vt_unserialize_vkCmdUpdateBuffer(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, NULL, data, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onCmdBufferRef(commandBuffer, dstBuffer, "update_buffer", 1);
    VortekGpuTrack_onGpuAccess(commandBuffer, "update_buffer", NULL, 0, 0, dstBuffer,
                               (uint64_t)dstOffset, (uint64_t)dataSize, 0, 0, 0, -1);
#endif
    vulkanWrapper.vkCmdUpdateBuffer(commandBuffer, dstBuffer, dstOffset, dataSize, data);
}

void vt_handle_vkCmdFillBuffer(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t dstBufferId;
    VkDeviceSize dstOffset;
    VkDeviceSize size;
    uint32_t data;

    vt_unserialize_vkCmdFillBuffer((VkCommandBuffer)&commandBufferId, (VkBuffer)&dstBufferId, &dstOffset, &size, &data, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer dstBuffer = VkObject_fromId(dstBufferId);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onCmdBufferRef(commandBuffer, dstBuffer, "fill_buffer", 1);
    VortekGpuTrack_onGpuAccess(commandBuffer, "fill_buffer", NULL, 0, 0, dstBuffer,
                               (uint64_t)dstOffset, (uint64_t)size, 0, 0, 0, -1);
#endif
    vulkanWrapper.vkCmdFillBuffer(commandBuffer, dstBuffer, dstOffset, size, data);
}

void vt_handle_vkCmdClearColorImage(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t imageId;
    VkImageLayout imageLayout;
    VkClearColorValue color = {0};
    uint32_t rangeCount;

    vt_unserialize_vkCmdClearColorImage((VkCommandBuffer)&commandBufferId, (VkImage)&imageId, &imageLayout, &color, &rangeCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkImage image = VkObject_fromId(imageId);

    VkImageSubresourceRange ranges[rangeCount];
    vt_unserialize_vkCmdClearColorImage(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, NULL, NULL, ranges, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdClearColorImage(commandBuffer, image, imageLayout, &color, rangeCount, ranges);
}

void vt_handle_vkCmdClearDepthStencilImage(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t imageId;
    VkImageLayout imageLayout;
    VkClearDepthStencilValue depthStencil = {0};
    uint32_t rangeCount;

    vt_unserialize_vkCmdClearDepthStencilImage((VkCommandBuffer)&commandBufferId, (VkImage)&imageId, &imageLayout, &depthStencil, &rangeCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkImage image = VkObject_fromId(imageId);

    VkImageSubresourceRange ranges[rangeCount];
    vt_unserialize_vkCmdClearDepthStencilImage(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, NULL, NULL, ranges, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdClearDepthStencilImage(commandBuffer, image, imageLayout, &depthStencil, rangeCount, ranges);
}

void vt_handle_vkCmdClearAttachments(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t attachmentCount;
    uint32_t rectCount;

    vt_unserialize_vkCmdClearAttachments((VkCommandBuffer)&commandBufferId, &attachmentCount, NULL, &rectCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkClearAttachment attachments[attachmentCount];
    VkClearRect rects[rectCount];
    vt_unserialize_vkCmdClearAttachments(VK_NULL_HANDLE, NULL, attachments, NULL, rects, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdClearAttachments(commandBuffer, attachmentCount, attachments, rectCount, rects);
}

void vt_handle_vkCmdResolveImage(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t srcImageId;
    VkImageLayout srcImageLayout;
    uint64_t dstImageId;
    VkImageLayout dstImageLayout;
    uint32_t regionCount;

    vt_unserialize_vkCmdResolveImage((VkCommandBuffer)&commandBufferId, (VkImage)&srcImageId, &srcImageLayout, (VkImage)&dstImageId, &dstImageLayout, &regionCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkImage srcImage = VkObject_fromId(srcImageId);
    VkImage dstImage = VkObject_fromId(dstImageId);

    VkImageResolve regions[regionCount];
    vt_unserialize_vkCmdResolveImage(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, VK_NULL_HANDLE, NULL, NULL, regions, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdResolveImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, regions);
}

void vt_handle_vkCmdSetEvent(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t eventId;
    VkPipelineStageFlags stageMask;

    vt_unserialize_vkCmdSetEvent((VkCommandBuffer)&commandBufferId, (VkEvent)&eventId, &stageMask, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkEvent event = VkObject_fromId(eventId);

    vulkanWrapper.vkCmdSetEvent(commandBuffer, event, stageMask);
}

void vt_handle_vkCmdResetEvent(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t eventId;
    VkPipelineStageFlags stageMask;

    vt_unserialize_vkCmdResetEvent((VkCommandBuffer)&commandBufferId, (VkEvent)&eventId, &stageMask, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkEvent event = VkObject_fromId(eventId);

    vulkanWrapper.vkCmdResetEvent(commandBuffer, event, stageMask);
}

void vt_handle_vkCmdWaitEvents(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t eventCount;
    VkPipelineStageFlags srcStageMask;
    VkPipelineStageFlags dstStageMask;
    uint32_t memoryBarrierCount;
    uint32_t bufferMemoryBarrierCount;
    uint32_t imageMemoryBarrierCount;

    vt_unserialize_vkCmdWaitEvents((VkCommandBuffer)&commandBufferId, &eventCount, VK_NULL_HANDLE, &srcStageMask, &dstStageMask, &memoryBarrierCount, NULL, &bufferMemoryBarrierCount, NULL, &imageMemoryBarrierCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkEvent events[eventCount];
    VkMemoryBarrier memoryBarriers[memoryBarrierCount];
    VkBufferMemoryBarrier bufferMemoryBarriers[bufferMemoryBarrierCount];
    VkImageMemoryBarrier imageMemoryBarriers[imageMemoryBarrierCount];
    vt_unserialize_vkCmdWaitEvents(VK_NULL_HANDLE, NULL, events, NULL, NULL, NULL, memoryBarriers, NULL, bufferMemoryBarriers, NULL, imageMemoryBarriers, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdWaitEvents(commandBuffer, eventCount, events, srcStageMask, dstStageMask, memoryBarrierCount, memoryBarriers, bufferMemoryBarrierCount, bufferMemoryBarriers, imageMemoryBarrierCount, imageMemoryBarriers);
}

void vt_handle_vkCmdPipelineBarrier(VkContext* context) {
    uint64_t commandBufferId;
    VkPipelineStageFlags srcStageMask;
    VkPipelineStageFlags dstStageMask;
    VkDependencyFlags dependencyFlags;
    uint32_t memoryBarrierCount;
    uint32_t bufferMemoryBarrierCount;
    uint32_t imageMemoryBarrierCount;

    vt_unserialize_vkCmdPipelineBarrier((VkCommandBuffer)&commandBufferId, &srcStageMask, &dstStageMask, &dependencyFlags, &memoryBarrierCount, NULL, &bufferMemoryBarrierCount, NULL, &imageMemoryBarrierCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkMemoryBarrier memoryBarriers[memoryBarrierCount];
    VkBufferMemoryBarrier bufferMemoryBarriers[bufferMemoryBarrierCount];
    VkImageMemoryBarrier imageMemoryBarriers[imageMemoryBarrierCount];
    vt_unserialize_vkCmdPipelineBarrier(VK_NULL_HANDLE, NULL, NULL, NULL, NULL, memoryBarriers, NULL, bufferMemoryBarriers, NULL, imageMemoryBarriers, context->inputBuffer, &context->memoryPool);

    #ifdef BACHATA_VORTEK_SERVER
    for (uint32_t bi = 0; bi < imageMemoryBarrierCount; bi++) {
        VortekGpuTrack_onImageBarrier(
            commandBuffer, imageMemoryBarriers[bi].image,
            (uint32_t)srcStageMask, (uint32_t)imageMemoryBarriers[bi].srcAccessMask,
            (uint32_t)dstStageMask, (uint32_t)imageMemoryBarriers[bi].dstAccessMask,
            (uint32_t)imageMemoryBarriers[bi].oldLayout,
            (uint32_t)imageMemoryBarriers[bi].newLayout);
    }
#endif
    vulkanWrapper.vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, memoryBarriers, bufferMemoryBarrierCount, bufferMemoryBarriers, imageMemoryBarrierCount, imageMemoryBarriers);
}

void vt_handle_vkCmdBeginQuery(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t queryPoolId;
    uint32_t query;
    VkQueryControlFlags flags;

    vt_unserialize_vkCmdBeginQuery((VkCommandBuffer)&commandBufferId, (VkQueryPool)&queryPoolId, &query, &flags, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);

    vulkanWrapper.vkCmdBeginQuery(commandBuffer, queryPool, query, flags);
}

void vt_handle_vkCmdEndQuery(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t queryPoolId;
    uint32_t query;

    vt_unserialize_vkCmdEndQuery((VkCommandBuffer)&commandBufferId, (VkQueryPool)&queryPoolId, &query, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);

    vulkanWrapper.vkCmdEndQuery(commandBuffer, queryPool, query);
}

void vt_handle_vkCmdBeginConditionalRenderingEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkConditionalRenderingBeginInfoEXT conditionalRenderingBegin = {0};

    vt_unserialize_vkCmdBeginConditionalRenderingEXT((VkCommandBuffer)&commandBufferId, &conditionalRenderingBegin, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdBeginConditionalRendering(commandBuffer, &conditionalRenderingBegin);
}

void vt_handle_vkCmdEndConditionalRenderingEXT(VkContext* context) {
    uint64_t commandBufferId;

    vt_unserialize_VkCommandBuffer((VkCommandBuffer)&commandBufferId, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdEndConditionalRendering(commandBuffer);
}

void vt_handle_vkCmdResetQueryPool(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t queryPoolId;
    uint32_t firstQuery;
    uint32_t queryCount;

    vt_unserialize_vkCmdResetQueryPool((VkCommandBuffer)&commandBufferId, (VkQueryPool)&queryPoolId, &firstQuery, &queryCount, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);

    vulkanWrapper.vkCmdResetQueryPool(commandBuffer, queryPool, firstQuery, queryCount);
}

void vt_handle_vkCmdWriteTimestamp(VkContext* context) {
    uint64_t commandBufferId;
    VkPipelineStageFlagBits pipelineStage;
    uint64_t queryPoolId;
    uint32_t query;

    vt_unserialize_vkCmdWriteTimestamp((VkCommandBuffer)&commandBufferId, &pipelineStage, (VkQueryPool)&queryPoolId, &query, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);

    vulkanWrapper.vkCmdWriteTimestamp(commandBuffer, pipelineStage, queryPool, query);
}

void vt_handle_vkCmdCopyQueryPoolResults(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t queryPoolId;
    uint32_t firstQuery;
    uint32_t queryCount;
    uint64_t dstBufferId;
    VkDeviceSize dstOffset;
    VkDeviceSize stride;
    VkQueryResultFlags flags;

    vt_unserialize_vkCmdCopyQueryPoolResults((VkCommandBuffer)&commandBufferId, (VkQueryPool)&queryPoolId, &firstQuery, &queryCount, (VkBuffer)&dstBufferId, &dstOffset, &stride, &flags, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);
    VkBuffer dstBuffer = VkObject_fromId(dstBufferId);

    vulkanWrapper.vkCmdCopyQueryPoolResults(commandBuffer, queryPool, firstQuery, queryCount, dstBuffer, dstOffset, stride, flags);
}

void vt_handle_vkCmdPushConstants(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t layoutId;
    VkShaderStageFlags stageFlags;
    uint32_t offset;
    uint32_t size;

    vt_unserialize_vkCmdPushConstants((VkCommandBuffer)&commandBufferId, (VkPipelineLayout)&layoutId, &stageFlags, &offset, &size, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkPipelineLayout layout = VkObject_fromId(layoutId);

    char values[size];
    vt_unserialize_vkCmdPushConstants(VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, NULL, NULL, values, context->inputBuffer, &context->memoryPool);

    if (stageFlags & VK_SHADER_STAGE_VERTEX_BIT) stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    vulkanWrapper.vkCmdPushConstants(commandBuffer, layout, stageFlags, offset, size, values);
}

void vt_handle_vkCmdBeginRenderPass(VkContext* context) {
    uint64_t commandBufferId;
    VkRenderPassBeginInfo renderPassBegin = {0};
    VkSubpassContents contents;

    vt_unserialize_vkCmdBeginRenderPass((VkCommandBuffer)&commandBufferId, &renderPassBegin, &contents, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdBeginRenderPass(commandBuffer, &renderPassBegin, contents);
}

void vt_handle_vkCmdNextSubpass(VkContext* context) {
    uint64_t commandBufferId;
    VkSubpassContents contents;

    vt_unserialize_vkCmdNextSubpass((VkCommandBuffer)&commandBufferId, &contents, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdNextSubpass(commandBuffer, contents);
}

void vt_handle_vkCmdEndRenderPass(VkContext* context) {
    uint64_t commandBufferId;

    vt_unserialize_VkCommandBuffer((VkCommandBuffer)&commandBufferId, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdEndRenderPass(commandBuffer);
}

void vt_handle_vkCmdExecuteCommands(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t commandBufferCount;

    vt_unserialize_vkCmdExecuteCommands((VkCommandBuffer)&commandBufferId, &commandBufferCount, VK_NULL_HANDLE, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkCommandBuffer commandBuffers[commandBufferCount];
    vt_unserialize_vkCmdExecuteCommands(VK_NULL_HANDLE, NULL, commandBuffers, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onExecuteCommands(commandBuffer, (void* const*)commandBuffers, commandBufferCount);
#endif
    vulkanWrapper.vkCmdExecuteCommands(commandBuffer, commandBufferCount, commandBuffers);
}

void vt_handle_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkContext* context) {
    uint64_t physicalDeviceId;
    uint64_t windowId;

    vt_unserialize_vkGetPhysicalDeviceSurfaceCapabilitiesKHR((VkPhysicalDevice)&physicalDeviceId, (VkSurfaceKHR)&windowId, NULL, context->inputBuffer, &context->memoryPool);

    VkExtent2D windowSize;
    getWindowExtent(&context->jmethods, windowId, &windowSize);

    VkSurfaceCapabilitiesKHR surfaceCapabilities = {0};
    surfaceCapabilities.minImageCount = getSurfaceMinImageCount();
    /* Allow double-buffer; host allocates distinct AHBs per image when possible. */
    surfaceCapabilities.maxImageCount = surfaceCapabilities.minImageCount == 1 ? 2 : 0;
    surfaceCapabilities.currentExtent = windowSize;
    surfaceCapabilities.minImageExtent = windowSize;
    surfaceCapabilities.maxImageExtent = windowSize;
    surfaceCapabilities.maxImageArrayLayers = 1;
    surfaceCapabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    surfaceCapabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    surfaceCapabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR |
                                                  VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    surfaceCapabilities.supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_SAMPLED_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                              VK_IMAGE_USAGE_STORAGE_BIT |
                                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                              VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

    VT_SERIALIZE_CMD(VkSurfaceCapabilitiesKHR, &surfaceCapabilities);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceSurfaceFormatsKHR(VkContext* context) {
    uint64_t physicalDeviceId;
    uint32_t surfaceFormatCount;

    vt_unserialize_vkGetPhysicalDeviceSurfaceFormatsKHR((VkPhysicalDevice)&physicalDeviceId, NULL, &surfaceFormatCount, NULL, context->inputBuffer, &context->memoryPool);

    VkSurfaceFormatKHR* surfaceFormats = surfaceFormatCount > 0 ? getSurfaceFormats(&surfaceFormatCount) : NULL;
    if (surfaceFormatCount == 0) getSurfaceFormats(&surfaceFormatCount);

    VT_SERIALIZE_CMD(vkGetPhysicalDeviceSurfaceFormatsKHR, VK_NULL_HANDLE, NULL, &surfaceFormatCount, surfaceFormats);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);

    MEMFREE(surfaceFormats);
}

void vt_handle_vkGetPhysicalDeviceSurfacePresentModesKHR(VkContext* context) {
    static const VkPresentModeKHR supportedPresentModes[] = {VK_PRESENT_MODE_IMMEDIATE_KHR,
                                                             VK_PRESENT_MODE_MAILBOX_KHR,
                                                             VK_PRESENT_MODE_FIFO_KHR,
                                                             VK_PRESENT_MODE_FIFO_RELAXED_KHR};
    uint64_t physicalDeviceId;
    uint32_t presentModeCount;

    vt_unserialize_vkGetPhysicalDeviceSurfacePresentModesKHR((VkPhysicalDevice)&physicalDeviceId, NULL, &presentModeCount, NULL, context->inputBuffer, &context->memoryPool);

    VkPresentModeKHR* presentModes = presentModeCount > 0 ? supportedPresentModes : NULL;
    if (presentModeCount == 0) presentModeCount = ARRAY_SIZE(supportedPresentModes);

    VT_SERIALIZE_CMD(vkGetPhysicalDeviceSurfacePresentModesKHR, VK_NULL_HANDLE, NULL, &presentModeCount, presentModes);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkCreateSwapchainKHR(VkContext* context) {
    uint64_t deviceId;
    VkSwapchainCreateInfoKHR createInfo = {0};
    uint64_t windowId;
    createInfo.surface = (VkSurfaceKHR)&windowId;

    vt_unserialize_vkCreateSwapchainKHR((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkResult result = VK_SUCCESS;
    VkExtent2D windowSize;
    getWindowExtent(&context->jmethods, windowId, &windowSize);

    XWindowSwapchain* swapchain = NULL;
    if (createInfo.imageExtent.width == windowSize.width && createInfo.imageExtent.height == windowSize.height) {
        swapchain = XWindowSwapchain_create(device, context->graphicsQueueIndex, &createInfo, &context->jmethods, windowId);
        if (!swapchain) result = VK_ERROR_INITIALIZATION_FAILED;
    }
    else result = VK_ERROR_SURFACE_LOST_KHR;

    VT_SERIALIZE_CMD(VkSwapchainKHR, (VkSwapchainKHR)swapchain);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroySwapchainKHR(VkContext* context) {
    uint64_t deviceId;
    uint64_t swapchainId;

    vt_unserialize_vkDestroySwapchainKHR((VkDevice)&deviceId, (VkSwapchainKHR)&swapchainId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    XWindowSwapchain* swapchain = VkObject_fromId(swapchainId);

    XWindowSwapchain_destroy(device, swapchain);
}

void vt_handle_vkGetSwapchainImagesKHR(VkContext* context) {
    uint64_t deviceId;
    uint64_t swapchainId;
    uint32_t swapchainImageCount = 0;

    vt_unserialize_vkGetSwapchainImagesKHR((VkDevice)&deviceId, (VkSwapchainKHR)&swapchainId, &swapchainImageCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    XWindowSwapchain* swapchain = VkObject_fromId(swapchainId);

    VkImage* swapchainImages = swapchainImageCount > 0 ? calloc(swapchain->imageCount, sizeof(VkImage)) : NULL;
    if (swapchainImages) {
        for (int i = 0; i < swapchain->imageCount; i++) {
            swapchainImages[i] = swapchain->images[i].image;
        }
        swapchainImageCount = (uint32_t)swapchain->imageCount;
    } else {
        swapchainImageCount = (uint32_t)swapchain->imageCount;
    }

    VT_SERIALIZE_CMD(vkGetSwapchainImagesKHR, NULL, VK_NULL_HANDLE, &swapchainImageCount, swapchainImages);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);

    MEMFREE(swapchainImages);
}

void vt_handle_vkAcquireNextImageKHR(VkContext* context) {
    uint64_t deviceId;
    uint64_t swapchainId;
    uint64_t timeout;
    uint64_t semaphoreId;
    uint64_t fenceId;

    vt_unserialize_vkAcquireNextImageKHR((VkDevice)&deviceId, (VkSwapchainKHR)&swapchainId, &timeout, (VkSemaphore)&semaphoreId, (VkFence)&fenceId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    XWindowSwapchain* swapchain = VkObject_fromId(swapchainId);
    VkSemaphore semaphore = VkObject_fromId(semaphoreId);
    VkFence fence = VkObject_fromId(fenceId);

    uint32_t imageIndex = 0;
    VkResult result = XWindowSwapchain_acquireNextImage(swapchain, timeout, semaphore, fence, &imageIndex);
    if (result == VK_ERROR_DEVICE_LOST) context->status = result;
    /* ACQUIRE_RESULT logged inside XWindowSwapchain_acquireNextImage when server. */

    vt_send(context->clientRing, result == VK_SUCCESS ? imageIndex : result, NULL, 0);
}

void vt_handle_vkQueuePresentKHR(VkContext* context) {
    VkPresentInfoKHR presentInfo = {0};
    vt_unserialize_VkPresentInfoKHR(&presentInfo, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    {
        static uint64_t s_guest_present_id = 0;
        s_guest_present_id++;
        uint32_t imageIndex = 0;
        if (presentInfo.pImageIndices && presentInfo.swapchainCount > 0) {
            imageIndex = presentInfo.pImageIndices[0];
        }
        for (int i = 0; i < presentInfo.swapchainCount; i++) {
            XWindowSwapchain* sc = (XWindowSwapchain*)presentInfo.pSwapchains[i];
            VkResult pr = BachataXWindowSwapchain_presentWithWaits(
                sc, imageIndex, presentInfo.pWaitSemaphores, presentInfo.waitSemaphoreCount,
                s_guest_present_id);
            if (pr == VK_ERROR_DEVICE_LOST) {
                context->status = pr;
            }
        }
        bachata_drain_deferred_destroys();
    }
#else
    for (int i = 0; i < presentInfo.swapchainCount; i++) {
        XWindowSwapchain_presentImage((XWindowSwapchain*)presentInfo.pSwapchains[i]);
    }
#endif
}

void vt_handle_vkGetPhysicalDeviceFeatures2(VkContext* context) {
    uint64_t physicalDeviceId;
    VkPhysicalDeviceFeatures2 features = {0};

    vt_unserialize_vkGetPhysicalDeviceFeatures2((VkPhysicalDevice)&physicalDeviceId, &features, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    vulkanWrapper.vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
    checkDeviceFeatures(&features.features, features.pNext);

    VT_SERIALIZE_CMD(VkPhysicalDeviceFeatures2, &features);
    vt_send(context->clientRing, 0, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceProperties2(VkContext* context) {
    uint64_t physicalDeviceId;
    VkPhysicalDeviceProperties2 properties = {0};

    vt_unserialize_vkGetPhysicalDeviceProperties2((VkPhysicalDevice)&physicalDeviceId, &properties, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    vulkanWrapper.vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
    checkDeviceProperties(context, &properties.properties, properties.pNext);

    VT_SERIALIZE_CMD(VkPhysicalDeviceProperties2, &properties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceFormatProperties2(VkContext* context) {
    uint64_t physicalDeviceId;
    VkFormat format;
    VkFormatProperties2 formatProperties = {0};

    vt_unserialize_vkGetPhysicalDeviceFormatProperties2((VkPhysicalDevice)&physicalDeviceId, &format, &formatProperties, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    vulkanWrapper.vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, &formatProperties);
    checkFormatProperties(physicalDevice, format, &formatProperties.formatProperties);

    VT_SERIALIZE_CMD(VkFormatProperties2, &formatProperties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceImageFormatProperties2(VkContext* context) {
    uint64_t physicalDeviceId;
    VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {0};
    VkImageFormatProperties2 imageFormatProperties = {0};

    vt_unserialize_vkGetPhysicalDeviceImageFormatProperties2((VkPhysicalDevice)&physicalDeviceId, &imageFormatInfo, &imageFormatProperties, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkResult result = vulkanWrapper.vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, &imageFormatInfo, &imageFormatProperties);
    checkImageFormatProperties(imageFormatInfo.format, imageFormatInfo.type, imageFormatInfo.tiling, imageFormatInfo.usage, imageFormatInfo.flags, &imageFormatProperties.imageFormatProperties, &result);

    VT_SERIALIZE_CMD(VkImageFormatProperties2, &imageFormatProperties);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceQueueFamilyProperties2(VkContext* context) {
    uint64_t physicalDeviceId;
    uint32_t queueFamilyPropertyCount;

    vt_unserialize_vkGetPhysicalDeviceQueueFamilyProperties2((VkPhysicalDevice)&physicalDeviceId, &queueFamilyPropertyCount, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkQueueFamilyProperties2* queueFamilyProperties = queueFamilyPropertyCount > 0 ? calloc(queueFamilyPropertyCount, sizeof(VkQueueFamilyProperties2)) : NULL;
    if (queueFamilyProperties) vt_unserialize_vkGetPhysicalDeviceQueueFamilyProperties2(VK_NULL_HANDLE, NULL, queueFamilyProperties, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &queueFamilyPropertyCount, queueFamilyProperties);

    VT_SERIALIZE_CMD(vkGetPhysicalDeviceQueueFamilyProperties2, VK_NULL_HANDLE, &queueFamilyPropertyCount, queueFamilyProperties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);

    MEMFREE(queueFamilyProperties);
}

void vt_handle_vkGetPhysicalDeviceMemoryProperties2(VkContext* context) {
    uint64_t physicalDeviceId;
    VkPhysicalDeviceMemoryProperties2 memoryProperties = {0};

    vt_unserialize_vkGetPhysicalDeviceMemoryProperties2((VkPhysicalDevice)&physicalDeviceId, &memoryProperties, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    vulkanWrapper.vkGetPhysicalDeviceMemoryProperties2(physicalDevice, &memoryProperties);
    checkDeviceMemoryProperties(context, &memoryProperties.memoryProperties, memoryProperties.pNext);

    VT_SERIALIZE_CMD(VkPhysicalDeviceMemoryProperties2, &memoryProperties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceSparseImageFormatProperties2(VkContext* context) {
    uint64_t physicalDeviceId;
    VkPhysicalDeviceSparseImageFormatInfo2 formatInfo = {0};
    uint32_t propertyCount;

    vt_unserialize_vkGetPhysicalDeviceSparseImageFormatProperties2((VkPhysicalDevice)&physicalDeviceId, &formatInfo, &propertyCount, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkSparseImageFormatProperties2* properties = propertyCount > 0 ? calloc(propertyCount, sizeof(VkSparseImageFormatProperties2)) : NULL;
    if (properties) vt_unserialize_vkGetPhysicalDeviceSparseImageFormatProperties2(VK_NULL_HANDLE, NULL, NULL, properties, context->inputBuffer, &context->memoryPool);
    
    vulkanWrapper.vkGetPhysicalDeviceSparseImageFormatProperties2(physicalDevice, &formatInfo, &propertyCount, properties);

    VT_SERIALIZE_CMD(vkGetPhysicalDeviceSparseImageFormatProperties2, VK_NULL_HANDLE, NULL, &propertyCount, properties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);

    MEMFREE(properties);
}

void vt_handle_vkCmdPushDescriptorSetKHR(VkContext* context) {
    uint64_t commandBufferId;
    VkPipelineBindPoint pipelineBindPoint;
    uint64_t layoutId;
    uint32_t set;
    uint32_t descriptorWriteCount;

    vt_unserialize_vkCmdPushDescriptorSetKHR((VkCommandBuffer)&commandBufferId, &pipelineBindPoint, (VkPipelineLayout)&layoutId, &set, &descriptorWriteCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkPipelineLayout layout = VkObject_fromId(layoutId);

    VkWriteDescriptorSet descriptorWrites[descriptorWriteCount];
    vt_unserialize_vkCmdPushDescriptorSetKHR(VK_NULL_HANDLE, NULL, VK_NULL_HANDLE, NULL, NULL, descriptorWrites, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    /* Detile uses push descriptors: stamp real buffer resources before dispatch. */
    {
        void* bufs[64];
        uint32_t bindings[64];
        uint32_t dtypes[64];
        uint64_t offs[64];
        uint64_t ranges[64];
        uint32_t bc = 0;
        for (uint32_t wi = 0; wi < descriptorWriteCount && bc < 64; wi++) {
            const VkWriteDescriptorSet* w = &descriptorWrites[wi];
            if (!w->pBufferInfo || w->descriptorCount == 0) {
                continue;
            }
            for (uint32_t bi = 0; bi < w->descriptorCount && bc < 64; bi++) {
                if (!w->pBufferInfo[bi].buffer) {
                    continue;
                }
                bufs[bc] = (void*)w->pBufferInfo[bi].buffer;
                bindings[bc] = w->dstBinding + bi;
                dtypes[bc] = (uint32_t)w->descriptorType;
                offs[bc] = (uint64_t)w->pBufferInfo[bi].offset;
                ranges[bc] = (uint64_t)w->pBufferInfo[bi].range;
                bc++;
            }
        }
        if (bc > 0) {
            VortekGpuTrack_onPushDescriptorBuffers(commandBuffer, set, bindings, dtypes, bufs, offs,
                                                   ranges, bc);
        }
    }
#endif
    vulkanWrapper.vkCmdPushDescriptorSet(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, descriptorWrites);
}

void vt_handle_vkTrimCommandPool(VkContext* context) {
    uint64_t deviceId;
    uint64_t commandPoolId;
    VkCommandPoolTrimFlags flags;

    vt_unserialize_vkTrimCommandPool((VkDevice)&deviceId, (VkCommandPool)&commandPoolId, &flags, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkCommandPool commandPool = VkObject_fromId(commandPoolId);

    vulkanWrapper.vkTrimCommandPool(device, commandPool, flags);
}

void vt_handle_vkGetPhysicalDeviceExternalBufferProperties(VkContext* context) {
    uint64_t physicalDeviceId;
    VkPhysicalDeviceExternalBufferInfo bufferInfo = {0};

    vt_unserialize_vkGetPhysicalDeviceExternalBufferProperties((VkPhysicalDevice)&physicalDeviceId, &bufferInfo, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkExternalBufferProperties properties = {0};
    vulkanWrapper.vkGetPhysicalDeviceExternalBufferProperties(physicalDevice, &bufferInfo,  &properties);

    VT_SERIALIZE_CMD(VkExternalBufferProperties, &properties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetMemoryFdKHR(VkContext* context) {
    uint64_t memoryId;
    vt_unserialize_VkDeviceMemory((VkDeviceMemory)&memoryId, context->inputBuffer, &context->memoryPool);
    ResourceMemory* resourceMemory = VkObject_fromId(memoryId);

    VkResult result = resourceMemory->fd != -1 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
    send_fds(context->clientFd, &resourceMemory->fd, 1, &result, sizeof(VkResult));
}

void vt_handle_vkGetPhysicalDeviceExternalSemaphoreProperties(VkContext* context) {
    uint64_t physicalDeviceId;
    VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo = {0};

    vt_unserialize_vkGetPhysicalDeviceExternalSemaphoreProperties((VkPhysicalDevice)&physicalDeviceId, &semaphoreInfo, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkExternalSemaphoreProperties properties = {0};
    vulkanWrapper.vkGetPhysicalDeviceExternalSemaphoreProperties(physicalDevice, &semaphoreInfo,  &properties);

    VT_SERIALIZE_CMD(VkExternalSemaphoreProperties, &properties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetSemaphoreFdKHR(VkContext* context) {
    uint64_t deviceId;
    VkSemaphoreGetFdInfoKHR getFdInfo = {0};

    vt_unserialize_vkGetSemaphoreFdKHR((VkDevice)&deviceId, &getFdInfo, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    getFdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

    int fd;
    VkResult result = vulkanWrapper.vkGetSemaphoreFd(device, &getFdInfo, &fd);
    send_fds(context->clientFd, &fd, 1, &result, sizeof(VkResult));
    CLOSEFD(fd);
}

void vt_handle_vkGetPhysicalDeviceExternalFenceProperties(VkContext* context) {
    uint64_t physicalDeviceId;
    VkPhysicalDeviceExternalFenceInfo fenceInfo = {0};

    vt_unserialize_vkGetPhysicalDeviceExternalFenceProperties((VkPhysicalDevice)&physicalDeviceId, &fenceInfo, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkExternalFenceProperties properties = {0};
    vulkanWrapper.vkGetPhysicalDeviceExternalFenceProperties(physicalDevice, &fenceInfo,  &properties);

    VT_SERIALIZE_CMD(VkExternalFenceProperties, &properties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetFenceFdKHR(VkContext* context) {
    uint64_t deviceId;
    VkFenceGetFdInfoKHR getFdInfo = {0};

    vt_unserialize_vkGetFenceFdKHR((VkDevice)&deviceId, &getFdInfo, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    getFdInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

    int fd;
    VkResult result = vulkanWrapper.vkGetFenceFd(device, &getFdInfo, &fd);
    send_fds(context->clientFd, &fd, 1, &result, sizeof(VkResult));
    CLOSEFD(fd);
}

void vt_handle_vkEnumeratePhysicalDeviceGroups(VkContext* context) {
    uint32_t physicalDeviceGroupCount;
    uint64_t instanceId;
    vt_unserialize_vkEnumeratePhysicalDeviceGroups((VkInstance)&instanceId, &physicalDeviceGroupCount, NULL, context->inputBuffer, &context->memoryPool);
    VkInstance instance = VkObject_fromId(instanceId);

    VkPhysicalDeviceGroupProperties* physicalDeviceGroupProperties = physicalDeviceGroupCount > 0 ? calloc(physicalDeviceGroupCount, sizeof(VkPhysicalDeviceGroupProperties)) : NULL;

    if (physicalDeviceGroupProperties) {
        for (int i = 0; i < physicalDeviceGroupCount; i++) physicalDeviceGroupProperties[i].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
    }

    VkResult result = vulkanWrapper.vkEnumeratePhysicalDeviceGroups(instance, &physicalDeviceGroupCount, physicalDeviceGroupProperties);

    VT_SERIALIZE_CMD(vkEnumeratePhysicalDeviceGroups, VK_NULL_HANDLE, &physicalDeviceGroupCount, physicalDeviceGroupProperties);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);

    MEMFREE(physicalDeviceGroupProperties);
}

void vt_handle_vkGetDeviceGroupPeerMemoryFeatures(VkContext* context) {
    uint64_t deviceId;
    uint32_t heapIndex;
    uint32_t localDeviceIndex;
    uint32_t remoteDeviceIndex;

    vt_unserialize_vkGetDeviceGroupPeerMemoryFeatures((VkDevice)&deviceId, &heapIndex, &localDeviceIndex, &remoteDeviceIndex, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkPeerMemoryFeatureFlags featureFlags = 0;
    vulkanWrapper.vkGetDeviceGroupPeerMemoryFeatures(device, heapIndex, localDeviceIndex, remoteDeviceIndex, &featureFlags);

    vt_send(context->clientRing, VK_SUCCESS, &featureFlags, 4);
}

void vt_handle_vkBindBufferMemory2(VkContext* context) {
    uint64_t deviceId;
    uint32_t bindInfoCount;

    vt_unserialize_vkBindBufferMemory2((VkDevice)&deviceId, &bindInfoCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkBindBufferMemoryInfo bindInfos[bindInfoCount];
    if (context->textureDecoder) {
        vortekSerializerCastVkObject = false;
        vt_unserialize_vkBindBufferMemory2(VK_NULL_HANDLE, NULL, bindInfos, context->inputBuffer, &context->memoryPool);
        vortekSerializerCastVkObject = true;

        for (int i = 0; i < bindInfoCount; i++) {
            ResourceMemory* resourceMemory = (ResourceMemory*)bindInfos[i].memory;
#ifdef BACHATA_VORTEK_SERVER
            VkMemoryRequirements memReqs = {0};
            vulkanWrapper.vkGetBufferMemoryRequirements(device, bindInfos[i].buffer, &memReqs);
            VortekGpuTrack_onBindBuffer(bindInfos[i].buffer, resourceMemory,
                                        (uint64_t)bindInfos[i].memoryOffset,
                                        (uint64_t)memReqs.size, (uint64_t)memReqs.alignment);
            bachata_note_track_device(device);
            {
                uint32_t typeIndex = getMemoryTypeIndex(
                    memReqs.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if ((memReqs.memoryTypeBits & (1u << typeIndex)) == 0) {
                    for (uint32_t ti = 0; ti < 32; ++ti) {
                        if (memReqs.memoryTypeBits & (1u << ti)) {
                            typeIndex = ti;
                            break;
                        }
                    }
                }
                VkDeviceMemory poolMem = VK_NULL_HANDLE;
                VkDeviceSize poolOff = 0;
                if (bachata_try_pool_bind(device, bindInfos[i].buffer, resourceMemory,
                                          bindInfos[i].memoryOffset, memReqs.size, typeIndex,
                                          &poolMem, &poolOff)) {
                    bindInfos[i].memory = poolMem;
                    bindInfos[i].memoryOffset = poolOff;
                } else {
                    bachata_wait_suballoc_overlap(device);
                    bindInfos[i].memory = resourceMemory->memory;
                }
            }
#else
            bindInfos[i].memory = resourceMemory->memory;
#endif
            TextureDecoder_addBoundBuffer(context->textureDecoder, resourceMemory, bindInfos[i].buffer, bindInfos[i].memoryOffset);
        }
    }
    else {
#ifdef BACHATA_VORTEK_SERVER
        vortekSerializerCastVkObject = false;
        vt_unserialize_vkBindBufferMemory2(VK_NULL_HANDLE, NULL, bindInfos, context->inputBuffer, &context->memoryPool);
        vortekSerializerCastVkObject = true;
        for (uint32_t i = 0; i < bindInfoCount; i++) {
            ResourceMemory* resourceMemory = (ResourceMemory*)bindInfos[i].memory;
            VkMemoryRequirements memReqs = {0};
            vulkanWrapper.vkGetBufferMemoryRequirements(device, bindInfos[i].buffer, &memReqs);
            VortekGpuTrack_onBindBuffer(bindInfos[i].buffer, resourceMemory,
                                        (uint64_t)bindInfos[i].memoryOffset,
                                        (uint64_t)memReqs.size, (uint64_t)memReqs.alignment);
            bachata_note_track_device(device);
            {
                uint32_t typeIndex = getMemoryTypeIndex(
                    memReqs.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if ((memReqs.memoryTypeBits & (1u << typeIndex)) == 0) {
                    for (uint32_t ti = 0; ti < 32; ++ti) {
                        if (memReqs.memoryTypeBits & (1u << ti)) {
                            typeIndex = ti;
                            break;
                        }
                    }
                }
                VkDeviceMemory poolMem = VK_NULL_HANDLE;
                VkDeviceSize poolOff = 0;
                if (bachata_try_pool_bind(device, bindInfos[i].buffer, resourceMemory,
                                          bindInfos[i].memoryOffset, memReqs.size, typeIndex,
                                          &poolMem, &poolOff)) {
                    bindInfos[i].memory = poolMem;
                    bindInfos[i].memoryOffset = poolOff;
                } else {
                    bachata_wait_suballoc_overlap(device);
                    bindInfos[i].memory = resourceMemory->memory;
                }
            }
        }
#else
        vt_unserialize_vkBindBufferMemory2(VK_NULL_HANDLE, NULL, bindInfos, context->inputBuffer, &context->memoryPool);
#endif
    }

    VkResult result = vulkanWrapper.vkBindBufferMemory2(device, bindInfoCount, bindInfos);

    if (result != VK_SUCCESS && context->textureDecoder) {
        for (int i = 0; i < bindInfoCount; i++) TextureDecoder_removeBoundBuffer(context->textureDecoder, bindInfos[i].buffer);
    }

    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkBindImageMemory2(VkContext* context) {
    uint64_t deviceId;
    uint32_t bindInfoCount;

    vt_unserialize_vkBindImageMemory2((VkDevice)&deviceId, &bindInfoCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkBindImageMemoryInfo bindInfos[bindInfoCount];
#ifdef BACHATA_VORTEK_SERVER
    vortekSerializerCastVkObject = false;
    vt_unserialize_vkBindImageMemory2(VK_NULL_HANDLE, NULL, bindInfos, context->inputBuffer, &context->memoryPool);
    vortekSerializerCastVkObject = true;
    for (uint32_t i = 0; i < bindInfoCount; i++) {
        ResourceMemory* resourceMemory = (ResourceMemory*)bindInfos[i].memory;
        VkMemoryRequirements memReqs = {0};
        vulkanWrapper.vkGetImageMemoryRequirements(device, bindInfos[i].image, &memReqs);
        VortekGpuTrack_onBindImage(bindInfos[i].image, resourceMemory,
                                   (uint64_t)bindInfos[i].memoryOffset,
                                   (uint64_t)memReqs.size, (uint64_t)memReqs.alignment);
        bindInfos[i].memory = resourceMemory->memory;
    }
#else
    vt_unserialize_vkBindImageMemory2(VK_NULL_HANDLE, NULL, bindInfos, context->inputBuffer, &context->memoryPool);
#endif

    VkResult result = vulkanWrapper.vkBindImageMemory2(device, bindInfoCount, bindInfos);
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkCmdSetDeviceMask(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t deviceMask;

    vt_unserialize_vkCmdSetDeviceMask((VkCommandBuffer)&commandBufferId, &deviceMask, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDeviceMask(commandBuffer, deviceMask);
}

void vt_handle_vkAcquireNextImage2KHR(VkContext* context) {
    VkAcquireNextImageInfoKHR acquireInfo = {0};
    vt_unserialize_VkAcquireNextImageInfoKHR(&acquireInfo, context->inputBuffer, &context->memoryPool);
    XWindowSwapchain* swapchain = (XWindowSwapchain*)acquireInfo.swapchain;

    uint32_t imageIndex = 0;
    VkResult result = XWindowSwapchain_acquireNextImage(swapchain, acquireInfo.timeout, acquireInfo.semaphore, acquireInfo.fence, &imageIndex);
    if (result == VK_ERROR_DEVICE_LOST) context->status = result;

    vt_send(context->clientRing, result == VK_SUCCESS ? imageIndex : result, NULL, 0);
}

void vt_handle_vkCmdDispatchBase(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t baseGroupX;
    uint32_t baseGroupY;
    uint32_t baseGroupZ;
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;

    vt_unserialize_vkCmdDispatchBase((VkCommandBuffer)&commandBufferId, &baseGroupX, &baseGroupY, &baseGroupZ, &groupCountX, &groupCountY, &groupCountZ, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdDispatchBase(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
}

void vt_handle_vkGetPhysicalDevicePresentRectanglesKHR(VkContext* context) {
    uint64_t physicalDeviceId;
    uint64_t windowId;
    uint32_t rectCount;

    vt_unserialize_vkGetPhysicalDevicePresentRectanglesKHR((VkPhysicalDevice)&physicalDeviceId, (VkSurfaceKHR)&windowId, &rectCount, NULL, context->inputBuffer, &context->memoryPool);

    VkRect2D* rects = rectCount > 0 ? calloc(1, sizeof(VkPhysicalDeviceGroupProperties)) : NULL;
    if (rects) getWindowExtent(&context->jmethods, windowId, &rects[0].extent);
    rectCount = 1;

    VT_SERIALIZE_CMD(vkGetPhysicalDevicePresentRectanglesKHR, VK_NULL_HANDLE, VK_NULL_HANDLE, &rectCount, rects);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);

    MEMFREE(rects);
}

void vt_handle_vkCmdSetSampleLocationsEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkSampleLocationsInfoEXT sampleLocationsInfo = {0};

    vt_unserialize_vkCmdSetSampleLocationsEXT((VkCommandBuffer)&commandBufferId, &sampleLocationsInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetSampleLocations(commandBuffer, &sampleLocationsInfo);
}

void vt_handle_vkGetPhysicalDeviceMultisamplePropertiesEXT(VkContext* context) {
    uint64_t physicalDeviceId;
    VkSampleCountFlagBits samples;

    vt_unserialize_vkGetPhysicalDeviceMultisamplePropertiesEXT((VkPhysicalDevice)&physicalDeviceId, &samples, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkMultisamplePropertiesEXT multisampleProperties = {0};
    vulkanWrapper.vkGetPhysicalDeviceMultisampleProperties(physicalDevice, samples, &multisampleProperties);

    VT_SERIALIZE_CMD(VkMultisamplePropertiesEXT, &multisampleProperties);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetBufferMemoryRequirements2(VkContext* context) {
    uint64_t deviceId;
    VkBufferMemoryRequirementsInfo2 requirementsInfo = {0};
    VkMemoryRequirements2 memoryRequirements = {0};

    vt_unserialize_vkGetBufferMemoryRequirements2((VkDevice)&deviceId, &requirementsInfo, &memoryRequirements, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    vulkanWrapper.vkGetBufferMemoryRequirements2(device, &requirementsInfo, &memoryRequirements);

    VT_SERIALIZE_CMD(VkMemoryRequirements2, &memoryRequirements);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetImageMemoryRequirements2(VkContext* context) {
    uint64_t deviceId;
    VkImageMemoryRequirementsInfo2 requirementsInfo = {0};
    VkMemoryRequirements2 memoryRequirements = {0};

    vt_unserialize_vkGetImageMemoryRequirements2((VkDevice)&deviceId, &requirementsInfo, &memoryRequirements, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    vulkanWrapper.vkGetImageMemoryRequirements2(device, &requirementsInfo, &memoryRequirements);

    VT_SERIALIZE_CMD(VkMemoryRequirements2, &memoryRequirements);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetImageSparseMemoryRequirements2(VkContext* context) {
    uint64_t deviceId;
    VkImageSparseMemoryRequirementsInfo2 requirementsInfo = {0};
    uint32_t requirementCount;

    vt_unserialize_vkGetImageSparseMemoryRequirements2((VkDevice)&deviceId, &requirementsInfo, &requirementCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkSparseImageMemoryRequirements2* requirements = requirementCount > 0 ? calloc(requirementCount, sizeof(VkSparseImageMemoryRequirements2)) : NULL;
    if (requirements) vt_unserialize_vkGetImageSparseMemoryRequirements2(VK_NULL_HANDLE, NULL, NULL, requirements, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkGetImageSparseMemoryRequirements2(device, &requirementsInfo, &requirementCount, requirements);

    VT_SERIALIZE_CMD(vkGetImageSparseMemoryRequirements2, VK_NULL_HANDLE, NULL, &requirementCount, requirements);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);

    MEMFREE(requirements);
}

void vt_handle_vkGetDeviceBufferMemoryRequirements(VkContext* context) {
    uint64_t deviceId;
    VkDeviceBufferMemoryRequirements requirementsInfo = {0};
    VkMemoryRequirements2 memoryRequirements = {0};

    vt_unserialize_vkGetDeviceBufferMemoryRequirements((VkDevice)&deviceId, &requirementsInfo, &memoryRequirements, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    vulkanWrapper.vkGetDeviceBufferMemoryRequirements(device, &requirementsInfo, &memoryRequirements);

    VT_SERIALIZE_CMD(VkMemoryRequirements2, &memoryRequirements);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetDeviceImageMemoryRequirements(VkContext* context) {
    uint64_t deviceId;
    VkDeviceImageMemoryRequirements requirementsInfo = {0};
    VkMemoryRequirements2 memoryRequirements = {0};

    vt_unserialize_vkGetDeviceImageMemoryRequirements((VkDevice)&deviceId, &requirementsInfo, &memoryRequirements, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    vulkanWrapper.vkGetDeviceImageMemoryRequirements(device, &requirementsInfo, &memoryRequirements);

    VT_SERIALIZE_CMD(VkMemoryRequirements2, &memoryRequirements);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetDeviceImageSparseMemoryRequirements(VkContext* context) {
    uint64_t deviceId;
    VkDeviceImageMemoryRequirements requirementsInfo = {0};
    uint32_t requirementCount;

    vt_unserialize_vkGetDeviceImageSparseMemoryRequirements((VkDevice)&deviceId, &requirementsInfo, &requirementCount, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkSparseImageMemoryRequirements2* requirements = requirementCount > 0 ? calloc(requirementCount, sizeof(VkSparseImageMemoryRequirements2)) : NULL;
    if (requirements) vt_unserialize_vkGetDeviceImageSparseMemoryRequirements(VK_NULL_HANDLE, NULL, NULL, requirements, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkGetDeviceImageSparseMemoryRequirements(device, &requirementsInfo, &requirementCount, requirements);

    VT_SERIALIZE_CMD(vkGetDeviceImageSparseMemoryRequirements, VK_NULL_HANDLE, NULL, &requirementCount, requirements);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);

    MEMFREE(requirements);
}

void vt_handle_vkCreateSamplerYcbcrConversion(VkContext* context) {
    uint64_t deviceId;
    VkSamplerYcbcrConversionCreateInfo createInfo = {0};

    vt_unserialize_vkCreateSamplerYcbcrConversion((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkSamplerYcbcrConversion ycbcrConversion;
    VkResult result = vulkanWrapper.vkCreateSamplerYcbcrConversion(device, &createInfo, NULL, &ycbcrConversion);

    VT_SERIALIZE_CMD(VkSamplerYcbcrConversion, ycbcrConversion);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkDestroySamplerYcbcrConversion(VkContext* context) {
    uint64_t deviceId;
    uint64_t ycbcrConversionId;

    vt_unserialize_vkDestroySamplerYcbcrConversion((VkDevice)&deviceId, (VkSamplerYcbcrConversion)&ycbcrConversionId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkSamplerYcbcrConversion ycbcrConversion = VkObject_fromId(ycbcrConversionId);

    vulkanWrapper.vkDestroySamplerYcbcrConversion(device, ycbcrConversion, NULL);
}

void vt_handle_vkGetDeviceQueue2(VkContext* context) {
    uint64_t deviceId;
    VkDeviceQueueInfo2 queueInfo = {0};

    vt_unserialize_vkGetDeviceQueue2((VkDevice)&deviceId, &queueInfo, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkQueue queue;
    vulkanWrapper.vkGetDeviceQueue2(device, &queueInfo, &queue);

    VT_SERIALIZE_CMD(VkQueue, queue);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetDescriptorSetLayoutSupport(VkContext* context) {
    uint64_t deviceId;
    VkDescriptorSetLayoutCreateInfo createInfo = {0};

    vt_unserialize_vkGetDescriptorSetLayoutSupport((VkDevice)&deviceId, &createInfo, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkDescriptorSetLayoutSupport support = {0};
    vulkanWrapper.vkGetDescriptorSetLayoutSupport(device, &createInfo, &support);

    VT_SERIALIZE_CMD(VkDescriptorSetLayoutSupport, &support);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(VkContext* context) {
    uint64_t physicalDeviceId;
    uint32_t timeDomainCount;

    vt_unserialize_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR((VkPhysicalDevice)&physicalDeviceId, &timeDomainCount, NULL, context->inputBuffer, &context->memoryPool);
    VkPhysicalDevice physicalDevice = VkObject_fromId(physicalDeviceId);

    VkTimeDomainKHR* timeDomains = timeDomainCount > 0 ? calloc(timeDomainCount, sizeof(VkTimeDomainKHR)) : NULL;
    VkResult result = vulkanWrapper.vkGetPhysicalDeviceCalibrateableTimeDomains(physicalDevice, &timeDomainCount, timeDomains);

    VT_SERIALIZE_CMD(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR, VK_NULL_HANDLE, &timeDomainCount, timeDomains);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetCalibratedTimestampsKHR(VkContext* context) {
    uint64_t deviceId;
    uint32_t timestampCount;

    vt_unserialize_vkGetCalibratedTimestampsKHR((VkDevice)&deviceId, &timestampCount, NULL, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkCalibratedTimestampInfoKHR timestampInfos[timestampCount];
    vt_unserialize_vkGetCalibratedTimestampsKHR(VK_NULL_HANDLE, NULL, timestampInfos, NULL, NULL, context->inputBuffer, &context->memoryPool);

    uint64_t timestamps[timestampCount];
    uint64_t maxDeviation;
    VkResult result = vulkanWrapper.vkGetCalibratedTimestamps(device, timestampCount, timestampInfos, timestamps, &maxDeviation);

    VT_SERIALIZE_CMD(vkGetCalibratedTimestampsKHR, VK_NULL_HANDLE, timestampCount, NULL, timestamps, &maxDeviation);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkCreateRenderPass2(VkContext* context) {
    uint64_t deviceId;
    VkRenderPassCreateInfo2 createInfo = {0};

    vt_unserialize_vkCreateRenderPass2((VkDevice)&deviceId, &createInfo, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkRenderPass renderPass;
    VkResult result = vulkanWrapper.vkCreateRenderPass2(device, &createInfo, NULL, &renderPass);

    VT_SERIALIZE_CMD(VkRenderPass, renderPass);
    vt_send(context->clientRing, result, outputBuffer, bufferSize);
}

void vt_handle_vkCmdBeginRenderPass2(VkContext* context) {
    uint64_t commandBufferId;
    VkRenderPassBeginInfo renderPassBegin = {0};
    VkSubpassBeginInfo subpassBeginInfo = {0};

    vt_unserialize_vkCmdBeginRenderPass2((VkCommandBuffer)&commandBufferId, &renderPassBegin, &subpassBeginInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdBeginRenderPass2(commandBuffer, &renderPassBegin, &subpassBeginInfo);
}

void vt_handle_vkCmdNextSubpass2(VkContext* context) {
    uint64_t commandBufferId;
    VkSubpassBeginInfo subpassBeginInfo = {0};
    VkSubpassEndInfo subpassEndInfo = {0};

    vt_unserialize_vkCmdNextSubpass2((VkCommandBuffer)&commandBufferId, &subpassBeginInfo, &subpassEndInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdNextSubpass2(commandBuffer, &subpassBeginInfo, &subpassEndInfo);
}

void vt_handle_vkCmdEndRenderPass2(VkContext* context) {
    uint64_t commandBufferId;
    VkSubpassEndInfo subpassEndInfo = {0};

    vt_unserialize_vkCmdEndRenderPass2((VkCommandBuffer)&commandBufferId, &subpassEndInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdEndRenderPass2(commandBuffer, &subpassEndInfo);
}

void vt_handle_vkGetSemaphoreCounterValue(VkContext* context) {
    uint64_t deviceId;
    uint64_t semaphoreId;

    vt_unserialize_vkGetSemaphoreCounterValue((VkDevice)&deviceId, (VkSemaphore)&semaphoreId, NULL, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    VkSemaphore semaphore = VkObject_fromId(semaphoreId);

    uint64_t value = 0;
    VkResult result = vulkanWrapper.vkGetSemaphoreCounterValue(device, semaphore, &value);
    vt_send(context->clientRing, result, &semaphore, sizeof(uint64_t));
}

void vt_handle_vkWaitSemaphores(VkContext* context) {
    TimelineSemaphore_asyncWait(context->clientFd, context->threadPool, context->inputBuffer, context->inputBufferSize);
}

void vt_handle_vkSignalSemaphore(VkContext* context) {
    uint64_t deviceId;
    VkSemaphoreSignalInfo signalInfo = {0};

    vt_unserialize_vkSignalSemaphore((VkDevice)&deviceId, &signalInfo, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkResult result = vulkanWrapper.vkSignalSemaphore(device, &signalInfo);
    vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkCmdDrawIndirectCount(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t bufferId;
    VkDeviceSize offset;
    uint64_t countBufferId;
    VkDeviceSize countBufferOffset;
    uint32_t maxDrawCount;
    uint32_t stride;

    vt_unserialize_vkCmdDrawIndirectCount((VkCommandBuffer)&commandBufferId, (VkBuffer)&bufferId, &offset, (VkBuffer)&countBufferId, &countBufferOffset, &maxDrawCount, &stride, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer buffer = VkObject_fromId(bufferId);
    VkBuffer countBuffer = VkObject_fromId(countBufferId);

    vulkanWrapper.vkCmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

void vt_handle_vkCmdDrawIndexedIndirectCount(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t bufferId;
    VkDeviceSize offset;
    uint64_t countBufferId;
    VkDeviceSize countBufferOffset;
    uint32_t maxDrawCount;
    uint32_t stride;

    vt_unserialize_vkCmdDrawIndexedIndirectCount((VkCommandBuffer)&commandBufferId, (VkBuffer)&bufferId, &offset, (VkBuffer)&countBufferId, &countBufferOffset, &maxDrawCount, &stride, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer buffer = VkObject_fromId(bufferId);
    VkBuffer countBuffer = VkObject_fromId(countBufferId);

    vulkanWrapper.vkCmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

void vt_handle_vkCmdBindTransformFeedbackBuffersEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstBinding;
    uint32_t bindingCount;

    vt_unserialize_vkCmdBindTransformFeedbackBuffersEXT((VkCommandBuffer)&commandBufferId, &firstBinding, &bindingCount, VK_NULL_HANDLE, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkBuffer buffers[bindingCount];
    VkDeviceSize offsets[bindingCount];
    VkDeviceSize sizes[bindingCount];
    vt_unserialize_vkCmdBindTransformFeedbackBuffersEXT(VK_NULL_HANDLE, NULL, NULL, buffers, offsets, sizes, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdBindTransformFeedbackBuffers(commandBuffer, firstBinding, bindingCount, buffers, offsets, sizes);
}

void vt_handle_vkCmdBeginTransformFeedbackEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstCounterBuffer;
    uint32_t counterBufferCount;

    vt_unserialize_vkCmdBeginTransformFeedbackEXT((VkCommandBuffer)&commandBufferId, &firstCounterBuffer, &counterBufferCount, VK_NULL_HANDLE, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkBuffer counterBuffers[counterBufferCount];
    VkDeviceSize counterBufferOffsets[counterBufferCount];
    vt_unserialize_vkCmdBeginTransformFeedbackEXT(VK_NULL_HANDLE, NULL, NULL, counterBuffers, counterBufferOffsets, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdBeginTransformFeedback(commandBuffer, firstCounterBuffer, counterBufferCount, counterBuffers, counterBufferOffsets);
}

void vt_handle_vkCmdEndTransformFeedbackEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstCounterBuffer;
    uint32_t counterBufferCount;

    vt_unserialize_vkCmdEndTransformFeedbackEXT((VkCommandBuffer)&commandBufferId, &firstCounterBuffer, &counterBufferCount, VK_NULL_HANDLE, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkBuffer counterBuffers[counterBufferCount];
    VkDeviceSize counterBufferOffsets[counterBufferCount];
    vt_unserialize_vkCmdEndTransformFeedbackEXT(VK_NULL_HANDLE, NULL, NULL, counterBuffers, counterBufferOffsets, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdEndTransformFeedback(commandBuffer, firstCounterBuffer, counterBufferCount, counterBuffers, counterBufferOffsets);
}

void vt_handle_vkCmdBeginQueryIndexedEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t queryPoolId;
    uint32_t query;
    VkQueryControlFlags flags;
    uint32_t index;

    vt_unserialize_vkCmdBeginQueryIndexedEXT((VkCommandBuffer)&commandBufferId, (VkQueryPool)&queryPoolId, &query, &flags, &index, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);

    vulkanWrapper.vkCmdBeginQueryIndexed(commandBuffer, queryPool, query, flags, index);
}

void vt_handle_vkCmdEndQueryIndexedEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t queryPoolId;
    uint32_t query;
    uint32_t index;

    vt_unserialize_vkCmdEndQueryIndexedEXT((VkCommandBuffer)&commandBufferId, (VkQueryPool)&queryPoolId, &query, &index, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);

    vulkanWrapper.vkCmdEndQueryIndexed(commandBuffer, queryPool, query, index);
}

void vt_handle_vkCmdDrawIndirectByteCountEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t instanceCount;
    uint32_t firstInstance;
    uint64_t counterBufferId;
    VkDeviceSize counterBufferOffset;
    uint32_t counterOffset;
    uint32_t vertexStride;

    vt_unserialize_vkCmdDrawIndirectByteCountEXT((VkCommandBuffer)&commandBufferId, &instanceCount, &firstInstance, (VkBuffer)&counterBufferId, &counterBufferOffset, &counterOffset, &vertexStride, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkBuffer counterBuffer = VkObject_fromId(counterBufferId);

    vulkanWrapper.vkCmdDrawIndirectByteCount(commandBuffer, instanceCount, firstInstance, counterBuffer, counterBufferOffset, counterOffset, vertexStride);
}

void vt_handle_vkGetBufferOpaqueCaptureAddress(VkContext* context) {
    uint64_t deviceId;
    VkBufferDeviceAddressInfo info = {0};

    vt_unserialize_vkGetBufferOpaqueCaptureAddress((VkDevice)&deviceId, &info, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    uint64_t value = vulkanWrapper.vkGetBufferOpaqueCaptureAddress(device, &info);
    vt_send(context->clientRing, VK_SUCCESS, &value, sizeof(uint64_t));
}

void vt_handle_vkGetBufferDeviceAddress(VkContext* context) {
    uint64_t deviceId;
    VkBufferDeviceAddressInfo info = {0};

    vt_unserialize_vkGetBufferDeviceAddress((VkDevice)&deviceId, &info, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    VkDeviceAddress value = vulkanWrapper.vkGetBufferDeviceAddress(device, &info);
#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_onBufferDeviceAddress((void*)info.buffer, (uint64_t)value, 0);
#endif
    vt_send(context->clientRing, VK_SUCCESS, &value, sizeof(VkDeviceAddress));
}

void vt_handle_vkGetDeviceMemoryOpaqueCaptureAddress(VkContext* context) {
    uint64_t deviceId;
    VkDeviceMemoryOpaqueCaptureAddressInfo info = {0};

    vt_unserialize_vkGetDeviceMemoryOpaqueCaptureAddress((VkDevice)&deviceId, &info, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    uint64_t value = vulkanWrapper.vkGetDeviceMemoryOpaqueCaptureAddress(device, &info);
    vt_send(context->clientRing, VK_SUCCESS, &value, sizeof(uint64_t));
}

void vt_handle_vkCmdSetLineStippleKHR(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t lineStippleFactor;
    uint16_t lineStipplePattern;

    vt_unserialize_vkCmdSetLineStippleKHR((VkCommandBuffer)&commandBufferId, &lineStippleFactor, &lineStipplePattern, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetLineStipple(commandBuffer, lineStippleFactor, lineStipplePattern);
}

void vt_handle_vkCmdSetCullMode(VkContext* context) {
    uint64_t commandBufferId;
    VkCullModeFlags cullMode;

    vt_unserialize_vkCmdSetCullMode((VkCommandBuffer)&commandBufferId, &cullMode, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetCullMode(commandBuffer, cullMode);
}

void vt_handle_vkCmdSetFrontFace(VkContext* context) {
    uint64_t commandBufferId;
    VkFrontFace frontFace;

    vt_unserialize_vkCmdSetFrontFace((VkCommandBuffer)&commandBufferId, &frontFace, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetFrontFace(commandBuffer, frontFace);
}

void vt_handle_vkCmdSetPrimitiveTopology(VkContext* context) {
    uint64_t commandBufferId;
    VkPrimitiveTopology primitiveTopology;

    vt_unserialize_vkCmdSetPrimitiveTopology((VkCommandBuffer)&commandBufferId, &primitiveTopology, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetPrimitiveTopology(commandBuffer, primitiveTopology);
}

void vt_handle_vkCmdSetViewportWithCount(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t viewportCount;

    vt_unserialize_vkCmdSetViewportWithCount((VkCommandBuffer)&commandBufferId, &viewportCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkViewport viewports[viewportCount];
    vt_unserialize_vkCmdSetViewportWithCount(VK_NULL_HANDLE, NULL, viewports, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetViewportWithCount(commandBuffer, viewportCount, viewports);
}

void vt_handle_vkCmdSetScissorWithCount(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t scissorCount;

    vt_unserialize_vkCmdSetScissorWithCount((VkCommandBuffer)&commandBufferId, &scissorCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkRect2D scissors[scissorCount];
    vt_unserialize_vkCmdSetScissorWithCount(VK_NULL_HANDLE, NULL, scissors, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetScissorWithCount(commandBuffer, scissorCount, scissors);
}

void vt_handle_vkCmdBindVertexBuffers2(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstBinding;
    uint32_t bindingCount;

    vt_unserialize_vkCmdBindVertexBuffers2((VkCommandBuffer)&commandBufferId, &firstBinding, &bindingCount, VK_NULL_HANDLE, NULL, NULL, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkBuffer buffers[bindingCount];
    VkDeviceSize offsets[bindingCount];
    VkDeviceSize sizes[bindingCount];
    VkDeviceSize strides[bindingCount];
    vt_unserialize_vkCmdBindVertexBuffers2(VK_NULL_HANDLE, NULL, NULL, buffers, offsets, sizes, strides, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdBindVertexBuffers2(commandBuffer, firstBinding, bindingCount, buffers, offsets, sizes, strides);
}

void vt_handle_vkCmdSetDepthTestEnable(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 depthTestEnable;

    vt_unserialize_vkCmdSetDepthTestEnable((VkCommandBuffer)&commandBufferId, &depthTestEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDepthTestEnable(commandBuffer, depthTestEnable);
}

void vt_handle_vkCmdSetDepthWriteEnable(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 depthWriteEnable;

    vt_unserialize_vkCmdSetDepthWriteEnable((VkCommandBuffer)&commandBufferId, &depthWriteEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDepthWriteEnable(commandBuffer, depthWriteEnable);
}

void vt_handle_vkCmdSetDepthCompareOp(VkContext* context) {
    uint64_t commandBufferId;
    VkCompareOp depthCompareOp;

    vt_unserialize_vkCmdSetDepthCompareOp((VkCommandBuffer)&commandBufferId, &depthCompareOp, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDepthCompareOp(commandBuffer, depthCompareOp);
}

void vt_handle_vkCmdSetDepthBoundsTestEnable(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 depthBoundsTestEnable;

    vt_unserialize_vkCmdSetDepthBoundsTestEnable((VkCommandBuffer)&commandBufferId, &depthBoundsTestEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDepthBoundsTestEnable(commandBuffer, depthBoundsTestEnable);
}

void vt_handle_vkCmdSetStencilTestEnable(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 stencilTestEnable;

    vt_unserialize_vkCmdSetStencilTestEnable((VkCommandBuffer)&commandBufferId, &stencilTestEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetStencilTestEnable(commandBuffer, stencilTestEnable);
}

void vt_handle_vkCmdSetStencilOp(VkContext* context) {
    uint64_t commandBufferId;
    VkStencilFaceFlags faceMask;
    VkStencilOp failOp;
    VkStencilOp passOp;
    VkStencilOp depthFailOp;
    VkCompareOp compareOp;

    vt_unserialize_vkCmdSetStencilOp((VkCommandBuffer)&commandBufferId, &faceMask, &failOp, &passOp, &depthFailOp, &compareOp, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetStencilOp(commandBuffer, faceMask, failOp, passOp, depthFailOp, compareOp);
}

void vt_handle_vkCmdSetRasterizerDiscardEnable(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 rasterizerDiscardEnable;

    vt_unserialize_vkCmdSetRasterizerDiscardEnable((VkCommandBuffer)&commandBufferId, &rasterizerDiscardEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetRasterizerDiscardEnable(commandBuffer, rasterizerDiscardEnable);
}

void vt_handle_vkCmdSetDepthBiasEnable(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 depthBiasEnable;

    vt_unserialize_vkCmdSetDepthBiasEnable((VkCommandBuffer)&commandBufferId, &depthBiasEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDepthBiasEnable(commandBuffer, depthBiasEnable);
}

void vt_handle_vkCmdSetPrimitiveRestartEnable(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 primitiveRestartEnable;

    vt_unserialize_vkCmdSetPrimitiveRestartEnable((VkCommandBuffer)&commandBufferId, &primitiveRestartEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetPrimitiveRestartEnable(commandBuffer, primitiveRestartEnable);
}

void vt_handle_vkCmdSetTessellationDomainOriginEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkTessellationDomainOrigin domainOrigin;

    vt_unserialize_vkCmdSetTessellationDomainOriginEXT((VkCommandBuffer)&commandBufferId, &domainOrigin, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetTessellationDomainOrigin(commandBuffer, domainOrigin);
}

void vt_handle_vkCmdSetDepthClampEnableEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 depthClampEnable;

    vt_unserialize_vkCmdSetDepthClampEnableEXT((VkCommandBuffer)&commandBufferId, &depthClampEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDepthClampEnable(commandBuffer, depthClampEnable);
}

void vt_handle_vkCmdSetPolygonModeEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkPolygonMode polygonMode;

    vt_unserialize_vkCmdSetPolygonModeEXT((VkCommandBuffer)&commandBufferId, &polygonMode, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetPolygonMode(commandBuffer, polygonMode);
}

void vt_handle_vkCmdSetRasterizationSamplesEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkSampleCountFlagBits rasterizationSamples;

    vt_unserialize_vkCmdSetRasterizationSamplesEXT((VkCommandBuffer)&commandBufferId, &rasterizationSamples, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetRasterizationSamples(commandBuffer, rasterizationSamples);
}

void vt_handle_vkCmdSetSampleMaskEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkSampleCountFlagBits samples;

    vt_unserialize_vkCmdSetSampleMaskEXT((VkCommandBuffer)&commandBufferId, &samples, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkSampleMask sampleMask[samples];
    vt_unserialize_vkCmdSetSampleMaskEXT(VK_NULL_HANDLE, NULL, sampleMask, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetSampleMask(commandBuffer, samples, sampleMask);
}

void vt_handle_vkCmdSetAlphaToCoverageEnableEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 alphaToCoverageEnable;

    vt_unserialize_vkCmdSetAlphaToCoverageEnableEXT((VkCommandBuffer)&commandBufferId, &alphaToCoverageEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetAlphaToCoverageEnable(commandBuffer, alphaToCoverageEnable);
}

void vt_handle_vkCmdSetAlphaToOneEnableEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 alphaToOneEnable;

    vt_unserialize_vkCmdSetAlphaToOneEnableEXT((VkCommandBuffer)&commandBufferId, &alphaToOneEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetAlphaToOneEnable(commandBuffer, alphaToOneEnable);
}

void vt_handle_vkCmdSetLogicOpEnableEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 logicOpEnable;

    vt_unserialize_vkCmdSetLogicOpEnableEXT((VkCommandBuffer)&commandBufferId, &logicOpEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetLogicOpEnable(commandBuffer, logicOpEnable);
}

void vt_handle_vkCmdSetColorBlendEnableEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstAttachment;
    uint32_t attachmentCount;

    vt_unserialize_vkCmdSetColorBlendEnableEXT((VkCommandBuffer)&commandBufferId, &firstAttachment, &attachmentCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkBool32 colorBlendEnables[attachmentCount];
    vt_unserialize_vkCmdSetColorBlendEnableEXT(VK_NULL_HANDLE, NULL, NULL, colorBlendEnables, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetColorBlendEnable(commandBuffer, firstAttachment, attachmentCount, colorBlendEnables);
}

void vt_handle_vkCmdSetColorBlendEquationEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstAttachment;
    uint32_t attachmentCount;

    vt_unserialize_vkCmdSetColorBlendEquationEXT((VkCommandBuffer)&commandBufferId, &firstAttachment, &attachmentCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkColorBlendEquationEXT colorBlendEquations[attachmentCount];
    vt_unserialize_vkCmdSetColorBlendEquationEXT(VK_NULL_HANDLE, NULL, NULL, colorBlendEquations, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetColorBlendEquation(commandBuffer, firstAttachment, attachmentCount, colorBlendEquations);
}

void vt_handle_vkCmdSetColorWriteMaskEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstAttachment;
    uint32_t attachmentCount;

    vt_unserialize_vkCmdSetColorWriteMaskEXT((VkCommandBuffer)&commandBufferId, &firstAttachment, &attachmentCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkColorComponentFlags colorWriteMasks[attachmentCount];
    vt_unserialize_vkCmdSetColorWriteMaskEXT(VK_NULL_HANDLE, NULL, NULL, colorWriteMasks, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetColorWriteMask(commandBuffer, firstAttachment, attachmentCount, colorWriteMasks);
}

void vt_handle_vkCmdSetRasterizationStreamEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t rasterizationStream;

    vt_unserialize_vkCmdSetRasterizationStreamEXT((VkCommandBuffer)&commandBufferId, &rasterizationStream, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetRasterizationStream(commandBuffer, rasterizationStream);
}

void vt_handle_vkCmdSetConservativeRasterizationModeEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkConservativeRasterizationModeEXT conservativeRasterizationMode;

    vt_unserialize_vkCmdSetConservativeRasterizationModeEXT((VkCommandBuffer)&commandBufferId, &conservativeRasterizationMode, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetConservativeRasterizationMode(commandBuffer, conservativeRasterizationMode);
}

void vt_handle_vkCmdSetExtraPrimitiveOverestimationSizeEXT(VkContext* context) {
    uint64_t commandBufferId;
    float extraPrimitiveOverestimationSize;

    vt_unserialize_vkCmdSetExtraPrimitiveOverestimationSizeEXT((VkCommandBuffer)&commandBufferId, &extraPrimitiveOverestimationSize, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetExtraPrimitiveOverestimationSize(commandBuffer, extraPrimitiveOverestimationSize);
}

void vt_handle_vkCmdSetDepthClipEnableEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 depthClipEnable;

    vt_unserialize_vkCmdSetDepthClipEnableEXT((VkCommandBuffer)&commandBufferId, &depthClipEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDepthClipEnable(commandBuffer, depthClipEnable);
}

void vt_handle_vkCmdSetSampleLocationsEnableEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 sampleLocationsEnable;

    vt_unserialize_vkCmdSetSampleLocationsEnableEXT((VkCommandBuffer)&commandBufferId, &sampleLocationsEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetSampleLocationsEnable(commandBuffer, sampleLocationsEnable);
}

void vt_handle_vkCmdSetColorBlendAdvancedEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t firstAttachment;
    uint32_t attachmentCount;

    vt_unserialize_vkCmdSetColorBlendAdvancedEXT((VkCommandBuffer)&commandBufferId, &firstAttachment, &attachmentCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkColorBlendAdvancedEXT colorBlendAdvanced[attachmentCount];
    vt_unserialize_vkCmdSetColorBlendAdvancedEXT(VK_NULL_HANDLE, NULL, NULL, colorBlendAdvanced, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetColorBlendAdvanced(commandBuffer, firstAttachment, attachmentCount, colorBlendAdvanced);
}

void vt_handle_vkCmdSetProvokingVertexModeEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkProvokingVertexModeEXT provokingVertexMode;

    vt_unserialize_vkCmdSetProvokingVertexModeEXT((VkCommandBuffer)&commandBufferId, &provokingVertexMode, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetProvokingVertexMode(commandBuffer, provokingVertexMode);
}

void vt_handle_vkCmdSetLineRasterizationModeEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkLineRasterizationModeEXT lineRasterizationMode;

    vt_unserialize_vkCmdSetLineRasterizationModeEXT((VkCommandBuffer)&commandBufferId, &lineRasterizationMode, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetLineRasterizationMode(commandBuffer, lineRasterizationMode);
}

void vt_handle_vkCmdSetLineStippleEnableEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 stippledLineEnable;

    vt_unserialize_vkCmdSetLineStippleEnableEXT((VkCommandBuffer)&commandBufferId, &stippledLineEnable, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetLineStippleEnable(commandBuffer, stippledLineEnable);
}

void vt_handle_vkCmdSetDepthClipNegativeOneToOneEXT(VkContext* context) {
    uint64_t commandBufferId;
    VkBool32 negativeOneToOne;

    vt_unserialize_vkCmdSetDepthClipNegativeOneToOneEXT((VkCommandBuffer)&commandBufferId, &negativeOneToOne, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdSetDepthClipNegativeOneToOne(commandBuffer, negativeOneToOne);
}

void vt_handle_vkCmdCopyBuffer2(VkContext* context) {
    uint64_t commandBufferId;
    VkCopyBufferInfo2 copyBufferInfo = {0};

    vt_unserialize_vkCmdCopyBuffer2((VkCommandBuffer)&commandBufferId, &copyBufferInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdCopyBuffer2(commandBuffer, &copyBufferInfo);
}

void vt_handle_vkCmdCopyImage2(VkContext* context) {
    uint64_t commandBufferId;
    VkCopyImageInfo2 copyImageInfo = {0};

    vt_unserialize_vkCmdCopyImage2((VkCommandBuffer)&commandBufferId, &copyImageInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdCopyImage2(commandBuffer, &copyImageInfo);
}

void vt_handle_vkCmdBlitImage2(VkContext* context) {
    uint64_t commandBufferId;
    VkBlitImageInfo2 blitImageInfo = {0};

    vt_unserialize_vkCmdBlitImage2((VkCommandBuffer)&commandBufferId, &blitImageInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdBlitImage2(commandBuffer, &blitImageInfo);
}

void vt_handle_vkCmdCopyBufferToImage2(VkContext* context) {
    uint64_t commandBufferId;
    VkCopyBufferToImageInfo2 copyBufferToImageInfo = {0};

    vt_unserialize_vkCmdCopyBufferToImage2((VkCommandBuffer)&commandBufferId, &copyBufferToImageInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    if (context->textureDecoder && TextureDecoder_containsImage(context->textureDecoder, copyBufferToImageInfo.dstImage)) {
        if (copyBufferToImageInfo.pRegions[0].imageSubresource.mipLevel > 0) return;
        TextureDecoder_copyBufferToImage(context->textureDecoder, commandBuffer, copyBufferToImageInfo.srcBuffer, copyBufferToImageInfo.dstImage, copyBufferToImageInfo.dstImageLayout, copyBufferToImageInfo.pRegions[0].bufferOffset);
    }
    else vulkanWrapper.vkCmdCopyBufferToImage2(commandBuffer, &copyBufferToImageInfo);
}

void vt_handle_vkCmdCopyImageToBuffer2(VkContext* context) {
    uint64_t commandBufferId;
    VkCopyImageToBufferInfo2 copyImageToBufferInfo = {0};

    vt_unserialize_vkCmdCopyImageToBuffer2((VkCommandBuffer)&commandBufferId, &copyImageToBufferInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdCopyImageToBuffer2(commandBuffer, &copyImageToBufferInfo);
}

void vt_handle_vkCmdResolveImage2(VkContext* context) {
    uint64_t commandBufferId;
    VkResolveImageInfo2 resolveImageInfo = {0};

    vt_unserialize_vkCmdResolveImage2((VkCommandBuffer)&commandBufferId, &resolveImageInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdResolveImage2(commandBuffer, &resolveImageInfo);
}

void vt_handle_vkCmdSetColorWriteEnableEXT(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t attachmentCount;

    vt_unserialize_vkCmdSetColorWriteEnableEXT((VkCommandBuffer)&commandBufferId, &attachmentCount, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkBool32 colorWriteEnables[attachmentCount];
    vt_unserialize_vkCmdSetColorWriteEnableEXT(VK_NULL_HANDLE, NULL, colorWriteEnables, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdSetColorWriteEnable(commandBuffer, attachmentCount, colorWriteEnables);
}

void vt_handle_vkCmdSetEvent2(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t eventId;
    VkDependencyInfo dependencyInfo = {0};

    vt_unserialize_vkCmdSetEvent2((VkCommandBuffer)&commandBufferId, (VkEvent)&eventId, &dependencyInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkEvent event = VkObject_fromId(eventId);

    vulkanWrapper.vkCmdSetEvent2(commandBuffer, event, &dependencyInfo);
}

void vt_handle_vkCmdResetEvent2(VkContext* context) {
    uint64_t commandBufferId;
    uint64_t eventId;
    VkPipelineStageFlags2 stageMask;

    vt_unserialize_vkCmdResetEvent2((VkCommandBuffer)&commandBufferId, (VkEvent)&eventId, &stageMask, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkEvent event = VkObject_fromId(eventId);

    vulkanWrapper.vkCmdResetEvent2(commandBuffer, event, stageMask);
}

void vt_handle_vkCmdWaitEvents2(VkContext* context) {
    uint64_t commandBufferId;
    uint32_t eventCount;

    vt_unserialize_vkCmdWaitEvents2((VkCommandBuffer)&commandBufferId, &eventCount, VK_NULL_HANDLE, NULL, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    VkEvent events[eventCount];
    VkDependencyInfo dependencyInfos[eventCount];
    vt_unserialize_vkCmdWaitEvents2(VK_NULL_HANDLE, NULL, events, dependencyInfos, context->inputBuffer, &context->memoryPool);

    vulkanWrapper.vkCmdWaitEvents2(commandBuffer, eventCount, events, dependencyInfos);
}

void vt_handle_vkCmdPipelineBarrier2(VkContext* context) {
    uint64_t commandBufferId;
    VkDependencyInfo dependencyInfo = {0};

    vt_unserialize_vkCmdPipelineBarrier2((VkCommandBuffer)&commandBufferId, &dependencyInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
}

void vt_handle_vkQueueSubmit2(VkContext* context) {
    uint64_t queueId;
    uint32_t submitCount;
    uint64_t fenceId;

    vt_unserialize_vkQueueSubmit2((VkQueue)&queueId, &submitCount, NULL, (VkFence)&fenceId, context->inputBuffer, &context->memoryPool);
    VkQueue queue = VkObject_fromId(queueId);
    VkFence fence = VkObject_fromId(fenceId);

    bool clientWaiting = RingBuffer_hasStatus(context->clientRing, RING_STATUS_WAIT);
    if (context->textureDecoder) TextureDecoder_decodeAll(context->textureDecoder);

    VkSubmitInfo2 submits[submitCount];
    vt_unserialize_vkQueueSubmit2(VK_NULL_HANDLE, NULL, submits, VK_NULL_HANDLE, context->inputBuffer, &context->memoryPool);

#ifdef BACHATA_VORTEK_SERVER
    VortekGpuTrack_noteQueue(queue);
    uint64_t submission = VortekGpuTrack_beginSubmission();
    for (uint32_t si = 0; si < submitCount; si++) {
        for (uint32_t ci = 0; ci < submits[si].commandBufferInfoCount; ci++) {
            VortekGpuTrack_noteSubmitCommandBuffer(submits[si].pCommandBufferInfos[ci].commandBuffer);
        }
    }
    VkFence submitFence = fence;
    VkFence internalFence = VK_NULL_HANDLE;
    if (fence == VK_NULL_HANDLE && g_bachata_track_device) {
        internalFence = bachata_acquire_internal_fence(g_bachata_track_device);
        if (internalFence != VK_NULL_HANDLE) {
            submitFence = internalFence;
        }
    }
#endif
    VkResult result = vulkanWrapper.vkQueueSubmit2(queue, submitCount, submits,
#ifdef BACHATA_VORTEK_SERVER
                                                   submitFence
#else
                                                   fence
#endif
    );
    if (result == VK_ERROR_DEVICE_LOST) context->status = result;
#ifdef BACHATA_VORTEK_SERVER
    if (internalFence != VK_NULL_HANDLE && result == VK_SUCCESS) {
        VortekGpuTrack_bindSubmissionCompletion(submission, (void*)internalFence,
                                                VORTEK_COMPLETION_INTERNAL_FENCE);
    } else if (fence != VK_NULL_HANDLE) {
        VortekGpuTrack_bindSubmissionCompletion(submission, (void*)fence,
                                                VORTEK_COMPLETION_CALLER_FENCE);
    } else {
        VortekGpuTrack_bindSubmissionFence(submission, fence);
        if (internalFence != VK_NULL_HANDLE) {
            bachata_release_internal_fence(internalFence);
        }
    }
    VortekGpuTrack_endSubmission(submission, (int)result, result == VK_ERROR_DEVICE_LOST);
    bachata_drain_deferred_destroys();
#endif

    if (clientWaiting) vt_send(context->clientRing, result, NULL, 0);
}

void vt_handle_vkCmdWriteTimestamp2(VkContext* context) {
    uint64_t commandBufferId;
    VkPipelineStageFlags2 stage;
    uint64_t queryPoolId;
    uint32_t query;

    vt_unserialize_vkCmdWriteTimestamp2((VkCommandBuffer)&commandBufferId, &stage, (VkQueryPool)&queryPoolId, &query, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);
    VkQueryPool queryPool = VkObject_fromId(queryPoolId);

    vulkanWrapper.vkCmdWriteTimestamp2(commandBuffer, stage, queryPool, query);
}

void vt_handle_vkCmdBeginRendering(VkContext* context) {
    uint64_t commandBufferId;
    VkRenderingInfo renderingInfo = {0};

    vt_unserialize_vkCmdBeginRendering((VkCommandBuffer)&commandBufferId, &renderingInfo, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdBeginRendering(commandBuffer, &renderingInfo);
}

void vt_handle_vkCmdEndRendering(VkContext* context) {
    uint64_t commandBufferId;

    vt_unserialize_VkCommandBuffer((VkCommandBuffer)&commandBufferId, context->inputBuffer, &context->memoryPool);
    VkCommandBuffer commandBuffer = VkObject_fromId(commandBufferId);

    vulkanWrapper.vkCmdEndRendering(commandBuffer);
}

void vt_handle_vkGetShaderModuleIdentifierEXT(VkContext* context) {
    uint64_t deviceId;
    uint64_t shaderModuleId;
    VkShaderModuleIdentifierEXT identifier = {0};

    vt_unserialize_vkGetShaderModuleIdentifierEXT((VkDevice)&deviceId, (VkShaderModule)&shaderModuleId, &identifier, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);
    ShaderModule* shaderModule = VkObject_fromId(shaderModuleId);

    vulkanWrapper.vkGetShaderModuleIdentifier(device, shaderModule->module, &identifier);

    VT_SERIALIZE_CMD(VkShaderModuleIdentifierEXT, &identifier);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

void vt_handle_vkGetShaderModuleCreateInfoIdentifierEXT(VkContext* context) {
    uint64_t deviceId;
    VkShaderModuleCreateInfo createInfo = {0};
    VkShaderModuleIdentifierEXT identifier = {0};

    vt_unserialize_vkGetShaderModuleCreateInfoIdentifierEXT((VkDevice)&deviceId, &createInfo, &identifier, context->inputBuffer, &context->memoryPool);
    VkDevice device = VkObject_fromId(deviceId);

    vulkanWrapper.vkGetShaderModuleCreateInfoIdentifier(device, &createInfo, &identifier);

    VT_SERIALIZE_CMD(VkShaderModuleIdentifierEXT, &identifier);
    vt_send(context->clientRing, VK_SUCCESS, outputBuffer, bufferSize);
}

HandleRequestFunc handleRequestFuncs[] = {
    vt_handle_vkCreateInstance,
    vt_handle_vkDestroyInstance,
    vt_handle_vkEnumeratePhysicalDevices,
    vt_handle_vkGetPhysicalDeviceProperties,
    vt_handle_vkGetPhysicalDeviceQueueFamilyProperties,
    vt_handle_vkGetPhysicalDeviceMemoryProperties,
    vt_handle_vkGetPhysicalDeviceFeatures,
    vt_handle_vkGetPhysicalDeviceFormatProperties,
    vt_handle_vkGetPhysicalDeviceImageFormatProperties,
    vt_handle_vkCreateDevice,
    vt_handle_vkDestroyDevice,
    vt_handle_vkEnumerateInstanceVersion,
    vt_handle_vkEnumerateInstanceExtensionProperties,
    vt_handle_vkEnumerateDeviceExtensionProperties,
    vt_handle_vkGetDeviceQueue,
    vt_handle_vkQueueSubmit,
    vt_handle_vkQueueWaitIdle,
    vt_handle_vkDeviceWaitIdle,
    vt_handle_vkAllocateMemory,
    vt_handle_vkFreeMemory,
    vt_handle_vkMapMemory,
    vt_handle_vkUnmapMemory,
    vt_handle_vkFlushMappedMemoryRanges,
    vt_handle_vkInvalidateMappedMemoryRanges,
    vt_handle_vkGetDeviceMemoryCommitment,
    vt_handle_vkGetBufferMemoryRequirements,
    vt_handle_vkBindBufferMemory,
    vt_handle_vkGetImageMemoryRequirements,
    vt_handle_vkBindImageMemory,
    vt_handle_vkGetImageSparseMemoryRequirements,
    vt_handle_vkGetPhysicalDeviceSparseImageFormatProperties,
    vt_handle_vkQueueBindSparse,
    vt_handle_vkCreateFence,
    vt_handle_vkDestroyFence,
    vt_handle_vkResetFences,
    vt_handle_vkGetFenceStatus,
    vt_handle_vkWaitForFences,
    vt_handle_vkCreateSemaphore,
    vt_handle_vkDestroySemaphore,
    vt_handle_vkCreateEvent,
    vt_handle_vkDestroyEvent,
    vt_handle_vkGetEventStatus,
    vt_handle_vkSetEvent,
    vt_handle_vkResetEvent,
    vt_handle_vkCreateQueryPool,
    vt_handle_vkDestroyQueryPool,
    vt_handle_vkGetQueryPoolResults,
    vt_handle_vkResetQueryPool,
    vt_handle_vkCreateBuffer,
    vt_handle_vkDestroyBuffer,
    vt_handle_vkCreateBufferView,
    vt_handle_vkDestroyBufferView,
    vt_handle_vkCreateImage,
    vt_handle_vkDestroyImage,
    vt_handle_vkGetImageSubresourceLayout,
    vt_handle_vkCreateImageView,
    vt_handle_vkDestroyImageView,
    vt_handle_vkCreateShaderModule,
    vt_handle_vkDestroyShaderModule,
    vt_handle_vkCreatePipelineCache,
    vt_handle_vkDestroyPipelineCache,
    vt_handle_vkGetPipelineCacheData,
    vt_handle_vkMergePipelineCaches,
    vt_handle_vkCreateGraphicsPipelines,
    vt_handle_vkCreateComputePipelines,
    vt_handle_vkDestroyPipeline,
    vt_handle_vkCreatePipelineLayout,
    vt_handle_vkDestroyPipelineLayout,
    vt_handle_vkCreateSampler,
    vt_handle_vkDestroySampler,
    vt_handle_vkCreateDescriptorSetLayout,
    vt_handle_vkDestroyDescriptorSetLayout,
    vt_handle_vkCreateDescriptorPool,
    vt_handle_vkDestroyDescriptorPool,
    vt_handle_vkResetDescriptorPool,
    vt_handle_vkAllocateDescriptorSets,
    vt_handle_vkFreeDescriptorSets,
    vt_handle_vkUpdateDescriptorSets,
    vt_handle_vkCreateFramebuffer,
    vt_handle_vkDestroyFramebuffer,
    vt_handle_vkCreateRenderPass,
    vt_handle_vkDestroyRenderPass,
    vt_handle_vkGetRenderAreaGranularity,
    vt_handle_vkCreateCommandPool,
    vt_handle_vkDestroyCommandPool,
    vt_handle_vkResetCommandPool,
    vt_handle_vkAllocateCommandBuffers,
    vt_handle_vkFreeCommandBuffers,
    vt_handle_vkBeginCommandBuffer,
    vt_handle_vkEndCommandBuffer,
    vt_handle_vkResetCommandBuffer,
    vt_handle_vkCmdBindPipeline,
    vt_handle_vkCmdSetViewport,
    vt_handle_vkCmdSetScissor,
    vt_handle_vkCmdSetLineWidth,
    vt_handle_vkCmdSetDepthBias,
    vt_handle_vkCmdSetBlendConstants,
    vt_handle_vkCmdSetDepthBounds,
    vt_handle_vkCmdSetStencilCompareMask,
    vt_handle_vkCmdSetStencilWriteMask,
    vt_handle_vkCmdSetStencilReference,
    vt_handle_vkCmdBindDescriptorSets,
    vt_handle_vkCmdBindIndexBuffer,
    vt_handle_vkCmdBindVertexBuffers,
    vt_handle_vkCmdDraw,
    vt_handle_vkCmdDrawIndexed,
    vt_handle_vkCmdDrawIndirect,
    vt_handle_vkCmdDrawIndexedIndirect,
    vt_handle_vkCmdDispatch,
    vt_handle_vkCmdDispatchIndirect,
    vt_handle_vkCmdCopyBuffer,
    vt_handle_vkCmdCopyImage,
    vt_handle_vkCmdBlitImage,
    vt_handle_vkCmdCopyBufferToImage,
    vt_handle_vkCmdCopyImageToBuffer,
    vt_handle_vkCmdUpdateBuffer,
    vt_handle_vkCmdFillBuffer,
    vt_handle_vkCmdClearColorImage,
    vt_handle_vkCmdClearDepthStencilImage,
    vt_handle_vkCmdClearAttachments,
    vt_handle_vkCmdResolveImage,
    vt_handle_vkCmdSetEvent,
    vt_handle_vkCmdResetEvent,
    vt_handle_vkCmdWaitEvents,
    vt_handle_vkCmdPipelineBarrier,
    vt_handle_vkCmdBeginQuery,
    vt_handle_vkCmdEndQuery,
    vt_handle_vkCmdBeginConditionalRenderingEXT,
    vt_handle_vkCmdEndConditionalRenderingEXT,
    vt_handle_vkCmdResetQueryPool,
    vt_handle_vkCmdWriteTimestamp,
    vt_handle_vkCmdCopyQueryPoolResults,
    vt_handle_vkCmdPushConstants,
    vt_handle_vkCmdBeginRenderPass,
    vt_handle_vkCmdNextSubpass,
    vt_handle_vkCmdEndRenderPass,
    vt_handle_vkCmdExecuteCommands,
    vt_handle_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
    vt_handle_vkGetPhysicalDeviceSurfaceFormatsKHR,
    vt_handle_vkGetPhysicalDeviceSurfacePresentModesKHR,
    vt_handle_vkCreateSwapchainKHR,
    vt_handle_vkDestroySwapchainKHR,
    vt_handle_vkGetSwapchainImagesKHR,
    vt_handle_vkAcquireNextImageKHR,
    vt_handle_vkQueuePresentKHR,
    vt_handle_vkGetPhysicalDeviceFeatures2,
    vt_handle_vkGetPhysicalDeviceProperties2,
    vt_handle_vkGetPhysicalDeviceFormatProperties2,
    vt_handle_vkGetPhysicalDeviceImageFormatProperties2,
    vt_handle_vkGetPhysicalDeviceQueueFamilyProperties2,
    vt_handle_vkGetPhysicalDeviceMemoryProperties2,
    vt_handle_vkGetPhysicalDeviceSparseImageFormatProperties2,
    vt_handle_vkCmdPushDescriptorSetKHR,
    vt_handle_vkTrimCommandPool,
    vt_handle_vkGetPhysicalDeviceExternalBufferProperties,
    vt_handle_vkGetMemoryFdKHR,
    vt_handle_vkGetPhysicalDeviceExternalSemaphoreProperties,
    vt_handle_vkGetSemaphoreFdKHR,
    vt_handle_vkGetPhysicalDeviceExternalFenceProperties,
    vt_handle_vkGetFenceFdKHR,
    vt_handle_vkEnumeratePhysicalDeviceGroups,
    vt_handle_vkGetDeviceGroupPeerMemoryFeatures,
    vt_handle_vkBindBufferMemory2,
    vt_handle_vkBindImageMemory2,
    vt_handle_vkCmdSetDeviceMask,
    vt_handle_vkAcquireNextImage2KHR,
    vt_handle_vkCmdDispatchBase,
    vt_handle_vkGetPhysicalDevicePresentRectanglesKHR,
    vt_handle_vkCmdSetSampleLocationsEXT,
    vt_handle_vkGetPhysicalDeviceMultisamplePropertiesEXT,
    vt_handle_vkGetBufferMemoryRequirements2,
    vt_handle_vkGetImageMemoryRequirements2,
    vt_handle_vkGetImageSparseMemoryRequirements2,
    vt_handle_vkGetDeviceBufferMemoryRequirements,
    vt_handle_vkGetDeviceImageMemoryRequirements,
    vt_handle_vkGetDeviceImageSparseMemoryRequirements,
    vt_handle_vkCreateSamplerYcbcrConversion,
    vt_handle_vkDestroySamplerYcbcrConversion,
    vt_handle_vkGetDeviceQueue2,
    vt_handle_vkGetDescriptorSetLayoutSupport,
    vt_handle_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR,
    vt_handle_vkGetCalibratedTimestampsKHR,
    vt_handle_vkCreateRenderPass2,
    vt_handle_vkCmdBeginRenderPass2,
    vt_handle_vkCmdNextSubpass2,
    vt_handle_vkCmdEndRenderPass2,
    vt_handle_vkGetSemaphoreCounterValue,
    vt_handle_vkWaitSemaphores,
    vt_handle_vkSignalSemaphore,
    vt_handle_vkCmdDrawIndirectCount,
    vt_handle_vkCmdDrawIndexedIndirectCount,
    vt_handle_vkCmdBindTransformFeedbackBuffersEXT,
    vt_handle_vkCmdBeginTransformFeedbackEXT,
    vt_handle_vkCmdEndTransformFeedbackEXT,
    vt_handle_vkCmdBeginQueryIndexedEXT,
    vt_handle_vkCmdEndQueryIndexedEXT,
    vt_handle_vkCmdDrawIndirectByteCountEXT,
    vt_handle_vkGetBufferOpaqueCaptureAddress,
    vt_handle_vkGetBufferDeviceAddress,
    vt_handle_vkGetDeviceMemoryOpaqueCaptureAddress,
    vt_handle_vkCmdSetLineStippleKHR,
    vt_handle_vkCmdSetCullMode,
    vt_handle_vkCmdSetFrontFace,
    vt_handle_vkCmdSetPrimitiveTopology,
    vt_handle_vkCmdSetViewportWithCount,
    vt_handle_vkCmdSetScissorWithCount,
    vt_handle_vkCmdBindVertexBuffers2,
    vt_handle_vkCmdSetDepthTestEnable,
    vt_handle_vkCmdSetDepthWriteEnable,
    vt_handle_vkCmdSetDepthCompareOp,
    vt_handle_vkCmdSetDepthBoundsTestEnable,
    vt_handle_vkCmdSetStencilTestEnable,
    vt_handle_vkCmdSetStencilOp,
    vt_handle_vkCmdSetRasterizerDiscardEnable,
    vt_handle_vkCmdSetDepthBiasEnable,
    vt_handle_vkCmdSetPrimitiveRestartEnable,
    vt_handle_vkCmdSetTessellationDomainOriginEXT,
    vt_handle_vkCmdSetDepthClampEnableEXT,
    vt_handle_vkCmdSetPolygonModeEXT,
    vt_handle_vkCmdSetRasterizationSamplesEXT,
    vt_handle_vkCmdSetSampleMaskEXT,
    vt_handle_vkCmdSetAlphaToCoverageEnableEXT,
    vt_handle_vkCmdSetAlphaToOneEnableEXT,
    vt_handle_vkCmdSetLogicOpEnableEXT,
    vt_handle_vkCmdSetColorBlendEnableEXT,
    vt_handle_vkCmdSetColorBlendEquationEXT,
    vt_handle_vkCmdSetColorWriteMaskEXT,
    vt_handle_vkCmdSetRasterizationStreamEXT,
    vt_handle_vkCmdSetConservativeRasterizationModeEXT,
    vt_handle_vkCmdSetExtraPrimitiveOverestimationSizeEXT,
    vt_handle_vkCmdSetDepthClipEnableEXT,
    vt_handle_vkCmdSetSampleLocationsEnableEXT,
    vt_handle_vkCmdSetColorBlendAdvancedEXT,
    vt_handle_vkCmdSetProvokingVertexModeEXT,
    vt_handle_vkCmdSetLineRasterizationModeEXT,
    vt_handle_vkCmdSetLineStippleEnableEXT,
    vt_handle_vkCmdSetDepthClipNegativeOneToOneEXT,
    vt_handle_vkCmdCopyBuffer2,
    vt_handle_vkCmdCopyImage2,
    vt_handle_vkCmdBlitImage2,
    vt_handle_vkCmdCopyBufferToImage2,
    vt_handle_vkCmdCopyImageToBuffer2,
    vt_handle_vkCmdResolveImage2,
    vt_handle_vkCmdSetColorWriteEnableEXT,
    vt_handle_vkCmdSetEvent2,
    vt_handle_vkCmdResetEvent2,
    vt_handle_vkCmdWaitEvents2,
    vt_handle_vkCmdPipelineBarrier2,
    vt_handle_vkQueueSubmit2,
    vt_handle_vkCmdWriteTimestamp2,
    vt_handle_vkCmdBeginRendering,
    vt_handle_vkCmdEndRendering,
    vt_handle_vkGetShaderModuleIdentifierEXT,
    vt_handle_vkGetShaderModuleCreateInfoIdentifierEXT,
};

HandleRequestFunc getHandleRequestFunc(short requestCode) {
    int index = requestCode - REQUEST_CODE_VK_CALL_START;
    return index >= 0 && index < REQUEST_CODE_VK_CALL_COUNT ? handleRequestFuncs[index] : NULL;
}
