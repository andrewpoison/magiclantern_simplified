/*
 * R6.190 M7.4: Canon's FaceYuv image as ML's LiveView image source.
 *
 * Factored out of lvprobe.c (M7.3 "FaceYuv sample", camera-verified
 * 2026-10-02: the picked buffer is the one of the newest ring frame in every
 * snapshot). Used by lvprobe.c (sample, log) and src/vram.c
 * (get_yuv422_vram under CONFIG_R6_FACEYUV_LV).
 *
 * READ-ONLY. Plain RAM reads of Canon globals and of the FaceYuv controller
 * context, with the lvprobe.c rules: every pointer non-zero, 4-byte aligned,
 * the whole access inside 0x1000..0x3FFFFFFF (cached RAM) or, only if
 * Canon's memif RAM copy is filled and windows 4..7 equal windows 0..3,
 * 0x40001000..0x7FFFFFFF (uncached alias). Page 0 is unmapped on R6.
 * 64-bit values are read twice and must match. No Canon function is called,
 * nothing is written. The pixels themselves are read by ML's overlay code
 * through the returned uncached CPU address (approved 2026-10-01).
 *
 * Buffer choice (ROM 1.9.0, LiveViewComp::LvFaceYuvController.c):
 *  - ctx = MEM(0x1ED68+0xC) (0xB060 bytes); 4 YUV buffers as 64-bit IBus
 *    addresses at ctx+0x20+8*k (MEMNAVI region 0x10004, count 4 asserted at
 *    0xE08092E2; copied at 0xE0809376..0xE0809386)
 *  - the next-buffer getter 0xE0809390 hands out ctx+0x20+8*[ctx+0xB029]
 *    and then increments that byte mod 4 (0xE08093AC..0xE08093BE)
 *  - the per-frame step 0xE0809212 stores {flag, FrameNum} into the 3-entry
 *    ring 0x164614 at index s8 MEM(0x1ED68) and increments it mod 3
 *    (0xE0809290..0xE08092B6); newest = (index + 2) % 3
 *  - the consumer request 0xE0809550 (r0 type 0..2, r1 FrameNum, r2 request)
 *    looks the frame up in that ring (0xE0809602..0xE080961A) and fills a
 *    record ctx+0x40+0xA0*k, k = [ctx+0xB02A] then incremented mod 4
 *    (0xE080958A..0xE0809598): +4 = FrameNum (strd at 0xE08095A2), +0x10 =
 *    the request's 64-bit buffer address (ldrd/strd 0xE08095B8/0xE08095BC)
 * So: FrameNum of the newest ring entry -> the record with that FrameNum ->
 * its buffer address, accepted only if it equals one of the 4 buffers
 * ("rec"). Else the newest record not newer than that frame ("rec_old").
 * Else the getter rule: (ctx[0xB029] - 2) mod 4, i.e. the buffer handed out
 * before the most recent one (the most recent may still be written) ("get").
 *
 * M9.3 (r6_fy_locate_done, the motion detector; spec m9/m93 spec_m93.md 1.1 -
 * 1.3 with the corrections of spec_m93_skeptic.md A1 / A2 / A5). R2Y job of
 * one LV frame N (raw2yuvProcessPath 0xE0365006): ring step 0xE0365100 ->
 * 0xE0809212 ({flag, N} into the ring), then the getter 0xE0364FE4 ->
 * 0xE0809390 (hands out ctx+0x20+8*[ctx+0xB029], then +1 mod 4), then the
 * record 0xE0365428 -> 0xE0809550 ("YuvStar": +4 N, +0x10 the buffer), then
 * the kick 0xE036542C (the hardware writes the buffer only after ring entry
 * and record exist). So the newest frame F1 may still be written. Its record
 * proves only that F1 was STARTED: the stage accepts the next job before the
 * last one's outputs are done (state object "Raw2YuvStage", PathComplete in
 * state 1, overlap bookkeeping [0x12A44+0x28], skeptic A1 / A2), so "F2 is
 * complete" is inferred from timing, not proven; the detector discards torn
 * frames (r6_motion.c) and needs Confirm frames in a row. [ctx+0xB048] bit 1
 * hands out a debug dump area (0xE08093C2..0xE08093F8), bit 2 always buffer 0
 * while the record stores buffer 3 (0xE0809404 / 0xE08096AE): read only with
 * both bits 0. Rules (a) [ctx+0xB048] & 6 == 0, (b) F1 newest, F2 = the ring
 * entry before it, F1 - F2 in 1..16, (c) a record of F1 naming one of the 4
 * buffers, (d) a record of F2 naming another one, (e) exactly one hand-out
 * since F2: F1 in (g+3)&3, F2 in (g+2)&3. F2's buffer is handed out again
 * only by the 3rd hand-out after g: r6_fy_recheck() after the read.
 */

