#pragma once

/* Tiny CPU-side 3D value noise + fbm, matching the GLSL noise in
 * assets/shaders/sun.glsl byte-for-byte (same hash constants, same
 * smoothstep interpolation). Used to bake per-vertex biome colours
 * into the planet meshes at init time. Not for hot-path use — call
 * during mesh generation, not per frame. */

#include "HandmadeMath.h"

float noise_hash3(HMM_Vec3 p);
float noise_value3(HMM_Vec3 p);

/* Standard fractal Brownian motion: `octaves` iterations, gain 0.5,
 * lacunarity 2.0. Returns a value roughly in [0, 1]. */
float noise_fbm3(HMM_Vec3 p, int octaves);
