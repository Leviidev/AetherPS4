#include "vortek_gpu_track.h"
#include "resource_memory.h"
#include "vortek.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* intptr_t for registerSwapchainBacking nativeHandle */
#include <stdint.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#define VT_TRACK_LOG(...) \
    __android_log_print(ANDROID_LOG_WARN, "Bachata.Vortek.GpuTrack", __VA_ARGS__)
#else
#define VT_TRACK_LOG(...)              \
    do {                               \
        fprintf(stderr, __VA_ARGS__);  \
        fputc('\n', stderr);           \
    } while (0)
#endif

/* Keep marker referenced so strip/LTO cannot drop the string. */
static const char kBachataVortekGpuVaTrackMarker[] = BACHATA_VORTEK_GPU_TRACK_MARKER;

enum {
    kMaxAllocs = 4096,
    kMaxResources = 8192,
    kMaxCmdBuffers = 512,
    kMaxRefsPerCmd = 128,
    kMaxDeferred = 1024,
    kOpRing = 64,
    kSubRing = 512,
    kMaxResPerSub = 256,
    kMaxQueues = 16,
    kMaxQueueUses = 4,
};

typedef enum TrackResourceKind {
    TRACK_RES_BUFFER = 1,
    TRACK_RES_IMAGE = 2,
    TRACK_RES_IMAGE_VIEW = 3,
} TrackResourceKind;

typedef enum DeferredKind {
    DEFER_BUFFER = 1,
    DEFER_IMAGE = 2,
    DEFER_MEMORY = 3,
    DEFER_IMAGE_VIEW = 4,
} DeferredKind;

typedef enum CompletionSource {
    COMPLETION_UNKNOWN = 0,
    COMPLETION_INTERNAL_FENCE = 1,
    COMPLETION_CALLER_FENCE = 2,
    COMPLETION_TIMELINE = 3,
    COMPLETION_QUEUE_IDLE = 4,
    COMPLETION_DEVICE_IDLE = 5,
} CompletionSource;

typedef struct QueueTracker {
    void* queue;
    uint64_t next_serial;      /* next serial to assign (1-based after first submit) */
    uint64_t completed_serial; /* highest fully completed serial on this queue */
    uint64_t last_submitted_serial;
    CompletionSource completed_source; /* proof for completed_serial */
    bool used;
} QueueTracker;

typedef struct QueueUse {
    int queue_index; /* -1 = unused */
    uint64_t serial;
} QueueUse;

typedef struct TrackedGpuAllocation {
    uint64_t id;                 /* allocation identity (never equals backing_id) */
    uint64_t backing_id;         /* distinct external/host backing identity */
    uint64_t mapping_id;         /* CPU/guest map identity (0 until mapped) */
    uint64_t gpu_va_mapping_id;  /* GPU-VA map identity (0 until mapped) */
    ResourceMemory* rm;
    void* memory_handle;
    void* device;
    uint64_t allocation_size;
    uint32_t memory_type_index;
    uint64_t guest_va;
    uint64_t guest_size;
    uint64_t gpu_va;
    uint64_t gpu_size;
    bool mapped;
    bool alive;
    bool destroy_requested;
    bool free_requested;
    bool unmap_requested;
    bool actually_destroyed;
    bool physically_freed;
    bool external_backing;
    bool backing_release_requested;
    bool backing_released;
    bool mapping_unmap_requested;
    bool mapping_unmapped;
    bool gpu_va_unmapped;
    char backing_type[16];
    uint64_t created_submission;
    uint64_t last_submitted_use; /* global id for logs */
    uint64_t last_completed_use;
    CompletionSource last_completion_source;
    uint32_t pending_submission_refs;
    uint32_t live_child_objects;
    uint32_t pending_child_objects;
    QueueUse last_use[kMaxQueueUses];
    uint64_t last_cpu_write;
    uint64_t last_gpu_read_submission;
    uint64_t last_gpu_write_submission;
    uint64_t destroy_requested_submission;
    uint64_t actual_free_submission;
    bool retire_sticky_block;
    uint64_t last_retire_attempt_completed;
    uint32_t last_retire_attempt_refs;
    /* GPU address binding / external FD lifetime (mapping dig). */
    uint64_t bind_generation;       /* increments on each memory bind replace */
    int original_fd;                /* first imported/export FD */
    int owned_fd;                   /* current owned FD (-1 if none) */
    bool fd_closed;
    uint64_t gpu_bind_address;      /* last reported GPU base (0 if unknown) */
    uint64_t gpu_bind_size;
    bool gpu_address_bound;
    uint64_t bind_gen_at_gpu_bind;  /* generation when GPU address bound */
    bool fhd_pinned;                /* pin_fhd_detile_sources retention */
    void* last_vk_memory;           /* detect memory handle replacement */
} TrackedGpuAllocation;

typedef struct TrackedResource {
    uint64_t id;
    void* handle;
    void* device;
    TrackResourceKind kind;
    bool alive;
    bool guest_visible;
    bool destroy_requested;
    bool actually_destroyed;
    bool physically_released;
    uint64_t allocation_id; /* parent allocation */
    uint64_t external_backing_id; /* distinct backing id, never collapse to alloc id */
    uint64_t mapping_id;          /* parent mapping if any */
    uint64_t gpu_va_mapping_id;
    uint64_t parent_image_id;     /* for image views */
    ResourceMemory* rm;
    uint64_t bind_offset;
    uint64_t requirements_size;
    uint64_t requirements_alignment;
    uint64_t buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t usage;
    uint32_t current_layout;
    uint64_t last_submitted_use;
    uint64_t last_completed_use;
    CompletionSource last_completion_source;
    uint32_t pending_submission_refs;
    QueueUse last_use[kMaxQueueUses];
    uint64_t last_gpu_read_submission;
    uint64_t last_gpu_write_submission;
    uint64_t destroy_requested_submission;
    char last_op[24];
    char creation_site[24];
    bool bda_enabled;
    bool unknown_use_retained;
    bool retire_sticky_block; /* quarantine / unknown-use: do not thrash collect */
    uint64_t last_retire_attempt_completed;
    uint32_t last_retire_attempt_refs;
    uint64_t bda_address;
    uint64_t bda_size;
    uint64_t bind_generation; /* live memory bind generation at last bind */
} TrackedResource;

enum { kMaxPushBindings = 8, kMaxBoundSetsPerCmd = 8 };
typedef struct CmdPushBinding {
    void* buffer;
    uint64_t offset;
    uint64_t range;
    uint32_t binding;
    uint32_t descriptor_type;
    uint8_t is_write;
} CmdPushBinding;

typedef struct TrackedCommandBuffer {
    void* handle;
    bool alive;
    uint64_t resource_ids[kMaxRefsPerCmd];
    uint8_t resource_writes[kMaxRefsPerCmd];
    int resource_count;
    /* Last push-descriptor snapshot for this cmd (detile uses binding 0/1 storage). */
    CmdPushBinding push_bindings[kMaxPushBindings];
    int push_binding_count;
    /* Last bound descriptor sets (non-push path when VK_KHR_push_descriptor missing). */
    void* bound_sets[kMaxBoundSetsPerCmd];
    int bound_set_count;
} TrackedCommandBuffer;

typedef struct DeferredDestroy {
    bool active;
    DeferredKind kind;
    uint64_t resource_or_alloc_id;
    void* handle;
    void* device;
    ResourceMemory* rm;
} DeferredDestroy;

typedef struct GpuOp {
    uint64_t submission;
    char operation[32];
    uint64_t src_resource;
    uint64_t src_offset;
    uint64_t src_size;
    uint64_t dst_resource;
    uint64_t dst_offset;
    uint64_t dst_size;
    uint32_t width;
    uint32_t height;
    int tiling_mode;
    bool range_invalid;
    bool completed;
} GpuOp;

typedef struct SubmissionRecord {
    uint64_t id; /* global monotonic for logs */
    int queue_index;
    uint64_t queue_serial;
    void* fence;                 /* caller fence if any */
    void* completion_fence;      /* internal or caller proof fence */
    CompletionSource completion_source;
    bool active;
    bool completed;
    bool completion_verified;
    uint64_t resource_ids[kMaxResPerSub];
    uint64_t alloc_ids[kMaxResPerSub];
    int resource_count;
    int alloc_count;
} SubmissionRecord;

typedef struct DeferredTake {
    DeferredKind kind;
    void* handle;
    void* device;
    ResourceMemory* rm;
} DeferredTake;

/* Non-overlapping ID spaces so logs never alias allocation vs backing vs mapping. */
static uint64_t g_next_alloc_id = 1;                    /* 0x0000_0000_0000_0001+ */
static uint64_t g_next_resource_id = 1;
static uint64_t g_next_backing_id = 0x1000000000000001ULL; /* 0x1xxx... */
static uint64_t g_next_mapping_id = 0x2000000000000001ULL; /* 0x2xxx... */
static uint64_t g_next_gpu_va_mapping_id = 0x3000000000000001ULL; /* 0x3xxx... */
static uint64_t g_last_submitted = 0;
static uint64_t g_last_completed = 0;
static uint64_t g_current_pending_submission = 0;
static int g_current_queue_index = -1;
static uint64_t g_current_queue_serial = 0;
static bool g_device_lost_dumped = false;
static bool g_inited = false;
static bool g_defer_destroy = false;
static bool g_wait_idle_destroy = false;
static bool g_wait_idle_buffer = false;
static bool g_wait_idle_memory = false;
static bool g_wait_idle_external = false;
static bool g_wait_idle_sync = false;
static bool g_wait_idle_cmdpool = false;
static bool g_wait_idle_mapping = false;
static bool g_wait_idle_image = false;
/* Narrow present-ownership diagnostic toggles (prefer over full D3). */
static bool g_wait_before_present_sem_reuse = false;
static bool g_wait_before_same_image_reuse = false;
static bool g_wait_before_same_ahb_reuse = false;
static bool g_wait_for_ahb_release_fence = false;
static bool g_wait_on_suballoc_overlap = false;
static bool g_suballoc_range_pool = false;
/* Both default OFF — deep run reached first stage without host detile stamp.
 * Stamp/ensure_lease on FHD made SEGA stick + early DEVICE_LOST (~20s).
 * Enable dig: debug.bachata.detile_stamp=1 (and exact_wait=1 only after stamp works). */
static bool g_detile_stamp = false;
static bool g_detile_source_exact_wait = false;
static bool g_pin_fhd_detile_sources = false;
/* Address-binding report default OFF (M1c storm regressed SEGA survival). */
static bool g_gpu_address_binding_report = false;
/* FHD prepare-write check logs default ON; set prop 0 for M1e. */
static bool g_fhd_prepare_write_diag = true;
static bool g_device_fault_query_wanted = true;
/* Device fault / address-binding dig state. */
static void* g_fault_device = NULL;
typedef VkResult (*VortekGetDeviceFaultInfoFn)(void* device, void* counts, void* info);
static VortekGetDeviceFaultInfoFn g_get_device_fault = NULL;
enum { kMaxGpuAddrBinds = 256 };
typedef struct GpuAddrBindEntry {
    bool used;
    bool bound;
    uint64_t base;
    uint64_t size;
    uint64_t allocation_id;
    uint64_t resource_id;
    uint64_t generation;
    uint64_t last_gpu_read;
    uint64_t unbound_at_completed;
    uint32_t flags;
} GpuAddrBindEntry;
static GpuAddrBindEntry g_gpu_addr_binds[kMaxGpuAddrBinds];
static int g_gpu_addr_bind_count = 0;
static int g_gpu_addr_bind_write = 0;
static int g_suballoc_pool_initial_slots = 4;
static int g_suballoc_pool_max_slots = 8;
static uint64_t g_suballoc_pool_max_bytes = 96ull * 1024ull * 1024ull;
static bool g_require_distinct_ahb = false;
static bool g_quarantine_gpu_releases = false;
static bool g_quarantine_buffers = false;
static bool g_quarantine_images = false;
static bool g_quarantine_memory = false;

/* Stashed by onBindBuffer / acquire when overlap needs a host wait. */
enum { kMaxSuballocWaitFences = 32 };
static struct {
    bool pending;
    uint64_t allocation_id;
    uint64_t bind_offset;
    uint64_t size;
    uint64_t new_resource_id;
    uint64_t generation;
    uint64_t max_overlap_use; /* global submission id */
    int queue_index;
    uint64_t queue_serial;
    int overlap_count;
    int fence_count;
    void* fences[kMaxSuballocWaitFences];
    uint64_t max_qserial_per_queue[kMaxQueues];
    int reason; /* 0=bind 1=cpu_write 2=pool_fallback */
} g_suballoc_wait;

/* R2: generation-based range leases (independent of buffer object lifetime). */
enum {
    kMaxSuballocLeases = 512,
    kSubLeaseCpuOwned = 1,
    kSubLeaseSubmitted = 2,
    kSubLeaseGpuInFlight = 3,
    kSubLeaseReusable = 4,
};
typedef struct SuballocLease {
    bool used;
    uint64_t allocation_id;
    uint64_t offset;
    uint64_t size;
    uint64_t generation;
    uint64_t owner_resource;
    int state;
    QueueUse last_read;
    QueueUse last_write;
    uint64_t last_read_global;
    uint64_t last_write_global;
    bool cpu_write_owner;
    int pool_slot; /* -1 = guest range; >=0 = physical pool redirect index */
} SuballocLease;
static SuballocLease g_leases[kMaxSuballocLeases];
static int g_lease_count = 0;
static uint64_t g_next_lease_generation = 1;

/* Durable serial→fence map: survives g_subs ring wrap (fixes R1 94% QueueWaitIdle). */
enum { kMaxDurableFences = 1024 };
typedef struct DurableFence {
    bool used;
    bool completed;
    uint64_t global_id;
    int queue_index;
    uint64_t queue_serial;
    void* fence;
    CompletionSource source;
} DurableFence;
static DurableFence g_durable_fences[kMaxDurableFences];

/* R2 aggregate counters (SUBALLOC_POOL_STATS every 5s). */
static uint64_t g_sub_stats_busy_skipped = 0;
static uint64_t g_sub_stats_pool_grows = 0;
static uint64_t g_sub_stats_exact_waits = 0;
static uint64_t g_sub_stats_queue_idle_fallbacks = 0;
static uint64_t g_sub_stats_range_acquired = 0;
static uint64_t g_sub_stats_cpu_blocked = 0;
static uint64_t g_sub_stats_stale_completion = 0;
static uint64_t g_sub_stats_gen_mismatch = 0;
static uint64_t g_sub_stats_pool_bytes = 0;
static int g_sub_stats_pool_slots = 0;
static uint64_t g_sub_stats_last_log_ms = 0;
static bool g_retain_unknown_buffers = false;
static bool g_first_frame_seen = false;
static uint64_t g_quarantine_block_count = 0;
static uint64_t g_quarantine_log_count = 0;
static uint64_t g_unknown_buffer_retain_count = 0;
static uint64_t g_unknown_buffer_retain_bytes = 0;
static uint64_t g_buffer_retire_commit_count = 0;
static uint64_t g_buffer_retire_block_count = 0;
static uint64_t g_direct_destroy_bypass_count = 0;
static uint64_t g_last_retention_summary_ms = 0;
static uint64_t g_phys_rel_log_count = 0;
static CompletionSource g_last_completion_source = COMPLETION_UNKNOWN;

enum { kFreeflightRing = 256 };
typedef struct FreeflightEvent {
    uint64_t seq;
    uint64_t ts_ms;
    char tag[28];
    void* queue;
    uint64_t submission;
    void* resource;
    uint64_t allocation;
    uint64_t backing;
    uint64_t mapping;
    uint64_t last_use;
    uint64_t completed;
    uint32_t pending_refs;
    int logical_destroy;
    int physical_destroy;
} FreeflightEvent;
static FreeflightEvent g_freeflight[kFreeflightRing];
static int g_freeflight_write = 0;
static int g_freeflight_count = 0;
static uint64_t g_freeflight_seq = 0;
static void* g_last_queue = NULL;
static uint64_t g_last_present_id = 0;
static int g_retire_queue_depth = 0;
static int g_retire_queue_max = 0;
static uint64_t g_retire_request_count = 0;
static uint64_t g_retire_commit_count = 0;

enum { kMaxDescSets = 2048, kMaxBuffersPerDesc = 64, kMaxBda = 1024 };
/* Per-binding entry: UpdateDescriptorSets issues one write per binding; must MERGE
 * into the set snapshot (replace wiped detile src/dst leaving only last UBO). */
typedef struct DescBufEntry {
    void* buffer;
    uint64_t offset;
    uint64_t range;
    uint32_t binding;
    uint32_t descriptor_type;
    uint64_t resource_id;
    uint64_t bind_generation; /* snapshot of buffer bind gen at update */
} DescBufEntry;
typedef struct DescSetSnapshot {
    void* set;
    bool used;
    DescBufEntry entries[kMaxBuffersPerDesc];
    int buffer_count;
} DescSetSnapshot;
static DescSetSnapshot g_desc_sets[kMaxDescSets];

typedef struct BdaEntry {
    void* buffer;
    uint64_t address;
    uint64_t size;
    bool used;
} BdaEntry;
static BdaEntry g_bda[kMaxBda];
static int g_bda_count = 0;

enum { kMaxPresentSems = 64, kMaxPresentImages = 16, kPhysRelRing = 64, kAcquireRing = 64 };
typedef struct PresentSemTrack {
    void* semaphore;
    uint32_t image_index;
    uint64_t present_id;
    bool in_flight;
    bool present_pending;
} PresentSemTrack;
static PresentSemTrack g_present_sems[kMaxPresentSems];
static int g_present_sem_count = 0;

/* AVAILABLE=0 ACQUIRED=1 RENDER_SUBMITTED=2 PRESENT_PENDING=3 */
typedef enum PresentImageLife {
    PRESENT_LIFE_AVAILABLE = 0,
    PRESENT_LIFE_ACQUIRED = 1,
    PRESENT_LIFE_RENDER_SUBMITTED = 2,
    PRESENT_LIFE_PRESENT_PENDING = 3,
} PresentImageLife;

typedef struct PresentImageState {
    bool used;
    uint32_t image_index;
    void* image;
    void* ahb;
    void* memory;
    void* gpu_mapping;
    void* acquire_semaphore;
    void* render_finished; /* present wait semaphore owner (per-image) */
    uint64_t acquire_generation;
    uint64_t render_submission;
    uint64_t present_id;
    uint64_t last_acquire_id;
    PresentImageLife life;
    bool present_pending;
    bool acquired;
    bool render_pending;
} PresentImageState;
static PresentImageState g_present_images[kMaxPresentImages];

enum { kMaxSyncFds = 32 };
typedef struct SyncFdTrack {
    int fd;
    char owner[32];
    uint32_t image_index;
    uint64_t present_id;
    bool imported;
    bool waited;
    bool closed;
    bool used;
} SyncFdTrack;
static SyncFdTrack g_sync_fds[kMaxSyncFds];
static int g_sync_fd_count = 0;
static void* g_shared_ahb = NULL;
static int g_shared_ahb_image_count = 0;

static uint32_t g_presenter_swapchain_image_count = 0;
static uint32_t g_presenter_internal_image_count = 0;
static uint32_t g_presenter_frame_slot_count = 0;
static uint32_t g_presenter_present_qfamily = 0;
static uint32_t g_presenter_graphics_qfamily = 0;
static void* g_presenter_swapchain_hint = NULL;
static uint64_t g_acquire_call_id = 0;
static uint32_t g_last_acquired_image_index = 0;

typedef struct PhysRelEvent {
    uint64_t seq;
    int kind; /* VortekReleaseReason */
    uint64_t id;
    void* handle;
    int allowed; /* 1 commit, 0 blocked */
    char tag[24];
} PhysRelEvent;
static PhysRelEvent g_phys_rel_ring[kPhysRelRing];
static int g_phys_rel_write = 0;
static int g_phys_rel_count = 0;
static uint64_t g_phys_rel_seq = 0;

typedef struct AcquireEvent {
    uint64_t call_id;
    int result;
    uint32_t image_index;
    uint64_t prev_present_id;
    void* acquire_semaphore;
} AcquireEvent;
static AcquireEvent g_acquire_ring[kAcquireRing];
static int g_acquire_write = 0;
static int g_acquire_count = 0;

static QueueTracker g_queues[kMaxQueues];
static int g_queue_count = 0;

static TrackedGpuAllocation g_allocs[kMaxAllocs];
static int g_alloc_count = 0;
static TrackedResource g_resources[kMaxResources];
static int g_resource_count = 0;
static TrackedCommandBuffer g_cmds[kMaxCmdBuffers];
static int g_cmd_count = 0;
static DeferredDestroy g_deferred[kMaxDeferred];
static GpuOp g_ops[kOpRing];
static int g_op_write = 0;
static int g_op_count = 0;
static SubmissionRecord g_subs[kSubRing];
static DeferredTake g_ready_destroy[kMaxDeferred];
static int g_ready_destroy_count = 0;

static bool env_truthy(const char* v) {
    return v && v[0] && v[0] != '0' && v[0] != 'f' && v[0] != 'F' && v[0] != 'n' &&
           v[0] != 'N';
}

/* First 20 always; then every 100th. Violations/device-loss bypass this. */
static bool rate_allow(uint64_t* counter) {
    if (!counter) {
        return true;
    }
    (*counter)++;
    if (*counter <= 20) {
        return true;
    }
    return ((*counter) % 100ull) == 0;
}

static bool reason_is_buffer(int reason) {
    return reason == VORTEK_RELEASE_DESTROY_BUFFER;
}
static bool reason_is_image(int reason) {
    return reason == VORTEK_RELEASE_DESTROY_IMAGE || reason == VORTEK_RELEASE_IMAGE_VIEW;
}

static bool quarantine_reason_enabled(int reason) {
    if (g_quarantine_gpu_releases) {
        return true;
    }
    if (reason_is_buffer(reason) && g_quarantine_buffers) {
        return true;
    }
    if (reason_is_image(reason) && g_quarantine_images) {
        return true;
    }
    if ((reason == VORTEK_RELEASE_FREE_MEMORY || reason == VORTEK_RELEASE_UNMAP_MEMORY ||
         reason == VORTEK_RELEASE_EXTERNAL || reason == VORTEK_RELEASE_GPU_VA_UNMAP) &&
        g_quarantine_memory) {
        return true;
    }
    return false;
}

static const char* quarantine_class_str(int reason) {
    if (reason_is_buffer(reason)) {
        return "buffers";
    }
    if (reason_is_image(reason)) {
        return "images";
    }
    if (reason == VORTEK_RELEASE_FREE_MEMORY || reason == VORTEK_RELEASE_UNMAP_MEMORY ||
        reason == VORTEK_RELEASE_EXTERNAL || reason == VORTEK_RELEASE_GPU_VA_UNMAP) {
        return "memory";
    }
    return "other";
}

static bool read_android_prop(const char* key) {
#ifdef __ANDROID__
    char buf[PROP_VALUE_MAX];
    buf[0] = '\0';
    if (__system_property_get(key, buf) > 0) {
        return env_truthy(buf);
    }
#else
    (void)key;
#endif
    return false;
}

static int read_android_prop_int(const char* key, int default_value) {
#ifdef __ANDROID__
    char buf[PROP_VALUE_MAX];
    buf[0] = '\0';
    if (__system_property_get(key, buf) > 0 && buf[0] != '\0') {
        return atoi(buf);
    }
#else
    (void)key;
#endif
    return default_value;
}

/* Returns default_value when key unset; otherwise env_truthy of prop/env string. */
static bool read_bool_default(const char* prop_key, const char* env_key, bool default_value) {
    if (env_key) {
        const char* e = getenv(env_key);
        if (e && e[0] != '\0') {
            return env_truthy(e);
        }
    }
#ifdef __ANDROID__
    if (prop_key) {
        char buf[PROP_VALUE_MAX];
        buf[0] = '\0';
        if (__system_property_get(prop_key, buf) > 0 && buf[0] != '\0') {
            return env_truthy(buf);
        }
    }
#else
    (void)prop_key;
#endif
    return default_value;
}

static bool ranges_overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    return !(a1 <= b0 || a0 >= b1);
}

static bool queue_use_completed(const QueueUse* u) {
    if (!u || u->queue_index < 0 || u->serial == 0) {
        return true;
    }
    if (u->queue_index >= kMaxQueues || !g_queues[u->queue_index].used) {
        return true;
    }
    return g_queues[u->queue_index].completed_serial >= u->serial;
}

static bool lease_gpu_uses_completed(const SuballocLease* L) {
    if (!L) {
        return true;
    }
    return queue_use_completed(&L->last_read) && queue_use_completed(&L->last_write);
}

static void durable_fence_register(uint64_t global_id, int queue_index, uint64_t queue_serial,
                                   void* fence, CompletionSource source) {
    if (!fence || !global_id) {
        return;
    }
    /* Fence handle reuse: drop stale durable rows still pointing at this fence.
     * Completing a recycled fence must not advance old serials. */
    for (int i = 0; i < kMaxDurableFences; ++i) {
        if (g_durable_fences[i].used && !g_durable_fences[i].completed &&
            g_durable_fences[i].fence == fence && g_durable_fences[i].global_id != global_id) {
            g_durable_fences[i].fence = NULL; /* keep row until true completion or overwrite */
        }
    }
    /* Update existing by global submission id. */
    for (int i = 0; i < kMaxDurableFences; ++i) {
        if (g_durable_fences[i].used && g_durable_fences[i].global_id == global_id) {
            g_durable_fences[i].fence = fence;
            g_durable_fences[i].queue_index = queue_index;
            g_durable_fences[i].queue_serial = queue_serial;
            g_durable_fences[i].source = source;
            g_durable_fences[i].completed = false;
            return;
        }
    }
    /* Free slot. */
    for (int i = 0; i < kMaxDurableFences; ++i) {
        if (!g_durable_fences[i].used || g_durable_fences[i].completed) {
            g_durable_fences[i].used = true;
            g_durable_fences[i].completed = false;
            g_durable_fences[i].global_id = global_id;
            g_durable_fences[i].queue_index = queue_index;
            g_durable_fences[i].queue_serial = queue_serial;
            g_durable_fences[i].fence = fence;
            g_durable_fences[i].source = source;
            return;
        }
    }
    VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=durableFence");
}

static void durable_fence_complete_by_fence(void* fence, CompletionSource source) {
    if (!fence) {
        return;
    }
    for (int i = 0; i < kMaxDurableFences; ++i) {
        DurableFence* d = &g_durable_fences[i];
        if (!d->used || d->completed || d->fence != fence) {
            continue;
        }
        d->completed = true;
        if (d->queue_index >= 0 && d->queue_index < kMaxQueues && g_queues[d->queue_index].used &&
            d->queue_serial > g_queues[d->queue_index].completed_serial &&
            source != COMPLETION_UNKNOWN) {
            /* complete_queue_up_to will also walk g_subs; keep watermark consistent. */
            if (d->queue_serial > g_queues[d->queue_index].completed_serial) {
                /* Handled by complete_submission_record path; just mark durable. */
            }
        }
        (void)source;
    }
}

static void durable_fence_complete_global(uint64_t global_id) {
    for (int i = 0; i < kMaxDurableFences; ++i) {
        if (g_durable_fences[i].used && g_durable_fences[i].global_id == global_id) {
            g_durable_fences[i].completed = true;
        }
    }
}

static int collect_durable_fences_for_need(int queue_index, uint64_t need_serial, void** outFences,
                                           int maxCount) {
    if (!outFences || maxCount <= 0 || need_serial == 0) {
        return 0;
    }
    int n = 0;
    /* Prefer exact serial, then any serial >= need on same queue (queue order). */
    void* exact = NULL;
    void* later = NULL;
    uint64_t later_serial = UINT64_MAX;
    for (int i = 0; i < kMaxDurableFences; ++i) {
        DurableFence* d = &g_durable_fences[i];
        if (!d->used || d->completed || !d->fence) {
            continue;
        }
        if (queue_index >= 0 && d->queue_index != queue_index) {
            continue;
        }
        if (d->queue_serial == need_serial) {
            exact = d->fence;
        } else if (d->queue_serial >= need_serial && d->queue_serial < later_serial) {
            later = d->fence;
            later_serial = d->queue_serial;
        }
    }
    if (exact && n < maxCount) {
        outFences[n++] = exact;
    }
    if (later && later != exact && n < maxCount) {
        outFences[n++] = later;
    }
    /* Also any incomplete durable on that queue with serial <= need (covers lagging). */
    for (int i = 0; i < kMaxDurableFences && n < maxCount; ++i) {
        DurableFence* d = &g_durable_fences[i];
        if (!d->used || d->completed || !d->fence) {
            continue;
        }
        if (queue_index >= 0 && d->queue_index != queue_index) {
            continue;
        }
        if (d->queue_serial == 0 || d->queue_serial > need_serial) {
            continue;
        }
        int dup = 0;
        for (int j = 0; j < n; ++j) {
            if (outFences[j] == d->fence) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            outFences[n++] = d->fence;
        }
    }
    return n;
}

static SuballocLease* find_lease_for_range(uint64_t allocation_id, uint64_t offset, uint64_t size) {
    for (int i = 0; i < kMaxSuballocLeases; ++i) {
        SuballocLease* L = &g_leases[i];
        if (!L->used) {
            continue;
        }
        if (L->allocation_id == allocation_id && L->offset == offset && L->size == size) {
            return L;
        }
    }
    return NULL;
}

static SuballocLease* alloc_lease_slot(void) {
    for (int i = 0; i < kMaxSuballocLeases; ++i) {
        if (!g_leases[i].used || g_leases[i].state == kSubLeaseReusable) {
            if (!g_leases[i].used) {
                g_lease_count++;
            }
            memset(&g_leases[i], 0, sizeof(g_leases[i]));
            g_leases[i].used = true;
            g_leases[i].pool_slot = -1;
            return &g_leases[i];
        }
    }
    return NULL;
}

static void stamp_lease_queue_use(QueueUse* u, int qi, uint64_t serial) {
    if (!u || qi < 0 || serial == 0) {
        return;
    }
    if (u->queue_index == qi) {
        if (serial > u->serial) {
            u->serial = serial;
        }
        return;
    }
    if (u->queue_index < 0 || u->serial == 0) {
        u->queue_index = qi;
        u->serial = serial;
    } else if (serial >= u->serial) {
        u->queue_index = qi;
        u->serial = serial;
    }
}

static bool lease_incomplete_overlap(uint64_t allocation_id, uint64_t offset, uint64_t size,
                                     uint64_t* out_max_global, int* out_qi, uint64_t* out_qserial,
                                     uint64_t max_qserial_per_queue[kMaxQueues], int* out_count) {
    int count = 0;
    uint64_t max_global = 0;
    int worst_qi = -1;
    uint64_t worst_serial = 0;
    if (max_qserial_per_queue) {
        memset(max_qserial_per_queue, 0, sizeof(uint64_t) * kMaxQueues);
    }
    const uint64_t end = offset + size;
    for (int i = 0; i < kMaxSuballocLeases; ++i) {
        SuballocLease* L = &g_leases[i];
        if (!L->used || L->state == kSubLeaseReusable) {
            continue;
        }
        if (L->allocation_id != allocation_id) {
            continue;
        }
        if (!ranges_overlap(offset, end, L->offset, L->offset + L->size)) {
            continue;
        }
        if (lease_gpu_uses_completed(L) && !L->cpu_write_owner &&
            L->state != kSubLeaseGpuInFlight && L->state != kSubLeaseSubmitted &&
            L->state != kSubLeaseCpuOwned) {
            continue;
        }
        /* Still incomplete if any use not done or CPU owns. */
        const bool incomplete =
            L->cpu_write_owner || !lease_gpu_uses_completed(L) ||
            L->state == kSubLeaseGpuInFlight || L->state == kSubLeaseSubmitted ||
            L->state == kSubLeaseCpuOwned;
        if (!incomplete) {
            if (L->state != kSubLeaseReusable) {
                L->state = kSubLeaseReusable;
                VT_TRACK_LOG(
                    "SUBALLOC_RANGE_REUSABLE allocation=%" PRIu64 " offset=0x%" PRIx64
                    " size=0x%" PRIx64 " generation=%" PRIu64 " owner=%" PRIu64,
                    L->allocation_id, L->offset, L->size, L->generation, L->owner_resource);
            }
            continue;
        }
        count++;
        uint64_t gmax = L->last_write_global > L->last_read_global ? L->last_write_global
                                                                   : L->last_read_global;
        if (gmax > max_global) {
            max_global = gmax;
        }
        if (L->last_write.queue_index >= 0 && L->last_write.serial > 0) {
            const int qi = L->last_write.queue_index;
            if (max_qserial_per_queue && qi < kMaxQueues &&
                L->last_write.serial > max_qserial_per_queue[qi]) {
                max_qserial_per_queue[qi] = L->last_write.serial;
            }
            if (L->last_write.serial > worst_serial) {
                worst_serial = L->last_write.serial;
                worst_qi = qi;
            }
        }
        if (L->last_read.queue_index >= 0 && L->last_read.serial > 0) {
            const int qi = L->last_read.queue_index;
            if (max_qserial_per_queue && qi < kMaxQueues &&
                L->last_read.serial > max_qserial_per_queue[qi]) {
                max_qserial_per_queue[qi] = L->last_read.serial;
            }
            if (L->last_read.serial > worst_serial) {
                worst_serial = L->last_read.serial;
                worst_qi = qi;
            }
        }
    }
    if (out_count) {
        *out_count = count;
    }
    if (out_max_global) {
        *out_max_global = max_global;
    }
    if (out_qi) {
        *out_qi = worst_qi;
    }
    if (out_qserial) {
        *out_qserial = worst_serial;
    }
    return count > 0;
}

static SuballocLease* begin_new_generation(uint64_t allocation_id, uint64_t offset, uint64_t size,
                                           uint64_t owner_resource) {
    SuballocLease* L = find_lease_for_range(allocation_id, offset, size);
    if (!L) {
        L = alloc_lease_slot();
    }
    if (!L) {
        VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=suballocLease");
        return NULL;
    }
    L->used = true;
    L->allocation_id = allocation_id;
    L->offset = offset;
    L->size = size;
    L->generation = g_next_lease_generation++;
    L->owner_resource = owner_resource;
    L->state = kSubLeaseCpuOwned;
    L->last_read.queue_index = -1;
    L->last_read.serial = 0;
    L->last_write.queue_index = -1;
    L->last_write.serial = 0;
    L->last_read_global = 0;
    L->last_write_global = 0;
    L->cpu_write_owner = true;
    L->pool_slot = -1;
    g_sub_stats_range_acquired++;
    VT_TRACK_LOG(
        "SUBALLOC_RANGE_ACQUIRED allocation=%" PRIu64 " offset=0x%" PRIx64 " size=0x%" PRIx64
        " generation=%" PRIu64 " owner=%" PRIu64 " state=CpuOwned",
        allocation_id, offset, size, L->generation, owner_resource);
    return L;
}

