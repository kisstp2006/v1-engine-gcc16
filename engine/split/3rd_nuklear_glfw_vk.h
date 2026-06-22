/*
 * 3rd_nuklear_glfw_vk.h — Vulkan Nuklear GLFW backend for FWK.
 *
 * Mirrors 3rd_nuklear_glfw_gl3.h API pattern, but uses Vulkan via the
 * engine_vulkan.h raw-draw interface instead of OpenGL.
 *
 * Included ONLY from engine.c (where NK_IMPLEMENTATION is defined and all
 * NK types are available).  engine_vulkan.c has NO Nuklear dependencies.
 *
 * Usage (inside engine.c / engine_window.c):
 *   Init:      nk_glfw_vk_init(&nk_glfw_vk, window)
 *   New frame: nk_glfw_vk_new_frame(&nk_glfw_vk)  — call before user UI
 *   Shutdown:  nk_glfw_vk_shutdown(&nk_glfw_vk)
 *   Render:    called automatically via s_nk_render_fn callback
 */
#ifndef NK_GLFW_VK_H_
#define NK_GLFW_VK_H_

#ifdef NK_GLFW_VK_IMPLEMENTATION

#include "engine_vulkan.h"   /* raw Vulkan NK interface */

/* ── Nuklear vertex layout (matches 3rd_nuklear_glfw_gl3.h) ─────────────── */
struct nk_glfw_vk_vertex {
    float    position[2];
    float    uv[2];
    nk_byte  col[4];
};

#define NK_VK_VERTEX_BYTES (512u * 1024u)
#define NK_VK_INDEX_BYTES  (128u * 1024u)

/* ── Device state (like nk_glfw_device but for Vulkan) ───────────────────── */
struct nk_glfw_vk_device {
    struct nk_buffer           cmds;
    struct nk_draw_null_texture null;
};

/* ── Main state struct (mirrors struct nk_glfw) ──────────────────────────── */
struct nk_glfw_vk {
    GLFWwindow              *win;
    int                      width, height;
    struct nk_glfw_vk_device dev;
    struct nk_context        ctx;
    struct nk_font_atlas     atlas;
};

/* ── Static vertex/index staging buffers ─────────────────────────────────── */
static unsigned char s_vk_nk_vbuf[NK_VK_VERTEX_BYTES];
static unsigned char s_vk_nk_ibuf[NK_VK_INDEX_BYTES];
static struct nk_glfw_vk *s_nk_vk_instance = NULL;

