/*
 * R6.190 M7.3: ownership-aware refresh of ML's pixels in Canon's layer 0.
 *
 * Why: on DIGIC X without a dedicated layer ML draws into Canon's own GUI
 * layer 0 (2048x1080 RGBA, CONFIG_COMPOSITOR_XCM). The old loop in
 * refresh_yuv_from_rgb wrote every opaque indexed pixel and skipped the
 * transparent ones, so a pixel ML stopped drawing stayed in layer 0 until
 * Canon repainted it (M7.2a camera test: remnants of longer status lines
 * behind shorter ones). Erasing blindly is not allowed either: Canon may
 * have painted that pixel since.
 *
 * What: an ownership table per 32-pixel block of ML's 960x540 indexed
 * buffer (30 x 540 = 16200 blocks), 8 bytes each = 129600 bytes:
 *   mask: bit i = ML wrote pixel i of the block in its last conversion
 *   hash: FNV-1a over the RGBA words ML wrote there, in bit order
 * Conversion, per block (32 indexed bytes loaded once, as 8 words):
 *   - all transparent (indices 0 / 3) and nothing owned: skipped (fast path,
 *     8 word tests, no LUT, no layer access)
 *   - pixels owned but transparent now: erased (0x00000000) only if the hash
 *     of layer 0 over ALL owned pixels of the block still equals the stored
 *     hash, i.e. layer 0 still holds exactly what ML wrote there. Otherwise
 *     (Canon painted into the block since) nothing is erased in that block
 *     ("veto"); those pixels are no longer ML's (ownership dropped).
 *   - opaque pixels are written as before (ML re-asserts its pixels), the
 *     new mask and hash are stored.
 *   - piggyback conversions (the guard saw Canon repainting layer 0 right
 *     now, allow_erase 0): given-up pixels are verified but not erased; if
 *     the hash still matches they stay owned (mask = new | kept; M7.7: the
 *     hash is computed from ML's own values - LUT value written, verified
 *     copy kept - not read back), and bmp.c requests another refresh, whose
 *     conversion (Canon quiet, own XimrExe path) erases them. M7.7: once
 *     kept for R6L_DEFER_MAX_MS over an unbroken run of conversions, a
 *     piggyback conversion erases them too (in the EVF Canon flushes ~15 x
 *     per second: no own XimrExe ever), except while the guard settles. So
 *     erases can now run while Canon flushes; each store re-checks its pixel
 *     right before it (erase / re-assert: still the value read for the hash;
 *     claim: still empty), which leaves a single load / compare / store
 *     window (it can still be preempted). Erases are re-checked in both
 *     modes, re-asserts and claims in the write-under mode only (the ML
 *     menu paints over Canon by design).
 * rgba_buffer_clear (clrscr): every owned block is verified the same way and
 * erased, then the table is empty, and bmp.c zeroes the whole indexed buffer
 * while it still holds the guard lock (no refresh can re-own old content in
 * between; also covers indexed row 539, which clrscr's bmp_fill misses).
 * The clear does not defer while Canon is active: it runs when ML closes
 * its screens (menu), before Canon repaints there, and deferring would let
 * Canon's repaint veto those blocks (persistent ML remnants).
 *
 * Memory alternatives considered (ML heap minimum on the camera ~513 KB):
 *   1 bit per pixel only (64.8 KB): cannot tell ML's pixel from Canon's.
 *   previous indexed frame (518 KB): too big; only for owned blocks it is
 *     still up to 518 KB with the ML menu open (every block owned).
 *   mask + 32-bit hash per block (129.6 KB, chosen): bounded, exact up to a
 *     hash collision (a single changed pixel is always detected: the
 *     word-wise FNV-1a chain is bijective in each value; a collision needs
 *     >= 2 changed pixels in the block, and value differences only in high
 *     bits are weaker than 2^-32 - worst pair of R6 LUT colours: 18
 *     effective bits; cosmetic either way: M7.5 lazy skip, see below);
 *     granularity: one Canon pixel in a block keeps ML's given-up pixels of
 *     that block (cosmetic, never destructive).
 *   mask + 16-bit hash (97.2 KB): possible fallback if memory gets tight.
 *
 * Remaining limits (not closable from ML without a tag in the pixel values):
 *  - race: Canon's GUI may write a pixel between ML's per-pixel re-check and
 *    ML's store (M7.7: one load / compare / store; in any conversion and in
 *    clrscr; a preemption inside it is the worst case: one Canon pixel).
 *  - the hash detects a change, not who wrote: if Canon paints exactly the
 *    RGBA values ML wrote over all owned pixels of a block, ML erases them
 *    (cosmetic hole until Canon repaints; ML's black is 0xFF010101).
 *  - a veto drops every given-up pixel of the block, not only the ones
 *    Canon changed: up to 31 ML pixels may stay next to a Canon element
 *    (counted in veto_* / clear_veto_*; camera test item).
 *
 * M7.4 write-under rule (r6_layer_convert with under = 1, i.e. whenever the
 * ML menu is closed): ML's overlays never cover Canon's UI. A pixel ML does
 * not own is written (and owned) only where layer 0 shows nothing of Canon
 * (R6L_EMPTY); owned pixels are re-asserted only while their block still
 * verifies. If Canon painted into an owned block, ML gives up the whole
 * block ("drop", like the veto) and claims only its empty pixels. So ML
 * never erases a Canon pixel it would have covered; its remaining pixels in
 * such a block stay until Canon repaints there (cosmetic, counted in
 * under_drop_*). The ML menu (under = 0) paints over Canon as in M6/M7.3.
 *
 * M7.5 lazy conversion (performance; camera M7.4: a conversion with the
 * false color band owned took up to ~205 ms, mostly layer 0 accesses, and a
 * small change such as the spotmeter text converted every owned block
 * again). ML's model of layer 0 (mask + hash per block) can only go wrong
 * through a Canon write into layer 0, and Canon shows such a write only with
 * a flush (VMIX transfer: buffer index change, pending, frame in flight),
 * which the refresh guard's polls count (r6g_canon_activity). If that count
 * did not change since the previous conversion, a block is skipped without
 * any layer 0 access when
 *   - its opaque pixels are exactly the pixels ML owns (nm == mask: no
 *     write-under pixel blocked by Canon, nothing kept by a piggyback), and
 *   - the FNV-1a hash of the LUT values ML would write equals the stored hash
 *     (= ML's own content of the block is unchanged; a LUT change changes it)
 * i.e. exactly the blocks for which M7.4 would verify layer 0 and rewrite the
 * same values (a no-op). Changed blocks, incomplete blocks (blocked or kept
 * pixels: re-evaluated every time, so a pixel Canon frees is claimed as in
 * M7.4) and given-up pixels (erased only after the hash check) go through the
 * M7.4 path unchanged. After Canon activity (and with lazy 0) the whole
 * conversion is the M7.4 one, which re-verifies every owned block, drops the
 * blocks Canon painted into and re-shows ML's pixels Canon wiped (zebra.c's
 * re-show after a VMIX index change requests that conversion); there, a
 * write-under block that verifies and is unchanged is not rewritten (same
 * values). A skip never writes layer 0, so it can neither erase nor cover a
 * Canon pixel: polls that miss a Canon flush (even number of index flips)
 * only delay the re-verification (cosmetic: e.g. ML's pixels that Canon
 * wiped stay away until the next conversion after Canon activity). No extra
 * memory: the stored block hash already is the hash of the values ML wrote.
 * Fix round (review): bmp.c passes lazy only in the write-under mode (ML
 * menu closed); the ML menu converts every block as in M7.4, so a missed
 * Canon flush cannot leave a stale block hash that would veto the menu's
 * clear (ML menu remnants). Unchanged complete blocks - also the "kept"
 * ones of a full conversion - are re-verified only by a full conversion:
 * after Canon activity, and at the latest R6L_LAZY_MAX_MS after the
 * previous full one (bounds the effect of polls that missed a flush;
 * longest lazy streak in lazy_streak_max).
 *
 * Reads/writes: ML's indexed buffer and Canon's layer 0 (bitmap_data of
 * Canon's MARV, the same memory the old loop wrote); no Canon calls (the
 * caller holds the guard lock), get_us_clock for the timing.
 */