/* Ensure a range lease exists so detile GPU reads stamp last_read (not only CPU-acquired ranges). */
static SuballocLease* ensure_lease_for_resource_use(TrackedResource* r) {
    if (!r || !r->allocation_id) {
        return NULL;
    }
    const uint64_t rsz =
        r->buffer_size > 0 ? r->buffer_size
                           : (r->requirements_size > 0 ? r->requirements_size : 0);
    if (rsz == 0) {
        return NULL;
    }
    SuballocLease* L = find_lease_for_range(r->allocation_id, r->bind_offset, rsz);
    if (L) {
        if (L->owner_resource == 0) {
            L->owner_resource = r->id;
        }
        return L;
    }
    L = alloc_lease_slot();
    if (!L) {
        VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=suballocLeaseDetile");
        return NULL;
    }
    L->used = true;
    L->allocation_id = r->allocation_id;
    L->offset = r->bind_offset;
    L->size = rsz;
    L->generation = g_next_lease_generation++;
    L->owner_resource = r->id;
    L->state = kSubLeaseSubmitted;
    L->last_read.queue_index = -1;
    L->last_read.serial = 0;
    L->last_write.queue_index = -1;
    L->last_write.serial = 0;
    L->last_read_global = 0;
    L->last_write_global = 0;
    L->cpu_write_owner = false;
    L->pool_slot = -1;
    g_sub_stats_range_acquired++;
    VT_TRACK_LOG(
        "SUBALLOC_RANGE_ACQUIRED allocation=%" PRIu64 " offset=0x%" PRIx64 " size=0x%" PRIx64
        " generation=%" PRIu64 " owner=%" PRIu64 " state=GpuUseEnsure path=detile_stamp",
        L->allocation_id, L->offset, L->size, L->generation, L->owner_resource);
    return L;
}

/* FHD-class buffer size used for focused mapping/reuse logs (matches guest kFullResStagingBytes). */
static bool is_fhd_class_size(uint64_t size) {
    return size >= 0x700000ull;
}

static bool resource_gpu_read_incomplete(const TrackedResource* r) {
    if (!r) {
        return false;
    }
    /* Prefer explicit lastGpuRead; also block if last submitted use not completed. */
    if (r->last_gpu_read_submission > g_last_completed) {
        return true;
    }
    if (r->last_submitted_use > g_last_completed && r->pending_submission_refs > 0) {
        return true;
    }
    return false;
}

static void stash_suballoc_wait(uint64_t allocation_id, uint64_t offset, uint64_t size,
                                uint64_t new_resource_id, uint64_t generation,
                                uint64_t max_overlap_use, int worst_qi, uint64_t worst_qserial,
                                int overlap_count, uint64_t max_qserial_per_queue[kMaxQueues],
                                int reason) {
    memset(&g_suballoc_wait, 0, sizeof(g_suballoc_wait));
    g_suballoc_wait.pending = true;
    g_suballoc_wait.allocation_id = allocation_id;
    g_suballoc_wait.bind_offset = offset;
    g_suballoc_wait.size = size;
    g_suballoc_wait.new_resource_id = new_resource_id;
    g_suballoc_wait.generation = generation;
    g_suballoc_wait.max_overlap_use = max_overlap_use;
    g_suballoc_wait.queue_index = worst_qi;
    g_suballoc_wait.queue_serial = worst_qserial;
    g_suballoc_wait.overlap_count = overlap_count;
    g_suballoc_wait.reason = reason;
    if (max_qserial_per_queue) {
        memcpy(g_suballoc_wait.max_qserial_per_queue, max_qserial_per_queue,
               sizeof(g_suballoc_wait.max_qserial_per_queue));
    }
}

static int find_or_create_queue(void* queue) {
    if (!queue) {
        return -1;
    }
    for (int i = 0; i < g_queue_count; ++i) {
        if (g_queues[i].used && g_queues[i].queue == queue) {
            return i;
        }
    }
    if (g_queue_count >= kMaxQueues) {
        /* Reuse first slot mapping for overflow (log once-ish). */
        VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=queue count=%d", g_queue_count);
        for (int i = 0; i < kMaxQueues; ++i) {
            if (!g_queues[i].used) {
                g_queues[i].used = true;
                g_queues[i].queue = queue;
                return i;
            }
        }
        g_queues[0].queue = queue;
        return 0;
    }
    int idx = g_queue_count++;
    memset(&g_queues[idx], 0, sizeof(g_queues[idx]));
    g_queues[idx].queue = queue;
    g_queues[idx].used = true;
    return idx;
}

static void stamp_queue_use(QueueUse* uses, int queue_index, uint64_t serial) {
    if (queue_index < 0 || serial == 0) {
        return;
    }
    for (int i = 0; i < kMaxQueueUses; ++i) {
        if (uses[i].queue_index == queue_index) {
            if (serial > uses[i].serial) {
                uses[i].serial = serial;
            }
            return;
        }
    }
    for (int i = 0; i < kMaxQueueUses; ++i) {
        if (uses[i].queue_index < 0) {
            uses[i].queue_index = queue_index;
            uses[i].serial = serial;
            return;
        }
    }
    /* Replace oldest (index 0) if full. */
    uses[0].queue_index = queue_index;
    uses[0].serial = serial;
}

static void clear_queue_uses(QueueUse* uses) {
    for (int i = 0; i < kMaxQueueUses; ++i) {
        uses[i].queue_index = -1;
        uses[i].serial = 0;
    }
}

static bool queue_uses_completed(const QueueUse* uses) {
    for (int i = 0; i < kMaxQueueUses; ++i) {
        if (uses[i].queue_index < 0) {
            continue;
        }
        int qi = uses[i].queue_index;
        if (qi < 0 || qi >= kMaxQueues || !g_queues[qi].used) {
            return false;
        }
        if (g_queues[qi].completed_serial < uses[i].serial) {
            return false;
        }
    }
    return true;
}

static uint64_t min_incomplete_serial(const QueueUse* uses, int* out_qi) {
    uint64_t best = 0;
    int best_qi = -1;
    for (int i = 0; i < kMaxQueueUses; ++i) {
        if (uses[i].queue_index < 0) {
            continue;
        }
        int qi = uses[i].queue_index;
        if (qi >= 0 && qi < kMaxQueues && g_queues[qi].used &&
            g_queues[qi].completed_serial < uses[i].serial) {
            if (best == 0 || uses[i].serial < best) {
                best = uses[i].serial;
                best_qi = qi;
            }
        }
    }
    if (out_qi) {
        *out_qi = best_qi;
    }
    return best;
}

static const char* kind_str(TrackResourceKind k) {
    if (k == TRACK_RES_BUFFER) return "buffer";
    if (k == TRACK_RES_IMAGE_VIEW) return "image_view";
    return "image";
}

static const char* completion_source_str(CompletionSource s) {
    switch (s) {
        case COMPLETION_INTERNAL_FENCE:
            return "internal_fence";
        case COMPLETION_CALLER_FENCE:
            return "fence";
        case COMPLETION_TIMELINE:
            return "timeline";
        case COMPLETION_QUEUE_IDLE:
            return "queue_idle";
        case COMPLETION_DEVICE_IDLE:
            return "device_idle";
        default:
            return "unknown";
    }
}

static CompletionSource map_api_completion_source(VortekCompletionSource s) {
    switch (s) {
        case VORTEK_COMPLETION_INTERNAL_FENCE:
            return COMPLETION_INTERNAL_FENCE;
        case VORTEK_COMPLETION_CALLER_FENCE:
            return COMPLETION_CALLER_FENCE;
        case VORTEK_COMPLETION_TIMELINE:
            return COMPLETION_TIMELINE;
        case VORTEK_COMPLETION_QUEUE_IDLE:
            return COMPLETION_QUEUE_IDLE;
        case VORTEK_COMPLETION_DEVICE_IDLE:
            return COMPLETION_DEVICE_IDLE;
        default:
            return COMPLETION_UNKNOWN;
    }
}

/* True only when every queue use has verified completed_serial proof. */
static bool has_verified_completion_uses(const QueueUse* uses) {
    bool any = false;
    for (int i = 0; i < kMaxQueueUses; ++i) {
        if (uses[i].queue_index < 0) {
            continue;
        }
        any = true;
        int qi = uses[i].queue_index;
        if (qi < 0 || qi >= kMaxQueues || !g_queues[qi].used) {
            return false;
        }
        if (g_queues[qi].completed_serial < uses[i].serial) {
            return false;
        }
        if (g_queues[qi].completed_source == COMPLETION_UNKNOWN) {
            return false;
        }
    }
    /* No queue uses: idle resource (never submitted) may retire. */
    return true;
    (void)any;
}

void VortekGpuTrack_initOnce(void) {
    if (g_inited) {
        return;
    }
    g_inited = true;
    (void)kBachataVortekGpuVaTrackMarker;

    const char* idle_env = getenv("BACHATA_VORTEK_WAIT_IDLE_BEFORE_DESTROY");
    /* Mode B product default: defer GPU free until completion oracle proves safe. */
    g_defer_destroy = read_bool_default(
        "debug.bachata.vortek_defer_destroy", "BACHATA_VORTEK_DEFER_RESOURCE_DESTROY", true);
    g_wait_idle_destroy = env_truthy(idle_env) ||
                          read_android_prop("debug.bachata.vortek_wait_idle_destroy");
    g_wait_idle_buffer = read_android_prop("debug.bachata.vortek_wait_idle_buffer") ||
                          read_android_prop("debug.bachata.wait_idle_buffer_destroy");
    g_wait_idle_memory = read_android_prop("debug.bachata.vortek_wait_idle_memory") ||
                          read_android_prop("debug.bachata.wait_idle_memory_free");
    g_wait_idle_external = read_android_prop("debug.bachata.vortek_wait_idle_external") ||
                            read_android_prop("debug.bachata.wait_idle_external_release");
    g_wait_idle_sync = read_android_prop("debug.bachata.vortek_wait_idle_sync") ||
                        read_android_prop("debug.bachata.wait_idle_sync_recycle");
    g_wait_idle_cmdpool = read_android_prop("debug.bachata.vortek_wait_idle_cmdpool") ||
                           read_android_prop("debug.bachata.wait_idle_command_pool_reset");
    g_wait_idle_mapping = read_android_prop("debug.bachata.vortek_wait_idle_mapping") ||
                           read_android_prop("debug.bachata.wait_idle_mapping_unmap");
    g_wait_idle_image = read_android_prop("debug.bachata.vortek_wait_idle_image") ||
                         read_android_prop("debug.bachata.wait_idle_image_destroy");
    g_wait_before_present_sem_reuse =
        read_android_prop("debug.bachata.vortek_wait_before_present_sem_reuse");
    g_wait_before_same_image_reuse =
        read_android_prop("debug.bachata.vortek_wait_before_same_image_reuse");
    g_wait_before_same_ahb_reuse =
        read_android_prop("debug.bachata.vortek_wait_before_same_ahb_reuse");
    g_wait_for_ahb_release_fence =
        read_android_prop("debug.bachata.vortek_wait_for_ahb_release_fence");
    g_require_distinct_ahb = read_android_prop("debug.bachata.require_distinct_ahb") ||
                             env_truthy(getenv("BACHATA_REQUIRE_DISTINCT_AHB"));
    g_quarantine_gpu_releases = read_android_prop("debug.bachata.quarantine_gpu_releases") ||
                                env_truthy(getenv("BACHATA_QUARANTINE_GPU_RELEASES"));
    g_quarantine_buffers = read_android_prop("debug.bachata.quarantine_buffers") ||
                           env_truthy(getenv("BACHATA_QUARANTINE_BUFFERS"));
    g_quarantine_images = read_android_prop("debug.bachata.quarantine_images") ||
                          env_truthy(getenv("BACHATA_QUARANTINE_IMAGES"));
    g_quarantine_memory = read_android_prop("debug.bachata.quarantine_memory") ||
                          env_truthy(getenv("BACHATA_QUARANTINE_MEMORY"));
    g_retain_unknown_buffers = read_android_prop("debug.bachata.retain_unknown_buffers") ||
                               env_truthy(getenv("BACHATA_RETAIN_UNKNOWN_BUFFERS"));
    /* Product defaults for Mali/Vortek freeflight (R1 proof on CUSA07023/Poco X6 Pro):
     * wait_on_suballoc_overlap=1 eliminates DEVICE_LOST from in-flight range rebind.
     * suballoc_range_pool=1 reduces QueueWaitIdle thrash via generation leases.
     * defer_destroy stays Mode-B-friendly when unset (also default ON below).
     * Prop/env=0 still disables. Dig stamp/exact/pin stay default OFF. */
    g_wait_on_suballoc_overlap = read_bool_default(
        "debug.bachata.wait_on_suballoc_overlap", "BACHATA_WAIT_ON_SUBALLOC_OVERLAP", true);
    g_suballoc_range_pool = read_bool_default(
        "debug.bachata.suballoc_range_pool", "BACHATA_SUBALLOC_RANGE_POOL", true);
    /* Dig knobs: default OFF (restore playability). Opt-in via prop=1. */
    g_detile_stamp =
        read_bool_default("debug.bachata.detile_stamp", "BACHATA_DETILE_STAMP", false);
    g_detile_source_exact_wait = read_bool_default(
        "debug.bachata.detile_source_exact_wait", "BACHATA_DETILE_SOURCE_EXACT_WAIT", false);
    g_pin_fhd_detile_sources = read_bool_default(
        "debug.bachata.pin_fhd_detile_sources", "BACHATA_PIN_FHD_DETILE_SOURCES", false);
    g_gpu_address_binding_report = read_bool_default(
        "debug.bachata.gpu_address_binding_report", "BACHATA_GPU_ADDRESS_BINDING_REPORT", false);
    /* Dig spam off by default — M1f cold baseline needed quiet host. */
    g_fhd_prepare_write_diag = read_bool_default(
        "debug.bachata.fhd_prepare_write_diag", "BACHATA_FHD_PREPARE_WRITE_DIAG", false);
    g_device_fault_query_wanted = read_bool_default(
        "debug.bachata.device_fault_query", "BACHATA_DEVICE_FAULT_QUERY", false);
    g_suballoc_pool_initial_slots =
        read_android_prop_int("debug.bachata.suballoc_pool_initial_slots", 4);
    g_suballoc_pool_max_slots =
        read_android_prop_int("debug.bachata.suballoc_pool_max_slots", 8);
    {
        int max_mb = read_android_prop_int("debug.bachata.suballoc_pool_max_mb", 96);
        if (max_mb < 8) {
            max_mb = 8;
        }
        if (max_mb > 512) {
            max_mb = 512;
        }
        g_suballoc_pool_max_bytes = (uint64_t)max_mb * 1024ull * 1024ull;
    }
    if (g_suballoc_pool_initial_slots < 1) {
        g_suballoc_pool_initial_slots = 1;
    }
    if (g_suballoc_pool_max_slots < g_suballoc_pool_initial_slots) {
        g_suballoc_pool_max_slots = g_suballoc_pool_initial_slots;
    }
    if (g_suballoc_pool_max_slots > 64) {
        g_suballoc_pool_max_slots = 64;
    }
    memset(&g_suballoc_wait, 0, sizeof(g_suballoc_wait));
    memset(g_leases, 0, sizeof(g_leases));
    memset(g_durable_fences, 0, sizeof(g_durable_fences));
    g_lease_count = 0;
    g_next_lease_generation = 1;

    VT_TRACK_LOG(
        "VORTEK_TRACK_CONFIG marker=%s deferDestroy=%d waitIdleDestroy=%d "
        "waitIdleBuffer=%d waitIdleMemory=%d waitIdleExternal=%d waitIdleSync=%d "
        "waitIdleCmdPool=%d waitIdleMapping=%d waitIdleImage=%d "
        "waitBeforePresentSemReuse=%d waitBeforeSameImageReuse=%d "
        "waitBeforeSameAhbReuse=%d waitForAhbReleaseFence=%d "
        "waitOnSuballocOverlap=%d suballocRangePool=%d poolInitialSlots=%d "
        "poolMaxSlots=%d poolMaxMb=%d detileStamp=%d detileSourceExactWait=%d "
        "pinFhdDetileSources=%d gpuAddressBindingReport=%d fhdPrepareWriteDiag=%d "
        "deviceFaultQueryWanted=%d "
        "requireDistinctAhb=%d quarantineGpuReleases=%d quarantineBuffers=%d "
        "quarantineImages=%d quarantineMemory=%d retainUnknownBuffers=%d "
        "freeflightRing=%d perQueueRetirement=1 fullClosure=1 internalCompletion=1 "
        "failClosed=1 physicalReleaseChoke=1 distinctBackingIds=1 offsetIdSpaces=1 "
        "presentOwnership=1 presentWaitSemConsume=1 bufferRetirementCoverage=1 multiClassRetirement=1 antiThrashCollect=1 suballocReuseTrack=1 suballocRangePoolTrack=1 durableFenceMap=1 detilePushStamp=1 gpuMappingLifetime=1 fhdPrepareWriteCheck=1 deviceAddressBinding=1 deviceFaultQuery=1 pinFhdSources=1",
        BACHATA_VORTEK_GPU_TRACK_MARKER, g_defer_destroy ? 1 : 0,
        g_wait_idle_destroy ? 1 : 0, g_wait_idle_buffer ? 1 : 0,
        g_wait_idle_memory ? 1 : 0, g_wait_idle_external ? 1 : 0, g_wait_idle_sync ? 1 : 0,
        g_wait_idle_cmdpool ? 1 : 0, g_wait_idle_mapping ? 1 : 0, g_wait_idle_image ? 1 : 0,
        g_wait_before_present_sem_reuse ? 1 : 0, g_wait_before_same_image_reuse ? 1 : 0,
        g_wait_before_same_ahb_reuse ? 1 : 0, g_wait_for_ahb_release_fence ? 1 : 0,
        g_wait_on_suballoc_overlap ? 1 : 0, g_suballoc_range_pool ? 1 : 0,
        g_suballoc_pool_initial_slots, g_suballoc_pool_max_slots,
        (int)(g_suballoc_pool_max_bytes / (1024ull * 1024ull)), g_detile_stamp ? 1 : 0,
        g_detile_source_exact_wait ? 1 : 0, g_pin_fhd_detile_sources ? 1 : 0,
        g_gpu_address_binding_report ? 1 : 0, g_fhd_prepare_write_diag ? 1 : 0,
        g_device_fault_query_wanted ? 1 : 0, g_require_distinct_ahb ? 1 : 0,
        g_quarantine_gpu_releases ? 1 : 0, g_quarantine_buffers ? 1 : 0,
        g_quarantine_images ? 1 : 0, g_quarantine_memory ? 1 : 0,
        g_retain_unknown_buffers ? 1 : 0, kFreeflightRing);
    VT_TRACK_LOG("GPU_ADDRESS_BINDING_REPORT enabled=%d", g_gpu_address_binding_report ? 1 : 0);
    VT_TRACK_LOG("DEVICE_FAULT_QUERY enabled=%d", g_device_fault_query_wanted ? 1 : 0);
}

bool VortekGpuTrack_deferDestroyEnabled(void) {
    VortekGpuTrack_initOnce();
    return g_defer_destroy;
}

bool VortekGpuTrack_waitIdleBeforeDestroyEnabled(void) {
    VortekGpuTrack_initOnce();
    return g_wait_idle_destroy;
}

bool VortekGpuTrack_shouldWaitIdleBefore(VortekWaitIdleClass klass) {
    VortekGpuTrack_initOnce();
    if (g_wait_idle_destroy) {
        return true;
    }
    switch (klass) {
        case VORTEK_WAIT_IDLE_BUFFER:
            return g_wait_idle_buffer;
        case VORTEK_WAIT_IDLE_MEMORY:
            return g_wait_idle_memory;
        case VORTEK_WAIT_IDLE_EXTERNAL:
            return g_wait_idle_external;
        case VORTEK_WAIT_IDLE_SYNC:
            return g_wait_idle_sync;
        case VORTEK_WAIT_IDLE_CMDPOOL:
            return g_wait_idle_cmdpool;
        case VORTEK_WAIT_IDLE_MAPPING:
            return g_wait_idle_mapping;
        case VORTEK_WAIT_IDLE_IMAGE:
            return g_wait_idle_image || g_wait_idle_buffer;
        default:
            return false;
    }
}

bool VortekGpuTrack_rangeFits(uint64_t allocation_size, uint64_t bind_offset,
                              uint64_t resource_offset, uint64_t access_size) {
    if (bind_offset > allocation_size) {
        return false;
    }
    const uint64_t remaining_after_bind = allocation_size - bind_offset;
    if (resource_offset > remaining_after_bind) {
        return false;
    }
    return access_size <= remaining_after_bind - resource_offset;
}

static const char* release_reason_str(int reason) {
    switch (reason) {
        case VORTEK_RELEASE_DESTROY_BUFFER: return "destroy_buffer";
        case VORTEK_RELEASE_DESTROY_IMAGE: return "destroy_image";
        case VORTEK_RELEASE_FREE_MEMORY: return "free_memory";
        case VORTEK_RELEASE_UNMAP_MEMORY: return "unmap_memory";
        case VORTEK_RELEASE_EXTERNAL: return "external_release";
        case VORTEK_RELEASE_GPU_VA_UNMAP: return "gpu_va_unmap";
        case VORTEK_RELEASE_IMAGE_VIEW: return "destroy_image_view";
        case VORTEK_RELEASE_SEMAPHORE: return "destroy_semaphore";
        case VORTEK_RELEASE_CMDPOOL: return "cmdpool";
        case VORTEK_RELEASE_HANDLE_ERASE: return "handle_erase";
        case VORTEK_RELEASE_COLLECTOR: return "collector";
        default: return "unknown";
    }
}

static void push_phys_rel(int reason, uint64_t id, void* handle, int allowed, const char* tag) {
    PhysRelEvent* e = &g_phys_rel_ring[g_phys_rel_write];
    memset(e, 0, sizeof(*e));
    e->seq = ++g_phys_rel_seq;
    e->kind = reason;
    e->id = id;
    e->handle = handle;
    e->allowed = allowed;
    if (tag) {
        strncpy(e->tag, tag, sizeof(e->tag) - 1);
    }
    g_phys_rel_write = (g_phys_rel_write + 1) % kPhysRelRing;
    if (g_phys_rel_count < kPhysRelRing) {
        g_phys_rel_count++;
    }
}

static PresentImageState* present_image_state(uint32_t image_index) {
    if (image_index >= kMaxPresentImages) {
        return NULL;
    }
    PresentImageState* s = &g_present_images[image_index];
    if (!s->used) {
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->image_index = image_index;
    }
    return s;
}

/* Defined later; acquire path may complete a still-pending present via reacquire. */
void VortekGpuTrack_notePresentComplete(uint64_t presentId, uint32_t imageIndex,
                                        const char* source);

/* Central physical-release gate. Returns true if caller may free now. */
static uint64_t freeflight_now_ms(void) {
#if defined(__ANDROID__) || defined(__linux__)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    }
#endif
    return 0;
}

void VortekGpuTrack_freeflightEvent(const char* tag, void* queue, uint64_t submission, void* resource,
                                    uint64_t allocation, uint64_t backing, uint64_t mapping,
                                    uint64_t lastUse, uint64_t completed, uint32_t pendingRefs,
                                    int logicalDestroy, int physicalDestroy) {
    FreeflightEvent* e = &g_freeflight[g_freeflight_write];
    memset(e, 0, sizeof(*e));
    e->seq = ++g_freeflight_seq;
    e->ts_ms = freeflight_now_ms();
    if (tag) {
        strncpy(e->tag, tag, sizeof(e->tag) - 1);
    }
    e->queue = queue;
    e->submission = submission;
    e->resource = resource;
    e->allocation = allocation;
    e->backing = backing;
    e->mapping = mapping;
    e->last_use = lastUse;
    e->completed = completed;
    e->pending_refs = pendingRefs;
    e->logical_destroy = logicalDestroy;
    e->physical_destroy = physicalDestroy;
    g_freeflight_write = (g_freeflight_write + 1) % kFreeflightRing;
    if (g_freeflight_count < kFreeflightRing) {
        g_freeflight_count++;
    }
}


