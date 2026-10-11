/**
 * Camera internals for EOS R6 1.9.0
 */

/** This camera has a DIGIC X chip */
#define CONFIG_DIGIC_X

/** Digic 6 and up does not have bitmap font in ROM, try to load it from card **/
#define CONFIG_NO_BFNT

/** disable SRM for now
 * DIGIC X has no old SRM / RscMgr; SRM_*1stJob are dummies that assert
 * (0xE080BB28 / 0xE080BB32, "Resource::RscMgrOldApiStub.c").
 */
#define CONFIG_MEMORY_SRM_NOT_WORKING

/* malloc struct at MALLOC_STRUCT_ADDR uses the V2 layout
 * (get_malloc_info 0xE037C234 reads +4, +8, +0x18, +0x1C, +0x20) */
#define CONFIG_MALLOC_STRUCT_V2

/* LiveView flag (M7.0): ML sets lv = !PROP_LV_ACTION (propvalues.c).
 * PROP_LV_ACTION 0x80050022: 0 = LV started, 1 = LV stopped.
 * - DispOperator property handler compares 0x80050022 at 0xE0750DCA and
 *   prints "PROP_LV_ACTION = %d" (string 0xE0751124)
 * - GUI_SetLvAction 0xE07AA730 stores the value at [[0x16954]+0x1828] and
 *   requests the property
 * - GuiMainEventHandler.c handle_START_LV_XX calls GUI_SetLvAction(0) at
 *   0xE0BD3830
 * - GmtFactory lv_start requests PROP_LV_ACTION = 0 with length 4 at
 *   0xE011DC16 (literal 0x80050022 at 0xE011DFCC, value r5 = 0 from
 *   0xE011DBD2, bl _prop_request_change 0xE07C2854)
 * On R6.190 lv only follows the property while Debug > LV probe > "LV flag
 * follows Canon" is on (default off): CONFIG_LV_FOLLOW_TOGGLE, propvalues.c.
 * CONFIG_LIVEVIEW also compiles in force_liveview and the LV AF frame code;
 * lvprobe.c refuses to build with the features that call them. */
#define CONFIG_LIVEVIEW
#define CONFIG_LV_FOLLOW_TOGGLE

/* M7.2a: guard for ML's OSD refresh (refresh_guard.c, hooks in src/bmp.c
 * refresh_yuv_from_rgb and refresh_yuv_from_rgb_task). Always on for R6,
 * independent of the LV probe. ML runs XimrExe on Canon's XCM context 0 from
 * its own tasks; Canon rebuilds that context without a lock in every
 * VMIX_TransferRectangleToVram (XCM_MakeContext 0xE0260C16 before
 * take_semaphore(winsys_sem, 5000) 0xE0260C22). The guard only suppresses or
 * delays ML's XimrExe: finite take, Canon VMIX state gates, piggyback on
 * Canon's own flushes, 1 s settle after display changes, 5 Hz in LV. RAM
 * reads only, no Canon writes. */
#define CONFIG_R6_REFRESH_GUARD

/* M7.3: ownership-aware conversion into Canon's layer 0 (r6_layer.c, hooks
 * in src/bmp.c refresh_yuv_from_rgb and rgba_buffer_clear): per 32-px block
 * an ownership mask and a hash of what ML wrote (129.6 KB, ML heap). Pixels
 * ML stops drawing are erased only where layer 0 still holds exactly ML's
 * values; empty blocks are skipped; clear and refresh are single writers
 * (guard lock); never the full-alpha (zebra_should_run) path; alpha test
 * '&' instead of '&&'. Needs CONFIG_R6_REFRESH_GUARD. */
#define CONFIG_R6_LAYER_OWNERSHIP

