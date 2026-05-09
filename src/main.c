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
#include "sokol_debugtext.h"

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
    engine_config_t       eng;
    /* One full per-planet config per entry in cfg.planets[], indexed
     * by the same planet index. Loaded eagerly from the configFile
     * path each planet entry carries. */
    planet_full_config_t  planet_full[CFG_MAX_PLANETS];
    int                   planet_full_count;
    bool                  cfg_loaded;
    bool                  eng_loaded;
    camera_t              camera;

    bool                  left_pressed;
    bool                  dragging;
    float                 press_x, press_y;
    float                 drag_total;

    /* Smoothed FPS for the HUD so the number isn't jittery. */
    float                 fps_smoothed;
} app;

static void on_init(void)
{
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
    /* HUD text. We only need one font for now — c64 is the chunkiest
     * of the builtins so it stays readable on phones at high DPI. */
    sdtx_setup(&(sdtx_desc_t){
        .fonts       = { [0] = sdtx_font_c64() },
        .logger.func = slog_func,
    });

    stm_setup();
    app.last_ticks = stm_now();

    /* Path is the same on native and web — emscripten preloads
     * `assets/` at the VFS root via `--preload-file assets@/assets`,
     * so a relative path resolves against `/` there and against the
     * project root on native. engine.yaml is loaded first so its
     * defaults are baked in even if the file is missing. */
    app.eng_loaded = config_load_engine("assets/config/engine.yaml", &app.eng);
    if (!app.eng_loaded) engine_config_apply_defaults(&app.eng);
    config_log_engine(&app.eng);

    app.cfg_loaded = config_load_solarsystem("assets/config/solarsystem.yaml", &app.cfg);
    if (app.cfg_loaded) config_log_solarsystem(&app.cfg);

    /* Each planet entry in solarsystem.yaml carries `configFile:` (e.g.
     * "planets/earth.yaml") relative to assets/. Load each one for its
     * surface + biome data — needed by the M2 mesh path. */
    app.planet_full_count = 0;
    for (int i = 0; i < app.cfg.planet_count && i < CFG_MAX_PLANETS; i++) {
        const char *cf = app.cfg.planets[i].self.config_file;
        if (cf[0] == '\0') continue;
        char path[256];
        snprintf(path, sizeof(path), "assets/%s", cf);
        if (config_load_planet(path, &app.planet_full[app.planet_full_count])) {
            config_log_planet(&app.planet_full[app.planet_full_count]);
            app.planet_full_count++;
        }
    }

    camera_init_solarsystem(&app.camera, &app.eng);
    solarsystem_init(&app.cfg, &app.eng);
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

    /* HUD overlay. The c64 font is 8x8 cells, so a 2x logical canvas
     * gives us ~16px characters at native resolution — readable on
     * phones without a separate scale uniform. */
    sdtx_canvas((float)w * 0.5f, (float)h * 0.5f);
    sdtx_origin(1.0f, 1.0f);
    sdtx_font(0);

    /* exponential moving average so the FPS readout is steady */
    float inst_fps = (dt > 1e-6) ? (float)(1.0 / dt) : 0.0f;
    app.fps_smoothed += (inst_fps - app.fps_smoothed) * 0.05f;

    sdtx_color3f(0.85f, 0.95f, 1.00f);
    sdtx_printf("rts-engine-c  %dx%d  %4.0f fps\n", w, h, app.fps_smoothed);

    sdtx_color3f(0.70f, 0.80f, 0.90f);
    sdtx_printf("config: %s   bodies: %d  (sun + %d planets)\n",
                app.cfg_loaded ? "loaded" : "MISSING",
                app.cfg.planet_count + 1,
                app.cfg.planet_count);

    sdtx_color3f(0.95f, 0.85f, 0.50f);
    sdtx_printf("view: %s%s   dist: %5.1f   az: %5.2f   el: %4.2f\n",
                solarsystem_active_body_name(),
                solarsystem_is_transitioning() ? " (zooming...)" : "",
                app.camera.distance, app.camera.azimuth, app.camera.elevation);

    sdtx_color3f(0.55f, 0.65f, 0.75f);
    sdtx_printf("\nclick a planet to zoom in   ESC: back to sun   drag: orbit   scroll: zoom\n");

    sg_begin_pass(&(sg_pass){
        .action    = solarsystem_pass_action(),
        .swapchain = sglue_swapchain(),
    });
    solarsystem_frame(dt, w, h, &app.camera);
    /* Text on top of the 3D scene. sdtx_draw() must run inside the
     * same pass so it composites against the rendered geometry. */
    sdtx_draw();
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
    sdtx_shutdown();
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
