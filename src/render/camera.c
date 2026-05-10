#include "camera.h"

#include <math.h>

/* Camera state is split between the engine.yaml-driven knobs and
 * the upstream-hardcoded ones. The latter (FOV, near/far, zoom_sens,
 * elev_min/max) aren't in upstream's engine.yaml either, so they
 * stay as compile-time constants here until someone surfaces them. */
void camera_init_solarsystem(camera_t *c, const engine_config_t *eng)
{
    *c = (camera_t){
        .focus_target = (HMM_Vec3){ .Elements = { 0.0f, 0.0f, 0.0f } },
        .azimuth      = 0.0f,
        .elevation    = eng->camera.default_elevation,
        .distance     = eng->solar_system_view.default_distance,

        .fov_y_deg    = 50.0f,
        .near_plane   = 0.5f,
        .far_plane    = 10000.0f,

        .orbit_sens   = eng->camera.pixels_to_radians,
        .zoom_sens    = 0.001f,
        .elev_min     = 0.1f,
        .elev_max     = 1.5f,
        .dist_min     = eng->solar_system_view.min_distance,
        .dist_max     = eng->solar_system_view.max_distance,
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
