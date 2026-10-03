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

/* M8.0 (approved 2026-10-02): ML menu Shoot > "R6 Interval" (r6_interval.c)
 * takes pictures with call("Release") from its own task, nothing else new
 * (no property write, no other Canon call, no MMIO, no file). Only M or Tv,
 * Tv <= 1 s, single shot, MF, at most 50 pictures; every call checks Canon's
 * state first (RAM reads) and every picture is verified (counter / file
 * number, Canon's job count back to 0), else the series stops. Never saved
 * as running, never started at boot. Upstream's FEATURE_INTERVALOMETER stays
 * off (r6_interval.c #errors on it). */
#define CONFIG_R6_INTERVAL

#define CONFIG_TASK_STRUCT_V2_SMP
#define CONFIG_TASK_ATTR_STRUCT_V5

/* src/rom_values.h assumes 64 MiB ROM1 on DIGIC X, R6 has 32 MiB
 * (gang200.bin, mapped at 0xF0000000). ROM0 is 64 MiB, matching the default. */
#define ROM1_SIZE 0x2000000