static void maybe_log_retention_summary(void) {
    uint64_t now = freeflight_now_ms();
    if (g_last_retention_summary_ms != 0 && now > g_last_retention_summary_ms &&
        (now - g_last_retention_summary_ms) < 5000ull) {
        return;
    }
    g_last_retention_summary_ms = now ? now : 1;
    uint64_t count = 0;
    uint64_t bytes = 0;
    char top_ops[4][24];
    int top_n = 0;
    memset(top_ops, 0, sizeof(top_ops));
    for (int i = 0; i < g_resource_count; ++i) {
        TrackedResource* r = &g_resources[i];
        if (r->kind != TRACK_RES_BUFFER || !r->alive) {
            continue;
        }
        if (r->unknown_use_retained ||
            (r->destroy_requested && r->last_submitted_use == 0 && !r->actually_destroyed)) {
            count++;
            bytes += r->buffer_size;
            if (top_n < 4 && r->last_op[0]) {
                int dup = 0;
                for (int t = 0; t < top_n; ++t) {
                    if (strncmp(top_ops[t], r->last_op, sizeof(top_ops[t])) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    strncpy(top_ops[top_n], r->last_op, sizeof(top_ops[top_n]) - 1);
                    top_n++;
                }
            }
        }
    }
    g_unknown_buffer_retain_count = count;
    g_unknown_buffer_retain_bytes = bytes;
    VT_TRACK_LOG(
        "UNKNOWN_BUFFER_RETENTION_SUMMARY count=%" PRIu64 " bytes=%" PRIu64
        " retireCommit=%" PRIu64 " retireBlock=%" PRIu64 " directBypass=%" PRIu64
        " quarantineBlocks=%" PRIu64 " retainUnknown=%d quarantineBuf=%d quarantineImg=%d "
        "quarantineMem=%d quarantineAll=%d topLastKnownUses=%s,%s,%s,%s",
        count, bytes, g_buffer_retire_commit_count, g_buffer_retire_block_count,
        g_direct_destroy_bypass_count, g_quarantine_block_count,
        g_retain_unknown_buffers ? 1 : 0, g_quarantine_buffers ? 1 : 0,
        g_quarantine_images ? 1 : 0, g_quarantine_memory ? 1 : 0,
        g_quarantine_gpu_releases ? 1 : 0, top_ops[0], top_ops[1], top_ops[2], top_ops[3]);
}

static TrackedResource* find_resource_by_id(uint64_t id);
static TrackedResource* find_resource(void* handle);

static bool request_physical_release(int reason, uint64_t id, void* handle,
                                     const QueueUse* uses, uint64_t last_use,
                                     uint32_t pending_refs, uint32_t dep_children,
                                     bool* out_release_requested, const char* entity) {
    if (out_release_requested) {
        *out_release_requested = true;
    }
    if (rate_allow(&g_phys_rel_log_count)) {
        VT_TRACK_LOG(
            "PHYSICAL_RELEASE_REQUEST reason=%s id=%" PRIu64 " handle=%p entity=%s "
            "lastUse=%" PRIu64 " pendingRefs=%u depChildren=%u",
            release_reason_str(reason), id, handle, entity ? entity : "?", last_use, pending_refs,
            dep_children);
    }

    VortekGpuTrack_freeflightEvent(release_reason_str(reason), g_last_queue, g_last_submitted, handle,
                                   id, 0, 0, last_use, g_last_completed, pending_refs, 1, 0);

    /* After first frame: class/global quarantine blocks physical free (diag only). */
    if (g_first_frame_seen && quarantine_reason_enabled(reason)) {
        g_quarantine_block_count++;
        if (rate_allow(&g_quarantine_log_count)) {
            VT_TRACK_LOG(
                "QUARANTINE_PHYSICAL_RELEASE reason=%s class=%s id=%" PRIu64 " handle=%p "
                "entity=%s blockCount=%" PRIu64 " lastUse=%" PRIu64,
                release_reason_str(reason), quarantine_class_str(reason), id, handle,
                entity ? entity : "?", g_quarantine_block_count, last_use);
        }
        VortekGpuTrack_freeflightEvent("QUARANTINE", g_last_queue, g_last_submitted, handle, id, 0, 0,
                                       last_use, g_last_completed, pending_refs, 1, 0);
        push_phys_rel(reason, id, handle, 0, "quarantine");
        return false;
    }

    /* Product fail-closed: post-first-frame GPU objects with no tracked use must not
     * destroy. "No known use" is not proof of safety (missing cmd refs / descriptors). */
    if (g_first_frame_seen && last_use == 0 &&
        (reason == VORTEK_RELEASE_DESTROY_BUFFER || reason == VORTEK_RELEASE_DESTROY_IMAGE ||
         reason == VORTEK_RELEASE_IMAGE_VIEW || reason == VORTEK_RELEASE_FREE_MEMORY ||
         reason == VORTEK_RELEASE_EXTERNAL || reason == VORTEK_RELEASE_UNMAP_MEMORY ||
         reason == VORTEK_RELEASE_GPU_VA_UNMAP)) {
        if (reason == VORTEK_RELEASE_DESTROY_BUFFER) {
            g_buffer_retire_block_count++;
            VT_TRACK_LOG(
                "BUFFER_RETIRE_BLOCKED_UNKNOWN_USE resource=%" PRIu64 " handle=%p "
                "retainUnknown=%d lastUse=0 completed=%" PRIu64 " pendingRefs=%u",
                id, handle, g_retain_unknown_buffers ? 1 : 0, g_last_completed, pending_refs);
            VortekGpuTrack_freeflightEvent("BUF_UNK_USE", g_last_queue, g_last_submitted, handle, id,
                                           0, 0, 0, g_last_completed, pending_refs, 1, 0);
        } else if (reason == VORTEK_RELEASE_DESTROY_IMAGE ||
                   reason == VORTEK_RELEASE_IMAGE_VIEW) {
            VT_TRACK_LOG(
                "IMAGE_RETIRE_BLOCKED_UNKNOWN_USE resource=%" PRIu64 " handle=%p "
                "reason=%s lastUse=0 completed=%" PRIu64 " pendingRefs=%u",
                id, handle, release_reason_str(reason), g_last_completed, pending_refs);
            VortekGpuTrack_freeflightEvent("IMG_UNK_USE", g_last_queue, g_last_submitted, handle, id,
                                           0, 0, 0, g_last_completed, pending_refs, 1, 0);
        } else {
            VT_TRACK_LOG(
                "MEMORY_RETIRE_BLOCKED_UNKNOWN_USE allocation=%" PRIu64 " handle=%p "
                "reason=%s lastUse=0 completed=%" PRIu64 " pendingRefs=%u depChildren=%u",
                id, handle, release_reason_str(reason), g_last_completed, pending_refs,
                dep_children);
            VortekGpuTrack_freeflightEvent("MEM_UNK_USE", g_last_queue, g_last_submitted, handle, id,
                                           0, 0, 0, g_last_completed, pending_refs, 1, 0);
        }
        push_phys_rel(reason, id, handle, 0, "unknown_use");
        return false;
    }

    /* BDA-enabled buffer without completed tracked use: fail closed. */
    if (reason == VORTEK_RELEASE_DESTROY_BUFFER && g_first_frame_seen) {
        TrackedResource* br = find_resource_by_id(id);
        /* find_resource_by_id may miss if only handle known; try handle scan below via entity */
        if (!br) {
            for (int i = 0; i < g_resource_count; ++i) {
                if (g_resources[i].handle == handle && g_resources[i].kind == TRACK_RES_BUFFER) {
                    br = &g_resources[i];
                    break;
                }
            }
        }
        if (br && br->bda_enabled && last_use == 0) {
            g_buffer_retire_block_count++;
            VT_TRACK_LOG(
                "BUFFER_RETIRE_BLOCKED_BDA resource=%" PRIu64 " handle=%p bda=0x%" PRIx64
                " size=%" PRIu64,
                id, handle, br->bda_address, br->bda_size);
            VT_TRACK_LOG("BDA_ALLOCATION_RETAINED resource=%" PRIu64 " allocation=%" PRIu64,
                         id, br->allocation_id);
            push_phys_rel(reason, id, handle, 0, "bda");
            return false;
        }
    }

    const bool has_use = last_use > 0;
    const bool uses_done = !uses || queue_uses_completed(uses);
    const bool verified = !has_use || (uses && has_verified_completion_uses(uses));
    if (has_use && (!uses_done || !verified)) {
        if (reason == VORTEK_RELEASE_DESTROY_BUFFER) {
            g_buffer_retire_block_count++;
            VT_TRACK_LOG(
                "BUFFER_RETIRE_BLOCKED_IN_FLIGHT resource=%" PRIu64 " lastUse=%" PRIu64
                " completed=%" PRIu64 " pendingRefs=%u",
                id, last_use, g_last_completed, pending_refs);
        }
        VT_TRACK_LOG(
            "PHYSICAL_RELEASE_BLOCKED_NO_COMPLETION reason=%s id=%" PRIu64
            " lastUse=%" PRIu64 " completed=%" PRIu64 " pendingRefs=%u",
            release_reason_str(reason), id, last_use, g_last_completed, pending_refs);
        push_phys_rel(reason, id, handle, 0, "no_completion");
        return false;
    }
    if (dep_children != 0) {
        if (reason == VORTEK_RELEASE_DESTROY_BUFFER) {
            g_buffer_retire_block_count++;
            VT_TRACK_LOG(
                "BUFFER_RETIRE_BLOCKED_CHILD_DEPENDENCY resource=%" PRIu64 " depChildren=%u",
                id, dep_children);
        }
        VT_TRACK_LOG(
            "PHYSICAL_RELEASE_BLOCKED_DEPENDENCY reason=%s id=%" PRIu64 " depChildren=%u",
            release_reason_str(reason), id, dep_children);
        push_phys_rel(reason, id, handle, 0, "dependency");
        return false;
    }
    if (pending_refs != 0) {
        if (reason == VORTEK_RELEASE_DESTROY_BUFFER) {
            g_buffer_retire_block_count++;
            VT_TRACK_LOG(
                "BUFFER_RETIRE_BLOCKED_IN_FLIGHT resource=%" PRIu64 " pendingRefs=%u", id,
                pending_refs);
        }
        VT_TRACK_LOG(
            "PHYSICAL_RELEASE_BLOCKED_DEPENDENCY reason=%s id=%" PRIu64 " pendingRefs=%u",
            release_reason_str(reason), id, pending_refs);
        push_phys_rel(reason, id, handle, 0, "pending_refs");
        return false;
    }
    if (rate_allow(&g_phys_rel_log_count)) {
        VT_TRACK_LOG(
            "PHYSICAL_RELEASE_COMMIT reason=%s id=%" PRIu64 " handle=%p entity=%s "
            "lastUse=%" PRIu64 " completed=%" PRIu64,
            release_reason_str(reason), id, handle, entity ? entity : "?", last_use,
            g_last_completed);
    }
    VortekGpuTrack_freeflightEvent("PHYSICAL_COMMIT", g_last_queue, g_last_submitted, handle, id, 0,
                                   0, last_use, g_last_completed, pending_refs, 1, 1);
    push_phys_rel(reason, id, handle, 1, "commit");
    return true;
}

static TrackedGpuAllocation* find_alloc_by_rm(ResourceMemory* rm) {
    if (!rm) {
        return NULL;
    }
    for (int i = 0; i < g_alloc_count; ++i) {
        if (g_allocs[i].alive && g_allocs[i].rm == rm) {
            return &g_allocs[i];
        }
    }
    return NULL;
}

static TrackedGpuAllocation* find_alloc_by_id(uint64_t id) {
    if (id == 0) {
        return NULL;
    }
    for (int i = 0; i < g_alloc_count; ++i) {
        if (g_allocs[i].id == id) {
            return &g_allocs[i];
        }
    }
    return NULL;
}

/* Guest-visible lookup: destroy_requested resources are hidden from new commands. */
static TrackedResource* find_resource(void* handle) {
    if (!handle) {
        return NULL;
    }
    for (int i = 0; i < g_resource_count; ++i) {
        if (g_resources[i].alive && g_resources[i].guest_visible &&
            g_resources[i].handle == handle) {
            return &g_resources[i];
        }
    }
    return NULL;
}

static TrackedResource* find_resource_by_id(uint64_t id) {
    if (id == 0) {
        return NULL;
    }
    for (int i = 0; i < g_resource_count; ++i) {
        if (g_resources[i].id == id) {
            return &g_resources[i];
        }
    }
    return NULL;
}

static TrackedResource* alloc_resource_slot(void) {
    for (int i = 0; i < g_resource_count; ++i) {
        if (g_resources[i].destroy_requested && !g_resources[i].actually_destroyed) {
            VT_TRACK_LOG(
                "HANDLE_SLOT_REUSE_BLOCKED kind=resource id=%" PRIu64
                " destroyRequested=1 actuallyDestroyed=0",
                g_resources[i].id);
            continue;
        }
        if (!g_resources[i].alive && g_resources[i].actually_destroyed) {
            return &g_resources[i];
        }
        if (!g_resources[i].alive && !g_resources[i].destroy_requested) {
            return &g_resources[i];
        }
    }
    if (g_resource_count >= kMaxResources) {
        VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=resource count=%d", g_resource_count);
        return NULL;
    }
    return &g_resources[g_resource_count++];
}

static TrackedGpuAllocation* alloc_alloc_slot(void) {
    for (int i = 0; i < g_alloc_count; ++i) {
        if (g_allocs[i].destroy_requested && !g_allocs[i].actually_destroyed) {
            VT_TRACK_LOG(
                "HANDLE_SLOT_REUSE_BLOCKED kind=allocation id=%" PRIu64
                " destroyRequested=1 actuallyDestroyed=0",
                g_allocs[i].id);
            continue;
        }
        if (!g_allocs[i].alive && g_allocs[i].actually_destroyed) {
            return &g_allocs[i];
        }
        if (!g_allocs[i].alive && !g_allocs[i].destroy_requested) {
            return &g_allocs[i];
        }
    }
    if (g_alloc_count >= kMaxAllocs) {
        VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=allocation count=%d", g_alloc_count);
        return NULL;
    }
    return &g_allocs[g_alloc_count++];
}

static TrackedCommandBuffer* find_or_create_cmd(void* handle) {
    if (!handle) {
        return NULL;
    }
    for (int i = 0; i < g_cmd_count; ++i) {
        if (g_cmds[i].alive && g_cmds[i].handle == handle) {
            return &g_cmds[i];
        }
    }
    for (int i = 0; i < g_cmd_count; ++i) {
        if (!g_cmds[i].alive) {
            memset(&g_cmds[i], 0, sizeof(g_cmds[i]));
            g_cmds[i].handle = handle;
            g_cmds[i].alive = true;
            return &g_cmds[i];
        }
    }
    if (g_cmd_count >= kMaxCmdBuffers) {
        VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=cmdBuffer count=%d", g_cmd_count);
        return NULL;
    }
    TrackedCommandBuffer* c = &g_cmds[g_cmd_count++];
    memset(c, 0, sizeof(*c));
    c->handle = handle;
    c->alive = true;
    return c;
}

static void push_op(const GpuOp* op) {
    g_ops[g_op_write] = *op;
    g_op_write = (g_op_write + 1) % kOpRing;
    if (g_op_count < kOpRing) {
        g_op_count++;
    }
}

static void track_cmd_resource(TrackedCommandBuffer* cmd, uint64_t resource_id, bool is_write) {
    if (!cmd || resource_id == 0) {
        return;
    }
    for (int i = 0; i < cmd->resource_count; ++i) {
        if (cmd->resource_ids[i] == resource_id) {
            if (is_write) {
                cmd->resource_writes[i] = 1;
            }
            return;
        }
    }
    if (cmd->resource_count >= kMaxRefsPerCmd) {
        VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=cmdRefs count=%d", cmd->resource_count);
        return;
    }
    cmd->resource_ids[cmd->resource_count] = resource_id;
    cmd->resource_writes[cmd->resource_count] = is_write ? 1 : 0;
    cmd->resource_count++;
}

/* Recursive-style closure: buffer/image → parent allocation (and external). */
static void add_resource_closure(TrackedCommandBuffer* cmd, uint64_t resource_id,
                                 bool is_write) {
    if (!cmd || resource_id == 0) {
        return;
    }
    int before = cmd->resource_count;
    track_cmd_resource(cmd, resource_id, is_write);
    if (cmd->resource_count == before) {
        /* Already present (or overflow). Still ensure parent if first insert failed only
         * on dup — for dup we still want parent once. */
    }
    TrackedResource* r = find_resource_by_id(resource_id);
    if (!r) {
        return;
    }
    /* Parent allocation is not a resource id; stamped at submit via allocation_id. */
    (void)r;
}

static void recount_alloc_children(TrackedGpuAllocation* a) {
    if (!a) {
        return;
    }
    uint32_t live = 0;
    uint32_t pending = 0;
    for (int i = 0; i < g_resource_count; ++i) {
        TrackedResource* r = &g_resources[i];
        if (r->allocation_id != a->id) {
            continue;
        }
        if (r->actually_destroyed) {
            continue;
        }
        if (r->destroy_requested) {
            pending++;
        } else if (r->alive && r->guest_visible) {
            live++;
        } else if (r->alive) {
            pending++;
        }
    }
    a->live_child_objects = live;
    a->pending_child_objects = pending;
}

static bool resource_can_retire(const TrackedResource* r) {
    if (!r) {
        return true;
    }
    if (r->pending_submission_refs != 0) {
        return false;
    }
    if (!queue_uses_completed(r->last_use)) {
        return false;
    }
    /* Global fallback: if last_use empty but global serial still open. */
    if (r->last_submitted_use > g_last_completed && r->last_submitted_use > 0) {
        /* Only apply if we have no per-queue uses yet (legacy path). */
        bool any_use = false;
        for (int i = 0; i < kMaxQueueUses; ++i) {
            if (r->last_use[i].queue_index >= 0) {
                any_use = true;
                break;
            }
        }
        if (!any_use) {
            return false;
        }
    }
    return true;
}

static bool alloc_can_retire(const TrackedGpuAllocation* a) {
    if (!a) {
        return true;
    }
    recount_alloc_children((TrackedGpuAllocation*)a);
    if (a->live_child_objects != 0 || a->pending_child_objects != 0) {
        return false;
    }
    if (a->pending_submission_refs != 0) {
        return false;
    }
    if (!queue_uses_completed(a->last_use)) {
        return false;
    }
    return true;
}

static bool resource_in_flight(const TrackedResource* r) {
    return r && !resource_can_retire(r);
}

static bool alloc_in_flight(const TrackedGpuAllocation* a) {
    return a && !alloc_can_retire(a);
}

static void enqueue_ready_destroy(DeferredKind kind, void* handle, void* device,
                                  ResourceMemory* rm) {
    if (g_ready_destroy_count >= kMaxDeferred) {
        VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=readyDestroy");
        return;
    }
    DeferredTake* t = &g_ready_destroy[g_ready_destroy_count++];
    t->kind = kind;
    t->handle = handle;
    t->device = device;
    t->rm = rm;
}

int VortekGpuTrack_takeDeferredDestroy(int* outKind, void** outHandle, void** outDevice,
                                       ResourceMemory** outRm) {
    if (g_ready_destroy_count <= 0) {
        return 0;
    }
    DeferredTake t = g_ready_destroy[--g_ready_destroy_count];
    if (outKind) {
        *outKind = (int)t.kind;
    }
    if (outHandle) {
        *outHandle = t.handle;
    }
    if (outDevice) {
        *outDevice = t.device;
    }
    if (outRm) {
        *outRm = t.rm;
    }
    return 1;
}

static void note_retire_queue_depth(void) {
    int depth = 0;
    for (int i = 0; i < g_resource_count; ++i) {
        if (g_resources[i].destroy_requested && !g_resources[i].actually_destroyed) {
            depth++;
        }
    }
    for (int i = 0; i < g_alloc_count; ++i) {
        if (g_allocs[i].destroy_requested && !g_allocs[i].actually_destroyed) {
            depth++;
        }
    }
    g_retire_queue_depth = depth;
    if (depth > g_retire_queue_max) {
        g_retire_queue_max = depth;
    }
}

static void commit_destroy_resource(TrackedResource* r) {
    if (!r || r->actually_destroyed) {
        return;
    }
    if (!has_verified_completion_uses(r->last_use) && r->last_submitted_use > 0) {
        VT_TRACK_LOG(
            "RETIRE_BLOCKED_NO_COMPLETION_PROOF resource=%" PRIu64 " kind=%s lastUse=%" PRIu64
            " completed=%" PRIu64 " completionSource=%s destroyRequested=1",
            r->id, kind_str(r->kind), r->last_submitted_use, g_last_completed,
            completion_source_str(r->last_completion_source));
        return;
    }
    if (r->last_submitted_use > 0 && r->last_completion_source == COMPLETION_UNKNOWN &&
        !queue_uses_completed(r->last_use)) {
        VT_TRACK_LOG(
            "RETIRE_BLOCKED_NO_COMPLETION_PROOF resource=%" PRIu64 " kind=%s lastUse=%" PRIu64
            " completed=%" PRIu64 " completionSource=unknown destroyRequested=1",
            r->id, kind_str(r->kind), r->last_submitted_use, g_last_completed);
        return;
    }

    int qi = -1;
    uint64_t last_serial = min_incomplete_serial(r->last_use, &qi);
    if (last_serial == 0) {
        for (int i = 0; i < kMaxQueueUses; ++i) {
            if (r->last_use[i].queue_index >= 0 && r->last_use[i].serial > last_serial) {
                last_serial = r->last_use[i].serial;
                qi = r->last_use[i].queue_index;
            }
        }
    }
    uint64_t completed =
        (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_serial : g_last_completed;
    CompletionSource src =
        (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_source : g_last_completion_source;
    if (src == COMPLETION_UNKNOWN) {
        src = r->last_completion_source;
    }
    if (r->last_submitted_use > 0 && src == COMPLETION_UNKNOWN) {
        VT_TRACK_LOG(
            "RETIRE_BLOCKED_NO_COMPLETION_PROOF resource=%" PRIu64 " kind=%s lastUse=%" PRIu64
            " completed=%" PRIu64 " completionSource=unknown destroyRequested=1",
            r->id, kind_str(r->kind), r->last_submitted_use, completed);
        return;
    }

    {
        int rel = VORTEK_RELEASE_DESTROY_BUFFER;
        if (r->kind == TRACK_RES_IMAGE) {
            rel = VORTEK_RELEASE_DESTROY_IMAGE;
        } else if (r->kind == TRACK_RES_IMAGE_VIEW) {
            rel = VORTEK_RELEASE_IMAGE_VIEW;
        }
        if (!request_physical_release(rel, r->id, r->handle, r->last_use, r->last_submitted_use,
                                      r->pending_submission_refs, 0, NULL, kind_str(r->kind))) {
            /* Sticky for permanent gates (quarantine / unknown-use) to stop collect thrash. */
            r->retire_sticky_block = true;
            r->last_retire_attempt_completed = g_last_completed;
            r->last_retire_attempt_refs = r->pending_submission_refs;
            return;
        }
    }
    r->retire_sticky_block = false;

    if (r->kind == TRACK_RES_BUFFER) {
        g_buffer_retire_commit_count++;
        VT_TRACK_LOG(
            "BUFFER_RETIRE_COMMIT resource=%" PRIu64 " lastUseQueue=%d lastUseSerial=%" PRIu64
            " completedSerial=%" PRIu64 " completionSource=%s allocation=%" PRIu64
            " backing=%" PRIu64 " handle=%p",
            r->id, qi, r->last_submitted_use, completed, completion_source_str(src),
            r->allocation_id, r->external_backing_id, r->handle);
    } else if (r->kind == TRACK_RES_IMAGE || r->kind == TRACK_RES_IMAGE_VIEW) {
        VT_TRACK_LOG(
            "IMAGE_RETIRE_COMMIT resource=%" PRIu64 " kind=%s lastUseQueue=%d "
            "lastUseSerial=%" PRIu64 " completedSerial=%" PRIu64 " completionSource=%s "
            "allocation=%" PRIu64 " handle=%p",
            r->id, kind_str(r->kind), qi, r->last_submitted_use, completed,
            completion_source_str(src), r->allocation_id, r->handle);
    }
    VT_TRACK_LOG("RETIRE_COMMIT resource=%" PRIu64 " kind=%s queue=%d lastUse=%" PRIu64
                 " completed=%" PRIu64 " completionSource=%s allocationId=%" PRIu64
                 " externalBackingId=%" PRIu64 " mappingId=%" PRIu64,
                 r->id, kind_str(r->kind), qi, r->last_submitted_use, completed,
                 completion_source_str(src), r->allocation_id, r->external_backing_id,
                 r->mapping_id);
    VT_TRACK_LOG("RESOURCE_DESTROY_COMMITTED resource=%" PRIu64 " kind=%s handle=%p "
                 "lastUse=%" PRIu64 " completed=%" PRIu64 " pendingRefs=%u "
                 "completionSource=%s",
                 r->id, kind_str(r->kind), r->handle, r->last_submitted_use, g_last_completed,
                 r->pending_submission_refs, completion_source_str(src));
    VortekGpuTrack_freeflightEvent(r->kind == TRACK_RES_BUFFER ? "DESTROY_BUFFER" : "DESTROY_IMAGE",
                                   g_last_queue, g_last_submitted, r->handle, r->allocation_id,
                                   r->external_backing_id, r->mapping_id, r->last_submitted_use,
                                   g_last_completed, r->pending_submission_refs, 1, 1);

    {
        DeferredKind dk = DEFER_IMAGE;
        if (r->kind == TRACK_RES_BUFFER) {
            dk = DEFER_BUFFER;
        } else if (r->kind == TRACK_RES_IMAGE_VIEW) {
            dk = DEFER_IMAGE_VIEW;
        }
        enqueue_ready_destroy(dk, r->handle, r->device, NULL);
    }
    r->actually_destroyed = true;
    r->physically_released = true;
    r->alive = false;
    r->guest_visible = false;
    r->destroy_requested = false;
    r->handle = NULL;
    g_retire_commit_count++;

    if (r->allocation_id) {
        TrackedGpuAllocation* a = find_alloc_by_id(r->allocation_id);
        if (a) {
            recount_alloc_children(a);
        }
    }
}

static void commit_destroy_alloc(TrackedGpuAllocation* a) {
    if (!a || a->actually_destroyed) {
        return;
    }
    recount_alloc_children(a);
    if (a->live_child_objects != 0 || a->pending_child_objects != 0) {
        int qi = -1;
        uint64_t use = min_incomplete_serial(a->last_use, &qi);
        VT_TRACK_LOG(
            "ALLOCATION_RETIRE_BLOCKED allocation=%" PRIu64 " liveChildren=%u "
            "pendingChildren=%u graphicsUse=%" PRIu64 " graphicsCompleted=%" PRIu64,
            a->id, a->live_child_objects, a->pending_child_objects, use,
            (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_serial : g_last_completed);
        if (a->external_backing) {
            VT_TRACK_LOG(
                "BACKING_RETIRE_BLOCKED backing=%" PRIu64 " allocation=%" PRIu64
                " liveChildren=%u pendingChildren=%u lastUse=%" PRIu64 " completed=%" PRIu64,
                a->backing_id, a->id, a->live_child_objects, a->pending_child_objects, use,
                (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_serial : g_last_completed);
        }
        return;
    }

    if (!has_verified_completion_uses(a->last_use) && a->last_submitted_use > 0) {
        VT_TRACK_LOG(
            "RETIRE_BLOCKED_NO_COMPLETION_PROOF resource=alloc-%" PRIu64
            " kind=memory lastUse=%" PRIu64 " completed=%" PRIu64
            " completionSource=%s destroyRequested=1",
            a->id, a->last_submitted_use, g_last_completed,
            completion_source_str(a->last_completion_source));
        if (a->external_backing) {
            VT_TRACK_LOG(
                "BACKING_RETIRE_BLOCKED backing=%" PRIu64 " allocation=%" PRIu64
                " reason=no_completion_proof lastUse=%" PRIu64,
                a->backing_id, a->id, a->last_submitted_use);
        }
        return;
    }
    CompletionSource src = a->last_completion_source;
    if (src == COMPLETION_UNKNOWN && a->last_submitted_use > 0) {
        /* Prefer queue proof. */
        for (int i = 0; i < kMaxQueueUses; ++i) {
            int qi = a->last_use[i].queue_index;
            if (qi >= 0 && qi < kMaxQueues && g_queues[qi].used &&
                g_queues[qi].completed_serial >= a->last_use[i].serial &&
                g_queues[qi].completed_source != COMPLETION_UNKNOWN) {
                src = g_queues[qi].completed_source;
                break;
            }
        }
    }
    if (a->last_submitted_use > 0 && src == COMPLETION_UNKNOWN) {
        VT_TRACK_LOG(
            "RETIRE_BLOCKED_NO_COMPLETION_PROOF resource=alloc-%" PRIu64
            " kind=memory lastUse=%" PRIu64 " completed=%" PRIu64
            " completionSource=unknown destroyRequested=1",
            a->id, a->last_submitted_use, g_last_completed);
        return;
    }

    VT_TRACK_LOG(
        "BACKING_RETIRE_REQUEST backing=%" PRIu64 " allocation=%" PRIu64 " mapping=%" PRIu64
        " gpuVaMapping=%" PRIu64 " lastUse=%" PRIu64 " completed=%" PRIu64,
        a->backing_id, a->id, a->mapping_id, a->gpu_va_mapping_id, a->last_submitted_use,
        g_last_completed);

    if (!request_physical_release(
            a->external_backing ? VORTEK_RELEASE_EXTERNAL : VORTEK_RELEASE_FREE_MEMORY, a->id,
            a->memory_handle, a->last_use, a->last_submitted_use, a->pending_submission_refs,
            a->live_child_objects + a->pending_child_objects, NULL,
            a->external_backing ? "external_backing" : "memory")) {
        VT_TRACK_LOG(
            "BACKING_RETIRE_BLOCKED backing=%" PRIu64 " allocation=%" PRIu64
            " reason=physical_release_gate lastUse=%" PRIu64,
            a->backing_id, a->id, a->last_submitted_use);
        a->retire_sticky_block = true;
        a->last_retire_attempt_completed = g_last_completed;
        a->last_retire_attempt_refs = a->pending_submission_refs;
        return;
    }
    a->retire_sticky_block = false;
    VT_TRACK_LOG(
        "MEMORY_RETIRE_COMMIT allocation=%" PRIu64 " lastUseSerial=%" PRIu64
        " completedSerial=%" PRIu64 " completionSource=%s backing=%" PRIu64 " external=%d",
        a->id, a->last_submitted_use, g_last_completed, completion_source_str(src), a->backing_id,
        a->external_backing ? 1 : 0);

    if (a->mapped && !a->mapping_unmapped) {
        VT_TRACK_LOG(
            "MAPPING_UNMAP_DEFERRED mapping=%" PRIu64 " allocation=%" PRIu64
            " lastUse=%" PRIu64 " completed=%" PRIu64 " (physical unmap with free)",
            a->mapping_id, a->id, a->last_submitted_use, g_last_completed);
        a->mapping_unmapped = true;
        a->mapped = false;
    }

    VT_TRACK_LOG(
        "BACKING_RETIRE_COMMIT backing=%" PRIu64 " allocation=%" PRIu64 " mapping=%" PRIu64
        " gpuVaMapping=%" PRIu64 " lastUse=%" PRIu64 " completed=%" PRIu64
        " completionSource=%s external=%d",
        a->backing_id, a->id, a->mapping_id, a->gpu_va_mapping_id, a->last_submitted_use,
        g_last_completed, completion_source_str(src), a->external_backing ? 1 : 0);

    VT_TRACK_LOG("RETIRE_COMMIT resource=alloc-%" PRIu64 " kind=memory lastUse=%" PRIu64
                 " completed=%" PRIu64 " completionSource=%s backing=%" PRIu64
                 " mapping=%" PRIu64 " gpuVaMapping=%" PRIu64,
                 a->id, a->last_submitted_use, g_last_completed, completion_source_str(src),
                 a->backing_id, a->mapping_id, a->gpu_va_mapping_id);
    VT_TRACK_LOG("RESOURCE_DESTROY_COMMITTED allocation=%" PRIu64 " lastUse=%" PRIu64
                 " completed=%" PRIu64 " pendingRefs=%u completionSource=%s",
                 a->id, a->last_submitted_use, g_last_completed, a->pending_submission_refs,
                 completion_source_str(src));

    if (!has_verified_completion_uses(a->last_use) && a->last_submitted_use == 0) {
        /* Never used on GPU — OK. */
    } else if (a->last_submitted_use > 0 && src == COMPLETION_UNKNOWN) {
        VT_TRACK_LOG("PHYSICAL_RELEASE_WITHOUT_COMPLETION allocation=%" PRIu64, a->id);
        return;
    }

    enqueue_ready_destroy(DEFER_MEMORY, a->memory_handle, a->device, a->rm);
    a->actually_destroyed = true;
    a->physically_freed = true;
    a->alive = false;
    a->destroy_requested = false;
    a->free_requested = false;
    a->backing_released = true;
    a->actual_free_submission = g_last_completed;
    a->rm = NULL;
    g_retire_commit_count++;
}

static void try_commit_resource(TrackedResource* r) {
    if (!r || !r->destroy_requested || r->actually_destroyed) {
        return;
    }
    /* Anti-thrash: only re-attempt when completion watermark or pending refs change.
     * Sticky blocks (quarantine / unknown-use) otherwise spin collectRetired into GB logs. */
    if (r->retire_sticky_block &&
        r->last_retire_attempt_completed == g_last_completed &&
        r->last_retire_attempt_refs == r->pending_submission_refs) {
        return;
    }
    if (!resource_can_retire(r)) {
        r->last_retire_attempt_completed = g_last_completed;
        r->last_retire_attempt_refs = r->pending_submission_refs;
        return;
    }
    /* Clear sticky only when we have a chance (completion advanced). */
    if (r->retire_sticky_block && r->last_retire_attempt_completed != g_last_completed) {
        r->retire_sticky_block = false;
    }
    commit_destroy_resource(r);
}

static void try_commit_alloc(TrackedGpuAllocation* a) {
    if (!a || !a->destroy_requested || a->actually_destroyed) {
        return;
    }
    if (a->retire_sticky_block &&
        a->last_retire_attempt_completed == g_last_completed &&
        a->last_retire_attempt_refs == a->pending_submission_refs) {
        return;
    }
    if (!alloc_can_retire(a)) {
        recount_alloc_children(a);
        a->last_retire_attempt_completed = g_last_completed;
        a->last_retire_attempt_refs = a->pending_submission_refs;
        static uint64_t s_alloc_block_log;
        if (rate_allow(&s_alloc_block_log)) {
            int qi = -1;
            uint64_t use = min_incomplete_serial(a->last_use, &qi);
            VT_TRACK_LOG(
                "ALLOCATION_RETIRE_BLOCKED allocation=%" PRIu64 " liveChildren=%u "
                "pendingChildren=%u graphicsUse=%" PRIu64 " graphicsCompleted=%" PRIu64,
                a->id, a->live_child_objects, a->pending_child_objects, a->last_submitted_use,
                (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_serial : g_last_completed);
            (void)use;
        }
        return;
    }
    if (a->retire_sticky_block && a->last_retire_attempt_completed != g_last_completed) {
        a->retire_sticky_block = false;
    }
    commit_destroy_alloc(a);
}

void VortekGpuTrack_collectRetired(void) {
    maybe_log_retention_summary();
    /* Children first, then parents. */
    for (int i = 0; i < g_resource_count; ++i) {
        try_commit_resource(&g_resources[i]);
    }
    for (int i = 0; i < g_alloc_count; ++i) {
        try_commit_alloc(&g_allocs[i]);
    }
    for (int i = 0; i < kMaxDeferred; ++i) {
        if (g_deferred[i].active) {
            g_deferred[i].active = false;
        }
    }
    note_retire_queue_depth();
}

static void complete_submission_record(SubmissionRecord* rec, CompletionSource source) {
    if (!rec || !rec->active || rec->completed) {
        return;
    }
    if (source == COMPLETION_UNKNOWN) {
        VT_TRACK_LOG(
            "RETIRE_BLOCKED_NO_COMPLETION_PROOF submission=%" PRIu64
            " queue=%d queueSerial=%" PRIu64 " (refusing complete without proof)",
            rec->id, rec->queue_index, rec->queue_serial);
        return;
    }
    rec->completed = true;
    rec->completion_verified = true;
    rec->completion_source = source;
    const uint64_t s = rec->id;
    const int qi = rec->queue_index;
    const uint64_t qserial = rec->queue_serial;
    const uint64_t old_completed =
        (qi >= 0 && qi < kMaxQueues && g_queues[qi].used) ? g_queues[qi].completed_serial
                                                          : g_last_completed;

    if (qi >= 0 && qi < kMaxQueues && g_queues[qi].used) {
        if (qserial > g_queues[qi].completed_serial) {
            g_queues[qi].completed_serial = qserial;
            g_queues[qi].completed_source = source;
        } else if (qserial == g_queues[qi].completed_serial) {
            g_queues[qi].completed_source = source;
        }
    }

    VT_TRACK_LOG(
        "INTERNAL_COMPLETION_OBSERVED queue=%d oldCompleted=%" PRIu64 " newCompleted=%" PRIu64
        " source=%s submission=%" PRIu64 " queueSerial=%" PRIu64,
        qi, old_completed,
        (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_serial : s,
        completion_source_str(source), s, qserial);

    for (int i = 0; i < rec->resource_count; ++i) {
        TrackedResource* r = find_resource_by_id(rec->resource_ids[i]);
        if (r) {
            r->last_completed_use = s;
            r->last_completion_source = source;
            if (r->pending_submission_refs > 0) {
                r->pending_submission_refs--;
            }
            /* Generation-aware lease completion for this resource's range. */
            if (r->allocation_id && r->kind == TRACK_RES_BUFFER) {
                const uint64_t rsz =
                    r->buffer_size > 0
                        ? r->buffer_size
                        : (r->requirements_size > 0 ? r->requirements_size : 0);
                SuballocLease* L =
                    find_lease_for_range(r->allocation_id, r->bind_offset, rsz);
                if (L && L->owner_resource == r->id) {
                    if (lease_gpu_uses_completed(L) && !L->cpu_write_owner) {
                        if (L->state != kSubLeaseReusable) {
                            L->state = kSubLeaseReusable;
                            VT_TRACK_LOG(
                                "SUBALLOC_RANGE_REUSABLE allocation=%" PRIu64
                                " offset=0x%" PRIx64 " size=0x%" PRIx64
                                " generation=%" PRIu64 " owner=%" PRIu64,
                                L->allocation_id, L->offset, L->size, L->generation,
                                L->owner_resource);
                        }
                    }
                } else if (L && L->owner_resource != r->id) {
                    /* Late completion for previous owner after reuse — ignore. */
                    g_sub_stats_stale_completion++;
                    static uint64_t s_stale;
                    if (rate_allow(&s_stale)) {
                        VT_TRACK_LOG(
                            "SUBALLOC_STALE_COMPLETION allocation=%" PRIu64
                            " offset=0x%" PRIx64 " generation=%" PRIu64
                            " owner=%" PRIu64 " completedOwner=%" PRIu64,
                            L->allocation_id, L->offset, L->generation, L->owner_resource,
                            r->id);
                    }
                }
            }
        }
    }
    for (int i = 0; i < rec->alloc_count; ++i) {
        TrackedGpuAllocation* a = find_alloc_by_id(rec->alloc_ids[i]);
        if (a) {
            a->last_completed_use = s;
            a->last_completion_source = source;
            if (a->pending_submission_refs > 0) {
                a->pending_submission_refs--;
            }
        }
    }
    for (int n = 0; n < kOpRing; ++n) {
        if (g_ops[n].submission == s) {
            g_ops[n].completed = true;
        }
    }
    durable_fence_complete_global(s);
    rec->active = false;
    if (s > g_last_completed) {
        g_last_completed = s;
    }
    g_last_completion_source = source;
}

static void complete_queue_up_to(int queue_index, uint64_t serial, CompletionSource source) {
    if (queue_index < 0 || queue_index >= kMaxQueues || !g_queues[queue_index].used) {
        return;
    }
    if (source == COMPLETION_UNKNOWN) {
        return;
    }
    if (serial < g_queues[queue_index].completed_serial) {
        return;
    }
    for (int i = 0; i < kSubRing; ++i) {
        SubmissionRecord* rec = &g_subs[i];
        if (!rec->active || rec->completed) {
            continue;
        }
        if (rec->queue_index == queue_index && rec->queue_serial <= serial) {
            complete_submission_record(rec, source);
        }
    }
    if (serial > g_queues[queue_index].completed_serial) {
        g_queues[queue_index].completed_serial = serial;
        g_queues[queue_index].completed_source = source;
    } else if (serial == g_queues[queue_index].completed_serial) {
        g_queues[queue_index].completed_source = source;
    }
    g_last_completion_source = source;
    VortekGpuTrack_collectRetired();
}

static void complete_all_queues_to_submitted(CompletionSource source) {
    if (source == COMPLETION_UNKNOWN) {
        VT_TRACK_LOG("RETIRE_BLOCKED_NO_COMPLETION_PROOF complete_all refused source=unknown");
        return;
    }
    for (int qi = 0; qi < g_queue_count; ++qi) {
        if (!g_queues[qi].used) {
            continue;
        }
        uint64_t old = g_queues[qi].completed_serial;
        uint64_t neu = g_queues[qi].last_submitted_serial;
        if (neu > old) {
            VT_TRACK_LOG(
                "DEVICE_WAIT_IDLE_COMPLETION_ADVANCE queue=%d oldCompleted=%" PRIu64
                " newCompleted=%" PRIu64 " source=%s",
                qi, old, neu, completion_source_str(source));
            complete_queue_up_to(qi, neu, source);
        }
    }
    for (int i = 0; i < kSubRing; ++i) {
        if (g_subs[i].active && !g_subs[i].completed) {
            complete_submission_record(&g_subs[i], source);
        }
    }
    if (g_last_submitted > g_last_completed) {
        g_last_completed = g_last_submitted;
    }
    g_last_completion_source = source;
    VortekGpuTrack_collectRetired();
}

static void queue_deferred_resource(TrackedResource* r) {
    for (int i = 0; i < kMaxDeferred; ++i) {
        if (!g_deferred[i].active) {
            g_deferred[i].active = true;
            g_deferred[i].kind =
                r->kind == TRACK_RES_BUFFER ? DEFER_BUFFER : DEFER_IMAGE;
            g_deferred[i].resource_or_alloc_id = r->id;
            g_deferred[i].handle = r->handle;
            g_deferred[i].device = r->device;
            g_deferred[i].rm = NULL;
            return;
        }
    }
    VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=deferred");
}

static void log_retire_request_resource(const TrackedResource* r) {
    int qi = -1;
    uint64_t use = min_incomplete_serial(r->last_use, &qi);
    if (use == 0) {
        use = r->last_submitted_use;
    }
    uint64_t completed =
        (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_serial : g_last_completed;
    VT_TRACK_LOG(
        "RETIRE_REQUEST resource=%" PRIu64 " kind=%s queue=%d lastUse=%" PRIu64
        " completed=%" PRIu64 " allocation=%" PRIu64 " externalBacking=%" PRIu64,
        r->id, kind_str(r->kind), qi, use, completed, r->allocation_id,
        r->external_backing_id);
    g_retire_request_count++;
}

void VortekGpuTrack_onAlloc(ResourceMemory* rm, uint32_t memoryTypeIndex,
                            const char* backingType) {
    VortekGpuTrack_initOnce();
    if (!rm) {
        return;
    }
    TrackedGpuAllocation* a = alloc_alloc_slot();
    if (!a) {
        return;
    }
    memset(a, 0, sizeof(*a));
    clear_queue_uses(a->last_use);
    a->id = g_next_alloc_id++;
    /* Always distinct from allocation id — never collapse externalBacking=allocation. */
    a->backing_id = g_next_backing_id++;
    a->mapping_id = 0;
    a->gpu_va_mapping_id = 0;
    a->rm = rm;
    a->memory_handle = (void*)(uintptr_t)rm->memory;
    a->allocation_size = (uint64_t)rm->allocationSize;
    a->memory_type_index = memoryTypeIndex;
    a->gpu_size = a->allocation_size;
    a->guest_size = a->allocation_size;
    a->alive = true;
    a->physically_freed = false;
    a->created_submission = g_last_submitted;
    if (backingType) {
        strncpy(a->backing_type, backingType, sizeof(a->backing_type) - 1);
    } else {
        strncpy(a->backing_type, "unknown", sizeof(a->backing_type) - 1);
    }
    a->external_backing =
        (strcmp(a->backing_type, "external") == 0) ||
        (strcmp(a->backing_type, "ahb") == 0) ||
        (strcmp(a->backing_type, "dma_buf") == 0);
    a->bind_generation = 1;
    a->original_fd = rm->fd;
    a->owned_fd = rm->fd;
    a->fd_closed = false;
    a->gpu_bind_address = 0;
    a->gpu_bind_size = 0;
    a->gpu_address_bound = false;
    a->bind_gen_at_gpu_bind = 0;
    a->last_vk_memory = a->memory_handle;
    a->fhd_pinned = g_pin_fhd_detile_sources && is_fhd_class_size(a->allocation_size);

    VT_TRACK_LOG(
        "VORTEK_ALLOC allocationId=%" PRIu64 " externalBackingId=%" PRIu64
        " memory=%p vkMemory=%p allocSize=0x%" PRIx64 " memoryType=%u guestVA=0x0 "
        "guestSize=0x%" PRIx64 " gpuVA=0x0 gpuSize=0x%" PRIx64
        " backingType=%s external=%d createdSubmission=%" PRIu64
        " bindGeneration=%" PRIu64 " originalFd=%d ownedFd=%d fhdPinned=%d",
        a->id, a->backing_id, (void*)rm, a->memory_handle, a->allocation_size,
        a->memory_type_index, a->guest_size, a->gpu_size, a->backing_type,
        a->external_backing ? 1 : 0, a->created_submission, a->bind_generation,
        a->original_fd, a->owned_fd, a->fhd_pinned ? 1 : 0);
    if (a->external_backing) {
        VT_TRACK_LOG(
            "EXTERNAL_FD_IMPORT allocation=%" PRIu64 " backing=%" PRIu64
            " originalFd=%d ownedFd=%d size=0x%" PRIx64 " path=onAlloc",
            a->id, a->backing_id, a->original_fd, a->owned_fd, a->allocation_size);
    }
    if (a->fhd_pinned) {
        VT_TRACK_LOG(
            "FHD_SOURCE_PINNED_RETENTION allocation=%" PRIu64 " size=0x%" PRIx64
            " action=pin_at_alloc path=onAlloc",
            a->id, a->allocation_size);
    }
}

void VortekGpuTrack_onMap(ResourceMemory* rm) {
    VortekGpuTrack_initOnce();
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!a) {
        VT_TRACK_LOG("UNTRACKED_VK_UNMAP_MEMORY where=onMap rm=%p (alloc missing)", (void*)rm);
        return;
    }
    a->mapped = true;
    a->mapping_unmapped = false;
    if (a->mapping_id == 0) {
        a->mapping_id = g_next_mapping_id++;
        VT_TRACK_LOG(
            "GPU_MAPPING_CREATE allocation=%" PRIu64 " mapping=%" PRIu64 " size=0x%" PRIx64
            " lastGpuRead=%" PRIu64 " completedSerial=%" PRIu64,
            a->id, a->mapping_id, a->allocation_size, a->last_gpu_read_submission,
            g_last_completed);
    }
    a->last_cpu_write = g_last_submitted;
    /* Do not pin leases as cpu_write_owner for whole-map — guest maps large heaps.
     * prepareCpuWrite already waited if a range was in-flight. */
    VT_TRACK_LOG(
        "VORTEK_MAP allocationId=%" PRIu64 " mappingId=%" PRIu64 " externalBackingId=%" PRIu64
        " memory=%p fd=%d mapped=1",
        a->id, a->mapping_id, a->backing_id, (void*)rm, rm ? rm->fd : -1);
    if (is_fhd_class_size(a->allocation_size) || a->last_gpu_read_submission > g_last_completed) {
        VT_TRACK_LOG(
            "GPU_MAPPING_USE allocation=%" PRIu64 " mapping=%" PRIu64 " size=0x%" PRIx64
            " lastGpuRead=%" PRIu64 " completedSerial=%" PRIu64 " path=onMap",
            a->id, a->mapping_id, a->allocation_size, a->last_gpu_read_submission,
            g_last_completed);
    }
}

bool VortekGpuTrack_onUnmap(ResourceMemory* rm, void* device) {
    VortekGpuTrack_initOnce();
    (void)device;
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!a) {
        VT_TRACK_LOG("UNTRACKED_VK_UNMAP_MEMORY rm=%p", (void*)rm);
        return true;
    }
    a->unmap_requested = true;
    a->mapping_unmap_requested = true;
    VT_TRACK_LOG(
        "GPU_MAPPING_UNMAP_REQUEST allocation=%" PRIu64 " mapping=%" PRIu64
        " lastGpuRead=%" PRIu64 " lastSubmitted=%" PRIu64 " completedSerial=%" PRIu64
        " pendingRefs=%u fhdPinned=%d gpuBound=%d",
        a->id, a->mapping_id, a->last_gpu_read_submission, a->last_submitted_use,
        g_last_completed, a->pending_submission_refs, a->fhd_pinned ? 1 : 0,
        a->gpu_address_bound ? 1 : 0);
    if (a->fhd_pinned || (g_pin_fhd_detile_sources && is_fhd_class_size(a->allocation_size))) {
        a->fhd_pinned = true;
        VT_TRACK_LOG(
            "FHD_SOURCE_PINNED_RETENTION allocation=%" PRIu64 " mapping=%" PRIu64
            " action=block_unmap path=onUnmap",
            a->id, a->mapping_id);
        return false;
    }
    if (a->last_gpu_read_submission > g_last_completed) {
        VT_TRACK_LOG(
            "DETILE_SOURCE_GPU_MAPPING_CHANGED_IN_FLIGHT allocation=%" PRIu64
            " mapping=%" PRIu64 " lastGpuRead=%" PRIu64 " completedSerial=%" PRIu64
            " path=onUnmap exactWait=%d",
            a->id, a->mapping_id, a->last_gpu_read_submission, g_last_completed,
            g_detile_source_exact_wait ? 1 : 0);
        /* Only hard-block unmap when exact wait enabled (default off — SEGA freeze). */
        if (g_detile_source_exact_wait) {
            return false;
        }
    }
    if (!request_physical_release(VORTEK_RELEASE_UNMAP_MEMORY, a->mapping_id ? a->mapping_id
                                                                             : a->id,
                                  a->memory_handle, a->last_use, a->last_submitted_use,
                                  a->pending_submission_refs, 0, NULL, "mapping")) {
        VT_TRACK_LOG(
            "MAPPING_UNMAP_DEFERRED mappingId=%" PRIu64 " allocationId=%" PRIu64
            " lastUse=%" PRIu64 " completed=%" PRIu64,
            a->mapping_id, a->id, a->last_submitted_use, g_last_completed);
        return false;
    }
    a->mapped = false;
    a->mapping_unmapped = true;
    VT_TRACK_LOG(
        "GPU_MAPPING_UNMAP_COMMIT allocation=%" PRIu64 " mapping=%" PRIu64
        " completedSerial=%" PRIu64,
        a->id, a->mapping_id, g_last_completed);
    return true;
}

void VortekGpuTrack_onGpuVaMap(ResourceMemory* rm, uint64_t gpuVa, uint64_t gpuSize) {
    VortekGpuTrack_initOnce();
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!a) {
        VT_TRACK_LOG("UNTRACKED_GPU_VA_UNMAP where=onGpuVaMap rm=%p", (void*)rm);
        return;
    }
    if (a->gpu_va_mapping_id == 0) {
        a->gpu_va_mapping_id = g_next_gpu_va_mapping_id++;
    }
    a->gpu_va = gpuVa;
    a->gpu_size = gpuSize ? gpuSize : a->allocation_size;
    a->gpu_va_unmapped = false;
    VT_TRACK_LOG(
        "GPU_VA_MAP allocationId=%" PRIu64 " gpuVaMappingId=%" PRIu64 " gpuVA=0x%" PRIx64
        " gpuSize=0x%" PRIx64 " externalBackingId=%" PRIu64,
        a->id, a->gpu_va_mapping_id, a->gpu_va, a->gpu_size, a->backing_id);
}

void VortekGpuTrack_onGuestVaMap(ResourceMemory* rm, uint64_t guestVa, uint64_t guestSize) {
    VortekGpuTrack_initOnce();
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!a) {
        return;
    }
    if (a->mapping_id == 0) {
        a->mapping_id = g_next_mapping_id++;
    }
    a->guest_va = guestVa;
    a->guest_size = guestSize ? guestSize : a->allocation_size;
    VT_TRACK_LOG(
        "GUEST_VA_MAP allocationId=%" PRIu64 " mappingId=%" PRIu64 " guestVA=0x%" PRIx64
        " guestSize=0x%" PRIx64,
        a->id, a->mapping_id, a->guest_va, a->guest_size);
}

bool VortekGpuTrack_onFree(ResourceMemory* rm, void* device) {
    VortekGpuTrack_initOnce();
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!a) {
        VT_TRACK_LOG("UNTRACKED_VK_FREE_MEMORY rm=%p device=%p", (void*)rm, device);
        return true;
    }
    a->device = device;
    a->destroy_requested = true;
    a->free_requested = true;
    a->destroy_requested_submission = g_last_submitted;
    recount_alloc_children(a);
    if (a->fhd_pinned || (g_pin_fhd_detile_sources && is_fhd_class_size(a->allocation_size))) {
        a->fhd_pinned = true;
        VT_TRACK_LOG(
            "FHD_SOURCE_PINNED_RETENTION allocation=%" PRIu64 " backing=%" PRIu64
            " size=0x%" PRIx64 " action=block_free path=onFree lastGpuRead=%" PRIu64
            " completed=%" PRIu64 " gpuBound=%d",
            a->id, a->backing_id, a->allocation_size, a->last_gpu_read_submission,
            g_last_completed, a->gpu_address_bound ? 1 : 0);
        if (a->gpu_address_bound) {
            VT_TRACK_LOG(
                "EXTERNAL_BACKING_RELEASED_WHILE_BOUND allocation=%" PRIu64
                " backing=%" PRIu64 " baseAddress=0x%" PRIx64 " size=0x%" PRIx64
                " blocked=1 path=onFree",
                a->id, a->backing_id, a->gpu_bind_address, a->gpu_bind_size);
        }
        a->retire_sticky_block = true;
        return false;
    }
    if (a->gpu_address_bound &&
        (a->last_gpu_read_submission > g_last_completed || a->pending_submission_refs > 0)) {
        VT_TRACK_LOG(
            "EXTERNAL_BACKING_RELEASED_WHILE_BOUND allocation=%" PRIu64 " backing=%" PRIu64
            " baseAddress=0x%" PRIx64 " lastGpuRead=%" PRIu64 " completed=%" PRIu64
            " pendingRefs=%u",
            a->id, a->backing_id, a->gpu_bind_address, a->last_gpu_read_submission,
            g_last_completed, a->pending_submission_refs);
    }

    const bool blocked = !alloc_can_retire(a);
    if (blocked) {
        int qi = -1;
        uint64_t use = min_incomplete_serial(a->last_use, &qi);
        if (use == 0) {
            use = a->last_submitted_use;
        }
        uint64_t completed =
            (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_serial : g_last_completed;

        /* Find referencing children for BACKING log. */
        char refs[128];
        int n = 0;
        refs[0] = '\0';
        for (int i = 0; i < g_resource_count && n < 100; ++i) {
            TrackedResource* r = &g_resources[i];
            if (r->allocation_id == a->id && !r->actually_destroyed) {
                n += snprintf(refs + n, sizeof(refs) - (size_t)n, "%s%" PRIu64,
                              n ? "," : "", r->id);
            }
        }

        a->backing_release_requested = true;
        VT_TRACK_LOG(
            "BACKING_RETIRE_REQUEST backing=%" PRIu64 " allocation=%" PRIu64 " mapping=%" PRIu64
            " lastUse=%" PRIu64 " completed=%" PRIu64,
            a->backing_id, a->id, a->mapping_id, use, completed);
        VT_TRACK_LOG(
            "BACKING_RELEASE_IN_FLIGHT backing=%" PRIu64 " allocation=%" PRIu64
            " referencedBy=%s lastUse=%" PRIu64 " completed=%" PRIu64 " liveChildren=%u "
            "pendingChildren=%u pendingRefs=%u",
            a->backing_id, a->id, refs[0] ? refs : "-", use, completed,
            a->live_child_objects, a->pending_child_objects, a->pending_submission_refs);
        if (a->mapped) {
            VT_TRACK_LOG(
                "MAPPING_UNMAP_DEFERRED mapping=%" PRIu64 " allocation=%" PRIu64
                " lastUse=%" PRIu64 " completed=%" PRIu64,
                a->mapping_id, a->id, use, completed);
        }
        if (a->external_backing) {
            VT_TRACK_LOG(
                "EXTERNAL_HANDLE_RELEASE_DEFERRED backing=%" PRIu64 " allocation=%" PRIu64
                " lastUse=%" PRIu64 " completed=%" PRIu64,
                a->backing_id, a->id, use, completed);
        }
        VT_TRACK_LOG(
            "RESOURCE_FREED_IN_FLIGHT resource=alloc-%" PRIu64 " lastUseSubmission=%" PRIu64
            " completedSubmission=%" PRIu64 " lastSubmitted=%" PRIu64 " pendingRefs=%u",
            a->id, a->last_submitted_use, g_last_completed, g_last_submitted,
            a->pending_submission_refs);
        VT_TRACK_LOG(
            "ALLOCATION_RETIRE_BLOCKED allocation=%" PRIu64 " liveChildren=%u "
            "pendingChildren=%u graphicsUse=%" PRIu64 " graphicsCompleted=%" PRIu64,
            a->id, a->live_child_objects, a->pending_child_objects, use, completed);

        if (g_defer_destroy) {
            VT_TRACK_LOG(
                "RESOURCE_DESTROY_DEFERRED allocation=%" PRIu64 " lastUse=%" PRIu64
                " completed=%" PRIu64 " pendingRefs=%u",
                a->id, a->last_submitted_use, g_last_completed, a->pending_submission_refs);
            VT_TRACK_LOG(
                "RETIRE_REQUEST resource=alloc-%" PRIu64 " kind=memory queue=%d "
                "lastUse=%" PRIu64 " completed=%" PRIu64 " allocation=%" PRIu64
                " externalBacking=%d",
                a->id, qi, use, completed, a->id, a->external_backing ? 1 : 0);
            g_retire_request_count++;
            note_retire_queue_depth();
            return false;
        }
    }

    recount_alloc_children(a);
    if (!request_physical_release(
            a->external_backing ? VORTEK_RELEASE_EXTERNAL : VORTEK_RELEASE_FREE_MEMORY, a->id,
            a->memory_handle, a->last_use, a->last_submitted_use, a->pending_submission_refs,
            a->live_child_objects + a->pending_child_objects, &a->destroy_requested,
            a->external_backing ? "external_backing" : "memory")) {
        a->retire_sticky_block = true;
        a->last_retire_attempt_completed = g_last_completed;
        a->last_retire_attempt_refs = a->pending_submission_refs;
        VT_TRACK_LOG(
            "RESOURCE_DESTROY_DEFERRED allocation=%" PRIu64 " lastUse=%" PRIu64
            " completed=%" PRIu64 " pendingRefs=%u (physical release blocked)",
            a->id, a->last_submitted_use, g_last_completed, a->pending_submission_refs);
        note_retire_queue_depth();
        return false;
    }

    a->actual_free_submission = g_last_submitted;
    VT_TRACK_LOG(
        "VORTEK_FREE allocationId=%" PRIu64 " externalBackingId=%" PRIu64 " mappingId=%" PRIu64
        " gpuVaMappingId=%" PRIu64 " gpuStart=0x0 gpuEnd=0x%" PRIx64
        " lastSubmission=%" PRIu64 " completedSubmission=%" PRIu64
        " destroyRequestedSubmission=%" PRIu64 " actualFreeSubmission=%" PRIu64
        " lastGpuRead=%" PRIu64 " lastGpuWrite=%" PRIu64 " pendingRefs=%u",
        a->id, a->backing_id, a->mapping_id, a->gpu_va_mapping_id, a->allocation_size,
        a->last_submitted_use, g_last_completed, a->destroy_requested_submission,
        a->actual_free_submission, a->last_gpu_read_submission, a->last_gpu_write_submission,
        a->pending_submission_refs);
    if (a->external_backing) {
        VT_TRACK_LOG(
            "BACKING_RETIRE_COMMIT backing=%" PRIu64 " allocation=%" PRIu64 " mapping=%" PRIu64
            " gpuVaMapping=%" PRIu64 " lastUse=%" PRIu64 " completed=%" PRIu64
            " (immediate free path)",
            a->backing_id, a->id, a->mapping_id, a->gpu_va_mapping_id, a->last_submitted_use,
            g_last_completed);
    }

    a->alive = false;
    a->actually_destroyed = true;
    a->physically_freed = true;
    a->backing_released = true;
    a->rm = NULL;
    a->mapped = false;
    a->destroy_requested = false;
    return true;
}

void VortekGpuTrack_onCreateBuffer(void* buffer, uint64_t size, uint32_t usage) {
    VortekGpuTrack_initOnce();
    if (!buffer) {
        return;
    }
    TrackedResource* r = alloc_resource_slot();
    if (!r) {
        return;
    }
    memset(r, 0, sizeof(*r));
    clear_queue_uses(r->last_use);
    r->id = g_next_resource_id++;
    r->handle = buffer;
    r->kind = TRACK_RES_BUFFER;
    r->alive = true;
    r->guest_visible = true;
    r->buffer_size = size;
    r->usage = usage;
    VT_TRACK_LOG("VORTEK_CREATE_BUFFER resource=%" PRIu64 " buffer=%p size=0x%" PRIx64
                 " usage=0x%x",
                 r->id, buffer, size, usage);
}

bool VortekGpuTrack_onDestroyBuffer(void* buffer, void* device) {
    VortekGpuTrack_initOnce();
    TrackedResource* r = find_resource(buffer);
    if (!r) {
        /* May already be destroy_requested — search non-visible. */
        for (int i = 0; i < g_resource_count; ++i) {
            if (g_resources[i].handle == buffer && !g_resources[i].actually_destroyed) {
                r = &g_resources[i];
                break;
            }
        }
    }
    if (!r) {
        VT_TRACK_LOG("UNTRACKED_VK_DESTROY_BUFFER buffer=%p device=%p", buffer, device);
        /* Fail closed after first frame: do not destroy untracked post-game buffers. */
        if (g_first_frame_seen) {
            g_buffer_retire_block_count++;
            VT_TRACK_LOG("BUFFER_RETIRE_BLOCKED_UNKNOWN_USE resource=0 handle=%p untracked=1",
                         buffer);
            return false;
        }
        return true;
    }
    r->device = device;
    r->destroy_requested = true;
    r->guest_visible = false;
    r->destroy_requested_submission = g_last_submitted;

    if (r->allocation_id) {
        TrackedGpuAllocation* a = find_alloc_by_id(r->allocation_id);
        if (a) {
            recount_alloc_children(a);
            if (a->fhd_pinned ||
                (g_pin_fhd_detile_sources &&
                 (is_fhd_class_size(a->allocation_size) || is_fhd_class_size(r->buffer_size)))) {
                a->fhd_pinned = true;
                VT_TRACK_LOG(
                    "FHD_SOURCE_PINNED_RETENTION allocation=%" PRIu64 " resource=%" PRIu64
                    " action=block_destroy_buffer path=onDestroyBuffer",
                    a->id, r->id);
                r->retire_sticky_block = true;
                queue_deferred_resource(r);
                return false;
            }
        }
    } else if (g_pin_fhd_detile_sources && is_fhd_class_size(r->buffer_size)) {
        VT_TRACK_LOG(
            "FHD_SOURCE_PINNED_RETENTION resource=%" PRIu64
            " action=block_destroy_buffer path=onDestroyBuffer no_alloc",
            r->id);
        r->retire_sticky_block = true;
        queue_deferred_resource(r);
        return false;
    }

    const uint64_t last_use = r->last_submitted_use;
    if (resource_in_flight(r)) {
        g_buffer_retire_block_count++;
        VT_TRACK_LOG(
            "BUFFER_RETIRE_BLOCKED_IN_FLIGHT resource=%" PRIu64 " lastUse=%" PRIu64
            " completed=%" PRIu64 " pendingRefs=%u",
            r->id, last_use, g_last_completed, r->pending_submission_refs);
        VT_TRACK_LOG(
            "RESOURCE_FREED_IN_FLIGHT resource=%" PRIu64
            " kind=buffer lastUseSubmission=%" PRIu64 " completedSubmission=%" PRIu64
            " pendingRefs=%u allocationId=%" PRIu64 " externalBackingId=%" PRIu64
            " mappingId=%" PRIu64 " bindOffset=0x%" PRIx64,
            r->id, last_use, g_last_completed, r->pending_submission_refs, r->allocation_id,
            r->external_backing_id, r->mapping_id, r->bind_offset);
        log_retire_request_resource(r);
        VT_TRACK_LOG(
            "RESOURCE_DESTROY_DEFERRED resource=%" PRIu64 " lastUse=%" PRIu64
            " completed=%" PRIu64 " pendingRefs=%u",
            r->id, last_use, g_last_completed, r->pending_submission_refs);
        queue_deferred_resource(r);
        note_retire_queue_depth();
        return false;
    }

    /* Physical release gate — quarantine / unknown-use / BDA / completion. */
    if (!request_physical_release(VORTEK_RELEASE_DESTROY_BUFFER, r->id, r->handle, r->last_use,
                                  last_use, r->pending_submission_refs, 0, NULL, "buffer")) {
        if (last_use == 0 && g_first_frame_seen) {
            r->unknown_use_retained = true;
        }
        queue_deferred_resource(r);
        note_retire_queue_depth();
        return false;
    }

    VT_TRACK_LOG("VORTEK_DESTROY_BUFFER resource=%" PRIu64 " buffer=%p lastUse=%" PRIu64
                 " pendingRefs=%u completed=%" PRIu64 " allocationId=%" PRIu64
                 " externalBackingId=%" PRIu64,
                 r->id, buffer, last_use, r->pending_submission_refs, g_last_completed,
                 r->allocation_id, r->external_backing_id);
    VT_TRACK_LOG(
        "BUFFER_RETIRE_COMMIT resource=%" PRIu64 " lastUseQueue=%d lastUseSerial=%" PRIu64
        " completedSerial=%" PRIu64 " completionSource=%s allocation=%" PRIu64
        " backing=%" PRIu64 " handle=%p",
        r->id, g_current_queue_index, last_use, g_last_completed,
        completion_source_str(r->last_completion_source), r->allocation_id,
        r->external_backing_id, buffer);
    VT_TRACK_LOG("RETIRE_COMMIT resource=%" PRIu64 " kind=buffer queue=-1 lastUse=%" PRIu64
                 " completed=%" PRIu64 " completionSource=immediate",
                 r->id, last_use, g_last_completed);
    r->alive = false;
    r->actually_destroyed = true;
    r->physically_released = true;
    r->destroy_requested = false;
    r->unknown_use_retained = false;
    g_retire_commit_count++;
    g_buffer_retire_commit_count++;
    if (r->allocation_id) {
        TrackedGpuAllocation* a = find_alloc_by_id(r->allocation_id);
        if (a) {
            recount_alloc_children(a);
        }
    }
    return true;
}

void VortekGpuTrack_onCreateImage(void* image, uint32_t width, uint32_t height,
                                  uint32_t format, uint32_t usage) {
    VortekGpuTrack_initOnce();
    if (!image) {
        return;
    }
    TrackedResource* r = alloc_resource_slot();
    if (!r) {
        return;
    }
    memset(r, 0, sizeof(*r));
    clear_queue_uses(r->last_use);
    r->id = g_next_resource_id++;
    r->handle = image;
    r->kind = TRACK_RES_IMAGE;
    r->alive = true;
    r->guest_visible = true;
    r->width = width;
    r->height = height;
    r->format = format;
    r->usage = usage;
    VT_TRACK_LOG("VORTEK_CREATE_IMAGE resource=%" PRIu64 " image=%p width=%u height=%u "
                 "format=0x%x usage=0x%x",
                 r->id, image, width, height, format, usage);
}

bool VortekGpuTrack_onDestroyImage(void* image, void* device) {
    VortekGpuTrack_initOnce();
    TrackedResource* r = find_resource(image);
    if (!r) {
        for (int i = 0; i < g_resource_count; ++i) {
            if (g_resources[i].handle == image && !g_resources[i].actually_destroyed) {
                r = &g_resources[i];
                break;
            }
        }
    }
    if (!r) {
        VT_TRACK_LOG("UNTRACKED_VK_DESTROY_IMAGE image=%p device=%p", image, device);
        if (g_first_frame_seen) {
            VT_TRACK_LOG("IMAGE_RETIRE_BLOCKED_UNKNOWN_USE resource=0 handle=%p untracked=1",
                         image);
            return false;
        }
        return true;
    }
    r->device = device;
    r->destroy_requested = true;
    r->guest_visible = false;
    r->destroy_requested_submission = g_last_submitted;

    if (r->allocation_id) {
        TrackedGpuAllocation* a = find_alloc_by_id(r->allocation_id);
        if (a) {
            recount_alloc_children(a);
        }
    }

    const uint64_t last_use = r->last_submitted_use;
    if (resource_in_flight(r)) {
        VT_TRACK_LOG(
            "RESOURCE_FREED_IN_FLIGHT resource=%" PRIu64
            " kind=image lastUseSubmission=%" PRIu64 " completedSubmission=%" PRIu64
            " pendingRefs=%u allocationId=%" PRIu64 " externalBackingId=%" PRIu64
            " bindOffset=0x%" PRIx64,
            r->id, last_use, g_last_completed, r->pending_submission_refs, r->allocation_id,
            r->external_backing_id, r->bind_offset);
        log_retire_request_resource(r);
        VT_TRACK_LOG(
            "RESOURCE_DESTROY_DEFERRED resource=%" PRIu64 " lastUse=%" PRIu64
            " completed=%" PRIu64 " pendingRefs=%u",
            r->id, last_use, g_last_completed, r->pending_submission_refs);
        queue_deferred_resource(r);
        note_retire_queue_depth();
        return false;
    }

    if (!request_physical_release(VORTEK_RELEASE_DESTROY_IMAGE, r->id, r->handle, r->last_use,
                                  last_use, r->pending_submission_refs, 0, NULL, "image")) {
        if (last_use == 0 && g_first_frame_seen) {
            r->unknown_use_retained = true;
        }
        r->retire_sticky_block = true;
        queue_deferred_resource(r);
        note_retire_queue_depth();
        return false;
    }

    VT_TRACK_LOG("VORTEK_DESTROY_IMAGE resource=%" PRIu64 " image=%p lastUse=%" PRIu64
                 " pendingRefs=%u completed=%" PRIu64 " allocationId=%" PRIu64
                 " externalBackingId=%" PRIu64,
                 r->id, image, last_use, r->pending_submission_refs, g_last_completed,
                 r->allocation_id, r->external_backing_id);
    VT_TRACK_LOG(
        "IMAGE_RETIRE_COMMIT resource=%" PRIu64 " kind=image lastUseQueue=-1 "
        "lastUseSerial=%" PRIu64 " completedSerial=%" PRIu64 " completionSource=immediate "
        "allocation=%" PRIu64 " handle=%p",
        r->id, last_use, g_last_completed, r->allocation_id, image);
    VT_TRACK_LOG("RETIRE_COMMIT resource=%" PRIu64 " kind=image queue=-1 lastUse=%" PRIu64
                 " completed=%" PRIu64 " completionSource=immediate",
                 r->id, last_use, g_last_completed);
    r->alive = false;
    r->actually_destroyed = true;
    r->physically_released = true;
    r->destroy_requested = false;
    r->retire_sticky_block = false;
    g_retire_commit_count++;
    if (r->allocation_id) {
        TrackedGpuAllocation* a = find_alloc_by_id(r->allocation_id);
        if (a) {
            recount_alloc_children(a);
        }
    }
    return true;
}

void VortekGpuTrack_onCreateImageView(void* imageView, void* image) {
    VortekGpuTrack_initOnce();
    if (!imageView) {
        return;
    }
    TrackedResource* parent = find_resource(image);
    TrackedResource* r = alloc_resource_slot();
    if (!r) {
        return;
    }
    memset(r, 0, sizeof(*r));
    clear_queue_uses(r->last_use);
    r->id = g_next_resource_id++;
    r->handle = imageView;
    r->kind = TRACK_RES_IMAGE_VIEW;
    r->alive = true;
    r->guest_visible = true;
    r->parent_image_id = parent ? parent->id : 0;
    r->allocation_id = parent ? parent->allocation_id : 0;
    r->external_backing_id = parent ? parent->external_backing_id : 0;
    r->mapping_id = parent ? parent->mapping_id : 0;
    VT_TRACK_LOG(
        "VORTEK_CREATE_IMAGE_VIEW resource=%" PRIu64 " view=%p parentImage=%" PRIu64
        " allocationId=%" PRIu64,
        r->id, imageView, r->parent_image_id, r->allocation_id);
}

bool VortekGpuTrack_onDestroyImageView(void* imageView, void* device) {
    VortekGpuTrack_initOnce();
    TrackedResource* r = find_resource(imageView);
    if (!r) {
        for (int i = 0; i < g_resource_count; ++i) {
            if (g_resources[i].handle == imageView && !g_resources[i].actually_destroyed) {
                r = &g_resources[i];
                break;
            }
        }
    }
    if (!r) {
        VT_TRACK_LOG("UNTRACKED_IMAGE_VIEW view=%p device=%p", imageView, device);
        if (g_first_frame_seen) {
            return false;
        }
        return true;
    }
    r->device = device;
    r->destroy_requested = true;
    r->guest_visible = false;
    r->destroy_requested_submission = g_last_submitted;
    if (!request_physical_release(VORTEK_RELEASE_IMAGE_VIEW, r->id, r->handle, r->last_use,
                                  r->last_submitted_use, r->pending_submission_refs, 0, NULL,
                                  "image_view")) {
        r->retire_sticky_block = true;
        queue_deferred_resource(r);
        return false;
    }
    r->alive = false;
    r->actually_destroyed = true;
    r->physically_released = true;
    r->destroy_requested = false;
    r->retire_sticky_block = false;
    return true;
}

void VortekGpuTrack_onCommandPoolReset(void* commandPool, void* device) {
    VortekGpuTrack_initOnce();
    (void)device;
    VT_TRACK_LOG("CMDPOOL_RESET pool=%p", commandPool);
    /* Diagnostic: class wait applied by request_handler. */
}

void VortekGpuTrack_onCommandPoolDestroy(void* commandPool, void* device) {
    VortekGpuTrack_initOnce();
    (void)device;
    VT_TRACK_LOG("CMDPOOL_DESTROY pool=%p", commandPool);
    push_phys_rel(VORTEK_RELEASE_CMDPOOL, 0, commandPool, 1, "cmdpool_destroy");
}

void VortekGpuTrack_onBindBuffer(void* buffer, ResourceMemory* rm, uint64_t bindOffset,
                                 uint64_t requirementsSize, uint64_t requirementsAlignment) {
    VortekGpuTrack_initOnce();
    TrackedResource* r = find_resource(buffer);
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!r) {
        VortekGpuTrack_onCreateBuffer(buffer, requirementsSize, 0);
        r = find_resource(buffer);
    }
    if (!r) {
        return;
    }
    r->rm = rm;
    r->allocation_id = a ? a->id : 0;
    r->external_backing_id = a ? a->backing_id : 0;
    r->mapping_id = a ? a->mapping_id : 0;
    r->gpu_va_mapping_id = a ? a->gpu_va_mapping_id : 0;
    r->bind_offset = bindOffset;
    r->requirements_size = requirementsSize;
    r->requirements_alignment = requirementsAlignment;
    if (r->buffer_size == 0) {
        r->buffer_size = requirementsSize;
    }
    if (a) {
        void* cur_mem = (void*)(uintptr_t)rm->memory;
        if (a->last_vk_memory && cur_mem && a->last_vk_memory != cur_mem) {
            a->bind_generation++;
            VT_TRACK_LOG(
                "GPU_BINDING_REPLACED_IN_FLIGHT allocation=%" PRIu64 " resource=%" PRIu64
                " oldMemory=%p newMemory=%p generation=%" PRIu64 " lastGpuRead=%" PRIu64
                " completed=%" PRIu64 " pendingRefs=%u fhdPinned=%d",
                a->id, r->id, a->last_vk_memory, cur_mem, a->bind_generation,
                a->last_gpu_read_submission, g_last_completed, a->pending_submission_refs,
                a->fhd_pinned ? 1 : 0);
            if (a->fhd_pinned || (g_pin_fhd_detile_sources && is_fhd_class_size(a->allocation_size))) {
                a->fhd_pinned = true;
                VT_TRACK_LOG(
                    "FHD_SOURCE_PINNED_RETENTION allocation=%" PRIu64
                    " action=note_memory_replace path=onBindBuffer generation=%" PRIu64,
                    a->id, a->bind_generation);
            }
        }
        a->last_vk_memory = cur_mem ? cur_mem : a->memory_handle;
        a->memory_handle = a->last_vk_memory;
        if (a->bind_generation == 0) {
            a->bind_generation = 1;
        }
        r->bind_generation = a->bind_generation;
        if (g_pin_fhd_detile_sources && is_fhd_class_size(a->allocation_size)) {
            a->fhd_pinned = true;
        }
        recount_alloc_children(a);
        if (g_fhd_prepare_write_diag &&
            (is_fhd_class_size(a->allocation_size) || is_fhd_class_size(r->buffer_size))) {
            const bool read_inflight = resource_gpu_read_incomplete(r) ||
                                       (a->last_gpu_read_submission > g_last_completed);
            uint64_t completed_q = g_last_completed;
            for (int u = 0; u < kMaxQueueUses; ++u) {
                if (r->last_use[u].queue_index >= 0 && r->last_use[u].queue_index < kMaxQueues &&
                    g_queues[r->last_use[u].queue_index].used) {
                    uint64_t cs = g_queues[r->last_use[u].queue_index].completed_serial;
                    if (cs < completed_q) {
                        completed_q = cs;
                    }
                }
            }
            VT_TRACK_LOG(
                "FHD_PREPARE_WRITE_CHECK resource=%" PRIu64 " allocation=%" PRIu64
                " generation=%" PRIu64 " path=BindReplace lastGpuRead=%" PRIu64
                " lastSubmittedUse=%" PRIu64 " completedGlobal=%" PRIu64
                " completedQueueSerial=%" PRIu64 " checkHit=1 wouldBlock=%d",
                r->id, a->id, a->bind_generation, r->last_gpu_read_submission,
                r->last_submitted_use, g_last_completed, completed_q, read_inflight ? 1 : 0);
        }
    }

    const uint64_t effective_size =
        r->buffer_size > 0 ? r->buffer_size : requirementsSize;
    const uint64_t gpu_start = bindOffset;
    const uint64_t gpu_end = bindOffset + effective_size;
    const bool align_ok =
        requirementsAlignment == 0 || (bindOffset % requirementsAlignment) == 0;
    const bool fits =
        a ? VortekGpuTrack_rangeFits(a->allocation_size, bindOffset, 0, effective_size)
          : false;

    VT_TRACK_LOG(
        "VORTEK_BIND_BUFFER resource=%" PRIu64 " allocation=%" PRIu64 " buffer=%p "
        "bindOffset=0x%" PRIx64 " requirementsSize=0x%" PRIx64
        " requirementsAlignment=0x%" PRIx64 " effectiveGpuStart=0x%" PRIx64
        " effectiveGpuEnd=0x%" PRIx64 " alignOk=%d rangeFits=%d externalBacking=%" PRIu64,
        r->id, r->allocation_id, buffer, bindOffset, requirementsSize,
        requirementsAlignment, gpu_start, gpu_end, align_ok ? 1 : 0, fits ? 1 : 0,
        r->external_backing_id);

    if (!align_ok || !fits) {
        VT_TRACK_LOG(
            "GPU_RANGE_INVALID submission=%" PRIu64 " operation=bind_buffer resource=%" PRIu64
            " allocation=%" PRIu64 " bindOffset=0x%" PRIx64 " resourceOffset=0 "
            "accessSize=0x%" PRIx64 " allocationSize=0x%" PRIx64 " alignOk=%d",
            g_last_submitted, r->id, r->allocation_id, bindOffset, effective_size,
            a ? a->allocation_size : 0, align_ok ? 1 : 0);
    }

    /* Suballocation freeflight: range lease + optional wait before reuse.
     * R2: generation leases; wait only when overlapping lease incomplete.
     * Host may redirect via physical pool (request_handler) before this wait. */
    if (a && effective_size > 0) {
        int overlap_count = 0;
        uint64_t max_overlap_use = 0;
        uint64_t overlap_id = 0;
        int worst_qi = -1;
        uint64_t worst_qserial = 0;
        uint64_t max_qserial_per_queue[kMaxQueues];
        memset(max_qserial_per_queue, 0, sizeof(max_qserial_per_queue));

        /* Prefer lease table (generation-aware). Also scan live buffers for inherit. */
        const bool lease_hit = lease_incomplete_overlap(
            a->id, bindOffset, effective_size, &max_overlap_use, &worst_qi, &worst_qserial,
            max_qserial_per_queue, &overlap_count);

        for (int i = 0; i < g_resource_count; ++i) {
            TrackedResource* o = &g_resources[i];
            if (o == r || !o->alive || o->kind != TRACK_RES_BUFFER) {
                continue;
            }
            if (o->allocation_id != a->id) {
                continue;
            }
            const uint64_t o_size =
                o->buffer_size > 0 ? o->buffer_size
                                   : (o->requirements_size > 0 ? o->requirements_size : 0);
            if (o_size == 0) {
                continue;
            }
            const uint64_t o_start = o->bind_offset;
            const uint64_t o_end = o->bind_offset + o_size;
            if (!ranges_overlap(gpu_start, gpu_end, o_start, o_end)) {
                continue;
            }
            const bool inflight = resource_in_flight(o) || o->destroy_requested ||
                                  o->pending_submission_refs > 0 ||
                                  (o->last_submitted_use > g_last_completed);
            if (!(inflight || o->destroy_requested)) {
                continue;
            }
            if (!lease_hit) {
                overlap_count++;
            }
            if (o->last_submitted_use > max_overlap_use) {
                max_overlap_use = o->last_submitted_use;
                overlap_id = o->id;
            }
            for (int u = 0; u < kMaxQueueUses; ++u) {
                if (o->last_use[u].queue_index >= 0) {
                    const int oqi = o->last_use[u].queue_index;
                    const uint64_t oserial = o->last_use[u].serial;
                    stamp_queue_use(r->last_use, oqi, oserial);
                    stamp_queue_use(a->last_use, oqi, oserial);
                    if (oqi >= 0 && oqi < kMaxQueues &&
                        (g_queues[oqi].completed_serial < oserial)) {
                        if (oserial > max_qserial_per_queue[oqi]) {
                            max_qserial_per_queue[oqi] = oserial;
                        }
                        if (oserial > worst_qserial) {
                            worst_qserial = oserial;
                            worst_qi = oqi;
                        }
                    }
                }
            }
            if (o->last_submitted_use > r->last_submitted_use) {
                r->last_submitted_use = o->last_submitted_use;
            }
            if (o->last_submitted_use > a->last_submitted_use) {
                a->last_submitted_use = o->last_submitted_use;
            }
        }

        /* need_wait if any incomplete serial on overlapping range. */
        bool need_wait = false;
        if (lease_hit || overlap_count > 0) {
            need_wait = (worst_qserial > 0);
            if (!need_wait) {
                /* Overlap resources but all queue serials already complete. */
                for (int qi = 0; qi < kMaxQueues; ++qi) {
                    if (max_qserial_per_queue[qi] > g_queues[qi].completed_serial) {
                        need_wait = true;
                        if (max_qserial_per_queue[qi] > worst_qserial) {
                            worst_qserial = max_qserial_per_queue[qi];
                            worst_qi = qi;
                        }
                    }
                }
            }
        }
        if (overlap_count > 0 || lease_hit) {
            static uint64_t s_suballoc_log;
            if (rate_allow(&s_suballoc_log)) {
                VT_TRACK_LOG(
                    "SUBALLOC_REUSE_IN_FLIGHT newResource=%" PRIu64 " allocation=%" PRIu64
                    " bindOffset=0x%" PRIx64 " size=0x%" PRIx64 " overlapCount=%d "
                    "maxOverlapUse=%" PRIu64 " exampleOverlap=%" PRIu64 " completed=%" PRIu64
                    " waitEnabled=%d rangePool=%d queue=%d queueSerial=%" PRIu64,
                    r->id, a->id, bindOffset, effective_size, overlap_count, max_overlap_use,
                    overlap_id, g_last_completed, g_wait_on_suballoc_overlap ? 1 : 0,
                    g_suballoc_range_pool ? 1 : 0, worst_qi, worst_qserial);
                /* Explicit unsafe tag when prior host submission still incomplete. */
                if (need_wait) {
                    VT_TRACK_LOG(
                        "SUBALLOC_REUSE_UNSAFE host_alloc_id=%" PRIu64 " offset=0x%" PRIx64
                        " size=0x%" PRIx64 " previous_last_submit=%" PRIu64
                        " host_completed_submit=%" PRIu64 " new_owner=%" PRIu64
                        " reuse_safe=0 queue=%d queueSerial=%" PRIu64,
                        a->id, bindOffset, effective_size, max_overlap_use, g_last_completed, r->id,
                        worst_qi, worst_qserial);
                }
            }
            VortekGpuTrack_freeflightEvent("SUBALLOC_REUSE", g_last_queue, max_overlap_use, buffer,
                                           a->id, a->backing_id, a->mapping_id, max_overlap_use,
                                           g_last_completed, (uint32_t)overlap_count, 0, 0);
        }

        const bool wait_enabled = g_wait_on_suballoc_overlap || g_suballoc_range_pool;

        /* Wait first — only then open a new generation (preserve prior gen uses). */
        if (need_wait && wait_enabled) {
            stash_suballoc_wait(a->id, bindOffset, effective_size, r->id, 0, max_overlap_use,
                                worst_qi, worst_qserial, overlap_count, max_qserial_per_queue,
                                0 /* bind */);
            g_suballoc_wait.fence_count = collect_durable_fences_for_need(
                worst_qi, worst_qserial, g_suballoc_wait.fences, kMaxSuballocWaitFences);
            for (int si = 0; si < kSubRing && g_suballoc_wait.fence_count < kMaxSuballocWaitFences;
                 ++si) {
                SubmissionRecord* rec = &g_subs[si];
                if (!rec->active || rec->completed) {
                    continue;
                }
                const int rqi = rec->queue_index;
                if (rqi < 0 || rqi >= kMaxQueues) {
                    continue;
                }
                const uint64_t need = max_qserial_per_queue[rqi];
                if (need == 0 || rec->queue_serial == 0 || rec->queue_serial > need) {
                    continue;
                }
                void* f = rec->completion_fence ? rec->completion_fence : rec->fence;
                if (!f) {
                    continue;
                }
                int dup = 0;
                for (int fi = 0; fi < g_suballoc_wait.fence_count; ++fi) {
                    if (g_suballoc_wait.fences[fi] == f) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    g_suballoc_wait.fences[g_suballoc_wait.fence_count++] = f;
                }
            }
            VT_TRACK_LOG(
                "SUBALLOC_BIND_BLOCKED_IN_FLIGHT newResource=%" PRIu64
                " allocation=%" PRIu64 " bindOffset=0x%" PRIx64 " size=0x%" PRIx64
                " maxOverlapUse=%" PRIu64 " queue=%d queueSerial=%" PRIu64
                " fenceCount=%d overlapCount=%d exampleOverlap=%" PRIu64,
                r->id, a->id, bindOffset, effective_size, max_overlap_use, worst_qi,
                worst_qserial, g_suballoc_wait.fence_count, overlap_count, overlap_id);
            /* Generation opened in noteSuballocTargetedWait after host wait. */
        } else {
            SuballocLease* neu =
                begin_new_generation(a->id, bindOffset, effective_size, r->id);
            const uint64_t gen = neu ? neu->generation : 0;
            if (!need_wait && (overlap_count > 0 || lease_hit)) {
                g_sub_stats_busy_skipped++;
                VT_TRACK_LOG(
                    "SUBALLOC_BUSY_SKIPPED allocation=%" PRIu64 " offset=0x%" PRIx64
                    " size=0x%" PRIx64 " generation=%" PRIu64 " reason=completed_lease",
                    a->id, bindOffset, effective_size, gen);
            }
        }
    }
}

void VortekGpuTrack_onBindImage(void* image, ResourceMemory* rm, uint64_t bindOffset,
                                uint64_t requirementsSize, uint64_t requirementsAlignment) {
    VortekGpuTrack_initOnce();
    TrackedResource* r = find_resource(image);
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!r) {
        VortekGpuTrack_onCreateImage(image, 0, 0, 0, 0);
        r = find_resource(image);
    }
    if (!r) {
        return;
    }
    r->rm = rm;
    r->allocation_id = a ? a->id : 0;
    r->external_backing_id = a ? a->backing_id : 0;
    r->mapping_id = a ? a->mapping_id : 0;
    r->gpu_va_mapping_id = a ? a->gpu_va_mapping_id : 0;
    r->bind_offset = bindOffset;
    r->requirements_size = requirementsSize;
    r->requirements_alignment = requirementsAlignment;
    if (a) {
        recount_alloc_children(a);
    }

    const uint64_t effective_size = requirementsSize;
    const bool align_ok =
        requirementsAlignment == 0 || (bindOffset % requirementsAlignment) == 0;
    const bool fits =
        a ? VortekGpuTrack_rangeFits(a->allocation_size, bindOffset, 0, effective_size)
          : false;

    VT_TRACK_LOG(
        "VORTEK_BIND_IMAGE resource=%" PRIu64 " allocation=%" PRIu64 " image=%p "
        "bindOffset=0x%" PRIx64 " requirementsSize=0x%" PRIx64
        " requirementsAlignment=0x%" PRIx64 " effectiveGpuStart=0x%" PRIx64
        " effectiveGpuEnd=0x%" PRIx64 " alignOk=%d rangeFits=%d externalBacking=%" PRIu64,
        r->id, r->allocation_id, image, bindOffset, requirementsSize,
        requirementsAlignment, bindOffset, bindOffset + effective_size, align_ok ? 1 : 0,
        fits ? 1 : 0, r->external_backing_id);

    if (!align_ok || !fits) {
        VT_TRACK_LOG(
            "GPU_RANGE_INVALID submission=%" PRIu64 " operation=bind_image resource=%" PRIu64
            " allocation=%" PRIu64 " bindOffset=0x%" PRIx64 " resourceOffset=0 "
            "accessSize=0x%" PRIx64 " allocationSize=0x%" PRIx64 " alignOk=%d",
            g_last_submitted, r->id, r->allocation_id, bindOffset, effective_size,
            a ? a->allocation_size : 0, align_ok ? 1 : 0);
    }
}

static bool validate_side(const char* operation, const char* side, TrackedResource* r,
                          uint64_t offset, uint64_t size) {
    if (!r) {
        return true;
    }
    TrackedGpuAllocation* a = r->allocation_id ? find_alloc_by_id(r->allocation_id) : NULL;
    if (!a && r->rm) {
        a = find_alloc_by_rm(r->rm);
    }
    if (!a || size == 0) {
        return true;
    }
    const bool fits =
        VortekGpuTrack_rangeFits(a->allocation_size, r->bind_offset, offset, size);
    if (!fits) {
        VT_TRACK_LOG(
            "GPU_RANGE_INVALID submission=%" PRIu64 " operation=%s side=%s resource=%" PRIu64
            " allocation=%" PRIu64 " bindOffset=0x%" PRIx64 " resourceOffset=0x%" PRIx64
            " accessSize=0x%" PRIx64 " allocationSize=0x%" PRIx64,
            g_current_pending_submission ? g_current_pending_submission
                                         : (g_last_submitted + 1),
            operation, side, r->id, a->id, r->bind_offset, offset, size, a->allocation_size);
    }
    return fits;
}

bool VortekGpuTrack_onGpuAccess(void* commandBuffer, const char* operation,
                                void* srcResource, uint64_t srcOffset, uint64_t srcSize,
                                void* dstResource, uint64_t dstOffset, uint64_t dstSize,
                                uint32_t width, uint32_t height, uint32_t pitch,
                                int tilingMode) {
    VortekGpuTrack_initOnce();
    TrackedResource* src = find_resource(srcResource);
    TrackedResource* dst = find_resource(dstResource);
    TrackedCommandBuffer* cmd = find_or_create_cmd(commandBuffer);

    /* Always associate resources with the command buffer — even when size is 0
     * (copy_buffer_to_image historically passed size=0 and skipped use tracking). */
    if (src) {
        add_resource_closure(cmd, src->id, false);
        if (operation) {
            strncpy(src->last_op, operation, sizeof(src->last_op) - 1);
        }
    }
    if (dst) {
        add_resource_closure(cmd, dst->id, true);
        if (operation) {
            strncpy(dst->last_op, operation, sizeof(dst->last_op) - 1);
        }
    }

    const uint64_t src_id = src ? src->id : 0;
    const uint64_t dst_id = dst ? dst->id : 0;
    const uint64_t src_gpu_start = src ? (src->bind_offset + srcOffset) : srcOffset;
    const uint64_t src_gpu_end = src_gpu_start + srcSize;
    const uint64_t dst_gpu_start = dst ? (dst->bind_offset + dstOffset) : dstOffset;
    const uint64_t dst_gpu_end = dst_gpu_start + dstSize;
    const uint64_t sub = g_current_pending_submission ? g_current_pending_submission
                                                      : (g_last_submitted + 1);

    bool ok = true;
    if (srcResource) {
        ok = validate_side(operation, "src", src, srcOffset, srcSize) && ok;
    }
    if (dstResource) {
        ok = validate_side(operation, "dst", dst, dstOffset, dstSize) && ok;
    }

    VT_TRACK_LOG(
        "GPU_ACCESS submission=%" PRIu64 " operation=%s srcResource=%" PRIu64
        " srcOffset=0x%" PRIx64 " srcSize=0x%" PRIx64 " srcGpuStart=0x%" PRIx64
        " srcGpuEnd=0x%" PRIx64 " dstResource=%" PRIu64 " dstOffset=0x%" PRIx64
        " dstSize=0x%" PRIx64 " dstGpuStart=0x%" PRIx64 " dstGpuEnd=0x%" PRIx64
        " width=%u height=%u pitch=%u tilingMode=%d rangeOk=%d cmdRefs=%d",
        sub, operation ? operation : "unknown", src_id, srcOffset, srcSize, src_gpu_start,
        src_gpu_end, dst_id, dstOffset, dstSize, dst_gpu_start, dst_gpu_end, width, height,
        pitch, tilingMode, ok ? 1 : 0, cmd ? cmd->resource_count : 0);

    GpuOp op;
    memset(&op, 0, sizeof(op));
    op.submission = sub;
    strncpy(op.operation, operation ? operation : "unknown", sizeof(op.operation) - 1);
    op.src_resource = src_id;
    op.src_offset = srcOffset;
    op.src_size = srcSize;
    op.dst_resource = dst_id;
    op.dst_offset = dstOffset;
    op.dst_size = dstSize;
    op.width = width;
    op.height = height;
    op.tiling_mode = tilingMode;
    op.range_invalid = !ok;
    push_op(&op);

    return ok;
}

bool VortekGpuTrack_onCopyBufferToImage(void* commandBuffer, void* srcBuffer, void* dstImage,
                                        uint32_t dstLayout, uint64_t bufferOffset,
                                        uint32_t rowLength, uint32_t imageHeight,
                                        uint32_t width, uint32_t height, uint32_t depth) {
    VortekGpuTrack_initOnce();
    TrackedResource* src = find_resource(srcBuffer);
    TrackedResource* dst = find_resource(dstImage);

    const uint64_t sub = g_current_pending_submission ? g_current_pending_submission
                                                      : (g_last_submitted + 1);
    const uint64_t access_size =
        (uint64_t)(rowLength ? rowLength : width) * (imageHeight ? imageHeight : height) *
        4;

    TrackedGpuAllocation* srcA =
        src && src->allocation_id ? find_alloc_by_id(src->allocation_id) : NULL;
    TrackedGpuAllocation* dstA =
        dst && dst->allocation_id ? find_alloc_by_id(dst->allocation_id) : NULL;
    VT_TRACK_LOG(
        "COPY_BUFFER_TO_IMAGE submission=%" PRIu64 " srcBuffer=%" PRIu64
        " srcLastUse=%" PRIu64 " srcDestroyRequested=%d srcPendingRefs=%u "
        "srcAllocation=%" PRIu64 " srcBacking=%" PRIu64 " srcMapping=%" PRIu64
        " srcGpuVaMapping=%" PRIu64 " srcBindOffset=0x%" PRIx64
        " dstImage=%" PRIu64 " dstOldLayout=0x%x dstCopyLayout=0x%x "
        "dstAllocation=%" PRIu64 " dstBacking=%" PRIu64 " dstMapping=%" PRIu64
        " extent=%ux%ux%u bufferOffset=0x%" PRIx64 " rowLength=%u imageHeight=%u "
        "commandBuffer=%p",
        sub, src ? src->id : 0, src ? src->last_submitted_use : 0,
        src && src->destroy_requested ? 1 : 0, src ? src->pending_submission_refs : 0,
        src ? src->allocation_id : 0, src ? src->external_backing_id : 0,
        src ? src->mapping_id : 0, src ? src->gpu_va_mapping_id : 0,
        src ? src->bind_offset : 0, dst ? dst->id : 0, dst ? dst->current_layout : 0, dstLayout,
        dst ? dst->allocation_id : 0, dst ? dst->external_backing_id : 0,
        dst ? dst->mapping_id : 0, width, height, depth, bufferOffset, rowLength, imageHeight,
        commandBuffer);
    VortekGpuTrack_freeflightEvent("COPY_BUFFER_TO_IMAGE", g_last_queue, sub,
                                   dst ? dst->handle : NULL, dst ? dst->allocation_id : 0,
                                   dst ? dst->external_backing_id : 0, dst ? dst->mapping_id : 0,
                                   dst ? dst->last_submitted_use : 0, g_last_completed,
                                   dst ? dst->pending_submission_refs : 0, 0, 0);
    (void)srcA;
    (void)dstA;

    if (dst) {
        dst->current_layout = dstLayout;
    }

    const uint64_t src_size = (width > 0 && height > 0) ? access_size : 0;
    return VortekGpuTrack_onGpuAccess(commandBuffer, "copy_buffer_to_image", srcBuffer,
                                      bufferOffset, src_size, dstImage, 0, 0, width, height,
                                      rowLength, -1);
}

void VortekGpuTrack_onImageBarrier(void* commandBuffer, void* image, uint32_t srcStage,
                                   uint32_t srcAccess, uint32_t dstStage, uint32_t dstAccess,
                                   uint32_t oldLayout, uint32_t newLayout) {
    VortekGpuTrack_initOnce();
    TrackedResource* r = find_resource(image);
    TrackedCommandBuffer* cmd = find_or_create_cmd(commandBuffer);
    if (r) {
        add_resource_closure(cmd, r->id, true);
        if (oldLayout != 0 && r->current_layout != 0 && r->current_layout != oldLayout) {
            VT_TRACK_LOG(
                "IMAGE_LAYOUT_MISMATCH resource=%" PRIu64 " trackedLayout=0x%x "
                "barrierOldLayout=0x%x newLayout=0x%x",
                r->id, r->current_layout, oldLayout, newLayout);
        }
        r->current_layout = newLayout;
    }
    VT_TRACK_LOG(
        "IMAGE_BARRIER submission=%" PRIu64 " resource=%" PRIu64
        " srcStage=0x%x srcAccess=0x%x dstStage=0x%x dstAccess=0x%x "
        "oldLayout=0x%x newLayout=0x%x",
        g_current_pending_submission ? g_current_pending_submission : (g_last_submitted + 1),
        r ? r->id : 0, srcStage, srcAccess, dstStage, dstAccess, oldLayout, newLayout);
}

void VortekGpuTrack_onCmdReset(void* commandBuffer) {
    TrackedCommandBuffer* cmd = find_or_create_cmd(commandBuffer);
    if (!cmd) {
        return;
    }
    cmd->resource_count = 0;
    cmd->push_binding_count = 0;
    cmd->bound_set_count = 0;
    memset(cmd->push_bindings, 0, sizeof(cmd->push_bindings));
    memset(cmd->bound_sets, 0, sizeof(cmd->bound_sets));
}

void VortekGpuTrack_noteQueue(void* queue) {
    if (queue) {
        g_last_queue = queue;
        g_current_queue_index = find_or_create_queue(queue);
    }
}

uint64_t VortekGpuTrack_beginSubmission(void) {
    VortekGpuTrack_initOnce();
    g_current_pending_submission = g_last_submitted + 1;

    int qi = g_current_queue_index;
    if (qi < 0 && g_last_queue) {
        qi = find_or_create_queue(g_last_queue);
        g_current_queue_index = qi;
    }
    g_current_queue_serial = 0;
    if (qi >= 0 && qi < kMaxQueues) {
        g_queues[qi].next_serial++;
        g_current_queue_serial = g_queues[qi].next_serial;
        g_queues[qi].last_submitted_serial = g_current_queue_serial;
    }

    /* Prefer free/completed ring slot; never clobber incomplete (R1 fence-loss fix). */
    SubmissionRecord* rec = NULL;
    const int prefer = (int)(g_current_pending_submission % kSubRing);
    for (int attempt = 0; attempt < kSubRing; ++attempt) {
        int idx = (prefer + attempt) % kSubRing;
        SubmissionRecord* cand = &g_subs[idx];
        if (!cand->active || cand->completed) {
            rec = cand;
            break;
        }
    }
    if (!rec) {
        /* Ring full of incomplete: keep durable fence map; reuse prefer slot but
         * durable map retains exact wait tokens. */
        rec = &g_subs[prefer];
        static uint64_t s_sub_ring_full;
        if (rate_allow(&s_sub_ring_full)) {
            VT_TRACK_LOG(
                "SUBALLOC_SUB_RING_FULL overwriting submission=%" PRIu64
                " incomplete active ring; durableFenceMap retains tokens",
                rec->id);
        }
    }
    memset(rec, 0, sizeof(*rec));
    rec->id = g_current_pending_submission;
    rec->queue_index = qi;
    rec->queue_serial = g_current_queue_serial;
    rec->active = true;
    return g_current_pending_submission;
}

static void stamp_resource_on_submit(TrackedResource* r, SubmissionRecord* rec, bool is_write) {
    if (!r || !rec) {
        return;
    }
    const uint64_t rid = r->id;
    int already = 0;
    for (int j = 0; j < rec->resource_count; ++j) {
        if (rec->resource_ids[j] == rid) {
            already = 1;
            break;
        }
    }

    r->last_submitted_use = g_current_pending_submission;
    stamp_queue_use(r->last_use, rec->queue_index, rec->queue_serial);
    if (is_write) {
        r->last_gpu_write_submission = g_current_pending_submission;
    } else {
        r->last_gpu_read_submission = g_current_pending_submission;
    }

    /* Stamp range lease at submit. Only auto-create FHD leases when detile stamp on
     * (ensure_lease always-on regressed SEGA; deep run had no ensure). */
    if (r->kind == TRACK_RES_BUFFER && r->allocation_id) {
        const uint64_t rsz_chk =
            r->buffer_size > 0 ? r->buffer_size
                               : (r->requirements_size > 0 ? r->requirements_size : 0);
        SuballocLease* L = find_lease_for_range(r->allocation_id, r->bind_offset, rsz_chk);
        if (!L && g_detile_stamp && is_fhd_class_size(rsz_chk)) {
            L = ensure_lease_for_resource_use(r);
        }
        if (L) {
            if (L->owner_resource != 0 && L->owner_resource != r->id) {
                g_sub_stats_gen_mismatch++;
                static uint64_t s_gen_mm;
                if (rate_allow(&s_gen_mm)) {
                    VT_TRACK_LOG(
                        "SUBALLOC_GENERATION_MISMATCH allocation=%" PRIu64
                        " offset=0x%" PRIx64 " generation=%" PRIu64 " owner=%" PRIu64
                        " submitResource=%" PRIu64,
                        L->allocation_id, L->offset, L->generation, L->owner_resource, r->id);
                }
            } else {
                if (is_write) {
                    stamp_lease_queue_use(&L->last_write, rec->queue_index, rec->queue_serial);
                    L->last_write_global = g_current_pending_submission;
                } else {
                    stamp_lease_queue_use(&L->last_read, rec->queue_index, rec->queue_serial);
                    L->last_read_global = g_current_pending_submission;
                }
                L->cpu_write_owner = false;
                L->state = kSubLeaseGpuInFlight;
                if (is_fhd_class_size(L->size) && !is_write) {
                    VT_TRACK_LOG(
                        "DETILE_SOURCE_LAST_GPU_READ allocation=%" PRIu64 " resource=%" PRIu64
                        " generation=%" PRIu64 " lastGpuRead=%" PRIu64 " submission=%" PRIu64
                        " queue=%d queueSerial=%" PRIu64,
                        L->allocation_id, r->id, L->generation, L->last_read_global,
                        g_current_pending_submission, rec->queue_index, rec->queue_serial);
                }
            }
        }
    }

    TrackedGpuAllocation* a =
        r->allocation_id ? find_alloc_by_id(r->allocation_id) : NULL;

    if (!already) {
        r->pending_submission_refs++;
        if (rec->resource_count < kMaxResPerSub) {
            rec->resource_ids[rec->resource_count++] = rid;
        }
    }

    if (!already) {
        VT_TRACK_LOG(
            "RESOURCE_USE buffer_or_image=%" PRIu64 " kind=%s serial=%" PRIu64 " queue=%d "
            "queueSerial=%" PRIu64,
            r->id, kind_str(r->kind), g_current_pending_submission, rec->queue_index,
            rec->queue_serial);
    }

    if (a) {
        int a_already = 0;
        for (int j = 0; j < rec->alloc_count; ++j) {
            if (rec->alloc_ids[j] == a->id) {
                a_already = 1;
                break;
            }
        }
        a->last_submitted_use = g_current_pending_submission;
        stamp_queue_use(a->last_use, rec->queue_index, rec->queue_serial);
        if (is_write) {
            a->last_gpu_write_submission = g_current_pending_submission;
        } else {
            a->last_gpu_read_submission = g_current_pending_submission;
        }
        if (!a_already) {
            a->pending_submission_refs++;
            if (rec->alloc_count < kMaxResPerSub) {
                rec->alloc_ids[rec->alloc_count++] = a->id;
            }
            VT_TRACK_LOG(
                "RESOURCE_USE allocation=%" PRIu64 " serial=%" PRIu64 " queue=%d "
                "queueSerial=%" PRIu64,
                a->id, g_current_pending_submission, rec->queue_index, rec->queue_serial);
            if (a->backing_id) {
                VT_TRACK_LOG(
                    "RESOURCE_USE externalBacking=%" PRIu64 " allocation=%" PRIu64
                    " serial=%" PRIu64 " queue=%d queueSerial=%" PRIu64,
                    a->backing_id, a->id, g_current_pending_submission, rec->queue_index,
                    rec->queue_serial);
            }
            if (a->mapping_id) {
                VT_TRACK_LOG(
                    "RESOURCE_USE mapping=%" PRIu64 " allocation=%" PRIu64 " serial=%" PRIu64
                    " queue=%d queueSerial=%" PRIu64,
                    a->mapping_id, a->id, g_current_pending_submission, rec->queue_index,
                    rec->queue_serial);
            }
        }
    }
}

void VortekGpuTrack_noteSubmitCommandBuffer(void* commandBuffer) {
    if (!g_current_pending_submission) {
        return;
    }
    TrackedCommandBuffer* cmd = find_or_create_cmd(commandBuffer);
    if (!cmd) {
        return;
    }
    SubmissionRecord* rec = &g_subs[g_current_pending_submission % kSubRing];
    for (int i = 0; i < cmd->resource_count; ++i) {
        TrackedResource* r = find_resource_by_id(cmd->resource_ids[i]);
        if (!r) {
            continue;
        }
        stamp_resource_on_submit(r, rec, cmd->resource_writes[i] != 0);
    }
}

void VortekGpuTrack_bindSubmissionFence(uint64_t submission, void* fence) {
    if (fence) {
        VortekGpuTrack_bindSubmissionCompletion(submission, fence, VORTEK_COMPLETION_CALLER_FENCE);
    } else {
        VortekGpuTrack_bindSubmissionCompletion(submission, NULL, VORTEK_COMPLETION_UNKNOWN);
    }
}

void VortekGpuTrack_bindSubmissionCompletion(uint64_t submission, void* fenceOrTimeline,
                                             VortekCompletionSource source) {
    if (!submission) {
        return;
    }
    SubmissionRecord* rec = &g_subs[submission % kSubRing];
    if (rec->id != submission || !rec->active) {
        return;
    }
    CompletionSource cs = map_api_completion_source(source);
    if (fenceOrTimeline) {
        rec->completion_fence = fenceOrTimeline;
        if (cs == COMPLETION_CALLER_FENCE || cs == COMPLETION_UNKNOWN) {
            rec->fence = fenceOrTimeline;
        }
        if (cs == COMPLETION_UNKNOWN) {
            /* Non-null token without source: treat as caller fence. */
            cs = COMPLETION_CALLER_FENCE;
        }
    }
    if (cs != COMPLETION_UNKNOWN) {
        rec->completion_source = cs;
    }

    /* Durable serial→fence so suballoc waits survive g_subs ring wrap. */
    void* durable = rec->completion_fence ? rec->completion_fence : rec->fence;
    if (durable) {
        durable_fence_register(rec->id, rec->queue_index, rec->queue_serial, durable,
                               rec->completion_source);
    }

    VT_TRACK_LOG(
        "INTERNAL_COMPLETION_SUBMIT queue=%d submission=%" PRIu64 " timelineValue=%" PRIu64
        " callerFence=%p completionFence=%p source=%s",
        rec->queue_index, rec->id, rec->queue_serial, rec->fence, rec->completion_fence,
        completion_source_str(rec->completion_source));
    VortekGpuTrack_freeflightEvent("SUBMIT", g_last_queue, rec->id, rec->completion_fence, 0, 0, 0,
                                   rec->queue_serial, g_last_completed, 0, 0, 0);
}

void VortekGpuTrack_endSubmission(uint64_t submission, int vkResult, bool deviceLost) {
    g_last_submitted = submission;
    for (int i = 0; i < g_cmd_count; ++i) {
        g_cmds[i].resource_count = 0;
        g_cmds[i].push_binding_count = 0;
        g_cmds[i].bound_set_count = 0;
    }
    g_current_pending_submission = 0;

    SubmissionRecord* rec = &g_subs[submission % kSubRing];
    int qi = (rec->id == submission) ? rec->queue_index : g_current_queue_index;
    uint64_t qserial = (rec->id == submission) ? rec->queue_serial : g_current_queue_serial;

    VT_TRACK_LOG("VORTEK_SUBMIT submission=%" PRIu64 " queue=%d queueSerial=%" PRIu64
                 " result=%d deviceLost=%d lastCompleted=%" PRIu64 " fence=%p "
                 "completionFence=%p completionSource=%s",
                 submission, qi, qserial, vkResult, deviceLost ? 1 : 0, g_last_completed,
                 (rec->id == submission) ? rec->fence : NULL,
                 (rec->id == submission) ? rec->completion_fence : NULL,
                 (rec->id == submission) ? completion_source_str(rec->completion_source)
                                         : "unknown");
    if (deviceLost) {
        VortekGpuTrack_onDeviceLost("vkQueueSubmit");
    }
    /* Do not infer completion from submit acceptance. Only poll known fences later. */
    VortekGpuTrack_collectRetired();
    (void)vkResult;
}

void VortekGpuTrack_noteFenceWaitResult(int vkResult) {
    /* Legacy path without fence list: never optimistically complete all work. */
    if (vkResult == -4 /* VK_ERROR_DEVICE_LOST */) {
        VortekGpuTrack_onDeviceLost("vkWaitForFences");
        return;
    }
    VT_TRACK_LOG(
        "VORTEK_FENCE_WAIT result=%d lastCompleted=%" PRIu64 " lastSubmitted=%" PRIu64
        " (no fence list; completion only via matched fence/status/idle)",
        vkResult, g_last_completed, g_last_submitted);
}

static int match_and_complete_fence(void* fence, CompletionSource preferred) {
    if (!fence) {
        return 0;
    }
    int any = 0;
    for (int i = 0; i < kSubRing; ++i) {
        SubmissionRecord* rec = &g_subs[i];
        if (!rec->active || rec->completed) {
            continue;
        }
        if (rec->completion_fence == fence || rec->fence == fence) {
            CompletionSource src = preferred;
            if (src == COMPLETION_UNKNOWN) {
                src = rec->completion_source;
            }
            if (src == COMPLETION_UNKNOWN) {
                src = COMPLETION_CALLER_FENCE;
            }
            complete_submission_record(rec, src);
            any = 1;
        }
    }
    /* Complete durable entries even if ring slot was overwritten.
     * Only advance queue watermark for rows that still own this fence token. */
    for (int i = 0; i < kMaxDurableFences; ++i) {
        DurableFence* d = &g_durable_fences[i];
        if (!d->used || d->completed || d->fence != fence) {
            continue;
        }
        CompletionSource src = preferred;
        if (src == COMPLETION_UNKNOWN) {
            src = d->source;
        }
        if (src == COMPLETION_UNKNOWN) {
            src = COMPLETION_CALLER_FENCE;
        }
        d->completed = true;
        d->fence = NULL;
        if (d->queue_index >= 0 && d->queue_index < kMaxQueues && g_queues[d->queue_index].used &&
            src != COMPLETION_UNKNOWN && d->queue_serial > g_queues[d->queue_index].completed_serial) {
            complete_queue_up_to(d->queue_index, d->queue_serial, src);
        }
        if (d->global_id > g_last_completed) {
            g_last_completed = d->global_id;
        }
        any = 1;
    }
    return any;
}

void VortekGpuTrack_noteFencesWait(void* const* fences, uint32_t fenceCount, int waitAll,
                                   int vkResult) {
    if (vkResult == -4) {
        VortekGpuTrack_onDeviceLost("vkWaitForFences");
        return;
    }
    if (vkResult != 0) {
        VT_TRACK_LOG("VORTEK_FENCES_WAIT result=%d count=%u waitAll=%d", vkResult, fenceCount,
                     waitAll);
        return;
    }
    if (!fences || fenceCount == 0) {
        /* Empty wait success is not proof of prior null-fence submissions. */
        VT_TRACK_LOG(
            "VORTEK_FENCES_WAIT result=0 count=0 waitAll=%d (no completion advance)", waitAll);
        return;
    }

    int any = 0;
    for (uint32_t fi = 0; fi < fenceCount; ++fi) {
        void* fence = fences[fi];
        if (!fence) {
            continue;
        }
        any |= match_and_complete_fence(fence, COMPLETION_CALLER_FENCE);
    }
    if (!any) {
        /* Unmatched fence must NOT complete unrelated serials. Fail closed. */
        VT_TRACK_LOG(
            "VORTEK_FENCES_WAIT result=0 count=%u waitAll=%d matched=0 "
            "lastCompleted=%" PRIu64 " lastSubmitted=%" PRIu64
            " (unmatched; no inferred completion)",
            fenceCount, waitAll, g_last_completed, g_last_submitted);
        return;
    }
    VortekGpuTrack_collectRetired();
    VT_TRACK_LOG("VORTEK_FENCES_WAIT result=0 count=%u waitAll=%d matched=%d "
                 "lastCompleted=%" PRIu64 " lastSubmitted=%" PRIu64,
                 fenceCount, waitAll, any, g_last_completed, g_last_submitted);
}

void VortekGpuTrack_noteFenceStatus(void* fence, int vkGetFenceStatusResult) {
    if (!fence) {
        return;
    }
    if (vkGetFenceStatusResult == -4) {
        VortekGpuTrack_onDeviceLost("vkGetFenceStatus");
        return;
    }
    if (vkGetFenceStatusResult != 0) {
        return; /* VK_NOT_READY or error — not complete */
    }
    /* VK_SUCCESS: fence signaled. */
    CompletionSource src = COMPLETION_INTERNAL_FENCE;
    for (int i = 0; i < kSubRing; ++i) {
        SubmissionRecord* rec = &g_subs[i];
        if (!rec->active || rec->completed) {
            continue;
        }
        if (rec->completion_fence == fence || rec->fence == fence) {
            if (rec->completion_source != COMPLETION_UNKNOWN) {
                src = rec->completion_source;
            } else if (rec->fence == fence && rec->completion_fence != fence) {
                src = COMPLETION_CALLER_FENCE;
            }
            break;
        }
    }
    if (match_and_complete_fence(fence, src)) {
        VortekGpuTrack_collectRetired();
    }
}

int VortekGpuTrack_fillPendingCompletionFences(void** outFences, int maxCount) {
    if (!outFences || maxCount <= 0) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < kSubRing && n < maxCount; ++i) {
        SubmissionRecord* rec = &g_subs[i];
        if (!rec->active || rec->completed) {
            continue;
        }
        void* f = rec->completion_fence ? rec->completion_fence : rec->fence;
        if (!f) {
            continue;
        }
        /* Dedupe */
        int dup = 0;
        for (int j = 0; j < n; ++j) {
            if (outFences[j] == f) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            outFences[n++] = f;
        }
    }
    return n;
}

void VortekGpuTrack_releaseCompletionFence(void* fence) {
    if (!fence) {
        return;
    }
    for (int i = 0; i < kSubRing; ++i) {
        SubmissionRecord* rec = &g_subs[i];
        if (rec->completion_fence == fence) {
            rec->completion_fence = NULL;
        }
        if (rec->fence == fence && rec->completed) {
            rec->fence = NULL;
        }
    }
}

void VortekGpuTrack_noteQueueWaitIdle(void* queue, int vkResult) {
    if (vkResult == -4) {
        VortekGpuTrack_onDeviceLost("vkQueueWaitIdle");
        return;
    }
    if (vkResult != 0) {
        return;
    }
    int qi = find_or_create_queue(queue);
    if (qi >= 0) {
        uint64_t old = g_queues[qi].completed_serial;
        uint64_t neu = g_queues[qi].last_submitted_serial;
        VT_TRACK_LOG(
            "DEVICE_WAIT_IDLE_COMPLETION_ADVANCE queue=%d oldCompleted=%" PRIu64
            " newCompleted=%" PRIu64 " reason=queue_wait_idle source=queue_idle",
            qi, old, neu);
        complete_queue_up_to(qi, neu, COMPLETION_QUEUE_IDLE);
    }
}

void VortekGpuTrack_noteDeviceWaitIdle(int vkResult) {
    if (vkResult == -4) {
        VortekGpuTrack_onDeviceLost("vkDeviceWaitIdle");
        return;
    }
    if (vkResult != 0) {
        return;
    }
    VortekGpuTrack_markAllSubmittedWorkCompleted();
}

void VortekGpuTrack_markAllSubmittedWorkCompleted(void) {
    VortekGpuTrack_initOnce();
    complete_all_queues_to_submitted(COMPLETION_DEVICE_IDLE);
    VT_TRACK_LOG("DEVICE_WAIT_IDLE_COMPLETION_ADVANCE queue=all oldCompleted=<see-queues> "
                 "newCompleted=%" PRIu64 " retireDepth=%d source=device_idle",
                 g_last_completed, g_retire_queue_depth);
}

void VortekGpuTrack_notePresent(void* queue, uint64_t presentId) {
    g_last_present_id = presentId;
    VortekGpuTrack_noteQueue(queue);
    VT_TRACK_LOG(
        "PRESENT_TRACK presentId=%" PRIu64 " queue=%p lastSubmitted=%" PRIu64
        " lastCompleted=%" PRIu64 " lastAcquiredImage=%u swapchainImageCount=%u",
        presentId, queue, g_last_submitted, g_last_completed, g_last_acquired_image_index,
        g_presenter_swapchain_image_count);
}

void VortekGpuTrack_notePresenterConfig(uint32_t swapchainImageCount,
                                        uint32_t internalImageCount, uint32_t frameSlotCount,
                                        uint32_t presentQueueFamily, uint32_t graphicsQueueFamily,
                                        void* swapchainHint) {
    VortekGpuTrack_initOnce();
    g_presenter_swapchain_image_count = swapchainImageCount;
    g_presenter_internal_image_count = internalImageCount;
    g_presenter_frame_slot_count = frameSlotCount;
    g_presenter_present_qfamily = presentQueueFamily;
    g_presenter_graphics_qfamily = graphicsQueueFamily;
    g_presenter_swapchain_hint = swapchainHint;
    VT_TRACK_LOG(
        "PRESENTER_CONFIG swapchainImageCount=%u internalImageCount=%u frameSlotCount=%u "
        "presentQueueFamily=%u graphicsQueueFamily=%u swapchain=%p",
        swapchainImageCount, internalImageCount, frameSlotCount, presentQueueFamily,
        graphicsQueueFamily, swapchainHint);
}

void VortekGpuTrack_noteAcquireResult(uint64_t callId, int vkResult, uint32_t returnedImageIndex,
                                      void* acquireSemaphore, void* acquireFence) {
    VortekGpuTrack_initOnce();
    if (callId == 0) {
        callId = ++g_acquire_call_id;
    } else {
        g_acquire_call_id = callId;
    }
    PresentImageState* st = present_image_state(returnedImageIndex);
    uint64_t prev_present = st ? st->present_id : 0;
    if (st && st->present_pending && vkResult == 0) {
        VT_TRACK_LOG(
            "PRESENT_IMAGE_REUSED_BEFORE_REACQUIRE imageIndex=%u presentId=%" PRIu64
            " acquireCallId=%" PRIu64,
            returnedImageIndex, st->present_id, callId);
        /* WSI reacquire of the exact image is present-completion proof. */
        VortekGpuTrack_notePresentComplete(st->present_id, returnedImageIndex, "reacquire");
    }
    if (st && vkResult == 0) {
        st->acquired = true;
        st->last_acquire_id = callId;
        st->acquire_generation++;
        st->life = PRESENT_LIFE_ACQUIRED;
        st->acquire_semaphore = acquireSemaphore;
    }
    g_last_acquired_image_index = returnedImageIndex;

    AcquireEvent* ae = &g_acquire_ring[g_acquire_write];
    memset(ae, 0, sizeof(*ae));
    ae->call_id = callId;
    ae->result = vkResult;
    ae->image_index = returnedImageIndex;
    ae->prev_present_id = prev_present;
    ae->acquire_semaphore = acquireSemaphore;
    g_acquire_write = (g_acquire_write + 1) % kAcquireRing;
    if (g_acquire_count < kAcquireRing) {
        g_acquire_count++;
    }

    VT_TRACK_LOG(
        "ACQUIRE_RESULT callId=%" PRIu64 " result=%d returnedImageIndex=%u "
        "previousPresentIdForImage=%" PRIu64 " acquireSemaphore=%p acquireFence=%p "
        "swapchainImageCount=%u",
        callId, vkResult, returnedImageIndex, prev_present, acquireSemaphore, acquireFence,
        g_presenter_swapchain_image_count);
    (void)acquireFence;
}

void VortekGpuTrack_notePresentWait(void* queue, uint64_t presentId, uint32_t imageIndex,
                                    void* semaphore) {
    (void)queue;
    (void)VortekGpuTrack_beginPresent(presentId, imageIndex, semaphore);
}

bool VortekGpuTrack_beginPresent(uint64_t presentId, uint32_t imageIndex, void* semaphore) {
    VortekGpuTrack_initOnce();
    g_last_present_id = presentId;
    PresentImageState* img = present_image_state(imageIndex);
    bool previous_pending = img && img->present_pending;
    bool sem_pending = false;

    if (semaphore) {
        for (int i = 0; i < g_present_sem_count; ++i) {
            if (g_present_sems[i].semaphore == semaphore &&
                (g_present_sems[i].in_flight || g_present_sems[i].present_pending)) {
                sem_pending = true;
                break;
            }
        }
    }

    VT_TRACK_LOG(
        "PRESENT_BEGIN image=%u presentId=%" PRIu64 " waitSemaphore=%p "
        "swapchainImageCount=%u life=%d",
        imageIndex, presentId, semaphore, g_presenter_swapchain_image_count,
        img ? (int)img->life : -1);
    VT_TRACK_LOG(
        "PRESENT_WAIT_BEGIN presentId=%" PRIu64 " imageIndex=%u semaphore=%p "
        "swapchainImageCount=%u",
        presentId, imageIndex, semaphore, g_presenter_swapchain_image_count);
    VT_TRACK_LOG(
        "PRESENT_SEMAPHORE_REUSE_CHECK imageIndex=%u semaphore=%p previousPresentPending=%d "
        "reuseAllowed=%d previousPresentId=%" PRIu64 " presentId=%" PRIu64,
        imageIndex, semaphore, previous_pending ? 1 : 0,
        (!previous_pending && !sem_pending) ? 1 : 0, img ? img->present_id : 0, presentId);

    if (previous_pending || sem_pending) {
        VT_TRACK_LOG(
            "PRESENT_SEMAPHORE_REUSE_BLOCKED semaphore=%p imageIndex=%u "
            "prevPresentId=%" PRIu64 " presentId=%" PRIu64 " imagePending=%d semPending=%d",
            semaphore, imageIndex, img ? img->present_id : 0, presentId,
            previous_pending ? 1 : 0, sem_pending ? 1 : 0);
        if (previous_pending) {
            VT_TRACK_LOG(
                "PRESENT_SEMAPHORE_REUSED_IN_FLIGHT semaphore=%p imageIndex=%u "
                "prevPresentId=%" PRIu64 " presentId=%" PRIu64,
                semaphore, imageIndex, img->present_id, presentId);
        }
        return false;
    }

    if (img) {
        img->render_finished = semaphore;
        img->present_id = presentId;
        img->present_pending = true;
        img->render_pending = false;
        img->acquired = false;
        img->life = PRESENT_LIFE_PRESENT_PENDING;
    }

    if (!semaphore) {
        return true;
    }
    for (int i = 0; i < g_present_sem_count; ++i) {
        if (g_present_sems[i].semaphore == semaphore) {
            if (g_present_sems[i].in_flight || g_present_sems[i].present_pending) {
                VT_TRACK_LOG(
                    "PRESENT_SEMAPHORE_REUSED semaphore=%p imageIndex=%u prevImage=%u "
                    "prevPresentId=%" PRIu64 " presentId=%" PRIu64 " presentPending=%d",
                    semaphore, imageIndex, g_present_sems[i].image_index,
                    g_present_sems[i].present_id, presentId,
                    g_present_sems[i].present_pending ? 1 : 0);
            }
            g_present_sems[i].image_index = imageIndex;
            g_present_sems[i].present_id = presentId;
            g_present_sems[i].in_flight = true;
            g_present_sems[i].present_pending = true;
            return true;
        }
    }
    if (g_present_sem_count < kMaxPresentSems) {
        PresentSemTrack* t = &g_present_sems[g_present_sem_count++];
        t->semaphore = semaphore;
        t->image_index = imageIndex;
        t->present_id = presentId;
        t->in_flight = true;
        t->present_pending = true;
    }
    return true;
}

void VortekGpuTrack_notePresentAccepted(uint64_t presentId, uint32_t imageIndex) {
    VT_TRACK_LOG("PRESENT_ACCEPTED image=%u presentId=%" PRIu64, imageIndex, presentId);
    VT_TRACK_LOG("RENDER_WAIT_COMPLETE image=%u presentId=%" PRIu64, imageIndex, presentId);
}

void VortekGpuTrack_notePresentComplete(uint64_t presentId, uint32_t imageIndex,
                                        const char* source) {
    PresentImageState* img = present_image_state(imageIndex);
    if (img) {
        img->present_pending = false;
        img->render_pending = false;
        img->life = PRESENT_LIFE_AVAILABLE;
        img->acquired = false;
        if (presentId != 0) {
            img->present_id = presentId;
        }
    }
    for (int i = 0; i < g_present_sem_count; ++i) {
        if (g_present_sems[i].image_index == imageIndex &&
            (presentId == 0 || g_present_sems[i].present_id == presentId ||
             g_present_sems[i].present_pending)) {
            if (presentId == 0 || g_present_sems[i].present_id == presentId ||
                g_present_sems[i].image_index == imageIndex) {
                g_present_sems[i].in_flight = false;
                g_present_sems[i].present_pending = false;
            }
        }
    }
    /* presentId==0: clear all pending for this imageIndex */
    if (presentId == 0) {
        for (int i = 0; i < g_present_sem_count; ++i) {
            if (g_present_sems[i].image_index == imageIndex) {
                g_present_sems[i].in_flight = false;
                g_present_sems[i].present_pending = false;
            }
        }
    }
    if (source && (strcmp(source, "compositor_sync") == 0)) {
        VT_TRACK_LOG("COMPOSITOR_SYNC_COMPLETE image=%u presentId=%" PRIu64, imageIndex, presentId);
        VortekGpuTrack_noteFirstFrame();
    }
    VT_TRACK_LOG(
        "PRESENT_COMPLETED image=%u presentId=%" PRIu64 " source=%s", imageIndex, presentId,
        source ? source : "unknown");
    VT_TRACK_LOG(
        "PRESENT_COMPLETE presentId=%" PRIu64 " imageIndex=%u source=%s", presentId, imageIndex,
        source ? source : "unknown");
    VortekGpuTrack_freeflightEvent("PRESENT_COMPLETED", NULL, presentId, NULL, 0, 0, 0, 0,
                                   g_last_completed, 0, 0, 0);
}

void VortekGpuTrack_noteSwapchainImageBacking(uint32_t imageIndex, void* image, void* ahb,
                                              void* memory, void* gpuMapping) {
    VortekGpuTrack_initOnce();
    PresentImageState* st = present_image_state(imageIndex);
    if (st) {
        st->image = image;
        st->ahb = ahb;
        st->memory = memory;
        st->gpu_mapping = gpuMapping;
        st->life = PRESENT_LIFE_AVAILABLE;
    }
    VT_TRACK_LOG(
        "SWAPCHAIN_IMAGE_BACKING imageIndex=%u image=%p ahb=%p memory=%p gpuMapping=%p",
        imageIndex, image, ahb, memory, gpuMapping);
}

void VortekGpuTrack_registerSwapchainBacking(uint32_t imageIndex, void* vkImage, void* vkMemory,
                                             void* ahb, intptr_t nativeHandle, uint64_t allocationSize,
                                             uint32_t usage, uint32_t stride, uint64_t* outAhbId,
                                             uint64_t* outMappingId, uint64_t* outGpuVaId) {
    VortekGpuTrack_initOnce();
    const uint64_t ahbId = g_next_backing_id++;
    const uint64_t mappingId = g_next_mapping_id++;
    const uint64_t gpuVaId = g_next_gpu_va_mapping_id++;
    PresentImageState* st = present_image_state(imageIndex);
    if (st) {
        st->image = vkImage;
        st->ahb = ahb;
        st->memory = vkMemory;
        st->life = PRESENT_LIFE_AVAILABLE;
    }
    if (outAhbId) {
        *outAhbId = ahbId;
    }
    if (outMappingId) {
        *outMappingId = mappingId;
    }
    if (outGpuVaId) {
        *outGpuVaId = gpuVaId;
    }
    VT_TRACK_LOG(
        "SWAPCHAIN_IMAGE_BACKING imageIndex=%u ahbId=0x%" PRIx64 " mappingId=0x%" PRIx64
        " gpuVaId=0x%" PRIx64 " vkImage=%p vkMemory=%p usage=0x%x allocationSize=%" PRIu64
        " stride=%u ahb=%p nativeHandle=%" PRIdPTR,
        imageIndex, ahbId, mappingId, gpuVaId, vkImage, vkMemory, (unsigned)usage, allocationSize,
        (unsigned)stride, ahb, nativeHandle);
    VortekGpuTrack_freeflightEvent("SWAPCHAIN_AHB", NULL, 0, vkImage, 0, ahbId, mappingId, 0,
                                   g_last_completed, 0, 0, 0);
    (void)nativeHandle;
}

bool VortekGpuTrack_requireDistinctAhb(void) {
    VortekGpuTrack_initOnce();
    return g_require_distinct_ahb;
}

bool VortekGpuTrack_quarantineGpuReleases(void) {
    VortekGpuTrack_initOnce();
    return g_quarantine_gpu_releases;
}

bool VortekGpuTrack_quarantineBuffers(void) {
    VortekGpuTrack_initOnce();
    return g_quarantine_buffers || g_quarantine_gpu_releases;
}

bool VortekGpuTrack_quarantineImages(void) {
    VortekGpuTrack_initOnce();
    return g_quarantine_images || g_quarantine_gpu_releases;
}

bool VortekGpuTrack_quarantineMemory(void) {
    VortekGpuTrack_initOnce();
    return g_quarantine_memory || g_quarantine_gpu_releases;
}

bool VortekGpuTrack_retainUnknownBuffers(void) {
    VortekGpuTrack_initOnce();
    return g_retain_unknown_buffers;
}

void VortekGpuTrack_noteFirstFrame(void) {
    if (!g_first_frame_seen) {
        g_first_frame_seen = true;
        VT_TRACK_LOG(
            "FIRST_FRAME_SEEN quarantineGpuReleases=%d quarantineBuffers=%d "
            "quarantineImages=%d quarantineMemory=%d retainUnknownBuffers=%d",
            g_quarantine_gpu_releases ? 1 : 0, g_quarantine_buffers ? 1 : 0,
            g_quarantine_images ? 1 : 0, g_quarantine_memory ? 1 : 0,
            g_retain_unknown_buffers ? 1 : 0);
    }
}

void VortekGpuTrack_onCmdBufferRef(void* commandBuffer, void* buffer, const char* operation,
                                   int isWrite) {
    VortekGpuTrack_initOnce();
    if (!commandBuffer || !buffer) {
        return;
    }
    TrackedCommandBuffer* cmd = find_or_create_cmd(commandBuffer);
    TrackedResource* r = find_resource(buffer);
    if (!r) {
        return;
    }
    add_resource_closure(cmd, r->id, isWrite != 0);
    if (operation) {
        strncpy(r->last_op, operation, sizeof(r->last_op) - 1);
    }
    static uint64_t s_cmd_ref_log;
    if (rate_allow(&s_cmd_ref_log)) {
        VT_TRACK_LOG(
            "CMD_BUFFER_REF operation=%s buffer=%" PRIu64 " handle=%p write=%d cmdRefs=%d",
            operation ? operation : "?", r->id, buffer, isWrite, cmd ? cmd->resource_count : 0);
    }
    VortekGpuTrack_freeflightEvent(operation ? operation : "CMD_BUF_REF", g_last_queue,
                                   g_current_pending_submission ? g_current_pending_submission
                                                                : g_last_submitted,
                                   buffer, r->allocation_id, r->external_backing_id, r->mapping_id,
                                   r->last_submitted_use, g_last_completed,
                                   r->pending_submission_refs, 0, 0);
}

void VortekGpuTrack_onExecuteCommands(void* primaryCommandBuffer, void* const* secondaryBuffers,
                                      uint32_t secondaryCount) {
    VortekGpuTrack_initOnce();
    TrackedCommandBuffer* primary = find_or_create_cmd(primaryCommandBuffer);
    if (!primary || !secondaryBuffers) {
        return;
    }
    for (uint32_t i = 0; i < secondaryCount; ++i) {
        TrackedCommandBuffer* sec = find_or_create_cmd(secondaryBuffers[i]);
        if (!sec) {
            continue;
        }
        for (int j = 0; j < sec->resource_count; ++j) {
            track_cmd_resource(primary, sec->resource_ids[j], sec->resource_writes[j] != 0);
        }
    }
    static uint64_t s_exec_log;
    if (rate_allow(&s_exec_log)) {
        VT_TRACK_LOG(
            "EXECUTE_COMMANDS_INHERIT primary=%p secondaryCount=%u primaryRefs=%d",
            primaryCommandBuffer, secondaryCount, primary->resource_count);
    }
}

static DescSetSnapshot* find_or_create_desc_slot(void* descriptorSet) {
    if (!descriptorSet) {
        return NULL;
    }
    for (int i = 0; i < kMaxDescSets; ++i) {
        if (g_desc_sets[i].used && g_desc_sets[i].set == descriptorSet) {
            return &g_desc_sets[i];
        }
    }
    for (int i = 0; i < kMaxDescSets; ++i) {
        if (!g_desc_sets[i].used) {
            memset(&g_desc_sets[i], 0, sizeof(g_desc_sets[i]));
            g_desc_sets[i].used = true;
            g_desc_sets[i].set = descriptorSet;
            return &g_desc_sets[i];
        }
    }
    VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=descSet");
    return NULL;
}

static void desc_slot_upsert_entry(DescSetSnapshot* slot, uint32_t binding, uint32_t dtype,
                                   void* buffer, uint64_t offset, uint64_t range) {
    if (!slot || !buffer) {
        return;
    }
    int idx = -1;
    for (int i = 0; i < slot->buffer_count; ++i) {
        if (slot->entries[i].binding == binding) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (slot->buffer_count >= kMaxBuffersPerDesc) {
            return;
        }
        idx = slot->buffer_count++;
    }
    slot->entries[idx].buffer = buffer;
    slot->entries[idx].offset = offset;
    slot->entries[idx].range = range;
    slot->entries[idx].binding = binding;
    slot->entries[idx].descriptor_type = dtype;
    {
        TrackedResource* br = find_resource(buffer);
        slot->entries[idx].resource_id = br ? br->id : 0;
        slot->entries[idx].bind_generation = br ? br->bind_generation : 0;
        if (br && br->allocation_id) {
            TrackedGpuAllocation* ba = find_alloc_by_id(br->allocation_id);
            if (ba) {
                slot->entries[idx].bind_generation = ba->bind_generation;
                if (is_fhd_class_size(ba->allocation_size) || is_fhd_class_size(br->buffer_size)) {
                    if (br->bind_generation != 0 && ba->bind_generation != 0 &&
                        br->bind_generation != ba->bind_generation) {
                        VT_TRACK_LOG(
                            "BUFFER_REBOUND_WITH_STALE_DESCRIPTOR resource=%" PRIu64
                            " allocation=%" PRIu64 " descGen=%" PRIu64 " liveGen=%" PRIu64
                            " binding=%u",
                            br->id, ba->id, br->bind_generation, ba->bind_generation, binding);
                    }
                    if (g_fhd_prepare_write_diag) {
                        VT_TRACK_LOG(
                            "FHD_PREPARE_WRITE_CHECK resource=%" PRIu64 " allocation=%" PRIu64
                            " generation=%" PRIu64 " path=DescriptorUpdate lastGpuRead=%" PRIu64
                            " lastSubmittedUse=%" PRIu64 " completedGlobal=%" PRIu64
                            " completedQueueSerial=%" PRIu64 " checkHit=1 wouldBlock=0",
                            br->id, ba->id, ba->bind_generation, br->last_gpu_read_submission,
                            br->last_submitted_use, g_last_completed, g_last_completed);
                    }
                }
            }
        }
    }
}

void VortekGpuTrack_onUpdateDescriptorBuffers(void* descriptorSet, void* const* buffers,
                                              uint32_t bufferCount) {
    VortekGpuTrack_initOnce();
    DescSetSnapshot* slot = find_or_create_desc_slot(descriptorSet);
    if (!slot) {
        return;
    }
    /* Legacy replace path (unknown bindings) — prefer onUpdateDescriptorWrites. */
    slot->buffer_count = 0;
    if (buffers) {
        for (uint32_t i = 0; i < bufferCount && slot->buffer_count < kMaxBuffersPerDesc; ++i) {
            if (buffers[i]) {
                desc_slot_upsert_entry(slot, i, 0, buffers[i], 0, 0);
            }
        }
    }
    static uint64_t s_upd_log;
    if (rate_allow(&s_upd_log)) {
        VT_TRACK_LOG("DESC_SET_BUFFER_SNAPSHOT set=%p bufferCount=%d path=legacy_replace",
                     descriptorSet, slot->buffer_count);
    }
}

void VortekGpuTrack_onUpdateDescriptorWrites(void* descriptorSet, const uint32_t* bindings,
                                             const uint32_t* descriptorTypes, void* const* buffers,
                                             const uint64_t* offsets, const uint64_t* ranges,
                                             uint32_t count) {
    VortekGpuTrack_initOnce();
    DescSetSnapshot* slot = find_or_create_desc_slot(descriptorSet);
    if (!slot || !buffers || count == 0) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!buffers[i]) {
            continue;
        }
        desc_slot_upsert_entry(slot, bindings ? bindings[i] : i,
                               descriptorTypes ? descriptorTypes[i] : 0, buffers[i],
                               offsets ? offsets[i] : 0, ranges ? ranges[i] : 0);
    }
    static uint64_t s_upd_w;
    if (rate_allow(&s_upd_w) || slot->buffer_count >= 2) {
        VT_TRACK_LOG(
            "DESC_SET_BUFFER_SNAPSHOT set=%p bufferCount=%d path=merge_writes added=%u",
            descriptorSet, slot->buffer_count, count);
    }
}

