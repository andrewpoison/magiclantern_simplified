/* R6.190 M15.0 (spec m15/m150 3.2): the bigger R6 blocks of src/zebra.c, moved here without a change of behaviour.
 * Not a translation unit of its own and not in the Makefile: src/zebra.c includes this file (like upstream cropmarks.c) at the
 * place of r6_gd_erase / r6_grid_tick, before lens_display_dirty. So every block keeps zebra.c's static variables, macros
 * and inlining (the moved hook blocks are static functions called once). The R6 changes inside upstream functions stay in
 * zebra.c (they use the caller's locals or return in the middle). Prototypes: r6_zebra.h. */
#ifndef _r6_zebra_h_
#error "r6_zebra.c is included by src/zebra.c only (after r6_zebra.h)"
#endif


#if defined(FEATURE_HISTOGRAM) && defined(CONFIG_R6_GLOBAL_DRAW)
/* R6 M7.5: hist_build without the per-pixel loop over FaceYuv (camera M7.4:
 * histogram + waveform 50..75 ms per pass, peak 230 ms, ~86400 uncached
 * pixel loads).
 *  - histogram: Canon's own LV histogram bins (r6_ml_hist_fill, the checks
 *    of the "Histogram (Canon)" box), filled like the loop below would
 *    (128 bins, luma always, R/G/B for the RGB type, max without bin 0,
 *    total_px = numPix); hist_draw_image keeps ML's log scale and clip
 *    warnings. Not usable (set / sum / range / FrameNum check failed, torn)
 *    -> the last good histogram is kept while it is at most R6_MLH_HOLD_MS
 *    old (fix round: no flicker and no erase + redraw conversion for a
 *    single bad read), then nothing (histogram.total_px 0,
 *    r6_gd_st.hist_valid 0: the box is erased, draw_histogram_and_waveform);
 *    stale (FrameNum frozen 1 s) -> nothing at once. No fallback pixel loop: it is the
 *    cost being removed, and it would run exactly in Canon's transitions
 *    (set switch, display path change) where FaceYuv is about to be refused
 *    as well; a FaceYuv histogram also has another scale than Canon's (Y of
 *    a white wall 85 vs 99, M7.3), so the box would jump between sources.
 *  - waveform (only if enabled): one sample per waveform column (its first
 *    ML x, 720 / 180 = 4 px) in every R6_WFM_YSTEP-th ML row = 180 x 60 =
 *    10800 samples instead of 86400; each adds R6_WFM_WEIGHT (8 = 480 / 60
 *    samples per column before) up to the same 250 cap, so the brightness of
 *    the traces stays the same on average. Same columns (x -> column as in
 *    waveform_add_pixel), same levels, same BM2LV mapping, same box.
 *    Fix round: the row phase is staggered per column (0, 6, 4, 2 ML rows,
 *    R6_WFM_PHASE), so together the columns sample every even ML row - all
 *    rows of M7.4's loop - and a thin horizontal feature (horizon, a line
 *    of highlights) shows in every 4th column instead of in none; static
 *    per column, so a static scene still gives an unchanged waveform (no
 *    redraw). The weight-8 samples make single hits visible (M7.4 hid cells
 *    with fewer than 4 of 480 hits): sparser, speckled traces are the
 *    accepted cost of 1/8 of the samples. */
