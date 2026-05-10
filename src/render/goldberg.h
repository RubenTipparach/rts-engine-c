#pragma once

/* Goldberg polyhedron mesh — the dual of an icosahedron-subdivided
 * sphere. Each cell is a regular hexagon, except for 12 pentagons at
 * the original icosahedron vertex positions. This is the cell shape
 * Civilization-style 4X games use, and what the upstream RTS engine
 * generates per planet for unit placement / pathing.
 *
 * Subdivision N produces 10·4^N + 2 cells:
 *   N=0 → 12     (just the icosahedron — all pentagons)
 *   N=1 → 42
 *   N=2 → 162
 *   N=3 → 642
 *   N=4 → 2562
 *
 * GOLDBERG_MAX_CELLS caps the static buffers; bump it (and the
 * downstream biome-vbuf cap) if you want N=4. */

#include <stdbool.h>
#include <stdint.h>

#include "HandmadeMath.h"

#define GOLDBERG_MAX_SUBDIV    3
#define GOLDBERG_MAX_CELLS     642
#define GOLDBERG_CORNERS_MAX   6

typedef struct {
    HMM_Vec3 center;                          /* unit-sphere */
    HMM_Vec3 corners[GOLDBERG_CORNERS_MAX];
    int      corner_count;                    /* 5 (pentagon) or 6 (hexagon) */
} goldberg_cell_t;

/* Generate the Goldberg cell list for the given subdivision level.
 * Returns true if the static buffers fit. */
bool goldberg_make(int subdiv,
                   goldberg_cell_t *out_cells, int max_cells, int *out_cell_count);
