#include "goldberg.h"

#include <math.h>
#include <string.h>

/* Internal scratch — sized for subdiv 3, the biggest level we ship in
 * uint16-indexed render buffers (see GOLDBERG_MAX_CELLS in the header).
 *
 *   subdiv 0:  12 verts,  20 tris
 *   subdiv 1:  42 verts,  80 tris
 *   subdiv 2: 162 verts, 320 tris
 *   subdiv 3: 642 verts, 1280 tris
 */
#define ICO_MAX_VERTS  642
#define ICO_MAX_TRIS  1280
#define EDGE_HASH_BITS  14
#define EDGE_HASH_SIZE  (1u << EDGE_HASH_BITS)
#define EDGE_HASH_MASK  (EDGE_HASH_SIZE - 1)

typedef struct {
    int  a, b;       /* sorted (min, max) */
    int  mid;        /* index of the midpoint vertex */
    bool used;
} edge_entry_t;

static HMM_Vec3      g_verts[ICO_MAX_VERTS];
static int           g_vert_count;
static int           g_tris[ICO_MAX_TRIS][3];
static int           g_tri_count;
static edge_entry_t  g_edges[EDGE_HASH_SIZE];

/* ---- helpers ---- */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static uint32_t edge_hash(int a, int b)
{
    int x = imin(a, b);
    int y = imax(a, b);
    /* Two large multipliers + xor — same trick as the noise hash. */
    uint32_t h = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u);
    return h & EDGE_HASH_MASK;
}

static int get_or_create_midpoint(int a, int b)
{
    int x = imin(a, b);
    int y = imax(a, b);
    uint32_t h = edge_hash(a, b);
    while (g_edges[h].used) {
        if (g_edges[h].a == x && g_edges[h].b == y) return g_edges[h].mid;
        h = (h + 1) & EDGE_HASH_MASK;
    }
    HMM_Vec3 mid = HMM_NormV3(HMM_AddV3(g_verts[a], g_verts[b]));
    int idx = g_vert_count++;
    g_verts[idx]    = mid;
    g_edges[h].a    = x;
    g_edges[h].b    = y;
    g_edges[h].mid  = idx;
    g_edges[h].used = true;
    return idx;
}

static void init_icosahedron(void)
{
    g_vert_count = 0;
    g_tri_count  = 0;

    const float t = 1.61803398875f;   /* golden ratio */
    /* 12 base vertices — the standard icosahedron. Each is normalised
     * to the unit sphere as we copy it in. */
    HMM_Vec3 raw[12] = {
        { .Elements = { -1.0f,  t,    0.0f } },  /* 0  */
        { .Elements = {  1.0f,  t,    0.0f } },  /* 1  */
        { .Elements = { -1.0f, -t,    0.0f } },  /* 2  */
        { .Elements = {  1.0f, -t,    0.0f } },  /* 3  */
        { .Elements = {  0.0f, -1.0f,  t   } },  /* 4  */
        { .Elements = {  0.0f,  1.0f,  t   } },  /* 5  */
        { .Elements = {  0.0f, -1.0f, -t   } },  /* 6  */
        { .Elements = {  0.0f,  1.0f, -t   } },  /* 7  */
        { .Elements = {  t,    0.0f, -1.0f } },  /* 8  */
        { .Elements = {  t,    0.0f,  1.0f } },  /* 9  */
        { .Elements = { -t,    0.0f, -1.0f } },  /* 10 */
        { .Elements = { -t,    0.0f,  1.0f } },  /* 11 */
    };
    for (int i = 0; i < 12; i++) {
        g_verts[g_vert_count++] = HMM_NormV3(raw[i]);
    }

    /* Standard icosahedron faces, CCW from outside. */
    int base_tris[20][3] = {
        {0,11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7,10}, {0,10,11},
        {1, 5, 9}, {5,11, 4}, {11,10,2}, {10,7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4,11}, {6, 2,10}, {8, 6, 7}, {9, 8, 1},
    };
    for (int i = 0; i < 20; i++) {
        g_tris[g_tri_count][0] = base_tris[i][0];
        g_tris[g_tri_count][1] = base_tris[i][1];
        g_tris[g_tri_count][2] = base_tris[i][2];
        g_tri_count++;
    }
}

static void subdivide_once(void)
{
    /* Reset the edge cache for this subdivision pass — midpoint
     * indices only persist within a single pass. */
    memset(g_edges, 0, sizeof(g_edges));

    static int new_tris[ICO_MAX_TRIS][3];
    int        new_count = 0;

    for (int t = 0; t < g_tri_count; t++) {
        int a = g_tris[t][0], b = g_tris[t][1], c = g_tris[t][2];
        int ab = get_or_create_midpoint(a, b);
        int bc = get_or_create_midpoint(b, c);
        int ca = get_or_create_midpoint(c, a);

        new_tris[new_count][0] = a;  new_tris[new_count][1] = ab; new_tris[new_count][2] = ca; new_count++;
        new_tris[new_count][0] = b;  new_tris[new_count][1] = bc; new_tris[new_count][2] = ab; new_count++;
        new_tris[new_count][0] = c;  new_tris[new_count][1] = ca; new_tris[new_count][2] = bc; new_count++;
        new_tris[new_count][0] = ab; new_tris[new_count][1] = bc; new_tris[new_count][2] = ca; new_count++;
    }
    memcpy(g_tris, new_tris, sizeof(int) * 3 * new_count);
    g_tri_count = new_count;
}

