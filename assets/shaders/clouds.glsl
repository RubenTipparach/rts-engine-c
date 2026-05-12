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
    vec4 p = mvp * vec4(aPos, 1.0);
    /* Shift cloud fragments forward in clip-space depth by a fixed
     * NDC amount. After perspective divide this becomes a constant
     * NDC-z offset, large enough to beat the planet's terrain depth
     * even when the cloud shell sits tight to it. Still small
     * compared to the distance to any other body, so a moon between
     * camera and cloud-planet still depth-tests correctly. */
    p.z -= 0.004 * p.w;
    gl_Position = p;
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

/* Take a local copy of p so the caller's variable is not modified.
 * sokol-shdc / SPIRV-Cross translates a parameter written inside the
 * function as `inout` in GLES output, which would propagate the
 * octave doubling back to whatever the caller passed in. Disastrous
 * when the caller chains multiple fbm samples on the same `base`. */
float fbm3(vec3 p) {
    vec3 q = p;
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; i++) {
        v += a * noise3(q);
        q *= 2.0;
        a *= 0.5;
    }
    return v;
}

void main() {
    /* DIAGNOSTIC: spherical checkerboard, alternating bright-cyan and
     * fully-transparent. If this pattern reads cleanly over the
     * planet's terrain, the cloud pipeline (vbuf / cull / depth /
     * blend) is correct and the original cloud look bug was in the
     * fbm/discard logic. If only the limb shows checker squares,
     * something else is rejecting the front hemisphere. */
    vec3  N     = normalize(vLocal);
    float theta = atan(N.z, N.x);         /* longitude in [-π, π] */
    float phi   = asin(clamp(N.y, -1.0, 1.0)); /* latitude  in [-π/2, π/2] */

    /* 12 longitude bands × 6 latitude bands. */
    float u = floor(theta * 1.91);
    float v = floor(phi   * 1.91);
    float checker = mod(u + v, 2.0);

    if (checker < 0.5) discard;
    /* Slow drift the longitude bands using params.x (drift_time) and
     * tint by color.rgb so sokol-shdc keeps the fs_params uniform
     * block live — the C side still binds it. sunDir is unused here
     * but referenced so its slot survives. */
    vec3 tint = mix(vec3(0.0, 1.0, 1.0), color.rgb, 0.5);
    FragColor = vec4(tint + sunDir.xyz * 0.0001, 0.55 + params.x * 0.0001);
}
@end

@program clouds cloud_vs cloud_fs
