/* Bachata Vortek GPU-VA / resource lifetime instrumentation.
 * Machine-searchable tags:
 *   VORTEK_ALLOC, VORTEK_BIND_BUFFER, VORTEK_BIND_IMAGE, VORTEK_FREE
 *   GPU_ACCESS, GPU_RANGE_INVALID, RESOURCE_FREED_IN_FLIGHT
 *   RESOURCE_DESTROY_DEFERRED, RESOURCE_DESTROY_COMMITTED
 *   RETIRE_REQUEST, RETIRE_COMMIT, RETIRE_BLOCKED_NO_COMPLETION_PROOF
 *   ALLOCATION_RETIRE_BLOCKED, ALLOCATION_STATE
 *   BACKING_RETIRE_REQUEST, BACKING_RETIRE_BLOCKED, BACKING_RETIRE_COMMIT
 *   BACKING_RELEASE_IN_FLIGHT, MAPPING_UNMAP_DEFERRED
 *   EXTERNAL_HANDLE_RELEASE_DEFERRED, HANDLE_SLOT_REUSE_BLOCKED
 *   INTERNAL_COMPLETION_SUBMIT, INTERNAL_COMPLETION_OBSERVED
 *   DEVICE_WAIT_IDLE_COMPLETION_ADVANCE
 *   COPY_BUFFER_TO_IMAGE, IMAGE_BARRIER, DEVICE_LOST_SNAPSHOT, PENDING_RESOURCE
 *   PRESENT_WAIT_BEGIN, PRESENT_SEMAPHORE_REUSED, PRESENT_SEMAPHORE_DESTROYED_IN_FLIGHT
 *   PRESENT_SEMAPHORE_REUSE_CHECK, PRESENT_SEMAPHORE_REUSED_IN_FLIGHT
 *   PRESENT_SEMAPHORE_REUSE_BLOCKED
 *   PRESENT_IMAGE_REUSED_BEFORE_REACQUIRE, PRESENT_SYNC_DESTROYED_IN_FLIGHT
 *   PRESENT_BEGIN, PRESENT_ACCEPTED, PRESENT_COMPLETED
 *   PRESENTER_CONFIG, ACQUIRE_RESULT, SWAPCHAIN_IMAGE_BACKING, SHARED_AHB
 *   PRESENT_ON_BUSY_AHB, AHB_REUSED_BEFORE_RELEASE, AHB_RELEASE_FENCE_NOT_SIGNALED
 *   SYNC_FD_IMPORTED_TWICE, SYNC_FD_CLOSED_EARLY, SYNC_FD_WAIT_AFTER_TRANSFER, SYNC_FD_REUSED
 *   PHYSICAL_RELEASE_REQUEST, PHYSICAL_RELEASE_BLOCKED_NO_COMPLETION
 *   PHYSICAL_RELEASE_BLOCKED_DEPENDENCY, PHYSICAL_RELEASE_COMMIT
 *   PHYSICAL_RELEASE_WITHOUT_COMPLETION
 *   BUFFER_RETIRE_COMMIT, BUFFER_RETIRE_BLOCKED_IN_FLIGHT
 *   BUFFER_RETIRE_BLOCKED_UNKNOWN_USE, BUFFER_RETIRE_BLOCKED_BDA
 *   BUFFER_RETIRE_BLOCKED_CHILD_DEPENDENCY, BUFFER_RETIRE_BLOCKED_HANDLE_REUSE
 *   IMAGE_RETIRE_COMMIT, IMAGE_RETIRE_BLOCKED_UNKNOWN_USE
 *   MEMORY_RETIRE_COMMIT, MEMORY_RETIRE_BLOCKED_UNKNOWN_USE
 *   multiClassRetirement=1 antiThrashCollect=1
 *   DIRECT_VK_DESTROY_BUFFER_BYPASS
 *   BDA_USE_RESOLVED, BDA_USE_UNRESOLVED, BDA_ALLOCATION_RETAINED
 *   UNKNOWN_BUFFER_RETENTION_SUMMARY
 *   QUARANTINE_CLASS buffers|images|memory
 *   UNTRACKED_VK_FREE_MEMORY, UNTRACKED_VK_UNMAP_MEMORY, UNTRACKED_EXTERNAL_RELEASE
 *   UNTRACKED_GPU_VA_UNMAP, UNTRACKED_HANDLE_ERASE, UNTRACKED_SYNC_RECYCLE
 *   UNTRACKED_VK_DESTROY_BUFFER, UNTRACKED_VK_DESTROY_IMAGE
 *   UNTRACKED_IMAGE_VIEW, UNTRACKED_CMDPOOL
 *
 * Runtime toggles (env or Android system property, no rebuild):
 * Product defaults (Mali freeflight fix, R1/R2 on CUSA07023):
 *   wait_on_suballoc_overlap=1, suballoc_range_pool=1, vortek_defer_destroy=1
 *   (set prop/env=0 to disable). Dig stamp/exact/pin remain default OFF.
 *   BACHATA_VORTEK_DEFER_RESOURCE_DESTROY=1
 *     / debug.bachata.vortek_defer_destroy
 *   BACHATA_VORTEK_WAIT_IDLE_BEFORE_DESTROY=1
 *     / debug.bachata.vortek_wait_idle_destroy
 *   Class-specific (OR with global wait-idle) — D1..D5 matrix:
 *     debug.bachata.vortek_wait_idle_external   (D1 external/GPU-VA)
 *     debug.bachata.vortek_wait_idle_memory     (D2 free/unmap)
 *     debug.bachata.vortek_wait_idle_sync       (D3 present semaphore)
 *     debug.bachata.vortek_wait_idle_buffer     (D4 buffer+image)
 *     debug.bachata.vortek_wait_idle_image      (D4 image-only alias)
 *     debug.bachata.vortek_wait_idle_cmdpool    (D5 command-pool)
 *     debug.bachata.vortek_wait_idle_mapping
 *   Narrow present-ownership diagnostics (prefer over full D3):
 *     debug.bachata.vortek_wait_before_present_sem_reuse
 *     debug.bachata.vortek_wait_before_same_image_reuse
 *     debug.bachata.vortek_wait_before_same_ahb_reuse
 *     debug.bachata.vortek_wait_for_ahb_release_fence
 *   Strict private AHB / freeflight diagnostics:
 *     debug.bachata.require_distinct_ahb=1
 *     debug.bachata.quarantine_gpu_releases=1  (after first frame; all physical free deferred)
 *   Class-specific quarantine (OR with global; after first frame):
 *     debug.bachata.quarantine_buffers=1
 *     debug.bachata.quarantine_images=1
 *     debug.bachata.quarantine_memory=1
 *   Targeted buffer retention (product diagnostic):
 *     debug.bachata.retain_unknown_buffers=1
 *       known-complete buffers retire; unknown/unresolved use retained
 *   Suballocation reuse race:
 *     debug.bachata.wait_on_suballoc_overlap=1
 *       R1 fallback: on BindBufferMemory overlap with in-flight range, wait the
 *       conflicting queue serial's completion fence (never DeviceWaitIdle).
 *     debug.bachata.suballoc_range_pool=1
 *       R2 product: generation-based range leases + multi-slot size-class pool.
 *       Prefer completed/non-overlapping slots; grow within max slots/bytes;
 *       exact-fence wait only when pool exhausted. R1 remains fallback.
 *       Props: debug.bachata.suballoc_pool_initial_slots (default 4)
 *              debug.bachata.suballoc_pool_max_slots (default 8)
 *              debug.bachata.suballoc_pool_max_mb (default 96)
 *       Logs: SUBALLOC_RANGE_ACQUIRED, SUBALLOC_BUSY_SKIPPED, SUBALLOC_POOL_GROWN,
 *             SUBALLOC_CPU_WRITE_BLOCKED_IN_FLIGHT, SUBALLOC_TARGETED_FENCE_WAIT,
 *             SUBALLOC_RANGE_REUSABLE, SUBALLOC_STALE_COMPLETION,
 *             SUBALLOC_GENERATION_MISMATCH, SUBALLOC_POOL_STATS,
 *             SUBALLOC_BIND_BLOCKED_IN_FLIGHT, SUBALLOC_TARGETED_WAIT,
 *             SUBALLOC_REUSE_ALLOWED.
 *   GPU mapping lifetime dig (exact-wait ruled out as primary fix):
 *     debug.bachata.pin_fhd_detile_sources=1
 *       Never free/unbind/replace/close FHD (0x7f8000-class) external sources.
 *     debug.bachata.gpu_address_binding_report=1
 *       Enable VK_EXT_device_address_binding_report + debug_utils ADDRESS_BINDING
 *       messenger (default OFF — M1c bind/unbind storm regressed early survival).
 *     debug.bachata.fhd_prepare_write_diag=0
 *       Disable always-on FHD_PREPARE_WRITE_CHECK logging (default ON).
 *     Logs: FHD_PREPARE_WRITE_CHECK, GPU_ADDRESS_BIND, GPU_ADDRESS_UNBIND,
 *           GPU_ADDRESS_UNBIND_IN_FLIGHT, DEVICE_FAULT_INFO, DEVICE_FAULT_OWNER,
 *           EXTERNAL_FD_*, EXTERNAL_FD_CLOSED_WHILE_BOUND,
 *           EXTERNAL_BACKING_RELEASED_WHILE_BOUND, GPU_BINDING_REPLACED_IN_FLIGHT,
 *           BUFFER_REBOUND_WITH_STALE_DESCRIPTOR, DESCRIPTOR_REFERENCES_OLD_BIND_GENERATION,
 *           FHD_SOURCE_PINNED_RETENTION, GPU_ADDRESS_BINDING_REPORT enabled=,
 *           DEVICE_FAULT_QUERY enabled=
 *   Aliases:
 *     debug.bachata.wait_idle_buffer_destroy
 *     debug.bachata.wait_idle_memory_free
 *     debug.bachata.wait_idle_external_release
 *     debug.bachata.wait_idle_mapping_unmap
 *     debug.bachata.wait_idle_sync_recycle
 *     debug.bachata.wait_idle_command_pool_reset
 *     debug.bachata.wait_idle_image_destroy
 *
 * Completion invariant:
 *   completed_serial advances only from verified Vulkan proof:
 *     internal fence signaled | caller fence wait matched | timeline |
 *     queue/device wait-idle diagnostic.
 *   Never advance because later submit/present/destroy/message arrived.
 *
 * Physical release invariant:
 *   Every real GPU free/unmap/external-close goes through the retirement
 *   oracle (RequestPhysicalRelease). Untracked bypass logs UNTRACKED_*.
 */
