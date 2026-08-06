import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("Vortek GPU track: per-queue retirement + full closure", () => {
  const header = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/include/vortek_gpu_track.h",
  );
  const track = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/vortek_gpu_track.c",
  );
  const resource = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/resource_memory.c",
  );
  const handler = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/request_handler.c",
  );
  const swap_wsi = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/bachata_xwindow_swapchain.c",
  );
  const cmake = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/CMakeLists.txt",
  );
  const verifier = read("runtime/tests/verify-native-fixes.mjs");
  const presenter = read("src/video_core/renderer_vulkan/vk_presenter.cpp");
  const swapchain = read("src/video_core/renderer_vulkan/vk_swapchain.cpp");

  assert.match(header, /bachata_vortek_gpu_va_track/);
  assert.match(header, /VortekGpuTrack_rangeFits/);
  assert.match(header, /VortekGpuTrack_takeDeferredDestroy/);
  assert.match(header, /BACHATA_VORTEK_DEFER_RESOURCE_DESTROY/);
  assert.match(header, /BACHATA_VORTEK_WAIT_IDLE_BEFORE_DESTROY/);
  assert.match(header, /VortekGpuTrack_collectRetired/);
  assert.match(header, /VortekGpuTrack_markAllSubmittedWorkCompleted/);
  assert.match(header, /VortekGpuTrack_noteFencesWait/);
  assert.match(header, /VortekGpuTrack_bindSubmissionFence/);
  assert.match(header, /VORTEK_WAIT_IDLE_BUFFER/);

  assert.match(track, /VORTEK_ALLOC/);
  assert.match(track, /VORTEK_BIND_BUFFER/);
  assert.match(track, /VORTEK_FREE/);
  assert.match(track, /GPU_ACCESS/);
  assert.match(track, /GPU_RANGE_INVALID/);
  assert.match(track, /RESOURCE_FREED_IN_FLIGHT/);
  assert.match(track, /RESOURCE_DESTROY_DEFERRED/);
  assert.match(track, /RESOURCE_DESTROY_COMMITTED/);
  assert.match(track, /RETIRE_REQUEST/);
  assert.match(track, /RETIRE_COMMIT/);
  assert.match(track, /ALLOCATION_RETIRE_BLOCKED/);
  assert.match(track, /BACKING_RELEASE_IN_FLIGHT/);
  assert.match(track, /DEVICE_WAIT_IDLE_COMPLETION_ADVANCE/);
  assert.match(track, /DEVICE_LOST_SNAPSHOT/);
  assert.match(track, /PENDING_RESOURCE/);
  assert.match(track, /COPY_BUFFER_TO_IMAGE/);
  assert.match(track, /IMAGE_BARRIER/);
  assert.match(track, /VORTEK_TRACK_CONFIG/);
  assert.match(track, /pending_submission_refs/);
  assert.match(track, /last_submitted_use/);
  assert.match(track, /QueueTracker/);
  assert.match(track, /completed_serial/);
  assert.match(track, /queue_uses_completed/);
  assert.match(track, /add_resource_closure/);
  assert.match(track, /live_child_objects/);
  assert.match(track, /perQueueRetirement=1/);
  assert.match(track, /fullClosure=1/);
  // Use is stamped at submit, not only at record.
  assert.match(track, /VortekGpuTrack_noteSubmitCommandBuffer/);
  assert.match(track, /stamp_resource_on_submit/);
  assert.match(track, /INTERNAL_COMPLETION_SUBMIT/);
  assert.match(track, /INTERNAL_COMPLETION_OBSERVED/);
  assert.match(track, /RETIRE_BLOCKED_NO_COMPLETION_PROOF/);
  assert.match(track, /completionSource=%s/);
  assert.match(track, /BACKING_RETIRE_REQUEST/);
  assert.match(track, /BACKING_RETIRE_COMMIT/);
  assert.match(track, /BACKING_RETIRE_BLOCKED/);
  assert.match(track, /MAPPING_UNMAP_DEFERRED/);
  assert.match(track, /EXTERNAL_HANDLE_RELEASE_DEFERRED/);
  assert.match(track, /HANDLE_SLOT_REUSE_BLOCKED/);
  assert.match(track, /PHYSICAL_RELEASE_WITHOUT_COMPLETION/);
  assert.match(track, /RESOURCE_USE allocation=/);
  assert.match(track, /RESOURCE_USE externalBacking=/);
  assert.match(track, /internalCompletion=1/);
  assert.match(track, /failClosed=1/);
  assert.match(track, /waitIdleMapping=/);
  assert.match(track, /waitIdleImage=/);
  assert.match(track, /physicalReleaseChoke=1/);
  assert.match(track, /distinctBackingIds=1/);
  assert.match(track, /PHYSICAL_RELEASE_REQUEST/);
  assert.match(track, /PHYSICAL_RELEASE_COMMIT/);
  assert.match(track, /PHYSICAL_RELEASE_BLOCKED_NO_COMPLETION/);
  assert.match(track, /PHYSICAL_RELEASE_BLOCKED_DEPENDENCY/);
  assert.match(track, /UNTRACKED_VK_FREE_MEMORY/);
  assert.match(track, /UNTRACKED_VK_DESTROY_BUFFER/);
  assert.match(track, /UNTRACKED_VK_DESTROY_IMAGE/);
  assert.match(track, /PRESENTER_CONFIG/);
  assert.match(track, /ACQUIRE_RESULT/);
  assert.match(track, /PRESENT_SEMAPHORE_REUSE_CHECK/);
  assert.match(track, /PRESENT_SEMAPHORE_REUSED_IN_FLIGHT/);
  assert.match(track, /PRESENT_SEMAPHORE_REUSE_BLOCKED/);
  assert.match(track, /PRESENT_BEGIN/);
  assert.match(track, /PRESENT_ACCEPTED/);
  assert.match(track, /PRESENT_COMPLETED/);
  assert.match(track, /RENDER_WAIT_COMPLETE/);
  assert.match(track, /COMPOSITOR_SYNC_COMPLETE/);
  assert.match(track, /PRESENT_IMAGE_REUSED_BEFORE_REACQUIRE/);
  assert.match(track, /SWAPCHAIN_IMAGE_BACKING/);
  assert.match(track, /SHARED_AHB/);
  assert.match(track, /PRESENT_ON_BUSY_AHB/);
  assert.match(track, /SYNC_FD_IMPORTED_TWICE/);
  assert.match(track, /offsetIdSpaces=1/);
  assert.match(track, /presentWaitSemConsume=1/);
  assert.match(track, /waitBeforePresentSemReuse=/);
  assert.match(track, /requireDistinctAhb=/);
  assert.match(track, /quarantineGpuReleases=/);

  assert.match(track, /quarantineBuffers=/);
  assert.match(track, /quarantineImages=/);
  assert.match(track, /quarantineMemory=/);
  assert.match(track, /retainUnknownBuffers=/);
  assert.match(track, /BUFFER_RETIRE_COMMIT/);
  assert.match(track, /BUFFER_RETIRE_BLOCKED_UNKNOWN_USE/);
  assert.match(track, /BUFFER_RETIRE_BLOCKED_IN_FLIGHT/);
  assert.match(track, /BUFFER_RETIRE_BLOCKED_BDA/);
  assert.match(track, /DIRECT_VK_DESTROY_BUFFER_BYPASS/);
  assert.match(track, /BDA_USE_RESOLVED/);
  assert.match(track, /UNKNOWN_BUFFER_RETENTION_SUMMARY/);
  assert.match(track, /bufferRetirementCoverage=1/);
  assert.match(track, /multiClassRetirement=1/);
  assert.match(track, /antiThrashCollect=1/);
  assert.match(track, /IMAGE_RETIRE_BLOCKED_UNKNOWN_USE/);
  assert.match(track, /MEMORY_RETIRE_BLOCKED_UNKNOWN_USE/);
  assert.match(track, /retire_sticky_block/);
  assert.match(track, /SUBALLOC_REUSE_IN_FLIGHT/);
  assert.match(track, /suballocReuseTrack=1/);
  assert.match(track, /waitOnSuballocOverlap=/);
  assert.match(track, /SUBALLOC_BIND_BLOCKED_IN_FLIGHT/);
  assert.match(track, /SUBALLOC_TARGETED_WAIT/);
  assert.match(track, /SUBALLOC_REUSE_ALLOWED/);
  assert.match(track, /SUBALLOC_TARGETED_WAIT_QUEUE_IDLE|suballocOverlapStillIncomplete|collect_suballoc_wait_fences/);
  assert.match(track, /wait_on_suballoc_overlap/);
  assert.match(header, /wait_on_suballoc_overlap/);
  assert.match(header, /VortekGpuTrack_waitOnSuballocOverlapEnabled/);
  assert.match(header, /VortekGpuTrack_fillSuballocOverlapWaitFences/);
  assert.match(header, /VortekGpuTrack_noteSuballocTargetedWait/);
  assert.match(header, /VortekGpuTrack_suballocOverlapStillIncomplete/);
  assert.match(handler, /bachata_wait_suballoc_overlap/);
  assert.match(handler, /SUBALLOC_TARGETED_WAIT_QUEUE_IDLE/);
  /* R2 range pool + generation leases + durable fences */
  assert.match(track, /suballocRangePool=/);
  assert.match(track, /durableFenceMap=1/);
  assert.match(track, /SUBALLOC_RANGE_ACQUIRED/);
  assert.match(track, /SUBALLOC_BUSY_SKIPPED/);
  assert.match(track, /SUBALLOC_POOL_GROWN/);
  assert.match(track, /SUBALLOC_CPU_WRITE_BLOCKED_IN_FLIGHT/);
  assert.match(track, /SUBALLOC_TARGETED_FENCE_WAIT/);
  assert.match(track, /SUBALLOC_RANGE_REUSABLE/);
  assert.match(track, /SUBALLOC_STALE_COMPLETION/);
  assert.match(track, /SUBALLOC_GENERATION_MISMATCH/);
  assert.match(track, /SUBALLOC_POOL_STATS/);
  assert.match(track, /SuballocLease/);
  assert.match(track, /DurableFence|g_durable_fences/);
  assert.match(track, /begin_new_generation/);
  assert.match(track, /collect_durable_fences_for_need/);
  assert.match(header, /suballoc_range_pool/);
  assert.match(header, /VortekGpuTrack_suballocRangePoolEnabled/);
  assert.match(header, /VortekGpuTrack_acquireSuballocLease/);
  assert.match(header, /VortekGpuTrack_prepareCpuWrite/);
  assert.match(header, /VortekGpuTrack_maybeLogSuballocPoolStats/);
  assert.match(handler, /bachata_try_pool_bind/);
  assert.match(handler, /kBachataInternalFencePool = 512/);
  /* Phase 1 exact-fence reliability: generation + active waiters */
  assert.match(handler, /BachataInternalFenceSlot/);
  assert.match(handler, /active_waiters/);
  assert.match(handler, /bachata_wait_fences_exact/);
  assert.match(handler, /FENCE_RECYCLED_WHILE_REFERENCED/);
  assert.match(handler, /SERIAL_FENCE_GENERATION_MISMATCH/);
  assert.match(handler, /SUBMISSION_WITHOUT_COMPLETION_TOKEN/);
  assert.match(handler, /TARGETED_FENCE_WAIT_SLOW/);
  assert.match(handler, /bachata_bind_internal_fence_serial/);
  assert.match(handler, /VortekGpuTrack_noteQueueIdleFallback/);
  assert.match(handler, /VortekGpuTrack_noteExactFenceWait/);
  assert.match(handler, /VortekGpuTrack_prepareCpuWrite/);
  /* Phase 2 guest staging ring (detile scratch) */
  const tile = read("src/video_core/texture_cache/tile_manager.cpp");
  const tile_h = read("src/video_core/texture_cache/tile_manager.h");
  assert.match(tile_h, /kScratchInitialSlots = 8/);
  assert.match(tile_h, /kScratchMaxSlots = 16/);
  assert.match(tile_h, /ScratchSlot/);
  assert.match(tile, /STAGING_SLOT_ACQUIRED/);
  assert.match(tile, /STAGING_SLOT_SUBMITTED/);
  assert.match(tile, /STAGING_SLOT_COMPLETED/);
  assert.match(tile, /STAGING_POOL_GROWN/);
  assert.match(tile, /STAGING_POOL_EXHAUSTED/);
  assert.match(tile, /STAGING_POOL_STATS/);
  assert.match(tile, /AcquireScratchSlot/);
  assert.match(tile, /slot\.tick >= cpu_tick/);
  assert.match(tile, /next_scratch_rr/);
  assert.match(tile, /strict_scratch/);
  assert.match(tile, /tick_lag/);
  assert.match(tile, /mode=exact_wait/);
  assert.doesNotMatch(tile, /GetScratchBuffer/);
  assert.doesNotMatch(tile, /vmaDestroyBuffer\(instance\.GetAllocator\(\), out_buffer/);
  assert.doesNotMatch(tile, /vmaDestroyBuffer\(instance\.GetAllocator\(\), temp_buffer/);
  /* Late DEVICE_LOST dig: provenance + A/B modes B/C/D/E */
  const staging_diag = read("src/video_core/staging_diag.h");
  assert.match(staging_diag, /BACHATA_STAGING_STRICT_SCRATCH/);
  assert.match(staging_diag, /BACHATA_STAGING_STRICT_STREAM/);
  assert.match(staging_diag, /BACHATA_STAGING_STRICT_BUFFER_CACHE/);
  assert.match(staging_diag, /BACHATA_STAGING_TICK_LAG/);
  assert.match(staging_diag, /BACHATA_BUFFER_CACHE_TICK_LAG/);
  assert.match(staging_diag, /buffer_cache_tick_lag/);
  assert.match(staging_diag, /STAGING_DIAG_CONFIG/);
  assert.match(staging_diag, /strict_buffer_cache/);
  const buf_cache = read("src/video_core/buffer_cache/buffer_cache.cpp");
  assert.match(buf_cache, /OBTAIN_BUFFER_FOR_IMAGE/);
  assert.match(buf_cache, /UPLOAD_STAGING_ACQUIRED/);
  assert.match(buf_cache, /image_staging_ring/);
  assert.match(buf_cache, /AcquireImageStagingSlot/);
  assert.match(buf_cache, /DETILE_SOURCE_USE/);
  assert.match(buf_cache, /DETILE_SOURCE_REUSE_UNSAFE/);
  assert.match(buf_cache, /DETILE_SOURCE_WAIT/);
  assert.match(buf_cache, /EnsureDetileSourceWritable/);
  assert.match(buf_cache, /NoteDetileSourceUse/);
  assert.match(buf_cache, /bufferCacheTickLag/);
  assert.match(buf_cache, /mode=tick_lag/);
  const tex_cpp = read("src/video_core/texture_cache/texture_cache.cpp");
  assert.match(tex_cpp, /NoteDetileSourceUse/);
  const image_cpp = read("src/video_core/texture_cache/image.cpp");
  assert.match(image_cpp, /COPY_BUFFER_TO_IMAGE_PROV/);
  assert.match(track, /SUBALLOC_REUSE_UNSAFE/);
  /* Host detile push-descriptor stamp + exact source reuse wait (H0). */
  assert.match(track, /DETILE_DISPATCH/);
  assert.match(track, /detileStamp=/);
  assert.match(track, /detileSourceExactWait=/);
  assert.match(track, /detilePushStamp=1/);
  assert.match(track, /ensure_lease_for_resource_use/);
  assert.match(track, /DETILE_SOURCE_REWRITE_BLOCKED/);
  assert.match(track, /DETILE_SOURCE_TARGETED_WAIT/);
  assert.match(track, /DETILE_SOURCE_REUSE_ALLOWED/);
  assert.match(track, /DETILE_SOURCE_LAST_GPU_READ/);
  assert.match(track, /GPU_MAPPING_CREATE/);
  assert.match(track, /GPU_MAPPING_USE/);
  assert.match(track, /GPU_MAPPING_UNMAP_REQUEST/);
  assert.match(track, /GPU_MAPPING_UNMAP_COMMIT/);
  assert.match(track, /DETILE_SOURCE_GPU_MAPPING_CHANGED_IN_FLIGHT/);
  assert.match(track, /PUSH_DESCRIPTOR_BUFFERS/);
  /* Non-push detile (Mali): merge UpdateDescriptorSets by binding. */
  assert.match(track, /merge_writes/);
  assert.match(track, /desc_slot_upsert_entry/);
  assert.match(track, /onUpdateDescriptorWrites/);
  assert.match(track, /path=merge_writes/);
  assert.match(header, /VortekGpuTrack_onUpdateDescriptorWrites/);
  assert.match(header, /VortekGpuTrack_onPushDescriptorBuffers/);
  assert.match(header, /VortekGpuTrack_onCmdDispatch/);
  assert.match(header, /VortekGpuTrack_prepareResourceWrite/);
  assert.match(header, /VortekGpuTrack_detileSourceExactWaitEnabled/);
  assert.match(header, /VortekGpuTrack_detileStampEnabled/);
  assert.match(track, /detile_source_exact_wait|detileSourceExactWait/);
  assert.match(track, /debug\.bachata\.detile_stamp|BACHATA_DETILE_STAMP/);
  /* GPU mapping lifetime dig (post exact-wait zero-hit result). */
  assert.match(track, /FHD_PREPARE_WRITE_CHECK/);
  assert.match(track, /GPU_ADDRESS_BIND/);
  assert.match(track, /GPU_ADDRESS_UNBIND/);
  assert.match(track, /GPU_ADDRESS_UNBIND_IN_FLIGHT/);
  assert.match(track, /DEVICE_FAULT_INFO/);
  assert.match(track, /DEVICE_FAULT_OWNER/);
  assert.match(track, /EXTERNAL_FD_IMPORT/);
  assert.match(track, /EXTERNAL_FD_CLOSED_WHILE_BOUND/);
  assert.match(track, /EXTERNAL_BACKING_RELEASED_WHILE_BOUND/);
  assert.match(track, /GPU_BINDING_REPLACED_IN_FLIGHT/);
  assert.match(track, /DESCRIPTOR_REFERENCES_OLD_BIND_GENERATION/);
  assert.match(track, /FHD_SOURCE_PINNED_RETENTION/);
  assert.match(track, /debug\.bachata\.pin_fhd_detile_sources|BACHATA_PIN_FHD_DETILE_SOURCES/);
  assert.match(track, /pinFhdDetileSources/);
  assert.match(track, /gpuMappingLifetime=1/);
  assert.match(track, /GPU_ADDRESS_BINDING_REPORT enabled=/);
  assert.match(track, /DEVICE_FAULT_QUERY enabled=/);
  assert.match(track, /debug\.bachata\.gpu_address_binding_report|BACHATA_GPU_ADDRESS_BINDING_REPORT/);
  assert.match(track, /debug\.bachata\.fhd_prepare_write_diag|BACHATA_FHD_PREPARE_WRITE_DIAG/);
  assert.match(track, /g_gpu_address_binding_report/);
  assert.match(track, /g_fhd_prepare_write_diag/);
  assert.match(header, /VortekGpuTrack_pinFhdDetileSourcesEnabled/);
  assert.match(header, /VortekGpuTrack_gpuAddressBindingReportEnabled/);
  assert.match(header, /VortekGpuTrack_fhdPrepareWriteDiagEnabled/);
  assert.match(header, /VortekGpuTrack_deviceFaultQueryWanted/);
  assert.match(header, /VortekGpuTrack_noteAddressBinding/);
  assert.match(header, /VortekGpuTrack_registerDeviceFaultQuery/);
  assert.match(header, /VortekGpuTrack_onExternalFdEvent/);
  assert.match(header, /pin_fhd_detile_sources/);
  assert.match(header, /gpu_address_binding_report/);
  assert.match(handler, /gpuAddressBindingReportEnabled/);
  assert.match(handler, /DEVICE_ADDRESS_BINDING_REPORT|device_address_binding_report/);
  assert.match(handler, /DEVICE_FAULT_EXTENSION|device_fault/);
  assert.match(handler, /DEBUG_UTILS_EXTENSION|debug_utils/);
  assert.match(handler, /VortekGpuTrack_registerDeviceFaultQuery/);
  assert.match(handler, /prepareResourceWrite\([\s\S]*?"Copy"/);
  assert.match(handler, /GPU_ADDRESS_BINDING_REPORT enabled=/);
  assert.match(resource, /VortekGpuTrack_onExternalFdEvent/);
  const helper = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/vulkan_helper.c",
  );
  assert.match(helper, /gpuAddressBindingReportEnabled/);
  /* Deep guest pin packaging */
  const pkg = read("runtime/scripts/package-runtime.mjs");
  assert.match(pkg, /deep-guest-d45f/);
  assert.match(pkg, /GUEST_RUNTIME_BUILD/);
  assert.match(pkg, /BACHATA_DEEP_GUEST_PIN|deepGuestForceOff/);
  assert.match(pkg, /guest-runtime\.json/);
  const pin = read("runtime/pins/deep-guest-d45f/PIN.json");
  assert.match(pin, /"revision": "d45f"/);
  assert.match(pin, /"sha256": "[0-9a-f]{64}"/);
  const verifyRt = read("runtime/tests/verify-runtime.mjs");
  assert.match(verifyRt, /guest-runtime\.json/);
  assert.match(verifyRt, /optional deep guest pin|GUEST_RUNTIME_BUILD/i);
  assert.match(handler, /VortekGpuTrack_onUpdateDescriptorWrites/);
  assert.match(handler, /VortekGpuTrack_onPushDescriptorBuffers/);
  assert.match(handler, /VortekGpuTrack_onCmdDispatch/);
  assert.match(handler, /VortekGpuTrack_prepareResourceWrite/);
  assert.match(header, /quarantine_buffers/);
  assert.match(header, /quarantine_images/);
  assert.match(header, /quarantine_memory/);
  assert.match(header, /retain_unknown_buffers/);
  assert.match(header, /VortekGpuTrack_onCmdBufferRef/);
  assert.match(header, /VortekGpuTrack_onExecuteCommands/);
  assert.match(header, /VortekGpuTrack_onBufferDeviceAddress/);
  assert.match(handler, /VortekGpuTrack_onCmdBufferRef/);
  assert.match(handler, /VortekGpuTrack_onBindDescriptorSets/);
  assert.match(handler, /VortekGpuTrack_onExecuteCommands/);
  assert.match(handler, /VortekGpuTrack_onBufferDeviceAddress/);

  assert.match(track, /FREEFLIGHT_EVENT/);
  assert.match(track, /QUARANTINE_PHYSICAL_RELEASE/);
  assert.match(track, /0x1000000000000001ULL/);
  assert.match(header, /require_distinct_ahb/);
  assert.match(header, /quarantine_gpu_releases/);
  assert.match(header, /VortekGpuTrack_registerSwapchainBacking/);
  assert.match(header, /VortekGpuTrack_requireDistinctAhb/);
  assert.match(header, /VortekGpuTrack_freeflightEvent/);
  assert.match(track, /ALLOCATION_STATE/);
  assert.match(track, /externalBackingId=/);
  assert.match(track, /srcBacking=/);
  assert.match(track, /request_physical_release/);
  assert.match(header, /VORTEK_COMPLETION_INTERNAL_FENCE/);
  assert.match(header, /VortekGpuTrack_bindSubmissionCompletion/);
  assert.match(header, /VortekGpuTrack_noteFenceStatus/);
  assert.match(header, /VortekGpuTrack_fillPendingCompletionFences/);
  assert.match(header, /VORTEK_WAIT_IDLE_MAPPING/);
  assert.match(header, /VORTEK_WAIT_IDLE_IMAGE/);
  assert.match(header, /wait_idle_mapping_unmap/);
  assert.match(header, /VortekGpuTrack_notePresenterConfig/);
  assert.match(header, /VortekGpuTrack_noteAcquireResult/);
  assert.match(header, /VortekGpuTrack_notePresentComplete/);
  assert.match(header, /VortekGpuTrack_beginPresent/);
  assert.match(header, /VortekGpuTrack_noteSwapchainImageBacking/);
  assert.match(header, /wait_before_present_sem_reuse/);
  assert.match(handler, /bachata_acquire_internal_fence/);
  assert.match(handler, /VORTEK_COMPLETION_INTERNAL_FENCE/);
  assert.match(handler, /bachata_poll_completion_fences/);
  assert.match(handler, /VortekGpuTrack_noteFenceStatus/);
  assert.match(handler, /BachataXWindowSwapchain_presentWithWaits/);
  assert.match(handler, /VORTEK_WAIT_IDLE_SYNC/);
  assert.match(handler, /VORTEK_WAIT_IDLE_CMDPOOL/);
  assert.match(handler, /VORTEK_WAIT_IDLE_IMAGE/);
  assert.match(handler, /VortekGpuTrack_onDestroyImageView/);
  assert.match(handler, /VortekGpuTrack_onCommandPoolDestroy/);

  assert.match(resource, /VortekGpuTrack_onAlloc/);
  assert.match(resource, /VortekGpuTrack_onFree/);

  assert.match(handler, /VortekGpuTrack_beginSubmission/);
  assert.match(handler, /VortekGpuTrack_noteSubmitCommandBuffer/);
  assert.match(handler, /VortekGpuTrack_bindSubmissionFence/);
  assert.match(handler, /VortekGpuTrack_noteFencesWait/);
  assert.match(handler, /VortekGpuTrack_noteQueueWaitIdle/);
  assert.match(handler, /VortekGpuTrack_markAllSubmittedWorkCompleted/);
  assert.match(handler, /VortekGpuTrack_onBindBuffer/);
  assert.match(handler, /VortekGpuTrack_onCopyBufferToImage/);
  assert.match(handler, /bachata_drain_deferred_destroys/);
  assert.match(handler, /VortekGpuTrack_onDestroyBuffer\(buffer, device\)/);
  assert.match(handler, /VortekGpuTrack_onImageBarrier/);
  assert.match(handler, /WAIT_IDLE_BEFORE_DESTROY_BEGIN/);
  assert.match(handler, /WAIT_IDLE_BEFORE_DESTROY_END/);

  assert.match(swap_wsi, /VortekGpuTrack_onPresentSyncFailed/);
  assert.match(swap_wsi, /VortekGpuTrack_noteQueueWaitIdle/);
  assert.match(swap_wsi, /VortekGpuTrack_notePresent/);
  assert.match(swap_wsi, /VortekGpuTrack_notePresentComplete/);
  assert.match(swap_wsi, /present_sync_failed/);
  assert.match(swap_wsi, /bachata_consume_present_wait_semaphores/);
  assert.match(swap_wsi, /compositor_sync/);
  assert.match(swap_wsi, /BachataXWindowSwapchain_presentWithWaits/);
  assert.match(swap_wsi, /PRESENT_SEMAPHORE_REUSE_BLOCKED/);
  const swap_impl = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/xwindow_swapchain.c",
  );
  assert.match(swap_impl, /nextAcquireIndex/);
  assert.match(swap_impl, /VortekGpuTrack_noteAcquireResult/);
  assert.match(swap_impl, /VortekGpuTrack_notePresenterConfig/);
  assert.match(swap_impl, /SWAPCHAIN_IMAGE_BACKING|registerSwapchainBacking/);
  assert.match(swap_impl, /PRIVATE_AHB_CAPS/);
  assert.match(swap_impl, /DISTINCT_AHB_CONFIRMED/);
  assert.match(swap_impl, /AHardwareBuffer_allocate/);
  assert.match(swap_impl, /vkGetPhysicalDeviceImageFormatProperties2/);
  assert.match(swap_impl, /androidHardwareBufferUsage/);
  assert.match(swap_impl, /requireDistinctAhb|REQUIRE_DISTINCT/);
  assert.match(swap_impl, /blit_image_to_window/);
  // Must not hardcode image index 0 forever (previous bug).
  assert.doesNotMatch(swap_impl, /\*imageIndex = 0;\s*\n\s*return result;/);

  assert.match(cmake, /vortek_gpu_track\.c/);
  assert.match(verifier, /bachata_vortek_gpu_va_track/);
  assert.match(verifier, /DEVICE_LOST_SNAPSHOT/);

  assert.match(presenter, /DEVICE_LOST_SNAPSHOT where=GetRenderFrame/);
  assert.match(presenter, /FRAME_SLOT_ACQUIRE/);
  assert.match(presenter, /GET_RENDER_FRAME_WAIT/);
  assert.match(swapchain, /DEVICE_LOST_SNAPSHOT where=AcquireNextImage/);
  assert.match(swapchain, /DEVICE_LOST_SNAPSHOT where=QueuePresent/);
});