#include "dryos.h"
#include "bmp.h"
#include "mem.h"
#include "r6_layer.h"
#include "refresh_guard.h"        /* M7.5: r6g_canon_activity */

#ifdef CONFIG_R6_LAYER_OWNERSHIP

#if !defined(CONFIG_DIGIC_X) || defined(CONFIG_COMPOSITOR_DEDICATED_LAYER) || !defined(CONFIG_R6_REFRESH_GUARD)
#error "r6_layer.c: DIGIC X shared layer with the R6 refresh guard only"
#endif

#define R6L_W       BMPPITCH                        /* 960 indexed pixels per row */
#define R6L_H       (BMP_H_PLUS - BMP_H_MINUS)      /* 540 rows */
#define R6L_BPR     (R6L_W / 32)                    /* 30 blocks per row */
#define R6L_BLOCKS  (R6L_BPR * R6L_H)               /* 16200 */
#define R6L_H0      0x811C9DC5u                     /* FNV-1a offset basis */
#define R6L_PRIME   0x01000193u
/* M7.4: a layer 0 pixel that shows nothing of Canon: alpha 0, or exactly
 * 0xFF000000 (opaque black is see-through on layer 0, M5 photo: Canon's
 * colour key, docs/m6_analyse.md) */
#define R6L_EMPTY(c) (!((c) & 0xFF000000) || (c) == 0xFF000000)

