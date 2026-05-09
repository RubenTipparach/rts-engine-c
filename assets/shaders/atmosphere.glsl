// Atmosphere shader — pixel-art-styled rim glow on a shell sphere
// outside the planet. Instead of a smooth Nishita scatter (which
// would conflict with the pixel-art aesthetic in CLAUDE.md), the
// rim brightness is quantised to a small number of discrete bands
// and the in-between transition is Bayer-dithered.
//
// Drawn back-to-front against the planet: the shell sphere is
// rendered with cull_mode = FRONT so we see the inside surface,
// and depth-write is disabled so the shell composites on top of
// the planet without z-fighting against it.

@module atmosphere

@vs atmo_vs
layout(binding=0) uniform atmo_vs_params {
    mat4 mvp;
};

layout(location=0) in vec3 aPos;

out vec3 vNormal;

void main() {
    gl_Position = mvp * vec4(aPos, 1.0);
    /* Local-space normal of a unit-sphere shell == position. */
    vNormal = aPos;
}
@end

@fs atmo_fs
layout(binding=1) uniform atmo_fs_params {
    vec4 viewDir;     // xyz = world-space view dir (camera→fragment), w = unused
    vec4 sunDir;      // xyz = world-space sun dir, w = unused
    vec4 tint;        // rgb = atmosphere colour, a = max alpha (0..1)
};

in vec3 vNormal;
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
    vec3 N = normalize(vNormal);
    vec3 V = normalize(viewDir.xyz);
    vec3 L = normalize(sunDir.xyz);

    /* Limb brightness peaks where the view direction grazes the
     * shell tangentially (rim) and falls off toward the centre.
     * `1 - |N·V|` gives [0,1] from centre→rim. Squaring sharpens
     * the band so the glow concentrates at the horizon. */
    float rim       = 1.0 - abs(dot(N, V));
    float rim_sharp = rim * rim;

    /* Day side only: dot(N,L) > 0 fades up toward 1 over a small
     * smoothstep so the night side has a faint halo instead of
     * cutting off as a hard line. */
    float day       = smoothstep(-0.15, 0.25, dot(N, L));

    /* Quantise the rim brightness into 4 discrete bands so the
     * atmosphere reads as pixel-art stepped rather than a smooth
     * gradient. Bayer-dither the boundary between bands to
     * preserve the impression of falloff without continuous tone. */
    float intensity = rim_sharp * day * tint.a;
    float steps     = 4.0;
    float scaled    = intensity * steps;
    float band      = floor(scaled);
    float frac      = scaled - band;
    uvec2 px        = uvec2(uint(gl_FragCoord.x), uint(gl_FragCoord.y));
    if (frac > bayer4(px)) band += 1.0;
    float quant     = clamp(band / steps, 0.0, 1.0);

    if (quant <= 0.001) discard;

    FragColor = vec4(tint.rgb * quant, quant);
}
@end

@program atmosphere atmo_vs atmo_fs