void VortekGpuTrack_onBindDescriptorSets(void* commandBuffer, void* const* descriptorSets,
                                         uint32_t setCount) {
    VortekGpuTrack_initOnce();
    TrackedCommandBuffer* cmd = find_or_create_cmd(commandBuffer);
    if (!cmd || !descriptorSets) {
        return;
    }
    /* Baseline (stamp off): attach buffers as reads only — original behavior. */
    if (!g_detile_stamp) {
        int attached = 0;
        for (uint32_t s = 0; s < setCount; ++s) {
            void* set = descriptorSets[s];
            if (!set) {
                continue;
            }
            for (int i = 0; i < kMaxDescSets; ++i) {
                if (!g_desc_sets[i].used || g_desc_sets[i].set != set) {
                    continue;
                }
                for (int b = 0; b < g_desc_sets[i].buffer_count; ++b) {
                    TrackedResource* r = find_resource(g_desc_sets[i].entries[b].buffer);
                    if (r) {
                        add_resource_closure(cmd, r->id, false);
                        strncpy(r->last_op, "descriptor_bind", sizeof(r->last_op) - 1);
                        attached++;
                    }
                }
                break;
            }
        }
        static uint64_t s_bind_base;
        if (rate_allow(&s_bind_base)) {
            VT_TRACK_LOG(
                "DESC_SET_BIND cmd=%p setCount=%u attachedBuffers=%d cmdRefs=%d path=baseline",
                commandBuffer, setCount, attached, cmd->resource_count);
        }
        return;
    }
    /* Dig path: remember sets, FHD leases, detile bind 0/1 write flags. */
    cmd->bound_set_count = 0;
    for (uint32_t s = 0; s < setCount && cmd->bound_set_count < kMaxBoundSetsPerCmd; ++s) {
        if (descriptorSets[s]) {
            cmd->bound_sets[cmd->bound_set_count++] = descriptorSets[s];
        }
    }
    int attached = 0;
    int fhd_attached = 0;
    for (uint32_t s = 0; s < setCount; ++s) {
        void* set = descriptorSets[s];
        if (!set) {
            continue;
        }
        for (int i = 0; i < kMaxDescSets; ++i) {
            if (!g_desc_sets[i].used || g_desc_sets[i].set != set) {
                continue;
            }
            for (int b = 0; b < g_desc_sets[i].buffer_count; ++b) {
                DescBufEntry* e = &g_desc_sets[i].entries[b];
                TrackedResource* r = find_resource(e->buffer);
                if (!r) {
                    continue;
                }
                const int is_storage = (e->descriptor_type == 7 || e->descriptor_type == 9);
                const int is_write = (is_storage && e->binding == 1) ? 1 : 0;
                add_resource_closure(cmd, r->id, is_write != 0);
                const uint64_t rsz =
                    r->buffer_size ? r->buffer_size
                                   : (e->range && e->range != UINT64_MAX ? e->range
                                                                        : r->requirements_size);
                if (is_fhd_class_size(rsz)) {
                    ensure_lease_for_resource_use(r);
                    fhd_attached++;
                }
                if (cmd->push_binding_count < kMaxPushBindings) {
                    int slot = -1;
                    for (int p = 0; p < cmd->push_binding_count; ++p) {
                        if (cmd->push_bindings[p].binding == e->binding) {
                            slot = p;
                            break;
                        }
                    }
                    if (slot < 0) {
                        slot = cmd->push_binding_count++;
                    }
                    cmd->push_bindings[slot].buffer = e->buffer;
                    cmd->push_bindings[slot].offset = e->offset;
                    cmd->push_bindings[slot].range = e->range;
                    cmd->push_bindings[slot].binding = e->binding;
                    cmd->push_bindings[slot].descriptor_type = e->descriptor_type;
                    cmd->push_bindings[slot].is_write = (uint8_t)is_write;
                }
                strncpy(r->last_op, is_write ? "desc_bind_write" : "desc_bind_read",
                        sizeof(r->last_op) - 1);
                attached++;
            }
            break;
        }
    }
    static uint64_t s_bind_log;
    if (rate_allow(&s_bind_log) || fhd_attached > 0) {
        VT_TRACK_LOG(
            "DESC_SET_BIND cmd=%p setCount=%u attachedBuffers=%d fhd=%d pushSlots=%d cmdRefs=%d "
            "path=detile_stamp",
            commandBuffer, setCount, attached, fhd_attached, cmd->push_binding_count,
            cmd->resource_count);
    }
}

