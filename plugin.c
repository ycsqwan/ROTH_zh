/*
 * ROTH.C Chinese localisation mod (plugin.dll)
 * ---------------------------------------------------------------------------
 * Target engine : rothc.exe (ROTH.C reconstruction engine, 32-bit i686)
 * Override     : draw_text_to_buffer  (canonical VA 0x14d04, eligible, no SMC)
 * Strategy     : conditional replace, registered OUTERMOST (priority 100).
 *                Higher priority = outer = dispatched first (see
 *                override_registry.c: slots are sorted descending, so a higher
 *                number runs before a lower one and a "replace" blocks only the
 *                links *inside* it). A line with no translation is handed to the
 *                rest of the chain with roth_next_draw_text_to_buffer(), so any
 *                lower-priority mod still sees every string we do not own;
 *                translated lines are rasterised from a TrueType CJK face via
 *                stb_truetype and we skip the engine's single-byte glyph loop.
 *
 * Why "replace" and not "wrap": draw_text_to_buffer is the innermost control-code
 * string renderer (menus, timed messages, dialogue panels, option menus, item
 * labels). Wrapping it would leave the original single-byte glyph loop in charge
 * of layout; replacing it lets us own line layout, line height and the ramp.
 *
 * Palette: the engine framebuffer is 8-bit indexed, so drawing is "write
 * palette indices". The original marks empty neighbours with literal index 1
 * (the outline) and blits ramp colours on top. We keep that convention.
 *
 * Game-RAM access: rb() reads a string byte. Pointers inside the game_ram
 * window go through the SDK's bounds-checked accessor (api->game_ram->u8);
 * pointers outside it -- the exe's own string/data pages, which the engine also
 * passes us -- are dereferenced flat, exactly as the engine's own RB() does.
 * See the "Game-RAM access" block below for why the two paths are both needed.
 *
 * Build (see Makefile):
 *   i686-w64-mingw32-gcc -shared -std=c11 -O2 -Wall -Wextra \
 *     -I<repo>/sdk/include -o plugin.dll plugin.c
 */

#include "roth_sdk.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Tunables
 * ========================================================================== */
#define ROTH_PLUGIN_ID  "io.github.ycsqwan.roth-chinese"
#define ROTH_PRIORITY   100   /* outermost: see the Strategy note above */

#define FONT_FILE       "WenQuanYi Bitmap Song 14px.ttf"
#define TABLE_FILE      "translations_zh.bin"
#define LOG_NAME        "chinese.log"

/* WQY Bitmap Song is a pixel-grid face: outline coordinates sit on a 100-unit
 * grid and unitsPerEm is 1500, so the face's NATIVE size is 1500/100 = 15 px.
 * It must be rasterised at exactly that em. At any other em the 100-unit grid
 * lands between pixel centres, the coverage stops being bilevel (0/255), and
 * blit_glyph()'s BITMAP_INK threshold then eats whole strokes -- that is what
 * made Cubic 11 come out as an unreadable smudge.
 *
 * WQY is the clean case: hhea ascent-descent is 1200 - (-300) = 1500, i.e.
 * exactly unitsPerEm, so stbtt_ScaleForPixelHeight(15) = 15/1500 = 0.01000 and
 * 100 units maps to exactly 1 px. (Cubic 11 does NOT have this property: its
 * ascent-descent is 1400 vs upem 1200, so an integer grid would need em 14.)
 * The 12/13/15/16 px files differ only in advance width, not in em.
 *
 * The engine lays menu lines out every `linestep` pixels (11 for the built-in
 * face, confirmed from consecutive draw_text_to_buffer dest addresses:
 * 0x398c2-0x37594, 0x3b442-0x398c2, ... are all exactly 0x1b80 = 11*640). */
#define FONT_EM         15.0f       /* px per em; MUST be the face's native em */
#define LINE_PAD        0           /* extra rows between lines (0 = exact)   */
#define OUTLINE_IDX     1           /* palette index used for the outline  */
/* Bitmap faces (Cubic 11 = a 12 px pixel grid) render at 100% coverage for
 * every inked pixel and 0% everywhere else, so antialiasing thresholds are the
 * wrong tool: an empty glyph comes back all zeros and an inked one all 0xff.
 * With a vector face these same values would destroy the outline, hence the
 * two tables below and the per-glyph branch in blit_glyph(). */
#define INK_THRESHOLD   190         /* coverage >= this counts as solid ink */
#define COVER_INK       128         /* coverage >= this is solid text ink   */
#define COVER_FRINGE    40          /* coverage >= this is a dark fringe   */
#define BITMAP_INK      128         /* bitmap face: ink / no-ink split     */
#define BITMAP_FRINGE   32          /* bitmap face: sub-threshold remnant  */
/* These faces are pixel grids (WQY Bitmap Song: 15 px em -> scale 0.01000,
 * i.e. 100 units = 1 px exactly). Autodetecting "bitmap vs vector" from the
 * coverage histogram is unreliable for a face sampled off-grid, so declare the
 * face type by name instead. A vector face still wants the fringe logic. */
#define FONT_IS_BITMAP  1
#define CJK_OUTLINE     0           /* 1 = also halo solid CJK pixels      */
#define MAX_LINES       16
#define MAX_LINE        256
#define MAX_UTF8        4096
#define TABLE_MAX       (16u * 1024u * 1024u)

/* canonical VA of g_font_descriptor (see src/engine/text_font.c) */
#define VA_FONT_DESC    0x70F12u

/* probe buffer used to harvest the engine's colour ramp */
#define PROBE_PITCH     512
#define PROBE_ROWS      32

/* Self-test banner. It is rasterised directly at the on-screen em (BANNER_EM)
 * and blitted 1:1 -- the old code rasterised at FONT_EM and then nearest-
 * neighbour magnified it BANNER_SCALE x, which is why every stroke came out
 * as a block of 3x3 squares. */
#define BANNER_SCALE    3           /* on-screen em = FONT_EM * BANNER_SCALE  */
#define BANNER_EM       (FONT_EM * BANNER_SCALE)

/* ==========================================================================
 * Logging
 * ========================================================================== */
static char g_log_path[1200];

static void logf_(const char *fmt, ...)
{
    FILE *f = fopen(g_log_path, "ab");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- queued logging ------------------------------------------------------
 * The override can run in the 70 Hz tick (TICK_ISR, where file I/O per line is
 * not acceptable), so it logs through logq(): format into a RAM buffer and let
 * on_frame_game drain it. logf_() stays for the contexts where fopen is fine.
 * ---------------------------------------------------------------------- */
#define LOGQ_MAX 8192
static char  g_logq[LOGQ_MAX];
static size_t g_logq_n;

static void logq(const char *fmt, ...)
{
    if (g_logq_n >= LOGQ_MAX) return;      /* drop rather than overflow */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_logq + g_logq_n, LOGQ_MAX - g_logq_n, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= LOGQ_MAX - g_logq_n) { g_logq_n = LOGQ_MAX - 1; return; }
    g_logq_n += (size_t)n;
}

static void logq_flush(void)
{
    if (!g_logq_n) return;
    FILE *f = fopen(g_log_path, "ab");
    if (!f) { g_logq_n = 0; return; }
    fwrite(g_logq, 1, g_logq_n, f);
    fclose(f);
    g_logq_n = 0;
}

/* ==========================================================================
 * Game-RAM access
 *
 * The engine hands draw_text_to_buffer() TWO kinds of string pointer, and they
 * need different handling:
 *
 *   1. A canonical VA + base (host address inside the game_ram window). Only
 *      the SDK accessor may touch these: plugin_loader.c documents that the
 *      sub-ranges of [0,size) which carry no named globals are "unmapped
 *      image-free (a plugin read there faults -- fail-process-wide, §10.6)".
 *      The accessor is bounds-checked (gr_u8 returns 0 out of range), so a
 *      bogus pointer reads as an empty string instead of killing the process.
 *
 *   2. A plain host pointer into the mapped image (the exe's own string/data
 *      pages, e.g. the menu text pool). These are OUTSIDE [base, base+size),
 *      so the accessor cannot reach them -- clamping the offset to the window
 *      would return 0 for every one of them and every menu line would fall
 *      through to English. The engine's own RB() is a flat dereference and the
 *      baseline mod relied on exactly that, which is why the baseline translated
 *      menus correctly.
 *
 * So: in-window -> SDK accessor; out-of-window -> flat dereference.
 * ========================================================================== */
