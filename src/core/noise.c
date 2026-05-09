#include "noise.h"

#include <math.h>

static float fract_f(float x) { return x - floorf(x); }

float noise_hash3(HMM_Vec3 p)
{
    /* Mirrors the hash3() in assets/shaders/sun.glsl. The constants
     * (127.1, 311.7, 74.7, 43758.5453) are the algorithm's identity —
     * arbitrary primes that make the hash decorrelate. Per CLAUDE.md
     * those stay inline rather than getting promoted to YAML. */
    float d = p.X * 127.1f + p.Y * 311.7f + p.Z * 74.7f;
    return fract_f(sinf(d) * 43758.5453f);
}

float noise_value3(HMM_Vec3 p)
{
    HMM_Vec3 i = { .Elements = { floorf(p.X), floorf(p.Y), floorf(p.Z) } };
    HMM_Vec3 f = { .Elements = { p.X - i.X,   p.Y - i.Y,   p.Z - i.Z   } };

    /* Smoothstep interpolation (3f² − 2f³). */
    HMM_Vec3 u = {
        .Elements = {
            f.X * f.X * (3.0f - 2.0f * f.X),
            f.Y * f.Y * (3.0f - 2.0f * f.Y),
            f.Z * f.Z * (3.0f - 2.0f * f.Z),
        }
    };

    float c000 = noise_hash3(i);
    float c100 = noise_hash3((HMM_Vec3){ .Elements = { i.X + 1, i.Y,     i.Z     } });
    float c010 = noise_hash3((HMM_Vec3){ .Elements = { i.X,     i.Y + 1, i.Z     } });
    float c110 = noise_hash3((HMM_Vec3){ .Elements = { i.X + 1, i.Y + 1, i.Z     } });
    float c001 = noise_hash3((HMM_Vec3){ .Elements = { i.X,     i.Y,     i.Z + 1 } });
    float c101 = noise_hash3((HMM_Vec3){ .Elements = { i.X + 1, i.Y,     i.Z + 1 } });
    float c011 = noise_hash3((HMM_Vec3){ .Elements = { i.X,     i.Y + 1, i.Z + 1 } });
    float c111 = noise_hash3((HMM_Vec3){ .Elements = { i.X + 1, i.Y + 1, i.Z + 1 } });

    float x00 = c000 * (1.0f - u.X) + c100 * u.X;
    float x10 = c010 * (1.0f - u.X) + c110 * u.X;
    float x01 = c001 * (1.0f - u.X) + c101 * u.X;
    float x11 = c011 * (1.0f - u.X) + c111 * u.X;
    float y0  = x00  * (1.0f - u.Y) + x10  * u.Y;
    float y1  = x01  * (1.0f - u.Y) + x11  * u.Y;
    return    y0    * (1.0f - u.Z) + y1    * u.Z;
}

float noise_fbm3(HMM_Vec3 p, int octaves)
{
    float v = 0.0f;
    float a = 0.5f;
    HMM_Vec3 q = p;
    for (int i = 0; i < octaves; i++) {
        v += a * noise_value3(q);
        q = (HMM_Vec3){ .Elements = { q.X * 2.0f, q.Y * 2.0f, q.Z * 2.0f } };
        a *= 0.5f;
    }
    return v;
}