#define R6_WFM_YSTEP  8
#define R6_WFM_WEIGHT 8
#define R6_WFM_PHASE(c) (2 * (((c) * 3) & 3))   /* 0, 6, 4, 2, 0, ... (even ML rows: M7.4 sampled y += 2) */
#define R6_MLH_HOLD_MS 1000
static void r6_hist_build(const uint32_t * buf)
{
    static int r6_mlh_t_ok = 0, r6_mlh_ok = 0;  /* last good fill (ms), histogram holds it */
    int rgb = (hist_type == 1 || hist_type == 2) && !EXT_MONITOR_RCA;   /* as below */
    int s = R6H_OFF;
    if (hist_draw)
    {
        R6GD_T0();
        s = r6_ml_hist_fill(&histogram, rgb);   /* SHOWN: histogram refilled; else untouched */
        r6_gd_st.hcanon_state = s;
        if (s == R6H_SHOWN) r6_gd_st.hcanon_ok++; else r6_gd_st.hcanon_fail++;
        R6GD_T1(hcanon_n, hcanon_us, hcanon_us_max);
    }
    int now = get_ms_clock();
    if (s == R6H_SHOWN)
    {
        r6_mlh_t_ok = now;
        r6_mlh_ok = 1;
        r6_gd_st.hist_valid = 1;
    }
    else if (hist_draw && s != R6H_STALE && r6_mlh_ok && now >= r6_mlh_t_ok && now - r6_mlh_t_ok <= R6_MLH_HOLD_MS)
    {
        r6_gd_st.hist_hold++;                    /* last good histogram kept (at most 1 s old) */
        r6_gd_st.hist_valid = 1;
    }
    else
    {
        memset(&histogram, 0, sizeof(histogram));
        histogram.is_rgb = rgb;
        r6_mlh_ok = 0;
        r6_gd_st.hist_valid = 0;
    }
    #ifdef FEATURE_WAVEFORM
    if (waveform_draw)
    {
        waveform_init();
        if (!waveform) return;
        R6GD_T0();
        uint16_t xw[WAVEFORM_WIDTH];            /* LV word offset of each column's sample */
        for (int c = 0; c < WAVEFORM_WIDTH; c++)
        {
            int x = os.x0 + (c * os.x_ex + WAVEFORM_WIDTH - 1) / WAVEFORM_WIDTH;   /* first ML x of column c */
            xw[c] = (BM2LV_X(x) << 1) >> 2;
        }
        uint32_t n = 0;
        int off = get_y_skip_offset_for_histogram();
        /* M7.7: this runs in livev_loprio_task without BMP_LOCK, and os.y0 /
         * bm2lv.ty now change at LCD <-> EVF (src/vram.c): take one
         * consistent snapshot, and bound every row to the FaceYuv buffer */
        int r6_y0, r6_ym, r6_sy, r6_ty;
        BMP_LOCK( r6_y0 = os.y0; r6_ym = os.y_max; r6_sy = bm2lv.sy; r6_ty = bm2lv.ty; )
        int y1 = r6_ym - off;
        for (int y = r6_y0 + off; y < y1; y += R6_WFM_YSTEP)
        {
            const uint32_t * rows[R6_WFM_YSTEP];
            for (int k = 0; k < R6_WFM_YSTEP; k++)
            {
                int ly = (((y + k) * r6_sy) >> 10) + r6_ty;     /* = BM2LV_Y(y + k) */
                rows[k] = (y + k < y1 && ly >= 0 && ly < R6_FY_H) ? buf + ly * (R6_FY_PITCH / 4) : NULL;
            }
            for (int c = 0; c < WAVEFORM_WIDTH; c++)
            {
                const uint32_t * row = rows[R6_WFM_PHASE(c)];
                if (!row) continue;                  /* last group: row past the end */
                uint32_t pixel = row[xw[c]];
                int Y = UYVY_GET_AVG_Y(pixel);
                uint8_t * w = &WAVEFORM(c, (Y * WAVEFORM_HEIGHT) >> 8);
                if (*w < 250) *w = MIN(*w + R6_WFM_WEIGHT, 250);
                n++;
            }
        }
        r6_gd_st.wfm_samples = n;
        R6GD_T1(wfm_n, wfm_us, wfm_us_max);
    }
    #endif
}
#endif

#ifdef CONFIG_R6_GLOBAL_DRAW
/* R6 M7.6: peaking was switched off while dots are shown (overlay task, ML
 * menu closed, BMP_LOCK held): erase the dots that are still there (indexed
 * pixel = mirror pixel) instead of restoring old, possibly stale values;
 * other overlays draw again by themselves. ML's buffer only. */
