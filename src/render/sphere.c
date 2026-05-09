#include "sphere.h"

#include <math.h>
#include <stdbool.h>

bool sphere_make_uv(int stacks, int slices,
                    sphere_vertex_t *out_verts, int max_verts, int *out_v_count,
                    uint16_t *out_indices, int max_indices, int *out_i_count)
{
    int v_needed = (stacks + 1) * (slices + 1);
    int i_needed = stacks * slices * 6;
    if (v_needed > max_verts || i_needed > max_indices) {
        *out_v_count = 0;
        *out_i_count = 0;
        return false;
    }

    int v = 0;
    for (int sy = 0; sy <= stacks; sy++) {
        float phi = (float)sy / (float)stacks * HMM_PI;
        float cp  = cosf(phi);
        float sp  = sinf(phi);
        for (int sx = 0; sx <= slices; sx++) {
            float theta = (float)sx / (float)slices * 2.0f * HMM_PI;
            float ct    = cosf(theta);
            float st    = sinf(theta);
            HMM_Vec3 p  = { .Elements = { sp * ct, cp, sp * st } };
            out_verts[v++] = (sphere_vertex_t){ .pos = p, .normal = p };
        }
    }

    int i = 0;
    int row = slices + 1;
    for (int sy = 0; sy < stacks; sy++) {
        for (int sx = 0; sx < slices; sx++) {
            uint16_t a = (uint16_t)(sy * row + sx);
            uint16_t b = (uint16_t)((sy + 1) * row + sx);
            uint16_t c = (uint16_t)((sy + 1) * row + (sx + 1));
            uint16_t d = (uint16_t)(sy * row + (sx + 1));
            /* CCW when viewed from outside the sphere (outward-facing
             * normal). Sokol pipelines default to CCW + back-cull so
             * outward triangles must be wound this way to survive. */
            out_indices[i++] = a; out_indices[i++] = c; out_indices[i++] = b;
            out_indices[i++] = a; out_indices[i++] = d; out_indices[i++] = c;
        }
    }

    *out_v_count = v;
    *out_i_count = i;
    return true;
}