static uintptr_t g_base;      /* canonical VA 0 as a host address; 0 = unknown */
static int       g_gr_ok;     /* base resolved -> rb() is usable               */
static uint32_t  g_ram_size;  /* game RAM bytes; 0 = unknown (set in ram_ready) */

static uint8_t rb(const struct roth_api_v1 *api, uint32_t hostptr)
{
    if (g_gr_ok && api->game_ram &&
        hostptr >= g_base &&
        (uint64_t)hostptr < (uint64_t)g_base + g_ram_size) {
        return api->game_ram->u8((uint32_t)(hostptr - g_base));
    }
    /* Out of window: a mapped image pointer, exactly as the engine reads it. */
    return *(volatile uint8_t *)(uintptr_t)hostptr;
}

/* ==========================================================================
 * Font face + glyph cache
 * ========================================================================== */
static stbtt_fontinfo g_font;
static int            g_font_ok;
static float          g_scale;
static int            g_baseline;   /* px from line top down to the baseline */
static int            g_line_h;     /* px between baselines for CJK text     */
static int            g_asc_units;  /* ascent in font units (scale-independent) */
static int            g_desc_units; /* descent in font units                   */
static uint8_t       *g_font_blob;
static char           g_font_path[1200];
/* 1 = the face is a pixel grid (Cubic 11): coverage is 0/255, so blit_glyph
 * thresholds bilevel instead of treating mid-levels as an antialiased fringe. */
static int            g_bitmap_face;

typedef struct {
    uint32_t cp;           /* codepoint this slot currently holds          */
    int      ok;
    int      xoff, yoff, w, h;
    int      advance;      /* 26.6 */
    uint8_t *bits;
} GCache;

/* The cache is built ONCE, in on_game_ram_ready (MAIN context, where
 * allocation and stb_truetype calls are legal), from the exact set of
 * codepoints that appear in the translation table.
 *
 * draw_text_to_buffer carries context_flags = tick_isr_reachable
 * (sdk/abi/override_targets.json), so the override runs in TICK_ISR, where
 * MODDING_GUIDE.md says "no engine calls, be fast". An exact set plus a
 * sorted-array lookup gives the allocation-free guarantee that context needs:
 * every codepoint render_utf8() can ever be handed is already rasterised, so
 * get_glyph() below is a pure read.
 *
 * The old fixed 64-slot hash took `cp * 2654435761u % 64`, i.e. the low 6 bits
 * of the codepoint. CJK codepoints cluster on their low byte, so slots
 * collided constantly and every miss re-allocated and re-rasterised inside
 * the override -- the exact thing TICK_ISR forbids. */
static GCache   *g_glyphs;      /* rasterised slots, parallel to g_cps        */
static uint32_t *g_cps;        /* sorted unique codepoints from the table    */
static uint32_t  g_gc_n;       /* number of populated slots                  */
static GCache    g_gc_miss;    /* handed back for a codepoint outside the set */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) logf_("FATAL: out of memory (%lu bytes)", (unsigned long)n);
    return p;
}

static int load_font(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { logf_("font: cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); logf_("font: empty file"); return 0; }
    uint8_t *buf = (uint8_t *)xmalloc((size_t)size);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); logf_("font: short read"); return 0; }

    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    if (off < 0 || !stbtt_InitFont(&g_font, buf, off)) {
        free(buf);
        logf_("font: stbtt_InitFont rejected %s", path);
        return 0;
    }
    g_font_blob = buf;                      /* stb keeps a pointer into it */
    g_scale = stbtt_ScaleForPixelHeight(&g_font, (float)FONT_EM);

    /* TrueType bitmap boxes are BASELINE-relative and normally start *above*
     * the baseline (y0 < 0). Treating yoff as "from the top of the line" draws
     * every glyph off the top of the destination: only the bottom 1px sliver
     * survives (this was the "banner 176x5" bug). Normalise here so yoff is
     * measured from the line top. */
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    g_asc_units = asc;
    g_desc_units = desc;
    g_baseline = (int)((float)asc * g_scale + 0.5f);
    g_line_h   = (int)((float)(asc - desc) * g_scale + 0.5f) + LINE_PAD;
    if (g_line_h < FONT_EM) g_line_h = FONT_EM;

    g_font_ok = 1;
    logf_("font: loaded %s (%ld bytes, scale=%.3f ascent=%d descent=%d "
          "baseline=%d line_h=%d)",
          path, size, g_scale, asc, desc, g_baseline, g_line_h);

    /* Measure a real CJK glyph box: the line step is 11 px, so if this box is
     * taller than 11 the next menu line will still paint over it. */
    {
        int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        stbtt_GetCodepointBitmapBox(&g_font, 0x4e2d, g_scale, g_scale,
                                    &bx0, &by0, &bx1, &by1);
        logf_("font: CJK box U+4E2D w=%d h=%d y[%d..%d] -> glyph rows %d..%d "
              "(line_h=%d)",
              bx1 - bx0, by1 - by0, by0, by1,
              g_baseline + by0, g_baseline + by1, g_line_h);

        /* Distinguish a pixel-grid face from a vector face by coverage
         * distribution: a bitmap font paints 0 or 255, a vector font paints a
         * spread of greys. Threshold policy depends on it. */
        int nmid = 0, ntot = 0, nw = 0, nh = 0;
        uint8_t *probe = (uint8_t *)xmalloc((size_t)(bx1 - bx0) * (by1 - by0));
        if (probe) {
            stbtt_MakeCodepointBitmap(&g_font, probe, bx1 - bx0, by1 - by0,
                                      bx1 - bx0, g_scale, g_scale, 0x4e2d);
            nw = bx1 - bx0; nh = by1 - by0;
            for (int i = 0; i < nw * nh; i++) {
                ntot++;
                if (probe[i] > BITMAP_FRINGE && probe[i] < BITMAP_INK) nmid++;
            }
            free(probe);
        }
        g_bitmap_face = (FONT_IS_BITMAP != 0);
        logf_("font: coverage probe %dx%d, mid-level px %d/%d -> %s",
              nw, nh, nmid, ntot,
              g_bitmap_face ? "BITMAP (bilevel)" : "VECTOR (antialiased)");
    }
    return 1;
}

static uint32_t utf8_decode(const unsigned char *s, int *out_len)
{
    unsigned int c = s[0];
    unsigned int min_seq = 0;
    int need;
    unsigned int cp;

    if (c < 0x80u) { cp = c; need = 0; }
    else if ((c & 0xe0u) == 0xc0u) { cp = c & 0x1fu; need = 1; min_seq = 0x80u; }
    else if ((c & 0xf0u) == 0xe0u) { cp = c & 0x0fu; need = 2; min_seq = 0x800u; }
    else if ((c & 0xf8u) == 0xf0u) { cp = c & 0x07u; need = 3; min_seq = 0x10000u; }
    else {
        if (out_len) *out_len = 1;
        return 0xfffdu;
    }

    for (int i = 0; i < need; i++) {
        if ((s[1 + i] & 0xc0u) != 0x80u) {
            if (out_len) *out_len = 1;
            return 0xfffdu;
        }
        cp = (cp << 6) | (s[1 + i] & 0x3fu);
    }
    if (cp < min_seq || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
        if (out_len) *out_len = 1;
        return 0xfffdu;
    }
    if (out_len) *out_len = need + 1;
    return cp;
}

/* Fill one slot. Only ever called from glyph_prewarm(), i.e. from
 * on_game_ram_ready -- never from the override. */