void VortekGpuTrack_onPushDescriptorBuffers(void* commandBuffer, uint32_t setIndex,
                                            const uint32_t* bindings,
                                            const uint32_t* descriptorTypes, void* const* buffers,
                                            const uint64_t* offsets, const uint64_t* ranges,
                                            uint32_t count) {
    VortekGpuTrack_initOnce();
    if (!g_detile_stamp || !commandBuffer || !buffers || count == 0) {
        return;
    }
    TrackedCommandBuffer* cmd = find_or_create_cmd(commandBuffer);
    if (!cmd) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        void* buf = buffers[i];
        if (!buf) {
            continue;
        }
        const uint32_t binding = bindings ? bindings[i] : i;
        const uint32_t dtype = descriptorTypes ? descriptorTypes[i] : 0;
        const uint64_t off = offsets ? offsets[i] : 0;
        const uint64_t range = ranges ? ranges[i] : 0;
        /* Detile layout: binding0 storage=src read, binding1 storage=dst write.
         * VK_DESCRIPTOR_TYPE_STORAGE_BUFFER=7, STORAGE_BUFFER_DYNAMIC=9. */
        const int is_storage = (dtype == 7 || dtype == 9);
        const int is_write = (is_storage && binding == 1) ? 1 : 0;

        /* Upsert push binding slot by binding index. */
        int slot = -1;
        for (int p = 0; p < cmd->push_binding_count; ++p) {
            if (cmd->push_bindings[p].binding == binding) {
                slot = p;
                break;
            }
        }
        if (slot < 0) {
            if (cmd->push_binding_count >= kMaxPushBindings) {
                continue;
            }
            slot = cmd->push_binding_count++;
        }
        cmd->push_bindings[slot].buffer = buf;
        cmd->push_bindings[slot].offset = off;
        cmd->push_bindings[slot].range = range;
        cmd->push_bindings[slot].binding = binding;
        cmd->push_bindings[slot].descriptor_type = dtype;
        cmd->push_bindings[slot].is_write = (uint8_t)is_write;

        TrackedResource* r = find_resource(buf);
        if (r) {
            const uint64_t rsz =
                r->buffer_size ? r->buffer_size
                               : (range && range != UINT64_MAX ? range : r->requirements_size);
            /* Always close resources for submit stamps; lease only FHD detile-class. */
            add_resource_closure(cmd, r->id, is_write != 0);
            if (is_fhd_class_size(rsz) || is_fhd_class_size(range)) {
                ensure_lease_for_resource_use(r);
            }
            strncpy(r->last_op, is_write ? "push_desc_write" : "push_desc_read",
                    sizeof(r->last_op) - 1);
            if (is_fhd_class_size(rsz) || is_fhd_class_size(range)) {
                TrackedGpuAllocation* a =
                    r->allocation_id ? find_alloc_by_id(r->allocation_id) : NULL;
                VT_TRACK_LOG(
                    "GPU_MAPPING_USE allocation=%" PRIu64 " resource=%" PRIu64
                    " binding=%u isWrite=%d offset=0x%" PRIx64 " range=0x%" PRIx64
                    " mapping=%" PRIu64 " gpuVaMapping=%" PRIu64 " set=%u",
                    a ? a->id : 0, r->id, binding, is_write, off, range,
                    a ? a->mapping_id : 0, a ? a->gpu_va_mapping_id : 0, setIndex);
            }
        }
    }
    static uint64_t s_push_log;
    if (rate_allow(&s_push_log)) {
        VT_TRACK_LOG(
            "PUSH_DESCRIPTOR_BUFFERS cmd=%p set=%u count=%u pushSlots=%d cmdRefs=%d",
            commandBuffer, setIndex, count, cmd->push_binding_count, cmd->resource_count);
    }
    (void)setIndex;
}

