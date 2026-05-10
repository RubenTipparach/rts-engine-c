// Atmosphere shader — smooth rim glow on a shell sphere outside the
// planet. Day-side only; uses a single-direction view approximation
// (camera→body) which is fine for a thin shell at solar-system
// distances. The full Nishita single-scatter port from the upstream
// is a follow-up; this shader establishes the shell + lighting
// plumbing so swapping in a richer scatter is local.
//
// Drawn back-to-front against the planet: shell rendered with
// cull_mode = FRONT so we see the inside surface, and depth-write
// is disabled so the shell composites on top of the planet without
// z-fighting.

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
    vec4 viewDir;     // xyz = world-space view dir (camera→body), w = unused
    vec4 sunDir;      // xyz = world-space sun dir, w = unused
    vec4 tint;        // rgb = atmosphere colour, a = max alpha (0..1)
};

in vec3 vNormal;
out vec4 FragColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(viewDir.xyz);
    vec3 L = normalize(sunDir.xyz);

    /* Rim peaks where the view grazes the shell tangentially —
     * `1 - |N·V|` ranges from 0 at the centre to 1 at the silhouette.
     * Squaring sharpens the band so the glow concentrates near the
     * horizon. */
    float rim       = 1.0 - abs(dot(N, V));
    float rim_sharp = rim * rim;

    /* Day-side fade: smoothstep on dot(N,L) so the night side has a
     * faint halo instead of cutting off as a hard line. */
    float day       = smoothstep(-0.15, 0.25, dot(N, L));

    float intensity = rim_sharp * day * tint.a;

    if (intensity <= 0.001) discard;

    FragColor = vec4(tint.rgb * intensity, intensity);
}
@end

@program atmosphere atmo_vs atmo_fs