#ifndef VORTEK_GPU_TRACK_H
#define VORTEK_GPU_TRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ResourceMemory ResourceMemory;

/* Marker string kept for APK/source verify tests — do not rename. */
#define BACHATA_VORTEK_GPU_TRACK_MARKER "bachata_vortek_gpu_va_track"

/* Wait-idle diagnostic classes (Mode C + narrower D1..D5 controls). */
typedef enum VortekWaitIdleClass {
    VORTEK_WAIT_IDLE_BUFFER = 1,   /* buffer destroy (also images if no IMAGE class) */
    VORTEK_WAIT_IDLE_MEMORY = 2,   /* memory free/unmap (D2) */
    VORTEK_WAIT_IDLE_EXTERNAL = 3, /* external/guest backing release (D1) */
    VORTEK_WAIT_IDLE_SYNC = 4,     /* fence/semaphore destroy + present recycle (D3) */
    VORTEK_WAIT_IDLE_CMDPOOL = 5,  /* command-pool reset/destroy (D5) */
    VORTEK_WAIT_IDLE_MAPPING = 6,  /* mapping unmap / guest map release */
    VORTEK_WAIT_IDLE_IMAGE = 7,    /* image/image-view destruction (D4) */
} VortekWaitIdleClass;

/* Proven completion sources. Unknown must not retire. */
typedef enum VortekCompletionSource {
    VORTEK_COMPLETION_UNKNOWN = 0,
    VORTEK_COMPLETION_INTERNAL_FENCE = 1,
    VORTEK_COMPLETION_CALLER_FENCE = 2,
    VORTEK_COMPLETION_TIMELINE = 3,
    VORTEK_COMPLETION_QUEUE_IDLE = 4,
    VORTEK_COMPLETION_DEVICE_IDLE = 5,
} VortekCompletionSource;

