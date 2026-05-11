#include "solarsystem.h"

#include "core/log.h"
#include "core/noise.h"
#include "render/goldberg.h"
#include "render/sphere.h"
#include "gen/sun.glsl.h"
#include "gen/solarsystem.glsl.h"
#include "gen/orbit.glsl.h"
#include "gen/atmosphere.glsl.h"
#include "gen/clouds.glsl.h"
#include "gen/starfield.glsl.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

/* M1 — solar-system port using the upstream sun.glsl + solarsystem.glsl
 * shaders compiled through sokol-shdc.
 *
 * Geometry: a single shared unit-sphere mesh (UV sphere, 32 stacks /
 * 48 slices). One ibuf is shared across every body; vbufs are
 * per-body so the planet's flat colour and brightness can be baked
 * into every vertex (which is the per-vertex layout the upstream
 * solarsystem shader expects). The sun pipeline reads only `aPos`
 * from the same 40-byte vertex layout — the unused fields cost a few
 * KB of VRAM total, the simplification is worth it.
 *
 * Time scale: matches upstream's GetPosition(),
 *   angle = phase + orbit_speed * sim_time * 0.1
 * (the 0.1 multiplier is the "TimeScale=0.1" comment in the YAML). */

#define ENGINE_TIME_SCALE       0.1f
#define SPHERE_STACKS           32
#define SPHERE_SLICES           48
#define SPHERE_MAX_VERTS        ((SPHERE_STACKS + 1) * (SPHERE_SLICES + 1))
#define SPHERE_MAX_INDICES      (SPHERE_STACKS * SPHERE_SLICES * 6)
/* Upper bound for the static orbit-ring vbuf. The actual segment
 * count comes from engine.yaml solarSystemView.orbitRingSegments at
 * runtime — clamped to [8, ORBIT_RING_MAX_SEGMENTS]. */
#define ORBIT_RING_MAX_SEGMENTS 128

/* Sun + every planet + every moon get their own vbuf. */
#define MAX_BODIES          (1 + CFG_MAX_PLANETS * (1 + CFG_MAX_MOONS))

/* Goldberg subdiv 3 → 642 cells. Each cell emits up to 6 hex tris
 * for the top fan + up to 6 walls × 4 tris (double-sided) = 30
 * tris per cell × 3 verts = 90 verts per cell. 642 × 90 = 57780,
 * comfortably under the uint16 cap. */
#define BIOME_VBUF_MAX      65535

typedef struct {
    HMM_Vec3 pos;        /* 0  */
    HMM_Vec3 normal;     /* 12 */
    HMM_Vec3 color;      /* 24 */
    float    brightness; /* 36 */
} sphere_full_vertex_t;  /* 40 bytes total */

typedef enum { BODY_SUN, BODY_PLANET, BODY_MOON } body_kind_t;

typedef struct {
    body_kind_t kind;
    char        name[CFG_NAME_LEN];
    sg_buffer   vbuf;
    sg_buffer   ibuf;                /* either ibuf (shared) or ibuf_biome   */
    int         draw_count;          /* index count for this body            */
    /* Optional water shell (smooth sphere at sea level), only set for
     * planets with `oceanLevel0: true` in their YAML. Drawn after the
     * terrain mesh — depth-tested so it sits above sea-floor cells
     * but below land cells. */
    sg_buffer   water_vbuf;
    bool        has_water;
    /* Atmosphere shell (shader expects body-local space — see
     * atmosphere.glsl). atmo_outer_mul is the shell's radius in
     * those units; atmo_sun_intensity is the YAML's sunIntensity. */
    sg_buffer   atmo_vbuf;
    bool        has_atmosphere;
    float       atmo_outer_mul;
    float       atmo_sun_intensity;
    /* Cloud shells — up to two layers per planet. Each layer has a
     * vbuf at its own radius and per-layer scale / threshold / drift /
     * alpha so the C side can vary thickness + speed across layers. */
    sg_buffer   cloud_vbuf[2];
    HMM_Vec4    cloud_color[2];     /* rgb + alpha */
    HMM_Vec4    cloud_params[2];    /* drift, scale, threshold, bump */
    int         cloud_layers;
    HMM_Vec3    base_color;          /* for log + future use */
    float       radius;              /* world-space draw radius */
    float       orbit_radius;
    float       orbit_speed;
    float       phase;
    int         parent_index;        /* -1 for sun/planets, planet index for moons */
    /* Click-zoom range, in radius units, copied from solarsystem.yaml. */
    float       zoom_min;
    float       zoom_max;
} body_entry_t;

static struct {
    bool                              inited;
    const solarsystem_config_t       *cfg;
    const engine_config_t            *eng;
    const planet_full_config_t       *planet_full;
    int                               planet_full_count;

    sg_buffer    ibuf;
    sg_buffer    ibuf_biome;       /* sequential 0..N-1 for cell-stepped planets */
    int          index_count;

    body_entry_t bodies[MAX_BODIES];
    int          body_count;

    sg_shader    sun_shd;
    sg_pipeline  sun_pip;
    sg_shader    ss_shd;
    sg_pipeline  ss_pip;

    sg_buffer    orbit_vbuf;
    int          orbit_segments;     /* line-strip vertex count = N+1 */
    sg_shader    orbit_shd;
    sg_pipeline  orbit_pip;

    /* Atmosphere shell — uses the same shared simple-sphere ibuf as
     * the water shell; per-planet vbufs scale the unit sphere outward
     * to outerRadiusMul × planet_radius. The pipeline disables depth
     * write and culls front faces (we view the inside-out shell from
     * outside the planet) so the rim glow composites cleanly on top
     * of the terrain mesh. */
    sg_shader    atmo_shd;
    sg_pipeline  atmo_pip;

    /* Cloud shells — same fbm/bump shader for every planet's cloud
     * layers, one pipeline shared across all of them. Per-body
     * vbuf + uniforms vary per layer. */
    sg_shader    cloud_shd;
    sg_pipeline  cloud_pip;

    /* Starfield — POINTS primitive, 1px stars at infinity. Drawn first
     * each frame against an identity-translated view matrix so the
     * stars rotate with the camera but never translate. */
    sg_buffer    starfield_vbuf;
    sg_shader    starfield_shd;
    sg_pipeline  starfield_pip;

    double       sim_time;

    /* Click-to-zoom state. active_body == 0 means "sun mode" (focus
     * at origin); otherwise it's an index into bodies[]. The
     * transition lerps focus + distance from `from_*` to the body's
     * current world position over `transition_dur` seconds, then
     * keeps `focus_target` glued to the body each frame. */
    int          active_body;
    bool         transitioning;
    float        transition_t;
    float        transition_dur;
    HMM_Vec3     from_focus;
    float        from_distance;
    float        to_distance;
} state;

static HMM_Vec3 body_world_pos(const body_entry_t *b, double t);

static void resolve_world_positions(HMM_Vec3 *out)
{
    for (int i = 0; i < state.body_count; i++) {
        HMM_Vec3 local = body_world_pos(&state.bodies[i], state.sim_time);
        out[i] = (state.bodies[i].parent_index >= 0)
            ? HMM_AddV3(out[state.bodies[i].parent_index], local)
            : local;
    }
}

/* ---- mesh + per-body vbuf ---- */

static sphere_vertex_t       s_unit_verts[SPHERE_MAX_VERTS];
static uint16_t              s_unit_indices[SPHERE_MAX_INDICES];
static int                   s_unit_v_count;
static int                   s_unit_i_count;
static sphere_full_vertex_t  s_full_scratch[SPHERE_MAX_VERTS];

/* Goldberg cell-stepped biome path — fan-triangulated verts per cell,
 * duplicated across cells so adjacent biomes render with crisp
 * colour breaks. */
