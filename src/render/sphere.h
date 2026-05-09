#pragma once

/* Procedural unit-radius UV sphere mesh — the placeholder geometry
 * used for the sun, planets, and moons in the M1 flat-shaded path.
 * The full per-planet terrain mesh from the planet YAMLs under
 * `assets/planets/` lands in M2; this is just enough to verify the
 * render pipeline. */

#include <stdbool.h>
#include <stdint.h>

#include "HandmadeMath.h"

typedef struct {
    HMM_Vec3 pos;
    HMM_Vec3 normal;
} sphere_vertex_t;

/* Generate a UV sphere at unit radius. `stacks` rings vertically (poles
 * count as rings), `slices` sectors azimuthally. Vertex count is
 * (stacks+1) * (slices+1); index count is stacks * slices * 6. The
 * caller sizes the destination arrays. Returns true if everything fit. */
bool sphere_make_uv(int stacks, int slices,
                    sphere_vertex_t *out_verts, int max_verts, int *out_v_count,
                    uint16_t *out_indices, int max_indices, int *out_i_count);
