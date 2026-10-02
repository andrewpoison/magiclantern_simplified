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
 *   - movie mode blocked by shooting_mode OR PROP_LV_MOVIE_SELECT == 2 */
#define CONFIG_R6_FACEYUV_LV
#define CONFIG_BMP_REAL_IS_INDEXED
#define CONFIG_NO_LVINFO_BARS
#define CONFIG_R6_GLOBAL_DRAW

#define CONFIG_TASK_STRUCT_V2_SMP
#define CONFIG_TASK_ATTR_STRUCT_V5

/* src/rom_values.h assumes 64 MiB ROM1 on DIGIC X, R6 has 32 MiB
 * (gang200.bin, mapped at 0xF0000000). ROM0 is 64 MiB, matching the default. */
#define ROM1_SIZE 0x2000000