static sphere_full_vertex_t  s_biome_scratch[BIOME_VBUF_MAX];
static uint16_t              s_biome_indices[BIOME_VBUF_MAX];

/* Goldberg cell list — built once at init from a level-3 icosphere.
 * Shared across every biome planet (same subdiv → same cell layout);
 * per-planet differences are baked into the per-vertex colour. */
static goldberg_cell_t       s_goldberg_cells[GOLDBERG_MAX_CELLS];
static int                   s_goldberg_cell_count;
/* Index count of the biome ibuf — set after the first planet builds
 * so the shared sequential ibuf can be sized correctly. */
static int                   s_biome_index_count;

static sg_buffer build_body_vbuf(HMM_Vec3 color, float brightness, const char *label)
{
    for (int i = 0; i < s_unit_v_count; i++) {
        s_full_scratch[i].pos        = s_unit_verts[i].pos;
        s_full_scratch[i].normal     = s_unit_verts[i].normal;
        s_full_scratch[i].color      = color;
        s_full_scratch[i].brightness = brightness;
    }
    return sg_make_buffer(&(sg_buffer_desc){
        .data = { .ptr = s_full_scratch,
                  .size = (size_t)s_unit_v_count * sizeof(sphere_full_vertex_t) },
        .label = label,
    });
}

/* Cloud shell — same simple sphere geometry, scaled outward to a
 * per-layer altitude (slightly above terrain, well below atmosphere).
 * Only aPos is read by clouds.glsl; the other fields stay zero. */
static sg_buffer build_cloud_vbuf(float radius_mul, const char *label)
{
    for (int i = 0; i < s_unit_v_count; i++) {
        s_full_scratch[i].pos        = HMM_MulV3F(s_unit_verts[i].pos, radius_mul);
        s_full_scratch[i].normal     = s_unit_verts[i].normal;
        s_full_scratch[i].color      = (HMM_Vec3){ .Elements = { 0, 0, 0 } };
        s_full_scratch[i].brightness = 0.0f;
    }
    return sg_make_buffer(&(sg_buffer_desc){
        .data = { .ptr = s_full_scratch,
                  .size = (size_t)s_unit_v_count * sizeof(sphere_full_vertex_t) },
        .label = label,
    });
}

/* Atmosphere shell — same simple sphere geometry, scaled outward by
 * outerRadiusMul. Per-vertex data isn't read by atmosphere.glsl
 * (only aPos), but we reuse the sphere_full_vertex_t layout so the
 * shared simple ibuf can index into it. */
static sg_buffer build_atmosphere_vbuf(float outer_mul, const char *label)
{
    for (int i = 0; i < s_unit_v_count; i++) {
        s_full_scratch[i].pos        = HMM_MulV3F(s_unit_verts[i].pos, outer_mul);
        s_full_scratch[i].normal     = s_unit_verts[i].normal;
        s_full_scratch[i].color      = (HMM_Vec3){ .Elements = { 0, 0, 0 } };
        s_full_scratch[i].brightness = 0.0f;
    }
    return sg_make_buffer(&(sg_buffer_desc){
        .data = { .ptr = s_full_scratch,
                  .size = (size_t)s_unit_v_count * sizeof(sphere_full_vertex_t) },
        .label = label,
    });
}

/* Smooth water shell at sea level (top of level-0 cells = level 1's
 * height). Same shared ibuf as the simple sphere; vbuf positions
 * scaled inward by sea_level_unit. Single uniform colour from the
 * planet's `water.fogColor`. */
static sg_buffer build_water_vbuf(HMM_Vec3 color, float sea_level_unit, const char *label)
{
    for (int i = 0; i < s_unit_v_count; i++) {
        HMM_Vec3 p = HMM_MulV3F(s_unit_verts[i].pos, sea_level_unit);
        s_full_scratch[i].pos        = p;
        s_full_scratch[i].normal     = s_unit_verts[i].normal;
        s_full_scratch[i].color      = color;
        s_full_scratch[i].brightness = 1.0f;
    }
    return sg_make_buffer(&(sg_buffer_desc){
        .data = { .ptr = s_full_scratch,
                  .size = (size_t)s_unit_v_count * sizeof(sphere_full_vertex_t) },
        .label = label,
    });
}

/* Pick a biome index for a unit-sphere position by sampling fbm noise
 * and walking the threshold table. Mirrors the per-cell logic the
 * upstream's GenerateMesh.cs does for its icosahedron cells, just on
 * UV-sphere vertices instead. The C-side fbm in src/core/noise.c is
 * algorithmically identical to the GLSL one in sun.glsl. */
static int planet_biome_index(HMM_Vec3 unit_pos, const planet_full_config_t *p)
{
    /* Seed offsets the noise lattice so each planet samples a
     * different region without requiring a different hash function. */
    float seed_off = (float)p->noise_seed * 0.13f;
    HMM_Vec3 q = {
        .Elements = {
            unit_pos.X * p->noise_frequency + seed_off,
            unit_pos.Y * p->noise_frequency + seed_off * 1.7f,
            unit_pos.Z * p->noise_frequency + seed_off * 2.3f,
        }
    };
    float n = noise_fbm3(q, 4);

    int biome = 0;
    for (int i = 0; i < p->noise_threshold_count; i++) {
        if (n >= p->noise_thresholds[i]) biome = i + 1;
    }
    if (biome >= p->level_count && p->level_count > 0) {
        biome = p->level_count - 1;
    }
    return biome;
}

/* Goldberg cell-stepped biome bake. Walks the precomputed Goldberg
 * cells (each cell is a hex or penta polygon on the unit sphere),
 * samples noise at the cell centre, picks a biome, and fan-emits
 * triangles around the cell with the biome's colour and the
 * biome-driven cliff drop applied to *all* the cell's verts. Adjacent
 * cells of different biomes don't share verts, so colour boundaries
 * are crisp and the radial drop creates a visible cliff at the seam.
 *
 *   step_unit = step_height / planet_radius
 *   drop      = (max_level - biome) * step_unit
 *   r         = 1.0 - drop
 *
 * The fan-triangle layout is (centre, corner[i], corner[(i+1) mod N])
 * so each cell contributes N triangles — 5 for pentagons, 6 for
 * hexagons. Triangle winding is CCW from outside the sphere because
 * the corners themselves are ordered CCW around the centre's outward
 * normal. */
/* Replicates upstream PlanetMesh.cs's EmitCellGeometry exactly (modulo
 * chamfer + slopes — those are a follow-up). Every cell sits at
 * absolute heights anchored to the planet surface (radius 1.0 in
 * body-local space), not radial drops:
 *
 *   level_h(0) = 1.0 + 0.75 * step_unit   // water surface
 *   level_h(k) = 1.0 + k    * step_unit   // for k > 0
 *
 * Level-0 cells are special: they emit a *seabed fan* at radius 1.0
 * (the planet base), NOT at level_h(0). For ocean planets the
 * separate water shell mesh sits above them at level_h(0) so the
 * water visually covers the seabed; for non-ocean planets level-0
 * cells just read as recessed canyons. Walls from a land cell to a
 * level-0 neighbour drop to the seabed (1.0), not to level_h(0).
 *
 * Walls are double-sided (4 triangles per wall) so they're visible
 * from either face — matches upstream's two-sided index winding and
 * means the slope/chamfer follow-ups don't need special-casing for
 * culling direction.
 *
 * Suppression: at each shared edge, only the higher of the two
 * cells emits the wall (the lower skips). For cells of equal level,
 * neither emits anything — adjacent same-level tops meet flush. */