#include "dryos.h"
#include "r6_faceyuv.h"

#define FY_STATE      0x1ED68   /* LvFaceYuvController: +0xC = ctx, s8 [+0] ring index */
#define FY_CTX_SIZE   0xB060
#define FY_RING       0x164614  /* 3 x {flag, FrameNum} */
#define FY_LVIC_SIZE  0x11FC8   /* u16 w, u16 h (640 x 426) */
#define FY_GET_IDX    0xB029
#define FY_B048       0xB048    /* M9.3: getter mode bits (bit 1 debug dump area, bit 2 buffer 0 / record buffer 3) */
#define FY_REC_IDX    0xB02A
#define FY_REC        0x40
#define FY_REC_STRIDE 0xA0
#define FY_MEMIF_WIN  0x1355A4  /* RAM copy of the 12 static windows (copy 0xE089F230), [11] = copied flag */
#define FY_CH_LIMIT   0x106484  /* DRAM channel end ch1..5 (MemifApiInit 0xE01053BC) */
#define FY_STALE_MS   1000

#define RD(a) (*(volatile uint32_t *)(a))

extern int lv_action_raw_get(uint32_t * value, int * len);   /* propvalues.c (CONFIG_LV_FOLLOW_TOGGLE) */

/* ---- guarded reads (same rules as lvprobe.c) ---- */
static int fy_uc_ok(void)
{
    if (!RD(FY_MEMIF_WIN + 0x2C)) return 0;
    for (int i = 0; i < 4; i++)
        if (RD(FY_MEMIF_WIN + 4 * (i + 4)) != RD(FY_MEMIF_WIN + 4 * i)) return 0;
    return 1;
}

static int fy_region(uint32_t a, int uc)
{
    if (a >= 0x1000 && a < 0x40000000) return 1;
    if (uc && a >= 0x40001000 && a < 0x80000000) return 2;
    return 0;
}

/* [a, a+n) aligned and inside one allowed region */
static int fy_ram_ok(uint32_t a, uint32_t n, int uc)
{
    if ((a & 3) || !n) return 0;
    int r = fy_region(a, uc);
    return r && a + n - 1 > a && fy_region(a + n - 1, uc) == r;
}

int r6_fy_ram_ok(uint32_t a, uint32_t n)
{
    return fy_ram_ok(a, n, fy_uc_ok());
}

static int fy_rd32(uint32_t a, uint32_t * v, int uc)
{
    if (!fy_ram_ok(a, 4, uc)) return 0;
    *v = RD(a);
    return 1;
}

/* byte at a, read as the aligned word that contains it */
static int fy_rd8(uint32_t a, uint32_t * v, int uc)
{
    uint32_t w;
    if (!fy_rd32(a & ~3u, &w, uc)) return 0;
    *v = (w >> (8 * (a & 3))) & 0xFF;
    return 1;
}

/* 1 = ok, -1 = changed between the two reads (4 tries), 0 = not readable */
static int fy_rd64(uint32_t a, uint32_t * lo, uint32_t * hi, int uc)
{
    if (!fy_ram_ok(a, 8, uc)) return 0;
    for (int i = 0; i < 4; i++)
    {
        uint32_t l1 = RD(a), h1 = RD(a + 4);
        uint32_t l2 = RD(a), h2 = RD(a + 4);
        *lo = l2; *hi = h2;
        if (l1 == l2 && h1 == h2) return 1;
    }
    return -1;
}

/* static window (4..11) of the RAM copy that maps IBus hi:lo by Canon's rule
 * (0xE089F2B8..0xE089F2D4: same channel, same 256 MiB slice); arithmetic only */
static int fy_win_for(const uint32_t * win, uint32_t lo, uint32_t hi)
{
    if (!lo && !hi) return -1;
    for (int i = 4; i < 12; i++)
        if ((win[i] & 7) == hi && (win[i] & 0xF0000000) == (lo & 0xF0000000) && !(win[i] & 0x0FFFFFF8))
            return i;
    return -1;
}

