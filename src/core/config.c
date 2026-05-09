#include "config.h"

#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tiny YAML subset:
 *
 *   - top-level sections (`sun:`, `planets:`),
 *   - `key: value` scalars (string or number),
 *   - `key: [a, b, c]` inline numeric arrays,
 *   - list items with `- key: value`,
 *   - nested moon lists under a planet,
 *   - `#` line comments.
 *
 * Indentation is 2-space and used to disambiguate sun-fields vs
 * planet-fields vs moon-fields (planets at indent 2 with leading dash,
 * planet fields at 4, moon list dashes at 6, moon fields at 8).
 *
 * The grammar is tighter than real YAML on purpose — anything weirder
 * (anchors, multiline strings, flow maps) is intentionally unsupported.
 * If a config needs more, use a different parser, not a bigger one
 * here. */

static void str_rstrip(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = '\0';
    }
}

static int count_indent(const char *s)
{
    int n = 0;
    while (*s == ' ') { n++; s++; }
    return n;
}

/* Truncate at the first '#' so trailing-comment removal happens before
 * any tokenization. The YAML subset we accept doesn't have quoted
 * strings, so a literal '#' inside a value isn't expressible. */
static void strip_comment(char *s)
{
    char *p = strchr(s, '#');
    if (p) *p = '\0';
}

/* Compare a YAML key (camelCase from upstream) against a C-side label,
 * case-insensitive and ignoring underscores in either side — this is
 * what lets the same parser accept `configFile`, `config_file`, and
 * `ConfigFile` interchangeably (per CLAUDE.md). */
static int key_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a == '_') { a++; continue; }
        if (*b == '_') { b++; continue; }
        char ca = (char)tolower((unsigned char)*a);
        char cb = (char)tolower((unsigned char)*b);
        if (ca != cb) return 0;
        a++; b++;
    }
    while (*a == '_') a++;
    while (*b == '_') b++;
    return *a == 0 && *b == 0;
}

/* Parse the value half of `key: <value>`:
 *   - `[a, b, c]` fills out_floats[0..*out_count],
 *   - bare numeric also fills out_floats[0],
 *   - everything also gets copied into out_str (whitespace-trimmed)
 *     so string-valued keys like `name` and `configFile` work without
 *     a separate code path. */