test("RangeFits semantics documented in tracker match investigation plan", () => {
  const track = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/vortek_gpu_track.c",
  );
  assert.match(track, /if \(bind_offset > allocation_size\)/);
  assert.match(track, /remaining_after_bind/);
  assert.match(track, /access_size <= remaining_after_bind - resource_offset/);
});

test("copy_buffer_to_image tracks resources even when size was historically zero", () => {
  const track = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/vortek_gpu_track.c",
  );
  // Record associates resources with the command buffer before submit stamps use.
  assert.match(track, /add_resource_closure\(cmd, src->id/);
  assert.match(track, /add_resource_closure\(cmd, dst->id/);
  assert.match(track, /Always associate resources with the command buffer/);
});

test("parent allocation is stamped at submit with buffer/image", () => {
  const track = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/vortek_gpu_track.c",
  );
  assert.match(track, /rec->alloc_ids\[rec->alloc_count\+\+\] = a->id/);
  assert.match(track, /a->pending_submission_refs\+\+/);
  assert.match(track, /stamp_queue_use\(a->last_use/);
});

test("completion oracle: no optimistic complete_all on unmatched fences", () => {
  const track = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/vortek_gpu_track.c",
  );
  // Unmatched wait must not infer completion.
  assert.match(track, /unmatched; no inferred completion/);
  assert.match(track, /complete_submission_record\(SubmissionRecord\* rec, CompletionSource source\)/);
  // Null-fence path uses internal fence source string.
  assert.match(track, /internal_fence/);
});