static int fy_fail(struct r6_fy_loc * r, const char * why)
{
    r->ok = 0;
    r->why = why;
    r->cpu = 0;
    return 0;
}

/* M9.3: the record with exactly this FrameNum that names one of the 4 buffers; index, *pick = buffer (else -1) */
static int fy_rec_find(uint32_t ctx, uint32_t frame, const uint32_t * blo, const uint32_t * bhi, int * pick, int uc)
{
    for (int k = 0; k < 4; k++)
    {
        uint32_t rb = ctx + FY_REC + FY_REC_STRIDE * k, lo, hi;
        if (RD(rb + 4) != frame || fy_rd64(rb + 0x10, &lo, &hi, uc) != 1) continue;
        for (int j = 0; j < 4; j++) if (lo == blo[j] && hi == bhi[j]) { *pick = j; return k; }
    }
    return -1;
}

/* done = 0: r6_fy_locate (unchanged); done = 1: r6_fy_locate_done (M9.3) */
static int fy_locate(struct r6_fy_loc * r, int done)
{
    memset(r, 0, sizeof(*r));
    r->pick = r->rec = r->win = r->ring_i = r->pick1 = -1;
    r->why = "";
    int uc = fy_uc_ok();

    uint32_t v, ctx = RD(FY_STATE + 0xC);
    if (!ctx) return fy_fail(r, "ctx_null");
    if (!fy_ram_ok(ctx, FY_CTX_SIZE, uc)) return fy_fail(r, "ctx_invalid");
    if (!fy_rd32(FY_LVIC_SIZE, &v, uc) || v != (((uint32_t) R6_FY_H << 16) | R6_FY_W)) return fy_fail(r, "size");
    uint32_t g = 0, rr = 0;
    fy_rd8(ctx + FY_GET_IDX, &g, uc);
    fy_rd8(ctx + FY_REC_IDX, &rr, uc);
    r->gidx = g; r->ridx = rr;
    if (!fy_rd8(FY_STATE, &v, uc)) return fy_fail(r, "ring_idx");
    r->ring_i = (int8_t) v;
    if (r->ring_i < 0 || r->ring_i > 2) return fy_fail(r, "ring_idx");
    int newest = (r->ring_i + 2) % 3;
    r->ring_flag = RD(FY_RING + 8 * newest);
    r->ring_frame = RD(FY_RING + 8 * newest + 4);

    uint32_t blo[4], bhi[4];
    for (int k = 0; k < 4; k++)
        if (fy_rd64(ctx + 0x20 + 8 * k, &blo[k], &bhi[k], uc) != 1) return fy_fail(r, "buf_unstable");

    if (done)
    {
        /* M9.3: rules (a) .. (e), see the header; never the getter fallback */
        uint32_t b = 0xFF;
        r->ctx = ctx;
        if (!fy_rd8(ctx + FY_B048, &b, uc)) b = 0xFF;
        r->b048 = b;
        if (b & 6) return fy_fail(r, "b048");                           /* 0xE08093C2 / 0xE0809404 */
        r->f1 = r->ring_frame;
        r->f2 = RD(FY_RING + 8 * (r->ring_i == 2 ? 0 : r->ring_i + 1) + 4);   /* the entry before the newest */
        uint32_t d = r->f1 - r->f2;
        if (d < 1 || d > 16) return fy_fail(r, "ring_f");
        if (fy_rec_find(ctx, r->f1, blo, bhi, &r->pick1, uc) < 0) return fy_fail(r, "no_rec_new");
        int k2 = fy_rec_find(ctx, r->f2, blo, bhi, &r->pick, uc);
        if (k2 < 0) return fy_fail(r, "no_rec_done");
        if (r->pick == r->pick1) return fy_fail(r, "same_buf");
        if (g > 3 || r->pick1 != (int) ((g + 3) & 3) || r->pick != (int) ((g + 2) & 3)) return fy_fail(r, "handout");
        r->rec = k2;
        r->rec_frame = r->f2;
        r->src = 4;
    }
    else
    {
        /* ring frame -> record -> buffer */
        int best = -1, best_d = 1 << 30;
        for (int k = 0; k < 4; k++)
        {
            uint32_t rb = ctx + FY_REC + FY_REC_STRIDE * k, lo, hi;
            uint32_t f = RD(rb + 4);
            if (fy_rd64(rb + 0x10, &lo, &hi, uc) != 1) continue;
            int m = -1;
            for (int j = 0; j < 4; j++) if (lo == blo[j] && hi == bhi[j]) m = j;
            if (m < 0) continue;
            int d = (int) (r->ring_frame - f);
            if (d < 0 || d >= 16) continue;
            if (d < best_d) { best_d = d; best = k; r->pick = m; r->rec_frame = f; }
        }
        if (best >= 0) { r->rec = best; r->src = best_d == 0 ? 1 : 2; }
        else
        {
            if (g > 3) return fy_fail(r, "get_idx");
            r->pick = (g + 2) & 3;
            r->src = 3;
        }
    }
    r->lo = blo[r->pick]; r->hi = bhi[r->pick];

    /* pointer: channel 1, aligned, a static window 4..7 (uncached alias),
     * whole buffer inside the window, channel 1's RAM and the verified region */
    if (r->hi != 1) return fy_fail(r, "not_ch1");
    if (r->lo & 3) return fy_fail(r, "misaligned");
    uint32_t win[12];
    for (int i = 0; i < 12; i++) win[i] = RD(FY_MEMIF_WIN + 4 * i);
    if (!win[11]) return fy_fail(r, "win_table_not_copied");
    if (!uc) return fy_fail(r, "uncached_unverified");
    r->win = fy_win_for(win, r->lo, r->hi);
    if (r->win < 4 || r->win > 7) return fy_fail(r, "no_uncached_window");
    if ((r->lo & 0x0FFFFFFF) + R6_FY_BYTES > 0x10000000) return fy_fail(r, "window_end");
    uint32_t lim = 0;
    if (!fy_rd32(FY_CH_LIMIT, &lim, uc) || !lim || r->lo + R6_FY_BYTES < r->lo || r->lo + R6_FY_BYTES > lim)
        return fy_fail(r, "ch1_end");
    uint32_t cpu = ((uint32_t) r->win << 28) | (r->lo & 0x0FFFFFFF);
    if (cpu < 0x40000000 || cpu >= 0x80000000 || !fy_ram_ok(cpu, R6_FY_BYTES, uc)) return fy_fail(r, "range");
    r->cpu = cpu;
    r->ok = 1;
    return 1;
}

