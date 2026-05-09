#include "solarsystem.h"

#include "core/log.h"
#include "core/noise.h"
#include "render/sphere.h"
#include "gen/sun.glsl.h"
#include "gen/solarsystem.glsl.h"
#include "gen/orbit.glsl.h"

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

static sg_buffer build_planet_biome_vbuf(const planet_full_config_t *p, const char *label)
{
    for (int i = 0; i < s_unit_v_count; i++) {
        HMM_Vec3 pos = s_unit_verts[i].pos;
        int biome    = planet_biome_index(pos, p);
        HMM_Vec3 col = (biome < p->level_count)
            ? p->levels[biome].color
            : (HMM_Vec3){ .Elements = { 1, 1, 1 } };

        s_full_scratch[i].pos        = pos;
        s_full_scratch[i].normal     = s_unit_verts[i].normal;
        s_full_scratch[i].color      = col;
        s_full_scratch[i].brightness = 1.0f;
    }
    return sg_make_buffer(&(sg_buffer_desc){
        .data  = { .ptr = s_full_scratch,
                   .size = (size_t)s_unit_v_count * sizeof(sphere_full_vertex_t) },
        .label = label,
    });
}

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
        b->vbuf = build_body_vbuf(state.cfg->sun.color, 1.0f, "sun-vbuf");
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
            /* If the per-planet YAML loaded successfully, use its
             * biome levels for per-vertex colour; otherwise fall back
             * to the flat colour from solarsystem.yaml. */
            b->vbuf = (full && full->level_count > 0
                            && full->noise_threshold_count > 0)
                ? build_planet_biome_vbuf(full, pl->self.name)
                : build_body_vbuf(pl->self.color, 1.0f, pl->self.name);
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
            b->vbuf = build_body_vbuf(m->color, 1.0f, m->name);
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

    /* Sun first. */
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
            .index_buffer      = state.ibuf,
        });
        sg_apply_uniforms(UB_sun_sun_vs_params, &(sg_range){ &vsp, sizeof(vsp) });
        sg_apply_uniforms(UB_sun_sun_fs_params, &(sg_range){ &fsp, sizeof(fsp) });
        sg_draw(0, state.index_count, 1);
    }

    /* Planets + moons share the solarsystem pipeline. */
    sg_apply_pipeline(state.ss_pip);

    HMM_Vec3 cam_dir_w = HMM_NormV3(HMM_SubV3((HMM_Vec3){ .Elements = { 0, 0, 0 } }, cam_pos));

    for (int i = 1; i < state.body_count; i++) {
        const body_entry_t *b = &state.bodies[i];
        HMM_Vec3 wp     = world_pos[i];
        HMM_Mat4 model  = HMM_MulM4(HMM_Translate(wp),
                                    HMM_Scale((HMM_Vec3){ .Elements = { b->radius, b->radius, b->radius } }));
        HMM_Mat4 mvp    = HMM_MulM4(view_proj, model);
        /* Sun is at origin; its light at this body points from origin
         * outward to the body, so the surface light direction is from
         * sun toward body = world_pos. The shader normalizes. */
        HMM_Vec3 sun_dir = wp;

        solarsystem_ss_vs_params_t vsp;
        memcpy(vsp.mvp, &mvp, sizeof(mvp));
        solarsystem_ss_fs_params_t fsp = {
            .sunDir  = { sun_dir.X, sun_dir.Y, sun_dir.Z, 0.0f },
            /* dither = 0 disables LOD fade; will hook up with M1 step 4 zoom. */
            .viewDir = { cam_dir_w.X, cam_dir_w.Y, cam_dir_w.Z, 0.0f },
        };

        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = b->vbuf,
            .index_buffer      = state.ibuf,
        });
        sg_apply_uniforms(UB_solarsystem_ss_vs_params, &(sg_range){ &vsp, sizeof(vsp) });
        sg_apply_uniforms(UB_solarsystem_ss_fs_params, &(sg_range){ &fsp, sizeof(fsp) });
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

    if (best_i < 0) return false;

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
    sg_destroy_pipeline(state.sun_pip);
    sg_destroy_shader(state.orbit_shd);
    sg_destroy_shader(state.ss_shd);
    sg_destroy_shader(state.sun_shd);
    for (int i = 0; i < state.body_count; i++) {
        sg_destroy_buffer(state.bodies[i].vbuf);
    }
    sg_destroy_buffer(state.orbit_vbuf);
    sg_destroy_buffer(state.ibuf);
    state.inited = false;
}
