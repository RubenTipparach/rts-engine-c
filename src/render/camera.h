#pragma once

/* Orbit-around-target camera matching the upstream solar-system view.
 *
 * Spherical (azimuth, elevation, distance) parameterization around
 * `focus_target`. Inputs are in pixel deltas (orbit) and scroll
 * delta (zoom); the sensitivities + bounds are explicit fields so
 * they can later be sourced from engine.yaml without changing the
 * public API.
 *
 * Exact formulas mirrored from the reference's
 * `SolarSystemRenderer.cs`:
 *   - orbit:   azimuth   += dx * orbit_sens;
 *              elevation += dy * orbit_sens;   (clamped to elev_min/max)
 *   - zoom:    distance  -= dy * distance * zoom_sens;
 *              (clamped to dist_min/max)
 *   - eye:     focus + distance * (cosE*cosA, sinE, cosE*sinA)
 */

#include "HandmadeMath.h"

typedef struct {
    HMM_Vec3 focus_target;
    float    azimuth;
    float    elevation;
    float    distance;

    float    fov_y_deg;
    float    near_plane;
    float    far_plane;

    float    orbit_sens;   /* engine.yaml camera.pixelsToRadians */
    float    zoom_sens;    /* upstream literal: 0.001 */
    float    elev_min, elev_max;
    float    dist_min, dist_max;
} camera_t;

void     camera_init_solarsystem(camera_t *c);
void     camera_orbit(camera_t *c, float dx_pixels, float dy_pixels);
void     camera_zoom(camera_t *c, float dy);
HMM_Vec3 camera_eye(const camera_t *c);
HMM_Mat4 camera_view(const camera_t *c);
HMM_Mat4 camera_proj(const camera_t *c, float aspect);
