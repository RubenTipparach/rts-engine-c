#include "solarsystem.h"

#include "core/log.h"
#include "render/sphere.h"
#include "gen/sun.glsl.h"
#include "gen/solarsystem.glsl.h"

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

#define ENGINE_TIME_SCALE   0.1f
#define SPHERE_STACKS       32
#define SPHERE_SLICES       48
#define SPHERE_MAX_VERTS    ((SPHERE_STACKS + 1) * (SPHERE_SLICES + 1))
#define SPHERE_MAX_INDICES  (SPHERE_STACKS * SPHERE_SLICES * 6)

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
    sg_buffer   vbuf;
    HMM_Vec3    base_color;          /* for log + future use */
    float       radius;              /* world-space draw radius */
    float       orbit_radius;
    float       orbit_speed;
    float       phase;
    int         parent_index;        /* -1 for sun/planets, planet index for moons */
} body_entry_t;

static struct {
    bool                       inited;
    const solarsystem_config_t *cfg;

    sg_buffer    ibuf;
    int          index_count;

    body_entry_t bodies[MAX_BODIES];
    int          body_count;

    sg_shader    sun_shd;
    sg_pipeline  sun_pip;
    sg_shader    ss_shd;
    sg_pipeline  ss_pip;

    double       sim_time;
} state;

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
}

static void build_bodies(void)
{
    state.body_count = 0;

    /* index 0 = sun */
    {
        body_entry_t *b = &state.bodies[state.body_count++];
        b->kind         = BODY_SUN;
        b->base_color   = state.cfg->sun.color;
        b->radius       = state.cfg->sun.radius;
        b->orbit_radius = 0.0f;
        b->orbit_speed  = 0.0f;
        b->phase        = 0.0f;
        b->parent_index = -1;
        b->vbuf = build_body_vbuf(state.cfg->sun.color, 1.0f, "sun-vbuf");
    }

    for (int i = 0; i < state.cfg->planet_count && state.body_count < MAX_BODIES; i++) {
        const planet_config_t *pl = &state.cfg->planets[i];
        int planet_body_idx = state.body_count;
        {
            body_entry_t *b = &state.bodies[state.body_count++];
            b->kind         = BODY_PLANET;
            b->base_color   = pl->self.color;
            b->radius       = pl->self.display_radius;
            b->orbit_radius = pl->self.orbit_radius;
            b->orbit_speed  = pl->self.orbit_speed;
            b->phase        = pl->self.phase;
            b->parent_index = -1;
            b->vbuf = build_body_vbuf(pl->self.color, 1.0f, pl->self.name);
        }
        for (int j = 0; j < pl->moon_count && state.body_count < MAX_BODIES; j++) {
            const body_config_t *m = &pl->moons[j];
            body_entry_t *b = &state.bodies[state.body_count++];
            b->kind         = BODY_MOON;
            b->base_color   = m->color;
            b->radius       = m->display_radius;
            b->orbit_radius = m->orbit_radius;
            b->orbit_speed  = m->orbit_speed;
            b->phase        = m->phase;
            b->parent_index = planet_body_idx;
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

void solarsystem_init(const solarsystem_config_t *cfg)
{
    state.cfg      = cfg;
    state.sim_time = 0.0;
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
    for (int i = 0; i < state.body_count; i++) {
        HMM_Vec3 local = body_world_pos(&state.bodies[i], state.sim_time);
        world_pos[i] = (state.bodies[i].parent_index >= 0)
            ? HMM_AddV3(world_pos[state.bodies[i].parent_index], local)
            : local;
    }

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
}

void solarsystem_shutdown(void)
{
    if (!state.inited) return;
    sg_destroy_pipeline(state.ss_pip);
    sg_destroy_pipeline(state.sun_pip);
    sg_destroy_shader(state.ss_shd);
    sg_destroy_shader(state.sun_shd);
    for (int i = 0; i < state.body_count; i++) {
        sg_destroy_buffer(state.bodies[i].vbuf);
    }
    sg_destroy_buffer(state.ibuf);
    state.inited = false;
}