static void r6_peak_off(void)
{
    #ifdef FEATURE_FOCUS_PEAK
    if (!dirty_pixels_num) return;
    uint8_t * const bvram = bmp_vram_real();
    if (bvram && bvram_mirror && dirty_pixels)
    {
        for (int i = 0; i < dirty_pixels_num; i++)
        {
            uint16_t * b1 = (uint16_t*)(bvram + dirty_pixels[i]);
            uint16_t * b2 = (uint16_t*)(bvram + dirty_pixels[i] + BMPPITCH);
            uint16_t * m1 = (uint16_t*)(bvram_mirror + dirty_pixels[i]);
            uint16_t * m2 = (uint16_t*)(bvram_mirror + dirty_pixels[i] + BMPPITCH);
            #ifdef CONFIG_R6_GRID
            if ((*m1 | *m2) & 0x8080)       /* M9.1 review: a grid / spotmeter pixel (0x80 mark) took a pixel of the dot over */
            {
                for (int k = 0; k < 4; k++)
                {
                    uint8_t * pb = bvram + dirty_pixels[i] + (k >> 1) * BMPPITCH + (k & 1);
                    uint8_t * pm = bvram_mirror + dirty_pixels[i] + (k >> 1) * BMPPITCH + (k & 1);
                    if (*pm & 0x80) continue;     /* theirs: pixel and mark stay */
                    if (*pb == *pm) *pb = 0;
                    *pm = 0;
                }
                continue;
            }
            #endif
            if (*b1 == *m1) *b1 = 0;
            if (*b2 == *m2) *b2 = 0;
            *m1 = *m2 = 0;
        }
    }
    dirty_pixels_num = 0;
    r6_peak_n = -1;
    r6_gd_st.peak_off++; r6_gd_st.refresh_req++;
    ml_refresh_display_needed = 1;
    #endif
}
#endif

#ifdef FEATURE_GLOBAL_DRAW
/* global_draw_display (zebra.c): the R6 warnings of the Global Draw entry */
static MENU_UPDATE_FUNC(r6_gd_menu_info)
{
    extern int lv_follow_canon_get(void);       /* propvalues.c */
    if (global_draw && !lv_follow_canon_get())
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Needs Debug > LV probe > LV flag follows Canon = ON.");
    else if (global_draw)
    {
        int r = r6_gd_st.last_outside_menu;
        if (r == R6GD_MOVIE)
            MENU_SET_WARNING(MENU_WARN_INFO, "Last LV screen: not drawn (movie mode: mode 0x%x, movie select %d).",
                (int) r6_gd_st.shooting_mode, (int) r6_gd_st.lv_movie_select);
        else if (r != R6GD_OK)
            MENU_SET_WARNING(MENU_WARN_INFO, "Last LV screen: not drawn (%s). LCD shooting screen only.", r6_gd_reason(r));
        else
            MENU_SET_WARNING(MENU_WARN_INFO, "LCD shooting screen only. Not saved: OFF at boot.");
    }
}
#endif

#ifdef FEATURE_HISTOGRAM
/* draw_histogram_and_waveform, if hist_draw: at the place of Canon's histogram box (r6_overlays.c steps aside) */
static void r6_hist_box(void)
{
    /* skipped while the bins and ML's box are as last drawn (the box
     * hash also catches an erase / clear by someone else); a refresh is
     * requested only if the drawing changed ML's buffer */
    static uint32_t r6_hk = 0, r6_hbox = 0;
    static int r6_hvalid = 0;
    R6GD_T0();
    uint32_t k = r6_gd_bv_hash_mem(&histogram, sizeof(histogram));
    int changed = 0;
    BMP_LOCK(
        uint32_t pre = r6_gd_bv_hash(R6_GD_HIST_X - 1, R6_GD_HIST_Y - 1, HIST_WIDTH + 2, hist_height + 2);
        if (!r6_gd_st.hist_valid)
        {
            /* M7.5: Canon's bins not usable (r6_hist_build): no
             * histogram - erase ML's box once (ML's pixels only) */
            r6_gd_st.hist_none++;
            if (r6_hvalid && !gui_menu_shown())
            {
                bmp_fill(COLOR_EMPTY, R6_GD_HIST_X - 1, R6_GD_HIST_Y - 1, HIST_WIDTH + 2, hist_height + 2);
                r6_hvalid = 0;
                changed = r6_gd_bv_hash(R6_GD_HIST_X - 1, R6_GD_HIST_Y - 1, HIST_WIDTH + 2, hist_height + 2) != pre;
            }
        }
        else if (!(r6_hvalid && k == r6_hk && pre == r6_hbox))
        {
            hist_draw_image( R6_GD_HIST_X, R6_GD_HIST_Y );
            r6_gd_st.drawn |= R6GD_DRAWN_HIST;
            r6_hbox = r6_gd_bv_hash(R6_GD_HIST_X - 1, R6_GD_HIST_Y - 1, HIST_WIDTH + 2, hist_height + 2);
            r6_hk = k; r6_hvalid = 1;
            changed = r6_hbox != pre;
        }
    )
    if (changed) { r6_gd_st.refresh_req++; ml_refresh_display_needed = 1; }
    else r6_gd_st.refresh_same++;
    R6GD_T1(hdraw_n, draw_us, draw_us_max);
}
#endif

