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

/* Engine-wide knobs from assets/config/engine.yaml. The fields are a
 * subset of the upstream schema — only the sections M1 actually
 * consumes (camera, lighting, solarSystemView). Other sections in
 * the file (lod, planetEditView, rtsCamera, slopes, etc.) are read
 * by later milestones and currently ignored by the loader.
 *
 * Defaults are populated by engine_config_apply_defaults() before
 * the YAML is parsed; any field whose YAML value is non-zero
 * overrides the default. This is the "compiled-in default merged
 * with YAML overrides" convention from CLAUDE.md. */

typedef struct {
    float transition_duration;   /* seconds for click-zoom slide  */
    float default_elevation;     /* radians                       */
    float pixels_to_radians;     /* orbit drag sensitivity        */
} camera_engine_t;

typedef struct {
    HMM_Vec3 sun_direction;
    float    ambient_intensity;
    float    diffuse_intensity;
} lighting_engine_t;

typedef struct {
    float default_distance;
    float min_distance;
    float max_distance;
    int   sphere_segments_planet;
    int   sphere_segments_sun;
    int   sphere_segments_moon;
    int   orbit_ring_segments;
    int   moon_orbit_segments;
    float pick_radius_multiplier;
} solarsystem_view_engine_t;

typedef struct {
    camera_engine_t           camera;
    lighting_engine_t         lighting;
    solarsystem_view_engine_t solar_system_view;
} engine_config_t;

void engine_config_apply_defaults(engine_config_t *cfg);
bool config_load_engine(const char *path, engine_config_t *out);
void config_log_engine(const engine_config_t *cfg);

/* Per-planet config — the YAMLs under `assets/planets/`, referenced
 * by `configFile:` in solarsystem.yaml. Surface and biome data the
 * M2 mesh + biome shader consume; orbital / display fields stay
 * denormalized in solarsystem.yaml's planet entry. The parser ignores
 * sections it doesn't yet read (water, atmosphere, camera) so a
 * copy-pasted upstream YAML works as-is. */

#define CFG_MAX_TERRAIN_LEVELS  8

typedef struct {
    char     name[CFG_NAME_LEN];
    HMM_Vec3 color;
} terrain_level_t;

typedef struct {
    char            name[CFG_NAME_LEN];
    float           radius;
    int             subdivisions;
    float           step_height;
    bool            ocean_level0;

    int             noise_seed;
    float           noise_frequency;
    float           noise_thresholds[CFG_MAX_NOISE_THRESHOLDS];
    int             noise_threshold_count;

    terrain_level_t levels[CFG_MAX_TERRAIN_LEVELS];
    int             level_count;

    /* Water section (optional, only for oceanLevel0 planets). */
    bool            has_water;
    HMM_Vec3        water_color;        /* water.fogColor */
    float           water_fog_density;

    /* Atmosphere section (optional). innerRadiusMul / outerRadiusMul
     * are upstream's Nishita scatter knobs; my pixel-art atmosphere
     * shader uses outerRadiusMul as the shell radius and a simple
     * tint colour in lieu of full scattering. */
    bool            has_atmosphere;
    float           atmosphere_inner_mul;
    float           atmosphere_outer_mul;
    float           atmosphere_sun_intensity;
    HMM_Vec3        atmosphere_sun_dir;
} planet_full_config_t;

bool config_load_planet(const char *path, planet_full_config_t *out);
void config_log_planet(const planet_full_config_t *cfg);
