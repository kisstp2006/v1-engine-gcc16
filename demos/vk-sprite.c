// vk-sprite: 2D sprite demo that runs on both OpenGL and Vulkan backends.
//
// Usage:
//   ./vk-sprite          -- OpenGL  (default)
//   ./vk-sprite --vulkan -- Vulkan
//
// Compile (GL):
//   gcc vk-sprite.c engine/engine.c -Iengine -Iengine/vendor -lm -ldl -lpthread -lX11 -o vk-sprite
//
// Compile (Vulkan enabled):
//   gcc vk-sprite.c engine/engine.c engine/engine_vulkan.c -DENABLE_VULKAN=1 \
//       -Iengine -Iengine/vendor -lm -ldl -lpthread -lX11 -lvulkan -o vk-sprite

#include "engine.h"

// ── Cat sprite state ──────────────────────────────────────────────────────────

typedef struct {
    float x, y;
    float vx, vy;
    int   cat, flip;
    float anim_speed;
    float move_timer;
    float elapsed;
} cat_t;

#define NUM_CATS 200

static cat_t cats[NUM_CATS];

static void cats_init(void) {
    for (int i = 0; i < NUM_CATS; ++i) {
        randset(i);
        cats[i].x          = randf() * window_width();
        cats[i].y          = randf() * window_height();
        cats[i].vx         = 0;
        cats[i].vy         = 0;
        cats[i].cat        = randi(0, 4);
        cats[i].flip       = randf() < 0.5f;
        cats[i].anim_speed = 0.8f + randf() * 0.3f;
        cats[i].move_timer = 0;
        cats[i].elapsed    = 0;
    }
}

static void cats_update_draw(texture_t cat_img, texture_t shadow_img, float dt) {
    const int w = window_width(), h = window_height();
    const float walk_speed = 60.f;

    for (int i = 0; i < NUM_CATS; ++i) {
        cat_t *c = &cats[i];

        // movement
        c->x += c->vx * dt;
        c->y += c->vy * dt;
        if (c->x < 0)  c->x += w;  else if (c->x > w) c->x -= w;
        if (c->y < 0)  c->y += h;  else if (c->y > h) c->y -= h;

        int walking = (c->vx != 0 || c->vy != 0);
        c->elapsed    += dt * (walking ? 3.f : 1.f) * c->anim_speed;
        c->move_timer -= dt * (walking ? 3.f : 1.f);

        if (c->move_timer < 0) {
            if (randf() < 0.2f) {
                c->vx         = (randf() * 2 - 1) * walk_speed;
                c->vy         = (randf() * 2 - 1) * walk_speed * 0.5f;
                c->flip       = c->vx < 0;
            } else {
                c->vx = c->vy = 0;
            }
            c->move_timer = 1.f + randf() * 5.f;
        }

        // animation frame (8x4 sprite sheet: rows 0-3 idle, rows 4-7 walk)
        int base_frame = c->cat * 8 + (walking ? 4 : 0);
        int frame_num  = base_frame + ((int)(c->elapsed * 8)) % 4;

        float xscale = 2.f * (c->flip ? -1.f : 1.f);
        float scale[2]  = { xscale, 2.f };
        float pos[3]    = { c->x, c->y, c->y };
        float no_off[2] = { 0, 0 };
        float sheet[3]  = { frame_num, 8, 4 };

        // shadow (render behind by using negative z-index)
        float spos[3]   = { c->x, c->y, -c->y };
        float soff[2]   = { -1.f, 5.f };
        float ssheet[3] = { 0, 0, 0 };
        sprite_sheet(shadow_img, ssheet, spos, 0, soff, scale, rgba(255,255,255,153), 0);

        // cat sprite
        sprite_sheet(cat_img, sheet, pos, 0, no_off, scale, WHITE, 0);
    }
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    engine_backend_t backend = ENGINE_BACKEND_GL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--vulkan") == 0) { backend = ENGINE_BACKEND_VULKAN; break; }
    }

    if (!window_create_ex(75.f, 0, backend)) {
        fprintf(stderr, "Failed to create window (backend=%d)\n", backend);
        return 1;
    }

    window_title(va("vk-sprite [%s]", backend == ENGINE_BACKEND_VULKAN ? "Vulkan" : "OpenGL"));
    window_color(GRAY);

    // Camera in 2D mode: position = (cx, cy, zoom)
    camera_t cam = camera();
    cam.position  = vec3(window_width() / 2.f, window_height() / 2.f, 1.f);
    camera_enable(&cam);

    // Load sprite sheets
    texture_t cat_img    = texture("cat.png",        TEXTURE_LINEAR);
    texture_t shadow_img = texture("cat-shadow.png", TEXTURE_LINEAR);

    cats_init();

    while (window_swap()) {
        if (input_down(KEY_ESC)) break;
        if (input_down(KEY_F11)) window_fullscreen(window_has_fullscreen() ^ 1);

        // camera pan & zoom
        if (!ui_hover() && !ui_active()) {
            if (input(MOUSE_L)) {
                cam.position.x -= input_diff(MOUSE_X);
                cam.position.y -= input_diff(MOUSE_Y);
            }
            cam.position.z += input_diff(MOUSE_W) * 0.1f;
        }

        // fx_begin / fx_end are no-ops in Vulkan mode (guarded in engine)
        fx_begin();
            cats_update_draw(cat_img, shadow_img, window_delta());
            sprite_flush();
        fx_end(0, 0);

        /* Nuklear/UI csak GL módban érhető el — Vulkan módban ui_ctx nincs inicializálva */
        if (backend == ENGINE_BACKEND_GL && ui_panel("vk-sprite", 0)) {
            ui_label(va("Backend: OpenGL"));
            ui_label(va("FPS: %.1f", window_fps()));
            ui_label(va("Cats: %d", NUM_CATS));
            ui_panel_end();
        }
    }

    return 0;
}