/* Why a physical release is requested (audit ring). */
typedef enum VortekReleaseReason {
    VORTEK_RELEASE_DESTROY_BUFFER = 1,
    VORTEK_RELEASE_DESTROY_IMAGE = 2,
    VORTEK_RELEASE_FREE_MEMORY = 3,
    VORTEK_RELEASE_UNMAP_MEMORY = 4,
    VORTEK_RELEASE_EXTERNAL = 5,
    VORTEK_RELEASE_GPU_VA_UNMAP = 6,
    VORTEK_RELEASE_IMAGE_VIEW = 7,
    VORTEK_RELEASE_SEMAPHORE = 8,
    VORTEK_RELEASE_CMDPOOL = 9,
    VORTEK_RELEASE_HANDLE_ERASE = 10,
    VORTEK_RELEASE_COLLECTOR = 11,
} VortekReleaseReason;

void VortekGpuTrack_initOnce(void);

void VortekGpuTrack_onAlloc(ResourceMemory* rm, uint32_t memoryTypeIndex,
                            const char* backingType);
/* Returns true if caller should perform the real free now. */
bool VortekGpuTrack_onFree(ResourceMemory* rm, void* device);
void VortekGpuTrack_onMap(ResourceMemory* rm);
/* Returns true if caller should unmap now. Untracked if rm unknown. */
bool VortekGpuTrack_onUnmap(ResourceMemory* rm, void* device);