static sg_buffer build_planet_goldberg_vbuf(const planet_full_config_t *p, const char *label,
                                             int *out_index_count)
{
    int v_out     = 0;
    float step_unit = (p->radius > 0.0f && p->step_height > 0.0f)
        ? (p->step_height / p->radius) : 0.04f;

    /* Convert biome index → absolute radial distance (in unit-sphere
     * space). Mirrors PlanetMesh.LevelH(byte). */
    #define LEVEL_H(level) ((level) == 0 ? (1.0f + 0.75f * step_unit) \
                                         : (1.0f + (float)(level) * step_unit))
    /* Wall to a level-0 neighbour from a land cell drops to the
     * seabed, which sits at the planet base (radius 1.0). */
    const float SEABED_H = 1.0f;

    /* Pre-pass: classify every cell. */
    static int cell_level[GOLDBERG_MAX_CELLS];
    for (int c = 0; c < s_goldberg_cell_count; c++) {
        cell_level[c] = planet_biome_index(s_goldberg_cells[c].center, p);
    }

    /* Pass 1 — cell tops + level-0 seabeds. */
    for (int c = 0; c < s_goldberg_cell_count; c++) {
        const goldberg_cell_t *cell = &s_goldberg_cells[c];
        int      level = cell_level[c];
        HMM_Vec3 col   = (p->level_count > 0 && level < p->level_count)
            ? p->levels[level].color
            : (HMM_Vec3){ .Elements = { 1, 1, 1 } };

        /* Level-0 cells emit only a seabed fan at radius 1.0 (no top
         * fan at the water surface — that's the separate water
         * mesh's job for ocean planets, and recessed canyons read
         * correctly for non-ocean). Other levels emit their top fan
         * at level_h(level). */
        float    h   = (level == 0) ? SEABED_H : LEVEL_H(level);
        HMM_Vec3 ctr = HMM_MulV3F(cell->center, h);

        for (int k = 0; k < cell->corner_count; k++) {
            HMM_Vec3 u_k0 = cell->corners[k];
            HMM_Vec3 u_k1 = cell->corners[(k + 1) % cell->corner_count];
            HMM_Vec3 c0   = HMM_MulV3F(u_k0, h);
            HMM_Vec3 c1   = HMM_MulV3F(u_k1, h);

            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ ctr, cell->center, col, 1.0f };
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ c0,  u_k0,         col, 1.0f };
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ c1,  u_k1,         col, 1.0f };
        }
    }

    /* Pass 2 — cliff walls. Match upstream's wall emission:
     *   - wall to water from land drops to seabed
     *   - wall double-sided (front + back triangles)
     *   - lower cell skips (only higher emits) */
    for (int c = 0; c < s_goldberg_cell_count; c++) {
        const goldberg_cell_t *cell = &s_goldberg_cells[c];
        int      level   = cell_level[c];
        HMM_Vec3 col_me  = (p->level_count > 0 && level < p->level_count)
            ? p->levels[level].color
            : (HMM_Vec3){ .Elements = { 1, 1, 1 } };

        float my_top = (level == 0) ? SEABED_H : LEVEL_H(level);

        for (int k = 0; k < cell->corner_count; k++) {
            int n = cell->neighbors[k];
            if (n < 0 || n >= s_goldberg_cell_count) continue;
            int n_level = cell_level[n];

            /* Wall-to-water-from-land special case (upstream:
             *   nh = (level != 0 && nLevel == 0) ? Radius : LevelH(nLevel) ) */
            float n_top;
            if (level != 0 && n_level == 0) {
                n_top = SEABED_H;
            } else {
                n_top = (n_level == 0) ? SEABED_H : LEVEL_H(n_level);
            }

            /* Suppression: skip if I'm not strictly higher. */
            if (my_top <= n_top + 1e-5f) continue;

            HMM_Vec3 u_a = cell->corners[k];
            HMM_Vec3 u_b = cell->corners[(k + 1) % cell->corner_count];

            HMM_Vec3 top_a = HMM_MulV3F(u_a, my_top);
            HMM_Vec3 top_b = HMM_MulV3F(u_b, my_top);
            HMM_Vec3 bot_a = HMM_MulV3F(u_a, n_top);
            HMM_Vec3 bot_b = HMM_MulV3F(u_b, n_top);

            HMM_Vec3 wall_n = HMM_NormV3(HMM_AddV3(u_a, u_b));

            /* Double-sided: front winding + back winding so the wall
             * renders correctly from either face. Matches upstream's
             * two-sided wall winding (4 indices × 2 sides = 12). */
            /* Front: top_a → top_b → bot_b, top_a → bot_b → bot_a */
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ top_a, wall_n, col_me, 1.0f };
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ top_b, wall_n, col_me, 1.0f };
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ bot_b, wall_n, col_me, 1.0f };

            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ top_a, wall_n, col_me, 1.0f };
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ bot_b, wall_n, col_me, 1.0f };
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ bot_a, wall_n, col_me, 1.0f };

            /* Back winding (verts go opposite order so face culled-
             * away triangles still show up from the inside): */
            HMM_Vec3 nback = HMM_MulV3F(wall_n, -1.0f);
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ top_a, nback, col_me, 1.0f };
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ bot_b, nback, col_me, 1.0f };
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ top_b, nback, col_me, 1.0f };

            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ top_a, nback, col_me, 1.0f };
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ bot_a, nback, col_me, 1.0f };
            s_biome_scratch[v_out++] = (sphere_full_vertex_t){ bot_b, nback, col_me, 1.0f };

            if (v_out + 12 > BIOME_VBUF_MAX) {
                LOG_ERROR("solarsystem: biome scratch overflow on planet %s", p->name);
                goto done;
            }
        }
    }

done:
    if (out_index_count) *out_index_count = v_out;
    return sg_make_buffer(&(sg_buffer_desc){
        .data  = { .ptr = s_biome_scratch,
                   .size = (size_t)v_out * sizeof(sphere_full_vertex_t) },
        .label = label,
    });

    #undef LEVEL_H
}

/* ---- starfield ---- */

/* Fullscreen triangle in NDC. The starfield shader uses aPos.xy as
 * NDC coordinates directly (gl_Position = vec4(aPos.xy, 0.99999, 1))
 * so the triangle just needs to cover the [-1,1] viewport — extend
 * two corners beyond to avoid edge artifacts. Matches the upstream
 * starfield.glsl's fullscreen-triangle convention. */
static void build_starfield(void)
{
    static const HMM_Vec3 verts[3] = {
        { .Elements = { -1.0f, -1.0f, 0.0f } },
        { .Elements = {  3.0f, -1.0f, 0.0f } },
        { .Elements = { -1.0f,  3.0f, 0.0f } },
    };
    state.starfield_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .data  = { .ptr = verts, .size = sizeof(verts) },
        .label = "starfield-vbuf",
    });
}

/* ---- geometry ---- */