static void parse_value(const char *v,
                        char *out_str, size_t out_str_n,
                        float *out_floats, int *out_count, int max_floats)
{
    *out_count = 0;
    if (out_str_n) out_str[0] = '\0';

    while (*v == ' ' || *v == '\t') v++;
    if (*v == '\0') return;

    if (*v == '[') {
        v++;
        while (*v && *v != ']' && *out_count < max_floats) {
            while (*v == ' ' || *v == '\t' || *v == ',') v++;
            if (*v == ']' || *v == '\0') break;
            char *end;
            float f = strtof(v, &end);
            if (end == v) break;
            out_floats[(*out_count)++] = f;
            v = end;
        }
        return;
    }

    /* Scalar — keep both string and (if numeric) float forms. */
    size_t n = strlen(v);
    if (out_str_n) {
        size_t cn = (n < out_str_n - 1) ? n : (out_str_n - 1);
        memcpy(out_str, v, cn);
        out_str[cn] = '\0';
        /* trim trailing whitespace from the copy */
        char *e = out_str + strlen(out_str);
        while (e > out_str && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    }
    char *end;
    float f = strtof(v, &end);
    if (end != v) {
        out_floats[0] = f;
        *out_count = 1;
    }
}

static void body_apply_default(body_config_t *b)
{
    memset(b, 0, sizeof(*b));
    b->display_radius = 1.0f;
    b->color = (HMM_Vec3){ .Elements = { 1.0f, 1.0f, 1.0f } };
}

static void body_set_field(body_config_t *b, const char *key, const char *val_str,
                           const float *fl, int n)
{
    if (key_eq(key, "name")) {
        snprintf(b->name, sizeof(b->name), "%s", val_str);
    } else if (key_eq(key, "configFile")) {
        snprintf(b->config_file, sizeof(b->config_file), "%s", val_str);
    } else if (key_eq(key, "orbitRadius")    && n >= 1) b->orbit_radius    = fl[0];
    else if (key_eq(key, "orbitSpeed")       && n >= 1) b->orbit_speed     = fl[0];
    else if (key_eq(key, "phase")            && n >= 1) b->phase           = fl[0];
    else if (key_eq(key, "displayRadius")    && n >= 1) b->display_radius  = fl[0];
    else if (key_eq(key, "color")            && n >= 3) b->color = (HMM_Vec3){ .Elements = { fl[0], fl[1], fl[2] } };
    else if (key_eq(key, "noiseSeed")        && n >= 1) b->noise_seed      = (int)fl[0];
    else if (key_eq(key, "noiseFrequency")   && n >= 1) b->noise_frequency = fl[0];
    else if (key_eq(key, "noiseThresholds")) {
        int take = n < CFG_MAX_NOISE_THRESHOLDS ? n : CFG_MAX_NOISE_THRESHOLDS;
        for (int i = 0; i < take; i++) b->noise_thresholds[i] = fl[i];
        b->noise_threshold_count = take;
    }
    else if (key_eq(key, "zoomMin")          && n >= 1) b->zoom_min        = fl[0];
    else if (key_eq(key, "zoomMax")          && n >= 1) b->zoom_max        = fl[0];
}

static void sun_set_field(sun_config_t *s, const char *key, const char *val_str,
                          const float *fl, int n)
{
    if (key_eq(key, "name")) {
        snprintf(s->name, sizeof(s->name), "%s", val_str);
    } else if (key_eq(key, "color")        && n >= 3) s->color      = (HMM_Vec3){ .Elements = { fl[0], fl[1], fl[2] } };
    else if (key_eq(key, "radius")         && n >= 1) s->radius       = fl[0];
    else if (key_eq(key, "glowColor")      && n >= 3) s->glow_color = (HMM_Vec3){ .Elements = { fl[0], fl[1], fl[2] } };
    else if (key_eq(key, "glowRadius")     && n >= 1) s->glow_radius  = fl[0];
    else if (key_eq(key, "coronaSpeed")    && n >= 1) s->corona_speed = fl[0];
}

bool config_load_solarsystem(const char *path, solarsystem_config_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR("config: cannot open %s", path);
        return false;
    }

    enum { SECT_NONE, SECT_SUN, SECT_PLANETS } sect = SECT_NONE;
    int  planet_idx = -1;
    int  moon_idx   = -1;
    bool in_moons   = false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        strip_comment(line);
        str_rstrip(line);

        /* skip blank/comment-only lines */
        const char *probe = line;
        while (*probe == ' ' || *probe == '\t') probe++;
        if (*probe == '\0') continue;

        int indent = count_indent(line);
        char *p = line + indent;

        bool list_item = false;
        if (p[0] == '-' && p[1] == ' ') {
            list_item = true;
            p += 2;
        }

        char *colon = strchr(p, ':');
        if (!colon) continue;
        *colon = '\0';
        char *key = p;
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;

        char *key_end = key + strlen(key);
        while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t')) *--key_end = '\0';

        if (indent == 0) {
            if      (key_eq(key, "sun"))     sect = SECT_SUN;
            else if (key_eq(key, "planets")) {
                sect = SECT_PLANETS;
                planet_idx = -1;
                moon_idx   = -1;
                in_moons   = false;
            }
            else                             sect = SECT_NONE;
            continue;
        }

        char  val_str[256];
        float floats[16];
        int   count;
        parse_value(val, val_str, sizeof(val_str), floats, &count, 16);

        if (sect == SECT_SUN) {
            sun_set_field(&out->sun, key, val_str, floats, count);
            continue;
        }

        if (sect != SECT_PLANETS) continue;

        if (list_item && indent == 2) {
            if (out->planet_count >= CFG_MAX_PLANETS) continue;
            planet_idx = out->planet_count++;
            body_apply_default(&out->planets[planet_idx].self);
            out->planets[planet_idx].moon_count = 0;
            in_moons = false;
            moon_idx = -1;
            body_set_field(&out->planets[planet_idx].self, key, val_str, floats, count);
        } else if (planet_idx >= 0 && in_moons && list_item && indent == 6) {
            planet_config_t *pl = &out->planets[planet_idx];
            if (pl->moon_count >= CFG_MAX_MOONS) continue;
            moon_idx = pl->moon_count++;
            body_apply_default(&pl->moons[moon_idx]);
            body_set_field(&pl->moons[moon_idx], key, val_str, floats, count);
        } else if (planet_idx >= 0 && in_moons && moon_idx >= 0 && indent >= 8) {
            body_set_field(&out->planets[planet_idx].moons[moon_idx], key, val_str, floats, count);
        } else if (planet_idx >= 0 && indent == 4) {
            if (key_eq(key, "moons")) {
                in_moons = true;
                moon_idx = -1;
            } else {
                body_set_field(&out->planets[planet_idx].self, key, val_str, floats, count);
            }
        }
    }

    fclose(f);
    return true;
}