void VortekGpuTrack_onCmdDispatch(void* commandBuffer, uint32_t groupCountX, uint32_t groupCountY,
                                  uint32_t groupCountZ) {
    VortekGpuTrack_initOnce();
    /* Baseline: original null-resource GPU_ACCESS only (deep-run behavior). */
    if (!g_detile_stamp) {
        VortekGpuTrack_onGpuAccess(commandBuffer, "dispatch", NULL, 0, 0, NULL, 0, 0, groupCountX,
                                   groupCountY, groupCountZ, -1);
        return;
    }
    TrackedCommandBuffer* cmd = find_or_create_cmd(commandBuffer);
    void* src_buf = NULL;
    void* dst_buf = NULL;
    uint64_t src_off = 0;
    uint64_t src_sz = 0;
    uint64_t dst_off = 0;
    uint64_t dst_sz = 0;
    uint32_t src_binding = 0;
    uint32_t dst_binding = 1;
    uint64_t src_gen = 0;
    uint64_t dst_gen = 0;

    if (cmd && g_detile_stamp) {
        /* Prefer explicit detile bindings 0/1. */
        for (int p = 0; p < cmd->push_binding_count; ++p) {
            CmdPushBinding* b = &cmd->push_bindings[p];
            if (!b->buffer) {
                continue;
            }
            if (b->binding == 0 && !src_buf) {
                src_buf = b->buffer;
                src_off = b->offset;
                src_sz = b->range;
                src_binding = b->binding;
            } else if (b->binding == 1 && !dst_buf) {
                dst_buf = b->buffer;
                dst_off = b->offset;
                dst_sz = b->range;
                dst_binding = b->binding;
            }
        }
        /* Fallback: first two push buffers. */
        if (!src_buf && cmd->push_binding_count > 0) {
            src_buf = cmd->push_bindings[0].buffer;
            src_off = cmd->push_bindings[0].offset;
            src_sz = cmd->push_bindings[0].range;
            src_binding = cmd->push_bindings[0].binding;
        }
        if (!dst_buf && cmd->push_binding_count > 1) {
            dst_buf = cmd->push_bindings[1].buffer;
            dst_off = cmd->push_bindings[1].offset;
            dst_sz = cmd->push_bindings[1].range;
            dst_binding = cmd->push_bindings[1].binding;
        }
    }

    TrackedResource* src = find_resource(src_buf);
    TrackedResource* dst = find_resource(dst_buf);
    TrackedGpuAllocation* srcA =
        src && src->allocation_id ? find_alloc_by_id(src->allocation_id) : NULL;
    TrackedGpuAllocation* dstA =
        dst && dst->allocation_id ? find_alloc_by_id(dst->allocation_id) : NULL;
    if (src) {
        if (src_sz == 0) {
            src_sz = src->buffer_size ? src->buffer_size : src->requirements_size;
        }
        if (is_fhd_class_size(src_sz) || is_fhd_class_size(src->buffer_size)) {
            SuballocLease* L = ensure_lease_for_resource_use(src);
            if (L) {
                src_gen = L->generation;
            }
        }
        add_resource_closure(cmd, src->id, false);
    }
    if (dst) {
        if (dst_sz == 0) {
            dst_sz = dst->buffer_size ? dst->buffer_size : dst->requirements_size;
        }
        if (is_fhd_class_size(dst_sz) || is_fhd_class_size(dst->buffer_size)) {
            SuballocLease* L = ensure_lease_for_resource_use(dst);
            if (L) {
                dst_gen = L->generation;
            }
        }
        add_resource_closure(cmd, dst->id, true);
    }

    const uint64_t sub = g_current_pending_submission ? g_current_pending_submission
                                                      : (g_last_submitted + 1);
    VT_TRACK_LOG(
        "DETILE_DISPATCH submission=%" PRIu64 " srcResource=%" PRIu64 " srcAllocation=%" PRIu64
        " srcOffset=0x%" PRIx64 " srcSize=0x%" PRIx64 " srcGeneration=%" PRIu64
        " srcBinding=%u srcBacking=%" PRIu64 " srcMapping=%" PRIu64
        " srcBindGen=%" PRIu64 " srcGpuBound=%d srcBase=0x%" PRIx64
        " dstResource=%" PRIu64 " dstAllocation=%" PRIu64 " dstOffset=0x%" PRIx64
        " dstSize=0x%" PRIx64 " dstGeneration=%" PRIu64 " dstBinding=%u "
        "groups=%ux%ux%u cmdRefs=%d pushSlots=%d",
        sub, src ? src->id : 0, srcA ? srcA->id : 0, src_off, src_sz, src_gen, src_binding,
        srcA ? srcA->backing_id : 0, srcA ? srcA->mapping_id : 0,
        srcA ? srcA->bind_generation : 0, srcA && srcA->gpu_address_bound ? 1 : 0,
        srcA ? srcA->gpu_bind_address : 0, dst ? dst->id : 0,
        dstA ? dstA->id : 0, dst_off, dst_sz, dst_gen, dst_binding, groupCountX, groupCountY,
        groupCountZ, cmd ? cmd->resource_count : 0, cmd ? cmd->push_binding_count : 0);
    /* Descriptor snapshot generation vs live bind generation. */
    if (cmd) {
        for (int s = 0; s < cmd->bound_set_count; ++s) {
            void* set = cmd->bound_sets[s];
            for (int i = 0; i < kMaxDescSets; ++i) {
                if (!g_desc_sets[i].used || g_desc_sets[i].set != set) {
                    continue;
                }
                for (int b = 0; b < g_desc_sets[i].buffer_count; ++b) {
                    DescBufEntry* e = &g_desc_sets[i].entries[b];
                    TrackedResource* er = find_resource(e->buffer);
                    if (!er || !er->allocation_id) {
                        continue;
                    }
                    TrackedGpuAllocation* ea = find_alloc_by_id(er->allocation_id);
                    if (!ea) {
                        continue;
                    }
                    if (e->bind_generation != 0 && ea->bind_generation != 0 &&
                        e->bind_generation != ea->bind_generation) {
                        VT_TRACK_LOG(
                            "DESCRIPTOR_REFERENCES_OLD_BIND_GENERATION resource=%" PRIu64
                            " allocation=%" PRIu64 " descGen=%" PRIu64 " liveGen=%" PRIu64
                            " binding=%u path=CmdDispatch",
                            er->id, ea->id, e->bind_generation, ea->bind_generation,
                            e->binding);
                    }
                }
                break;
            }
        }
    }
    if (g_pin_fhd_detile_sources && srcA && is_fhd_class_size(src_sz)) {
        srcA->fhd_pinned = true;
    }

    VortekGpuTrack_onGpuAccess(commandBuffer, "dispatch", src_buf, src_off, src_sz, dst_buf,
                               dst_off, dst_sz, groupCountX, groupCountY, groupCountZ, -1);
}