void VortekGpuTrack_onCreateBuffer(void* buffer, uint64_t size, uint32_t usage);
/* Returns true if caller should vkDestroyBuffer now. */
bool VortekGpuTrack_onDestroyBuffer(void* buffer, void* device);
void VortekGpuTrack_onCreateImage(void* image, uint32_t width, uint32_t height,
                                  uint32_t format, uint32_t usage);
/* Returns true if caller should vkDestroyImage now. */
bool VortekGpuTrack_onDestroyImage(void* image, void* device);

void VortekGpuTrack_onCreateImageView(void* imageView, void* image);
/* Returns true if caller should vkDestroyImageView now. */
bool VortekGpuTrack_onDestroyImageView(void* imageView, void* device);

void VortekGpuTrack_onBindBuffer(void* buffer, ResourceMemory* rm,
                                 uint64_t bindOffset, uint64_t requirementsSize,
                                 uint64_t requirementsAlignment);
void VortekGpuTrack_onBindImage(void* image, ResourceMemory* rm,
                                uint64_t bindOffset, uint64_t requirementsSize,
                                uint64_t requirementsAlignment);

/* Optional GPU-VA / guest mapping stamp (distinct identity). */
void VortekGpuTrack_onGpuVaMap(ResourceMemory* rm, uint64_t gpuVa, uint64_t gpuSize);
void VortekGpuTrack_onGuestVaMap(ResourceMemory* rm, uint64_t guestVa, uint64_t guestSize);

/* Record-time: associate resources with the command buffer. Does NOT stamp
 * last_use serials (only QueueSubmit does). Returns false if range invalid. */
bool VortekGpuTrack_onGpuAccess(void* commandBuffer, const char* operation,
                                void* srcResource, uint64_t srcOffset,
                                uint64_t srcSize, void* dstResource,
                                uint64_t dstOffset, uint64_t dstSize,
                                uint32_t width, uint32_t height, uint32_t pitch,
                                int tilingMode);