/* ── Render callback — called from engine_vulkan_end_frame() ─────────────── */
static void nk_glfw_vk_render_cb(void) {
    struct nk_glfw_vk *glfw = s_nk_vk_instance;
    if (!glfw || !engine_vulkan_nk_is_ready()) return;

    static const struct nk_draw_vertex_layout_element vk_layout[] = {
        { NK_VERTEX_POSITION, NK_FORMAT_FLOAT,    NK_OFFSETOF(struct nk_glfw_vk_vertex, position) },
        { NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT,    NK_OFFSETOF(struct nk_glfw_vk_vertex, uv)       },
        { NK_VERTEX_COLOR,    NK_FORMAT_R8G8B8A8, NK_OFFSETOF(struct nk_glfw_vk_vertex, col)      },
        { NK_VERTEX_LAYOUT_END }
    };

    struct nk_convert_config cfg = {0};
    cfg.vertex_layout        = vk_layout;
    cfg.vertex_size          = sizeof(struct nk_glfw_vk_vertex);
    cfg.vertex_alignment     = NK_ALIGNOF(struct nk_glfw_vk_vertex);
    cfg.null                 = glfw->dev.null;
    cfg.shape_AA             = NK_ANTI_ALIASING_ON;
    cfg.line_AA              = NK_ANTI_ALIASING_ON;
    cfg.circle_segment_count = 22;
    cfg.curve_segment_count  = 22;
    cfg.arc_segment_count    = 22;
    cfg.global_alpha         = 1.0f;

    /* Convert NK draw list → vertex/index data into static staging buffers */
    struct nk_buffer vbuf, ibuf;
    nk_buffer_init_fixed(&vbuf, s_vk_nk_vbuf, NK_VK_VERTEX_BYTES);
    nk_buffer_init_fixed(&ibuf, s_vk_nk_ibuf, NK_VK_INDEX_BYTES);
    nk_convert(&glfw->ctx, &glfw->dev.cmds, &vbuf, &ibuf, &cfg);

    /* Upload to per-frame GPU buffers */
    void *vptr = engine_vulkan_nk_map_vbuf();
    void *iptr = engine_vulkan_nk_map_ibuf();
    if (!vptr || !iptr) { nk_clear(&glfw->ctx); return; }
    memcpy(vptr, s_vk_nk_vbuf, NK_VK_VERTEX_BYTES);
    memcpy(iptr, s_vk_nk_ibuf, NK_VK_INDEX_BYTES);
    engine_vulkan_nk_unmap();

    /* Setup pipeline + orthographic projection */
    float W = engine_vulkan_nk_viewport_w();
    float H = engine_vulkan_nk_viewport_h();
    if (W <= 0 || H <= 0) { nk_clear(&glfw->ctx); return; }
    engine_vulkan_nk_begin_draw(W, H);

    /* Execute each NK draw command */
    uint32_t idx_offset = 0;
    const struct nk_draw_command *cmd;
    nk_draw_foreach(cmd, &glfw->ctx, &glfw->dev.cmds) {
        if (!cmd->elem_count) continue;
        engine_vulkan_nk_draw_cmd(
            (uint64_t)(uintptr_t)cmd->texture.id,
            (int32_t)cmd->clip_rect.x, (int32_t)cmd->clip_rect.y,
            (int32_t)cmd->clip_rect.w, (int32_t)cmd->clip_rect.h,
            cmd->elem_count, idx_offset);
        idx_offset += cmd->elem_count;
    }
    nk_clear(&glfw->ctx);
    nk_buffer_clear(&glfw->dev.cmds);
}

/* ── Init ────────────────────────────────────────────────────────────────── */
static struct nk_context *nk_glfw_vk_init(struct nk_glfw_vk *glfw, GLFWwindow *win) {
    if (!engine_vulkan_nk_is_ready()) return NULL;

    glfw->win = win;
    glfwGetWindowSize(win, &glfw->width, &glfw->height);

    /* Font atlas */
    nk_font_atlas_init_default(&glfw->atlas);
    nk_font_atlas_begin(&glfw->atlas);
    struct nk_font *font = nk_font_atlas_add_default(&glfw->atlas, 13.0f, NULL);
    int aw, ah;
    const void *pixels = nk_font_atlas_bake(&glfw->atlas, &aw, &ah, NK_FONT_ATLAS_RGBA32);

    /* Upload font atlas */
    fwk_backend_texture_t font_vk = engine_vulkan_nk_upload_font(pixels, aw, ah);
    fwk_backend_texture_t null_vk = engine_vulkan_nk_white_tex();

    nk_font_atlas_end(&glfw->atlas,
                      nk_handle_id((int)(uintptr_t)font_vk),
                      &glfw->dev.null);
    glfw->dev.null.texture = nk_handle_id((int)(uintptr_t)null_vk);

    nk_init_default(&glfw->ctx, &font->handle);
    nk_buffer_init_default(&glfw->dev.cmds);

    /* Register render callback — called from inside engine_vulkan_end_frame */
    s_nk_vk_instance = glfw;
    engine_vulkan_set_nk_render_fn(nk_glfw_vk_render_cb);

    fprintf(stderr, "[Vulkan NK] Initialized  font=%dx%d\n", aw, ah);
    return &glfw->ctx;
}

