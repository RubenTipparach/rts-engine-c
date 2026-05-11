#pragma once

/* Solar system renderer — first port subsystem.
 *
 * Mirrors `reference/rts-engine/src/RtsEngine.Game/SolarSystemRenderer.cs`
 * in shape but flat-shaded for M1. The full sun corona + atmosphere
 * shader port lands as a follow-up commit (per the user's M1 step 3
 * scoping decision). Rendering: sun + N planets + M moons, each as a
 * UV sphere translated to the body's current orbital position; orbit
 * angles tick from frame dt. */

#include "core/config.h"
#include "render/camera.h"
#include "sokol_gfx.h"

/* Initialize the renderer with the parsed YAML configs. `planet_full`
 * is an array of per-planet full configs in the same order as
 * cfg->planets[]; pass NULL/0 to fall back to the planet's flat
 * colour from solarsystem.yaml (no biome bands). */
void           solarsystem_init(const solarsystem_config_t  *cfg,
                                const engine_config_t       *eng,
                                const planet_full_config_t  *planet_full,
                                int                          planet_full_count);
sg_pass_action solarsystem_pass_action(void);

/* Run before solarsystem_frame() — slides the camera's focus_target
 * and distance toward the active body for a click-to-zoom transition,
 * and keeps the focus locked on the body after the transition (so a
 * planet that's still orbiting stays centered). No-op in sun mode. */
void           solarsystem_pre_frame(double dt, camera_t *cam);

void           solarsystem_frame(double dt, int fb_width, int fb_height,
                                 const camera_t *cam);

/* Click-pick: build a world-space ray from the screen position and
 * test it against each body's bounding sphere. If something hits,
 * start a smooth transition (zoom in to that body, or back to the
 * sun if the sun was clicked). Returns true if a body was picked. */
bool           solarsystem_pick(int sx, int sy, int fb_w, int fb_h,
                                 const camera_t *cam);

/* Force the camera back to the sun-centered view (Escape key, etc). */
void           solarsystem_focus_sun(const camera_t *cam);

/* Name of the body the camera is currently focused on (for debug HUD).
 * Returns "Sol" in sun mode, the planet/moon name otherwise. */
const char *   solarsystem_active_body_name(void);

/* True while a click-zoom transition is animating, for debug HUD. */
bool           solarsystem_is_transitioning(void);

/* Number of cloud draw calls dispatched in the most recent frame.
 * Zero means the per-body loop never reached its cloud branch — a
 * structural / build-side issue. Non-zero means draws are dispatched
 * and any invisibility is GPU-side. */
int            solarsystem_cloud_draws_last_frame(void);

void           solarsystem_shutdown(void);
