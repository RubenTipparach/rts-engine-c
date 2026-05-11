// Clouds — alpha-transparent fbm noise mask with normal-map style
// gradient bumping for depth. No ray-march. Drawn after the
// atmosphere shell so cloud cover composites *over* atmospheric haze.

@module clouds

@vs cloud_vs
layout(binding=0) uniform cloud_vs_params {
    mat4 mvp;
};
layout(location=0) in vec3 aPos;
out vec3 vLocal;
void main() {
    gl_Position = mvp * vec4(aPos, 1.0);
    vLocal = aPos;
}
@end

@fs cloud_fs
layout(binding=1) uniform cloud_fs_params {
    vec4 sunDir;
    vec4 params;   // x = drift_time, y = noise_scale, z = threshold, w = bump_strength
    vec4 color;    // rgb = cloud tint, a = max alpha
};
in vec3 vLocal;
out vec4 FragColor;

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
    float t       = params.x;
    float scale   = params.y;
    float thresh  = params.z;
    float bumpAmt = params.w;

    vec3 N    = normalize(vLocal);
    vec3 base = N * scale + vec3(t, t * 0.6, t * -0.4);

    /* Density via fbm. Smooth alpha (not binary) so cloud edges
     * fade to transparent instead of hard-cutting. */
    float d       = fbm3(base);
    float density = smoothstep(thresh, thresh + 0.20, d);
    if (density < 0.01) discard;

    /* Normal-map style: sample fbm at small offsets in the tangent
     * frame, compute gradient, perturb the surface normal toward
     * the higher-density direction. Bump is light (params.w around
     * 0.5-1.0) so Lambert stays positive. */
    vec3  up  = abs(N.y) > 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3  tng = normalize(cross(up, N));
    vec3  btn = cross(N, tng);
    float eps = 0.10;
    float du  = fbm3(base + tng * eps) - d;
    float dv  = fbm3(base + btn * eps) - d;
    vec3  N_bump = normalize(N - (tng * du + btn * dv) * bumpAmt);

    /* Lambert against the sun direction. Generous ambient floor so
     * unlit cloud edges stay readable. */
    vec3  L     = normalize(sunDir.xyz);
    float NdotL = max(dot(N_bump, L), 0.0);
    float lit   = 0.40 + 0.60 * NdotL;

    /* Day-side fade controls alpha only (no double attenuation on
     * the colour). */
    float day   = smoothstep(-0.20, 0.40, dot(N, L));
    float alpha = density * color.a * day;

    FragColor = vec4(color.rgb * lit, alpha);
}
@end

@program clouds cloud_vs cloud_fs
