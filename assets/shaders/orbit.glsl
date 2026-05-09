// Orbit-ring shader — flat-colour line strip for the planet/moon
// orbital paths drawn in the solar-system view. The geometry is a
// unit-radius circle in the xz plane; per-draw mvp scales + (for
// moons) translates it to world space.

@module orbit

@vs orbit_vs
layout(binding=0) uniform orbit_vs_params {
    mat4 mvp;
};
layout(location=0) in vec3 aPos;
void main() {
    gl_Position = mvp * vec4(aPos, 1.0);
}
@end

@fs orbit_fs
layout(binding=1) uniform orbit_fs_params {
    vec4 color;   // rgb + alpha
};
out vec4 FragColor;
void main() {
    FragColor = color;
}
@end

@program orbit orbit_vs orbit_fs
