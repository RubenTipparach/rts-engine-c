#include "camera.h"

#include <math.h>

/* Compiled-in defaults from the upstream `engine.yaml` (camera +
 * solarSystemView sections) and SolarSystemRenderer.cs constants. They
 * stay live as defaults even after engine.yaml is wired in — the YAML
 * just overrides any field whose YAML value is non-zero (per the
 * config-merge convention in CLAUDE.md). */
void camera_init_solarsystem(camera_t *c)
{
    *c = (camera_t){
        .focus_target = (HMM_Vec3){ .Elements = { 0.0f, 0.0f, 0.0f } },
        .azimuth      = 0.0f,
        .elevation    = 0.4f,      /* engine.yaml camera.defaultElevation */
        .distance     = 80.0f,     /* engine.yaml solarSystemView.defaultDistance */

        .fov_y_deg    = 50.0f,
        .near_plane   = 0.5f,
        .far_plane    = 10000.0f,

        .orbit_sens   = 0.005f,    /* engine.yaml camera.pixelsToRadians */
        .zoom_sens    = 0.001f,
        .elev_min     = 0.1f,
        .elev_max     = 1.5f,
        .dist_min     = 10.0f,     /* engine.yaml solarSystemView.minDistance */
        .dist_max     = 200.0f,    /* engine.yaml solarSystemView.maxDistance */
    };
}

void camera_orbit(camera_t *c, float dx_pixels, float dy_pixels)
{
    c->azimuth   += dx_pixels * c->orbit_sens;
    c->elevation += dy_pixels * c->orbit_sens;
    if (c->elevation < c->elev_min) c->elevation = c->elev_min;
    if (c->elevation > c->elev_max) c->elevation = c->elev_max;
}

void camera_zoom(camera_t *c, float dy)
{
    c->distance -= dy * c->distance * c->zoom_sens;
    if (c->distance < c->dist_min) c->distance = c->dist_min;
    if (c->distance > c->dist_max) c->distance = c->dist_max;
}

HMM_Vec3 camera_eye(const camera_t *c)
{
    float ce = cosf(c->elevation);
    float se = sinf(c->elevation);
    float ca = cosf(c->azimuth);
    float sa = sinf(c->azimuth);
    HMM_Vec3 offset = { .Elements = {
        ce * ca * c->distance,
        se      * c->distance,
        ce * sa * c->distance,
    }};
    return HMM_AddV3(c->focus_target, offset);
}

HMM_Mat4 camera_view(const camera_t *c)
{
    HMM_Vec3 eye = camera_eye(c);
    HMM_Vec3 up  = { .Elements = { 0.0f, 1.0f, 0.0f } };
    return HMM_LookAt_RH(eye, c->focus_target, up);
}

HMM_Mat4 camera_proj(const camera_t *c, float aspect)
{
    /* sokol_gfx depth ranges:
     *  GL / GLES3 → NDC [-1, 1]  (use _NO)
     *  Metal / D3D11 → [0, 1]    (use _ZO)
     * Only GL/GLES3 are wired in this build; flip when Metal lands. */
#if defined(SOKOL_METAL) || defined(SOKOL_D3D11)
    return HMM_Perspective_RH_ZO(HMM_AngleDeg(c->fov_y_deg), aspect,
                                 c->near_plane, c->far_plane);
#else
    return HMM_Perspective_RH_NO(HMM_AngleDeg(c->fov_y_deg), aspect,
                                 c->near_plane, c->far_plane);
#endif
}