#ifdef FEATURE_WAVEFORM
/* draw_histogram_and_waveform, if waveform_draw && waveform: small waveform only, above Canon's bottom bar */
static void r6_wfm_box(void)
{
    /* same rule as the histogram box (waveform data + box hash) */
    static uint32_t r6_wk = 0, r6_wbox = 0;
    static int r6_wvalid = 0;
    R6GD_T0();
    uint32_t k = r6_gd_bv_hash_mem(waveform, WAVEFORM_WIDTH * WAVEFORM_HEIGHT);
    int changed = 0;
    BMP_LOCK(
        uint32_t pre = r6_gd_bv_hash(R6_GD_WFM_X - 1, R6_GD_WFM_Y - 1, WAVEFORM_WIDTH + 2, R6_GD_WFM_H + 2);
        if (!(r6_wvalid && k == r6_wk && pre == r6_wbox))
        {
            waveform_draw_image( R6_GD_WFM_X, R6_GD_WFM_Y, R6_GD_WFM_H );
            r6_gd_st.drawn |= R6GD_DRAWN_WFM;
            r6_wbox = r6_gd_bv_hash(R6_GD_WFM_X - 1, R6_GD_WFM_Y - 1, WAVEFORM_WIDTH + 2, R6_GD_WFM_H + 2);
            r6_wk = k; r6_wvalid = 1;
            changed = r6_wbox != pre;
        }
    )
    if (changed) { r6_gd_st.refresh_req++; ml_refresh_display_needed = 1; }
    else r6_gd_st.refresh_same++;
    R6GD_T1(wdraw_n, draw_us, draw_us_max);
}
#endif

#ifdef CONFIG_R6_GLOBAL_DRAW
/* R6 M7.4: the overlay screen state ended (Canon MENU / Q / INFO settings,
 * display off, HDMI, LV off, Global Draw OFF; M7.6: no longer the EVF, but
 * any mask other than exactly LCD or EVF, e.g. during a switch): erase what ML's overlays put
 * into ML's indexed buffer - zebra / false color pixels via the mirror (only
 * pixels that still hold exactly what the overlay wrote), the spotmeter, the
 * histogram and waveform boxes - and request a refresh. The ownership
 * refresh (r6_layer.c) then erases them in layer 0 only where layer 0 still
 * holds ML's values, under the refresh guard's gates. Instead of redraw(),
 * which asks Canon to repaint (dialog_redraw(LiveViewApp_dialog)) while
 * Canon's own screen is changing. Never while the ML menu is shown (the menu
 * owns the indexed buffer and clears it on close). */
static void r6_gd_erase(void)
{
    if (!r6_gd_st.drawn) return;
    if (gui_menu_shown()) return;
    BMP_LOCK(
        if (bmp_vram_indexed_ready() && bvram_mirror && !gui_menu_shown())
        {
            uint32_t d = r6_gd_st.drawn;
            if (d & R6GD_DRAWN_MIRROR) clrscr_mirror_do();
            #ifdef FEATURE_FOCUS_PEAK
            /* M7.6: the peaking dots are mirror pixels too; nothing left to restore */
            if (d & R6GD_DRAWN_MIRROR) { dirty_pixels_num = 0; r6_peak_n = -1; }
            #endif
            #ifdef FEATURE_SPOTMETER
            if (d & R6GD_DRAWN_SPOT) spotmeter_erase();
            #endif
            if (d & R6GD_DRAWN_HIST) bmp_fill(COLOR_EMPTY, R6_GD_HIST_X - 1, R6_GD_HIST_Y - 1, HIST_WIDTH + 2, hist_height + 2);
            if (d & R6GD_DRAWN_WFM) bmp_fill(COLOR_EMPTY, R6_GD_WFM_X - 1, R6_GD_WFM_Y - 1, WAVEFORM_WIDTH + 2, R6_GD_WFM_H + 2);
            #ifdef CONFIG_R6_GRID
            /* M9.1: the grid's own pixels (indexed buffer and mirror mark;
             * clrscr_mirror_do above leaves marked pixels alone) */
            if (d & R6GD_DRAWN_GRID) r6_grid_erase();
            #endif
            r6_gd_st.drawn = 0;
            r6_gd_st.erase_n++;
            r6_gd_st.refresh_req++;
            ml_refresh_display_needed = 1;
        }
    )
}
#endif

