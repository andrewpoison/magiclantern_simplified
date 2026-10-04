/*
 * R6.190 M9.1: grids and frame masks (Overlay > "Grids & masks", r6_grid.c).
 * Only with CONFIG_R6_GRID (platform/R6.190/internals.h). Used by
 * src/zebra.c (overlay task, r6_gd_erase) and lvprobe.c (log keys grid.*).
 */
#ifndef _r6_grid_h_
#define _r6_grid_h_

#include <stdint.h>

/* a rectangle in ML coordinates, x0 / y0 inclusive, x1 / y1 exclusive */
struct r6_grid_rect { int16_t x0, y0, x1, y1; };

/* exclusion rectangles passed by zebra.c: the erase / hash rectangles of
 * the boxes that redraw when their area in ML's buffer changes (ML's
 * histogram, the waveform, the spotmeter, "Histogram (Canon)"). A grid
 * pixel inside one of them would ping-pong with that box (box redraw wipes
 * it -> grid repairs it -> box hash differs -> box redraws ...). */
#define R6_GRID_NEXCL 4

/* palette entries for the grid (src/bmp.c indexed2rgbLUT, CONFIG_R6_GRID; unused before M9.1, the menu's *_MOD
 * colours are 21..25): semi-transparent, RGB 0x010101 / 0xebebeb so they differ from Canon's partial-alpha
 * values (0x80000000, 0x3f000000, ...) in the ownership hash and in the QEMU checks */
#define R6_GRID_MASK50   26     /* 0x80010101: mask 50 % */
#define R6_GRID_MASK75   27     /* 0xc0010101: mask 75 % */
#define R6_GRID_WHITE60  28     /* 0x99ebebeb: white line, 60 % */

/* overlay task (src/zebra.c livev_hipriority_task), every R6_GD_GRID_MS:
 * BMP_LOCK held, ML menu closed and the overlay gate passed (both checked
 * again by the caller inside the lock). Draws the wanted shape, erases the
 * previous one when the settings / screen / exclusions changed (the drawn
 * shape is regenerated from its snapshot), otherwise repairs missing pixels
 * of the drawn shape. ML's indexed buffer and its mirror only. Returns 1 if
 * ML's indexed buffer changed (the caller requests one refresh). */
int r6_grid_step(const struct r6_grid_rect * ex, int n);

/* erase the drawn shape (from its snapshot), BMP_LOCK held; called by
 * r6_gd_erase when the overlay screen state ended and by r6_grid_step when
 * the grid is switched off. Returns 1 if ML's indexed buffer changed. */
int r6_grid_erase(void);

/* 1 = the grid menu is ON and selects something to draw, or (M9.3) the motion display of R6 Motion is wanted
 * (r6_motion.c, drawn by r6_grid_step with the same pixel rules); src/zebra.c steps while this or drawn is set */
int r6_grid_wanted(void);
/* M9.3: the grid alone (log key grid.wanted, skeptic B8) */
int r6_grid_wanted_grid(void);

struct r6_grid_stats
{
    uint32_t cfg;            /* settings of the drawn shape, one nibble each: line, color, safe, style, aspect, center, type, on */
    uint32_t sig;            /* signature (FNV-1a of the snapshot, r6_gd_bv_hash_mem) */
    uint32_t drawn;          /* 1 = a shape is in ML's buffer */
    int32_t  evf, y0;        /* screen of the drawn shape: 1 = EVF, image top row (ML) */
    uint32_t px_line, px_fill, px_shadow;   /* pixels of the drawn shape after clipping (overlaps counted twice) */
    uint32_t steps, off;     /* r6_grid_step calls, of these with nothing wanted */
    uint32_t draws, erases;  /* shapes drawn (new signature) / erased */
    uint32_t repairs, repair_px;   /* steps that restored pixels of the unchanged shape, pixels restored */
    uint32_t claim_ovl;      /* pixels taken over from zebras / false color / peaking (indexed == mirror, no mark) */
    uint32_t cleanup_px;     /* overlay pixels erased next to newly claimed pixels (would stay stale otherwise) */
    uint32_t skip_foreign;   /* pixels left alone: someone else's pixel (text, boxes, ML menu) */
    uint32_t skip_mark;      /* pixels left alone: another 0x80 mark in the mirror (spotmeter, other grid colour) */
    uint32_t excl;           /* exclusion rectangles of the drawn shape */
    uint32_t us_last, us_max, us_draw_max;  /* step duration; draws only */
    uint32_t y0_mismatch;    /* steps where os.y0 (src/vram.c) did not match the gate's screen yet */
    uint32_t changed;        /* steps that changed ML's buffer (refresh requested by the caller) */
    /* M9.3: the motion display (drawn covers it as well) */
    uint32_t md_drawn, md_n, md_sig, md_draws;
};
const struct r6_grid_stats * r6_grid_stats(void);

#endif