/* Detailed copy_buffer_to_image path (also tracks cmd resources + closure). */
bool VortekGpuTrack_onCopyBufferToImage(void* commandBuffer, void* srcBuffer,
                                        void* dstImage, uint32_t dstLayout,
                                        uint64_t bufferOffset, uint32_t rowLength,
                                        uint32_t imageHeight, uint32_t width,
                                        uint32_t height, uint32_t depth);

void VortekGpuTrack_onImageBarrier(void* commandBuffer, void* image,
                                   uint32_t srcStage, uint32_t srcAccess,
                                   uint32_t dstStage, uint32_t dstAccess,
                                   uint32_t oldLayout, uint32_t newLayout);

void VortekGpuTrack_onCmdReset(void* commandBuffer);
void VortekGpuTrack_onCommandPoolReset(void* commandPool, void* device);
void VortekGpuTrack_onCommandPoolDestroy(void* commandPool, void* device);

/* QueueSubmit: pass command buffers participating in this submit. */
uint64_t VortekGpuTrack_beginSubmission(void);
void VortekGpuTrack_noteSubmitCommandBuffer(void* commandBuffer);
/* Legacy: binds caller fence as CALLER_FENCE when non-null. Prefer Completion. */
void VortekGpuTrack_bindSubmissionFence(uint64_t submission, void* fence);
/* Attach completion token (internal or caller fence / timeline handle). */
void VortekGpuTrack_bindSubmissionCompletion(uint64_t submission, void* fenceOrTimeline,
                                             VortekCompletionSource source);
void VortekGpuTrack_endSubmission(uint64_t submission, int vkResult, bool deviceLost);
void VortekGpuTrack_noteQueue(void* queue);

/* Completion signals — verified only. Never infer from later messages. */
void VortekGpuTrack_noteFenceWaitResult(int vkResult);
void VortekGpuTrack_noteFencesWait(void* const* fences, uint32_t fenceCount, int waitAll,
                                   int vkResult);
/* Poll path: host GetFenceStatus on completion tokens. SUCCESS advances that sub only. */
void VortekGpuTrack_noteFenceStatus(void* fence, int vkGetFenceStatusResult);
/* Fill pending completion fences for host poll. Returns count written. */
int VortekGpuTrack_fillPendingCompletionFences(void** outFences, int maxCount);
/* After host recycles a signaled internal fence, clear tracker binding. */
void VortekGpuTrack_releaseCompletionFence(void* fence);

void VortekGpuTrack_noteQueueWaitIdle(void* queue, int vkResult);
void VortekGpuTrack_noteDeviceWaitIdle(int vkResult);
/* Tracker accuracy after diagnostic DeviceWaitIdle (Mode C) — not production fix. */
void VortekGpuTrack_markAllSubmittedWorkCompleted(void);

void VortekGpuTrack_onPresentSyncFailed(int vkResult);
void VortekGpuTrack_notePresent(void* queue, uint64_t presentId);
void VortekGpuTrack_notePresentWait(void* queue, uint64_t presentId, uint32_t imageIndex,
                                    void* semaphore);
/* Product present begin: returns false if semaphore/image still PRESENT_PENDING. */
bool VortekGpuTrack_beginPresent(uint64_t presentId, uint32_t imageIndex, void* waitSemaphore);
void VortekGpuTrack_notePresentAccepted(uint64_t presentId, uint32_t imageIndex);
void VortekGpuTrack_notePresentSemaphoreDestroyed(void* semaphore);
/* Presenter / acquire diagnostics (imageIndex=0 investigation). */
void VortekGpuTrack_notePresenterConfig(uint32_t swapchainImageCount,
                                        uint32_t internalImageCount, uint32_t frameSlotCount,
                                        uint32_t presentQueueFamily, uint32_t graphicsQueueFamily,
                                        void* swapchainHint);
