// Sun shader — procedural surface, granulation, limb darkening, flares.
// Math is the upstream sun.wgsl/sun.glsl port; only the surrounding
// sokol-shdc annotation is new. Generated header at src/gen/sun.glsl.h
// via tools/gen-shaders.sh.

@module sun

@block log_depth
const float LOG_DEPTH_FAR = 10000.0;
vec4 applyLogDepth(vec4 p) {
    float logZ = log2(max(1e-6, 1.0 + p.w)) / log2(1.0 + LOG_DEPTH_FAR);
    return vec4(p.x, p.y, logZ * p.w, p.w);
}
@end

@vs sun_vs
@include_block log_depth

layout(binding=0) uniform sun_vs_params {
    mat4 mvp;
};

layout(location=0) in vec3 aPos;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vLocalPos;

void main() {
    gl_Position = applyLogDepth(mvp * vec4(aPos, 1.0));
    vWorldPos = aPos;
    vNormal = normalize(aPos);
    vLocalPos = aPos;
}
@end

@fs sun_fs

layout(binding=1) uniform sun_fs_params {
    vec4 params;   // x = sim time, y = glow ratio (0..1)
};

in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vLocalPos;

out vec4 FragColor;

float hash3(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

float noise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 uu = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(hash3(i),                      hash3(i + vec3(1,0,0)), uu.x),
            mix(hash3(i + vec3(0,1,0)),        hash3(i + vec3(1,1,0)), uu.x), uu.y),
        mix(mix(hash3(i + vec3(0,0,1)),        hash3(i + vec3(1,0,1)), uu.x),
            mix(hash3(i + vec3(0,1,1)),        hash3(i + vec3(1,1,1)), uu.x), uu.y),
        uu.z);
}

float fbm3(vec3 p) {
    float v = 0.0;
    float a = 0.5;
    vec3 q = p;
    for (int i = 0; i < 4; i++) {
        v += a * noise3(q);
        q *= 2.0;
        a *= 0.5;
    }
    return v;
}

void main() {
    float t = params.x;
    vec3 N = normalize(vNormal);
    vec3 dir = normalize(vLocalPos);

    float n1 = fbm3(dir * 5.0 + vec3(t * 0.05, t * 0.03, t * 0.04));
    float n2 = fbm3(dir * 10.0 + vec3(-t * 0.08, t * 0.06, t * 0.02));
    float granule = n1 * 0.6 + n2 * 0.4;

    vec3 hotColor = vec3(1.0, 0.98, 0.9);
    vec3 warmColor = vec3(1.0, 0.7, 0.2);
    vec3 coolColor = vec3(0.8, 0.3, 0.05);

    vec3 base = mix(warmColor, hotColor, granule);

    float viewDot = max(dot(N, normalize(-vWorldPos)), 0.0);
    float limb = pow(viewDot, 0.4);
    vec3 col = mix(coolColor, base, limb);

    float flare = smoothstep(0.72, 0.78, n1) * 0.5;
    col += vec3(flare, flare * 0.8, flare * 0.3);

    col *= 1.8;
    FragColor = vec4(col, 1.0);
}
@end

@program sun sun_vs sun_fs
