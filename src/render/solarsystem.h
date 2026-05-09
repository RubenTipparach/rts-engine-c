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

void           solarsystem_init(const solarsystem_config_t *cfg);
sg_pass_action solarsystem_pass_action(void);
void           solarsystem_frame(double dt, int fb_width, int fb_height,
                                 const camera_t *cam);
void           solarsystem_shutdown(void);