static void glyph_rasterise(GCache *g, uint32_t cp)
{
    int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    memset(g, 0, sizeof *g);
    g->cp = cp;
    stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, &lsb);
    stbtt_GetCodepointBitmapBox(&g_font, (int)cp, g_scale, g_scale, &x0, &y0, &x1, &y1);

    g->w     = x1 - x0;
    g->h     = y1 - y0;
    g->xoff  = x0;
    g->yoff  = g_baseline + y0;      /* from line top, not from baseline */
    g->advance = adv ? (int)((float)adv * g_scale * 64.0f) : (int)(FONT_EM * 64.0f);
    if (g->w <= 0 || g->h <= 0 || g->w > 64 || g->h > 64) {
        g->w = g->h = 0; g->ok = 1; return;
    }

    g->bits = (uint8_t *)xmalloc((size_t)g->w * (size_t)g->h);
    if (!g->bits) { g->ok = 0; return; }
    stbtt_MakeCodepointBitmap(&g_font, g->bits, g->w, g->h, g->w,
                              g_scale, g_scale, (int)cp);
    g->ok = 1;
}

/* Pure read: no allocation, no rasterisation. Safe in TICK_ISR. */
static GCache *get_glyph(uint32_t cp)
{
    if (!g_cps || !g_gc_n) return &g_gc_miss;
    uint32_t lo = 0, hi = g_gc_n;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (g_cps[mid] < cp)      lo = mid + 1;
        else if (g_cps[mid] > cp) hi = mid;
        else                      return &g_glyphs[mid];
    }
    return &g_gc_miss;
}

/* Declared here so glyph_prewarm() can read the blob it is about to
 * rasterise; defined with the rest of the translation table further down. */
static uint8_t *g_tbl_strs;
static uint32_t g_tbl_size;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (int)(x > y) - (int)(x < y);
}

/* Rasterise every codepoint that can reach render_utf8(). The override only
 * ever renders translation-table values, so the table IS the complete working
 * set -- pre-building it here is what makes the override allocation-free.
 * Runs in on_game_ram_ready (MAIN context). */
static int glyph_prewarm(void)
{
    if (!g_font_ok || !g_tbl_strs || g_tbl_size == 0) {
        logf_("glyph: prewarm skipped (font_ok=%d table=%u bytes)",
              g_font_ok, g_tbl_size);
        return 0;
    }

    /* Count lead bytes first so the scratch array is exactly sized. */
    size_t cap = 0;
    for (uint32_t i = 0; i < g_tbl_size; i++)
        if ((g_tbl_strs[i] & 0xc0u) != 0x80u) cap++;
    if (!cap) return 0;

    uint32_t *tmp = (uint32_t *)xmalloc(cap * sizeof *tmp);
    if (!tmp) return 0;

    size_t n = 0;
    for (uint32_t i = 0; i < g_tbl_size; i++) {
        if ((g_tbl_strs[i] & 0xc0u) == 0x80u) continue;   /* continuation byte */
        int len = 0;
        uint32_t cp = utf8_decode(g_tbl_strs + i, &len);
        if (len < 1) len = 1;
        if (cp) tmp[n++] = cp;                            /* skip the NUL separator */
        i += (uint32_t)(len - 1);
    }
    if (!n) { free(tmp); return 0; }

    qsort(tmp, n, sizeof *tmp, cmp_u32);
    size_t uniq = 0;
    for (size_t i = 0; i < n; i++)
        if (i == 0 || tmp[i] != tmp[i - 1]) tmp[uniq++] = tmp[i];

    g_cps    = (uint32_t *)xmalloc((size_t)uniq * sizeof *g_cps);
    g_glyphs = (GCache *)xmalloc((size_t)uniq * sizeof *g_glyphs);
    if (!g_cps || !g_glyphs) { free(tmp); free(g_cps); g_cps = NULL; return 0; }
    memcpy(g_cps, tmp, (size_t)uniq * sizeof *g_cps);
    free(tmp);

    g_gc_n = (uint32_t)uniq;
    int failed = 0;
    for (uint32_t i = 0; i < g_gc_n; i++) {
        glyph_rasterise(&g_glyphs[i], g_cps[i]);
        if (!g_glyphs[i].ok) failed++;
    }
    logf_("glyph: prewarmed %u unique codepoints (%d raster failures)",
          g_gc_n, failed);
    return 1;
}

/* ==========================================================================
 * Engine colour ramp
 * --------------------------------------------------------------------------
 * The original renderer picks its colour per glyph *row* from a static 12-entry
 * ramp:   ramp[i] = fsb[(i << 8) + colour]
 * where fsb is the DOS arena "font shade base" (g_os_sel_base, not exposed).
 * We cannot reach fsb from the plugin, so we harvest the ramp by asking the
 * recorded original body to render a probe string of known glyphs into a
 * scratch buffer, then read the palette indices back out.
 *
 * The probe is built from the game's own 176-glyph bitmap font descriptor
 * (g_font_descriptor, canon VA 0x70F12), so no font data is duplicated here.
 *
 * Descriptor layout (little-endian):
 *   u16 max_char, u16 linestep, u16 default_adv, int16 goff[256]
 *   glyph @ (desc + goff): u16 metrics, then rowcount bytes of bit rows
 *   metrics: mlow = width (bits 0..3, +1 if bit 5), adv = (mlow & 0xf) + 1
 *            mhigh = (topoff << 4) | (rowcount - 1)
 *   rows are MSB-first, `width` bits per row; width <= 4 packs two rows per byte
 * ========================================================================== */

typedef struct {
    int      valid;
    int      w, topoff, rc, adv;
    uint32_t rows;      /* canonical VA of the first bit row */
} DGlyph;

static DGlyph g_dg[256];
static int    g_dg_ready;
static int    g_max_char, g_linestep, g_default_adv;

typedef struct {
    int      valid;
    uint8_t  ch;
    uint32_t x;         /* pen x (dest-relative) of this probe glyph */
    int      row;       /* absolute row (dest-relative) to read       */
    int      dx;        /* x offset of a set bit inside that row      */
} ProbeEnt;

static ProbeEnt g_probe_ent[12];
static int      g_probe_n;
static uint8_t  g_probe_str[32];
static int      g_probe_len;

static uint32_t gr16(const struct roth_api_v1 *api, uint32_t off)
{
    return (uint32_t)api->game_ram->u8(off) | ((uint32_t)api->game_ram->u8(off + 1) << 8);
}

static int desc_first_set_x(const uint8_t *font, int c, int row)
{
    const DGlyph *d = &g_dg[c];
    if (!d->valid || row < 0 || row >= d->rc) return -1;
    const uint8_t *rows = font + (d->rows - VA_FONT_DESC);
    if (d->w <= 4) {
        uint8_t b = rows[row >> 1];          /* two rows per byte */
        for (int x = 0; x < d->w; x++) {
            int bit = (row & 1) ? (3 - x) : (7 - x);
            if ((b >> bit) & 1) return x;
        }
    } else {
        uint8_t b = rows[row];
        for (int x = 0; x < d->w; x++) {
            if ((b >> (7 - x)) & 1) return x;
        }
    }
    return -1;
}

static void desc_parse(const struct roth_api_v1 *api)
{
    const uint8_t *font = (const uint8_t *)api->game_ram->to_ptr(VA_FONT_DESC);
    if (!font) { logf_("desc: to_ptr(%05x) failed", VA_FONT_DESC); return; }

    g_max_char    = (int)gr16(api, VA_FONT_DESC + 0);
    g_linestep    = (int)gr16(api, VA_FONT_DESC + 2);
    g_default_adv = (int)gr16(api, VA_FONT_DESC + 4);
    if (g_max_char < 0 || g_max_char > 255) g_max_char = 255;
    if (g_linestep <= 0) g_linestep = 11;
    if (g_default_adv <= 0) g_default_adv = 3;

    int nglyphs = 0;
    for (int c = 0; c < 256; c++) {
        int goff = (int16_t)gr16(api, VA_FONT_DESC + 6 + c * 2);
        DGlyph *d = &g_dg[c];
        d->valid = 0;
        if (goff == 0) continue;
        uint32_t ga = VA_FONT_DESC + (int32_t)goff;
        uint32_t m = gr16(api, ga);
        uint32_t mlow = m & 0xffu, mhigh = m >> 8;
        d->w      = (int)((mlow & 0x0fu) + ((mlow & 0x20u) ? 1u : 0u));
        d->adv    = (int)((mlow & 0x0fu) + 1u);
        d->topoff = (int)(mhigh >> 4);
        d->rc     = (int)((mhigh & 0x0fu) + 1u);
        d->rows   = ga + 2u;
        d->valid  = 1;
        nglyphs++;
    }
    g_dg_ready = 1;
    logf_("desc: max_char=%d linestep=%d default_adv=%d glyphs=%d",
          g_max_char, g_linestep, g_default_adv, nglyphs);
}