static void build_geometry(void)
{
    if (!sphere_make_uv(SPHERE_STACKS, SPHERE_SLICES,
                        s_unit_verts, SPHERE_MAX_VERTS, &s_unit_v_count,
                        s_unit_indices, SPHERE_MAX_INDICES, &s_unit_i_count)) {
        LOG_ERROR("solarsystem: sphere mesh did not fit in static buffers");
        return;
    }

    state.ibuf = sg_make_buffer(&(sg_buffer_desc){
        .usage = { .index_buffer = true },
        .data  = { .ptr = s_unit_indices,
                   .size = (size_t)s_unit_i_count * sizeof(uint16_t) },
        .label = "sphere-ibuf",
    });
    state.index_count = s_unit_i_count;

    /* Goldberg hex sphere — replaces the UV sphere for biome planets.
     * Built once at level 3 (642 cells); promote to a per-planet
     * subdivision when we want Earth coarser than Glacius, etc. */
    if (!goldberg_make(3, s_goldberg_cells, GOLDBERG_MAX_CELLS, &s_goldberg_cell_count)) {
        LOG_ERROR("solarsystem: goldberg mesh did not fit in static buffers");
        s_goldberg_cell_count = 0;
    }

    /* Sequential ibuf sized for the worst-case biome vbuf (cell tops
     * + cliff walls). Each planet's actual draw_count is whatever
     * its biome bake produced, always <= BIOME_VBUF_MAX. */
    for (int i = 0; i < BIOME_VBUF_MAX; i++) {
        s_biome_indices[i] = (uint16_t)i;
    }
    state.ibuf_biome = sg_make_buffer(&(sg_buffer_desc){
        .usage = { .index_buffer = true },
        .data  = { .ptr = s_biome_indices,
                   .size = (size_t)BIOME_VBUF_MAX * sizeof(uint16_t) },
        .label = "sphere-biome-ibuf",
    });
    s_biome_index_count = BIOME_VBUF_MAX;

    /* Unit-radius circle in the xz plane, line-strip with the first
     * vertex repeated at the end so we get a closed loop. Segment
     * count from engine.yaml, clamped to the static buffer cap. */
    int seg = state.eng->solar_system_view.orbit_ring_segments;
    if (seg < 8)                       seg = 8;
    if (seg > ORBIT_RING_MAX_SEGMENTS) seg = ORBIT_RING_MAX_SEGMENTS;
    state.orbit_segments = seg;

    static HMM_Vec3 ring_verts[ORBIT_RING_MAX_SEGMENTS + 1];
    for (int i = 0; i <= seg; i++) {
        float ang = (float)i / (float)seg * 2.0f * HMM_PI;
        ring_verts[i] = (HMM_Vec3){ .Elements = { cosf(ang), 0.0f, sinf(ang) } };
    }
    state.orbit_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .data  = { .ptr = ring_verts, .size = (size_t)(seg + 1) * sizeof(HMM_Vec3) },
        .label = "orbit-vbuf",
    });
}

static void build_bodies(void)
{
    state.body_count = 0;

    /* index 0 = sun */
    {
        body_entry_t *b = &state.bodies[state.body_count++];
        b->kind         = BODY_SUN;
        snprintf(b->name, sizeof(b->name), "%s",
                 state.cfg->sun.name[0] ? state.cfg->sun.name : "Sol");
        b->base_color   = state.cfg->sun.color;
        b->radius       = state.cfg->sun.radius;
        b->orbit_radius = 0.0f;
        b->orbit_speed  = 0.0f;
        b->phase        = 0.0f;
        b->parent_index = -1;
        b->zoom_min     = 0.0f;
        b->zoom_max     = 0.0f;
        b->vbuf       = build_body_vbuf(state.cfg->sun.color, 1.0f, "sun-vbuf");
        b->ibuf       = state.ibuf;
        b->draw_count = state.index_count;
    }

    for (int i = 0; i < state.cfg->planet_count && state.body_count < MAX_BODIES; i++) {
        const planet_config_t      *pl   = &state.cfg->planets[i];
        const planet_full_config_t *full = (i < state.planet_full_count)
            ? &state.planet_full[i] : NULL;

        int planet_body_idx = state.body_count;
        {
            body_entry_t *b = &state.bodies[state.body_count++];
            b->kind         = BODY_PLANET;
            snprintf(b->name, sizeof(b->name), "%s", pl->self.name);
            b->base_color   = pl->self.color;
            b->radius       = pl->self.display_radius;
            b->orbit_radius = pl->self.orbit_radius;
            b->orbit_speed  = pl->self.orbit_speed;
            b->phase        = pl->self.phase;
            b->parent_index = -1;
            b->zoom_min     = pl->self.zoom_min;
            b->zoom_max     = pl->self.zoom_max;
            /* If the per-planet YAML loaded successfully *and* the
             * Goldberg mesh built, use cell-stepped colour. Otherwise
             * fall back to the smooth UV-sphere with the planet's
             * flat colour. */
            bool biome_path = (full && full->level_count > 0
                                    && full->noise_threshold_count > 0
                                    && s_goldberg_cell_count > 0);
            if (biome_path) {
                int gidx = 0;
                b->vbuf       = build_planet_goldberg_vbuf(full, pl->self.name, &gidx);
                b->ibuf       = state.ibuf_biome;
                b->draw_count = gidx;
            } else {
                b->vbuf       = build_body_vbuf(pl->self.color, 1.0f, pl->self.name);
                b->ibuf       = state.ibuf;
                b->draw_count = state.index_count;
            }

            /* Water shell — only for ocean planets. Sits at upstream's
             * LevelH(0) = R + 0.75 * StepHeight (the water surface,
             * which is above the seabed at R = unit-sphere radius 1.0
             * but below the level-1 sand cells at 1.0 + 1*step_unit). */
            if (biome_path && full->ocean_level0 && full->has_water
                && full->radius > 0.0f && full->step_height > 0.0f
                && full->level_count >= 2)
            {
                float step_unit       = full->step_height / full->radius;
                float sea_level_unit  = 1.0f + 0.75f * step_unit;
                b->water_vbuf = build_water_vbuf(full->water_color, sea_level_unit,
                                                 pl->self.name);
                b->has_water  = true;
            }

            /* Atmosphere shell — any planet with an atmosphere section.
             * The Nishita shader picks colour from Rayleigh
             * wavelengths × sunIntensity, so we just plumb the YAML
             * knobs through. Sun intensity falls back to upstream's
             * 30.0 default if the YAML omits it.
             *
             * The YAML's outerRadiusMul (1.5 for Earth) leaves a big
             * visible shell gap between the planet's surface (radius
             * 1.0) and the bright atmospheric limb (radius 1.5). We
             * shrink the *thickness* of the atmosphere by 0.4 so the
             * limb hugs the planet, while keeping the YAML pristine.
             *   visible_outer = 1.0 + (yaml_outer - 1.0) * 0.4 */
            if (biome_path && full->has_atmosphere && full->atmosphere_outer_mul > 1.0f) {
                const float ATMO_THICKNESS_SCALE = 0.4f;
                float visible_outer = 1.0f + (full->atmosphere_outer_mul - 1.0f) * ATMO_THICKNESS_SCALE;
                b->atmo_vbuf          = build_atmosphere_vbuf(visible_outer, pl->self.name);
                b->atmo_outer_mul     = visible_outer;
                b->atmo_sun_intensity = (full->atmosphere_sun_intensity > 0.0f)
                    ? full->atmosphere_sun_intensity : 30.0f;
                b->has_atmosphere     = true;
            }

            /* Cloud layers — every planet with an atmosphere gets some
             * variant of weather. Per-planet colour + density makes
             * the four worlds visually distinct from a distance:
             *   Earth  — white, full coverage
             *   Glacius — pale blue, icy
             *   Venus  — yellowish, thick / opaque
             *   Mars   — dusty red, thin / sparse
             * If the planet doesn't have atmosphere, no clouds. */
            if (biome_path && full->has_atmosphere) {
                /* Default values — most planets are white with full
                 * coverage. Per-planet overrides below shape the
                 * look (icy/yellow/dust). Thresholds dropped to 0.30
                 * so clouds are unambiguously visible (mean fbm sample
                 * is ~0.47, so ~70% of cells now have alpha>0). If you
                 * see clouds you don't want, narrow the threshold;
                 * if you see none, that's a real bug. */
                HMM_Vec3 lo_color = (HMM_Vec3){ .Elements = { 1.00f, 1.00f, 1.00f } };
                HMM_Vec3 hi_color = (HMM_Vec3){ .Elements = { 1.00f, 1.00f, 1.00f } };
                float    lo_alpha = 0.85f;
                float    hi_alpha = 0.45f;
                float    lo_thresh = 0.30f;
                float    hi_thresh = 0.35f;
                /* Cheap per-planet identification by biome palette
                 * (avoids adding another YAML field for now). The
                 * lookup is by name match in the body entry. */
                if (strcmp(b->name, "Glacius") == 0) {
                    lo_color = (HMM_Vec3){ .Elements = { 0.90f, 0.95f, 1.00f } };
                    hi_color = (HMM_Vec3){ .Elements = { 0.85f, 0.90f, 1.00f } };
                    lo_alpha = 0.70f; hi_alpha = 0.30f;
                } else if (strcmp(b->name, "Venus") == 0) {
                    lo_color = (HMM_Vec3){ .Elements = { 0.95f, 0.85f, 0.55f } };
                    hi_color = (HMM_Vec3){ .Elements = { 0.85f, 0.70f, 0.40f } };
                    lo_alpha = 0.95f; hi_alpha = 0.65f;
                    lo_thresh = 0.35f; hi_thresh = 0.40f;
                } else if (strcmp(b->name, "Mars") == 0) {
                    lo_color = (HMM_Vec3){ .Elements = { 0.85f, 0.55f, 0.40f } };
                    hi_color = (HMM_Vec3){ .Elements = { 0.75f, 0.50f, 0.40f } };
                    lo_alpha = 0.55f; hi_alpha = 0.30f;
                    lo_thresh = 0.45f; hi_thresh = 0.50f;
                }
                /* Cloud altitudes sit just above the highest biome
                 * peak so they don't clip into snow caps. With
                 * upstream's absolute LevelH heights the planet
                 * surface goes up to 1.0 + max_level * step_unit;
                 * clouds float a bit above that. */
                float c_step = (full->radius > 0.0f && full->step_height > 0.0f)
                    ? (full->step_height / full->radius) : 0.04f;
                int   c_max  = (full->level_count > 0) ? (full->level_count - 1) : 5;
                float biome_top  = 1.0f + (float)c_max * c_step;
                float cloud_lo_r = biome_top + 0.5f * c_step;
                float cloud_hi_r = biome_top + 2.0f * c_step;

                b->cloud_vbuf[0]   = build_cloud_vbuf(cloud_lo_r, "clouds-low");
                b->cloud_color[0]  = (HMM_Vec4){ .Elements = { lo_color.X, lo_color.Y, lo_color.Z, lo_alpha } };
                b->cloud_params[0] = (HMM_Vec4){ .Elements = { 0.020f, 4.0f, lo_thresh, 4.0f } };
                b->cloud_vbuf[1]   = build_cloud_vbuf(cloud_hi_r, "clouds-high");
                b->cloud_color[1]  = (HMM_Vec4){ .Elements = { hi_color.X, hi_color.Y, hi_color.Z, hi_alpha } };
                b->cloud_params[1] = (HMM_Vec4){ .Elements = { 0.040f, 9.0f, hi_thresh, 3.0f } };
                b->cloud_layers    = 2;
            }
        }
        for (int j = 0; j < pl->moon_count && state.body_count < MAX_BODIES; j++) {
            const body_config_t *m = &pl->moons[j];
            body_entry_t *b = &state.bodies[state.body_count++];
            b->kind         = BODY_MOON;
            snprintf(b->name, sizeof(b->name), "%s", m->name);
            b->base_color   = m->color;
            b->radius       = m->display_radius;
            b->orbit_radius = m->orbit_radius;
            b->orbit_speed  = m->orbit_speed;
            b->phase        = m->phase;
            b->parent_index = planet_body_idx;
            b->zoom_min     = m->zoom_min;
            b->zoom_max     = m->zoom_max;
            b->vbuf         = build_body_vbuf(m->color, 1.0f, m->name);
            b->ibuf         = state.ibuf;
            b->draw_count   = state.index_count;
        }
    }
}

