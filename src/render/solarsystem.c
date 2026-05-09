#include "solarsystem.h"

#include "core/log.h"
#include "render/sphere.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

/* M1 flat-shaded renderer.
 *
 * One sphere mesh shared by the sun, all planets and all moons. Each
 * body is drawn with its own model matrix (translate × scale) and a
 * per-draw fs uniform that selects flat colour + "ambient = 1" for
 * the sun (so it ignores lighting) vs "ambient = engine.ambient" for
 * lit bodies. The sphere shader port (corona, atmosphere) replaces
 * this in the next commit; the geometry layout and draw count stay
 * the same.
 *
 * Time scale: matches upstream's GetPosition() — angle = phase +
 * orbit_speed * sim_time * 0.1. The 0.1 multiplier is the upstream
 * TimeScale literal ("rad/sec, scaled by TimeScale=0.1" — see
 * comments in solarsystem.yaml). */

#define ENGINE_TIME_SCALE     0.1f
#define ENGINE_AMBIENT        0.15f
#define SPHERE_STACKS         32
#define SPHERE_SLICES         48
#define SPHERE_MAX_VERTS    ((SPHERE_STACKS + 1) * (SPHERE_SLICES + 1))
#define SPHERE_MAX_INDICES  (SPHERE_STACKS * SPHERE_SLICES * 6)

typedef struct {
    HMM_Mat4 mvp;
    HMM_Mat4 model;
} vs_params_t;

typedef struct {
    HMM_Vec4 color;        /* rgb = base colour, a unused */
    HMM_Vec4 light_amb;    /* xyz = light dir (world), w = ambient */
} fs_params_t;

static struct {
    bool                       inited;
    const solarsystem_config_t *cfg;
    sg_buffer                  vbuf;
    sg_buffer                  ibuf;
    int                        index_count;
    sg_shader                  shd;
    sg_pipeline                pip;
    double                     sim_time;
} state;

/* ---- shaders ---- */

#if defined(SOKOL_GLES3)
#  define VS_VERSION "#version 300 es\n"
#  define FS_VERSION "#version 300 es\nprecision highp float;\n"
#else
#  define VS_VERSION "#version 330\n"
#  define FS_VERSION "#version 330\n"
#endif

/* sokol_gfx's GL/GLES3 backend doesn't use real UBOs — `uniform_blocks[]`
 * is just a packed-individual-uniforms description, set with
 * glUniformXXX under the hood. So the shader declares *individual*
 * uniforms (`uniform mat4 mvp;`), not a `layout(std140) uniform { ... }`
 * block. The std140 layout hint on the C side enforces matching
 * struct alignment, nothing more. */
static const char *VS_SRC =
    VS_VERSION
    "uniform mat4 mvp;\n"
    "uniform mat4 model;\n"
    "in vec3 a_pos;\n"
    "in vec3 a_normal;\n"
    "out vec3 v_normal_ws;\n"
    "void main() {\n"
    "    gl_Position = mvp * vec4(a_pos, 1.0);\n"
    "    v_normal_ws = (model * vec4(a_normal, 0.0)).xyz;\n"
    "}\n";

static const char *FS_SRC =
    FS_VERSION
    "uniform vec4 color;\n"
    "uniform vec4 light_amb;\n"  /* xyz = world-space light dir, w = ambient */
    "in vec3 v_normal_ws;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    vec3 n = normalize(v_normal_ws);\n"
    "    vec3 l = normalize(-light_amb.xyz);\n"
    "    float ndotl = max(dot(n, l), 0.0);\n"
    "    float a = light_amb.w;\n"
    "    vec3 lit = color.rgb * (a + (1.0 - a) * ndotl);\n"
    "    frag_color = vec4(lit, 1.0);\n"
    "}\n";

/* ---- init ---- */

static void build_mesh(void)
{
    static sphere_vertex_t verts[SPHERE_MAX_VERTS];
    static uint16_t        indices[SPHERE_MAX_INDICES];
    int v_count = 0, i_count = 0;
    if (!sphere_make_uv(SPHERE_STACKS, SPHERE_SLICES,
                        verts, SPHERE_MAX_VERTS, &v_count,
                        indices, SPHERE_MAX_INDICES, &i_count)) {
        LOG_ERROR("solarsystem: sphere mesh did not fit in static buffers");
        return;
    }

    state.vbuf = sg_make_buffer(&(sg_buffer_desc){
        .data = { .ptr = verts, .size = (size_t)v_count * sizeof(sphere_vertex_t) },
        .label = "sphere-vbuf",
    });
    state.ibuf = sg_make_buffer(&(sg_buffer_desc){
        .usage = { .index_buffer = true },
        .data = { .ptr = indices, .size = (size_t)i_count * sizeof(uint16_t) },
        .label = "sphere-ibuf",
    });
    state.index_count = i_count;
}

