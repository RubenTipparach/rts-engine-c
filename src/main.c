/* main.c — sokol_app entry point for rts-engine-c.
 *
 * Wires the platform shell (window/canvas, input, frame loop) to the
 * solar-system renderer + the orbit camera. Backend selection is at
 * compile time:
 *
 *   - native Linux:   SOKOL_GLCORE
 *   - native macOS:   SOKOL_METAL
 *   - native Windows: SOKOL_D3D11   (not wired in the Makefile yet)
 *   - web:            SOKOL_GLES3   (WebGL2 via emscripten)
 *
 * sokol_app + sokol_gfx + sokol_glue are single-header libs vendored
 * under third_party/sokol/ and pinned in third_party/sokol/SOKOL_COMMIT.
 * The matching impls compile in src/sokol_impl.c. */

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"

#include "core/config.h"
#include "core/log.h"
#include "render/camera.h"
#include "render/solarsystem.h"

#include <math.h>

/* Pixels of cumulative pointer travel that flips a press from "click"
 * (releases as a pick) to "drag" (releases without picking, keeps the
 * camera-orbit it started). 5 px is the Win32 default. */
#define DRAG_VS_CLICK_THRESHOLD_PX 5.0f

static struct {
    uint64_t              last_ticks;
    solarsystem_config_t  cfg;
    bool                  cfg_loaded;
    camera_t              camera;

    bool                  left_pressed;
    bool                  dragging;
    float                 press_x, press_y;
    float                 drag_total;
} app;

static void on_init(void)
{
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
    stm_setup();
    app.last_ticks = stm_now();

    /* Path is the same on native and web — emscripten preloads
     * `assets/` at the VFS root via `--preload-file assets@/assets`,
     * so a relative path resolves against `/` there and against the
     * project root on native. */
    app.cfg_loaded = config_load_solarsystem("assets/config/solarsystem.yaml", &app.cfg);
    if (app.cfg_loaded) config_log_solarsystem(&app.cfg);

    camera_init_solarsystem(&app.camera);
    solarsystem_init(&app.cfg);
    LOG_INFO("rts-engine-c started — backend=%d", (int)sg_query_backend());
}

static void on_frame(void)
{
    const uint64_t now = stm_now();
    const double dt = stm_sec(stm_diff(now, app.last_ticks));
    app.last_ticks = now;

    /* Pre-frame hooks: advance click-zoom transition + lock focus on
     * the active body so an orbiting planet stays centered. */
    solarsystem_pre_frame(dt, &app.camera);

    const int w = sapp_width();
    const int h = sapp_height();

    sg_begin_pass(&(sg_pass){
        .action    = solarsystem_pass_action(),
        .swapchain = sglue_swapchain(),
    });
    solarsystem_frame(dt, w, h, &app.camera);
    sg_end_pass();
    sg_commit();
}

static void on_event(const sapp_event *ev)
{
    switch (ev->type) {
        case SAPP_EVENTTYPE_MOUSE_DOWN:
            if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
                app.left_pressed = true;
                app.press_x      = ev->mouse_x;
                app.press_y      = ev->mouse_y;
                app.drag_total   = 0.0f;
                /* Don't start orbit yet — wait until the cursor has
                 * actually moved past the click threshold. That lets
                 * a tap-with-zero-motion be picked up as a click. */
            }
            break;

        case SAPP_EVENTTYPE_MOUSE_UP:
            if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT && app.left_pressed) {
                if (!app.dragging && app.drag_total <= DRAG_VS_CLICK_THRESHOLD_PX) {
                    /* Click → pick. ev->mouse_x/y are valid here
                     * because the mouse was never locked. */
                    solarsystem_pick((int)app.press_x, (int)app.press_y,
                                     sapp_width(), sapp_height(), &app.camera);
                }
                if (app.dragging) {
                    app.dragging = false;
                    sapp_lock_mouse(false);
                }
                app.left_pressed = false;
            }
            break;

        case SAPP_EVENTTYPE_MOUSE_MOVE:
            if (app.left_pressed) {
                app.drag_total += fabsf(ev->mouse_dx) + fabsf(ev->mouse_dy);
                if (!app.dragging && app.drag_total > DRAG_VS_CLICK_THRESHOLD_PX) {
                    app.dragging = true;
                    sapp_lock_mouse(true);
                }
                if (app.dragging) {
                    /* sapp delivers raw deltas while the pointer is
                     * locked, which is the pixel-delta unit camera_orbit
                     * expects. */
                    camera_orbit(&app.camera, ev->mouse_dx, ev->mouse_dy);
                }
            }
            break;

        case SAPP_EVENTTYPE_MOUSE_SCROLL:
            /* sapp scroll_y is wheel ticks (positive = scroll up).
             * The upstream zoom formula expects "delta" in ~pixels;
             * 100 turns one notch into a perceptible step. */
            camera_zoom(&app.camera, ev->scroll_y * 100.0f);
            break;

        case SAPP_EVENTTYPE_KEY_DOWN:
            if (ev->key_code == SAPP_KEYCODE_ESCAPE) {
                solarsystem_focus_sun(&app.camera);
            }
            break;

        default:
            break;
    }
}

static void on_cleanup(void)
{
    solarsystem_shutdown();
    sg_shutdown();
}

sapp_desc sokol_main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    return (sapp_desc){
        .init_cb     = on_init,
        .frame_cb    = on_frame,
        .event_cb    = on_event,
        .cleanup_cb  = on_cleanup,
        .width       = 1280,
        .height      = 720,
        .window_title = "rts-engine-c",
        .high_dpi    = true,
        .logger.func = slog_func,
    };
}
