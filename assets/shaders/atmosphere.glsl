// Atmospheric scattering — simplified Nishita single-scatter.
//
// Port of reference/rts-engine/assets/shaders/atmosphere.glsl, which
// is itself a port of atmosphere.wgsl from the upstream RTS engine.
// Algorithmically identical (Rayleigh + Mie, 8 view samples × 4
// light samples, exponential density falloff, exp(-tR-tM)
// attenuation); only the surrounding sokol-shdc annotation and the
// uniform-block split into per-stage blocks differ.
//
// Coordinate space: aPos / cameraPos / sunDir are all in *body-local*
// space, where the planet's surface is at radius 1.0 and the
// atmosphere shell sits at radius `params.y` (atmosphere_outer_mul
// from the planet YAML). The C side computes
//   cameraPos = (cam_world - body_world) / body_radius
//   sunDir    = normalize(-body_world)            // sun is at origin
// and passes them per-frame so each fragment can ray-march the same
// way regardless of the body's actual world-space scale or position.
//
// Drawn with cull_mode = FRONT (visible == back hemisphere of shell)
// so that the camera→fragment ray goes through the entire atmosphere
// before reaching the rendered fragment, which is what the
// integration along [tStart, tEnd] expects.

@module atmosphere

@vs atmo_vs
layout(binding=0) uniform atmo_vs_params {
    mat4 mvp;
};
layout(location=0) in vec3 aPos;
out vec3 vLocalPos;
void main() {
    gl_Position = mvp * vec4(aPos, 1.0);
    vLocalPos = aPos;
}
@end

@fs atmo_fs
layout(binding=1) uniform atmo_fs_params {
    vec4 sunDir;        // xyz = body-local direction to sun (unit)
    vec4 cameraPos;     // xyz = body-local camera position
    vec4 params;        // x = planet radius, y = atmosphere radius, z = sun intensity
};
in vec3 vLocalPos;
out vec4 FragColor;

const float PI                = 3.14159265;
const int   NUM_SAMPLES       = 8;
const int   NUM_LIGHT_SAMPLES = 4;
const float SCALE_HEIGHT      = 0.5;
const vec3  WAVE_INV4         = vec3(5.602, 9.473, 19.644);

float rayleighPhase(float ct) { return 0.75 * (1.0 + ct * ct); }
float miePhase(float ct, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * ct, 1.5));
}
vec2 raySphere(vec3 ro, vec3 rd, float r) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - r * r;
    float d = b * b - c;
    if (d < 0.0) return vec2(-1.0);
    float sq = sqrt(d);
    return vec2(-b - sq, -b + sq);
}
float density(float alt) { return exp(-alt / SCALE_HEIGHT); }
float lightOpticalDepth(vec3 ro, vec3 rd, float len, float pR, float aR) {
    float step = len / float(NUM_LIGHT_SAMPLES);
    float d = 0.0;
    for (int i = 0; i < NUM_LIGHT_SAMPLES; i++) {
        vec3 p = ro + rd * (step * (float(i) + 0.5));
        float alt = clamp((length(p) - pR) / (aR - pR), 0.0, 1.0);
        d += density(alt) * step;
    }
    return d;
}

void main() {
    float pR   = params.x;
    float aR   = params.y;
    float sunI = params.z;
    vec3  L    = normalize(sunDir.xyz);
    vec3  ro   = cameraPos.xyz;
    vec3  rd   = normalize(vLocalPos - ro);

    vec2 aHit = raySphere(ro, rd, aR);
    if (aHit.y < 0.0) discard;

    float tStart = max(0.0, aHit.x);
    float tEnd   = aHit.y;

    /* If the planet itself blocks the ray, integrate only up to the
     * planet's near surface — beyond that point the planet is opaque. */
    vec2 pHit = raySphere(ro, rd, pR);
    if (pHit.x > 0.0) tEnd = min(tEnd, pHit.x);
    if (tStart >= tEnd) discard;

    float step   = (tEnd - tStart) / float(NUM_SAMPLES);
    float rScale = 0.005;
    float mScale = 0.003;
    float mG     = 0.76;

    vec3  rSum = vec3(0.0);
    vec3  mSum = vec3(0.0);
    float odR  = 0.0;
    float odM  = 0.0;

    for (int i = 0; i < NUM_SAMPLES; i++) {
        vec3  sp = ro + rd * (tStart + step * (float(i) + 0.5));
        float h  = length(sp);
        if (h < pR) continue;

        float alt = clamp((h - pR) / (aR - pR), 0.0, 1.0);
        float ld  = density(alt);
        odR += ld * step;
        odM += ld * step;

        /* Skip if this sample is in shadow of the planet. */
        vec2 spHit = raySphere(sp, L, pR);
        if (spHit.x > 0.0) continue;

        vec2  saHit = raySphere(sp, L, aR);
        float sod   = lightOpticalDepth(sp, L, max(0.0, saHit.y), pR, aR);

        vec3 tR  = rScale * WAVE_INV4 * (odR + sod);
        vec3 tM  = vec3(mScale * (odM + sod));
        vec3 att = exp(-(tR + tM));

        rSum += ld * att * step;
        mSum += ld * att * step;
    }

    rSum *= rScale * WAVE_INV4;
    mSum *= mScale;

    float ct = dot(rd, L);
    vec3  col = sunI * (rSum * rayleighPhase(ct) + mSum * miePhase(ct, mG));
    col = 1.0 - exp(-col);

    float a = clamp(length(col) * 2.0, 0.0, 0.9);
    FragColor = vec4(col, a);
}
@end

@program atmosphere atmo_vs atmo_fs
