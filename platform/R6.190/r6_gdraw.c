/*
 * R6.190 M7.4: screen gate for ML's own overlays (FEATURE_GLOBAL_DRAW).
 *
 * get_global_draw() (src/zebra.c) calls r6_gd_screen_ok() on every call, in
 * addition to ML's own conditions (lv, Global Draw ON, display on, no ML
 * menu via liveview_display_idle, LiveViewApp dialog). ML's overlays draw
 * only when all of these hold:
 *  - Canon's LV runs: PROP_LV_ACTION == 0 as recorded by propvalues.c (and
 *    ML's lv == 1, i.e. Debug > LV probe > "LV flag follows Canon" ON,
 *    checked by get_global_draw itself)
 *  - CURRENT_GUI_MODE == 0 (shooting screen; Canon MENU 0x2003, Q 0xC048,
 *    playback 0x2002, quick review 0x4033 are not) and gui_state == 0
 *  - photo mode (movie mode not reviewed for M7.4: ML does not see recording
 *    on R6, PROP_MVR_REC_START is denied; movie LV / FaceYuv not checked).
 *    Two independent indicators, either one blocks: is_movie_mode()
 *    (shooting_mode == SHOOTMODE_MOVIE) and PROP_LV_MOVIE_SELECT == 2
 *    (propvalues.c, existing read-only handler); both values are logged
 *  - LV_OVERLAYS_MODE is one of the INFO variants of the LV shooting screen
 *    (consts.h LV_OVERLAYS_MODE_ML_OK: 0..3; 6 = Canon's settings screen)
 *  - DISPLAY_IS_ON, display mask MEM(MEM(0xEA40)+0x18) exactly 0x01 = LCD
 *    or, M7.6, exactly 0x10 = EVF; not HDMI/LINE 0x1100 (other geometry) and
 *    not both bits at once. M7.7: in the EVF only with Canon's top-aligned
 *    1280x854 stills layout (gd_evf_layout_ok; the image is ML rows
 *    -30..449 there, r6_gdraw.h / src/vram.c)
 *  - LiveViewApp dialog (0x1624C) != 0
 *  - a verified FaceYuv frame (r6_fy_lv_frame via get_yuv422_vram)
 *  - M9.2: no bulb picture of R6 Interval running (r6_iv_bulb_running; also
 *    after its overrun stop while Canon's bulb runs): Canon shows no live
 *    image, ML would draw on a stale frame. The user's own bulb does not pause.
 * Same rules as the M7.3 histogram overlay (r6_overlays.c). RAM reads only,
 * every pointer checked (r6_fy_ram_ok); nothing written, no Canon call.
 */

#include "dryos.h"
#include "bmp.h"
#include "property.h"
#include "propvalues.h"
#include "menu.h"
#include "gui.h"
#include "vram.h"
#include "r6_faceyuv.h"
#include "r6_gdraw.h"
#ifdef CONFIG_R6_INTERVAL
#include "r6_interval.h"           /* M9.2: r6_iv_bulb_running */
#endif

#ifdef CONFIG_R6_GLOBAL_DRAW

#if !defined(CONFIG_R6_LAYER_OWNERSHIP) || !defined(CONFIG_R6_FACEYUV_LV) || !defined(CONFIG_BMP_REAL_IS_INDEXED) \
 || !defined(CONFIG_NO_LVINFO_BARS) || !defined(LV_OVERLAYS_MODE_ML_OK)
#error "R6 global draw needs the ownership refresh, the FaceYuv LV image, ML-only bmp_vram_real, no lvinfo bars and the LV_OVERLAYS_MODE mapping"
#endif
#if defined(FEATURE_ZEBRA_FAST) || defined(FEATURE_MAGIC_ZOOM) || defined(CONFIG_DISPLAY_FILTERS) \
 || defined(FEATURE_FOCUS_PEAK_DISP_FILTER) || defined(FEATURE_VECTORSCOPE) || defined(FEATURE_CROPMARKS) \
 || defined(FEATURE_GHOST_IMAGE) || defined(FEATURE_OVERLAYS_IN_PLAYBACK_MODE) || defined(FEATURE_RAW_HISTOGRAM) \
 || defined(FEATURE_RAW_ZEBRAS) || defined(FEATURE_RAW_SPOTMETER) || defined(CONFIG_RAW_LIVEVIEW) \
 || defined(FEATURE_LV_DISPLAY_PRESETS) || defined(FEATURE_CLEAR_OVERLAYS) || defined(CONFIG_ELECTRONIC_LEVEL) \
 || defined(FEATURE_LV_FOCUS_BOX_AUTOHIDE) || defined(FEATURE_UPSIDE_DOWN) || defined(CONFIG_KILL_FLICKER)