/* M7.4: ML's own overlays (FEATURE_GLOBAL_DRAW, features.h) with Canon's
 * FaceYuv image as LV image source.
 * CONFIG_R6_FACEYUV_LV (src/vram.c, src/imgconv.h, r6_faceyuv.c):
 *   get_yuv422_vram() / get_yuv422_hd_vram() describe FaceYuv (640x426,
 *   pitch 1280, UYVY, signed chroma) and return the uncached alias of the
 *   current buffer, re-verified on every call (r6_fy_locate, the M7.3
 *   sample rules), or a struct with .vram = NULL; never NULL themselves.
 *   bm2lv maps ML's 720x480 LCD area onto the whole buffer; vram_clear_lv
 *   is a no-op. Reads only (approved 2026-10-01).
 * CONFIG_BMP_REAL_IS_INDEXED (src/bmp.c): bmp_vram_real() and
 *   bmp_vram_idle() are ML's indexed buffer (bmp_vram()), never Canon's RGBA
 *   layer; bmp_idle_copy is a no-op. So zebras / false color write ML's
 *   buffer only, and the ownership refresh (r6_layer.c) moves the pixels.
 * CONFIG_NO_LVINFO_BARS (src/lens.c): ML's top / bottom info bars (lvinfo)
 *   stay off: should_draw_bottom_bar() = 0, draw_ml_topbar/bottombar return.
 * CONFIG_R6_GLOBAL_DRAW (src/zebra.c, src/falsecolor.c, src/histogram.c,
 *   r6_gdraw.c): overlays only on the LCD shooting screen (r6_gd_screen_ok),
 *   only with lv == 1 (LV flag follows Canon ON); Global Draw OFF at every
 *   boot (not saved), every overlay defaults to OFF; zebras / false color in
 *   the band y 50..385; no Canon redraw (dialog_redraw) from the overlay
 *   tasks, ML erases its own pixels instead; rate limits; refresh requests
 *   only when a pass changed ML's buffer. Fix round (review 2026-10-02):
 *   - write-under rule in the ownership refresh while the ML menu is closed
 *     (r6_layer.c): ML's pixels never cover Canon's UI pixels, so ML never
 *     erases a Canon pixel; the ML menu still paints over Canon
 *   - conversion cap (bmp.c / r6_gdraw.c): the next conversion waits 10x the
 *     last conversion time (max 2 s) while the ML menu is closed
 *   - refresh guard: no piggyback conversion within 1 s of a display mask /
 *     vrammgr_mode change (LCD <-> EVF, display off/on)
 *   - movie mode blocked by shooting_mode OR PROP_LV_MOVIE_SELECT == 2
 * M7.6 (r6_gdraw.c, src/zebra.c, src/falsecolor.c):
 *   - also on the EVF shooting screen (display mask exactly 0x10). Canon uses
 *     the same layer 0 there with WINSYS origin 1 = 720x540 at (152,0)
 *     (WINSYS_SetRGBAVramActualSizeAndOffset 0xE078C82C, calls at
 *     0xE09D2AD2..0xE09D2B00): ML's 720x480 area at (152,30) is its middle,
 *     where the 1280x854 EVF image lies if Canon centres it in the 1280x960
 *     panel (assumed, checked on the camera). Same ML coordinates on both
 *     screens; zebras / false color cover the whole 720x480 in the EVF
 *     (Canon's EVF info is assumed in the bands above and below)
 *   - focus peaking (dots) on FaceYuv
 *   - _redraw_do (MLEV_REDRAW, from redraw()) no longer calls
 *     dialog_redraw(LiveViewApp_dialog): on R6 that runs Canon's TRV in
 *     GuiMainTask next to CtrlSrv (TRV is not reentrant; suspected M7.1
 *     hang at LCD <-> EVF via _prop_handler_PROP_LV_OUTPUT_TYPE). ML erases
 *     its own pixels instead (clrscr: ML's buffer and ML-owned layer-0
 *     pixels only). No new Canon call. */
#define CONFIG_R6_FACEYUV_LV
#define CONFIG_BMP_REAL_IS_INDEXED
#define CONFIG_NO_LVINFO_BARS
#define CONFIG_R6_GLOBAL_DRAW

/* M7.6 (approved 2026-10-02): "LV flag follows Canon" ON and Global Draw
 * saved (default ON), but only after a clean shutdown. src/module.c
 * module_load_task already marks a running session with ML/modules/
 * LOADING.LCK (removed by module_shutdown at a clean power-off); it calls
 * r6_autostart(clean) (lvprobe.c) once at boot: clean = no lockfile from the
 * last session AND the new one was written. Otherwise (battery pulled after
 * a hang, lockfile not writable, module autoload disabled) the LV flag stays
 * OFF and Global Draw is set OFF, with a NotifyBox. Until then lv = 0, so no
 * overlay can draw before the decision. Battery / card door opened while ML
 * runs (src/tasks.c: PROP_ABORT, PROP_SHUTDOWN_REASON 4 / 8) counts as
 * unclean: module_mark_unclean keeps (or writes back) the lockfile, which
 * upstream removes there. Global Draw from the config is clamped to 0..1. */