void VortekGpuTrack_onBufferDeviceAddress(void* buffer, uint64_t address, uint64_t size) {
    VortekGpuTrack_initOnce();
    if (!buffer || address == 0) {
        return;
    }
    TrackedResource* r = find_resource(buffer);
    if (r) {
        r->bda_enabled = true;
        r->bda_address = address;
        if (size) {
            r->bda_size = size;
        } else if (r->buffer_size) {
            r->bda_size = r->buffer_size;
        }
    }
    int slot = -1;
    for (int i = 0; i < g_bda_count; ++i) {
        if (g_bda[i].used && g_bda[i].buffer == buffer) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_bda_count >= kMaxBda) {
            VT_TRACK_LOG("VORTEK_TRACK_OVERFLOW kind=bda");
            return;
        }
        slot = g_bda_count++;
        memset(&g_bda[slot], 0, sizeof(g_bda[slot]));
        g_bda[slot].used = true;
        g_bda[slot].buffer = buffer;
    }
    g_bda[slot].address = address;
    g_bda[slot].size = r ? r->bda_size : size;
    VT_TRACK_LOG(
        "BDA_USE_RESOLVED buffer=%p resource=%" PRIu64 " address=0x%" PRIx64 " size=%" PRIu64,
        buffer, r ? r->id : 0, address, g_bda[slot].size);
}

void VortekGpuTrack_noteDirectDestroyBufferBypass(void* buffer, void* device, const char* site) {
    g_direct_destroy_bypass_count++;
    VT_TRACK_LOG(
        "DIRECT_VK_DESTROY_BUFFER_BYPASS buffer=%p device=%p site=%s count=%" PRIu64,
        buffer, device, site ? site : "?", g_direct_destroy_bypass_count);
    VortekGpuTrack_freeflightEvent("DIRECT_DESTROY_BUF", g_last_queue, g_last_submitted, buffer, 0,
                                   0, 0, 0, g_last_completed, 0, 1, 1);
}

void VortekGpuTrack_noteSharedAhb(void* ahb, uint32_t imageCount) {
    g_shared_ahb = ahb;
    g_shared_ahb_image_count = (int)imageCount;
    VT_TRACK_LOG(
        "SHARED_AHB ahb=%p imageCount=%u note=two-image-rotation-cosmetic-serialize-writes",
        ahb, imageCount);
}

void VortekGpuTrack_noteSingleImageCap(uint32_t requested, uint32_t actual) {
    VT_TRACK_LOG(
        "SINGLE_IMAGE_CAP requested=%u actual=%u reason=single_window_ahb_no_alias",
        requested, actual);
}

void VortekGpuTrack_logBusyAhb(void* ahb, uint32_t newImageIndex, uint32_t busyImageIndex) {
    VT_TRACK_LOG(
        "PRESENT_ON_BUSY_AHB ahb=%p newImageIndex=%u busyImageIndex=%u", ahb, newImageIndex,
        busyImageIndex);
    VT_TRACK_LOG(
        "AHB_REUSED_BEFORE_RELEASE ahb=%p newImageIndex=%u busyImageIndex=%u", ahb,
        newImageIndex, busyImageIndex);
    if (g_wait_for_ahb_release_fence) {
        VT_TRACK_LOG("AHB_RELEASE_FENCE_NOT_SIGNALED ahb=%p imageIndex=%u", ahb, busyImageIndex);
    }
}

void VortekGpuTrack_forcePresentCompleteForAhb(void* ahb, const char* source) {
    for (uint32_t i = 0; i < kMaxPresentImages; ++i) {
        PresentImageState* s = &g_present_images[i];
        if (!s->used) {
            continue;
        }
        if (ahb && s->ahb != ahb) {
            continue;
        }
        if (s->present_pending || s->life == PRESENT_LIFE_PRESENT_PENDING) {
            VortekGpuTrack_notePresentComplete(s->present_id, s->image_index,
                                               source ? source : "force_ahb");
        }
    }
}

void VortekGpuTrack_noteSyncFd(int fd, const char* owner, uint32_t imageIndex, uint64_t presentId,
                               int imported, int waited, int closed) {
    if (fd < 0) {
        return;
    }
    VortekGpuTrack_initOnce();
    SyncFdTrack* slot = NULL;
    for (int i = 0; i < g_sync_fd_count; ++i) {
        if (g_sync_fds[i].used && g_sync_fds[i].fd == fd) {
            slot = &g_sync_fds[i];
            break;
        }
    }
    if (!slot && g_sync_fd_count < kMaxSyncFds) {
        slot = &g_sync_fds[g_sync_fd_count++];
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->fd = fd;
    }
    if (!slot) {
        return;
    }
    if (imported && slot->imported && !slot->closed) {
        VT_TRACK_LOG("SYNC_FD_IMPORTED_TWICE fd=%d owner=%s imageIndex=%u presentId=%" PRIu64,
                     fd, owner ? owner : "?", imageIndex, presentId);
    }
    if (closed && slot->closed) {
        VT_TRACK_LOG("SYNC_FD_REUSED fd=%d owner=%s", fd, owner ? owner : "?");
    }
    if (waited && slot->closed) {
        VT_TRACK_LOG("SYNC_FD_WAIT_AFTER_TRANSFER fd=%d owner=%s", fd, owner ? owner : "?");
    }
    if (closed && slot->imported && !slot->waited && slot->owner[0]) {
        VT_TRACK_LOG("SYNC_FD_CLOSED_EARLY fd=%d owner=%s imageIndex=%u", fd,
                     owner ? owner : "?", imageIndex);
    }
    if (owner) {
        strncpy(slot->owner, owner, sizeof(slot->owner) - 1);
    }
    slot->image_index = imageIndex;
    slot->present_id = presentId;
    if (imported) {
        slot->imported = true;
    }
    if (waited) {
        slot->waited = true;
    }
    if (closed) {
        slot->closed = true;
    }
    VT_TRACK_LOG(
        "SYNC_FD fd=%d owner=%s imageIndex=%u presentId=%" PRIu64
        " imported=%d waited=%d closed=%d",
        fd, owner ? owner : "?", imageIndex, presentId, imported, waited, closed);
}

void VortekGpuTrack_syncFdImported(int fd, const char* owner, uint32_t imageIndex,
                                   uint64_t presentId) {
    VortekGpuTrack_noteSyncFd(fd, owner, imageIndex, presentId, 1, 0, 0);
}

void VortekGpuTrack_syncFdWaited(int fd, const char* owner) {
    VortekGpuTrack_noteSyncFd(fd, owner, 0, 0, 0, 1, 0);
}

void VortekGpuTrack_syncFdClosed(int fd, const char* owner) {
    VortekGpuTrack_noteSyncFd(fd, owner, 0, 0, 0, 0, 1);
}

bool VortekGpuTrack_waitBeforePresentSemReuse(void) {
    VortekGpuTrack_initOnce();
    return g_wait_before_present_sem_reuse;
}

bool VortekGpuTrack_waitBeforeSameImageReuse(void) {
    VortekGpuTrack_initOnce();
    return g_wait_before_same_image_reuse;
}

bool VortekGpuTrack_waitBeforeSameAhbReuse(void) {
    VortekGpuTrack_initOnce();
    return g_wait_before_same_ahb_reuse;
}

bool VortekGpuTrack_waitForAhbReleaseFence(void) {
    VortekGpuTrack_initOnce();
    return g_wait_for_ahb_release_fence;
}

bool VortekGpuTrack_waitOnSuballocOverlapEnabled(void) {
    VortekGpuTrack_initOnce();
    /* pending only counts when dig wait path stashed — avoid sticky enable. */
    return g_wait_on_suballoc_overlap || g_suballoc_range_pool ||
           (g_detile_source_exact_wait && g_suballoc_wait.pending);
}

bool VortekGpuTrack_detileStampEnabled(void) {
    VortekGpuTrack_initOnce();
    return g_detile_stamp;
}

bool VortekGpuTrack_detileSourceExactWaitEnabled(void) {
    VortekGpuTrack_initOnce();
    return g_detile_source_exact_wait;
}

bool VortekGpuTrack_pinFhdDetileSourcesEnabled(void) {
    VortekGpuTrack_initOnce();
    return g_pin_fhd_detile_sources;
}

bool VortekGpuTrack_gpuAddressBindingReportEnabled(void) {
    VortekGpuTrack_initOnce();
    return g_gpu_address_binding_report;
}

bool VortekGpuTrack_fhdPrepareWriteDiagEnabled(void) {
    VortekGpuTrack_initOnce();
    return g_fhd_prepare_write_diag;
}

bool VortekGpuTrack_deviceFaultQueryWanted(void) {
    VortekGpuTrack_initOnce();
    return g_device_fault_query_wanted;
}

bool VortekGpuTrack_suballocRangePoolEnabled(void) {
    VortekGpuTrack_initOnce();
    return g_suballoc_range_pool;
}

static int collect_suballoc_wait_fences(void** outFences, int maxCount) {
    if (!outFences || maxCount <= 0 || !g_suballoc_wait.pending) {
        return 0;
    }
    int n = 0;
    /* 0) Durable serial→fence map (survives ring wrap). Prefer exact/later serial. */
    n = collect_durable_fences_for_need(g_suballoc_wait.queue_index, g_suballoc_wait.queue_serial,
                                        outFences, maxCount);
    /* Also per-queue needs from multi-queue overlap. */
    for (int qi = 0; qi < kMaxQueues && n < maxCount; ++qi) {
        const uint64_t need = g_suballoc_wait.max_qserial_per_queue[qi];
        if (need == 0) {
            continue;
        }
        void* tmp[kMaxSuballocWaitFences];
        int m = collect_durable_fences_for_need(qi, need, tmp, kMaxSuballocWaitFences);
        for (int i = 0; i < m && n < maxCount; ++i) {
            int dup = 0;
            for (int j = 0; j < n; ++j) {
                if (outFences[j] == tmp[i]) {
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                outFences[n++] = tmp[i];
            }
        }
    }
    /* 1) Live submission ring. */
    for (int si = 0; si < kSubRing && n < maxCount; ++si) {
        SubmissionRecord* rec = &g_subs[si];
        if (!rec->active || rec->completed) {
            continue;
        }
        const int rqi = rec->queue_index;
        if (rqi < 0 || rqi >= kMaxQueues) {
            continue;
        }
        const uint64_t need = g_suballoc_wait.max_qserial_per_queue[rqi];
        if (need == 0 || rec->queue_serial == 0 || rec->queue_serial > need) {
            continue;
        }
        void* f = rec->completion_fence ? rec->completion_fence : rec->fence;
        if (!f) {
            continue;
        }
        int dup = 0;
        for (int j = 0; j < n; ++j) {
            if (outFences[j] == f) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            outFences[n++] = f;
        }
    }
    /* 2) Fallback: any pending completion fence on the worst queue. */
    if (n == 0) {
        for (int si = 0; si < kSubRing && n < maxCount; ++si) {
            SubmissionRecord* rec = &g_subs[si];
            if (!rec->active || rec->completed) {
                continue;
            }
            if (g_suballoc_wait.queue_index >= 0 &&
                rec->queue_index != g_suballoc_wait.queue_index) {
                continue;
            }
            void* f = rec->completion_fence ? rec->completion_fence : rec->fence;
            if (!f) {
                continue;
            }
            int dup = 0;
            for (int j = 0; j < n; ++j) {
                if (outFences[j] == f) {
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                outFences[n++] = f;
            }
        }
    }
    /* 3) Global pending fences if still empty. */
    if (n == 0) {
        n = VortekGpuTrack_fillPendingCompletionFences(outFences, maxCount);
    }
    return n;
}

int VortekGpuTrack_fillSuballocOverlapWaitFences(void** outFences, int maxCount,
                                                 uint64_t* outMaxSubmission,
                                                 int* outQueueIndex,
                                                 uint64_t* outQueueSerial) {
    VortekGpuTrack_initOnce();
    if (!g_suballoc_wait.pending) {
        if (outMaxSubmission) {
            *outMaxSubmission = 0;
        }
        if (outQueueIndex) {
            *outQueueIndex = -1;
        }
        if (outQueueSerial) {
            *outQueueSerial = 0;
        }
        return 0;
    }
    if (outMaxSubmission) {
        *outMaxSubmission = g_suballoc_wait.max_overlap_use;
    }
    if (outQueueIndex) {
        *outQueueIndex = g_suballoc_wait.queue_index;
    }
    if (outQueueSerial) {
        *outQueueSerial = g_suballoc_wait.queue_serial;
    }
    /* Refresh fence list from live submission ring (may have new tokens). */
    g_suballoc_wait.fence_count =
        collect_suballoc_wait_fences(g_suballoc_wait.fences, kMaxSuballocWaitFences);
    if (!outFences || maxCount <= 0) {
        return g_suballoc_wait.fence_count;
    }
    int n = g_suballoc_wait.fence_count;
    if (n > maxCount) {
        n = maxCount;
    }
    for (int i = 0; i < n; ++i) {
        outFences[i] = g_suballoc_wait.fences[i];
    }
    return n;
}

bool VortekGpuTrack_suballocOverlapStillIncomplete(void) {
    VortekGpuTrack_initOnce();
    if (!g_suballoc_wait.pending) {
        return false;
    }
    for (int qi = 0; qi < kMaxQueues; ++qi) {
        const uint64_t need = g_suballoc_wait.max_qserial_per_queue[qi];
        if (need == 0) {
            continue;
        }
        if (!g_queues[qi].used || g_queues[qi].completed_serial < need) {
            return true;
        }
    }
    return false;
}

void* VortekGpuTrack_suballocOverlapQueue(void) {
    VortekGpuTrack_initOnce();
    const int qi = g_suballoc_wait.queue_index;
    if (qi >= 0 && qi < kMaxQueues && g_queues[qi].used) {
        return g_queues[qi].queue;
    }
    return NULL;
}

void VortekGpuTrack_noteSuballocTargetedWait(uint64_t maxSubmission, int queueIndex,
                                             uint64_t queueSerial, int fenceCount,
                                             int vkResult) {
    VortekGpuTrack_initOnce();
    const int qi = queueIndex >= 0 ? queueIndex : g_suballoc_wait.queue_index;
    const uint64_t need =
        queueSerial ? queueSerial : g_suballoc_wait.queue_serial;
    const uint64_t done =
        (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_serial : g_last_completed;
    const bool still = VortekGpuTrack_suballocOverlapStillIncomplete();
    VT_TRACK_LOG(
        "SUBALLOC_TARGETED_WAIT allocation=%" PRIu64 " bindOffset=0x%" PRIx64
        " size=0x%" PRIx64 " newResource=%" PRIu64 " maxOverlapUse=%" PRIu64
        " queue=%d queueSerial=%" PRIu64 " fenceCount=%d result=%d completedSerial=%" PRIu64
        " neededSerial=%" PRIu64 " stillIncomplete=%d",
        g_suballoc_wait.allocation_id, g_suballoc_wait.bind_offset, g_suballoc_wait.size,
        g_suballoc_wait.new_resource_id,
        maxSubmission ? maxSubmission : g_suballoc_wait.max_overlap_use, qi, need, fenceCount,
        vkResult, done, need, still ? 1 : 0);
    if (fenceCount > 0) {
        VT_TRACK_LOG(
            "SUBALLOC_TARGETED_FENCE_WAIT allocation=%" PRIu64 " queue=%d queueSerial=%" PRIu64
            " fenceCount=%d result=%d",
            g_suballoc_wait.allocation_id, qi, need, fenceCount, vkResult);
    }

    if (!still && g_suballoc_wait.pending) {
        VT_TRACK_LOG(
            "SUBALLOC_REUSE_ALLOWED allocation=%" PRIu64 " bindOffset=0x%" PRIx64
            " size=0x%" PRIx64 " newResource=%" PRIu64 " queue=%d completedSerial=%" PRIu64
            " neededSerial=%" PRIu64,
            g_suballoc_wait.allocation_id, g_suballoc_wait.bind_offset, g_suballoc_wait.size,
            g_suballoc_wait.new_resource_id, qi, done, need);
        VT_TRACK_LOG(
            "DETILE_SOURCE_REUSE_ALLOWED allocation=%" PRIu64 " bindOffset=0x%" PRIx64
            " size=0x%" PRIx64 " completedSerial=%" PRIu64 " neededSerial=%" PRIu64
            " path=targetedWait",
            g_suballoc_wait.allocation_id, g_suballoc_wait.bind_offset, g_suballoc_wait.size,
            done, need);
        /* Open new generation only after prior use completed. */
        if (g_suballoc_wait.allocation_id && g_suballoc_wait.size > 0) {
            begin_new_generation(g_suballoc_wait.allocation_id, g_suballoc_wait.bind_offset,
                                 g_suballoc_wait.size, g_suballoc_wait.new_resource_id);
        }
    } else if (g_suballoc_wait.pending) {
        VT_TRACK_LOG(
            "SUBALLOC_TARGETED_WAIT_INCOMPLETE allocation=%" PRIu64 " maxOverlapUse=%" PRIu64
            " queue=%d queueSerial=%" PRIu64 " fenceCount=%d result=%d "
            "(bind proceeds after wait attempts)",
            g_suballoc_wait.allocation_id, g_suballoc_wait.max_overlap_use, qi, need, fenceCount,
            vkResult);
        /* Still open generation so subsequent submits stamp a lease (fail-open path). */
        if (g_suballoc_wait.allocation_id && g_suballoc_wait.size > 0) {
            begin_new_generation(g_suballoc_wait.allocation_id, g_suballoc_wait.bind_offset,
                                 g_suballoc_wait.size, g_suballoc_wait.new_resource_id);
        }
    }
    memset(&g_suballoc_wait, 0, sizeof(g_suballoc_wait));
    VortekGpuTrack_maybeLogSuballocPoolStats();
    (void)maxSubmission;
}

int VortekGpuTrack_acquireSuballocLease(void* buffer, ResourceMemory* rm, uint64_t bindOffset,
                                        uint64_t size, uint64_t alignment,
                                        uint64_t* out_generation) {
    (void)alignment;
    VortekGpuTrack_initOnce();
    if (out_generation) {
        *out_generation = 0;
    }
    /* onBindBuffer performs detect+stash; this helper reports pending wait. */
    VortekGpuTrack_onBindBuffer(buffer, rm, bindOffset, size, alignment);
    if (out_generation) {
        TrackedResource* r = find_resource(buffer);
        TrackedGpuAllocation* a = find_alloc_by_rm(rm);
        if (r && a) {
            SuballocLease* L = find_lease_for_range(a->id, bindOffset, size);
            if (L) {
                *out_generation = L->generation;
            } else if (g_suballoc_wait.pending) {
                *out_generation = g_suballoc_wait.generation;
            }
        }
    }
    return g_suballoc_wait.pending ? 1 : 0;
}

int VortekGpuTrack_prepareCpuWrite(ResourceMemory* rm, uint64_t offset, uint64_t size,
                                   uint64_t* out_generation) {
    VortekGpuTrack_initOnce();
    if (out_generation) {
        *out_generation = 0;
    }
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!a) {
        return 0;
    }
    /* Whole-map: offset/size 0 means entire allocation. */
    const uint64_t off = offset;
    const uint64_t sz = size > 0 ? size : a->allocation_size;
    if (is_fhd_class_size(sz) || is_fhd_class_size(a->allocation_size)) {
        if (g_pin_fhd_detile_sources) {
            a->fhd_pinned = true;
        }
        if (g_fhd_prepare_write_diag) {
            const bool would_block = a->last_gpu_read_submission > g_last_completed ||
                                     a->last_submitted_use > g_last_completed;
            VT_TRACK_LOG(
                "FHD_PREPARE_WRITE_CHECK resource=0 allocation=%" PRIu64 " generation=%" PRIu64
                " path=MapWrite lastGpuRead=%" PRIu64 " lastSubmittedUse=%" PRIu64
                " completedGlobal=%" PRIu64 " completedQueueSerial=%" PRIu64
                " checkHit=1 wouldBlock=%d offset=0x%" PRIx64 " size=0x%" PRIx64
                " gpuBound=%d baseAddress=0x%" PRIx64 " fhdPinned=%d",
                a->id, a->bind_generation, a->last_gpu_read_submission, a->last_submitted_use,
                g_last_completed, g_last_completed, would_block ? 1 : 0, off, sz,
                a->gpu_address_bound ? 1 : 0, a->gpu_bind_address, a->fhd_pinned ? 1 : 0);
        }
    }
    /* Detile exact wait must NOT fire on whole-heap MapMemory — that froze SEGA.
     * Only classic suballoc flags apply to unrestricted map; FHD-targeted wait is
     * prepareResourceWrite on copy_buffer dest. */
    int count = 0;
    uint64_t max_g = 0;
    int worst_qi = -1;
    uint64_t worst_serial = 0;
    uint64_t max_q[kMaxQueues];
    if (!lease_incomplete_overlap(a->id, off, sz, &max_g, &worst_qi, &worst_serial, max_q,
                                  &count)) {
        return 0;
    }
    const bool wait_enabled = g_wait_on_suballoc_overlap || g_suballoc_range_pool;
    if (!wait_enabled || worst_serial == 0) {
        if (max_g > g_last_completed &&
            (is_fhd_class_size(sz) || a->last_gpu_read_submission > g_last_completed)) {
            VT_TRACK_LOG(
                "DETILE_SOURCE_REUSE_UNSAFE allocation=%" PRIu64 " offset=0x%" PRIx64
                " size=0x%" PRIx64 " neededSerial=%" PRIu64 " completedSerial=%" PRIu64
                " path=prepareCpuWrite log_only",
                a->id, off, sz, max_g, g_last_completed);
        }
        return 0;
    }
    g_sub_stats_cpu_blocked++;
    VT_TRACK_LOG(
        "SUBALLOC_CPU_WRITE_BLOCKED_IN_FLIGHT allocation=%" PRIu64 " offset=0x%" PRIx64
        " size=0x%" PRIx64 " overlapCount=%d queue=%d queueSerial=%" PRIu64,
        a->id, off, sz, count, worst_qi, worst_serial);
    stash_suballoc_wait(a->id, off, sz, 0, 0, max_g, worst_qi, worst_serial, count, max_q,
                        1 /* cpu_write */);
    g_suballoc_wait.fence_count = collect_durable_fences_for_need(
        worst_qi, worst_serial, g_suballoc_wait.fences, kMaxSuballocWaitFences);
    if (out_generation) {
        *out_generation = g_suballoc_wait.generation;
    }
    return 1;
}

int VortekGpuTrack_prepareResourceWrite(void* resource, uint64_t offset, uint64_t size,
                                        const char* path) {
    VortekGpuTrack_initOnce();
    if (!resource) {
        return 0;
    }
    TrackedResource* r = find_resource(resource);
    if (!r || r->kind != TRACK_RES_BUFFER) {
        return 0;
    }
    TrackedGpuAllocation* a = r->allocation_id ? find_alloc_by_id(r->allocation_id) : NULL;
    const uint64_t off = r->bind_offset + offset;
    const uint64_t sz =
        size > 0 ? size
                 : (r->buffer_size > 0 ? r->buffer_size
                                       : (a ? a->allocation_size : r->requirements_size));
    const char* p = path && path[0] ? path : "Copy";

    /* Only FHD-class buffer_cache detile sources — never stall small copies. */
    if (!is_fhd_class_size(sz) && !is_fhd_class_size(r->buffer_size) &&
        !(a && is_fhd_class_size(a->allocation_size))) {
        return 0;
    }

    if (g_pin_fhd_detile_sources && a) {
        a->fhd_pinned = true;
    }

    const bool read_inflight = resource_gpu_read_incomplete(r) ||
                               (a && a->last_gpu_read_submission > g_last_completed);
    const bool submit_inflight =
        (r->last_submitted_use > g_last_completed) ||
        (a && a->last_submitted_use > g_last_completed && a->pending_submission_refs > 0);
    const bool would_block = read_inflight || submit_inflight;
    uint64_t completed_q = g_last_completed;
    for (int u = 0; u < kMaxQueueUses; ++u) {
        if (r->last_use[u].queue_index >= 0 && r->last_use[u].queue_index < kMaxQueues &&
            g_queues[r->last_use[u].queue_index].used) {
            uint64_t cs = g_queues[r->last_use[u].queue_index].completed_serial;
            if (cs < completed_q) {
                completed_q = cs;
            }
        }
    }
    /* FHD write check log (M1e: disable via fhd_prepare_write_diag=0). */
    if (g_fhd_prepare_write_diag) {
        VT_TRACK_LOG(
            "FHD_PREPARE_WRITE_CHECK resource=%" PRIu64 " allocation=%" PRIu64
            " generation=%" PRIu64 " path=%s lastGpuRead=%" PRIu64 " lastSubmittedUse=%" PRIu64
            " completedGlobal=%" PRIu64 " completedQueueSerial=%" PRIu64
            " checkHit=1 wouldBlock=%d offset=0x%" PRIx64 " size=0x%" PRIx64
            " gpuBound=%d baseAddress=0x%" PRIx64 " bindGen=%" PRIu64 " fhdPinned=%d",
            r->id, a ? a->id : 0, a ? a->bind_generation : r->bind_generation, p,
            r->last_gpu_read_submission > (a ? a->last_gpu_read_submission : 0)
                ? r->last_gpu_read_submission
                : (a ? a->last_gpu_read_submission : 0),
            r->last_submitted_use, g_last_completed, completed_q, would_block ? 1 : 0, off, sz,
            a && a->gpu_address_bound ? 1 : 0, a ? a->gpu_bind_address : 0,
            a ? a->bind_generation : 0, a && a->fhd_pinned ? 1 : 0);
    }

    if (read_inflight) {
        VT_TRACK_LOG(
            "DETILE_SOURCE_REUSE_UNSAFE source=%" PRIu64 " allocation=%" PRIu64
            " offset=0x%" PRIx64 " size=0x%" PRIx64 " neededSerial=%" PRIu64
            " completedSerial=%" PRIu64 " lastGpuReadRes=%" PRIu64 " lastGpuReadAlloc=%" PRIu64
            " path=%s exactWait=%d",
            r->id, a ? a->id : 0, off, sz,
            r->last_gpu_read_submission > (a ? a->last_gpu_read_submission : 0)
                ? r->last_gpu_read_submission
                : (a ? a->last_gpu_read_submission : 0),
            g_last_completed, r->last_gpu_read_submission,
            a ? a->last_gpu_read_submission : 0, p, g_detile_source_exact_wait ? 1 : 0);
    }

    /* Default: stamp+log only. Exact wait needs prop=1 after stamp proven. */
    if (!g_detile_source_exact_wait) {
        return 0;
    }

    int count = 0;
    uint64_t max_g = 0;
    int worst_qi = -1;
    uint64_t worst_serial = 0;
    uint64_t max_q[kMaxQueues];
    memset(max_q, 0, sizeof(max_q));
    const bool lease_hit =
        a && lease_incomplete_overlap(a->id, off, sz, &max_g, &worst_qi, &worst_serial, max_q,
                                      &count);

    if (!lease_hit && !read_inflight) {
        if (is_fhd_class_size(sz)) {
            VT_TRACK_LOG(
                "DETILE_SOURCE_REUSE_ALLOWED allocation=%" PRIu64 " resource=%" PRIu64
                " offset=0x%" PRIx64 " size=0x%" PRIx64 " completedSerial=%" PRIu64
                " path=%s",
                a ? a->id : 0, r->id, off, sz, g_last_completed, p);
        }
        return 0;
    }

    if (!lease_hit || worst_serial == 0) {
        max_g = r->last_gpu_read_submission;
        if (a && a->last_gpu_read_submission > max_g) {
            max_g = a->last_gpu_read_submission;
        }
        for (int i = 0; i < kMaxQueueUses; ++i) {
            if (r->last_use[i].queue_index >= 0 && r->last_use[i].serial > 0) {
                if (r->last_use[i].serial > worst_serial) {
                    worst_serial = r->last_use[i].serial;
                    worst_qi = r->last_use[i].queue_index;
                }
                if (r->last_use[i].queue_index < kMaxQueues &&
                    r->last_use[i].serial > max_q[r->last_use[i].queue_index]) {
                    max_q[r->last_use[i].queue_index] = r->last_use[i].serial;
                }
            }
        }
        count = count > 0 ? count : 1;
    }

    if (worst_serial == 0 && max_g == 0) {
        return 0;
    }

    g_sub_stats_cpu_blocked++;
    VT_TRACK_LOG(
        "DETILE_SOURCE_REWRITE_BLOCKED allocation=%" PRIu64 " resource=%" PRIu64
        " offset=0x%" PRIx64 " size=0x%" PRIx64 " neededSerial=%" PRIu64
        " completedSerial=%" PRIu64 " queue=%d queueSerial=%" PRIu64 " path=%s",
        a ? a->id : 0, r->id, off, sz, max_g ? max_g : r->last_gpu_read_submission,
        g_last_completed, worst_qi, worst_serial, p);
    stash_suballoc_wait(a ? a->id : 0, off, sz, r->id, 0, max_g ? max_g : r->last_gpu_read_submission,
                        worst_qi, worst_serial, count, max_q, 1 /* cpu/gpu write reuse */);
    g_suballoc_wait.fence_count = collect_durable_fences_for_need(
        worst_qi, worst_serial, g_suballoc_wait.fences, kMaxSuballocWaitFences);
    VT_TRACK_LOG(
        "DETILE_SOURCE_TARGETED_WAIT allocation=%" PRIu64 " resource=%" PRIu64
        " neededSerial=%" PRIu64 " queue=%d queueSerial=%" PRIu64 " fenceCount=%d",
        a ? a->id : 0, r->id, g_suballoc_wait.max_overlap_use, worst_qi, worst_serial,
        g_suballoc_wait.fence_count);
    return 1;
}

void VortekGpuTrack_noteCpuWriteBegin(ResourceMemory* rm, uint64_t offset, uint64_t size) {
    VortekGpuTrack_initOnce();
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!a) {
        return;
    }
    const uint64_t off = offset;
    const uint64_t sz = size > 0 ? size : a->allocation_size;
    SuballocLease* L = find_lease_for_range(a->id, off, sz);
    if (L) {
        L->cpu_write_owner = true;
        L->state = kSubLeaseCpuOwned;
    }
    VT_TRACK_LOG("CPU_WRITE_BEGIN allocation=%" PRIu64 " offset=0x%" PRIx64 " size=0x%" PRIx64,
                 a->id, off, sz);
}

void VortekGpuTrack_noteCpuWriteEnd(ResourceMemory* rm, uint64_t offset, uint64_t size) {
    VortekGpuTrack_initOnce();
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!a) {
        return;
    }
    const uint64_t off = offset;
    const uint64_t sz = size > 0 ? size : a->allocation_size;
    SuballocLease* L = find_lease_for_range(a->id, off, sz);
    if (L) {
        L->cpu_write_owner = false;
    }
    VT_TRACK_LOG("CPU_WRITE_END allocation=%" PRIu64 " offset=0x%" PRIx64 " size=0x%" PRIx64,
                 a->id, off, sz);
}