void VortekGpuTrack_noteAcquireResult(uint64_t callId, int vkResult, uint32_t returnedImageIndex,
                                      void* acquireSemaphore, void* acquireFence);
/* Present complete with proof source: compositor_sync|reacquire|diag_*|ahb_release_fence. */
void VortekGpuTrack_notePresentComplete(uint64_t presentId, uint32_t imageIndex,
                                        const char* source);

/* Swapchain AHB / image backing ownership. */
void VortekGpuTrack_noteSwapchainImageBacking(uint32_t imageIndex, void* image, void* ahb,
                                              void* memory, void* gpuMapping);
/* Register private AHB import; writes distinct ahbId/mappingId/gpuVaId (0x1/2/3 spaces). */
void VortekGpuTrack_registerSwapchainBacking(uint32_t imageIndex, void* vkImage, void* vkMemory,
                                             void* ahb, intptr_t nativeHandle, uint64_t allocationSize,
                                             uint32_t usage, uint32_t stride, uint64_t* outAhbId,
                                             uint64_t* outMappingId, uint64_t* outGpuVaId);
void VortekGpuTrack_noteSharedAhb(void* ahb, uint32_t imageCount);
/* Guest requested multi-image; host capped to |actual| (single window AHB). */
void VortekGpuTrack_noteSingleImageCap(uint32_t requested, uint32_t actual);
void VortekGpuTrack_logBusyAhb(void* ahb, uint32_t newImageIndex, uint32_t busyImageIndex);
void VortekGpuTrack_forcePresentCompleteForAhb(void* ahb, const char* source);

bool VortekGpuTrack_requireDistinctAhb(void);
bool VortekGpuTrack_quarantineGpuReleases(void);
bool VortekGpuTrack_quarantineBuffers(void);
bool VortekGpuTrack_quarantineImages(void);
bool VortekGpuTrack_quarantineMemory(void);
bool VortekGpuTrack_retainUnknownBuffers(void);
void VortekGpuTrack_noteFirstFrame(void);

/* Record-time buffer use that is not a full GpuAccess (bind/indirect/fill/etc). */
void VortekGpuTrack_onCmdBufferRef(void* commandBuffer, void* buffer, const char* operation,
                                   int isWrite);
/* Primary inherits secondary command buffer resource closure. */
void VortekGpuTrack_onExecuteCommands(void* primaryCommandBuffer, void* const* secondaryBuffers,
                                      uint32_t secondaryCount);
/* Snapshot descriptor-set buffer bindings from UpdateDescriptorSets.
 * Legacy: replaces snapshot (incomplete for multi-write detile). Prefer Writes. */
void VortekGpuTrack_onUpdateDescriptorBuffers(void* descriptorSet, void* const* buffers,
                                              uint32_t bufferCount);
/* Merge per-binding writes into set snapshot (detile: 3 writes for bind 0/1/2). */
void VortekGpuTrack_onUpdateDescriptorWrites(void* descriptorSet, const uint32_t* bindings,
                                             const uint32_t* descriptorTypes, void* const* buffers,
                                             const uint64_t* offsets, const uint64_t* ranges,
                                             uint32_t count);
/* Bind-time: attach snapshotted descriptor buffers to the command buffer. */
void VortekGpuTrack_onBindDescriptorSets(void* commandBuffer, void* const* descriptorSets,
                                         uint32_t setCount);
/* Push descriptors (detile path): stamp real buffer bindings on the command buffer.
 * binding 0 storage = tiled source (read); binding 1 storage = linear dest (write). */
void VortekGpuTrack_onPushDescriptorBuffers(void* commandBuffer, uint32_t setIndex,
                                            const uint32_t* bindings, const uint32_t* descriptorTypes,
                                            void* const* buffers, const uint64_t* offsets,
                                            const uint64_t* ranges, uint32_t count);
/* CmdDispatch: resolve push/bind descriptor buffers → DETILE_DISPATCH + GPU_ACCESS with
 * real src/dst resource ids (fixes srcResource=0). */
