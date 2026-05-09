#include "solarsystem.h"

#include "sokol_gfx.h"

/* First-cut renderer: reads the swapchain pass that main.c opens and
 * does nothing inside it (the clear colour set in init() is what's
 * visible). Sun + orbit rings + planet spheres land in subsequent
 * commits; this stub exists so the build/render pipeline can be
 * wired end-to-end before any real geometry shows up. */

static struct {
    bool inited;
} state;

sg_pass_action solarsystem_pass_action(void);

void solarsystem_init(void)
{
    state.inited = true;
}

sg_pass_action solarsystem_pass_action(void)
{
    /* Deep-space clear colour — placeholder; will move to YAML once the
     * config loader lands. */
    return (sg_pass_action){
        .colors[0] = {
            .load_action = SG_LOADACTION_CLEAR,
            .clear_value = { 0.012f, 0.018f, 0.035f, 1.0f },
        },
    };
}

void solarsystem_frame(double dt, int fb_width, int fb_height)
{
    (void)dt; (void)fb_width; (void)fb_height;
    /* Nothing to draw yet — the pass clear is what's visible. */
}

void solarsystem_shutdown(void)
{
    state.inited = false;
}