/* Build the probe string: for ramp index i (0..11) pick the lowest character
 * whose glyph ramp range covers i and which has a set bit in that row. */
static void probe_build(const struct roth_api_v1 *api)
{
    const uint8_t *font = (const uint8_t *)api->game_ram->to_ptr(VA_FONT_DESC);
    if (!font || !g_dg_ready) return;

    uint32_t px = 1;                    /* pen starts at dest + pitch + 1 */
    int nc = 0;
    static uint8_t chars[16];

    for (int i = 0; i < 12; i++) {
        int found = -1, fdx = -1;
        for (int c = 0x0e; c <= 0xff && found < 0; c++) {
            const DGlyph *d = &g_dg[c];
            if (!d->valid) continue;
            if (d->topoff <= i && i < d->topoff + d->rc) {
                int dx = desc_first_set_x(font, c, i - d->topoff);
                if (dx >= 0) { found = c; fdx = dx; }
            }
        }
        if (found < 0) break;           /* higher ramp slots are unreachable */
        chars[nc++] = (uint8_t)found;
        g_probe_ent[i].valid = 1;
        g_probe_ent[i].ch    = (uint8_t)found;
        g_probe_ent[i].x     = px;
        g_probe_ent[i].row   = i;       /* absolute row = topoff + (i - topoff) */
        g_probe_ent[i].dx    = fdx;
        px += (uint32_t)g_dg[found].adv;
    }
    g_probe_n = nc;
    g_probe_str[0] = 0x01;              /* set colour ramp */
    g_probe_str[1] = 0;                 /* colour patched per call */
    memcpy(g_probe_str + 2, chars, (size_t)nc);
    g_probe_str[2 + nc] = 0;            /* end of string */
    g_probe_len = 2 + nc + 1;
    logf_("probe: %d ramp slots harvestable", g_probe_n);
}

typedef void (ROTH_CDECL *draw_text_fn)(uint32_t, uint32_t, uint32_t, uint32_t);

static uint8_t      g_ramp[256][12];
static uint8_t      g_ramp_ok[256];
static uint8_t      g_ramp_pend[256];   /* harvest requested, not done yet  */
static draw_text_fn g_draw_orig;        /* recorded original body (entry+5)  */

/* Called from the override. This runs in whatever context the original ran in
 * -- sdk/MODDING_GUIDE.md: "no engine calls, be fast" -- so it must not render
 * anything itself. It only makes sure g_ramp[colour] holds *something* usable
 * and books the real harvest for on_frame_game. Until that lands the colour
 * renders flat; the colours the game actually uses are pre-harvested on the
 * first frame, so in practice this fallback is never visible. */
static void ramp_prefetch(uint8_t colour)
{
    if (g_ramp_ok[colour]) return;
    g_ramp_pend[colour] = 1;
    for (int i = 0; i < 12; i++) g_ramp[colour][i] = colour;
}

/* Called from on_frame_game, where engine calls are legal. Renders the probe
 * string with the recorded original body and reads the palette indices back to
 * learn the real 12-step shade ramp for this text colour. */
static void ramp_harvest(uint8_t colour)
{
    uint8_t ramp[12];
    for (int i = 0; i < 12; i++) ramp[i] = colour;   /* flat fallback */

    if (g_draw_orig && g_probe_n > 0) {
        /* Single writer: only on_frame_game touches this, and it is not
         * re-entered from itself. (The override's scratch lives on the stack.) */
        static uint8_t scratch[PROBE_PITCH * PROBE_ROWS];
        memset(scratch, 0, sizeof scratch);
        g_probe_str[1] = colour;
        g_draw_orig((uint32_t)(uintptr_t)g_probe_str,
                    (uint32_t)(uintptr_t)scratch, PROBE_PITCH, 4u /* skip prepass */);

        int got = 0;
        for (int k = 0; k < g_probe_n; k++) {
            uint32_t off = g_probe_ent[k].x + (uint32_t)g_probe_ent[k].dx
                         + (uint32_t)g_probe_ent[k].row * PROBE_PITCH;
            if (off < sizeof scratch && scratch[off]) {
                ramp[g_probe_ent[k].row] = scratch[off];
                got++;
            }
        }
        if (!got) {
            for (int i = 0; i < 12; i++) ramp[i] = colour;
        } else {
            for (int i = g_probe_n; i < 12; i++) ramp[i] = ramp[g_probe_n - 1];
        }
        logf_("ramp[%02x] = %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
              colour, ramp[0], ramp[1], ramp[2], ramp[3], ramp[4], ramp[5],
              ramp[6], ramp[7], ramp[8], ramp[9], ramp[10], ramp[11]);
    }
    memcpy(g_ramp[colour], ramp, sizeof ramp);
    g_ramp_ok[colour] = 1;
    g_ramp_pend[colour] = 0;
}

/* ==========================================================================
 * Translation table
 * --------------------------------------------------------------------------
 * Flat binary blob, generated by tools/make_table.py:
 *
 *   char     magic[8]   "ROTHZH01"
 *   uint32   count
 *   uint32   entries_off
 *   uint32   strings_off  (total size of the strings blob)
 *   entries: count x { uint64 hash; uint32 off; uint32 len; }
 *   strings: UTF-8 bytes, NUL-terminated per entry
 *
 * Lookup key: the plain text of a single *line* (control bytes stripped,
 * newlines kept as '\n'). Exact match, no case folding.
 * ========================================================================== */
static uint8_t *g_tbl_ents, *g_tbl_strs;
static uint32_t g_tbl_count, g_tbl_size;
static char     g_table_path[1200];

static uint64_t hash_key(const char *s)
{
    /* FNV-1a 64 offset basis = 0xcbf29ce484222325 = 14695981039346656037.
     * This MUST be bit-identical to make_table.py's `0xcbf29ce484222325`;
     * an earlier version had ...0665603 (one digit short), which made every
     * hash mismatch the table and every lookup miss. */
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static void tbl_free(void)
{
    free(g_tbl_ents); g_tbl_ents = NULL;
    free(g_tbl_strs); g_tbl_strs = NULL;
    g_tbl_count = 0;
}

static int tbl_load(void)
{
    tbl_free();
    FILE *f = fopen(g_table_path, "rb");
    if (!f) { logf_("tbl: %s not found (pass-through mode)", g_table_path); return 0; }
    char magic[8];
    uint32_t hdr[3];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "ROTHZH01", 8) != 0 ||
        fread(hdr, 4, 3, f) != 3) {
        fclose(f); logf_("tbl: bad header"); return 0;
    }
    /* hdr[2] is the ABSOLUTE file offset where the value blob starts, not its
     * size. The blob length is (filesize - strs_at). Treating it as a size used
     * to truncate the blob to the header+entries length, so every entry whose
     * string offset fell past that point resolved to NULL. */
    uint32_t count = hdr[0], entries_off = hdr[1], strs_at = hdr[2];
    if (count > 1000000u) {
        fclose(f); logf_("tbl: implausible entry count"); return 0;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    if (fsz < 0 || (long)strs_at > fsz) {
        fclose(f); logf_("tbl: bad strings offset"); return 0;
    }
    uint32_t blob = (uint32_t)(fsz - (long)strs_at);   /* real value-blob size */
    if (blob > TABLE_MAX) {
        fclose(f); logf_("tbl: implausible string blob"); return 0;
    }
    g_tbl_size = blob;
    g_tbl_ents = (uint8_t *)xmalloc((size_t)count * 16u + 1u);
    g_tbl_strs = (uint8_t *)xmalloc((size_t)blob + 1u);
    if (!g_tbl_ents || !g_tbl_strs) { fclose(f); return 0; }
    fseek(f, (long)entries_off, SEEK_SET);
    if (fread(g_tbl_ents, 16u, count, f) != count) {
        fclose(f); logf_("tbl: entry read failed"); return 0;
    }
    fseek(f, (long)strs_at, SEEK_SET);
    if (fread(g_tbl_strs, 1, blob, f) != blob) {
        fclose(f); logf_("tbl: string read failed"); return 0;
    }
    fclose(f);
    g_tbl_strs[blob] = 0;
    g_tbl_count = count;
    logf_("tbl: loaded %u entries (%u bytes of values)", count, blob);
    return 1;
}