void VortekGpuTrack_onCmdDispatch(void* commandBuffer, uint32_t groupCountX, uint32_t groupCountY,
                                  uint32_t groupCountZ);
/* Register buffer device address for BDA range tracking. */
void VortekGpuTrack_onBufferDeviceAddress(void* buffer, uint64_t address, uint64_t size);
/* Direct vkDestroyBuffer outside the retirement choke point. */
void VortekGpuTrack_noteDirectDestroyBufferBypass(void* buffer, void* device, const char* site);

/* Freeflight / lifetime event ring (dumped on DEVICE_LOST). */
void VortekGpuTrack_freeflightEvent(const char* tag, void* queue, uint64_t submission, void* resource,
                                    uint64_t allocation, uint64_t backing, uint64_t mapping,
                                    uint64_t lastUse, uint64_t completed, uint32_t pendingRefs,
                                    int logicalDestroy, int physicalDestroy);

/* Sync-FD ownership (acquire/release fences). */
void VortekGpuTrack_noteSyncFd(int fd, const char* owner, uint32_t imageIndex, uint64_t presentId,
                               int imported, int waited, int closed);
void VortekGpuTrack_syncFdImported(int fd, const char* owner, uint32_t imageIndex,
                                   uint64_t presentId);
void VortekGpuTrack_syncFdWaited(int fd, const char* owner);
void VortekGpuTrack_syncFdClosed(int fd, const char* owner);

/* Narrow present-ownership diagnostic toggles. */
bool VortekGpuTrack_waitBeforePresentSemReuse(void);
bool VortekGpuTrack_waitBeforeSameImageReuse(void);
bool VortekGpuTrack_waitBeforeSameAhbReuse(void);
bool VortekGpuTrack_waitForAhbReleaseFence(void);

/* Suballoc range-reuse: wait exact conflicting serials (not DeviceWaitIdle). */
bool VortekGpuTrack_waitOnSuballocOverlapEnabled(void);
bool VortekGpuTrack_suballocRangePoolEnabled(void);
/* Detile dig knobs (both default OFF — deep-run playability). */
bool VortekGpuTrack_detileStampEnabled(void);
/* Exact wait: block FHD rewrite while lastGpuRead incomplete (needs stamp on). */
bool VortekGpuTrack_detileSourceExactWaitEnabled(void);
/* Pin FHD detile sources: never free/unbind/replace external backing (default OFF). */
bool VortekGpuTrack_pinFhdDetileSourcesEnabled(void);
/* VK_EXT_device_address_binding_report + ADDRESS_BINDING messenger (default OFF). */
bool VortekGpuTrack_gpuAddressBindingReportEnabled(void);
/* Always-on FHD_PREPARE_WRITE_CHECK log spam (default ON; set 0 for M1e). */
bool VortekGpuTrack_fhdPrepareWriteDiagEnabled(void);
/* Device-fault query on LOST remains available when pfn registered (always preferred ON). */
bool VortekGpuTrack_deviceFaultQueryWanted(void);
/* Before rewrite/map/copy into a buffer range. Logs FHD_PREPARE_WRITE_CHECK when
 * fhd_prepare_write_diag on. path: Copy|MapWrite|SynchronizeBuffer|DescriptorUpdate|BindReplace.
 * Returns 1 if host must wait (only when exact_wait enabled). */
int VortekGpuTrack_prepareResourceWrite(void* resource, uint64_t offset, uint64_t size,
                                        const char* path);
/* External FD lifecycle (import / dup / close / ownership). */
void VortekGpuTrack_onExternalFdEvent(ResourceMemory* rm, int fd, int originalFd,
                                      const char* event);
/* VK_EXT_device_address_binding_report (via debug utils messenger).
 * bindingType: 0=BIND 1=UNBIND. No-op when report prop off. */
void VortekGpuTrack_noteAddressBinding(uint64_t baseAddress, uint64_t size, int bindingType,
                                       uint32_t flags);