/* ── New frame (process GLFW input → NK) ─────────────────────────────────── */
static void nk_glfw_vk_new_frame(struct nk_glfw_vk *glfw) {
    if (!glfw || !glfw->win) return;
    GLFWwindow *win = glfw->win;
    glfwGetWindowSize(win, &glfw->width, &glfw->height);

    struct nk_context *ctx = &glfw->ctx;
    nk_input_begin(ctx);

    /* Text input from char callback (stored by engine_vulkan.c) */
    {
        int *len = engine_vulkan_nk_text_len();
        unsigned *buf = engine_vulkan_nk_text_buf();
        for (int i = 0; i < *len; i++)
            nk_input_unicode(ctx, buf[i]);
        *len = 0;
    }

    /* Keyboard */
    int ctrl = glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) || glfwGetKey(win, GLFW_KEY_RIGHT_CONTROL);
    nk_input_key(ctx, NK_KEY_DEL,            glfwGetKey(win, GLFW_KEY_DELETE)    == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_ENTER,          glfwGetKey(win, GLFW_KEY_ENTER)     == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_TAB,            glfwGetKey(win, GLFW_KEY_TAB)       == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_BACKSPACE,      glfwGetKey(win, GLFW_KEY_BACKSPACE) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_UP,             glfwGetKey(win, GLFW_KEY_UP)        == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_DOWN,           glfwGetKey(win, GLFW_KEY_DOWN)      == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_LEFT,           !ctrl && glfwGetKey(win, GLFW_KEY_LEFT)  == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_RIGHT,          !ctrl && glfwGetKey(win, GLFW_KEY_RIGHT) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_TEXT_LINE_START, ctrl && glfwGetKey(win, GLFW_KEY_LEFT)  == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_TEXT_LINE_END,   ctrl && glfwGetKey(win, GLFW_KEY_RIGHT) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_SCROLL_START,   glfwGetKey(win, GLFW_KEY_HOME)     == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_SCROLL_END,     glfwGetKey(win, GLFW_KEY_END)      == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_SCROLL_UP,      glfwGetKey(win, GLFW_KEY_PAGE_UP)  == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_SCROLL_DOWN,    glfwGetKey(win, GLFW_KEY_PAGE_DOWN)== GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_COPY,   ctrl && glfwGetKey(win, GLFW_KEY_C) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_PASTE,  ctrl && glfwGetKey(win, GLFW_KEY_V) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_CUT,    ctrl && glfwGetKey(win, GLFW_KEY_X) == GLFW_PRESS);
    nk_input_key(ctx, NK_KEY_TEXT_UNDO, ctrl && glfwGetKey(win, GLFW_KEY_Z) == GLFW_PRESS);

    /* Mouse */
    double mx, my;
    glfwGetCursorPos(win, &mx, &my);
    nk_input_motion(ctx, (int)mx, (int)my);
    nk_input_button(ctx, NK_BUTTON_LEFT,   (int)mx,(int)my, glfwGetMouseButton(win,GLFW_MOUSE_BUTTON_LEFT)  ==GLFW_PRESS);
    nk_input_button(ctx, NK_BUTTON_MIDDLE, (int)mx,(int)my, glfwGetMouseButton(win,GLFW_MOUSE_BUTTON_MIDDLE)==GLFW_PRESS);
    nk_input_button(ctx, NK_BUTTON_RIGHT,  (int)mx,(int)my, glfwGetMouseButton(win,GLFW_MOUSE_BUTTON_RIGHT) ==GLFW_PRESS);

    /* Scroll — read BEFORE input_update resets eng_scroll_x/y */
    extern double eng_scroll_x, eng_scroll_y;
    nk_input_scroll(ctx, nk_vec2((float)eng_scroll_x, (float)eng_scroll_y));

    nk_input_end(ctx);
}

/* ── Shutdown ────────────────────────────────────────────────────────────── */
static void nk_glfw_vk_shutdown(struct nk_glfw_vk *glfw) {
    if (!glfw) return;
    engine_vulkan_set_nk_render_fn(NULL);
    s_nk_vk_instance = NULL;
    nk_buffer_free(&glfw->dev.cmds);
    nk_font_atlas_clear(&glfw->atlas);
    nk_free(&glfw->ctx);
    memset(glfw, 0, sizeof(*glfw));
}

#endif /* NK_GLFW_VK_IMPLEMENTATION */
#endif /* NK_GLFW_VK_H_ */