#error "R6 M7.4: overlay feature not reviewed for the shared layer (MMIO, Canon buffers, Canon GUI calls or files)"
#endif

extern int lv_action_raw_get(uint32_t * value, int * len);   /* propvalues.c */
extern struct dialog * LiveViewApp_dialog;                    /* stubs.S DATA_PTR 0x1624C */

#define GD_DISPDEV_PTR 0xEA40     /* [[0xEA40]+0x18] = display mask (0xE01694C8): bit0 PNL, bit4 EVF, 0x1100 LINE */
#define GD_MASK_PNL    0x01
#define GD_MASK_EVF    0x10
#define GD_DVC_PTR     0xEA60     /* display VRAM control (lvprobe LP_DVC_PTR; DISP_GetImageAddress 0xE07B1054) */

/* M7.7: Canon's live EVF image info. DISPIMAGEINFO (0x98 bytes, builder
 * 0xE02995A8, validator 0xE07B0438) is copied for OutKind 1 to DVC+0x390
 * (dispVramCtrl_SetOnlyParamIMGRepVram 0xE061DF20), +0x428 when double
 * buffered; live = +0x00 == 1 && +0x04 == 1 (Canon marks a dead slot 2 / 5).
 * +0x14/+0x18 buffer W/H, +0x24/+0x28 output X/Y in EVF panel pixels
 * (DispVramCtrlEvf 0xE01C130C -> 0xE01BBC60 adds them as the image position),
 * +0x2C/+0x30 visible W/H. 1 = the layout ML maps (EVF stills, Device 4,
 * BlackBelt entry 0x4000: 1280x854 buffer at panel (0,0) = ML rows -30..449,
 * r6_gdraw.h); 0 = anything else (Device 5 "small" layout 1124x748 at
 * (78,106), movie 1280x720 at Y 67, slot not live / not readable). RAM reads,
 * every pointer checked. */
static int gd_evf_layout_ok(void)
{
    static const uint16_t slots[2] = { 0x390, 0x428 };
    uint32_t dvc = MEM(GD_DVC_PTR);
    r6_gd_st.evf_slot = 0;
    for (int s = 0; s < 2; s++)
    {
        uint32_t b = dvc + slots[s];
        if (!dvc || !r6_fy_ram_ok(b, 0x34)) return 0;
        if (MEM(b) != 1 || MEM(b + 4) != 1) continue;
        r6_gd_st.evf_slot = slots[s];
        r6_gd_st.evf_bw = MEM(b + 0x14); r6_gd_st.evf_bh = MEM(b + 0x18);
        r6_gd_st.evf_x = MEM(b + 0x24);  r6_gd_st.evf_y = MEM(b + 0x28);
        r6_gd_st.evf_vw = MEM(b + 0x2C); r6_gd_st.evf_vh = MEM(b + 0x30);
        return r6_gd_st.evf_bw == 1280 && r6_gd_st.evf_bh == 854 && r6_gd_st.evf_x == 0 && r6_gd_st.evf_y == 0;
    }
    return 0;
}

struct r6_gd_stats r6_gd_st = { .mask = 0xFFFFFFFF };
int r6_gd_evf = 0;

static const char * const gd_names[R6GD_NREASONS] = {
    "drawn", "Canon LV off", "Canon GUI mode", "GUI state", "INFO/settings screen",
    "display off", "not LCD/EVF", "no LV dialog", "no FaceYuv frame", "movie mode", "EVF layout",
    "ML bulb picture",
};

const char * r6_gd_reason(int r)
{
    return (r >= 0 && r < R6GD_NREASONS) ? gd_names[r] : "?";
}

