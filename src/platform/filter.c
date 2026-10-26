// xBR 4x upscaling filter (after Hyllian's xBR level 2 algorithm).
//
// For each source pixel E, each of its four corners is checked for an edge
// passing through it, using colour distances over a 5x5 neighbourhood:
//
//        A1 B1 C1
//     A0 A  B  C  C4
//     D0 D  E  F  F4
//     G0 G  H  I  I4
//        G5 H5 I5
//
// The bottom-right corner is written out below; the other three use the same
// code on a rotated neighbourhood. Edges found are drawn into the 4x4 output
// block as full or partial (1/4, 1/2, 3/4) blends with the neighbour colour.
//
// The game draws with a 256-colour palette, so the distance between two
// colours is precomputed for every pair of palette entries.
#include "filter.h"
#include <stdlib.h>
#include <string.h>

#define SW 320
#define SH 200

static uint16_t dist[256][256];   // colour distance; bit 15 = "close enough to be equal"
static uint32_t cur_pal[256];
static bool have_pal;

static void build_tables(const uint32_t *pal) {
    int y[256], u[256], v[256];
    for (int i = 0; i < 256; i++) {
        int r = pal[i] >> 16 & 255, g = pal[i] >> 8 & 255, b = pal[i] & 255;
        y[i] = (299 * r + 587 * g + 114 * b) / 1000;
        u[i] = (-169 * r - 331 * g + 500 * b) / 1000 + 128;
        v[i] = (500 * r - 419 * g - 81 * b) / 1000 + 128;
    }
    for (int a = 0; a < 256; a++)
        for (int b = 0; b < 256; b++) {
            int dy = abs(y[a] - y[b]), du = abs(u[a] - u[b]), dv = abs(v[a] - v[b]);
            int d = 48 * dy + 7 * du + 6 * dv;     // weights of the hqx/xBR YUV metric
            if (d > 0x7FFF) d = 0x7FFF;
            dist[a][b] = (uint16_t)(d | (dy <= 48 && du <= 7 && dv <= 6 ? 0x8000 : 0));
        }
    memcpy(cur_pal, pal, sizeof cur_pal);
    have_pal = true;
}

#define DF(a, b) (dist[a][b] & 0x7FFF)
#define EQ(a, b) (dist[a][b] >> 15)
#define SAME(a, b) ((a) == (b) || cur_pal[a] == cur_pal[b])

static inline uint32_t blend(uint32_t dst, uint32_t src, int w) {   // w/4 of src
    uint32_t rb = ((dst & 0xFF00FF) * (4 - w) + (src & 0xFF00FF) * w) >> 2 & 0xFF00FF;
    uint32_t g = ((dst & 0x00FF00) * (4 - w) + (src & 0x00FF00) * w) >> 2 & 0x00FF00;
    return rb | g;
}

// Neighbourhood offsets for the bottom-right corner; rotation r turns them
// (and the output block) by r quarter turns.
enum { E, I, H, F, G, C, D, B, A, I4, I5, H5, F4, NB };
static const int8_t nb_off[NB][2] = {
    { 0, 0 }, { 1, 1 }, { 0, 1 }, { 1, 0 }, { -1, 1 }, { 1, -1 }, { -1, 0 }, { 0, -1 },
    { -1, -1 }, { 2, 1 }, { 1, 2 }, { 0, 2 }, { 2, 0 } };
static int8_t rot_off[4][NB][2];
static uint8_t rot_cell[4][16];      // bottom-right-frame cell -> output cell (y*4+x)

static void init_rotations(void) {
    for (int r = 0; r < 4; r++) {
        for (int n = 0; n < NB; n++) {
            int dx = nb_off[n][0], dy = nb_off[n][1];
            for (int k = 0; k < r; k++) { int t = dx; dx = dy; dy = -t; }
            rot_off[r][n][0] = (int8_t)dx; rot_off[r][n][1] = (int8_t)dy;
        }
        for (int c = 0; c < 16; c++) {
            int u = 2 * (c & 3) - 3, v = 2 * (c >> 2) - 3;      // cell centre, doubled
            for (int k = 0; k < r; k++) { int t = u; u = v; v = -t; }
            rot_cell[r][c] = (uint8_t)(((v + 3) / 2) * 4 + (u + 3) / 2);
        }
    }
}

// The source is copied with a 2-pixel border (edge pixels repeated), so the
// 5x5 neighbourhood never needs bounds checks.
#define PW (SW + 4)
static uint8_t padded[(SH + 4) * PW];
static int rot_lin[4][NB];           // rot_off as offsets into padded
static const uint32_t *f_pal;
static uint32_t *f_dst;