void VortekGpuTrack_noteHostFlush(ResourceMemory* rm, uint64_t offset, uint64_t size) {
    VortekGpuTrack_initOnce();
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    if (!a) {
        return;
    }
    VT_TRACK_LOG("HOST_FLUSH allocation=%" PRIu64 " offset=0x%" PRIx64 " size=0x%" PRIx64, a->id,
                 offset, size > 0 ? size : a->allocation_size);
}

int VortekGpuTrack_tryReservePoolSlot(uint64_t size, uint32_t memoryTypeIndex,
                                      int* out_slot_index, uint64_t* out_generation) {
    (void)size;
    (void)memoryTypeIndex;
    VortekGpuTrack_initOnce();
    if (out_slot_index) {
        *out_slot_index = -1;
    }
    if (out_generation) {
        *out_generation = 0;
    }
    /* Physical slot table lives in request_handler; tracker only gatekeeps policy. */
    if (!g_suballoc_range_pool) {
        return 0;
    }
    if (g_sub_stats_pool_slots >= g_suballoc_pool_max_slots) {
        return 0;
    }
    if (g_sub_stats_pool_bytes + size > g_suballoc_pool_max_bytes) {
        return 0;
    }
    return 1; /* host may allocate a new slot */
}

void VortekGpuTrack_notePoolSlotBound(int slot_index, void* buffer, void* memory, uint64_t size,
                                     uint64_t generation) {
    (void)buffer;
    (void)memory;
    (void)generation;
    VortekGpuTrack_initOnce();
    g_sub_stats_busy_skipped++;
    VT_TRACK_LOG(
        "SUBALLOC_BUSY_SKIPPED slot=%d size=0x%" PRIx64 " generation=%" PRIu64
        " reason=pool_redirect",
        slot_index, size, generation);
}

void VortekGpuTrack_notePoolSlotFreed(int slot_index) {
    VortekGpuTrack_initOnce();
    VT_TRACK_LOG("SUBALLOC_POOL_SLOT_FREED slot=%d", slot_index);
}

void VortekGpuTrack_notePoolGrown(int slots, uint64_t bytes) {
    VortekGpuTrack_initOnce();
    g_sub_stats_pool_slots = slots;
    g_sub_stats_pool_bytes = bytes;
    g_sub_stats_pool_grows++;
    VT_TRACK_LOG("SUBALLOC_POOL_GROWN slots=%d bytes=%" PRIu64 " maxSlots=%d maxBytes=%" PRIu64,
                 slots, bytes, g_suballoc_pool_max_slots, g_suballoc_pool_max_bytes);
}

void VortekGpuTrack_noteQueueIdleFallback(void) {
    VortekGpuTrack_initOnce();
    g_sub_stats_queue_idle_fallbacks++;
}

void VortekGpuTrack_noteExactFenceWait(void) {
    VortekGpuTrack_initOnce();
    g_sub_stats_exact_waits++;
}

void VortekGpuTrack_maybeLogSuballocPoolStats(void) {
    VortekGpuTrack_initOnce();
    const uint64_t now = freeflight_now_ms();
    if (g_sub_stats_last_log_ms != 0 && now > g_sub_stats_last_log_ms &&
        (now - g_sub_stats_last_log_ms) < 5000) {
        return;
    }
    g_sub_stats_last_log_ms = now;
    int busy = 0;
    int reusable = 0;
    int used = 0;
    uint64_t max_lag = 0;
    for (int i = 0; i < kMaxSuballocLeases; ++i) {
        if (!g_leases[i].used) {
            continue;
        }
        used++;
        if (g_leases[i].state == kSubLeaseReusable) {
            reusable++;
        } else if (g_leases[i].state == kSubLeaseGpuInFlight ||
                   g_leases[i].state == kSubLeaseSubmitted ||
                   g_leases[i].state == kSubLeaseCpuOwned) {
            busy++;
            uint64_t gmax = g_leases[i].last_write_global > g_leases[i].last_read_global
                                ? g_leases[i].last_write_global
                                : g_leases[i].last_read_global;
            if (gmax > g_last_completed) {
                uint64_t lag = gmax - g_last_completed;
                if (lag > max_lag) {
                    max_lag = lag;
                }
            }
        }
    }
    VT_TRACK_LOG(
        "SUBALLOC_POOL_STATS slots=%d busy=%d reusable=%d usedLeases=%d bytes=%" PRIu64
        " grows=%" PRIu64 " busySkipped=%" PRIu64 " exactWaits=%" PRIu64
        " queueIdleFallbacks=%" PRIu64 " maxGpuLag=%" PRIu64 " rangeAcquired=%" PRIu64
        " cpuBlocked=%" PRIu64 " staleCompletion=%" PRIu64 " genMismatch=%" PRIu64
        " poolEnabled=%d waitEnabled=%d",
        g_sub_stats_pool_slots, busy, reusable, used, g_sub_stats_pool_bytes,
        g_sub_stats_pool_grows, g_sub_stats_busy_skipped, g_sub_stats_exact_waits,
        g_sub_stats_queue_idle_fallbacks, max_lag, g_sub_stats_range_acquired,
        g_sub_stats_cpu_blocked, g_sub_stats_stale_completion, g_sub_stats_gen_mismatch,
        g_suballoc_range_pool ? 1 : 0, g_wait_on_suballoc_overlap ? 1 : 0);
}

void VortekGpuTrack_notePresentSemaphoreDestroyed(void* semaphore) {
    if (!semaphore) {
        return;
    }
    for (int i = 0; i < g_present_sem_count; ++i) {
        if (g_present_sems[i].semaphore == semaphore) {
            if (g_present_sems[i].in_flight || g_present_sems[i].present_pending) {
                VT_TRACK_LOG(
                    "PRESENT_SEMAPHORE_DESTROYED_IN_FLIGHT semaphore=%p imageIndex=%u "
                    "presentId=%" PRIu64,
                    semaphore, g_present_sems[i].image_index, g_present_sems[i].present_id);
                VT_TRACK_LOG(
                    "PRESENT_SYNC_DESTROYED_IN_FLIGHT semaphore=%p imageIndex=%u "
                    "presentId=%" PRIu64,
                    semaphore, g_present_sems[i].image_index, g_present_sems[i].present_id);
            }
            g_present_sems[i].semaphore = NULL;
            g_present_sems[i].in_flight = false;
            g_present_sems[i].present_pending = false;
            return;
        }
    }
    VT_TRACK_LOG("UNTRACKED_SYNC_RECYCLE semaphore=%p", semaphore);
}

void VortekGpuTrack_onPresentSyncFailed(int vkResult) {
    VT_TRACK_LOG("present_sync_failed result=%d lastSubmitted=%" PRIu64
                 " lastCompleted=%" PRIu64 " presentId=%" PRIu64 " retireDepth=%d",
                 vkResult, g_last_submitted, g_last_completed, g_last_present_id,
                 g_retire_queue_depth);
    if (vkResult == -4 /* VK_ERROR_DEVICE_LOST */ || vkResult != 0) {
        VortekGpuTrack_onDeviceLost("present_sync_failed");
    }
}

void VortekGpuTrack_registerDeviceFaultQuery(void* device, void* pfnGetDeviceFaultInfoEXT) {
    VortekGpuTrack_initOnce();
    g_fault_device = device;
    g_get_device_fault = (VortekGetDeviceFaultInfoFn)pfnGetDeviceFaultInfoEXT;
    if (!g_device_fault_query_wanted) {
        g_get_device_fault = NULL;
        VT_TRACK_LOG("DEVICE_FAULT_QUERY enabled=0 device=%p", device);
        return;
    }
    VT_TRACK_LOG(
        "DEVICE_FAULT_QUERY enabled=1 device=%p pfn=%p pinFhd=%d detileStamp=%d "
        "gpuAddressBindingReport=%d",
        device, pfnGetDeviceFaultInfoEXT, g_pin_fhd_detile_sources ? 1 : 0,
        g_detile_stamp ? 1 : 0, g_gpu_address_binding_report ? 1 : 0);
    VT_TRACK_LOG("GPU_ADDRESS_BINDING_REPORT enabled=%d", g_gpu_address_binding_report ? 1 : 0);
}

void VortekGpuTrack_onExternalFdEvent(ResourceMemory* rm, int fd, int originalFd,
                                      const char* event) {
    VortekGpuTrack_initOnce();
    TrackedGpuAllocation* a = find_alloc_by_rm(rm);
    const char* ev = event && event[0] ? event : "unknown";
    if (!a) {
        VT_TRACK_LOG("EXTERNAL_FD_EVENT allocation=0 fd=%d originalFd=%d event=%s untracked=1",
                     fd, originalFd, ev);
        return;
    }
    if (strcmp(ev, "import") == 0 || strcmp(ev, "EXTERNAL_FD_IMPORT") == 0) {
        if (a->original_fd < 0) {
            a->original_fd = originalFd >= 0 ? originalFd : fd;
        }
        a->owned_fd = fd;
        a->fd_closed = false;
        VT_TRACK_LOG(
            "EXTERNAL_FD_IMPORT allocation=%" PRIu64 " backing=%" PRIu64
            " originalFd=%d ownedFd=%d size=0x%" PRIx64,
            a->id, a->backing_id, a->original_fd, a->owned_fd, a->allocation_size);
        return;
    }
    if (strcmp(ev, "dup") == 0 || strcmp(ev, "EXTERNAL_FD_DUP") == 0) {
        VT_TRACK_LOG(
            "EXTERNAL_FD_DUP allocation=%" PRIu64 " backing=%" PRIu64
            " originalFd=%d dupFd=%d",
            a->id, a->backing_id, originalFd >= 0 ? originalFd : a->original_fd, fd);
        return;
    }
    if (strcmp(ev, "close") == 0 || strcmp(ev, "EXTERNAL_FD_CLOSE") == 0) {
        if (a->gpu_address_bound || a->fhd_pinned ||
            a->last_gpu_read_submission > g_last_completed) {
            VT_TRACK_LOG(
                "EXTERNAL_FD_CLOSED_WHILE_BOUND allocation=%" PRIu64 " backing=%" PRIu64
                " fd=%d lastGpuRead=%" PRIu64 " completed=%" PRIu64 " gpuBound=%d fhdPinned=%d",
                a->id, a->backing_id, fd, a->last_gpu_read_submission, g_last_completed,
                a->gpu_address_bound ? 1 : 0, a->fhd_pinned ? 1 : 0);
            if (a->fhd_pinned || (g_pin_fhd_detile_sources && is_fhd_class_size(a->allocation_size))) {
                VT_TRACK_LOG(
                    "FHD_SOURCE_PINNED_RETENTION allocation=%" PRIu64
                    " action=block_fd_close path=onExternalFdEvent",
                    a->id);
                return;
            }
        }
        a->fd_closed = true;
        a->owned_fd = -1;
        VT_TRACK_LOG(
            "EXTERNAL_FD_CLOSE allocation=%" PRIu64 " backing=%" PRIu64 " fd=%d",
            a->id, a->backing_id, fd);
        return;
    }
    VT_TRACK_LOG(
        "EXTERNAL_FD_EVENT allocation=%" PRIu64 " backing=%" PRIu64 " fd=%d originalFd=%d "
        "event=%s",
        a->id, a->backing_id, fd, originalFd, ev);
}

void VortekGpuTrack_noteAddressBinding(uint64_t baseAddress, uint64_t size, int bindingType,
                                       uint32_t flags) {
    VortekGpuTrack_initOnce();
    if (!g_gpu_address_binding_report) {
        return;
    }
    /* bindingType 0=BIND 1=UNBIND */
    TrackedGpuAllocation* best = NULL;
    TrackedResource* best_r = NULL;
    /* Prefer exact size match on live FHD external allocs; else any overlapping bound. */
    for (int i = 0; i < g_alloc_count; ++i) {
        TrackedGpuAllocation* a = &g_allocs[i];
        if (!a->alive && !a->destroy_requested) {
            continue;
        }
        if (size > 0 && a->allocation_size == size) {
            best = a;
            break;
        }
        if (a->gpu_address_bound && a->gpu_bind_address != 0 && size > 0) {
            const uint64_t a0 = a->gpu_bind_address;
            const uint64_t a1 = a0 + (a->gpu_bind_size ? a->gpu_bind_size : a->allocation_size);
            if (baseAddress >= a0 && baseAddress < a1) {
                best = a;
            }
        }
    }
    if (best) {
        for (int i = 0; i < g_resource_count; ++i) {
            if (g_resources[i].allocation_id == best->id && g_resources[i].alive &&
                g_resources[i].kind == TRACK_RES_BUFFER) {
                if (!best_r || is_fhd_class_size(g_resources[i].buffer_size)) {
                    best_r = &g_resources[i];
                }
            }
        }
    }

    GpuAddrBindEntry* slot = NULL;
    for (int i = 0; i < kMaxGpuAddrBinds; ++i) {
        if (g_gpu_addr_binds[i].used && g_gpu_addr_binds[i].base == baseAddress) {
            slot = &g_gpu_addr_binds[i];
            break;
        }
    }
    if (!slot) {
        slot = &g_gpu_addr_binds[g_gpu_addr_bind_write % kMaxGpuAddrBinds];
        g_gpu_addr_bind_write++;
        if (g_gpu_addr_bind_count < kMaxGpuAddrBinds) {
            g_gpu_addr_bind_count++;
        }
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
    }
    slot->base = baseAddress;
    slot->size = size;
    slot->flags = flags;
    slot->allocation_id = best ? best->id : 0;
    slot->resource_id = best_r ? best_r->id : 0;
    slot->generation = best ? best->bind_generation : 0;
    slot->last_gpu_read = best ? best->last_gpu_read_submission : 0;

    if (bindingType == 0) {
        slot->bound = true;
        if (best) {
            best->gpu_address_bound = true;
            best->gpu_bind_address = baseAddress;
            best->gpu_bind_size = size ? size : best->allocation_size;
            best->bind_gen_at_gpu_bind = best->bind_generation;
        }
        VT_TRACK_LOG(
            "GPU_ADDRESS_BIND allocation=%" PRIu64 " resource=%" PRIu64
            " baseAddress=0x%" PRIx64 " size=0x%" PRIx64 " bindingType=BIND flags=0x%x "
            "generation=%" PRIu64,
            best ? best->id : 0, best_r ? best_r->id : 0, baseAddress, size, flags,
            best ? best->bind_generation : 0);
    } else {
        slot->bound = false;
        slot->unbound_at_completed = g_last_completed;
        const bool inflight =
            best && (best->last_gpu_read_submission > g_last_completed ||
                     best->pending_submission_refs > 0 ||
                     best->last_submitted_use > g_last_completed);
        if (inflight) {
            VT_TRACK_LOG(
                "GPU_ADDRESS_UNBIND_IN_FLIGHT allocation=%" PRIu64 " resource=%" PRIu64
                " baseAddress=0x%" PRIx64 " size=0x%" PRIx64 " lastGpuRead=%" PRIu64
                " completed=%" PRIu64 " pendingRefs=%u generation=%" PRIu64,
                best ? best->id : 0, best_r ? best_r->id : 0, baseAddress, size,
                best ? best->last_gpu_read_submission : 0, g_last_completed,
                best ? best->pending_submission_refs : 0,
                best ? best->bind_generation : 0);
        }
        if (best) {
            if (best->fhd_pinned ||
                (g_pin_fhd_detile_sources && is_fhd_class_size(best->allocation_size))) {
                best->fhd_pinned = true;
                VT_TRACK_LOG(
                    "FHD_SOURCE_PINNED_RETENTION allocation=%" PRIu64
                    " action=note_unbind_while_pinned path=noteAddressBinding "
                    "baseAddress=0x%" PRIx64,
                    best->id, baseAddress);
            }
            best->gpu_address_bound = false;
        }
        VT_TRACK_LOG(
            "GPU_ADDRESS_UNBIND allocation=%" PRIu64 " baseAddress=0x%" PRIx64
            " size=0x%" PRIx64 " lastGpuRead=%" PRIu64 " completed=%" PRIu64
            " generation=%" PRIu64 " flags=0x%x",
            best ? best->id : 0, baseAddress, size,
            best ? best->last_gpu_read_submission : 0, g_last_completed,
            best ? best->bind_generation : 0, flags);
    }
}

static void dump_device_fault_info(const char* where) {
    if (!g_fault_device || !g_get_device_fault) {
        VT_TRACK_LOG(
            "DEVICE_FAULT_INFO unavailable where=%s device=%p pfn=%p",
            where ? where : "?", g_fault_device, (void*)g_get_device_fault);
        return;
    }
    PFN_vkGetDeviceFaultInfoEXT getFault =
        (PFN_vkGetDeviceFaultInfoEXT)(void*)g_get_device_fault;
    VkDeviceFaultCountsEXT counts;
    memset(&counts, 0, sizeof(counts));
    counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
    VkResult cr = getFault((VkDevice)g_fault_device, &counts, NULL);
    VT_TRACK_LOG(
        "DEVICE_FAULT_INFO where=%s countResult=%d addressInfoCount=%u vendorInfoCount=%u "
        "vendorBinarySize=%u",
        where ? where : "?", (int)cr, counts.addressInfoCount, counts.vendorInfoCount,
        counts.vendorBinarySize);
    if (cr != 0 && cr != 5 /* VK_INCOMPLETE */) {
        return;
    }
    VkDeviceFaultAddressInfoEXT addrs[16];
    VkDeviceFaultVendorInfoEXT vendors[8];
    memset(addrs, 0, sizeof(addrs));
    memset(vendors, 0, sizeof(vendors));
    VkDeviceFaultInfoEXT info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    if (counts.addressInfoCount > 16) {
        counts.addressInfoCount = 16;
    }
    if (counts.vendorInfoCount > 8) {
        counts.vendorInfoCount = 8;
    }
    info.pAddressInfos = counts.addressInfoCount ? addrs : NULL;
    info.pVendorInfos = counts.vendorInfoCount ? vendors : NULL;
    VkResult ir = getFault((VkDevice)g_fault_device, &counts, &info);
    VT_TRACK_LOG(
        "DEVICE_FAULT_INFO description=%s result=%d addressType0=%d reportedAddress=0x%" PRIx64
        " addressPrecision=0x%" PRIx64 " vendorBinarySize=%u",
        info.description[0] ? info.description : "(none)", (int)ir,
        counts.addressInfoCount ? (int)addrs[0].addressType : -1,
        counts.addressInfoCount ? (uint64_t)addrs[0].reportedAddress : 0,
        counts.addressInfoCount ? (uint64_t)addrs[0].addressPrecision : 0,
        (uint32_t)counts.vendorBinarySize);
    for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
        const uint64_t rep = (uint64_t)addrs[i].reportedAddress;
        const uint64_t prec =
            addrs[i].addressPrecision ? (uint64_t)addrs[i].addressPrecision : 1ull;
        TrackedGpuAllocation* owner = NULL;
        GpuAddrBindEntry* be = NULL;
        for (int b = 0; b < kMaxGpuAddrBinds; ++b) {
            if (!g_gpu_addr_binds[b].used || g_gpu_addr_binds[b].base == 0) {
                continue;
            }
            const uint64_t b0 = g_gpu_addr_binds[b].base & ~(prec - 1);
            const uint64_t b1 = g_gpu_addr_binds[b].base + g_gpu_addr_binds[b].size;
            const uint64_t r0 = rep & ~(prec - 1);
            if (r0 >= b0 && r0 < b1) {
                be = &g_gpu_addr_binds[b];
                break;
            }
        }
        if (be && be->allocation_id) {
            owner = find_alloc_by_id(be->allocation_id);
        }
        if (!owner) {
            for (int ai = 0; ai < g_alloc_count; ++ai) {
                TrackedGpuAllocation* a = &g_allocs[ai];
                if (a->gpu_bind_address == 0) {
                    continue;
                }
                const uint64_t a0 = a->gpu_bind_address;
                const uint64_t a1 = a0 + (a->gpu_bind_size ? a->gpu_bind_size : a->allocation_size);
                if (rep >= a0 && rep < a1) {
                    owner = a;
                    break;
                }
            }
        }
        VT_TRACK_LOG(
            "DEVICE_FAULT_OWNER allocation=%" PRIu64 " resource=%" PRIu64
            " bindingGeneration=%" PRIu64 " state=%s lastUse=%" PRIu64 " completed=%" PRIu64
            " reportedAddress=0x%" PRIx64 " addressType=%d precision=0x%" PRIx64,
            owner ? owner->id : 0, be ? be->resource_id : 0,
            owner ? owner->bind_generation : (be ? be->generation : 0),
            owner ? (owner->gpu_address_bound ? "bound" : (owner->physically_freed ? "unbound" : "replaced"))
                  : (be ? (be->bound ? "bound" : "unbound") : "unknown"),
            owner ? owner->last_submitted_use : 0, g_last_completed, rep, (int)addrs[i].addressType,
            prec);
    }
    for (int b = 0; b < kMaxGpuAddrBinds; ++b) {
        if (!g_gpu_addr_binds[b].used) {
            continue;
        }
        VT_TRACK_LOG(
            "DEVICE_LOST_GPU_ADDR_BIND base=0x%" PRIx64 " size=0x%" PRIx64
            " allocation=%" PRIu64 " resource=%" PRIu64 " generation=%" PRIu64
            " bound=%d lastGpuRead=%" PRIu64 " flags=0x%x",
            g_gpu_addr_binds[b].base, g_gpu_addr_binds[b].size,
            g_gpu_addr_binds[b].allocation_id, g_gpu_addr_binds[b].resource_id,
            g_gpu_addr_binds[b].generation, g_gpu_addr_binds[b].bound ? 1 : 0,
            g_gpu_addr_binds[b].last_gpu_read, g_gpu_addr_binds[b].flags);
    }
}

void VortekGpuTrack_onDeviceLost(const char* where) {
    VortekGpuTrack_initOnce();
    if (g_device_lost_dumped) {
        VT_TRACK_LOG("DEVICE_LOST_SNAPSHOT already_dumped where=%s", where ? where : "?");
        return;
    }
    g_device_lost_dumped = true;
    dump_device_fault_info(where);

    VT_TRACK_LOG("DEVICE_LOST_SNAPSHOT where=%s lastSubmitted=%" PRIu64
                 " lastCompleted=%" PRIu64 " currentPending=%" PRIu64
                 " liveAllocs=%d liveResources=%d opRing=%d deferDestroy=%d "
                 "retireDepth=%d retireMax=%d retireReq=%" PRIu64 " retireCommit=%" PRIu64
                 " swapchainImageCount=%u lastAcquiredImage=%u physRelRing=%d "
                 "acquireRing=%d",
                 where ? where : "?", g_last_submitted, g_last_completed,
                 g_current_pending_submission, g_alloc_count, g_resource_count, g_op_count,
                 g_defer_destroy ? 1 : 0, g_retire_queue_depth, g_retire_queue_max,
                 g_retire_request_count, g_retire_commit_count,
                 g_presenter_swapchain_image_count, g_last_acquired_image_index,
                 g_phys_rel_count, g_acquire_count);
    VT_TRACK_LOG(
        "PRESENTER_CONFIG snapshot swapchainImageCount=%u internalImageCount=%u "
        "frameSlotCount=%u presentQueueFamily=%u graphicsQueueFamily=%u",
        g_presenter_swapchain_image_count, g_presenter_internal_image_count,
        g_presenter_frame_slot_count, g_presenter_present_qfamily,
        g_presenter_graphics_qfamily);
    for (uint32_t ii = 0; ii < kMaxPresentImages; ++ii) {
        PresentImageState* s = &g_present_images[ii];
        if (!s->used) {
            continue;
        }
        VT_TRACK_LOG(
            "PRESENT_IMAGE_STATE imageIndex=%u life=%d presentPending=%d acquired=%d "
            "presentId=%" PRIu64 " lastAcquireId=%" PRIu64 " renderFinished=%p "
            "ahb=%p image=%p memory=%p",
            s->image_index, (int)s->life, s->present_pending ? 1 : 0, s->acquired ? 1 : 0,
            s->present_id, s->last_acquire_id, s->render_finished, s->ahb, s->image,
            s->memory);
    }
    if (g_shared_ahb) {
        VT_TRACK_LOG("DEVICE_LOST_SHARED_AHB ahb=%p imageCount=%d", g_shared_ahb,
                     g_shared_ahb_image_count);
    }
    VT_TRACK_LOG(
        "DEVICE_LOST_QUARANTINE enabled=%d buffers=%d images=%d memory=%d firstFrame=%d "
        "blockCount=%" PRIu64 " freeflightCount=%d retainUnknown=%d unknownRetain=%" PRIu64
        " unknownBytes=%" PRIu64 " bufferRetireCommit=%" PRIu64 " bufferRetireBlock=%" PRIu64
        " directBypass=%" PRIu64,
        g_quarantine_gpu_releases ? 1 : 0, g_quarantine_buffers ? 1 : 0,
        g_quarantine_images ? 1 : 0, g_quarantine_memory ? 1 : 0, g_first_frame_seen ? 1 : 0,
        g_quarantine_block_count, g_freeflight_count, g_retain_unknown_buffers ? 1 : 0,
        g_unknown_buffer_retain_count, g_unknown_buffer_retain_bytes,
        g_buffer_retire_commit_count, g_buffer_retire_block_count,
        g_direct_destroy_bypass_count);
    {
        const int start = g_freeflight_count < kFreeflightRing ? 0 : g_freeflight_write;
        for (int n = 0; n < g_freeflight_count; ++n) {
            const int idx = (start + n) % kFreeflightRing;
            const FreeflightEvent* e = &g_freeflight[idx];
            VT_TRACK_LOG(
                "FREEFLIGHT_EVENT seq=%" PRIu64 " tsMs=%" PRIu64 " tag=%s queue=%p "
                "submission=%" PRIu64 " resource=%p allocation=0x%" PRIx64 " backing=0x%" PRIx64
                " mapping=0x%" PRIx64 " lastUse=%" PRIu64 " completed=%" PRIu64
                " pendingRefs=%u logicalDestroy=%d physicalDestroy=%d",
                e->seq, e->ts_ms, e->tag, e->queue, e->submission, e->resource, e->allocation,
                e->backing, e->mapping, e->last_use, e->completed, e->pending_refs,
                e->logical_destroy, e->physical_destroy);
        }
    }
    /* Last physical release requests. */
    {
        const int start = g_phys_rel_count < kPhysRelRing ? 0
                                                          : g_phys_rel_write;
        for (int n = 0; n < g_phys_rel_count; ++n) {
            const int idx = (start + n) % kPhysRelRing;
            const PhysRelEvent* e = &g_phys_rel_ring[idx];
            VT_TRACK_LOG(
                "DEVICE_LOST_PHYS_REL seq=%" PRIu64 " reason=%s id=%" PRIu64 " handle=%p "
                "allowed=%d tag=%s",
                e->seq, release_reason_str(e->kind), e->id, e->handle, e->allowed, e->tag);
        }
    }
    {
        const int start = g_acquire_count < kAcquireRing ? 0 : g_acquire_write;
        for (int n = 0; n < g_acquire_count; ++n) {
            const int idx = (start + n) % kAcquireRing;
            const AcquireEvent* e = &g_acquire_ring[idx];
            VT_TRACK_LOG(
                "DEVICE_LOST_ACQUIRE callId=%" PRIu64 " result=%d imageIndex=%u "
                "prevPresentId=%" PRIu64 " semaphore=%p",
                e->call_id, e->result, e->image_index, e->prev_present_id,
                e->acquire_semaphore);
        }
    }

    for (int qi = 0; qi < g_queue_count; ++qi) {
        if (!g_queues[qi].used) {
            continue;
        }
        VT_TRACK_LOG(
            "DEVICE_LOST_QUEUE index=%d queue=%p lastSubmittedSerial=%" PRIu64
            " completedSerial=%" PRIu64 " nextSerial=%" PRIu64,
            qi, g_queues[qi].queue, g_queues[qi].last_submitted_serial,
            g_queues[qi].completed_serial, g_queues[qi].next_serial);
    }

    for (int i = 0; i < g_alloc_count; ++i) {
        TrackedGpuAllocation* a = &g_allocs[i];
        if (!a->alive && !a->destroy_requested) {
            continue;
        }
        recount_alloc_children(a);
        VT_TRACK_LOG(
            "DEVICE_LOST_ALLOC id=%" PRIu64 " alive=%d mapped=%d destroyRequested=%d "
            "memory=%p vkMemory=%p allocSize=0x%" PRIx64 " memoryType=%u backingType=%s "
            "lastSubmittedUse=%" PRIu64 " lastCompletedUse=%" PRIu64 " pendingRefs=%u "
            "lastGpuRead=%" PRIu64 " lastGpuWrite=%" PRIu64 " liveChildren=%u "
            "pendingChildren=%u external=%d externalBackingId=%" PRIu64 " mappingId=%" PRIu64
            " gpuVaMappingId=%" PRIu64 " physicallyFreed=%d",
            a->id, a->alive ? 1 : 0, a->mapped ? 1 : 0, a->destroy_requested ? 1 : 0,
            (void*)a->rm, a->memory_handle, a->allocation_size, a->memory_type_index,
            a->backing_type, a->last_submitted_use, a->last_completed_use,
            a->pending_submission_refs, a->last_gpu_read_submission,
            a->last_gpu_write_submission, a->live_child_objects, a->pending_child_objects,
            a->external_backing ? 1 : 0, a->backing_id, a->mapping_id, a->gpu_va_mapping_id,
            a->physically_freed ? 1 : 0);
        /* Child list for this allocation. */
        char children[192];
        int cn = 0;
        children[0] = '\0';
        for (int ri = 0; ri < g_resource_count && cn < 160; ++ri) {
            TrackedResource* cr = &g_resources[ri];
            if (cr->allocation_id != a->id || cr->actually_destroyed) {
                continue;
            }
            cn += snprintf(children + cn, sizeof(children) - (size_t)cn, "%s%" PRIu64 "%s",
                           cn ? "," : "", cr->id, cr->destroy_requested ? "*" : "");
        }
        int qi = -1;
        uint64_t use = min_incomplete_serial(a->last_use, &qi);
        uint64_t completed =
            (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_serial : g_last_completed;
        VT_TRACK_LOG(
            "ALLOCATION_STATE allocation=%" PRIu64 " children=%s pendingChildren=%u "
            "liveChildren=%u lastUseByQueue=%" PRIu64 " completionByQueue=%" PRIu64
            " freeRequested=%d physicallyFreed=%d externalBacking=%" PRIu64 " mapping=%" PRIu64
            " gpuVaMapping=%" PRIu64,
            a->id, children[0] ? children : "-", a->pending_child_objects,
            a->live_child_objects, use ? use : a->last_submitted_use, completed,
            a->free_requested ? 1 : 0, a->physically_freed ? 1 : 0, a->backing_id,
            a->mapping_id, a->gpu_va_mapping_id);
    }

    for (int i = 0; i < g_resource_count; ++i) {
        TrackedResource* r = &g_resources[i];
        if (!r->alive && !r->destroy_requested) {
            continue;
        }
        if (r->pending_submission_refs > 0 || r->destroy_requested ||
            !resource_can_retire(r)) {
            int qi = -1;
            uint64_t use = min_incomplete_serial(r->last_use, &qi);
            uint64_t completed =
                (qi >= 0 && qi < kMaxQueues) ? g_queues[qi].completed_serial
                                             : g_last_completed;
            VT_TRACK_LOG(
                "PENDING_RESOURCE resource=%" PRIu64 " type=%s destroyRequested=%d "
                "lastUse=%" PRIu64 " completed=%" PRIu64 " pendingRefs=%u "
                "allocationId=%" PRIu64 " externalBackingId=%" PRIu64 " mappingId=%" PRIu64
                " gpuVaMappingId=%" PRIu64 " bindOffset=0x%" PRIx64
                " handle=%p alive=%d queue=%d",
                r->id, kind_str(r->kind), r->destroy_requested ? 1 : 0,
                r->last_submitted_use, completed, r->pending_submission_refs,
                r->allocation_id, r->external_backing_id, r->mapping_id, r->gpu_va_mapping_id,
                r->bind_offset, r->handle, r->alive ? 1 : 0, qi);
            (void)use;
        }
        VT_TRACK_LOG(
            "DEVICE_LOST_RESOURCE id=%" PRIu64 " kind=%s handle=%p allocation=%" PRIu64
            " bindOffset=0x%" PRIx64 " reqSize=0x%" PRIx64 " bufferSize=0x%" PRIx64
            " width=%u height=%u lastSubmittedUse=%" PRIu64 " lastCompletedUse=%" PRIu64
            " pendingRefs=%u destroyRequested=%d layout=0x%x externalBacking=%" PRIu64,
            r->id, kind_str(r->kind), r->handle, r->allocation_id, r->bind_offset,
            r->requirements_size, r->buffer_size, r->width, r->height, r->last_submitted_use,
            r->last_completed_use, r->pending_submission_refs, r->destroy_requested ? 1 : 0,
            r->current_layout, r->external_backing_id);
    }

    const int start = g_op_count < kOpRing ? 0 : g_op_write;
    int submitted_n = 0;
    int completed_n = 0;
    for (int n = 0; n < g_op_count; ++n) {
        const int idx = (start + n) % kOpRing;
        const GpuOp* o = &g_ops[idx];
        VT_TRACK_LOG(
            "DEVICE_LOST_OP idx=%d submission=%" PRIu64 " operation=%s srcResource=%" PRIu64
            " srcOffset=0x%" PRIx64 " srcSize=0x%" PRIx64 " dstResource=%" PRIu64
            " dstOffset=0x%" PRIx64 " dstSize=0x%" PRIx64 " width=%u height=%u "
            "tilingMode=%d rangeInvalid=%d completed=%d",
            n, o->submission, o->operation, o->src_resource, o->src_offset, o->src_size,
            o->dst_resource, o->dst_offset, o->dst_size, o->width, o->height, o->tiling_mode,
            o->range_invalid ? 1 : 0, o->completed ? 1 : 0);
        if (o->completed) {
            completed_n++;
        } else {
            submitted_n++;
        }
    }
    VT_TRACK_LOG("DEVICE_LOST_OP_SUMMARY submittedRing=%d completedRing=%d", submitted_n,
                 completed_n);
}
