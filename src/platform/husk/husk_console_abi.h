/*
 * husk_console_abi.h -- the contract between Husk and a console module (libhuskps4.dylib).
 * Kept byte-identical in Husk (src/translation-layer-next/husk-console-abi.h) and in the module's
 * tree (Bachata-S4 src/platform/husk/husk_console_abi.h). Plain C, versioned: a table only grows
 * at its end, its struct_size says how much of it the other side filled, and a change to an
 * existing field's meaning bumps the version.
 *
 * Husk loads the module, fills a HuskHostV1 and calls husk_console_module_v1() for the module's
 * table. One game per process.
 */
#ifndef HUSK_CONSOLE_ABI_H
#define HUSK_CONSOLE_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUSK_CONSOLE_ABI_VERSION 1

/* What Husk gives the module. */
typedef struct HuskHostV1 {
    uint32_t struct_size; /* sizeof(HuskHostV1) as Husk built it */
    uint32_t version;     /* HUSK_CONSOLE_ABI_VERSION */

    /* Executable memory from Husk's one JIT region. jit_alloc returns the writable address and
     * stores the executable one in *rx_out (NULL when refused: the module's declared ceiling is
     * reached or the region is full). For every allocation rx = rw + jit_delta, and the region is
     * [jit_rw_base, jit_rw_base + jit_size) on the writable side, so the module translates with
     * one addition and a range check, safely inside a signal handler. Nothing ever traps to a
     * debugger. */
    void *(*jit_alloc)(size_t bytes, void **rx_out);
    int (*jit_free)(void *rw, size_t bytes); /* nonzero when freed */
    intptr_t jit_delta;
    uintptr_t jit_rw_base;
    size_t jit_size;

    /* Memory the process may still use before the system ends it (os_proc_available_memory on
     * iOS), and what it uses now (phys_footprint). */
    uint64_t (*available_memory)(void);
    uint64_t (*footprint)(void);

    /* One line of the module's log, without a newline. Husk writes it to the game's
     * native-run.log. */
    void (*log_line)(const char *line);

    /* Full path of the Vulkan implementation to load directly (MoltenVK in Husk's Frameworks), or
     * NULL for the platform default. */
    const char *vulkan_library_path;

    /* Asks the player for text: a game's keyboard dialog (a character or world name). Husk shows
     * its keyboard and later calls done(ctx, text) once, on any thread, with the UTF-8 text, or
     * with NULL when the player cancelled. title and initial are UTF-8 and may be empty; max_chars
     * is the field's limit in characters. Returns 0 when Husk took the request, nonzero when it
     * cannot ask now (the module then answers for the player). Absent from hosts built before it:
     * struct_size says. */
    int (*request_text)(const char *title, const char *initial, uint32_t max_chars,
                        void (*done)(void *ctx, const char *text), void *ctx);
} HuskHostV1;

/* Controller state Husk merges from its pads and the on-screen pad. Buttons are the PS4 pad's
 * bits (OrbisPadButtonDataOffset); sticks 0..255 with 128 centred; triggers 0..255. */
typedef struct HuskPadV1 {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2;
    uint8_t touch_down;
    uint8_t reserved;
    float touch_x, touch_y; /* 0..1 */
} HuskPadV1;

enum HuskConsoleState {
    HUSK_CONSOLE_IDLE = 0,
    HUSK_CONSOLE_BOOTING = 1, /* launched, no frame yet */
    HUSK_CONSOLE_RUNNING = 2, /* at least one frame presented */
    HUSK_CONSOLE_PAUSED = 3,
    HUSK_CONSOLE_STOPPED = 4, /* the game ended or stop() finished */
    HUSK_CONSOLE_FAILED = 5,  /* see the log */
};

/* What the module gives Husk. */
typedef struct HuskConsoleModuleV1 {
    uint32_t struct_size;
    uint32_t version;
    const char *platform; /* "ps4" */
    /* Most executable memory the module will hold at once; Husk refuses to start it when its
     * region is smaller. */
    size_t jit_ceiling_bytes;

    /* Once per process, before launch. user_dir holds the module's settings, saves, system
     * modules and caches. Returns 0 on success. */
    int (*init)(const HuskHostV1 *host, const char *user_dir);
    /* Starts the game at game (an eboot.bin path or a folder holding one) on a thread of its own
     * and returns. args: extra launch arguments joined by '\n' (may be NULL). metal_layer: the
     * CAMetalLayer to render into, width_px x height_px drawable pixels at scale pixels per point.
     * Returns 0 when the launch began. */
    int (*launch)(const char *game, const char *args, void *metal_layer, int width_px,
                  int height_px, float scale);
    int (*state)(void);       /* enum HuskConsoleState */
    uint64_t (*frames)(void); /* frames presented so far */
    void (*set_paused)(int paused);
    void (*set_pad)(const HuskPadV1 *pad);
    void (*resize)(int width_px, int height_px);
    /* Asks the game to end; state() becomes STOPPED when it has. */
    void (*stop)(void);
    /* After STOPPED: when the game asked to start another executable (sceSystemServiceLoadExec),
     * its path and '\n'-joined args are copied out and 1 is returned; else 0. */
    int (*take_pending_restart)(char *path, int path_size, char *args, int args_size);

    /* Extracts a package file (.pkg) into dest_dir, which it creates: eboot.bin and sce_sys/ for
     * a game, the changed files for an update. Needs no init and no running game, and runs on the
     * caller's thread. Only packages that open without a console's own keys install (fake
     * packages and homebrew); a retail package is refused. progress(ctx, done, total) may be
     * NULL. Returns 0 when extracted; else copies a message to err and returns nonzero.
     * Absent from modules built before it: struct_size says. */
    int (*install_package)(const char *package, const char *dest_dir,
                           void (*progress)(void *ctx, uint64_t done, uint64_t total), void *ctx,
                           char *err, int err_size);
} HuskConsoleModuleV1;

/* The module's one exported symbol. Returns NULL when it cannot run in this process. */
const HuskConsoleModuleV1 *husk_console_module_v1(void);
typedef const HuskConsoleModuleV1 *(*husk_console_module_v1_fn)(void);

#ifdef __cplusplus
}
#endif

#endif