#ifdef CONFIG_R6_GRID
/* M9.1 (platform/R6.190/r6_grid.c): one grid step, every R6_GD_GRID_MS from
 * the overlay task, BMP_LOCK held by the caller. The ML menu and the overlay
 * gate are checked again here, inside the lock. Exclusions: the erase / hash
 * rectangles of the boxes that redraw when their area in ML's buffer changes
 * (r6_grid.h): ML's histogram and waveform boxes (as in r6_gd_erase), the
 * spotmeter (R6_SPOT_BOX in spotmeter_step; where it is drawn, else at the
 * centre where spotmeter_step puts it on R6), "Histogram (Canon)". */
static void r6_grid_tick(void)
{
    if (!lv || gui_menu_shown() || !bvram_mirror || !zebra_should_run()) return;
    struct r6_grid_rect ex[R6_GRID_NEXCL];
    int n = 0;
    #define R6GR_EX(x, y, w, h) do { ex[n].x0 = (x); ex[n].y0 = (y); ex[n].x1 = (x) + (w); ex[n].y1 = (y) + (h); n++; } while (0)
    #ifdef FEATURE_HISTOGRAM
    if (hist_draw) R6GR_EX(R6_GD_HIST_X - 1, R6_GD_HIST_Y - 1, HIST_WIDTH + 2, hist_height + 2);
    #endif
    #ifdef FEATURE_WAVEFORM
    if (waveform_draw) R6GR_EX(R6_GD_WFM_X - 1, R6_GD_WFM_Y - 1, WAVEFORM_WIDTH + 2, R6_GD_WFM_H + 2);
    #endif
    #ifdef FEATURE_SPOTMETER
    if (spotmeter_draw)
    {
        int xc = spotmeter_dirty ? spot_prev_xcb : os.x0 + os.x_ex / 2;
        int yc = spotmeter_dirty ? spot_prev_ycb : os.y0 + os.y_ex / 2;
        int dx = spotmeter_formula == 2 ? 52 : 26;
        R6GR_EX(xc - dx - 4, (yc & ~1) - 13, 2 * dx + 12, 51);
    }
    #endif
    #ifdef FEATURE_R6_CANON_HISTOGRAM
    if (r6h_mode) R6GR_EX(R6H_BOX_X0, R6H_BOX_Y0, R6H_BOX_X1 - R6H_BOX_X0, R6H_BOX_Y1 - R6H_BOX_Y0);
    #endif
    #undef R6GR_EX
    if (r6_grid_step(ex, n)) { r6_gd_st.refresh_req++; ml_refresh_display_needed = 1; }
}
#endif

/* livev_hipriority_task (zebra.c), every pass after the zebra_should_run check: */
/* M7.6: the screen of the last passed check changed (LCD <-> EVF):
 * the band changed (r6_gdraw.h), so erase once (a no-op if a failed
 * check in between erased already); on the LCD nothing may stay in
 * Canon's top row / bottom bar from the EVF band */
static void r6_lv_evf_check(void)
{
    static int r6_evf_seen = -1;
    if (r6_evf_seen != r6_gd_evf)
    {
        if (r6_evf_seen >= 0) { r6_gd_st.evf_switch++; r6_gd_erase(); }
        r6_evf_seen = r6_gd_evf;
        vram_params_set_dirty();    /* M7.7: os.y0 / bm2lv follow the screen (src/vram.c) */
    }
}

#ifdef CONFIG_R6_GRID
/* M9.1: grids & masks before the other overlays (their mirror marks
 * keep zebras / false color / peaking off the lines); a repair only
 * unless the settings, the screen or an exclusion changed */
static void r6_lv_grid_poll(void)
{
    if (r6_grid_wanted() || r6_grid_stats()->drawn)     /* review: OFF and erased: no lock, no step */
    {
        static int r6_gr_aux = 0;
        if (should_run_polling_action(R6_GD_GRID_MS, &r6_gr_aux))
            BMP_LOCK( r6_grid_tick(); )
    }
}
#endif