#define CONFIG_R6_AUTOSTART

/* M8.0 (approved 2026-10-02) / M8.1 (approved 2026-10-03): ML menu Shoot >
 * "R6 Interval" (r6_interval.c) takes pictures from its own task through the
 * two CameraConductor shutter callbacks (SW1 / SW2, as the MPU interrupt
 * calls them), nothing else new (no property write, no other Canon call, no
 * MMIO, no file). M8.2: src/menu.c menu_open() keeps the ML menu closed while
 * a picture runs (r6_iv_menu_block). Only M with Tv <= 30 s, or B with Canon's
 * bulb timer 31 s .. 30 min (M9.2 / M9.3), single shot, MF, no Canon interval timer / timer
 * setting 0x80040064, at most 50 pictures;
 * every press checks Canon's state first (RAM reads) and every picture is
 * verified (counter / file number, Canon's job count back to 0, CC idle),
 * else the series stops. Never saved as running, never started at boot.
 * Upstream's FEATURE_INTERVALOMETER stays off (r6_interval.c #errors on it). */
#define CONFIG_R6_INTERVAL

/* R6.190 M10.2 "Tor 1" (approved by the user 2026-10-05, effective after the clean M10.1b camera log 2026-10-06;
 * spec m10/m102 spec_m102.md, docs/m10_plan.md 2.x / 5): the Tv write path (r6_propw.c) for R6 Interval's Bracket
 * mode, the HDR bracket per Tv. Only Tv (PROP_SET_TV 0x80000005, exactly 2 bytes Code768), only in M, only the 1/3
 * grid inside 1/8000 .. 30 s and Canon's own Tv ranges, never Bulb; one call site of _prop_request_change, called
 * only from r6_iv_task, never two requests in flight, every write behind its locks and a confirmation window; the
 * user's Tv is restored after every bracket (only while Canon's Tv is still ML's value); a marker file
 * ML/SETTINGS/R6TV.LCK while ML's value may be set, with a notice and "Tv back to ..." at the next start; main switch
 * "Tv writes" saved, default OFF; never a write at startup. CONFIG_PROP_REQUEST_CHANGE stays undefined and
 * prop_write_allow[] stays empty; nothing of the write path is exported to modules or Lua. */
#define CONFIG_R6_PROPW_TV

/* M9.1: Overlay > "Grids & masks" (r6_grid.c, docs/m9_plan.md 1.3): thirds /
 * golden ratio / diagonals, centre mark, aspect frames with masks, safe areas,
 * computed (no card files; the upstream cropmarks stay off). Pixels in ML's
 * indexed buffer with the 0x80 mirror mark (zebras / false color / peaking
 * keep off them), into layer 0 only through the write-under conversion
 * (Canon's elements stay on top); boxes of the other overlays are left out.
 * Three semi-transparent palette entries 26..28 (src/bmp.c). Settings saved
 * in ML's config, default OFF (user decisions 2026-10-04). No Canon call, no
 * write, no MMIO, no file. Needs CONFIG_R6_GLOBAL_DRAW. */
#define CONFIG_R6_GRID

/* R6.190 M9.3: "R6 Motion" (r6_motion.c, Shoot menu): motion trigger on Canon's FaceYuv image (second-newest
 * frame, r6_fy_locate_done; torn frames discarded). Releases only through R6 Interval's iv_shoot (shutter
 * callbacks, approved 2026-10-04); "Display only" never presses. No property write, no MMIO, no new Canon call.
 * Display through the grid's pixel rules. Needs CONFIG_R6_INTERVAL, CONFIG_R6_FACEYUV_LV, CONFIG_R6_GLOBAL_DRAW,
 * CONFIG_R6_GRID. */
#define CONFIG_R6_MOTION

