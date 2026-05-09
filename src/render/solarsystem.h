#pragma once

/* Solar system renderer — first port subsystem.
 *
 * Mirrors reference/rts-engine/src/RtsEngine.Game/SolarSystemRenderer.cs.
 * This first cut only sets up the pass and clears to a dark space colour;
 * sun + orbit rings + planet spheres land in subsequent commits. */

#include "sokol_gfx.h"

void           solarsystem_init(void);
sg_pass_action solarsystem_pass_action(void);
void           solarsystem_frame(double dt, int fb_width, int fb_height);
void           solarsystem_shutdown(void);
