/*
 * ico20_edge_neighbor.c — Edge-neighbor table for the 20-face icosahedron
 *
 * Builds a neighbor table (face × edge → neighbor face), verifies correctness,
 * walks a neighbor path, measures face-center distances, and benchmarks lookups.
 *
 * Compile: gcc -O2 -std=c11 -lm -o runner/explore/ico20_edge_neighbor.exe runner/explore/ico20_edge_neighbor.c
 * Run:     runner/explore/ico20_edge_neighbor.exe
 * Expect:  6/6 PASS
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ── constants ─────────────────────────────────────────────────────── */
#define N_VERT  12
#define N_FACE  20
#define N_EDGE  30
#define PHI     ((1.0 + sqrt(5.0)) / 2.0)   /* golden ratio ≈ 1.618 */

/* ── icosahedron data ──────────────────────────────────────────────── */
/* 12 vertices: cyclic permutations of (0, ±1, ±φ), ordered so that the
   FACES[] table below yields equilateral triangles (edge length = 2). */
static const double VERTICES[N_VERT][3] = {
    /*  0 */ {  0,  1,  PHI},   /*  1 */ {  0, -1,  PHI},
    /*  2 */ {  PHI, 0,  1},   /*  3 */ {  1,  PHI, 0},
    /*  4 */ { -1,  PHI, 0},   /*  5 */ { -PHI, 0,  1},
    /*  6 */ {  1, -PHI, 0},   /*  7 */ {  PHI, 0, -1},
    /*  8 */ {  0,  1, -PHI},  /*  9 */ { -PHI, 0, -1},
    /* 10 */ { -1, -PHI, 0},   /* 11 */ {  0, -1, -PHI}
};

/* 20 triangular faces (vertex indices, counter-clockwise outward) */
static const int FACES[N_FACE][3] = {
    /* top ring  (around vertex 0) */
    { 0,  1,  2}, { 0,  2,  3}, { 0,  3,  4}, { 0,  4,  5}, { 0,  5,  1},
    /* middle ring 1 */
    { 1,  6,  2}, { 2,  7,  3}, { 3,  8,  4}, { 4,  9,  5}, { 5, 10,  1},
    /* middle ring 2 */
    { 6,  7,  2}, { 7,  8,  3}, { 8,  9,  4}, { 9, 10,  5}, {10,  6,  1},
    /* bottom ring (around vertex 11) */
    { 6, 11,  7}, { 7, 11,  8}, { 8, 11,  9}, { 9, 11, 10}, {10, 11,  6}
};

/* ── neighbour table ───────────────────────────────────────────────── */
/* neighbor[face][edge] = face index of the neighbour sharing that edge.
   edge 0 = FACES[face][0]-FACES[face][1], etc.                      */
static int neighbor[N_FACE][3];

/* ── helpers ───────────────────────────────────────────────────────── */
static int edge_key(int a, int b)
{
    /* canonical edge key (smaller index first) */
    return a < b ? a * N_VERT + b : b * N_VERT + a;
}

static void build_neighbor_table(void)
{
    /* edge_map: for each canonical edge_key, record up to 2 faces */
    int map_size = N_VERT * N_VERT;
    int *count  = (int *)calloc(map_size, sizeof(int));
    int *face_a = (int *)calloc(map_size, sizeof(int));
    int *face_b = (int *)calloc(map_size, sizeof(int));

    memset(count, 0, map_size * sizeof(int));

    for (int f = 0; f < N_FACE; f++) {
        for (int e = 0; e < 3; e++) {
            int v0 = FACES[f][e];
            int v1 = FACES[f][(e + 1) % 3];
            int key = edge_key(v0, v1);
            if (count[key] == 0) {
                face_a[key] = f;
            } else {
                face_b[key] = f;
            }
            count[key]++;
        }
    }

    /* fill neighbour table */
    memset(neighbor, -1, sizeof(neighbor));
    for (int f = 0; f < N_FACE; f++) {
        for (int e = 0; e < 3; e++) {
            int v0 = FACES[f][e];
            int v1 = FACES[f][(e + 1) % 3];
            int key = edge_key(v0, v1);
            int other = (face_a[key] == f) ? face_b[key] : face_a[key];
            neighbor[f][e] = other;
        }
    }

    free(count);
    free(face_a);
    free(face_b);
}

/* ── face centre (average of 3 vertex positions) ──────────────────── */
static void face_centre(int f, double c[3])
{
    c[0] = c[1] = c[2] = 0;
    for (int i = 0; i < 3; i++) {
        int v = FACES[f][i];
        c[0] += VERTICES[v][0];
        c[1] += VERTICES[v][1];
        c[2] += VERTICES[v][2];
    }
    c[0] /= 3.0;
    c[1] /= 3.0;
    c[2] /= 3.0;
}

static double vec_dist(const double a[3], const double b[3])
{
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}

