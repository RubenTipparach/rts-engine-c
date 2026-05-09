// Starfield — single-pixel point primitives at infinity. Per-vertex
// colour bakes in brightness + slight tint variation so we get the
// pixel-art impression of distant stars without any per-fragment
// shading.
//
// Drawn first each frame against an identity-translated view matrix
// (stars rotate with the camera but don't translate), so they read
// as a celestial sphere far behind the solar system.

@module starfield

@vs star_vs
layout(binding=0) uniform star_vs_params {
    mat4 mvp;
};
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
out vec3 vColor;
void main() {
    gl_Position  = mvp * vec4(aPos, 1.0);
    gl_PointSize = 1.0;
    vColor = aColor;
}
@end

@fs star_fs
in vec3 vColor;
out vec4 FragColor;
void main() {
    FragColor = vec4(vColor, 1.0);
}
@end

@program starfield star_vs star_fs