bool goldberg_make(int subdiv,
                   goldberg_cell_t *out_cells, int max_cells, int *out_cell_count)
{
    if (subdiv < 0)                  subdiv = 0;
    if (subdiv > GOLDBERG_MAX_SUBDIV) subdiv = GOLDBERG_MAX_SUBDIV;

    init_icosahedron();
    for (int s = 0; s < subdiv; s++) {
        subdivide_once();
        if (g_vert_count > ICO_MAX_VERTS || g_tri_count > ICO_MAX_TRIS) {
            *out_cell_count = 0;
            return false;
        }
    }

    if (g_vert_count > max_cells) {
        *out_cell_count = 0;
        return false;
    }

    /* Adjacency: for each vertex, list the (up to 6) triangles
     * incident to it. The 12 original ico verts get 5; everyone
     * else gets 6. */
    static int adj[ICO_MAX_VERTS][6];
    static int adj_count[ICO_MAX_VERTS];
    memset(adj_count, 0, sizeof(int) * g_vert_count);

    for (int t = 0; t < g_tri_count; t++) {
        for (int k = 0; k < 3; k++) {
            int v = g_tris[t][k];
            if (adj_count[v] < 6) {
                adj[v][adj_count[v]++] = t;
            }
        }
    }

    /* Each ico vertex becomes one Goldberg cell. The cell's center
     * is the vertex; each corner is the centroid of one of the
     * adjacent triangles, projected to the unit sphere. */
    *out_cell_count = 0;
    for (int v = 0; v < g_vert_count; v++) {
        if (*out_cell_count >= max_cells) return false;
        goldberg_cell_t *cell = &out_cells[(*out_cell_count)++];
        cell->center       = g_verts[v];
        cell->corner_count = adj_count[v];

        /* Centroids of each adjacent triangle, projected to sphere. */
        HMM_Vec3 centroids[6];
        for (int i = 0; i < adj_count[v]; i++) {
            int   tr = adj[v][i];
            HMM_Vec3 sum = HMM_AddV3(HMM_AddV3(g_verts[g_tris[tr][0]],
                                               g_verts[g_tris[tr][1]]),
                                     g_verts[g_tris[tr][2]]);
            centroids[i] = HMM_NormV3(HMM_MulV3F(sum, 1.0f / 3.0f));
        }

        /* Order corners CCW around the vertex (outward-facing). Build
         * a tangent frame at the vertex normal, project each
         * centroid into 2D, sort by atan2 angle. */
        HMM_Vec3 N    = g_verts[v];
        HMM_Vec3 ref  = (fabsf(N.Y) > 0.9f)
            ? (HMM_Vec3){ .Elements = { 1.0f, 0.0f, 0.0f } }
            : (HMM_Vec3){ .Elements = { 0.0f, 1.0f, 0.0f } };
        HMM_Vec3 tng  = HMM_NormV3(HMM_Cross(ref, N));
        HMM_Vec3 btn  = HMM_Cross(N, tng);

        float angles[6];
        for (int i = 0; i < adj_count[v]; i++) {
            HMM_Vec3 d = HMM_SubV3(centroids[i], N);
            float    x = HMM_DotV3(d, tng);
            float    y = HMM_DotV3(d, btn);
            angles[i]  = atan2f(y, x);
        }

        /* Insertion sort indices by angle (small N, no need for
         * anything fancier). */
        int order[6];
        for (int i = 0; i < adj_count[v]; i++) order[i] = i;
        for (int i = 1; i < adj_count[v]; i++) {
            int   o  = order[i];
            float ka = angles[o];
            int   j  = i;
            while (j > 0 && angles[order[j - 1]] > ka) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = o;
        }

        for (int i = 0; i < adj_count[v]; i++) {
            cell->corners[i] = centroids[order[i]];
        }

        /* Neighbours: each edge of the cell runs between
         * corners[i] and corners[(i+1) % N]. Those corners are the
         * centroids of two ico triangles incident to vertex v. The
         * cell-graph neighbour across that edge is the *other*
         * vertex shared by those two triangles. */
        for (int i = 0; i < adj_count[v]; i++) {
            int t1 = adj[v][order[i]];
            int t2 = adj[v][order[(i + 1) % adj_count[v]]];
            int neighbour = -1;
            for (int k = 0; k < 3 && neighbour < 0; k++) {
                int vk = g_tris[t1][k];
                if (vk == v) continue;
                for (int j = 0; j < 3; j++) {
                    if (g_tris[t2][j] == vk) { neighbour = vk; break; }
                }
            }
            cell->neighbors[i] = neighbour;
        }
    }

    return true;
}
