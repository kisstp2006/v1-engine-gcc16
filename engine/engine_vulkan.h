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

#ifndef ENGINE_BACKEND_T_DEFINED
#define ENGINE_BACKEND_T_DEFINED
typedef enum engine_backend_t {
    ENGINE_BACKEND_GL     = 0,
    ENGINE_BACKEND_VULKAN = 1,
} engine_backend_t;
#endif

/* ── Backend dispatch vtable (shared by GL and Vulkan paths) ────────────── */
/*
 * Clean backend lifecycle:
 * init -> begin_frame -> clear -> end_frame -> resize -> shutdown.
 */
#ifndef FWK_BACKEND_API_DEFINED
#define FWK_BACKEND_API_DEFINED
typedef uint64_t fwk_backend_texture_t;

typedef struct fwk_backend_vertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
} fwk_backend_vertex;

typedef struct fwk_backend_api {
    const char *name;                           /* "OpenGL" or "Vulkan" */
    bool (*init)(void *glfw_window, int w, int h);
    bool (*begin_frame)(void);
    void (*clear)(float r, float g, float b, float a);
    bool (*end_frame)(void);
    bool (*resize)(int w, int h);
    fwk_backend_texture_t (*create_texture)(unsigned w, unsigned h, unsigned n, const void *pixels, int flags);
    bool (*update_texture)(fwk_backend_texture_t texture, unsigned w, unsigned h, unsigned n, const void *pixels, int flags);
    void (*destroy_texture)(fwk_backend_texture_t texture);
    void (*set_viewport)(int x, int y, int w, int h);
    void (*set_blend)(bool enabled);
    void (*set_depth)(bool test_enabled, bool write_enabled);
    void (*draw_line)(const fwk_backend_vertex *a, const fwk_backend_vertex *b);
    void (*draw_triangle)(const fwk_backend_vertex vertices[3]);
    void (*draw_quad)(const fwk_backend_vertex vertices[4]);
    void (*draw_textured_quad)(fwk_backend_texture_t texture, const fwk_backend_vertex vertices[4]);
    void (*shutdown)(void);
} fwk_backend_api;

typedef fwk_backend_api fwk_render_api;
#endif

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
bool engine_vulkan_begin_frame(void);
bool engine_vulkan_end_frame(void);
void engine_vulkan_clear(float r, float g, float b, float a);
bool engine_vulkan_resize(int width, int height);

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