#if (R6L_W % 32) || (BMP_VRAM_SIZE != R6L_W * R6L_H)
#error "r6_layer.c: unexpected indexed buffer geometry"
#endif

struct r6l_blk { uint32_t mask; uint32_t hash; };

static struct r6l_blk * r6l_own = NULL;
static uint32_t * r6l_layer = NULL;          /* layer the table describes */
static uint32_t r6l_lut[256];
static struct r6_layer_stats st = { .us_min = 0xFFFFFFFF };
static uint32_t r6l_act = 0;                 /* M7.5: r6g_canon_activity() at the previous conversion */
static int r6l_act_valid = 0;
#define R6L_LAZY_MAX_MS 2000                 /* M7.5 fix: a full conversion at least every 2 s (while converting) */
static uint32_t r6l_t_full = 0;              /* ms (us clock / 1000) of the last full conversion */
/* M7.7: deferred erases. Kept (given-up, verified) pixels are erased by the
 * first conversion after they have been kept for R6L_DEFER_MAX_MS, also a
 * piggyback one (not while the guard settles). The age runs over an
 * unbroken run of conversions: a pause longer than R6L_DEFER_GAP_MS (display
 * off, LV paused; the conversion cap is at most 2 s) restarts it. */
#define R6L_DEFER_MAX_MS 250
#define R6L_DEFER_GAP_MS 2500
static int r6l_defer_on = 0;                 /* the previous conversion kept given-up pixels */
static uint32_t r6l_defer_t0 = 0;            /* ms when the current run of keeping started */
static uint32_t r6l_conv_t = 0;              /* ms when the previous conversion ended */
static int r6l_conv_seen = 0;

static inline uint32_t r6l_mix(uint32_t h, uint32_t v)
{
    return (h ^ v) * R6L_PRIME;
}

/* hash of layer pixels d[i] for the bits of m, in bit order */
static inline uint32_t r6l_hash_layer(const uint32_t * d, uint32_t m)
{
    uint32_t h = R6L_H0;
    for (; m; m &= m - 1)
        h = r6l_mix(h, d[__builtin_ctz(m)]);
    return h;
}

/* M7.7: the same, keeping a copy of the values read (c[i] for the bits of m):
 * a later store checks that the pixel still holds c[i] (store race) */
static inline uint32_t r6l_hash_copy(const uint32_t * d, uint32_t m, uint32_t * c)
{
    uint32_t h = R6L_H0;
    for (; m; m &= m - 1)
    {
        int i = __builtin_ctz(m);
        uint32_t v = d[i];
        c[i] = v;
        h = r6l_mix(h, v);
    }
    return h;
}

/* table for this layer; 0 = not available */
static int r6l_table(uint32_t * layer)
{
    if (!r6l_own)
    {
        if (st.alloc_failed) return 0;
        r6l_own = malloc(R6L_BLOCKS * sizeof(struct r6l_blk));
        if (!r6l_own) { st.alloc_failed = 1; return 0; }
        memset(r6l_own, 0, R6L_BLOCKS * sizeof(struct r6l_blk));
        st.table_bytes = R6L_BLOCKS * sizeof(struct r6l_blk);
        r6l_layer = layer;
    }
    if (layer != r6l_layer)
    {
        /* other surface: ML's pixels in the old one are not touched any more */
        memset(r6l_own, 0, R6L_BLOCKS * sizeof(struct r6l_blk));
        r6l_layer = layer;
        st.resets++;
        r6l_defer_on = 0;
    }
    return 1;
}