static const char *tbl_lookup(const char *key)
{
    if (!g_tbl_ents || !key || !*key) return NULL;
    uint64_t want = hash_key(key);
    uint32_t lo = 0, hi = g_tbl_count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        uint8_t *e = g_tbl_ents + (size_t)mid * 16u;
        uint64_t h = (uint64_t)e[0] | ((uint64_t)e[1] << 8) | ((uint64_t)e[2] << 16) |
                     ((uint64_t)e[3] << 24) | ((uint64_t)e[4] << 32) | ((uint64_t)e[5] << 40) |
                     ((uint64_t)e[6] << 48) | ((uint64_t)e[7] << 56);
        if (h < want) lo = mid + 1;
        else if (h > want) hi = mid;
        else {
            uint32_t off = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                           ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
            uint32_t len = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                           ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
            if (off >= g_tbl_size || len > g_tbl_size - off) return NULL;
            /* The blob stores the Chinese VALUE, so it must NOT be compared
             * against `key` (the English source). The 64-bit FNV hash already
             * identifies the key uniquely; the old memcmp made every lookup
             * miss because an English key is never byte-identical to its
             * Chinese translation. */
            return (const char *)g_tbl_strs + off;
        }
    }
    return NULL;
}

/* ==========================================================================
 * Rendering
 * ========================================================================== */
static void put_px(uint8_t *b, uint32_t pitch, uint32_t half, int dx, int dy, uint8_t c)
{
    /* `b` is the flat game-RAM address (RB()/WB() are identity), so a glyph
     * hanging past the bottom of the framebuffer would scribble over whatever
     * follows it. Clip instead of corrupting. */
    if (dy < 0 || dx < 0) return;
    int32_t off = (int32_t)dy * (int32_t)pitch + dx;
    if (g_ram_size && (uint32_t)off >= g_ram_size) return;
    b[off] = c;
    if (half && (!g_ram_size || (uint32_t)(off + (int32_t)half) < g_ram_size))
        b[off + (int32_t)half] = c;
}

/* A rasterised glyph at an arbitrary scale. GCache is the FONT_EM-sized cache
 * used by the in-game path; the self-test banner rasterises its own, larger,
 * glyphs so the overlay is never a magnified small bitmap. */
typedef struct {
    uint32_t cp;
    int      w, h, xoff, yoff, adv_px;
    uint8_t *bits;
} Rast;

static int rasterize(uint32_t cp, float scale, int baseline, Rast *r)
{
    int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    r->cp = cp;
    r->bits = NULL;
    r->w = r->h = r->xoff = r->yoff = r->adv_px = 0;
    stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, &lsb);
    stbtt_GetCodepointBitmapBox(&g_font, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
    r->xoff = x0;
    r->yoff = baseline + y0;          /* from line top, not from baseline */
    r->adv_px = adv ? (int)((float)adv * scale + 0.5f) : (int)scale;
    r->w = x1 - x0;
    r->h = y1 - y0;
    if (r->w <= 0 || r->h <= 0 || r->w > 512 || r->h > 512) { r->w = r->h = 0; return 1; }
    r->bits = (uint8_t *)xmalloc((size_t)r->w * (size_t)r->h);
    if (!r->bits) return 0;
    stbtt_MakeCodepointBitmap(&g_font, r->bits, r->w, r->h, r->w,
                              scale, scale, (int)cp);
    return 1;
}

static int is_cjk(uint32_t cp)
{
    return (cp >= 0x2e80u && cp <= 0x9fffu) ||   /* radicals .. CJK unified */
           (cp >= 0x3000u && cp <= 0x303fu) ||   /* CJK punctuation        */
           (cp >= 0xf900u && cp <= 0xfaffu) ||   /* compat ideographs      */
           (cp >= 0xff00u && cp <= 0xffefu);     /* fullwidth forms        */
}

/* Neighbourhood outline: only SOLID ink (cov >= INK_THRESHOLD) grows a halo.
 * Haloing the antialiased fringe too would thicken every 1 px stroke into a
 * 3 px blob, which is what made the overlay read as "bold mush".
 *
 * This is the ORIGINAL engine's convention (empty neighbours of a glyph are
 * marked with index 1) and it is only right for a 1-bit bitmap font. A CJK
 * glyph at ~15 px has ~1.5 px strokes, so doubling every stroke is fatal; CJK
 * gets the antialiased fringe instead (see blit_glyph). */
static void outline_pass(uint8_t *base, uint32_t pitch2, uint32_t half,
                         int pen_x, int pen_y,
                         const uint8_t *bits, int gw, int gh)
{
    static const int nb[8][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        {-1, -1}, {1, -1}, {-1, 1}, {1, 1}
    };
    if (!bits) return;
    for (int y = 0; y < gh; y++) {
        const uint8_t *row = bits + y * gw;
        for (int x = 0; x < gw; x++) {
            if (row[x] < INK_THRESHOLD) continue;
            int px = pen_x + x, py = pen_y + y;
            for (int k = 0; k < 8; k++) {
                int qx = px + nb[k][0], qy = py + nb[k][1];
                if (qx < 0 || qy < 0) continue;
                size_t qoff = (size_t)qy * (size_t)pitch2 + (size_t)qx;
                if (g_ram_size && qoff >= g_ram_size) continue;
                uint8_t *q = base + qoff;
                if (*q == 0) {
                    *q = OUTLINE_IDX;
                    if (half && (!g_ram_size || qoff + half < g_ram_size))
                        q[half] = OUTLINE_IDX;
                }
            }
        }
    }
}

/* Coverage is NOT a shade ramp: the harvested ramp is flat (it is the engine's
 * per-ROW shade table, not an alpha ramp), so mapping cov -> ramp[] painted
 * every antialiased fringe pixel at full text brightness. That is the single
 * biggest reason the overlay looked "too bold / merged": a 1.5 px stroke came
 * out ~4 px wide.
 *
 * Instead: threshold the coverage. CJK keeps a dark fringe (the antialiased
 * edge reads as a 1 px outline for free); Latin keeps the engine's halo.
 *
 * Bitmap faces need a different cut: a pixel-grid face (Cubic 11) yields 0 or
 * 255 per pixel, so any mid-level "fringe" is an artefact of a non-integer
 * scale, not a designed edge. Treat it as bilevel instead. */
static void blit_glyph(uint8_t *base, uint32_t pitch2, uint32_t half,
                       int pen_x, int pen_y,
                       const uint8_t *bits, int gw, int gh,
                       const uint8_t *ramp, int cjk)
{
    if (!bits) return;
    for (int y = 0; y < gh; y++) {
        const uint8_t *row = bits + y * gw;
        int dy = pen_y + y;
        if (dy < 0) continue;
        for (int x = 0; x < gw; x++) {
            uint8_t cov = row[x];
            uint8_t c;
            if (g_bitmap_face) {
                if (cov < BITMAP_INK) continue;
                c = ramp[11];
            } else {
                if (cov < COVER_FRINGE) continue;
                if (cov >= COVER_INK)      c = ramp[11];
                else if (cjk)              c = OUTLINE_IDX;
                else                       continue;   /* halo comes from outline_pass */
            }
            int dx = pen_x + x;
            if (dx < 0) continue;
            put_px(base, pitch2, half, dx, dy, c);
        }
    }
}