static void build_pipelines(void)
{
    sg_backend backend = sg_query_backend();

    state.sun_shd = sg_make_shader(sun_sun_shader_desc(backend));
    state.sun_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = state.sun_shd,
        .layout = {
            .buffers[0] = { .stride = sizeof(sphere_full_vertex_t) },
            .attrs = {
                [ATTR_sun_sun_aPos] = {
                    .offset = offsetof(sphere_full_vertex_t, pos),
                    .format = SG_VERTEXFORMAT_FLOAT3,
                },
            },
        },
        .index_type   = SG_INDEXTYPE_UINT16,
        .cull_mode    = SG_CULLMODE_BACK,
        .face_winding = SG_FACEWINDING_CCW,
        .depth        = { .compare = SG_COMPAREFUNC_LESS_EQUAL, .write_enabled = true },
        .label        = "sun-pipeline",
    });

    state.orbit_shd = sg_make_shader(orbit_orbit_shader_desc(backend));
    state.orbit_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = state.orbit_shd,
        .layout = {
            .buffers[0] = { .stride = sizeof(HMM_Vec3) },
            .attrs = {
                [ATTR_orbit_orbit_aPos] = { .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 },
            },
        },
        .primitive_type = SG_PRIMITIVETYPE_LINE_STRIP,
        .colors[0].blend = {
            .enabled          = true,
            .src_factor_rgb   = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb   = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        },
        /* Test against existing depth so rings are occluded behind
         * planets, but don't write — keeps subsequent transparent
         * primitives from depth-fighting. */
        .depth = { .compare = SG_COMPAREFUNC_LESS_EQUAL, .write_enabled = false },
        .label = "orbit-pipeline",
    });

    state.starfield_shd = sg_make_shader(starfield_starfield_shader_desc(backend));
    state.starfield_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = state.starfield_shd,
        .layout = {
            .buffers[0] = { .stride = sizeof(HMM_Vec3) },
            .attrs = {
                [ATTR_starfield_starfield_aPos] = {
                    .offset = 0,
                    .format = SG_VERTEXFORMAT_FLOAT3,
                },
            },
        },
        .primitive_type = SG_PRIMITIVETYPE_TRIANGLES,
        /* Fullscreen triangle at z=0.99999 (just shy of the far
         * plane) so the rest of the scene depth-tests over it.
         * Depth compare ALWAYS / write off keeps the starfield
         * out of the way of subsequent draws. */
        .depth = { .compare = SG_COMPAREFUNC_ALWAYS, .write_enabled = false },
        .label = "starfield-pipeline",
    });

    state.cloud_shd = sg_make_shader(clouds_clouds_shader_desc(backend));
    state.cloud_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = state.cloud_shd,
        .layout = {
            .buffers[0] = { .stride = sizeof(sphere_full_vertex_t) },
            .attrs = {
                [ATTR_clouds_clouds_aPos] = {
                    .offset = offsetof(sphere_full_vertex_t, pos),
                    .format = SG_VERTEXFORMAT_FLOAT3,
                },
            },
        },
        .index_type   = SG_INDEXTYPE_UINT16,
        /* Standard back-cull on the cloud shell (we view its outside).
         * Alpha-blended over terrain + water; depth-test on, depth-
         * write off so the second layer can composite over the
         * first without z-fighting. */
        .cull_mode    = SG_CULLMODE_BACK,
        .face_winding = SG_FACEWINDING_CCW,
        .colors[0].blend = {
            .enabled          = true,
            .src_factor_rgb   = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb   = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        },
        .depth = { .compare = SG_COMPAREFUNC_LESS_EQUAL, .write_enabled = false },
        .label = "clouds-pipeline",
    });

    state.atmo_shd = sg_make_shader(atmosphere_atmosphere_shader_desc(backend));
    state.atmo_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = state.atmo_shd,
        .layout = {
            .buffers[0] = { .stride = sizeof(sphere_full_vertex_t) },
            .attrs = {
                [ATTR_atmosphere_atmosphere_aPos] = {
                    .offset = offsetof(sphere_full_vertex_t, pos),
                    .format = SG_VERTEXFORMAT_FLOAT3,
                },
            },
        },
        .index_type   = SG_INDEXTYPE_UINT16,
        /* Render the inside of the shell sphere from the outside —
         * front-face culling keeps just the back-facing hemisphere
         * (the rim glow). */
        .cull_mode    = SG_CULLMODE_FRONT,
        .face_winding = SG_FACEWINDING_CCW,
        .colors[0].blend = {
            .enabled          = true,
            .src_factor_rgb   = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb   = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        },
        .depth = { .compare = SG_COMPAREFUNC_LESS_EQUAL, .write_enabled = false },
        .label = "atmosphere-pipeline",
    });

    state.ss_shd = sg_make_shader(solarsystem_solarsystem_shader_desc(backend));
    state.ss_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = state.ss_shd,
        .layout = {
            .buffers[0] = { .stride = sizeof(sphere_full_vertex_t) },
            .attrs = {
                [ATTR_solarsystem_solarsystem_aPos] = {
                    .offset = offsetof(sphere_full_vertex_t, pos),        .format = SG_VERTEXFORMAT_FLOAT3 },
                [ATTR_solarsystem_solarsystem_aNormal] = {
                    .offset = offsetof(sphere_full_vertex_t, normal),     .format = SG_VERTEXFORMAT_FLOAT3 },
                [ATTR_solarsystem_solarsystem_aColor] = {
                    .offset = offsetof(sphere_full_vertex_t, color),      .format = SG_VERTEXFORMAT_FLOAT3 },
                [ATTR_solarsystem_solarsystem_aBrightness] = {
                    .offset = offsetof(sphere_full_vertex_t, brightness), .format = SG_VERTEXFORMAT_FLOAT  },
            },
        },
        .index_type   = SG_INDEXTYPE_UINT16,
        .cull_mode    = SG_CULLMODE_BACK,
        .face_winding = SG_FACEWINDING_CCW,
        .depth        = { .compare = SG_COMPAREFUNC_LESS_EQUAL, .write_enabled = true },
        .label        = "solarsystem-pipeline",
    });
}

