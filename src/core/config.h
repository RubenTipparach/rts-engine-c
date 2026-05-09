#pragma once

/* Solar-system + body config schema, mirrored from the upstream
 * reference's `assets/config/solarsystem.yaml` and `assets/planets/`
 * YAML files.
 *
 * The loader is a deliberately tiny YAML subset (top-level sections,
 * scalars, [a, b, c] inline arrays, list items, nested moons, `#`
 * comments) — see src/core/config.c. Don't pull in libyaml for a
 * handful of files. */

#include <stdbool.h>

#include "HandmadeMath.h"

#define CFG_MAX_PLANETS          16
#define CFG_MAX_MOONS             4
#define CFG_MAX_NOISE_THRESHOLDS  8
#define CFG_NAME_LEN             64
#define CFG_PATH_LEN            128

/* Shared shape for a planet *or* a moon — both have the same orbital
 * + procedural fields in the upstream YAML. */
typedef struct {
    char     name[CFG_NAME_LEN];
    char     config_file[CFG_PATH_LEN];
    float    orbit_radius;
    float    orbit_speed;        /* rad/sec, scaled by upstream's TimeScale */
    float    phase;              /* starting orbital angle, rad */
    float    display_radius;
    HMM_Vec3 color;
    int      noise_seed;
    float    noise_frequency;
    float    noise_thresholds[CFG_MAX_NOISE_THRESHOLDS];
    int      noise_threshold_count;
    float    zoom_min;
    float    zoom_max;
} body_config_t;

typedef struct {
    body_config_t self;
    body_config_t moons[CFG_MAX_MOONS];
    int           moon_count;
} planet_config_t;

typedef struct {
    char     name[CFG_NAME_LEN];
    HMM_Vec3 color;
    float    radius;
    HMM_Vec3 glow_color;
    float    glow_radius;
    float    corona_speed;
} sun_config_t;

typedef struct {
    sun_config_t    sun;
    planet_config_t planets[CFG_MAX_PLANETS];
    int             planet_count;
} solarsystem_config_t;

bool config_load_solarsystem(const char *path, solarsystem_config_t *out);
void config_log_solarsystem(const solarsystem_config_t *cfg);