int r6_fy_locate(struct r6_fy_loc * r) { return fy_locate(r, 0); }
int r6_fy_locate_done(struct r6_fy_loc * r) { return fy_locate(r, 1); }

int r6_fy_recheck(const struct r6_fy_loc * r)
{
    if (!r->ok || r->src != 4) return -1;                               /* skeptic A5: only an ok src-4 locate */
    int uc = fy_uc_ok();
    uint32_t g = 0xFF, b = 0xFF;
    if (RD(FY_STATE + 0xC) != r->ctx || !fy_rd8(r->ctx + FY_GET_IDX, &g, uc) || !fy_rd8(r->ctx + FY_B048, &b, uc)
        || (b & 6) || g > 3)
        return -1;
    return (int) ((g - (uint32_t) r->gidx) & 3);
}

/* ---- ML's LV image ---- */
static struct r6_fy_stats st = { .last_why = "" };
static volatile uint32_t fy_last_frame = 0;
static volatile int fy_t_frame = 0, fy_frame_seen = 0;

const struct r6_fy_stats * r6_fy_stats(void) { return &st; }

int r6_fy_lv_frame(uint32_t * cpu)
{
    uint32_t v = 1;
    int len = 0, now = get_ms_clock();
    struct r6_fy_loc r;
    int res = R6FY_OK;

    st.calls++;
    if (!lv_action_raw_get(&v, &len) || len != 4 || v != 0)
    {
        res = R6FY_NO_LV;
        fy_frame_seen = 0;
    }
    else if (!r6_fy_locate(&r))
    {
        res = R6FY_LOCATE;
        st.last_why = r.why;
    }
    else
    {
        /* ring progress: a frozen ring (LV paused, Canon stopped the face
         * detection) is not a live image */
        if (!fy_frame_seen || r.ring_frame != fy_last_frame)
        {
            fy_last_frame = r.ring_frame;
            fy_t_frame = now;
            fy_frame_seen = 1;
        }
        else if (now - fy_t_frame > FY_STALE_MS || now < fy_t_frame)
            res = R6FY_STALE;
    }

    st.reasons[res]++;
    st.last = res;
    if (res != R6FY_OK) return 0;
    if (r.cpu != st.cpu) st.switches++;
    st.cpu = r.cpu;
    st.frame = r.ring_frame;
    *cpu = r.cpu;
    return 1;
}
