// Clouds — procedural 3D fbm density on a sphere shell, bump-mapped
// via finite-difference noise samples (no ray-march). Two layers per
// cloudy planet; the C side picks the radii / scale / drift / colour
// per layer and dispatches the same shader twice.
//
// Pipeline expects this on a sphere mesh slightly larger than the
// planet's terrain radius, drawn alpha-blended after the terrain +
// water but before the atmosphere shell. cull_mode = BACK keeps the
// far hemisphere from drawing on top of the near hemisphere.

@module clouds

@vs cloud_vs
layout(binding=0) uniform cloud_vs_params {
    mat4 mvp;
};
layout(location=0) in vec3 aPos;
out vec3 vLocal;
void main() {
    gl_Position = mvp * vec4(aPos, 1.0);
    vLocal = aPos;       /* body-local position on this layer's shell */
}
@end

@fs cloud_fs
layout(binding=1) uniform cloud_fs_params {
    vec4 sunDir;     // xyz = body-local sun direction
    vec4 params;     // x = time*drift_speed, y = noise scale, z = density threshold, w = bump strength
    vec4 color;      // rgb = base cloud colour, a = max alpha
};
in vec3 vLocal;
out vec4 FragColor;

/* Same hash + value noise + fbm constants as src/core/noise.c and
 * sun.glsl — kept inline because they're algorithm-identity per
 * CLAUDE.md ("magic constants in a hash"). */
float hash3(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}
float noise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(hash3(i),                  hash3(i + vec3(1,0,0)), u.x),
            mix(hash3(i + vec3(0,1,0)),    hash3(i + vec3(1,1,0)), u.x), u.y),
        mix(mix(hash3(i + vec3(0,0,1)),    hash3(i + vec3(1,0,1)), u.x),
            mix(hash3(i + vec3(0,1,1)),    hash3(i + vec3(1,1,1)), u.x), u.y),
        u.z);
}
float fbm3(vec3 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; i++) {
        v += a * noise3(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

void main() {
    float t      = params.x;
    float scale  = params.y;
    float thresh = params.z;

    vec3 N    = normalize(vLocal);
    vec3 base = N * scale + vec3(t, t * 0.6, t * -0.4);

    /* Density via fbm + smooth threshold. The alpha-test below
     * `discard`s sub-threshold fragments so they don't write into
     * the depth buffer — required because the pipeline now has
     * depth_write_enabled = true. */
    float d       = fbm3(base);
    float density = smoothstep(thresh, thresh + 0.15, d);

    /* Alpha-test: drop empty cloud cells. Anything below this is
     * effectively transparent — let the layer beneath show through
     * and don't pollute depth. */
    if (density < 0.05) discard;

    /* Lambert against the sun direction (no bump for clarity). */
    vec3  L     = normalize(sunDir.xyz);
    float NdotL = max(dot(N, L), 0.0);
    float lit   = 0.40 + 0.60 * NdotL;

    /* Day-side fade controls alpha only — no double attenuation. */
    float day   = smoothstep(-0.25, 0.30, dot(N, L));
    float alpha = density * color.a * day;

    FragColor = vec4(color.rgb * lit, alpha);
}
@end

@program clouds cloud_vs cloud_fs