/* Render one UTF-8 string with its pen at (pen_x, pen_y), dest-relative. */
static void render_utf8(uint8_t *base, uint32_t pitch2, uint32_t half,
                        int pen_x, int pen_y, const char *s, const uint8_t *ramp)
{
    const unsigned char *p = (const unsigned char *)s;
    int x = pen_x;
    int guard = 0;
    while (*p && guard++ < MAX_UTF8) {
        int n = 0;
        uint32_t cp = utf8_decode(p, &n);
        if (n < 1) n = 1;
        p += (size_t)n;
        if (cp == '\n') { x = pen_x; pen_y += g_line_h; continue; }
        GCache *g = get_glyph(cp);
        if (!g->ok) continue;
        if (g->w && g->h) {
            int cjk = is_cjk(cp);
            if (CJK_OUTLINE || !cjk)
                outline_pass(base, pitch2, half, x + g->xoff, pen_y + g->yoff,
                             g->bits, g->w, g->h);
            blit_glyph(base, pitch2, half, x + g->xoff, pen_y + g->yoff,
                       g->bits, g->w, g->h, ramp, cjk);
        }
        x += (g->advance + 32) >> 6;
    }
}

/* ==========================================================================
 * Override
 * ========================================================================== */
static int g_ready;

static uint8_t ramp_colour(const struct roth_api_v1 *api, uint32_t str)
{
    for (int i = 0; i < 256; i++) {
        uint8_t c = rb(api, str + (uint32_t)i);
        if (c == 0) break;
        if (c == 0x01) return rb(api, str + (uint32_t)i + 1u);
    }
    return 0x0d;
}

/* self-test state -- declared here because on_draw_text_to_buffer's raw dump
 * needs them; banner_build/on_compose_tick sit further down. */
static int       g_selftest;
static uint32_t  g_draw_calls;      /* total on_draw_text_to_buffer invocations */
static int       g_banner_ok;
static int       g_banner_w, g_banner_h;
static int       g_banner_ink;     /* ramp colour the banner is drawn in */
static uint8_t  *g_banner_px;
static uint32_t  g_selftest_frames;

/* ---- cadence -----------------------------------------------------------
 * The old code paced its diagnostics with time(NULL) and a callback count.
 * sdk/README.md is explicit: use the engine's own 70 Hz counter at 0x90bcc
 * (game_ram->u16) rather than wall-clock or callback counts, so the pacing
 * tracks the game rather than the host. on_compose_tick logs the true tick
 * rate once every 70 ticks.
 * ---------------------------------------------------------------------- */
static uint32_t g_tick_n;      /* ticks seen by on_compose_tick            */
static uint16_t g_tick_t0;     /* engine 70 Hz counter at first tick        */
static uint32_t g_frame_n;     /* ticks seen by on_frame_game               */
static uint16_t g_frame_t0;    /* engine 70 Hz counter at first frame       */

static void ROTH_CDECL on_draw_text_to_buffer(struct roth_chain *chain,
                                              const struct roth_api_v1 *api,
                                              uint32_t str, uint32_t dest,
                                              uint32_t pitch, uint32_t flags)
{
    if (!g_ready || str == 0 || pitch == 0) {
        roth_next_draw_text_to_buffer(chain, str, dest, pitch, flags);
        return;
    }
    uint32_t str0 = str;                    /* the scan below advances `str` */

    g_draw_calls++;
    /* Unconditional call accounting: without this we cannot tell "the override
     * is never reached" from "it is reached but every lookup misses". */
    if (g_draw_calls <= 24 || g_draw_calls % 500u == 0) {
        char hex[160];
        size_t hn = 0;
        int ascii_n = 0;
        char ascii[40];
        for (int i = 0; i < 64 && hn < sizeof hex - 4; i++) {
            uint8_t c = rb(api, str + (uint32_t)i);
            if (c == 0) break;
            hn += (size_t)snprintf(hex + hn, sizeof hex - hn, "%02x ", c);
            if (c >= 0x20 && c < 0x7f && ascii_n < (int)sizeof ascii - 1)
                ascii[ascii_n++] = (char)c;
        }
        hex[hn] = 0;
        ascii[ascii_n] = 0;
        /* Diagnostic: dump the raw str pointer and whether it lands inside the
         * game_ram window (the two paths rb() takes). */
        int in_win = g_gr_ok && str >= g_base &&
                     (uint64_t)str < (uint64_t)g_base + g_ram_size;
        logq("draw#%u: str=%08x dest=%08x win=%d base=%08x ok=%d pitch=%u flags=%02x raw=%s ascii='%s'",
             g_draw_calls, str, dest, in_win, (uint32_t)g_base,
             g_gr_ok, pitch, flags, hex, ascii);
    }
    /* chain->base is the recorded original body (winner entry + 5). We keep it
     * for two jobs that genuinely need vanilla output: the shade-ramp harvest
     * (done later, in on_frame_game) and re-emitting an untranslated line's
     * indent. It is a code address in the statically mapped image, so unlike a
     * game_ram pool pointer it is stable for the whole process lifetime and safe
     * to cache. */
    if (!g_draw_orig) {
        g_draw_orig = chain->base;
        if (!g_draw_orig) {
            logq("override: no original body, pass-through");
            roth_next_draw_text_to_buffer(chain, str, dest, pitch, flags);
            return;
        }
        logq("override: original body @ %p", g_draw_orig);
    }

    uint32_t pitch2 = (flags & 1u) ? pitch * 2u : pitch;
    uint32_t half   = (flags & 1u) ? pitch : 0u;
    uint8_t  colour = ramp_colour(api, str);
    ramp_prefetch(colour);

    /* ---- scan the control-code string into per-line plain text ---- */
    /* Stack, not static: the override must be re-entrant-safe and this runs in
     * signal context (the 70 Hz tick can reach draw_text_to_buffer). ~4.7 KB on
     * the trap alt-stack (256 KB, see the host's sigaltstack setup) is fine. */
    char lines[MAX_LINES][MAX_LINE];
    const char *zh[MAX_LINES];
    int  adv[MAX_LINES];                    /* per-line 0x02 pen advance (px) */
    int nlines = 0;
    memset(adv, 0, sizeof adv);

    char plain[MAX_LINE];
    int pn = 0, nl = 0;
    for (;;) {
        uint8_t c = rb(api, str);
        str++;
        if (c == 0) break;
        if (c == 0x0d || c == 0x0a) {
            nl++;
            if (nl <= MAX_LINES && pn < MAX_LINE - 1) {
                memcpy(lines[nl - 1], plain, (size_t)pn);
                lines[nl - 1][pn] = 0;
                nlines = nl;
            }
            pn = 0;
            continue;
        }
        if (c < 0x0d) {
            if (c == 0x01) { str++; continue; }    /* <colour> */
            if (c == 0x02) {                        /* <lo> <hi> pen advance */
                if (nl < MAX_LINES) {
                    uint8_t lo = rb(api, str), hi = rb(api, str + 1u);
                    adv[nl] += (int)lo | ((int)hi << 8);
                }
                str += 2; continue;
            }
            continue;
        }
        if (pn < MAX_LINE - 1) plain[pn++] = (char)c;
    }
    if (pn > 0 && nl < MAX_LINES) {
        memcpy(lines[nl], plain, (size_t)pn);
        lines[nl][pn] = 0;
        nlines = nl + 1;
    }

    int translated = 0;
    for (int i = 0; i < nlines; i++) {
        if (!lines[i][0]) { zh[i] = NULL; continue; }
        const char *t = tbl_lookup(lines[i]);
        zh[i] = t;
        if (t) translated++;
    }

    /* ---- draw: line 0 at the original pen, later lines below it ---- */
    uint8_t *base = (uint8_t *)(uintptr_t)dest;
    uint8_t  line_h = (uint8_t)(g_line_h > 0 ? g_line_h : g_linestep);
    uint8_t  dflags = flags & ~1u;          /* always single-plane */

    /* Nothing in this block is translated: hand the ORIGINAL string back to the
     * rest of the chain (not to the raw body) so every control code -- colour
     * ramp and the 0x02 pen advance -- stays byte-for-byte vanilla, and any
     * lower-priority mod still gets to see the string. */
    if (translated == 0) {
        roth_next_draw_text_to_buffer(chain, str0, dest, pitch, flags);
        return;
    }

    for (int i = 0; i < nlines; i++) {
        uint32_t pen = dest + pitch2 + 1u + (uint32_t)i * (uint32_t)line_h * pitch2;
        if (zh[i]) {
            /* Start the Chinese where the English line started: the engine's
             * 0x02 code is the per-line indent / centring offset. */
            render_utf8(base, pitch2, half, 1 + adv[i], i * line_h + 1, zh[i], g_ramp[colour]);
        } else {
            /* Re-emit the indent so the fall-through line lands exactly where
             * vanilla put it (dropping it shifted every line left by adv). This
             * is the one place we call the recorded original body directly. We
             * deliberately do NOT use roth_next() here: this branch can run
             * several times for a single draw call, and every roth_next() spends
             * the chain cursor again, so only the first untranslated line would
             * reach the next link. We want the unmodified engine pen, not the
             * chain. */
            char buf[MAX_LINE + 8];
            size_t n = 0;
            buf[n++] = 0x01;
            buf[n++] = colour;
            buf[n++] = 0x02;
            buf[n++] = (char)(adv[i] & 0xff);
            buf[n++] = (char)((adv[i] >> 8) & 0xff);
            size_t l = strlen(lines[i]);
            memcpy(buf + n, lines[i], l);
            n += l;
            buf[n] = 0;
            g_draw_orig((uint32_t)(uintptr_t)buf, pen - pitch2 - 1u, pitch2, dflags);
        }
    }

    /* Log the miss case too, or "no Chinese" is indistinguishable from
     * "nothing to translate". Cheap: only the first 60 draws are noisy. */
    if (translated || g_draw_calls <= 60)
        logq("draw: %d/%d lines translated (pitch=%u flags=%u colour=%02x)",
             translated, nlines, pitch, flags, colour);
}