/* ============================================================
 * engine.yaml — section-keyed, scalar + vec3, same parser primitives.
 * ============================================================ */

void engine_config_apply_defaults(engine_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->camera.transition_duration   = 1.5f;
    cfg->camera.default_elevation     = 0.4f;
    cfg->camera.pixels_to_radians     = 0.005f;

    cfg->lighting.sun_direction       = (HMM_Vec3){ .Elements = { 0.5f, 0.7f, 0.5f } };
    cfg->lighting.ambient_intensity   = 0.15f;
    cfg->lighting.diffuse_intensity   = 0.85f;

    cfg->solar_system_view.default_distance       = 80.0f;
    cfg->solar_system_view.min_distance           = 10.0f;
    cfg->solar_system_view.max_distance           = 200.0f;
    cfg->solar_system_view.sphere_segments_planet = 40;
    cfg->solar_system_view.sphere_segments_sun    = 48;
    cfg->solar_system_view.sphere_segments_moon   = 24;
    cfg->solar_system_view.orbit_ring_segments    = 64;
    cfg->solar_system_view.moon_orbit_segments    = 32;
    cfg->solar_system_view.pick_radius_multiplier = 3.0f;
}

static void engine_set_camera(camera_engine_t *c, const char *key,
                              const float *fl, int n)
{
    if      (key_eq(key, "transitionDuration") && n >= 1) c->transition_duration = fl[0];
    else if (key_eq(key, "defaultElevation")   && n >= 1) c->default_elevation   = fl[0];
    else if (key_eq(key, "pixelsToRadians")    && n >= 1) c->pixels_to_radians   = fl[0];
}

static void engine_set_lighting(lighting_engine_t *l, const char *key,
                                const float *fl, int n)
{
    if      (key_eq(key, "sunDirection")     && n >= 3) l->sun_direction = (HMM_Vec3){ .Elements = { fl[0], fl[1], fl[2] } };
    else if (key_eq(key, "ambientIntensity") && n >= 1) l->ambient_intensity = fl[0];
    else if (key_eq(key, "diffuseIntensity") && n >= 1) l->diffuse_intensity = fl[0];
}

static void engine_set_solarsystem_view(solarsystem_view_engine_t *v, const char *key,
                                        const float *fl, int n)
{
    if      (key_eq(key, "defaultDistance")      && n >= 1) v->default_distance       = fl[0];
    else if (key_eq(key, "minDistance")          && n >= 1) v->min_distance           = fl[0];
    else if (key_eq(key, "maxDistance")          && n >= 1) v->max_distance           = fl[0];
    else if (key_eq(key, "sphereSegmentsPlanet") && n >= 1) v->sphere_segments_planet = (int)fl[0];
    else if (key_eq(key, "sphereSegmentsSun")    && n >= 1) v->sphere_segments_sun    = (int)fl[0];
    else if (key_eq(key, "sphereSegmentsMoon")   && n >= 1) v->sphere_segments_moon   = (int)fl[0];
    else if (key_eq(key, "orbitRingSegments")    && n >= 1) v->orbit_ring_segments    = (int)fl[0];
    else if (key_eq(key, "moonOrbitSegments")    && n >= 1) v->moon_orbit_segments    = (int)fl[0];
    else if (key_eq(key, "pickRadiusMultiplier") && n >= 1) v->pick_radius_multiplier = fl[0];
}