test("guest present_ready is per swapchain image index", () => {
  const swap = read("src/video_core/renderer_vulkan/vk_swapchain.cpp");
  const hdr = read("src/video_core/renderer_vulkan/vk_swapchain.h");
  assert.match(hdr, /present_ready\[image_index\]/);
  assert.match(swap, /pWaitSemaphores = &present_ready\[image_index\]/);
});

test("physical-release choke + distinct backing identities", () => {
  const track = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/vortek_gpu_track.c",
  );
  const header = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/include/vortek_gpu_track.h",
  );
  assert.match(track, /g_next_backing_id\+\+/);
  assert.match(track, /a->backing_id = g_next_backing_id\+\+/);
  assert.match(track, /gpu_va_mapping_id/);
  assert.match(track, /request_physical_release\(/);
  assert.match(header, /VORTEK_RELEASE_FREE_MEMORY/);
  assert.match(header, /VortekReleaseReason/);
});


test("Vortek GPU track: product suballoc freeflight defaults ON", () => {
  const track = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/vendor/src/vortek_gpu_track.c",
  );
  // R1 proof: in-flight range rebind wait must default ON (prop=0 still disables).
  assert.match(
    track,
    /g_wait_on_suballoc_overlap = read_bool_default\(\s*"debug\.bachata\.wait_on_suballoc_overlap"[\s\S]*?true\)/,
  );
  assert.match(
    track,
    /g_suballoc_range_pool = read_bool_default\(\s*"debug\.bachata\.suballoc_range_pool"[\s\S]*?true\)/,
  );
  assert.match(
    track,
    /g_defer_destroy = read_bool_default\(\s*"debug\.bachata\.vortek_defer_destroy"[\s\S]*?true\)/,
  );
  assert.match(track, /SUBALLOC_BIND_BLOCKED_IN_FLIGHT/);
  assert.match(track, /SUBALLOC_REUSE_ALLOWED/);
});