/* ==========================================================================
 * Self-test overlay
 * --------------------------------------------------------------------------
 * Deterministic proof that the CJK rasteriser really puts pixels on the
 * published frame. Enabled with ROTH_ZH_SELFTEST=1: a fixed banner is drawn on
 * every compose tick, independent of whatever the game happens to be printing
 * (the intro cutscene has no dialogue text at all, so nothing else can show
 * the pipeline working).
 *
 * TICK_ISR contract is "composite pre-rendered pixels, no allocation": the
 * banner is rasterised ONCE into a scratch bitmap, then memcpy'd from then on.
 * ========================================================================== */
#define BANNER_MAX_W 1024
#define BANNER_MAX_H 256
#define BANNER_BG    0           /* backdrop palette index                 */
#define BANNER_FRAME OUTLINE_IDX /* border ring                            */

static void banner_build(void)
{
    if (g_banner_ok) return;

    /* brightest harvestable ramp wins; fall back through the text colours */
    static const uint8_t try_col[] = { 0x0d, 0x0e, 0x0f, 0x0a, 0x0b, 0x0c };
    const uint8_t *ramp = NULL;
    for (unsigned i = 0; i < sizeof try_col / sizeof try_col[0]; i++) {
        ramp_prefetch(try_col[i]);
        if (g_ramp_ok[try_col[i]]) { ramp = g_ramp[try_col[i]]; break; }
    }
    if (!ramp) { logf_("selftest: no ramp harvestable, overlay disabled"); return; }

    static const char *s =
        "\xe4\xb8\xad\xe6\x96\x87"      /* 中文 */
        "\xe6\xb8\xb2\xe6\x9f\x93"      /* 渲染 */
        "\xe6\xb5\x8b\xe8\xaf\x95"      /* 测试 */
        " ABC 123";

    /* Rasterise at the FINAL on-screen em. Scaling a small bitmap up with
     * nearest neighbour is what produced the 3x3 blocky strokes. */
    float bscale = stbtt_ScaleForPixelHeight(&g_font, (float)BANNER_EM);
    int   bbase  = (int)((float)g_asc_units * bscale + 0.5f);
    int   bbot   = (int)((float)(g_asc_units - g_desc_units) * bscale + 0.5f);

    Rast rs[24];
    int nr = 0, adv = 0, maxy = bbot, guard = 0;
    for (const unsigned char *p = (const unsigned char *)s;
         *p && nr < (int)(sizeof rs / sizeof rs[0]) && guard++ < 64; ) {
        int n = 0;
        uint32_t cp = utf8_decode(p, &n);
        if (n < 1) n = 1;
        p += (size_t)n;
        if (!rasterize(cp, bscale, bbase, &rs[nr])) {
            logf_("selftest: rasterize failed");
            for (int i = 0; i < nr; i++) free(rs[i].bits);
            return;
        }
        if (rs[nr].w > 0 && rs[nr].h > 0) {
            adv += rs[nr].adv_px;
            if (rs[nr].yoff + rs[nr].h > maxy) maxy = rs[nr].yoff + rs[nr].h;
        } else {
            adv += BANNER_EM;
        }
        nr++;
    }
    int w = adv + 6, h = maxy + 6;
    if (w <= 0 || h <= 0 || w > BANNER_MAX_W || h > BANNER_MAX_H) {
        logf_("selftest: banner %dx%d implausible", w, h);
        return;
    }

    uint8_t *px = (uint8_t *)xmalloc((size_t)w * (size_t)h);
    if (!px) {
        for (int i = 0; i < nr; i++) free(rs[i].bits);
        return;
    }
    memset(px, BANNER_BG, (size_t)w * (size_t)h);
    for (int i = 0; i < w; i++) { px[i] = BANNER_FRAME; px[(h - 1) * w + i] = BANNER_FRAME; }
    for (int i = 0; i < h; i++) { px[i * w] = BANNER_FRAME; px[i * w + w - 1] = BANNER_FRAME; }

    int x = 3, base_y = 3;
    for (int i = 0; i < nr; i++) {
        Rast *r = &rs[i];
        if (r->w > 0 && r->h > 0) {
            int cjk = is_cjk(r->cp);
            if (CJK_OUTLINE || !cjk)
                outline_pass(px, (uint32_t)w, 0, x + r->xoff, base_y + r->yoff,
                             r->bits, r->w, r->h);
            blit_glyph(px, (uint32_t)w, 0, x + r->xoff, base_y + r->yoff,
                       r->bits, r->w, r->h, ramp, cjk);
        }
        x += r->adv_px;
        free(r->bits);
    }
    g_banner_ink = ramp[11];

    g_banner_px = px;
    g_banner_w = w;
    g_banner_h = h;
    g_banner_ok = 1;
    logf_("selftest: banner %dx%d built (em=%d baseline=%d)",
          w, h, BANNER_EM, bbase);

    /* ASCII proof-of-life: '.' backdrop, '+' outline ring, '#' glyph ink.
     * Lets the rasteriser be verified from the log alone. */
    logf_("selftest: banner ink map");
    for (int y = 0; y < h; y++) {
        char row[BANNER_MAX_W + 4];
        int rn = 0;
        for (int i = 0; i < w && rn < (int)sizeof row - 1; i++) {
            uint8_t c = px[y * w + i];
            row[rn++] = (c == BANNER_BG) ? '.' : (c == BANNER_FRAME ? '+' : '#');
        }
        row[rn] = 0;
        logf_("|%s|", row);
    }
}

