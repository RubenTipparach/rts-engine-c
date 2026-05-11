// Clouds — barest possible white-noise alpha mask while we debug
// visibility. Replace with proper fbm + lighting once we confirm
// the pipeline draws anything at all.

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
    vec4 params;   // x = time, y = scale, z = threshold, w = unused
    vec4 color;
};
in vec3 vLocal;
out vec4 FragColor;

float hash3(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

void main() {
    /* Sample a hash-driven white noise on the cloud sphere. params.y
     * scales the lookup so we get many "stars" of noise across the
     * surface, params.z is the binary threshold (anything below is
     * discarded, anything above is fully visible). */
    vec3  N = normalize(vLocal);
    float n = hash3(N * params.y + vec3(params.x));

    /* Alpha test: keep only the bright half of the noise. */
    if (n < params.z) discard;

    /* Touch sunDir so the uniform block isn't stripped. */
    float keep = sunDir.x * 1e-6;
    FragColor = vec4(vec3(1.0) + vec3(keep), color.a);
}
@end

@program clouds cloud_vs cloud_fs