void solarsystem_init(const solarsystem_config_t  *cfg,
                      const engine_config_t       *eng,
                      const planet_full_config_t  *planet_full,
                      int                          planet_full_count)
{
    state.cfg                = cfg;
    state.eng                = eng;
    state.planet_full        = planet_full;
    state.planet_full_count  = planet_full_count;
    state.sim_time       = 0.0;
    state.transition_dur = eng->camera.transition_duration;
    state.active_body    = 0;       /* 0 = sun mode */
    state.transitioning  = false;
    build_geometry();
    build_starfield();
    build_bodies();
    build_pipelines();
    state.inited = true;
}

sg_pass_action solarsystem_pass_action(void)
{
    return (sg_pass_action){
        .colors[0] = {
            .load_action = SG_LOADACTION_CLEAR,
            .clear_value = { 0.012f, 0.018f, 0.035f, 1.0f },
        },
        .depth = {
            .load_action = SG_LOADACTION_CLEAR,
            .clear_value = 1.0f,
        },
    };
}

/* ---- frame ---- */

static HMM_Vec3 body_world_pos(const body_entry_t *b, double t)
{
    if (b->orbit_radius <= 0.0f) return (HMM_Vec3){ .Elements = { 0, 0, 0 } };
    float ang = b->phase + b->orbit_speed * (float)t * ENGINE_TIME_SCALE;
    return (HMM_Vec3){ .Elements = {
        cosf(ang) * b->orbit_radius,
        0.0f,
        sinf(ang) * b->orbit_radius,
    }};
}

