// Solar system shader — Lambert-lit spheres (GLSL port of solarsystem.wgsl).
// Per-vertex: pos(3f) + normal(3f) + color(3f) + brightness(1f), stride 40.

layout(std140, binding = 0) uniform U {
    mat4 mvp;
    vec4 sunDir;
    vec4 viewDir; // xyz: cam dir, w: dither
} u;

const float LOG_DEPTH_FAR = 10000.0;
vec4 applyLogDepth(vec4 p) {
    float logZ = log2(max(1e-6, 1.0 + p.w)) / log2(1.0 + LOG_DEPTH_FAR);
    return vec4(p.x, p.y, logZ * p.w, p.w);
}

#ifdef VERTEX
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in float aBrightness;

out vec3 vNormal;
out vec3 vColor;
out float vBrightness;

void main() {
    gl_Position = applyLogDepth(u.mvp * vec4(aPos, 1.0));
    vNormal = aNormal;
    vColor = aColor;
    vBrightness = aBrightness;
}
#endif

#ifdef FRAGMENT
in vec3 vNormal;
in vec3 vColor;
in float vBrightness;
out vec4 FragColor;

float bayer4(uvec2 p) {
    float m[16] = float[16](
         0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
        12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
         3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
        15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
    );
    return m[(p.x & 3u) + (p.y & 3u) * 4u];
}

void main() {
    float dither = u.viewDir.w;
    if (dither > 0.001) {
        uvec2 p = uvec2(uint(gl_FragCoord.x), uint(gl_FragCoord.y));
        if (dither > bayer4(p)) discard;
    }

    vec3 N = normalize(vNormal);
    vec3 L = normalize(u.sunDir.xyz);
    vec3 V = normalize(u.viewDir.xyz);
    float NdotL = max(dot(N, L), 0.0);
    vec3 lit = vColor * vBrightness * (0.25 + NdotL * 0.9);

    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.5);
    float dayFactor = smoothstep(-0.1, 0.3, NdotL);
    lit = lit + vec3(0.35, 0.55, 0.95) * rim * 0.35 * dayFactor;

    FragColor = vec4(lit, 1.0);
}
#endif
