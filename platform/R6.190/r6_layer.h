/*
 * R6.190 M7.3: ownership-aware conversion of ML's indexed pixels into
 * Canon's RGBA layer 0, see r6_layer.c. Used by src/bmp.c
 * (refresh_yuv_from_rgb, rgba_buffer_clear) and lvprobe.c (counters), only
 * under CONFIG_R6_LAYER_OWNERSHIP (platform/R6.190/internals.h).
 */
#ifndef _r6_layer_h_
#define _r6_layer_h_

#include <stdint.h>

struct MARV;

/* Canon's layer 0 as ML's code expects it: rgb_vram_info != NULL,
 * bitmap_data != NULL and 4-byte aligned, width == BMP_LAYER_WIDTH (2048),
 * height >= 540 (camera M7.2a: 0x800 x 0x438). RAM reads of the MARV only.
 * 0 = neither the conversion nor the clear may touch it (counted). */
int r6_layer_target_ok(const struct MARV * m);

/* idx: ML's indexed 960x540 buffer (BMPPITCH), layer: Canon's layer 0
 * (2048 px pitch), same origin as the DIGIC X loop in bmp.c.
 * allow_erase: 1 = pixels ML gave up are erased (verified); 0 (piggyback:
 * Canon is repainting layer 0 right now) = they are only verified and kept
 * owned, the next conversion with allow_erase erases them (r6_layer_deferred
 * tells the caller to request one).
 * under (M7.4, ML menu closed): write-under rule. A pixel ML does not own
 * yet is written (and owned) only where layer 0 shows nothing of Canon
 * (alpha 0 or the colour key 0xFF000000); pixels ML owns are re-asserted
 * only while their block still verifies (hash), otherwise the block's ML
 * pixels are given up and only its empty pixels are claimed. So Canon's UI
 * stays on top of ML's overlays and ML never erases a Canon pixel it painted
 * over. under 0 (ML menu): ML paints over Canon (M6/M7.3).
 * Return 1 = done, 0 = not possible (no ownership table: allocation failed,
 * or a misaligned buffer); the caller then runs the old loop.
 * The caller makes them single writers (refresh: between r6g_begin and
 * r6g_end; clear: r6g_clear_lock). */
int r6_layer_convert(const uint8_t * idx, uint32_t * layer, int allow_erase, int under);
int r6_layer_clear(uint32_t * layer);
/* 1 = an ownership table exists (the clear may leave the indexed buffer to
 * the next refresh) */
int r6_layer_has_table(void);
/* blocks whose given-up pixels the last conversion kept (allow_erase 0) */
uint32_t r6_layer_deferred(void);

struct r6_layer_stats
{
    uint32_t table_bytes;    /* ownership table size (0 = not allocated yet) */
    uint32_t alloc_failed;   /* 1 = malloc failed: bmp.c uses the old loop */
    uint32_t conversions;    /* r6_layer_convert calls that ran */
    uint32_t clears;         /* r6_layer_clear calls that ran */
    uint32_t resets;         /* layer address changed: table zeroed (old layer not touched) */
    uint32_t slow_lut;       /* conversions without the word fast-skip (LUT transparency != {0, 3}) */
    /* last conversion */
    uint32_t blk_skip;       /* 32-px blocks fully transparent and not owned: skipped */
    uint32_t blk_conv;       /* blocks converted */
    uint32_t blk_owned;      /* blocks with ML pixels after the conversion */
    uint32_t px_owned;       /* ML pixels in layer 0 after the conversion */
    /* totals */
    uint32_t px_cleared;     /* pixels ML gave up and erased (layer still held exactly ML's values) */
    uint32_t veto_blk;       /* blocks where layer 0 no longer matched ML's pixels: nothing erased */
    uint32_t veto_px;        /* pixels not erased because of that (ownership dropped) */
    uint32_t clear_px;       /* pixels erased by rgba_buffer_clear */
    uint32_t clear_veto_blk, clear_veto_px;
    /* piggyback conversions (allow_erase 0): given-up pixels verified and kept */
    uint32_t defer_blk;      /* last conversion: blocks with kept given-up pixels */
    uint32_t defer_px;       /* total pixels kept that way (erased by a later conversion) */
    uint32_t defer_convs;    /* conversions with allow_erase 0 */
    /* M7.4 write-under conversions (under 1) */
    uint32_t under_convs;    /* conversions with under 1 */
    uint32_t under_blocked_last;   /* last conversion: ML pixels not written, layer 0 showed Canon there */
    uint32_t under_blocked_px;     /* total of these */
    uint32_t under_drop_blk, under_drop_px;   /* blocks / still drawn ML pixels given up: Canon painted into an owned block */
    uint32_t under_val_n;    /* distinct blocking layer values seen (the last 4 in under_val[n & 3]) */
    uint32_t under_val[4];
    /* target check (r6_layer_target_ok) */
    uint32_t bad_target;     /* refresh / clear calls refused: layer 0 not as expected */
    uint32_t bad_w, bad_h;   /* MARV width / height of the last refusal */
    uint32_t bad_data;       /* bitmap_data of the last refusal */
    /* conversion time (get_us_clock) */
    uint32_t us_last, us_max, us_min;
    uint64_t us_sum;
    uint32_t clear_us_last, clear_us_max;
};

const struct r6_layer_stats * r6_layer_stats(void);

#endif
