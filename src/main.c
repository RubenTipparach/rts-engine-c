/* main.c — sokol_app entry point for rts-engine-c.
 *
 * Wires the platform shell (window/canvas, input, frame loop) to the
 * solar-system renderer. Backend selection is at compile time:
 *
 *   - native Linux:   SOKOL_GLCORE
 *   - native macOS:   SOKOL_METAL
 *   - native Windows: SOKOL_D3D11   (not wired in the Makefile yet)
 *   - web:            SOKOL_GLES3   (WebGL2 via emscripten)
 *
 * sokol_app + sokol_gfx + sokol_glue are single-header libs vendored
 * under third_party/sokol/ and pinned in third_party/sokol/SOKOL_COMMIT.
 * The matching impls compile here and only here. */

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"

#include "core/config.h"
#include "core/log.h"
#include "render/solarsystem.h"

static struct {
    uint64_t              last_ticks;
    solarsystem_config_t  cfg;
    bool                  cfg_loaded;
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

    solarsystem_init();
    LOG_INFO("rts-engine-c started — backend=%d", (int)sg_query_backend());
}

static void on_frame(void)
{
    const uint64_t now = stm_now();
    const double dt = stm_sec(stm_diff(now, app.last_ticks));
    app.last_ticks = now;

    const int w = sapp_width();
    const int h = sapp_height();

    sg_begin_pass(&(sg_pass){
        .action    = solarsystem_pass_action(),
        .swapchain = sglue_swapchain(),
    });
    solarsystem_frame(dt, w, h);
    sg_end_pass();
    sg_commit();
}

static void on_event(const sapp_event *ev)
{
    /* Input plumbing lands with the camera (orbit/zoom) commit. */
    (void)ev;
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
