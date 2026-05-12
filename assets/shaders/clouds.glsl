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

    /* Two-band density: a low-freq fbm carves the big continent-sized
     * cloud shapes; a higher-freq fbm erodes their edges into wisps.
     * Multiplying keeps the dense core opaque but breaks the silhouette
     * into smaller puffs rather than a soupy blanket. */
    float bigShape = fbm3(base);
    float fine     = fbm3(base * 2.7 + vec3(7.3, 13.1, 4.7));
    float d        = bigShape * (0.55 + 0.45 * fine);

    /* Narrow smoothstep window for crisp cloud edges. Anything below
     * thresh is sky; above thresh + 0.18 is fully opaque cloud. */
    float density = smoothstep(thresh, thresh + 0.18, d);
    float alpha   = density * color.a;
    if (alpha < 0.005) discard;

    /* Normal-map style bump: sample fbm at small offsets in the tangent
     * frame, compute gradient, perturb the surface normal toward
     * higher-density direction. Smaller eps + bumpAmt means soft,
     * fluffy puffs instead of jagged noise. */
    vec3  up  = abs(N.y) > 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3  tng = normalize(cross(up, N));
    vec3  btn = cross(N, tng);
    float eps = 0.06;
    float du  = fbm3(base + tng * eps) - bigShape;
    float dv  = fbm3(base + btn * eps) - bigShape;
    vec3  N_bump = normalize(N - (tng * du + btn * dv) * bumpAmt);

    /* Lambert + warm-sun / cool-shadow tinting so cloud volumes read
     * as 3D. Sunlit side picks up the layer's base color directly;
     * shadow side falls toward a soft blue-grey. */
    vec3  L         = normalize(sunDir.xyz);
    float NdotL     = max(dot(N_bump, L), 0.0);
    float shading   = 0.35 + 0.65 * smoothstep(0.0, 0.7, NdotL);
    vec3  sunTint   = color.rgb;
    vec3  shadeTint = mix(vec3(0.42, 0.50, 0.62), color.rgb, 0.35);
    vec3  lit       = mix(shadeTint, sunTint, shading);

    /* Silver-lining: thin cloud edges catch sun and glow brighter.
     * Density near the threshold = thin edge fragment. */
    float edge      = 1.0 - density;
    float silver    = edge * NdotL * 0.40;
    lit            += vec3(silver);

    FragColor = vec4(lit, alpha);
}
@end

@program clouds cloud_vs cloud_fs