/* LUT copy for this conversion; 1 = transparent indices are exactly 0 and 3
 * (COLOR_EMPTY, COLOR_TRANSPARENT_BLACK), so the word test below is valid */
static int r6l_build_lut(void)
{
    int fast = 1;
    for (int i = 0; i < 256; i++)
    {
        uint32_t v = indexed2rgb(i);
        r6l_lut[i] = v;
        int transparent = !(v & 0xFF000000);
        if (transparent != (i == 0 || i == 3)) fast = 0;
    }
    return fast;
}

int r6_layer_target_ok(const struct MARV * m)
{
    if (m && m->bitmap_data && !((uintptr_t) m->bitmap_data & 3)
        && m->width == BMP_LAYER_WIDTH && m->height >= (uint32_t) R6L_H)
        return 1;
    st.bad_target++;
    st.bad_w = m ? m->width : 0;
    st.bad_h = m ? m->height : 0;
    st.bad_data = m ? (uint32_t) m->bitmap_data : 0;
    return 0;
}

int r6_layer_has_table(void)
{
    return r6l_own != NULL;
}

int r6_layer_owns(int x, int y)
{
    if (!r6l_own || x < 0 || y < 0 || x >= R6L_W || y >= R6L_H) return 0;
    return (r6l_own[y * R6L_BPR + x / 32].mask >> (x & 31)) & 1;   /* bit i = pixel bx * 32 + i (r6_layer_convert) */
}

uint32_t r6_layer_deferred(void)
{
    return st.defer_blk;
}

