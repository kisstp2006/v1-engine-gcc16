// -----------------------------------------------------------------------------
// window framework
// - rlyeh, public domain
//
// @todo: window_cursor(ico);
// @todo: if WINDOW_PORTRAIT && exist portrait monitor, use that instead of primary one
// @todo: WINDOW_TRAY

enum WINDOW_FLAGS {
    WINDOW_MSAA2 = 0x02,
    WINDOW_MSAA4 = 0x04,
    WINDOW_MSAA8 = 0x08,

    WINDOW_SQUARE = 0x20,
    WINDOW_PORTRAIT = 0x40,
    WINDOW_LANDSCAPE = 0x80,
    WINDOW_ASPECT = 0x100, // keep aspect
    WINDOW_FIXED = 0x200, // disable resizing
    WINDOW_TRANSPARENT = 0x400,
    WINDOW_BORDERLESS = 0x800,
    WINDOW_TRUE_BORDERLESS = 0x4000,

    WINDOW_VSYNC = 0x2000,
    WINDOW_VSYNC_ADAPTIVE = 0x1000,
};

#ifndef ENGINE_BACKEND_T_DEFINED
#define ENGINE_BACKEND_T_DEFINED
typedef enum engine_backend_t {
    ENGINE_BACKEND_GL     = 0,
    ENGINE_BACKEND_VULKAN = 1,
} engine_backend_t;
#endif

#ifndef FWK_BACKEND_API_DEFINED
#define FWK_BACKEND_API_DEFINED
typedef uint64_t fwk_backend_texture_t;

typedef struct fwk_backend_vertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
} fwk_backend_vertex;

typedef struct fwk_backend_api {
    const char *name;
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

#if ENABLE_VULKAN
extern fwk_render_api *g_render_api;
extern fwk_render_api fwk_vulkan_render_api;
#endif

API bool     window_create(float scale, unsigned flags);
API bool     window_create_ex(float scale, unsigned flags, engine_backend_t backend);
API bool     window_create_from_handle(void *handle, float scale, unsigned flags);
API void     window_destroy();
API void     window_reload();

API int      window_frame_begin();
API void     window_frame_end();
API void     window_frame_swap();
API int      window_swap(); // single function that combines above functions (desktop only)

API void     window_loop(void (*function)(void* loopArg), void* loopArg ); // run main loop function continuously (emscripten only)
API void     window_loop_exit(); // exit from main loop function (emscripten only)

API void     window_title(const char *title);
API void     window_color(unsigned color);
API char     window_msaa();
API vec2     window_canvas();
API void*    window_handle();
API char*    window_stats();

API uint64_t window_frame();
API int      window_width();
API int      window_height();
API double   window_time();
API double   window_delta();

// API bool  window_hook(void (*func)(), void* userdata); // deprecated
// API void  window_unhook(void (*func)()); // deprecated

API void     window_focus(); // window attribute api using haz catz language for now
API int      window_has_focus();
API void     window_fullscreen(int mode); // 0 = windowed, 1 = borderless, 2 = exclusive
API int      window_has_fullscreen();
API void     window_cursor(int visible);
API int      window_has_cursor();
API void     window_pause(int paused);
API int      window_has_pause();
API void     window_visible(int visible);
API int      window_has_visible();
API void     window_maximize(int enabled);
API int      window_has_maximize();
API void     window_set_resolution(int width, int height);
API void     window_transparent(int enabled);
API int      window_has_transparent();
API void     window_icon(const char *file_icon);
API int      window_has_icon();
API void     window_debug(int visible);
API int      window_has_debug();

API double   window_aspect();
API void     window_aspect_lock(unsigned numer, unsigned denom);
API void     window_aspect_unlock();

API double   window_fps();
API double   window_fps_target();
API void     window_fps_lock(float fps);
API void     window_fps_unlock();
API void     window_fps_vsync(int vsync);

API void     window_screenshot(const char* outfile_png); // , bool record_cursor
API int      window_record(const char *outfile_mp4); // , bool record_cursor

API vec2     window_dpi();

enum CURSOR_SHAPES {
    CURSOR_NONE,
    CURSOR_HW_ARROW,  // default
    CURSOR_HW_IBEAM,  // i-beam text cursor
    CURSOR_HW_HDRAG,  // horizontal drag/resize
    CURSOR_HW_VDRAG,  // vertical drag/resize
    CURSOR_HW_HAND,   // hand, clickable
    CURSOR_HW_CROSS,  // crosshair
    CURSOR_SW_AUTO,   // software cursor, ui driven. note: this is the only icon that may be recorded or snapshotted
};

API void     window_cursor_shape(unsigned shape);

API const char *window_clipboard();
API void        window_setclipboard(const char *text);
