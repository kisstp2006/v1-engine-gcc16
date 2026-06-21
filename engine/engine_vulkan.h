#ifndef ENGINE_VULKAN_H
#define ENGINE_VULKAN_H

/*
 * engine_vulkan.h — Nebula Vulkan backend for FWK.
 *
 * Adds Vulkan rendering alongside OpenGL via a shared dispatch vtable.
 * The existing engine.h / engine.c OpenGL path is NEVER modified.
 *
 * New files only:  engine_vulkan.h  engine_vulkan.c
 * Minimal engine.h additions: engine_backend_t enum, window_create_ex()
 *
 * Phase 5 scope: instance, device, swapchain, clear-to-colour, present.
 * Full model rendering through Vulkan comes in Phase 6+.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Backend selection ───────────────────────────────────────────────────── */

typedef enum {
    ENGINE_BACKEND_GL     = 0,
    ENGINE_BACKEND_VULKAN = 1,
} engine_backend_t;

/* ── Render dispatch vtable (shared by GL and Vulkan paths) ─────────────── */
/*
 * Each backend fills in this table during init.
 * High-level code calls through the vtable — no #ifdef, no duplicate paths.
 */
typedef struct fwk_render_api {
    const char *name;                           /* "OpenGL" or "Vulkan" */
    bool (*init)(void *glfw_window, int w, int h);
    void (*shutdown)(void);
    void (*begin_frame)(void);
    void (*end_frame)(void);
    void (*clear)(float r, float g, float b, float a);
    /* More entries added as Vulkan support expands in Phase 6+ */
} fwk_render_api;

/* Active backend vtable — set by fwk_window_create_standalone_ex() */
extern fwk_render_api *g_render_api;

/* Built-in backend vtables (defined in engine_vulkan.c and engine.c) */
extern fwk_render_api fwk_vulkan_render_api;

/* ── Vulkan backend lifecycle ────────────────────────────────────────────── */

/*
 * Initialise the Vulkan backend from an existing GLFW window that was
 * created with GLFW_CLIENT_API = GLFW_NO_API.
 * Returns true on success, false if Vulkan is unavailable.
 */
bool engine_vulkan_init(void *glfw_window, int width, int height);
void engine_vulkan_shutdown(void);
void engine_vulkan_begin_frame(void);
void engine_vulkan_end_frame(void);
void engine_vulkan_clear(float r, float g, float b, float a);

/*
 * Create a GLFW window with GLFW_NO_API (no GL context) and initialise Vulkan.
 * Called by fwk_bind.c for the Vulkan path of fwk_window_create_standalone_ex().
 * Does NOT call FWK's window_create() — completely independent from the GL path.
 */
bool engine_vulkan_init_window(const char *title, int width, int height);

/*
 * Per-frame event pump: polls GLFW events, returns false if window close requested.
 * Used by fwk_window_swap() for the Vulkan path.
 */
bool engine_vulkan_poll_events(void);

/* Width/height query (so downstream code can set viewports etc.) */
int engine_vulkan_width(void);
int engine_vulkan_height(void);

#endif /* ENGINE_VULKAN_H */