static int gd_check(void)
{
    uint32_t v = 1;
    int len = 0;
#ifdef CONFIG_R6_INTERVAL
    if (r6_iv_bulb_running()) return R6GD_ML_BULB;          /* M9.2: erase and stop during ML's bulb */
#endif
    if (!lv_action_raw_get(&v, &len) || len != 4 || v != 0) return R6GD_NO_LV;
    if (CURRENT_GUI_MODE != 0) return R6GD_GUI;
    r6_gd_st.shooting_mode = shooting_mode;
    r6_gd_st.lv_movie_select = lv_movie_select;
    if (is_movie_mode()) { r6_gd_st.movie_sm++; return R6GD_MOVIE; }
    if (lv_movie_select == LVMS_ENABLE_MOVIE) { r6_gd_st.movie_lvms++; return R6GD_MOVIE; }
    if (gui_state != GUISTATE_IDLE) return R6GD_GUISTATE;
    uint32_t ovl = LV_OVERLAYS_MODE;
    r6_gd_st.ovl = ovl;
    if (!LV_OVERLAYS_MODE_ML_OK(ovl)) return R6GD_OVL;
    if (!DISPLAY_IS_ON) return R6GD_DISP_OFF;
    uint32_t dd = MEM(GD_DISPDEV_PTR);
    if (!dd || !r6_fy_ram_ok(dd, 0x1C)) { r6_gd_st.mask = 0xFFFFFFFF; return R6GD_NOT_PANEL; }
    uint32_t mask = MEM(dd + 0x18);
    r6_gd_st.mask = mask;
    if (mask != GD_MASK_PNL && mask != GD_MASK_EVF) return R6GD_NOT_PANEL;     /* LCD or EVF alone: no HDMI/LINE bits */
    if (mask == GD_MASK_EVF && !gd_evf_layout_ok()) return R6GD_EVF_LAYOUT;   /* M7.7: only the layout ML maps */
    if (!LiveViewApp_dialog) return R6GD_NO_DIALOG;
    /* the image source itself: without a verified frame nothing may draw, and
     * what was drawn is erased (instead of zebras frozen on a stale frame) */
    if (!get_yuv422_vram()->vram) return R6GD_NO_IMAGE;
    /* M7.6: the band (r6_gdraw.h) follows the screen; set only here, where
     * the overlays are allowed to draw */
    r6_gd_evf = mask == GD_MASK_EVF;
    if (r6_gd_evf) r6_gd_st.evf_ok++;
    return R6GD_OK;
}

uint32_t r6_gd_disp_mask(void)
{
    uint32_t dd = MEM(GD_DISPDEV_PTR);
    if (!dd || !r6_fy_ram_ok(dd, 0x1C)) return 0xFFFFFFFF;
    return MEM(dd + 0x18);
}

int r6_gd_screen_ok(void)
{
    int r = gd_check();
    r6_gd_st.checks++;
    r6_gd_st.reasons[r]++;
    r6_gd_st.last = r;
    if (!gui_menu_shown()) r6_gd_st.last_outside_menu = r;
    return r;
}

/* ---- conversion cap (bmp.c) ---- */
static int gd_t_conv = 0, gd_conv_seen = 0;
static uint32_t gd_conv_us = 0;

int r6_gd_conv_hold(void)
{
    if (!gd_conv_seen || gui_menu_shown()) return 0;
    uint32_t ms = gd_conv_us * R6_GD_CAP_DUTY / 1000;
    if (ms > R6_GD_CAP_MAX_MS) ms = R6_GD_CAP_MAX_MS;
    r6_gd_st.cap_ms = ms;
    int d = get_ms_clock() - gd_t_conv;
    if (d < 0) { gd_t_conv = get_ms_clock(); d = 0; }     /* clock glitch: restart the interval once */
    if ((uint32_t) d >= ms) return 0;
    r6_gd_st.cap_hold++;
    return 1;
}

void r6_gd_conv_done(uint32_t us)
{
    gd_t_conv = get_ms_clock();
    gd_conv_us = us;
    gd_conv_seen = 1;
}

/* ---- change detection for the overlay boxes ---- */
uint32_t r6_gd_bv_hash_mem(const void * p, uint32_t n)
{
    uint32_t hs = 0x811C9DC5u;
    if (!p) return 0;
    const uint8_t * b = p;
    for (uint32_t i = 0; i < n; i++)
        hs = (hs ^ b[i]) * 0x01000193u;
    return hs;
}

uint32_t r6_gd_bv_hash(int x, int y, int w, int h)
{
    uint8_t * b = bmp_vram();
    if (!b) return 0;
    int x0 = MAX(x, BMP_W_MINUS), x1 = MIN(x + w, BMP_W_PLUS);
    int y0 = MAX(y, BMP_H_MINUS), y1 = MIN(y + h, BMP_H_PLUS);
    uint32_t hs = 0x811C9DC5u;
    for (int yy = y0; yy < y1; yy++)
    {
        const uint8_t * row = b + BM(0, yy);
        for (int xx = x0; xx < x1; xx++)
            hs = (hs ^ row[xx]) * 0x01000193u;
    }
    return hs;
}

#endif /* CONFIG_R6_GLOBAL_DRAW */