static void build_shader_and_pipeline(void)
{
    state.shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func.source   = VS_SRC,
        .fragment_func.source = FS_SRC,
        .attrs = {
            [0] = { .glsl_name = "a_pos" },
            [1] = { .glsl_name = "a_normal" },
        },
        .uniform_blocks[0] = {
            .stage = SG_SHADERSTAGE_VERTEX,
            .size  = sizeof(vs_params_t),
            .layout = SG_UNIFORMLAYOUT_STD140,
            .glsl_uniforms = {
                [0] = { .glsl_name = "mvp",   .type = SG_UNIFORMTYPE_MAT4 },
                [1] = { .glsl_name = "model", .type = SG_UNIFORMTYPE_MAT4 },
            },
        },
        .uniform_blocks[1] = {
            .stage = SG_SHADERSTAGE_FRAGMENT,
            .size  = sizeof(fs_params_t),
            .layout = SG_UNIFORMLAYOUT_STD140,
            .glsl_uniforms = {
                [0] = { .glsl_name = "color",     .type = SG_UNIFORMTYPE_FLOAT4 },
                [1] = { .glsl_name = "light_amb", .type = SG_UNIFORMTYPE_FLOAT4 },
            },
        },
        .label = "flat-lit-shader",
    });

    state.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = state.shd,
        .layout = {
            .attrs = {
                [0] = { .format = SG_VERTEXFORMAT_FLOAT3 },
                [1] = { .format = SG_VERTEXFORMAT_FLOAT3 },
            },
        },
        .index_type = SG_INDEXTYPE_UINT16,
        .cull_mode  = SG_CULLMODE_BACK,
        .face_winding = SG_FACEWINDING_CCW,
        .depth = {
            .compare      = SG_COMPAREFUNC_LESS_EQUAL,
            .write_enabled = true,
        },
        .label = "flat-lit-pipeline",
    });
}

void solarsystem_init(const solarsystem_config_t *cfg)
{
    state.cfg = cfg;
    state.sim_time = 0.0;
    build_mesh();
    build_shader_and_pipeline();
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

static HMM_Vec3 body_position(float orbit_radius, float orbit_speed, float phase, double t)
{
    float ang = phase + orbit_speed * (float)t * ENGINE_TIME_SCALE;
    return (HMM_Vec3){ .Elements = {
        cosf(ang) * orbit_radius,
        0.0f,
        sinf(ang) * orbit_radius,
    }};
}

static void draw_body(HMM_Vec3 world_pos, float radius, HMM_Vec3 color,
                      bool emissive, HMM_Mat4 view_proj, HMM_Vec3 light_dir)
{
    HMM_Mat4 model = HMM_MulM4(HMM_Translate(world_pos),
                               HMM_Scale((HMM_Vec3){ .Elements = { radius, radius, radius } }));
    vs_params_t vsp = {
        .mvp   = HMM_MulM4(view_proj, model),
        .model = model,
    };
    fs_params_t fsp = {
        .color     = { .Elements = { color.X, color.Y, color.Z, 1.0f } },
        .light_amb = { .Elements = { light_dir.X, light_dir.Y, light_dir.Z,
                                     emissive ? 1.0f : ENGINE_AMBIENT } },
    };

    sg_apply_uniforms(0, &(sg_range){ &vsp, sizeof(vsp) });
    sg_apply_uniforms(1, &(sg_range){ &fsp, sizeof(fsp) });
    sg_draw(0, state.index_count, 1);
}

void solarsystem_frame(double dt, int fb_width, int fb_height, const camera_t *cam)
{
    if (!state.inited || !state.cfg) return;
    state.sim_time += dt;

    float aspect = (fb_height > 0) ? (float)fb_width / (float)fb_height : 1.0f;
    HMM_Mat4 view_proj = HMM_MulM4(camera_proj(cam, aspect), camera_view(cam));

    sg_apply_pipeline(state.pip);
    sg_apply_bindings(&(sg_bindings){
        .vertex_buffers[0] = state.vbuf,
        .index_buffer      = state.ibuf,
    });

    /* Sun first — emissive (no shading), at origin. */
    {
        HMM_Vec3 dir    = { .Elements = { 1.0f, 0.0f, 0.0f } }; /* irrelevant for emissive */
        HMM_Vec3 origin = { .Elements = { 0.0f, 0.0f, 0.0f } };
        draw_body(origin, state.cfg->sun.radius,
                  state.cfg->sun.color, true, view_proj, dir);
    }

    /* Planets + their moons. Light direction at each body is the
     * vector from the sun (origin) to the body — i.e. the world-space
     * direction the light is travelling. */
    const solarsystem_config_t *cfg = state.cfg;
    for (int i = 0; i < cfg->planet_count; i++) {
        const planet_config_t *pl = &cfg->planets[i];
        HMM_Vec3 ppos = body_position(pl->self.orbit_radius, pl->self.orbit_speed,
                                       pl->self.phase, state.sim_time);
        draw_body(ppos, pl->self.display_radius, pl->self.color,
                  false, view_proj, ppos);

        for (int j = 0; j < pl->moon_count; j++) {
            const body_config_t *m = &pl->moons[j];
            HMM_Vec3 rel  = body_position(m->orbit_radius, m->orbit_speed,
                                          m->phase, state.sim_time);
            HMM_Vec3 mpos = HMM_AddV3(ppos, rel);
            draw_body(mpos, m->display_radius, m->color,
                      false, view_proj, mpos);
        }
    }
}

void solarsystem_shutdown(void)
{
    if (!state.inited) return;
    sg_destroy_pipeline(state.pip);
    sg_destroy_shader(state.shd);
    sg_destroy_buffer(state.ibuf);
    sg_destroy_buffer(state.vbuf);
    state.inited = false;
}