/* ── tests ─────────────────────────────────────────────────────────── */
static int pass_count = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  [PASS] %s\n", name);
        pass_count++;
    } else {
        printf("  [FAIL] %s\n", name);
    }
}

int main(void)
{
    printf("=== ico20_edge_neighbor — 20-face icosahedron neighbour table ===\n\n");

    build_neighbor_table();

    /* ---- TEST 1: every face has exactly 3 valid neighbours ---- */
    {
        int ok = 1;
        for (int f = 0; f < N_FACE; f++) {
            for (int e = 0; e < 3; e++) {
                int n = neighbor[f][e];
                if (n < 0 || n >= N_FACE || n == f) { ok = 0; break; }
            }
        }
        check(ok, "Test 1: every face has 3 valid neighbours");
    }

    /* ---- TEST 2: neighbour relation is symmetric ---- */
    {
        int ok = 1;
        for (int f = 0; f < N_FACE; f++) {
            for (int e = 0; e < 3; e++) {
                int n = neighbor[f][e];
                /* find which edge of n points back to f */
                int found = 0;
                for (int e2 = 0; e2 < 3; e2++) {
                    if (neighbor[n][e2] == f) { found = 1; break; }
                }
                if (!found) { ok = 0; break; }
            }
        }
        check(ok, "Test 2: symmetry (if A neighbours B then B neighbours A)");
    }

    /* ---- TEST 3: each edge is shared exactly twice ---- */
    {
        int map_size = N_VERT * N_VERT;
        int *count = (int *)calloc(map_size, sizeof(int));
        memset(count, 0, map_size * sizeof(int));
        for (int f = 0; f < N_FACE; f++) {
            for (int e = 0; e < 3; e++) {
                int v0 = FACES[f][e], v1 = FACES[f][(e + 1) % 3];
                count[edge_key(v0, v1)]++;
            }
        }
        int ok = 1;
        int edge_count = 0;
        for (int i = 0; i < map_size; i++) {
            if (count[i] > 0) {
                edge_count++;
                if (count[i] != 2) { ok = 0; break; }
            }
        }
        check(ok && edge_count == N_EDGE,
              "Test 3: each edge shared exactly twice (30 edges)");
        printf("        (detected %d distinct edges)\n", edge_count);
        free(count);
    }

    /* ---- TEST 4: walk path — start at face 0, follow edge 0 repeatedly ---- */
    {
        printf("\n--- Path walk: start face 0, follow edge 0 each step ---\n");
        int path[N_FACE + 2];
        int visited[N_FACE];
        memset(visited, 0, sizeof(visited));
        int cur = 0, len = 0;
        while (!visited[cur] && len < N_FACE) {
            path[len++] = cur;
            visited[cur] = 1;
            cur = neighbor[cur][0];
        }
        path[len++] = cur;  /* one more to show where we land */

        printf("  Length of open path: %d faces\n", len - 1);
        printf("  Path: ");
        for (int i = 0; i < len; i++) {
            printf("%d", path[i]);
            if (i < len - 1) printf(" → ");
        }
        printf("\n");

        /* On an icosahedron, following one "edge 0" ring gives 5 faces
           (pentagon around one vertex). Verify cycle length. */
        check(len == 6, "Test 4: path length = 5+1 (pentagonal cycle)");
    }

    /* ---- TEST 5: face-centre distances for neighbours are equal ---- */
    {
        double centres[N_FACE][3];
        for (int f = 0; f < N_FACE; f++)
            face_centre(f, centres[f]);

        double d0 = vec_dist(centres[0], centres[neighbor[0][0]]);
        double eps = 1e-12;
        int ok = 1;
        for (int f = 0; f < N_FACE; f++) {
            for (int e = 0; e < 3; e++) {
                double d = vec_dist(centres[f], centres[neighbor[f][e]]);
                if (fabs(d - d0) > eps) { ok = 0; break; }
            }
        }
        printf("\n  Uniform neighbour distance = %.12f\n", d0);
        check(ok, "Test 5: all neighbour centre distances equal");
    }

    /* ---- TEST 6: benchmark — 1 M lookups ---- */
    {
        const long N = 1000000;
        volatile int sink = 0;
        clock_t t0 = clock();
        for (long i = 0; i < N; i++) {
            int f = (int)(i % N_FACE);
            int e = (int)(i % 3);
            sink += neighbor[f][e];
        }
        clock_t t1 = clock();
        double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;
        printf("\n  Benchmark: %ld lookups in %.4f s (%.1f M/s)\n",
               N, elapsed, N / elapsed / 1e6);
        check(elapsed < 10.0, "Test 6: benchmark completes in < 10 s");
    }

    /* ---- summary ---- */
    printf("\n=== %d/6 PASS ===\n", pass_count);
    return pass_count == 6 ? 0 : 1;
}