void solarsystem_frame(double dt, int fb_width, int fb_height, const camera_t *cam)
{
    if (!state.inited || !state.cfg) return;
    state.sim_time += dt;

    float    aspect   = (fb_height > 0) ? (float)fb_width / (float)fb_height : 1.0f;
    HMM_Mat4 proj     = camera_proj(cam, aspect);
    HMM_Mat4 view     = camera_view(cam);
    HMM_Mat4 view_proj = HMM_MulM4(proj, view);
    HMM_Vec3 cam_pos  = camera_eye(cam);

    /* Resolve world positions once so moons can read their parents. */
    HMM_Vec3 world_pos[MAX_BODIES];
    resolve_world_positions(world_pos);

    /* Starfield first — fullscreen triangle that reconstructs the
     * per-fragment ray direction from the camera basis (right/up/
     * forward) + NDC. The upstream shader's voronoi-based stars and
     * fbm nebula need the camera basis in world space and the
     * field-of-view tan/aspect to compute view rays. */
    {
        HMM_Vec3 eye      = camera_eye(cam);
        HMM_Vec3 cam_fwd  = HMM_NormV3(HMM_SubV3(cam->focus_target, eye));
        HMM_Vec3 world_up = { .Elements = { 0.0f, 1.0f, 0.0f } };
        HMM_Vec3 cam_rgt  = HMM_NormV3(HMM_Cross(cam_fwd, world_up));
        HMM_Vec3 cam_up   = HMM_Cross(cam_rgt, cam_fwd);

        float fov_y_rad  = cam->fov_y_deg * (HMM_PI / 180.0f);
        float tan_half   = tanf(fov_y_rad * 0.5f);

        starfield_star_fs_params_t sfsp = {
            .camRight   = { cam_rgt.X, cam_rgt.Y, cam_rgt.Z, 0.0f },
            .camUp      = { cam_up.X,  cam_up.Y,  cam_up.Z,  0.0f },
            .camForward = { cam_fwd.X, cam_fwd.Y, cam_fwd.Z, 0.0f },
            .params     = { tan_half, aspect, (float)state.sim_time, 0.0f },
        };

        sg_apply_pipeline(state.starfield_pip);
        sg_apply_bindings(&(sg_bindings){ .vertex_buffers[0] = state.starfield_vbuf });
        sg_apply_uniforms(UB_starfield_star_fs_params, &(sg_range){ &sfsp, sizeof(sfsp) });
        sg_draw(0, 3, 1);
    }

    /* Sun next. */
    {
        const body_entry_t *b = &state.bodies[0];
        HMM_Mat4 model = HMM_MulM4(HMM_Translate(world_pos[0]),
                                   HMM_Scale((HMM_Vec3){ .Elements = { b->radius, b->radius, b->radius } }));
        HMM_Mat4 mvp = HMM_MulM4(view_proj, model);

        sun_sun_vs_params_t vsp;
        memcpy(vsp.mvp, &mvp, sizeof(mvp));
        sun_sun_fs_params_t fsp = {
            .params = { (float)state.sim_time, 1.0f, 0.0f, 0.0f },
        };

        sg_apply_pipeline(state.sun_pip);
        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = b->vbuf,
            .index_buffer      = b->ibuf,
        });
        sg_apply_uniforms(UB_sun_sun_vs_params, &(sg_range){ &vsp, sizeof(vsp) });
        sg_apply_uniforms(UB_sun_sun_fs_params, &(sg_range){ &fsp, sizeof(fsp) });
        sg_draw(0, b->draw_count, 1);
    }

    /* Planets + moons share the solarsystem pipeline.
     * Capture the camera + view info once so the atmosphere pass
     * below can reuse it without re-resolving. */
    sg_apply_pipeline(state.ss_pip);

    HMM_Vec3 cam_dir_w = HMM_NormV3(HMM_SubV3((HMM_Vec3){ .Elements = { 0, 0, 0 } }, cam_pos));

    for (int i = 1; i < state.body_count; i++) {
        const body_entry_t *b = &state.bodies[i];
        HMM_Vec3 wp     = world_pos[i];
        HMM_Mat4 model  = HMM_MulM4(HMM_Translate(wp),
                                    HMM_Scale((HMM_Vec3){ .Elements = { b->radius, b->radius, b->radius } }));
        HMM_Mat4 mvp    = HMM_MulM4(view_proj, model);
        /* Sun is at world origin; the planet shader's `sunDir` is the
         * direction *from the surface toward the sun* (standard
         * Lambert convention — `dot(N, L) > 0` on the lit hemisphere).
         * That's `-normalize(wp)`. Earlier this was passing `wp` and
         * lighting the *far* hemisphere by accident. */
        HMM_Vec3 sun_dir = HMM_NormV3(HMM_MulV3F(wp, -1.0f));

        solarsystem_ss_vs_params_t vsp;
        memcpy(vsp.mvp, &mvp, sizeof(mvp));
        solarsystem_ss_fs_params_t fsp = {
            .sunDir  = { sun_dir.X, sun_dir.Y, sun_dir.Z, 0.0f },
            /* dither = 0 disables LOD fade; will hook up with M1 step 4 zoom. */
            .viewDir = { cam_dir_w.X, cam_dir_w.Y, cam_dir_w.Z, 0.0f },
        };

        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = b->vbuf,
            .index_buffer      = b->ibuf,
        });
        sg_apply_uniforms(UB_solarsystem_ss_vs_params, &(sg_range){ &vsp, sizeof(vsp) });
        sg_apply_uniforms(UB_solarsystem_ss_fs_params, &(sg_range){ &fsp, sizeof(fsp) });
        sg_draw(0, b->draw_count, 1);

        /* Water shell — same shader, same uniforms (lighting from sun
         * works identically), just a different vbuf at sea level on
         * the simple shared ibuf. Depth-tested so it sits inside
         * land cells but covers the sea-floor cells. */
        if (b->has_water) {
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = b->water_vbuf,
                .index_buffer      = state.ibuf,
            });
            sg_apply_uniforms(UB_solarsystem_ss_vs_params, &(sg_range){ &vsp, sizeof(vsp) });
            sg_apply_uniforms(UB_solarsystem_ss_fs_params, &(sg_range){ &fsp, sizeof(fsp) });
            sg_draw(0, state.index_count, 1);
        }
    }

    /* Cloud pass — fbm shells per cloudy planet, between the opaque
     * terrain/water and the atmosphere shell so atmosphere haze
     * composites *over* the cloud tops. Two layers per planet, drawn
     * inner→outer so the higher layer alpha-blends over the lower. */
    sg_apply_pipeline(state.cloud_pip);
    for (int i = 1; i < state.body_count; i++) {
        const body_entry_t *b = &state.bodies[i];
        if (b->cloud_layers <= 0) continue;

        HMM_Vec3 wp     = world_pos[i];
        HMM_Mat4 model  = HMM_MulM4(
            HMM_Translate(wp),
            HMM_Scale((HMM_Vec3){ .Elements = { b->radius, b->radius, b->radius } }));
        HMM_Mat4 mvp    = HMM_MulM4(view_proj, model);
        HMM_Vec3 sun_local = HMM_NormV3(HMM_MulV3F(wp, -1.0f));

        for (int layer = 0; layer < b->cloud_layers; layer++) {
            float drift_speed = b->cloud_params[layer].X;
            float drift_time  = (float)state.sim_time * drift_speed;

            clouds_cloud_vs_params_t cvsp;
            memcpy(cvsp.mvp, &mvp, sizeof(mvp));
            clouds_cloud_fs_params_t cfsp = {
                .sunDir = { sun_local.X, sun_local.Y, sun_local.Z, 0.0f },
                /* Pack: (drift_time, noise_scale, threshold, bump). The
                 * X slot is the time the shader actually samples, with
                 * the per-layer drift speed already applied. */
                .params = {
                    drift_time,
                    b->cloud_params[layer].Y,
                    b->cloud_params[layer].Z,
                    b->cloud_params[layer].W,
                },
                .color  = {
                    b->cloud_color[layer].X, b->cloud_color[layer].Y,
                    b->cloud_color[layer].Z, b->cloud_color[layer].W,
                },
            };

            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = b->cloud_vbuf[layer],
                .index_buffer      = state.ibuf,
            });
            sg_apply_uniforms(UB_clouds_cloud_vs_params, &(sg_range){ &cvsp, sizeof(cvsp) });
            sg_apply_uniforms(UB_clouds_cloud_fs_params, &(sg_range){ &cfsp, sizeof(cfsp) });
            sg_draw(0, state.index_count, 1);
        }
    }

    /* Atmosphere pass — Nishita single-scatter on a back-rendered
     * shell. Drawn after every opaque planet + water but before the
     * orbit rings; the shader does its own ray-march in body-local
     * space, so we pass camera + sun in that frame and let the GPU
     * integrate per-fragment. */
    sg_apply_pipeline(state.atmo_pip);
    for (int i = 1; i < state.body_count; i++) {
        const body_entry_t *b = &state.bodies[i];
        if (!b->has_atmosphere) continue;

        HMM_Vec3 wp    = world_pos[i];
        HMM_Mat4 model = HMM_MulM4(
            HMM_Translate(wp),
            HMM_Scale((HMM_Vec3){ .Elements = { b->radius, b->radius, b->radius } }));
        HMM_Mat4 mvp   = HMM_MulM4(view_proj, model);

        /* Body-local space: model is translate(wp) * scale(radius), so
         *   inverse(model) = scale(1/radius) * translate(-wp)
         * which puts the planet centre at the origin and the surface
         * at radius 1.0. The atmosphere shell sits at atmo_outer_mul. */
        float    inv_r        = (b->radius > 0.0f) ? (1.0f / b->radius) : 1.0f;
        HMM_Vec3 cam_local    = HMM_MulV3F(HMM_SubV3(cam_pos, wp), inv_r);
        /* Sun is at world origin; body is at wp. Direction from a
         * point near the body toward the sun = -wp / |wp|. The same
         * vector in body-local space (no rotation in the model
         * matrix) — direction normalises away the scale. */
        HMM_Vec3 sun_local    = HMM_NormV3(HMM_MulV3F(wp, -1.0f));

        atmosphere_atmo_vs_params_t avsp;
        memcpy(avsp.mvp, &mvp, sizeof(mvp));
        atmosphere_atmo_fs_params_t afsp = {
            .sunDir    = { sun_local.X, sun_local.Y, sun_local.Z, 0.0f },
            .cameraPos = { cam_local.X, cam_local.Y, cam_local.Z, 0.0f },
            .params    = { 1.0f, b->atmo_outer_mul, b->atmo_sun_intensity, 0.0f },
        };

        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = b->atmo_vbuf,
            .index_buffer      = state.ibuf,
        });
        sg_apply_uniforms(UB_atmosphere_atmo_vs_params, &(sg_range){ &avsp, sizeof(avsp) });
        sg_apply_uniforms(UB_atmosphere_atmo_fs_params, &(sg_range){ &afsp, sizeof(afsp) });
        sg_draw(0, state.index_count, 1);
    }

    /* Orbit rings drawn after opaque bodies so transparency composites
     * correctly. Planet rings are centered at the sun (origin); moon
     * rings are centered at the parent planet's *current* world pos. */
    sg_apply_pipeline(state.orbit_pip);
    sg_apply_bindings(&(sg_bindings){ .vertex_buffers[0] = state.orbit_vbuf });

    /* Bright cool-blue, fully opaque against the dark background so
     * the rings are obvious for the visual sanity check. Tone down /
     * expose via engine.yaml later. GLES3 doesn't honour glLineWidth
     * past 1.0 reliably, so colour is the only knob we have here. */
    HMM_Vec4 ring_color = { .Elements = { 0.55f, 0.70f, 0.95f, 0.85f } };

    for (int i = 1; i < state.body_count; i++) {
        const body_entry_t *b = &state.bodies[i];
        if (b->orbit_radius <= 0.0f) continue;

        HMM_Vec3 center = (b->parent_index >= 0)
            ? world_pos[b->parent_index]
            : (HMM_Vec3){ .Elements = { 0, 0, 0 } };
        HMM_Mat4 model = HMM_MulM4(
            HMM_Translate(center),
            HMM_Scale((HMM_Vec3){ .Elements = { b->orbit_radius, b->orbit_radius, b->orbit_radius } }));
        HMM_Mat4 mvp   = HMM_MulM4(view_proj, model);

        orbit_orbit_vs_params_t vsp;
        memcpy(vsp.mvp, &mvp, sizeof(mvp));
        orbit_orbit_fs_params_t fsp;
        memcpy(fsp.color, &ring_color, sizeof(ring_color));

        sg_apply_uniforms(UB_orbit_orbit_vs_params, &(sg_range){ &vsp, sizeof(vsp) });
        sg_apply_uniforms(UB_orbit_orbit_fs_params, &(sg_range){ &fsp, sizeof(fsp) });
        sg_draw(0, state.orbit_segments + 1, 1);
    }
}