int r6_layer_convert(const uint8_t * idx, uint32_t * layer, int allow_erase, int under, int lazy)
{
    st.defer_blk = 0;
    if (!idx || !layer || ((uintptr_t) idx & 3) || ((uintptr_t) layer & 3)) return 0;
    if (!r6l_table(layer)) return 0;

    uint64_t t0 = get_us_clock();
    int fast = r6l_build_lut();
    if (!fast) st.slow_lut++;
    if (under) st.under_convs++;
    /* M7.5: lazy only while Canon was quiet since the previous conversion */
    uint32_t act = r6g_canon_activity();
    uint32_t now_ms = (uint32_t) (t0 / 1000);
    /* M7.7: aged deferred erases (see R6L_DEFER_MAX_MS) */
    /* gap = idle time since the end of the previous conversion (r6l_conv_t is
     * set at the end), so long conversions never look like a pause */
    if (r6l_conv_seen && now_ms - r6l_conv_t > R6L_DEFER_GAP_MS) r6l_defer_t0 = now_ms;
    r6l_conv_seen = 1;
    int er = allow_erase;
    if (!er && r6l_defer_on && now_ms - r6l_defer_t0 >= R6L_DEFER_MAX_MS && !r6g_settling_now())
    {
        er = 1;
        st.defer_forced++;
        if (now_ms - r6l_defer_t0 > st.defer_age_max) st.defer_age_max = now_ms - r6l_defer_t0;
    }
    if (!er) st.defer_convs++;
    int lz = lazy && r6l_act_valid && act == r6l_act;
    if (lz && now_ms - r6l_t_full >= R6L_LAZY_MAX_MS)
    {
        lz = 0;                                  /* re-verify the skipped / kept blocks now and then */
        st.full_forced++;
    }
    r6l_act = act;
    r6l_act_valid = 1;
    if (lz)
    {
        st.lazy_convs++;
        if (++st.lazy_streak > st.lazy_streak_max) st.lazy_streak_max = st.lazy_streak;
    }
    else
    {
        st.full_convs++;
        st.lazy_streak = 0;
        r6l_t_full = now_ms;
    }
    uint32_t skip = 0, conv = 0, owned_b = 0, owned_px = 0, defer_b = 0, blocked_px = 0, same = 0, kept = 0;

    struct r6l_blk * o = r6l_own;
    for (int y = 0; y < R6L_H; y++)
    {
        const uint32_t * srow = (const uint32_t *) (idx + y * R6L_W);
        uint32_t * drow = layer + y * BMP_LAYER_WIDTH;
        for (int bx = 0; bx < R6L_BPR; bx++, o++)
        {
            union { uint32_t w[8]; uint8_t b[32]; } s;
            const uint32_t * sp = srow + bx * 8;
            uint32_t any = 0;
            for (int k = 0; k < 8; k++)
            {
                uint32_t w = sp[k];
                s.w[k] = w;
                /* a byte is 0 or 3 <=> bits 2..7 clear and bit 0 == bit 1 */
                any |= (w & 0xFCFCFCFC) | ((w ^ (w >> 1)) & 0x01010101);
            }
            uint32_t om = o->mask;
            if (fast && !any && !om) { skip++; continue; }

            uint32_t nm = 0;
            /* M7.5: for an owned block, the hash of the values ML would write
             * (opaque pixels in bit order, as stored below) in the same pass */
            uint32_t hn = R6L_H0;
            if (om)
            {
                for (int i = 0; i < 32; i++)
                {
                    uint32_t v = r6l_lut[s.b[i]];
                    if (v & 0xFF000000) { nm |= 1u << i; hn = r6l_mix(hn, v); }
                }
            }
            else
            {
                for (int i = 0; i < 32; i++)
                    if (r6l_lut[s.b[i]] & 0xFF000000) nm |= 1u << i;
            }
            if (!nm && !om) { skip++; continue; }       /* slow LUT path only */

            /* M7.5: ML's content of a complete block unchanged (same opaque
             * pixels, hash of the new values == stored hash of the values written)? */
            int unchanged = om && nm == om && hn == o->hash;
            if (unchanged && lz)
            {
                same++;                                  /* lazy: no layer 0 access at all */
                owned_b++; owned_px += __builtin_popcount(om);
                continue;
            }
            conv++;

            uint32_t * d = drow + bx * 32;
            uint32_t cm = om & ~nm;                      /* ML gave these up */
            uint32_t keep = 0;                           /* given up, verified, kept (piggyback) */
            uint32_t cv[32];                             /* M7.7: owned values as read for the hash (valid if vc) */
            /* M7.4: the hash check is needed for the given-up pixels and, in
             * the write-under mode, for the re-asserted ones */
            int vc = om && (cm || under);
            int verified = vc ? r6l_hash_copy(d, om, cv) == o->hash : 1;
            if (unchanged && under && verified)
            {
                /* M7.5: layer 0 holds exactly ML's unchanged values (M7.4
                 * rewrote the same values): keep, no writes (re-verified
                 * by the next full conversion, see the header) */
                kept++;
                owned_b++; owned_px += __builtin_popcount(om);
                continue;
            }
            if (cm)
            {
                if (!verified)
                {
                    st.veto_blk++;
                    st.veto_px += __builtin_popcount(cm);
                }
                else if (er)
                {
                    /* M7.7: only where the pixel still holds what ML verified */
                    for (uint32_t m = cm; m; m &= m - 1)
                    {
                        int i = __builtin_ctz(m);
                        if (d[i] == cv[i]) { d[i] = 0x00000000; st.px_cleared++; }
                        else st.race_px++;
                    }
                }
                else
                {
                    keep = cm;
                    defer_b++;
                    st.defer_px += __builtin_popcount(cm);
                }
            }

            /* pixels to write: all opaque ones (overpaint, ML menu), or in
             * the write-under mode the owned ones of a verified block plus
             * the ones where layer 0 is empty (Canon shows nothing there) */
            uint32_t wm = nm;
            if (under && nm)
            {
                wm = verified ? (nm & om) : 0;
                if (!verified && om)
                {
                    st.under_drop_blk++;
                    st.under_drop_px += __builtin_popcount(om & ~cm);
                }
                uint32_t blocked = 0;
                for (uint32_t m = nm & ~wm; m; m &= m - 1)
                {
                    int i = __builtin_ctz(m);
                    uint32_t c = d[i];
                    if (R6L_EMPTY(c)) wm |= 1u << i;
                    else
                    {
                        blocked++;
                        if (c != st.under_val[st.under_val_n & 3] && c != st.under_val[(st.under_val_n - 1) & 3])
                            st.under_val[++st.under_val_n & 3] = c;
                    }
                }
                blocked_px += blocked;
            }

            /* M7.7: in the write-under mode every store re-checks its pixel
             * right before it: an owned (verified) pixel must still hold the
             * value read for the hash, a claimed one must still be empty;
             * otherwise Canon painted it meanwhile: not written, not owned */
            uint32_t wd = 0;
            for (uint32_t m = wm; m; m &= m - 1)
            {
                int i = __builtin_ctz(m);
                uint32_t v = r6l_lut[s.b[i]];
                if (under)
                {
                    uint32_t cur = d[i];
                    int ok = (vc && verified && ((om >> i) & 1)) ? cur == cv[i] : R6L_EMPTY(cur);
                    if (!ok) { st.race_px++; continue; }
                }
                d[i] = v;
                wd |= 1u << i;
            }
            uint32_t mk = wd | keep;
            /* hash of what ML now owns, from ML's own values (written: LUT
             * value; kept: the verified copy) - no read-back of layer 0 */
            uint32_t h = R6L_H0;
            for (uint32_t m = mk; m; m &= m - 1)
            {
                int i = __builtin_ctz(m);
                h = r6l_mix(h, ((wd >> i) & 1) ? r6l_lut[s.b[i]] : cv[i]);
            }
            o->mask = mk;
            o->hash = h;
            if (mk) { owned_b++; owned_px += __builtin_popcount(mk); }
        }
    }

    st.defer_blk = defer_b;
    if (defer_b) { if (!r6l_defer_on) { r6l_defer_on = 1; r6l_defer_t0 = now_ms; } }
    else r6l_defer_on = 0;
    st.under_blocked_last = blocked_px;
    st.under_blocked_px += blocked_px;
    st.blk_skip = skip;
    st.blk_conv = conv;
    st.blk_same = same;
    st.blk_same_total += same;
    st.blk_keep = kept;
    st.blk_keep_total += kept;
    st.blk_owned = owned_b;
    st.px_owned = owned_px;
    st.conversions++;
    uint32_t us = (uint32_t) (get_us_clock() - t0);
    r6l_conv_t = now_ms + us / 1000;
    st.us_last = us;
    if (us > st.us_max) st.us_max = us;
    if (us < st.us_min) st.us_min = us;
    st.us_sum += us;
    if (lz) { st.us_lazy_last = us; if (us > st.us_lazy_max) st.us_lazy_max = us; }
    else    { st.us_full_last = us; if (us > st.us_full_max) st.us_full_max = us; }
    return 1;
}