bool config_load_engine(const char *path, engine_config_t *out)
{
    engine_config_apply_defaults(out);

    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR("config: cannot open %s", path);
        return false;
    }

    /* Sections we don't yet consume parse to "skip" so unknown keys
     * inside them are silently ignored — matches the CLAUDE.md rule
     * that omitted-or-zero fields keep the compiled-in default. */
    enum {
        E_NONE,
        E_CAMERA,
        E_LIGHTING,
        E_SOLAR_VIEW,
        E_SKIP,
    } sect = E_NONE;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        strip_comment(line);
        str_rstrip(line);

        const char *probe = line;
        while (*probe == ' ' || *probe == '\t') probe++;
        if (*probe == '\0') continue;

        int   indent = count_indent(line);
        char *p      = line + indent;
        if (p[0] == '-' && p[1] == ' ') continue;        /* engine.yaml has no list items */

        char *colon = strchr(p, ':');
        if (!colon) continue;
        *colon = '\0';
        char *key = p;
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;

        char *key_end = key + strlen(key);
        while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t')) *--key_end = '\0';

        if (indent == 0) {
            if      (key_eq(key, "camera"))          sect = E_CAMERA;
            else if (key_eq(key, "lighting"))        sect = E_LIGHTING;
            else if (key_eq(key, "solarSystemView")) sect = E_SOLAR_VIEW;
            else                                      sect = E_SKIP;
            continue;
        }

        char  val_str[256];
        float floats[16];
        int   count;
        parse_value(val, val_str, sizeof(val_str), floats, &count, 16);

        switch (sect) {
            case E_CAMERA:     engine_set_camera(&out->camera, key, floats, count); break;
            case E_LIGHTING:   engine_set_lighting(&out->lighting, key, floats, count); break;
            case E_SOLAR_VIEW: engine_set_solarsystem_view(&out->solar_system_view, key, floats, count); break;
            default: break;
        }
    }

    fclose(f);
    return true;
}

void config_log_engine(const engine_config_t *cfg)
{
    LOG_INFO("config: engine camera=(transitionDur=%.2fs elevation=%.2f sens=%.4f)",
             cfg->camera.transition_duration, cfg->camera.default_elevation,
             cfg->camera.pixels_to_radians);
    LOG_INFO("config: engine view=(dist=%.1f range=[%.1f,%.1f] pickMult=%.2f rings=%d)",
             cfg->solar_system_view.default_distance,
             cfg->solar_system_view.min_distance,
             cfg->solar_system_view.max_distance,
             cfg->solar_system_view.pick_radius_multiplier,
             cfg->solar_system_view.orbit_ring_segments);
    LOG_INFO("config: engine light=(ambient=%.2f diffuse=%.2f sunDir=(%.2f,%.2f,%.2f))",
             cfg->lighting.ambient_intensity,
             cfg->lighting.diffuse_intensity,
             cfg->lighting.sun_direction.X,
             cfg->lighting.sun_direction.Y,
             cfg->lighting.sun_direction.Z);
}

void config_log_solarsystem(const solarsystem_config_t *cfg)
{
    LOG_INFO("config: sun=%s r=%.2f color=(%.2f,%.2f,%.2f) glow=%.2f coronaSpeed=%.2f",
             cfg->sun.name, cfg->sun.radius,
             cfg->sun.color.X, cfg->sun.color.Y, cfg->sun.color.Z,
             cfg->sun.glow_radius, cfg->sun.corona_speed);
    for (int i = 0; i < cfg->planet_count; i++) {
        const planet_config_t *pl = &cfg->planets[i];
        LOG_INFO("config: planet[%d] %-9s orbit=%6.2f speed=%.2f phase=%.2f r=%.2f color=(%.2f,%.2f,%.2f)",
                 i, pl->self.name, pl->self.orbit_radius, pl->self.orbit_speed,
                 pl->self.phase, pl->self.display_radius,
                 pl->self.color.X, pl->self.color.Y, pl->self.color.Z);
        for (int j = 0; j < pl->moon_count; j++) {
            const body_config_t *m = &pl->moons[j];
            LOG_INFO("config:   moon[%d] %-9s orbit=%6.2f speed=%.2f r=%.2f",
                     j, m->name, m->orbit_radius, m->orbit_speed, m->display_radius);
        }
    }
}