static void ROTH_CDECL on_compose_tick(const struct roth_api_v1 *api,
                                       uint8_t *pixels, uint32_t width, uint32_t height)
{
    if (!g_selftest || !g_ready || !pixels) return;
    if (!width || !height || width > 4096u || height > 4096u) return;

    /* The banner is already at its final size, so this is a plain copy. It is
     * built in on_frame_game (GAME context); a compose tick is TICK_ISR, where
     * allocation and font calls are off-limits. */
    if (!g_banner_ok || !g_banner_px) return;

    int sw = g_banner_w, sh = g_banner_h;
    if (sw > (int)width || sh > (int)height) return;

    int ox = ((int)width - sw) / 2;
    int oy = 8;
    if (ox < 0) ox = 0;
    if (oy + sh > (int)height) oy = (int)height - sh;

    for (int y = 0; y < sh; y++) {
        const uint8_t *src = g_banner_px + (size_t)y * (size_t)g_banner_w;
        uint8_t *dst = pixels + (size_t)(oy + y) * width + (size_t)ox;
        memcpy(dst, src, (size_t)sw);
    }

    if (g_selftest_frames < 5)
        logf_("selftest: frame %u frame=%ux%u banner %dx%d at %d,%d",
              g_selftest_frames, width, height, sw, sh, ox, oy);
    g_selftest_frames++;

    /* The old "erase probe" read the frame at the top of the compose tick,
     * i.e. AFTER the publish memcpy and BEFORE our own draw, so it always
     * reported 0% -- it was measuring the wrong buffer, not a repaint. What it
     * actually exposed was that only the first few ticks were logged, because
     * the counter was clamped. Log the real cadence instead. */
    /* Timing law (sdk/MODDING_GUIDE.md): pace off the engine's own 70 Hz
     * counter at 0x90bcc -- never wall-clock or callback counts, which drift
     * several-fold between scenes. */
    if (!g_tick_t0) g_tick_t0 = api && api->game_ram ? api->game_ram->u16(0x90bcc) : 0u;
    g_tick_n++;
    if (g_tick_n % 70u == 0 && api && api->game_ram) {
        uint16_t now = api->game_ram->u16(0x90bcc);
        uint16_t el  = (uint16_t)(now - g_tick_t0);
        logf_("selftest: tick=%u engine_ticks=%u rate=%.1fHz",
              g_tick_n, el, el ? (double)g_tick_n * 70.0 / (double)el : 0.0);
    }
}

/* Always-on cadence report: how many ticks elapsed and how many times the
 * draw_text_to_buffer override actually fired.  In non-selftest runs this is
 * the only signal that tells us whether the engine ever renders text at all. */
static void ROTH_CDECL on_frame_game(const struct roth_api_v1 *api)
{
    if (!g_ready) return;

    /* Drain whatever the override queued since the last frame. The override
     * can run in the 70 Hz tick, so it logs into RAM and we do the fopen here,
     * in GAME context, where file I/O is legal. */
    logq_flush();

    /* A3: run the booked ramp harvests here, where engine calls are legal.
     * Until a colour's harvest lands, ramp_prefetch() left it flat, so the
     * first frames of a session can render text in a slightly wrong shade. */
    for (unsigned c = 0; c < 256; c++) {
        if (g_ramp_pend[c]) { ramp_harvest((uint8_t)c); g_ramp_pend[c] = 0; }
    }

    /* A4: build the self-test banner here rather than in on_compose_tick --
     * that callback is TICK_ISR, where allocation and font calls are illegal. */
    if (g_selftest && !g_banner_ok) banner_build();

    if (!g_frame_t0) g_frame_t0 = api && api->game_ram ? api->game_ram->u16(0x90bcc) : 0u;
    g_frame_n++;
    if (g_frame_n % 200u == 0 && api && api->game_ram) {
        uint16_t now = api->game_ram->u16(0x90bcc);
        uint16_t el  = (uint16_t)(now - g_frame_t0);
        logf_("frame: tick=%u engine_ticks=%u draw_calls=%u",
              g_frame_n, el, g_draw_calls);
    }
}

/* ==========================================================================
 * SDK entry points
 * ========================================================================== */
static void ROTH_CDECL on_load(const struct roth_api_v1 *api)
{
    const char *dir = api->plugin_dir ? api->plugin_dir() : "";

    /* NOTE: the log path must be established BEFORE the first logf_() or the
     * "load:" line is silently dropped (fopen("") fails and logf_ returns). */
    if (dir && *dir) {
        size_t dl = strlen(dir);
        char *slash = (dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/')) ? "" : "\\";
        snprintf(g_font_path, sizeof g_font_path, "%s%s%s", dir, slash, FONT_FILE);
        snprintf(g_table_path, sizeof g_table_path, "%s%s%s", dir, slash, TABLE_FILE);
        snprintf(g_log_path, sizeof g_log_path, "%s%s%s", dir, slash, LOG_NAME);
    } else {
        snprintf(g_font_path, sizeof g_font_path, "%s", FONT_FILE);
        snprintf(g_table_path, sizeof g_table_path, "%s", TABLE_FILE);
        snprintf(g_log_path, sizeof g_log_path, "%s", LOG_NAME);
    }

    logf_("load: id=%s abi=%u.%u struct_size=%u engine=%p game_ram=%p dir='%s'",
          ROTH_PLUGIN_ID, api->abi_major, api->abi_minor, api->struct_size,
          (const void *)api->engine, (const void *)api->game_ram, dir ? dir : "");

    g_selftest = getenv("ROTH_ZH_SELFTEST") != NULL;
    logf_("selftest: %s", g_selftest ? "ON (compose-tick banner + raw draw dump)" : "off");

    load_font(g_font_path);
}

static void ROTH_CDECL on_game_ram_ready(const struct roth_api_v1 *api)
{
    g_ram_size = api->game_ram ? api->game_ram->size() : 0u;
    /* Resolve the base of the canonical address space. The engine hands us
     * runtime pointers as canonical VA + base, so rb() subtracts this to get
     * back a canonical offset. to_ptr(0) == base + 0 == base. */
    g_base  = (uintptr_t)api->game_ram->to_ptr(0u);
    g_gr_ok = g_base != 0u;
    logf_("game_ram_ready: size=%u base=%p", g_ram_size, (void *)g_base);
    desc_parse(api);
    probe_build(api);
    tbl_load();
    /* A4: rasterise the whole table here, in MAIN context. From this point on
     * the override never allocates and never rasterises, which is what the
     * TICK_ISR context it runs in requires. */
    glyph_prewarm();
    g_ready = 1;
}

static void ROTH_CDECL on_register_overrides(const struct roth_api_v1 *api,
                                             struct roth_registrar_v1 *reg)
{
    if (!api->engine) { logf_("register: no engine view"); return; }
    roth_override(reg, ROTH_FN_draw_text_to_buffer, on_draw_text_to_buffer, ROTH_PRIORITY);
    logf_("register: draw_text_to_buffer override (prio %d)", ROTH_PRIORITY);
}

static void ROTH_CDECL on_unload(const struct roth_api_v1 *api) { (void)api; logf_("unload"); }

ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void)
{
    static const struct roth_plugin_info_v1 info = {
        .abi_major = ROTH_ABI_MAJOR,
        .abi_minor = ROTH_ABI_MINOR,
        .struct_size = sizeof(struct roth_plugin_info_v1),
        .id = ROTH_PLUGIN_ID,
        .name = "ROTH Chinese Localisation",
        .version = "0.1.0",
        .sdk_req_major = 0,
        .sdk_req_minor = 5,
        .api_use = ROTH_API_USE_ENGINE | ROTH_API_USE_GAME_RAM | ROTH_API_USE_COMPOSE,
        .on_load = on_load,
        .on_game_ram_ready = on_game_ram_ready,
        .on_frame_game = on_frame_game,
        .on_compose_tick = on_compose_tick,
        .on_audio = NULL,
        .on_unload = on_unload,
        .on_register_overrides = on_register_overrides,
        .on_scancode = NULL,
        .on_mouse_poll = NULL,
    };
    return &info;
}