int r6_layer_clear(uint32_t * layer)
{
    if (!layer || ((uintptr_t) layer & 3)) return 0;
    if (!r6l_table(layer)) return 0;

    uint64_t t0 = get_us_clock();
    struct r6l_blk * o = r6l_own;
    for (int y = 0; y < R6L_H; y++)
    {
        uint32_t * drow = layer + y * BMP_LAYER_WIDTH;
        for (int bx = 0; bx < R6L_BPR; bx++, o++)
        {
            uint32_t om = o->mask;
            if (!om) continue;
            uint32_t * d = drow + bx * 32;
            uint32_t c[32];
            if (r6l_hash_copy(d, om, c) == o->hash)
            {
                /* M7.7: only where the pixel still holds the value checked */
                for (uint32_t m = om; m; m &= m - 1)
                {
                    int i = __builtin_ctz(m);
                    if (d[i] == c[i]) { d[i] = 0x00000000; st.clear_px++; }
                    else st.race_px++;
                }
            }
            else
            {
                st.clear_veto_blk++;
                st.clear_veto_px += __builtin_popcount(om);
            }
            o->mask = 0;
            o->hash = R6L_H0;
        }
    }
    st.blk_owned = 0;
    st.px_owned = 0;
    st.clears++;
    r6l_defer_on = 0;
    uint32_t us = (uint32_t) (get_us_clock() - t0);
    st.clear_us_last = us;
    if (us > st.clear_us_max) st.clear_us_max = us;
    return 1;
}

const struct r6_layer_stats * r6_layer_stats(void)
{
    return &st;
}

#endif /* CONFIG_R6_LAYER_OWNERSHIP */
