// Starfield — verbatim port of the upstream's procedural star +
// nebula background (reference/rts-engine/assets/shaders/starfield.glsl).
//
// Drawn first each frame as a fullscreen triangle at z=0.99999, just
// shy of the far plane so the rest of the scene renders on top
// without depth conflicts. Per-fragment ray direction is reconstructed
// from NDC + the camera basis (right/up/forward) passed in as uniforms.
//
// Algorithm bodies (hash1, hash3, rehash3, voronoi3D, starfield3D,
// vnoise, fbm, main) are byte-for-byte the upstream shader's — only
// the surrounding sokol-shdc annotation differs.

@module starfield

@vs star_vs
layout(location=0) in vec3 aPos;
out vec2 vNdc;
void main() {
    gl_Position = vec4(aPos.xy, 0.99999, 1.0);
    vNdc = aPos.xy;
}
@end

@fs star_fs
layout(binding=0) uniform star_fs_params {
    vec4 camRight;
    vec4 camUp;
    vec4 camForward;
    vec4 params;  // x = tan(fov/2), y = aspect, z = time
};

in vec2 vNdc;
out vec4 FragColor;

float hash1(float x) {
    return fract(x + 1.3215 * 1.8152);
}

float hash3(vec3 a) {
    return fract((hash1(a.z * 42.8883) + hash1(a.y * 36.9125) + hash1(a.x * 65.4321)) * 291.1257);
}

vec3 rehash3(float x) {
    return vec3(
        hash1(((x + 0.5283) * 59.3829) * 274.3487),
        hash1(((x + 0.8192) * 83.6621) * 345.3871),
        hash1(((x + 0.2157) * 36.6521) * 458.3971));
}

vec2 voronoi3D(vec3 posIn, float density) {
    vec3 pos = posIn * density;
    vec3 basePos = floor(pos);
    float m = 9999.0;
    float w = 0.0;

    for (int ix = -1; ix < 2; ix++) {
        for (int iy = -1; iy < 2; iy++) {
            for (int iz = -1; iz < 2; iz++) {
                vec3 cell = basePos + vec3(float(ix), float(iy), float(iz));
                float h = hash3(cell);
                vec3 pt = rehash3(h) + cell;
                vec3 diff = pos - pt;
                float d = dot(diff, diff);
                if (d < m) { m = d; w = h; }
            }
        }
    }
    return vec2(m, w);
}

float starfield3D(vec3 dir, float density) {
    vec2 v = voronoi3D(dir, density);
    float star = 1.0 - smoothstep(0.0, 0.02, v.x);
    float brightness = fract(v.y * 123.456);
    float mask = step(0.7, brightness);
    return star * brightness * mask;
}

float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 s = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(hash3(i),                hash3(i + vec3(1,0,0)),   s.x),
            mix(hash3(i + vec3(0,1,0)),  hash3(i + vec3(1,1,0)),   s.x), s.y),
        mix(mix(hash3(i + vec3(0,0,1)),  hash3(i + vec3(1,0,1)),   s.x),
            mix(hash3(i + vec3(0,1,1)),  hash3(i + vec3(1,1,1)),   s.x), s.y),
        s.z);
}

float fbm(vec3 p) {
    float v = 0.0;
    float a = 0.5;
    vec3 q = p;
    for (int i = 0; i < 5; i++) {
        v += a * vnoise(q);
        q *= 2.07;
        a *= 0.5;
    }
    return v;
}

void main() {
    vec3 ray = normalize(
        camForward.xyz
        + vNdc.x * params.y * params.x * camRight.xyz
        + vNdc.y * params.x * camUp.xyz);

    float stars = 0.0;
    stars += starfield3D(ray, 100.0) * 1.0;
    stars += starfield3D(ray, 200.0) * 0.8;
    stars += starfield3D(ray, 300.0) * 0.6;
    vec3 starColor = vec3(1.0, 0.98, 0.95) * stars;

    float drift = params.z * 0.005;
    float n = fbm(ray * 1.4 + vec3(drift, 0.0, drift * 0.7));
    float n2 = fbm(ray * 3.1 + vec3(7.3, 2.1, 9.9));
    float cloud = smoothstep(0.45, 0.78, n);
    vec3 warm = vec3(0.55, 0.18, 0.38);
    vec3 cool = vec3(0.10, 0.20, 0.50);
    vec3 nebula = mix(cool, warm, smoothstep(0.3, 0.8, n2)) * cloud * 0.20;

    vec3 space = vec3(0.005, 0.007, 0.018);
    FragColor = vec4(space + nebula + starColor, 1.0);
}
@end

@program starfield star_vs star_fs