/* R6.190 M10.1 (docs/m10_plan.md section 1, approved 2026-10-05): src/lens.c decodes the R6 1.9.0 lens data
 * (platform/R6.190/include/platform/lens.h: PROP_LENS_DYNAMIC_DATA 0x8C bytes, PROP_LENS_STATIC_DATA 0x1D0) and
 * PROP_APERTURE / PROP_APERTURE_AUTO as 2 bytes Code768 (APEX * 768). Only bytes Canon already delivers to the
 * registered handlers are read differently: no property write, no new Canon call, no new memory access. Other
 * lengths are ignored (no ASSERT), near = far = 0 has no distance, far 0xFFFF = infinity, 64-bit distance math,
 * FL > 2000 mm rejected, extender 0xFF = none. The handlers run in Canon's property task (dynamic data up to
 * ~5 / s): decode and plain stores only, no lock, no print, no allocation. focus_ring_powersave_fix (a property
 * write request on every focus ring turn) is compiled out. */
#define CONFIG_R6_LENS_DATA

/* R6.190 M11.0: "R6 ETTR" (r6_ettr.c, Shoot menu; docs/m11_plan.md, user answers 2026-10-07), the Auto-ETTR advisor
 * ("Display only"): Canon's LV histogram per channel R, G, B (r6_overlays.c r6h_read_canon, with the RandomData
 * binding), a proposed Tv in the ML menu and as one LV line (r6_grid.c, write-under, LCD only), a second log file
 * ETTnnnnn.LOG per snapshot (lvprobe.c). Never a property write, never a press: from the Tv write path only the read-only
 * _r6pw_state / _r6pw_ring / _r6pw_ring_n (r6_propw.c / .h unchanged). Reads only under the existing approvals and Tor 0e
 * (E2). Samples in r6_hist_task (r6_overlays.c), beside r6_iv_task; R6 Interval's bracket calls the read-only
 * r6_ettr_trace(). One more handler on PROP_SHUTTER (an id ML registers anyway). No new Canon call, no MMIO. Needs
 * FEATURE_R6_CANON_HISTOGRAM, CONFIG_R6_INTERVAL, CONFIG_R6_PROPW_TV and (for the LV line) CONFIG_R6_GRID. */
#define CONFIG_R6_ETTR

/* R6.190 M12.0: "Astro red" (r6_astro.c, Display menu; docs/m12_plan.md Rev. 2, spec m12/m120 spec_m120.md Rev. 2, user
 * answers of 2026-10-11 E1 a, E2, E5, E6, E8 c, E9, E13 a). Mode "ML only": ML's own palette by meaning in pure red, a level
 * per output (LCD / EVF 100/50/25/12/150/200 %), mapped in r6_layer.c r6l_build_lut once per conversion; black stays
 * 0xFF010101 and transparent entries stay exact; saved and on again at boot (no Canon access). The transparent ML menu view
 * is switched off while it is on. Tor 0f (E2, read only): Debug > LV probe "Display regs" (not saved) reads the status
 * word of the LCD / EVF output (0xD2823000 / 0xD2833000) about 4 / s and its 13 colour registers only while that output is
 * on and shown, Canon's display tasks were found on CPU0 and the reading task runs on CPU0, inside one IRQ lock
 * (r6as_mmio_rd, the only MMIO access; never +0x3010, +0x3020..0x3034, HDMI or Hrady0; nothing after
 * ml_shutdown_requested), a transition trace 50 / s for 2 s. ML/LOGS/ASTnnnnn.LOG per log snapshot, "Log snapshot in
 * 10 s" and "Key test" (lvprobe.c). No MMIO write, no Canon RAM write, no property write, no new Canon call. Needs
 * CONFIG_R6_LAYER_OWNERSHIP, CONFIG_R6_GLOBAL_DRAW, FEATURE_LV_BUFFER_PROBE and CONFIG_R6_ETTR (one file number). */
#define CONFIG_R6_ASTRO

#define CONFIG_TASK_STRUCT_V2_SMP
#define CONFIG_TASK_ATTR_STRUCT_V5

/* src/rom_values.h assumes 64 MiB ROM1 on DIGIC X, R6 has 32 MiB
 * (gang200.bin, mapped at 0xF0000000). ROM0 is 64 MiB, matching the default. */
#define ROM1_SIZE 0x2000000