static void filter_rows(int y0, int y1) {
    const uint32_t *pal = f_pal;
    const int dw = SW * 4;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < SW; x++) {
            const uint8_t *s = padded + (y + 2) * PW + x + 2;
            uint32_t out[16];
            for (int c = 0; c < 16; c++) out[c] = pal[*s];

            for (int r = 0; r < 4; r++) {
                uint8_t p[NB];
                for (int n = 0; n < NB; n++) p[n] = s[rot_lin[r][n]];
                if (SAME(p[E], p[H]) || SAME(p[E], p[F])) continue;
                int ev = DF(p[E], p[C]) + DF(p[E], p[G]) + DF(p[I], p[H5]) + DF(p[I], p[F4]) + (DF(p[H], p[F]) << 2);
                int iv = DF(p[H], p[D]) + DF(p[H], p[I5]) + DF(p[F], p[I4]) + DF(p[F], p[B]) + (DF(p[E], p[I]) << 2);
                uint32_t px = pal[DF(p[E], p[F]) <= DF(p[E], p[H]) ? p[F] : p[H]];
                const uint8_t *cell = rot_cell[r];
                #define O(n) out[cell[n]]
                if (ev < iv && ((!EQ(p[F], p[B]) && !EQ(p[H], p[D])) ||
                                (EQ(p[E], p[I]) && !EQ(p[F], p[I4]) && !EQ(p[H], p[I5])) ||
                                EQ(p[E], p[G]) || EQ(p[E], p[C]))) {
                    int ke = DF(p[F], p[G]), ki = DF(p[H], p[C]);
                    bool ex2 = !SAME(p[E], p[C]) && !SAME(p[B], p[C]), ex3 = !SAME(p[E], p[G]) && !SAME(p[D], p[G]);
                    bool shallow = (ke << 1) <= ki && ex3, steep = ke >= (ki << 1) && ex2;
                    if (shallow && steep) {
                        O(13) = blend(O(13), px, 3); O(12) = blend(O(12), px, 1);
                        O(15) = O(14) = O(11) = px;
                        O(10) = O(3) = O(12); O(7) = O(13);
                    } else if (shallow) {
                        O(11) = blend(O(11), px, 3); O(13) = blend(O(13), px, 3);
                        O(10) = blend(O(10), px, 1); O(12) = blend(O(12), px, 1);
                        O(14) = O(15) = px;
                    } else if (steep) {
                        O(14) = blend(O(14), px, 3); O(7) = blend(O(7), px, 3);
                        O(10) = blend(O(10), px, 1); O(3) = blend(O(3), px, 1);
                        O(11) = O(15) = px;
                    } else {
                        O(11) = blend(O(11), px, 2); O(14) = blend(O(14), px, 2);
                        O(15) = px;
                    }
                } else if (ev <= iv) {
                    O(15) = blend(O(15), px, 2);
                }
                #undef O
            }
            uint32_t *d = f_dst + (y * 4) * dw + x * 4;
            for (int row = 0; row < 4; row++) memcpy(d + row * dw, out + row * 4, 16);
        }
}

// The picture is cut into horizontal bands filtered in parallel.
#define BANDS 4
#ifdef _WIN32
#include <windows.h>
static DWORD WINAPI band_main(LPVOID a) { int b = (int)(intptr_t)a; filter_rows(SH * b / BANDS, SH * (b + 1) / BANDS); return 0; }
static void run_bands(void) {
    HANDLE h[BANDS - 1];
    for (int b = 1; b < BANDS; b++) h[b - 1] = CreateThread(NULL, 0, band_main, (LPVOID)(intptr_t)b, 0, NULL);
    band_main(0);
    for (int b = 1; b < BANDS; b++) if (h[b - 1]) { WaitForSingleObject(h[b - 1], INFINITE); CloseHandle(h[b - 1]); } else band_main((LPVOID)(intptr_t)b);
}
#else
#include <pthread.h>
static void *band_main(void *a) { int b = (int)(intptr_t)a; filter_rows(SH * b / BANDS, SH * (b + 1) / BANDS); return NULL; }
static void run_bands(void) {
    pthread_t t[BANDS - 1];
    bool ok[BANDS - 1];
    for (int b = 1; b < BANDS; b++) ok[b - 1] = pthread_create(&t[b - 1], NULL, band_main, (void *)(intptr_t)b) == 0;
    band_main(0);
    for (int b = 1; b < BANDS; b++) if (ok[b - 1]) pthread_join(t[b - 1], NULL); else band_main((void *)(intptr_t)b);
}
#endif

void filter_xbr4(const uint8_t *src, const uint32_t *pal, uint32_t *dst) {
    static bool inited;
    if (!inited) {
        init_rotations();
        for (int r = 0; r < 4; r++)
            for (int n = 0; n < NB; n++) rot_lin[r][n] = rot_off[r][n][1] * PW + rot_off[r][n][0];
        inited = true;
    }
    if (!have_pal || memcmp(pal, cur_pal, sizeof cur_pal)) build_tables(pal);
    for (int y = -2; y < SH + 2; y++) {
        const uint8_t *row = src + (y < 0 ? 0 : y >= SH ? SH - 1 : y) * SW;
        uint8_t *d = padded + (y + 2) * PW;
        d[0] = d[1] = row[0];
        memcpy(d + 2, row, SW);
        d[SW + 2] = d[SW + 3] = row[SW - 1];
    }
    f_pal = pal;
    f_dst = dst;
    run_bands();
}