/* Register device + vkGetDeviceFaultInfoEXT for DEVICE_LOST dumps. */
void VortekGpuTrack_registerDeviceFaultQuery(void* device, void* pfnGetDeviceFaultInfoEXT);
/* After onBindBuffer / acquire: fences for incomplete overlapping uses.
 * outMaxSubmission = global max overlap submission id; outQueueIndex/Serial = worst use.
 * Uses durable serial→fence map so ring wrap cannot drop exact tokens. */
int VortekGpuTrack_fillSuballocOverlapWaitFences(void** outFences, int maxCount,
                                                 uint64_t* outMaxSubmission,
                                                 int* outQueueIndex,
                                                 uint64_t* outQueueSerial);
/* True while pending overlap still has incomplete queue serial (after partial waits). */
bool VortekGpuTrack_suballocOverlapStillIncomplete(void);
void* VortekGpuTrack_suballocOverlapQueue(void); /* owning queue for last-resort QueueWaitIdle */
void VortekGpuTrack_noteSuballocTargetedWait(uint64_t maxSubmission, int queueIndex,
                                             uint64_t queueSerial, int fenceCount,
                                             int vkResult);
/* R2: acquire generation lease before bind/CPU write. Returns 1 if host must wait
 * on fillSuballocOverlapWaitFences before proceeding. out_generation set always. */
int VortekGpuTrack_acquireSuballocLease(void* buffer, ResourceMemory* rm, uint64_t bindOffset,
                                        uint64_t size, uint64_t alignment,
                                        uint64_t* out_generation);
/* R2: before Map/Flush of allocation — 1 if CPU write blocked (wait required). */
int VortekGpuTrack_prepareCpuWrite(ResourceMemory* rm, uint64_t offset, uint64_t size,
                                   uint64_t* out_generation);
void VortekGpuTrack_noteCpuWriteBegin(ResourceMemory* rm, uint64_t offset, uint64_t size);
void VortekGpuTrack_noteCpuWriteEnd(ResourceMemory* rm, uint64_t offset, uint64_t size);
void VortekGpuTrack_noteHostFlush(ResourceMemory* rm, uint64_t offset, uint64_t size);
/* R2: physical pool slot redirect for busy guest ranges (request_handler owns VkDeviceMemory). */
int VortekGpuTrack_tryReservePoolSlot(uint64_t size, uint32_t memoryTypeIndex,
                                      int* out_slot_index, uint64_t* out_generation);
void VortekGpuTrack_notePoolSlotBound(int slot_index, void* buffer, void* memory,
                                     uint64_t size, uint64_t generation);
void VortekGpuTrack_notePoolSlotFreed(int slot_index);
void VortekGpuTrack_notePoolGrown(int slots, uint64_t bytes);
void VortekGpuTrack_noteQueueIdleFallback(void);
void VortekGpuTrack_noteExactFenceWait(void);
/* Periodic aggregate (also callable from host after waits). */
void VortekGpuTrack_maybeLogSuballocPoolStats(void);

void VortekGpuTrack_onDeviceLost(const char* where);

bool VortekGpuTrack_rangeFits(uint64_t allocationSize, uint64_t bindOffset,
                              uint64_t resourceOffset, uint64_t accessSize);

/* Config query (after initOnce). */
bool VortekGpuTrack_deferDestroyEnabled(void);
bool VortekGpuTrack_waitIdleBeforeDestroyEnabled(void);
bool VortekGpuTrack_shouldWaitIdleBefore(VortekWaitIdleClass klass);

/* Drain one ready deferred destroy. Returns 1 if an entry was written.
 * outKind: 1=buffer, 2=image, 3=memory. Caller performs the real Vulkan free. */
int VortekGpuTrack_takeDeferredDestroy(int* outKind, void** outHandle, void** outDevice,
                                       ResourceMemory** outRm);

/* Periodic collector (also invoked on completion updates). */
void VortekGpuTrack_collectRetired(void);

#ifdef __cplusplus
}
#endif

#endif /* VORTEK_GPU_TRACK_H */