/* ---- click-to-zoom ---- */

static void start_transition_to(int body_idx, float to_distance, const camera_t *cam)
{
    state.active_body    = body_idx;
    state.transitioning  = true;
    state.transition_t   = 0.0f;
    state.from_focus     = cam->focus_target;
    state.from_distance  = cam->distance;
    state.to_distance    = to_distance;
}

void solarsystem_focus_sun(const camera_t *cam)
{
    if (!state.inited) return;
    if (state.active_body == 0 && !state.transitioning) return;
    start_transition_to(0, state.eng->solar_system_view.default_distance, cam);
}

bool solarsystem_pick(int sx, int sy, int fb_w, int fb_h, const camera_t *cam)
{
    if (!state.inited || fb_w <= 0 || fb_h <= 0) return false;

    /* Screen → NDC → world ray. */
    float ndc_x = 2.0f * (float)sx / (float)fb_w - 1.0f;
    float ndc_y = 1.0f - 2.0f * (float)sy / (float)fb_h;
    float aspect = (float)fb_w / (float)fb_h;
    HMM_Mat4 inv_vp = HMM_InvGeneralM4(HMM_MulM4(camera_proj(cam, aspect),
                                                  camera_view(cam)));
    HMM_Vec4 near_clip = { .Elements = { ndc_x, ndc_y, -1.0f, 1.0f } };
    HMM_Vec4 far_clip  = { .Elements = { ndc_x, ndc_y,  1.0f, 1.0f } };
    HMM_Vec4 nw = HMM_MulM4V4(inv_vp, near_clip);
    HMM_Vec4 fw = HMM_MulM4V4(inv_vp, far_clip);
    HMM_Vec3 near_pos = HMM_DivV3F((HMM_Vec3){ .Elements = { nw.X, nw.Y, nw.Z } }, nw.W);
    HMM_Vec3 far_pos  = HMM_DivV3F((HMM_Vec3){ .Elements = { fw.X, fw.Y, fw.Z } }, fw.W);
    HMM_Vec3 origin   = camera_eye(cam);
    HMM_Vec3 dir      = HMM_NormV3(HMM_SubV3(far_pos, near_pos));

    HMM_Vec3 world_pos[MAX_BODIES];
    resolve_world_positions(world_pos);

    /* Ray-sphere intersection per body. The pick radius is inflated
     * by engine.yaml solarSystemView.pickRadiusMultiplier so small
     * bodies remain tappable at the default zoom level. */
    const float pick_mult = state.eng->solar_system_view.pick_radius_multiplier;
    int   best_i = -1;
    float best_t = INFINITY;
    for (int i = 0; i < state.body_count; i++) {
        HMM_Vec3 oc = HMM_SubV3(origin, world_pos[i]);
        float r  = state.bodies[i].radius * pick_mult;
        float bc = HMM_DotV3(dir, oc);
        float cc = HMM_DotV3(oc, oc) - r * r;
        float disc = bc * bc - cc;
        if (disc < 0.0f) continue;
        float t = -bc - sqrtf(disc);
        if (t > 0.0f && t < best_t) {
            best_t = t;
            best_i = i;
        }
    }

    if (best_i < 0) {
        /* Tap on empty space while zoomed onto a planet → slide back
         * to the sun view. This is the touch equivalent of ESC since
         * mobile users have no keyboard (per CLAUDE.md). */
        if (state.active_body != 0) {
            solarsystem_focus_sun(cam);
            return true;
        }
        return false;
    }

    if (best_i == 0) {
        solarsystem_focus_sun(cam);
    } else {
        const body_entry_t *b = &state.bodies[best_i];
        /* Zoom-in distance: midpoint of the body's zoomMin/zoomMax
         * range from solarsystem.yaml (in radius units). Falls back
         * to 5x radius if the body didn't carry zoom limits. */
        float to_distance = b->radius * 5.0f;
        if (b->zoom_max > b->zoom_min && b->zoom_min > 0.0f) {
            to_distance = b->radius * 0.5f * (b->zoom_min + b->zoom_max);
        }
        start_transition_to(best_i, to_distance, cam);
    }
    return true;
}

void solarsystem_pre_frame(double dt, camera_t *cam)
{
    if (!state.inited) return;

    HMM_Vec3 world_pos[MAX_BODIES];
    resolve_world_positions(world_pos);

    HMM_Vec3 target_focus = world_pos[state.active_body];

    if (state.transitioning) {
        state.transition_t += (float)(dt / state.transition_dur);
        if (state.transition_t >= 1.0f) {
            state.transition_t   = 1.0f;
            state.transitioning  = false;
        }
        float t = state.transition_t;
        float s = t * t * (3.0f - 2.0f * t);   /* smoothstep */
        cam->focus_target = HMM_LerpV3(state.from_focus, s, target_focus);
        cam->distance     = state.from_distance + (state.to_distance - state.from_distance) * s;
        if (cam->distance < cam->dist_min) cam->distance = cam->dist_min;
        if (cam->distance > cam->dist_max) cam->distance = cam->dist_max;
    } else if (state.active_body != 0) {
        /* Lock-on follow — planet is still orbiting. */
        cam->focus_target = target_focus;
    }
}

const char *solarsystem_active_body_name(void)
{
    if (!state.inited || state.active_body < 0 || state.active_body >= state.body_count) {
        return "—";
    }
    return state.bodies[state.active_body].name;
}

bool solarsystem_is_transitioning(void)
{
    return state.transitioning;
}

void solarsystem_shutdown(void)
{
    if (!state.inited) return;
    sg_destroy_pipeline(state.orbit_pip);
    sg_destroy_pipeline(state.ss_pip);
    sg_destroy_pipeline(state.atmo_pip);
    sg_destroy_pipeline(state.cloud_pip);
    sg_destroy_pipeline(state.sun_pip);
    sg_destroy_pipeline(state.starfield_pip);
    sg_destroy_shader(state.orbit_shd);
    sg_destroy_shader(state.ss_shd);
    sg_destroy_shader(state.atmo_shd);
    sg_destroy_shader(state.cloud_shd);
    sg_destroy_shader(state.sun_shd);
    sg_destroy_shader(state.starfield_shd);
    sg_destroy_buffer(state.starfield_vbuf);
    for (int i = 0; i < state.body_count; i++) {
        sg_destroy_buffer(state.bodies[i].vbuf);
        if (state.bodies[i].has_water)      sg_destroy_buffer(state.bodies[i].water_vbuf);
        if (state.bodies[i].has_atmosphere) sg_destroy_buffer(state.bodies[i].atmo_vbuf);
        for (int c = 0; c < state.bodies[i].cloud_layers; c++) {
            sg_destroy_buffer(state.bodies[i].cloud_vbuf[c]);
        }
    }
    sg_destroy_buffer(state.orbit_vbuf);
    sg_destroy_buffer(state.ibuf_biome);
    sg_destroy_buffer(state.ibuf);
    state.inited = false;
}
