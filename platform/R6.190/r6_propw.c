/*
 * R6.190 M10.2 "Tor 1": the Tv write path of R6 Interval's bracket (spec m10/m102 spec_m102.md, docs/m10_plan.md
 * 2.x and 5; approved by the user 2026-10-05, effective after the clean M10.1b log 2026-10-06, user answers D1..D8
 * of 2026-10-06 and 13.1).
 *
 * The one new Canon call is _prop_request_change (0xE07C2854, stubs.S) with the constants PROP_SET_TV 0x80000005
 * and 2 bytes (Code768 = EV * 768), from exactly one call site, r6pw_post, called only by r6pw_write, only in ML's
 * shooting task r6_iv_task, never two requests in flight, at least 300 ms apart. Only in M, only multiples of 0x100
 * (1/3 stop) inside 1/8000 .. 30 s (0x2700 .. 0xF100), inside Canon's cached Tv range [p+0x116 .. p+0x114] and the
 * raw C.Fn range [p+0x112 .. p+0x110] (K36), never Bulb 0xDF00 / 0xDC00, never faster than 1/200 unless the flash
 * inputs [cc+0xA30] / [cc+0x7A8] are 0 (K37). CONFIG_PROP_REQUEST_CHANGE stays off, ML's prop_write_allow[] stays
 * empty and ML's prop_request_change() stays a "bx lr"; nothing of this file is exported (static, or _r6pw_* which
 * platform/Makefile keeps out of magiclantern.sym, K29). No other new Canon call, no MMIO, no write to Canon RAM.
 *
 * Every write runs through the 44 lock rows of spec 3.5 on a fresh read-only snapshot pair (CC, the settings sheet
 * p = [cc+0x34FC] in the uncached alias, the DataStore, Canon's M record and its copy, the slot flags, the CC
 * object globals of K38), then a 500 ms confirmation window that classifies the outcome (3.6 / 3.7: OK, DEP,
 * INVOLVED, CLAMP, FOREIGN, MODE, LANDED_OTHER resolve the request; NOCONF, SIG, FOREIGN_OPEN, MODE_OPEN keep it
 * "open", K4: Canon keeps requests without expiry, so ML then writes and presses nothing until a restart, unless a
 * later delivery can only be ML's own, LATE_APPLIED). A single read never decides anything: every Tv decision
 * needs two agreeing reads >= R6PW_STABLE_MS apart plus a handler delivery (3.6a). The user's Tv is restored after
 * every event, but only while Canon's Tv is still ML's last value (lock 44, plan 2.5); a value the user set wins.
 * The marker file ML/SETTINGS/R6TV.LCK lives from the first write of a series until M is back at the user's value
 * (3.8 / 3.9); the next boot reads it and offers "Tv back to ..." (a user-triggered write, all locks). Nothing is
 * ever written at startup.
 *
 * The handlers (PROP_SHUTTER and the three shutdown latches) run in Canon's PropMgr task and only copy (3.3).
 */

#include "dryos.h"
#include "bmp.h"
#include "menu.h"
#include "config.h"
#include "property.h"
#include "propvalues.h"
#include "fio-ml.h"
#include "tasks.h"
#include "r6_interval.h"
#include "r6_faceyuv.h"                  /* r6_fy_ram_ok(): p and the M record lie in the uncached alias (K12) */
#include "r6_propw.h"
#include <stdarg.h>

#ifdef CONFIG_R6_PROPW_TV

#ifndef CONFIG_R6_INTERVAL
#error "Tor 1: the Tv write path runs only in R6 Interval's task (CONFIG_R6_INTERVAL)"
#endif
#ifdef CONFIG_PROP_REQUEST_CHANGE
#error "Tor 1: CONFIG_PROP_REQUEST_CHANGE stays off"
#endif

/* stubs.S:169 (0xE07C2854, PropertyMgr.c cmd 6); declared only here, called only in r6pw_post */
extern void _prop_request_change(unsigned property, const void * addr, size_t len);

#define R6PW_ID            0x80000005u /* PROP_SET_TV = PROP_SHUTTER, Code768 (CC master size table: 2 bytes) */
#define R6PW_LEN           2
typedef char r6pw_len_is_2[R6PW_LEN == 2 ? 1 : -1];
typedef char r6pw_id_is_tv[R6PW_ID == 0x80000005u ? 1 : -1];
typedef char r6pw_hring_pow2[(R6PW_HRING & (R6PW_HRING - 1)) == 0 ? 1 : -1];

/* Canon RAM read here (read-only) */
#define R6PW_RAM_CC        0x10AB8     /* CameraConductor (r6_interval.h R6IV_RAM_CC) */
#define R6PW_RAM_DS        0x10868     /* Canon's DataStore (lvprobe LP_X_DS_PTR) */
#define R6PW_RAM_SUBST     0x10864     /* [0x1085C+8]: substitution flag objects 8..11 at +0x5A4 / 0x644 / 0x6E4 / 0x784 */
#define R6PW_RAM_CC_SLOT   0x1086C     /* [0x10868+4]: the slot API's CC (malloc 0x35AC, 0xE0711E7E; K38) */
#define R6PW_RAM_CC_RDY    0x10870     /* its ready flag */
#define R6PW_RAM_CC_TVRQ   0x2F800     /* the Tv request handler's CC (set 0xE0348688) */
#define R6PW_RAM_CC_LIM    0x10858     /* the Tv-limit module's CC (only writer 0xE038503C at CC init) */
#define R6PW_RAM_CC_API    0x10C28     /* CC_PropertyAPI block (malloc 0x10, 0xE06B4EEA): +8 = cc (dial apply 0xE06B70FA) */
#define R6PW_CC_SIZE       0x3510      /* cc fields up to the sheet pointers [cc+0x34FC..0x350F] (as LP_X_CC_SIZE) */
#define R6PW_P_SIZE        0xB9C       /* sheet fields up to [p+0xB98] (records are 0xC58 apart, LOG expo.sheet) */
#define R6PW_DS_SIZE       0x320       /* [0x10868] DataStore, as LP_X_DS_SIZE */
#define R6PW_MM_SIZE       0x54        /* M record / copy fields +0x40 .. +0x53 */
#define R6PW_FRESH_MS      1000        /* a consumer sample older than this does not count as the first of a pair */
#define R6PW_RECENT_MS     200         /* row 36: no other delivery newer than this */
#define R6PW_WAIT_POLL_MS  20          /* _r6pw_wait_ready */
#define R6PW_BOOT_MS       20000       /* boot check from here (R6IV_EARLY_MS) */

#define PW_RD(a) (*(volatile uint32_t *) (a))   /* only after pw_ram_ok / r6_fy_ram_ok, or for the fixed globals above */

/* ---- state ---- */
static CONFIG_INT("r6.pw.tv", pw_cfg_on, 0);   /* main switch "Tv writes", saved (answer 6b); static: not exported */

static int pw_inflight;                  /* a request is out and not resolved (3.7); while set: no write, no ML press */
static int pw_dead;                      /* writes off until reboot (NOCONF, DEP, FOREIGN_OPEN, MODE_OPEN, LATE_*, end of a
                                          * LIMITED restore); reason in S.dead_why */
static int pw_lim;                       /* LANDED_OTHER / CLAMP: only the one restore to base is admitted (row 2 LIMITED) */
static int pw_lim_why;
static uint32_t pw_t_last, pw_ncalls;
static volatile int pw_sig;              /* abort latches (3.3), never cleared */
static struct r6pw_state S = { .base = 0x7FFF, .last = 0x7FFF, .boot_rec = 0x7FFF };   /* 0x7FFF: no value (no INIT_FUNC: its
                                         * task_create_* object would be exported to modules) */
static char pw_msg[100] = "";            /* the newest notice (menu); NotifyBox keeps 98 characters */
static int pw_mk_len;                    /* length of the marker line this arm wrote */
static int pw_mk_ev = -1;                /* event whose first write checked the marker file (row 7) */
static int pw_bk_read;                   /* boot check: marker read */
static int pw_back_nolv;                 /* M10.2b review (S1): "Tv back" saw a hold of row 12 in its wait (LV lost) */

/* ---- handlers (PropMgr task: copy only) ---- */
struct r6pw_h { uint32_t t; int16_t v; uint8_t len, pad; };
static volatile struct r6pw_h pw_h[R6PW_HRING];   /* written only by the handler */
static volatile uint32_t pw_hn;          /* deliveries since boot */

/* another handler on an id ML registers anyway (r6_interval.c, lvprobe.c, src/lens.c): Canon's slave list is unchanged (K25) */
PROP_HANDLER(PROP_SHUTTER)
{
    int16_t v = 0;
    if (len >= 2) memcpy(&v, buf, 2);
    uint32_t n = pw_hn;
    volatile struct r6pw_h * e = &pw_h[n & (R6PW_HRING - 1)];
    e->t = get_ms_clock();
    e->v = v;
    e->len = MIN(len, 255u);
    __sync_synchronize();                /* DMB: the entry is visible before the count (PropMgr and r6_iv_task may run on
                                          * different cores) */
    pw_hn = n + 1;
}

/* the shutdown / abort latches (K30): one store each; a latch is never cleared (until reboot) */
PROP_HANDLER(PROP_ABORT)
{
    if (buf[0] == 1) pw_sig |= 1;
}

PROP_HANDLER(PROP_TERMINATE_SHUT_REQ)
{
    extern int ml_started;
    if (ml_started) pw_sig |= 2;         /* any value (10.17: narrowed to 0 / 3 if the log shows it without a shutdown) */
}

PROP_HANDLER(PROP_SHUTDOWN_REASON)
{
    extern int ml_started;
    if (buf[0] != 0 && ml_started) pw_sig |= 4;
}

static int r6pw_sig(void) { return ml_shutdown_requested || pw_sig; }
static int pw_task(void) { return strcmp(get_current_task_name(), "r6_iv_task") == 0; }

/* same rule as r6_interval.c iv_ram_ok: cached RAM, page 0 unmapped, 4-aligned, no wrap */
static int pw_ram_ok(uint32_t a, uint32_t n)
{
    return !(a & 3) && n && a >= 0x1000 && a + n - 1 > a && a + n - 1 < 0x40000000;
}

/* the deliveries [*from, pw_hn) into out (the newest R6PW_HRING); *from = the count read; returns 1 on overflow
 * (more than R6PW_HRING since *from, or overwritten while copying: then the range counts as foreign, 3.3) */
static int pw_hcopy(uint32_t * from, struct r6pw_h * out, int * cnt)
{
    uint32_t n = pw_hn;
    __sync_synchronize();
    uint32_t f = *from;
    int ovf = 0, c = 0;
    if (n - f > R6PW_HRING) { ovf = 1; f = n - R6PW_HRING; }
    for (uint32_t i = f; i != n; i++)
    {
        const volatile struct r6pw_h * h = &pw_h[i & (R6PW_HRING - 1)];
        out[c].t = h->t;
        out[c].v = h->v;
        out[c].len = h->len;
        c++;
    }
    __sync_synchronize();
    if (pw_hn - f > R6PW_HRING) ovf = 1;
    *from = n;
    *cnt = c;
    return ovf;
}

/* ---- snapshot (3.4): read-only, no Canon call; byte fields through their aligned word ---- */
#define PW_V_CC   0x01
#define PW_V_P    0x02
#define PW_V_DS   0x04
#define PW_V_CPY  0x08
#define PW_V_REC  0x10
#define PW_V_SUB  0x20
#define PW_V_ALL  0x3F
struct pw_g {                            /* the gating part: compared word by word between the two reads of a pair (row 41) */
    uint32_t v, cc, p, ds, cpy, rec, dsrec, ccg, sub;
    uint32_t c2c0, c2e0, c304, c31c, c5e8, c62c, dial, c860, ca84, cb74, cb78, c316c;
    uint32_t sl28, sl0f, sl2d, sl20, sl66, sl59;
    uint32_t aebbak, alobak, compbak, gis, f7a8, fa30, f728, f848, f6d0, fbe8, fb38, fbytes;
    uint32_t aeb, ae, alo, d4, d8, p100, cfnr, rng, comp, drive, hdr, msnr, iso, me, p5b8, tvav;
    uint32_t wbb_ba, wbb_gm, flash, fbkt, mecha, shtype;
    uint32_t cpy40, rec40, rec50;
    int32_t sm, smc, movie;
};
struct pw_snap { struct pw_g g; int t; uint32_t hn; int ht; int16_t hv; uint8_t hlen, hok; };

static const uint16_t pw_sub_off[4] = { 0x5A4, 0x644, 0x6E4, 0x784 };

static void pw_snap_read(struct pw_snap * s)
{
    memset(s, 0, sizeof(*s));
    s->t = get_ms_clock();
    uint32_t n = pw_hn;                  /* the handler first: CC stores before it publishes (K9) */
    __sync_synchronize();
    s->hn = n;
    if (n)
    {
        const volatile struct r6pw_h * h = &pw_h[(n - 1) & (R6PW_HRING - 1)];
        s->hv = h->v;
        s->hlen = h->len;
        s->ht = h->t;
        s->hok = 1;
    }
    struct pw_g * g = &s->g;
    g->sm = shooting_mode;
    g->smc = shooting_mode_custom;       /* C1..C3 read 7 / 0x10 / 0x11 here (src/propvalues.c, RR2-16) */
    g->movie = is_movie_mode() || lv_movie_select == LVMS_ENABLE_MOVIE;
    uint32_t cc = PW_RD(R6PW_RAM_CC);
    g->cc = cc;
    if (!pw_ram_ok(cc, R6PW_CC_SIZE)) return;
    g->v |= PW_V_CC;
    g->c2c0 = PW_RD(cc + 0x2C0);         /* movie variant of the sheets (0xE0711978) [I] */
    g->c2e0 = PW_RD(cc + 0x2E0);
    g->c304 = PW_RD(cc + 0x304);
    g->c31c = PW_RD(cc + 0x31C);
    g->c5e8 = PW_RD(cc + 0x5E8);
    g->c62c = PW_RD(cc + 0x62C);
    g->dial = PW_RD(cc + 0x89C);         /* 0x80000000 (3 = M) */
    g->c860 = PW_RD(cc + 0x860);
    g->ca84 = PW_RD(cc + 0xA84);
    g->cb74 = PW_RD(cc + 0xB74);
    g->cb78 = PW_RD(cc + 0xB78);
    g->c316c = PW_RD(cc + 0x316C);       /* >> 16: tvbak_val [cc+0x316E] */
    g->sl28 = PW_RD(cc + 0x2954);        /* slot flags [cc+0x28B4+4*slot] (0xE0712FB0) */
    g->sl0f = PW_RD(cc + 0x28F0);
    g->sl2d = PW_RD(cc + 0x2968);
    g->sl20 = PW_RD(cc + 0x2934);
    g->sl66 = PW_RD(cc + 0x2A4C);
    g->sl59 = PW_RD(cc + 0x2A18);
    /* Tor 0d (D1): backups, GIS, the flash predicate inputs (K37) */
    g->aebbak = PW_RD(cc + 0x2BFC);
    g->alobak = PW_RD(cc + 0x2C54) & 0xFFFFFF;
    g->compbak = PW_RD(cc + 0x3038) & 0xFFFF;
    g->gis = PW_RD(cc + 0x93C);
    g->f7a8 = PW_RD(cc + 0x7A8);
    g->fa30 = PW_RD(cc + 0xA30);
    g->f728 = PW_RD(cc + 0x728);
    g->f848 = PW_RD(cc + 0x848);
    g->f6d0 = PW_RD(cc + 0x6D0);
    g->fbe8 = PW_RD(cc + 0xBE8);
    g->fb38 = PW_RD(cc + 0xB38);
    g->fbytes = (PW_RD(cc + 0x794) & 0xFF) | (PW_RD(cc + 0xD8) >> 24) << 8 | (PW_RD(cc + 0xDC) & 0xFF) << 16;
    /* the CC object globals (K38, D1): bit 0 slot API, 1 Tv request handler, 2 Tv-limit module, 3 PropertyAPI, 4 ready */
    g->ccg = (PW_RD(R6PW_RAM_CC_SLOT) == cc) | (PW_RD(R6PW_RAM_CC_TVRQ) == cc) << 1 | (PW_RD(R6PW_RAM_CC_LIM) == cc) << 2
           | (PW_RD(R6PW_RAM_CC_RDY) != 0) << 4;
    uint32_t api = PW_RD(R6PW_RAM_CC_API);
    if (pw_ram_ok(api, 0xC) && PW_RD(api + 8) == cc) g->ccg |= 8;
    uint32_t p = PW_RD(cc + 0x34FC);
    g->p = p;
    if (r6_fy_ram_ok(p, R6PW_P_SIZE))
    {
        g->v |= PW_V_P;
        g->aeb = PW_RD(p + 0x000);
        g->ae = PW_RD(p + 0x008);
        g->alo = PW_RD(p + 0x0A8) & 0xFFFFFF;  /* A8 effective, A9 user, AA "off in M / B" (K17) */
        g->d4 = PW_RD(p + 0x0D4);
        g->d8 = PW_RD(p + 0x0D8);
        g->p100 = PW_RD(p + 0x100);
        g->cfnr = PW_RD(p + 0x110);          /* raw C.Fn range: lo16 fastest, hi16 slowest (K36) */
        g->rng = PW_RD(p + 0x114);           /* Canon's cached dial limits, what CC_SetTv clamps to (K13) */
        g->comp = PW_RD(p + 0x4A0) & 0xFFFF;
        g->drive = PW_RD(p + 0x4C0);
        g->hdr = PW_RD(p + 0x4EC) & 0xFF;
        g->msnr = PW_RD(p + 0x4F4);
        g->iso = PW_RD(p + 0x510);
        g->me = PW_RD(p + 0x5F4) & 0xFF;
        g->p5b8 = PW_RD(p + 0x5B8);          /* movie HDR 0x8002002E (K6) */
        g->tvav = PW_RD(p + 0x61C);          /* hi16 Tv [p+0x61E], lo16 Av */
        g->wbb_ba = PW_RD(p + 0x654);
        g->wbb_gm = PW_RD(p + 0x658);
        g->flash = PW_RD(p + 0x670);
        g->fbkt = PW_RD(p + 0x918);
        g->mecha = PW_RD(p + 0x92C);
        g->shtype = PW_RD(p + 0xB98);
    }
    uint32_t ds = PW_RD(R6PW_RAM_DS);
    g->ds = ds;
    g->cpy = PW_RD(cc + 0x34A4);         /* = ds + 4, cached (K12) */
    g->rec = PW_RD(cc + 0x34A8);         /* = [ds+0x2E8], the saved M record, uncached alias */
    if (pw_ram_ok(ds, R6PW_DS_SIZE)) { g->v |= PW_V_DS; g->dsrec = PW_RD(ds + 0x2E8); }
    if (pw_ram_ok(g->cpy, R6PW_MM_SIZE)) { g->v |= PW_V_CPY; g->cpy40 = PW_RD(g->cpy + 0x40); }
    if (r6_fy_ram_ok(g->rec, R6PW_MM_SIZE))
    {
        g->v |= PW_V_REC;
        g->rec40 = PW_RD(g->rec + 0x40);     /* lo16 Tv, hi16 Av */
        g->rec50 = PW_RD(g->rec + 0x50);
    }
    /* substitution flags 8..11 [[[0x10864]+off]+0x10]: bit i = flag set, bit 4+i = object not readable (the lvprobe
     * rule: cached RAM or the verified uncached alias) */
    uint32_t t = PW_RD(R6PW_RAM_SUBST);
    int ok = r6_fy_ram_ok(t, 0x788);
    for (int i = 0; i < 4; i++)
    {
        uint32_t a = ok ? PW_RD(t + pw_sub_off[i]) : 0;
        if (ok && r6_fy_ram_ok(a, 0x14)) g->sub |= (PW_RD(a + 0x10) != 0) << i;
        else g->sub |= 0x10 << i;
    }
    if (!(g->sub & 0xF0)) g->v |= PW_V_SUB;
}

static int16_t pw_tv(const struct pw_snap * s) { return (int16_t) (s->g.tvav >> 16); }
static int16_t pw_cpy(const struct pw_snap * s) { return (int16_t) s->g.cpy40; }
static int16_t pw_rec(const struct pw_snap * s) { return (int16_t) s->g.rec40; }
static int16_t pw_bak(const struct pw_snap * s) { return (int16_t) (s->g.c316c >> 16); }
static int pw_mrec_ok(const struct pw_snap * s) { return (s->g.v & (PW_V_CPY | PW_V_REC)) == (PW_V_CPY | PW_V_REC); }

/* all M (row 9) and not movie (row 10) */
static int pw_inm(const struct pw_snap * s)
{
    const struct pw_g * g = &s->g;
    return (g->v & (PW_V_CC | PW_V_P)) == (PW_V_CC | PW_V_P) && g->sm == SHOOTMODE_M && g->smc == SHOOTMODE_M
        && g->dial == 3 && g->ae == 3 && !g->movie && !g->c2c0;
}

/* the M views of one read agree on v: newest handler entry (len 2), [p+0x61E], copy and record */
static int pw_views(const struct pw_snap * s, int16_t v)
{
    return pw_inm(s) && s->hok && s->hlen == 2 && s->hv == v && pw_tv(s) == v && pw_mrec_ok(s) && pw_cpy(s) == v
        && pw_rec(s) == v;
}

/* slot flags: bit 0 0x28, 1 0x0F, 2 0x2D, 3 0x20, 4 0x66, 5 0x59 */
static int pw_slots(const struct pw_g * g)
{
    return (g->sl28 != 0) | (g->sl0f != 0) << 1 | (g->sl2d != 0) << 2 | (g->sl20 != 0) << 3 | (g->sl66 != 0) << 4
        | (g->sl59 != 0) << 5;
}

/* v inside the window, on the grid, inside Canon's cached range and the raw C.Fn range (rows 37 / 40, K36) */
static int pw_tv_in(int v, const struct pw_g * g)
{
    int fast = (int16_t) g->rng, slow = (int16_t) (g->rng >> 16);
    int cf_fast = (int16_t) g->cfnr, cf_slow = (int16_t) (g->cfnr >> 16);
    return !(v & (R6PW_GRID - 1)) && v >= R6PW_TV_MIN && v <= R6PW_TV_MAX && v <= fast && v >= slow
        && v <= cf_fast && v >= cf_slow;
}

/* ---- dependent set (3.6): must be equal before and after a write ---- */
#define PW_NDEP 34
static const uint8_t pw_dep_bit[PW_NDEP] = { 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 5, 5, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 8,
                                             9, 9, 9, 9, 9, 9, 10, 10, 10 };
static void pw_dep_vals(const struct pw_g * g, uint32_t * w)
{
    w[0] = g->sl59; w[1] = g->c316c >> 16;                       /* 0 slot 0x59 and tvbak_val */
    w[2] = g->aeb; w[3] = g->sl0f;                               /* 1 AEB word and slot 0x0F */
    w[4] = g->comp; w[5] = g->sl2d;                              /* 2 comp and slot 0x2D */
    w[6] = g->alo; w[7] = g->sl20;                               /* 3 ALO and slot 0x20 */
    w[8] = g->flash; w[9] = g->sl66;                             /* 4 flash and slot 0x66 */
    w[10] = g->d4; w[11] = g->d8; w[12] = g->p100; w[13] = g->sl28;   /* 5 step block and slot 0x28 */
    w[14] = g->tvav & 0xFFFF; w[15] = g->rec40 >> 16;            /* 6 Av, rec_av */
    w[16] = g->iso; w[17] = g->ae; w[18] = g->dial; w[19] = g->drive;  /* 7 */
    w[20] = g->c62c; w[21] = g->me; w[22] = g->sub; w[23] = g->cb78; w[24] = g->rec50 & 0xFFFF;   /* 8 */
    w[25] = g->hdr; w[26] = g->wbb_ba; w[27] = g->wbb_gm; w[28] = g->fbkt; w[29] = g->msnr; w[30] = g->shtype;   /* 9 */
    w[31] = g->aebbak; w[32] = g->alobak; w[33] = g->compbak;    /* 10 Tor 0d backups (D1) */
}

static uint32_t pw_dep_cmp(const uint32_t * d0, const struct pw_g * g)
{
    uint32_t w[PW_NDEP], m = 0;
    pw_dep_vals(g, w);
    for (int i = 0; i < PW_NDEP; i++) if (w[i] != d0[i]) m |= 1u << pw_dep_bit[i];
    return m;
}

/* ---- the ring consumer (3.3): the only reader outside a write window ---- */
struct pw_ms {                           /* one mode sample */
    int t;
    int16_t hv, tv, cpy, rec, bak;
    uint8_t ok, inm, ae, dial, hlen, mrec;
};
static struct pw_snap pw_cb;             /* the consumer's read (r6_iv_task) */
static struct pw_ms pw_cs[2];            /* [0] previous, [1] current sample */
static uint32_t pw_cn;                   /* next delivery the consumer folds */
static int16_t pw_w_pv;                  /* open-request watch: the previous delivery since the window end, its AE, count */
static uint8_t pw_w_pae;
static int pw_w_n;

static void pw_ms_set(struct pw_ms * m, const struct pw_snap * s)
{
    memset(m, 0, sizeof(*m));
    m->t = s->t;
    m->ok = (s->g.v & (PW_V_CC | PW_V_P)) == (PW_V_CC | PW_V_P);
    m->inm = pw_inm(s);
    m->ae = s->g.ae;
    m->dial = s->g.dial;
    m->hlen = s->hok ? s->hlen : 0;
    m->hv = s->hv;
    m->tv = pw_tv(s);
    m->mrec = pw_mrec_ok(s);
    m->cpy = pw_cpy(s);
    m->rec = pw_rec(s);
    m->bak = pw_bak(s);
}

static int pw_gap_ok(void)
{
    int d = pw_cs[1].t - pw_cs[0].t;
    return pw_cs[0].ok && pw_cs[1].ok && d >= R6PW_STABLE_MS && d <= R6PW_FRESH_MS;
}

/* 3.6a on the two newest samples: stable-M, stable-mode(a), M record */
static int pw_stable_m(int16_t * v)
{
    const struct pw_ms * a = &pw_cs[0], * b = &pw_cs[1];
    int16_t x = b->hv;
    if (!pw_gap_ok() || !a->inm || !b->inm || a->hlen != 2 || b->hlen != 2 || !a->mrec || !b->mrec) return 0;
    if (a->hv != x || a->tv != x || b->tv != x || a->cpy != x || b->cpy != x || a->rec != x || b->rec != x) return 0;
    *v = x;
    return 1;
}

static int pw_stable_mode(int ae, int16_t * v)
{
    const struct pw_ms * a = &pw_cs[0], * b = &pw_cs[1];
    int16_t x = b->hv;
    if (!pw_gap_ok() || a->ae != ae || b->ae != ae || a->dial != b->dial || a->hlen != 2 || b->hlen != 2) return 0;
    if (a->hv != x || a->tv != x || b->tv != x) return 0;
    *v = x;
    return 1;
}

static int pw_mrec(int16_t * v)
{
    const struct pw_ms * a = &pw_cs[0], * b = &pw_cs[1];
    int16_t x = b->rec;
    if (!pw_gap_ok() || !a->mrec || !b->mrec || a->rec != x || a->cpy != x || b->cpy != x) return 0;
    *v = x;
    return 1;
}

static int pw_ae_other(int ae) { return ae == 1 || ae == 4 || ae == 0x37; }   /* Tv, B, Fv: CC_SetTv stores there (K34) */

void _r6pw_consume(int now)
{
    if (!pw_task()) return;
    pw_snap_read(&pw_cb);
    pw_cs[0] = pw_cs[1];
    pw_ms_set(&pw_cs[1], &pw_cb);
    const struct pw_ms * a = &pw_cs[0], * b = &pw_cs[1];
    struct r6pw_h hb[R6PW_HRING];
    int nb = 0;
    uint32_t from = pw_cn;
    if (pw_hcopy(&from, hb, &nb)) S.ovf = 1;            /* only between two consecutive consumer calls */
    pw_cn = from;
    int inm2 = a->inm && b->inm;
    for (int i = 0; i < nb; i++)
    {
        const struct r6pw_h * d = &hb[i];
        if (d->len != 2) { if (S.watch) S.w_other = 1; continue; }   /* unknown value: never the user's, never ML's */
        /* fgn_m: an in-M delivery of another value; a republish of the current value or a mode burst at a transition
         * does not count (RV-4, RR2-2) */
        if (inm2 && d->v != S.last && d->v != a->rec) S.fgn_m = 1;
        S.u_last = d->v;
        S.u_t = d->t;
        S.u_ae = b->ae;
        if (S.watch)
        {
            if (d->v == S.open_want)
            {
                S.w_want = 1;
                /* late LANDED_OTHER: the want delivery directly follows a delivery of a different value in that mode */
                S.w_land = pw_w_n == 1 && !b->inm && pw_w_pae == b->ae && pw_w_pv != d->v
                    && ABS(d->v - pw_w_pv) > R6PW_GRID;
            }
            else if (inm2 && d->v != S.close_rec) S.w_other = 1;
            pw_w_pv = d->v;
            pw_w_pae = b->ae;
            pw_w_n++;
        }
    }
    if (b->mrec && (b->rec != S.last || b->cpy != S.last)) S.rec_moved = 1;   /* only a store in AE 3 moves it (K34) */
    if (S.watch && b->mrec && b->rec != S.close_rec && b->rec != S.open_want) S.w_recv = 1;
    int16_t v;
    if (S.landed_ae && !S.landed_ok && pw_stable_mode(S.landed_ae, &v) && v != S.landed_v) S.landed_ok = 1;
    if (S.landed_ae2 && S.landed_ae2 != 0xFF && !S.landed_ok2 && pw_stable_mode(S.landed_ae2, &v) && v != S.landed_v2)
        S.landed_ok2 = 1;                                /* the older record (review round 2, S2-1) */
    (void) now;
}

/* two consumer samples >= R6PW_STABLE_MS apart, the newer one fresh. A sleep of R6PW_STABLE_MS can end a little short of
 * it on the ms clock (tick alignment; QEMU m102 found it: the second of two back-to-back lock evaluations then saw no
 * stable pair and refused VIEWS), so the gap is checked again, at most 3 sleeps (read only, no Canon call) */
static void pw_fresh2(void)
{
    _r6pw_consume(get_ms_clock());
    for (int i = 0; i < 3; i++)
    {
        int d = pw_cs[1].t - pw_cs[0].t;
        if (d >= R6PW_STABLE_MS && d <= R6PW_FRESH_MS) return;
        msleep(R6PW_STABLE_MS);
        _r6pw_consume(get_ms_clock());
    }
}

/* user evidence for u (3.6a): stable-M u that is not ML's expected value and not pw.last, a handler delivery of u after
 * t_ev, no open request, no overflow. Only M's own Tv counts (review round 1, F2): every decision that uses it is about
 * M (lock 44, the pending restore, the offer, the M part of the marker rule); a value in the mode a landed value went to
 * says nothing about M and only resolves the landed part (landed_ok, in the consumer) */
static int pw_user_ev(int16_t expect, int16_t * u)
{
    int16_t v;
    if (pw_inflight || S.ovf || !pw_stable_m(&v)) return 0;
    if (v == expect || v == S.last || S.u_last != v || (int) (S.u_t - S.t_ev) <= 0) return 0;
    *u = v;
    return 1;
}

/* the M record is the base in two consumer samples, in any mode (3.6a "M record": only a store in AE 3 moves it, K34),
 * no open request: M holds the base, nothing to restore there (review round 1, L4) */
static int pw_mrec_base(void)
{
    int16_t r;
    return !pw_inflight && pw_mrec(&r) && r == S.base;
}

int _r6pw_tv_stable(int16_t * v)
{
    if (!pw_task()) return 0;
    if (!pw_gap_ok() || get_ms_clock() - pw_cs[1].t > R6PW_FRESH_MS) pw_fresh2();
    if (pw_stable_m(v)) return 1;
    if (pw_cs[0].inm && pw_cs[1].inm) S.uns++;   /* in M, but the views disagree: unsettled (3.6a) */
    return 0;
}

int _r6pw_user_evidence(int16_t expect, int16_t * u)
{
    if (!pw_task()) return 0;
    if (!pw_gap_ok() || get_ms_clock() - pw_cs[1].t > R6PW_FRESH_MS) pw_fresh2();   /* a fresh pair, as _r6pw_tv_stable */
    return pw_user_ev(expect, u);
}

/* ---- notices ---- */
static const char * const pw_tvn[55] = {           /* Tv thirds -15 (30 s) .. 39 (1/8000) */
    "30s", "25s", "20s", "15s", "13s", "10s", "8s", "6s", "5s", "4s", "3.2s", "2.5s", "2s", "1.6s", "1.3s", "1s",
    "0.8s", "0.6s", "0.5s", "0.4s", "0.3s", "1/4", "1/5", "1/6", "1/8", "1/10", "1/13", "1/15", "1/20", "1/25",
    "1/30", "1/40", "1/50", "1/60", "1/80", "1/100", "1/125", "1/160", "1/200", "1/250", "1/320", "1/400", "1/500",
    "1/640", "1/800", "1/1000", "1/1250", "1/1600", "1/2000", "1/2500", "1/3200", "1/4000", "1/5000", "1/6400", "1/8000",
};

char * _r6pw_tvs(char * buf, int v)
{
    int k = ((int16_t) v >> 8) + 15;
    if (!(v & 0xFF) && k >= 0 && k < (int) COUNT(pw_tvn)) snprintf(buf, 12, "%s", pw_tvn[k]);
    else snprintf(buf, 12, "0x%04x", v & 0xFFFF);
    return buf;
}

static const char * pw_ae_name(int ae)
{
    return ae == 1 ? "Tv" : ae == 4 ? "B" : ae == 0x37 ? "Fv" : ae == 3 ? "M" : ae == 2 ? "Av" : ae == 0 ? "P" : "?";
}

/* the notice for the menu and, outside the ML menu, as NotifyBox (<= 98 characters) */
static void pw_note(int box, const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(pw_msg, sizeof(pw_msg), fmt, ap);
    va_end(ap);
    if (box) NotifyBox(5000, "%s", pw_msg);
}

const char * _r6pw_msg(void) { return pw_msg; }

/* ---- the write ring (3.12) ---- */
static struct r6pw_rec pw_ring[R6PW_RING];
static uint32_t pw_rn;                   /* entries written since boot */
static uint32_t pw_open_rn;              /* the entry of the open request */

static struct r6pw_rec * pw_rec_new(int want, int kind, int k, int ev)
{
    struct r6pw_rec * e = &pw_ring[pw_rn % R6PW_RING];
    memset(e, 0, sizeof(*e));
    e->t = get_ms_clock();
    e->d_ret = e->d_h = e->d_cc = e->d_mm = e->d_conf = 0xFFFF;
    e->want = want;
    e->old = e->rep = e->cc = e->mm = e->ds4 = e->bak = 0x7FFF;
    e->kind = kind;
    e->k = k;
    e->ev = ev;
    pw_rn++;
    return e;
}

/* an attempt without a call (REFUSED, DONE, TOOK_OVER) */
static int pw_rec_end(struct r6pw_rec * e, int res, int lock)
{
    e->res = res;
    e->lock = lock;
    S.last_res = res;
    S.last_lock = lock;
    return res;
}

static int pw_refused(int want, int kind, int lock)
{
    return pw_rec_end(pw_rec_new(want, kind, 0xFF, S.ev_no), R6PW_R_REFUSED, lock);
}

static uint16_t pw_dms(int t, int t0) { int d = t - t0; return (uint16_t) COERCE(d, 0, 0xFFFE); }

int _r6pw_ring(int i, struct r6pw_rec * e)
{
    uint32_t n = pw_rn;
    int cnt = n < R6PW_RING ? (int) n : R6PW_RING;
    if (i >= 0 && i < cnt) *e = pw_ring[(n - cnt + i) % R6PW_RING];
    return cnt;
}

uint32_t _r6pw_ring_n(void) { return pw_rn; }

/* ---- marker file ML/SETTINGS/R6TV.LCK (3.9); card access only between pictures, never while Canon's job runs ---- */
static void pw_mk_name(char * n, int sz) { snprintf(n, sz, "%sR6TV.LCK", get_config_dir()); }

/* create, write, close, confirm the size; 0 = ok */
static int __attribute__((noinline)) pw_mk_write(const char * line, int len)
{
    char n[64];
    pw_mk_name(n, sizeof(n));
    FILE * f = FIO_CreateFile(n);
    if (!f) return -1;
    int w = FIO_WriteFile(f, line, len);
    FIO_CloseFile(f);
    uint32_t sz = 0;
    if (w != len || FIO_GetFileSize(n, &sz) || sz != (uint32_t) len) return -1;
    return 0;
}

/* the file size (-1 = no file); with buf: also its first max bytes */
static int __attribute__((noinline)) pw_mk_read(char * buf, int max)
{
    char n[64];
    pw_mk_name(n, sizeof(n));
    uint32_t sz = 0;
    if (FIO_GetFileSize(n, &sz)) return -1;
    if (buf && max > 0)
    {
        int r = read_file(n, buf, MIN((int) sz, max));
        if (r < 0) return -1;
        buf[MIN(r, max)] = 0;
    }
    return (int) sz;
}

static void __attribute__((noinline)) pw_mk_delete(void)
{
    char n[64];
    pw_mk_name(n, sizeof(n));
    FIO_RemoveFile(n);
}

/* "R6TV 2 base=0x.... ctx=. t=. ae=. v=0x....[ ae2=. v2=0x....]\n" (<= 72 bytes); land = 0: ae / v 0 (the arm of a
 * series), else the landed records of S (review round 2, S2-1: the older one as ae2 / v2) */
static int pw_mk_line(char * l, int sz, int base, int ctx, int land)
{
    int n = snprintf(l, sz, "R6TV 2 base=0x%04x ctx=%d t=%d ae=%d v=0x%04x", base & 0xFFFF, ctx, get_ms_clock(),
        land ? S.landed_ae : 0, land ? S.landed_v & 0xFFFF : 0);
    if (land && S.landed_ae2 && n > 0 && n < sz)
        n += snprintf(l + n, sz - n, " ae2=%d v2=0x%04x", S.landed_ae2, S.landed_v2 & 0xFFFF);
    if (n > 0 && n < sz) n += snprintf(l + n, sz - n, "\n");
    return COERCE(n, 0, sz - 1);
}

static int pw_card_ok(void);

/* LANDED_OTHER of any kind (STEP, the restores, BACK, the late watch) leaves this set until the marker line holds its
 * mode and value (review round 1, F3 / L5) */
static int pw_mk_land_due;

/* the landed records still open: 1 = the newest, 2 = the older one (review round 2, S2-1) */
static int pw_land_open1(void) { return S.landed_ae && !S.landed_ok; }
static int pw_land_open2(void) { return S.landed_ae2 && !S.landed_ok2; }

/* a LANDED_OTHER (ae, v) (review round 2, S2-1): one record per mode, the newest first. An unresolved record of another
 * mode is kept as the older one, never overwritten; a value in a recorded mode replaces that record (the mode now
 * holds the newer value). Two records cover every path of one session (after a LANDED_OTHER pw_lim admits one more
 * write, the restore to base, then pw_dead) and "Tv back" is refused while both are open (_r6pw_arm); should a third
 * mode still come, the older record becomes 0xFF ("Tv, B and Fv"), which only the user's own fix ends (marker kept) */
static void pw_land_rec(int ae, int16_t v)
{
    int keep1 = pw_land_open1() && S.landed_ae != ae, keep2 = pw_land_open2() && S.landed_ae2 != ae;
    if (keep1 && keep2) { S.landed_ae2 = 0xFF; S.landed_v2 = 0; S.landed_ok2 = 0; }
    else if (keep1) { S.landed_ae2 = S.landed_ae; S.landed_v2 = S.landed_v; S.landed_ok2 = 0; }
    else if (!keep2) { S.landed_ae2 = 0; S.landed_v2 = 0; S.landed_ok2 = 0; }
    S.landed_ae = ae;
    S.landed_v = v;
    S.landed_ok = 0;
    pw_mk_land_due = 1;                  /* the marker line gets both records (3.9), whatever the kind (review round 1, F3) */
}

/* LANDED_OTHER: the kept marker gets the mode and value (3.9; between pictures, Canon's job 0; base kept). Card not
 * free: _r6pw_tick tries again. FIO_CreateFile removes the old file first, so a failed write can leave no line or a
 * short one: one retry, then a notice, and pw_mk_len gets a length no file has, so a pending restore ends at once with
 * the offer (pw_pend_due, review round 2, L2-4) instead of silently (review round 1, L5) */
static void pw_mk_landed(void)
{
    char l[96];
    if (!pw_mk_land_due) return;
    if (S.marker < 1 || !S.landed_ae) { pw_mk_land_due = 0; return; }
    if (!pw_card_ok()) return;
    pw_mk_land_due = 0;
    int n = pw_mk_line(l, sizeof(l), S.base, S.armed == R6PW_CTX_BACK ? R6PW_CTX_BACK : R6PW_CTX_BKT, 1);
    if (!pw_mk_write(l, n) || !pw_mk_write(l, n)) { pw_mk_len = n; return; }
    pw_mk_len = -2;
    pw_note(1, "R6TV.LCK lost: check Tv in M\nand in mode %s.", pw_ae_name(S.landed_ae));
}

/* the value after key (hex after "0x", else decimal); 0 = not found */
static int pw_kv(const char * s, const char * key, int * out)
{
    const char * p = strstr(s, key);
    if (!p) return 0;
    p += strlen(key);
    int hex = p[0] == '0' && p[1] == 'x', v = 0, n = 0;
    if (hex) p += 2;
    for (;; p++, n++)
    {
        int c = *p, d = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0 || n > 8) break;
        v = v * (hex ? 16 : 10) + d;
    }
    *out = v;
    return n > 0;
}

/* a mode a landed record may name: Tv, B, Fv (K34), 0 = none; 0xFF only as the older record (review round 2, S2-1) */
static int pw_ae_rec(int ae, int older) { return ae == 0 || ae == 1 || ae == 4 || ae == 0x37 || (older && ae == 0xFF); }

/* "R6TV 1|2 base=0x.... [ae=. v=0x....[ ae2=. v2=0x....]]": 1 = parsed (" ae=" / " v=" never match " ae2=" / " v2=") */
static int pw_mk_parse(const char * l, int * base, int * ae, int * v, int * ae2, int * v2)
{
    if (strncmp(l, "R6TV ", 5) || (l[5] != '1' && l[5] != '2') || l[6] != ' ') return 0;
    if (!pw_kv(l, " base=", base)) return 0;
    *base = (int16_t) *base;
    *ae = *v = *ae2 = *v2 = 0;
    if (l[5] == '2' && (!pw_kv(l, " ae=", ae) || !pw_kv(l, " v=", v))) return 0;
    if (l[5] == '2' && pw_kv(l, " ae2=", ae2) && !pw_kv(l, " v2=", v2)) return 0;
    if (!pw_ae_rec(*ae, 0) || !pw_ae_rec(*ae2, 1) || (*ae2 && !*ae)) return 0;
    *v = (int16_t) *v;
    *v2 = (int16_t) *v2;
    return 1;
}

static int pw_fileno(void)
{
    struct card_info * c = get_shooting_card();
    return c ? c->file_number : 0;
}

/* card access only between pictures and while Canon's job count is 0 (3.9) */
static int pw_card_ok(void)
{
    int t_menu = 0, t_lv = 0, in_pic = 0;
    _r6iv_times(&t_menu, &t_lv, &in_pic);
    return !in_pic && PW_RD(R6IV_RAM_JOBCNT) == 0;
}

/* ---- locks (3.5) ---- */
struct pw_ev {
    int want, kind, cls, nolast;         /* nolast: row 44 is left to r6pw_write (the wait before a restore) */
    const struct pw_snap * a, * b;       /* the pair (HARD: a == b) */
    const struct r6_iv_cc * ca, * cb;
    int stab; int16_t sv;                /* stable-M from the consumer (3.6a) */
    int16_t old, u;                      /* out: the agreed Tv; user value for row 44 */
    int sticky, evid;                    /* out: row 44 failed sticky / with user evidence */
};

static int pw_restore_kind(int k) { return k >= R6PW_K_RESTORE && k <= R6PW_K_BACK; }

/* row 5: the armed context admits the kind (bit tests, no switch: canon_gate tbb noise) */
static int pw_ctx_admits(int k, int want)
{
    if (S.armed == R6PW_CTX_BKT)
        return ((1 << k) & ((1 << R6PW_K_STEP) | (1 << R6PW_K_RESTORE) | (1 << R6PW_K_RESTORE_STOP) | (1 << R6PW_K_CLAMPFIX))) != 0;
    if (S.armed == R6PW_CTX_BACK) return k == R6PW_K_BACK;
    if (S.armed == R6PW_CTX_PEND) return k == R6PW_K_RESTORE_PEND && want == S.base;
    return 0;
}

/* row 12: Canon GUI idle, ML menu closed >= R6PW_HOLD_MS, LV running >= R6PW_HOLD_MS, no ML picture. M10.2b: the two
 * holds from _r6iv_holds (r6_interval.c), the helper "Tv back to ..." waits on as well */
static int pw_gui_bad(int now)
{
    int t_menu = 0, t_lv = -1, in_pic = 0;
    _r6iv_times(&t_menu, &t_lv, &in_pic);
    return CURRENT_GUI_MODE != 0 || gui_state != GUISTATE_IDLE || _r6iv_holds(now) || in_pic || (icu_uilock & 0xFF)
        || sensor_cleaning;
}

/* row 36: a handler delivery newer than R6PW_RECENT_MS that differs from v (or is not 2 bytes) */
static int pw_recent_other(int16_t v, int now)
{
    uint32_t n = pw_hn;
    __sync_synchronize();
    for (int i = 1; i <= R6PW_HRING && (uint32_t) i <= n; i++)
    {
        const volatile struct r6pw_h * h = &pw_h[(n - i) & (R6PW_HRING - 1)];
        if (now - (int) h->t >= R6PW_RECENT_MS) break;
        if (h->len != 2 || h->v != v) return 1;
    }
    return 0;
}

/* row 44 (restore kinds, BACK): 0 = holds */
static int pw_last_bad(struct pw_ev * e, int16_t old)
{
    if (e->kind == R6PW_K_BACK) return !S.offer || old != S.boot_rec;
    int16_t l = S.last;
    if (old == l && pw_cpy(e->b) == l && pw_rec(e->b) == l && !S.fgn_m && !S.rec_moved && !S.ovf) return 0;
    e->sticky = S.fgn_m || S.rec_moved || S.ovf;
    e->evid = pw_user_ev(l, &e->u);
    return 1;
}

/* rows of 3.5 in their order (1..37, 42, 43, 44, 38..41); 0 = all hold, else the first failing row */
static int pw_rows(struct pw_ev * e)
{
    const struct pw_g * g = &e->a->g, * h = &e->b->g;
    const struct r6_iv_cc * c = e->ca;
    int all = e->cls == R6PW_CLASS_ALL, k = e->kind, w = e->want, wr = k != R6PW_K_NONE;
    int newv = w != R6PW_WANT_NONE;
    int now = get_ms_clock();
    /* 1 */
    if (!pw_cfg_on) return R6PW_L_OFF;
    /* 2 */
    if (pw_dead) return R6PW_L_DEAD;
    if (pw_lim && !((k == R6PW_K_CLAMPFIX || k == R6PW_K_RESTORE_PEND) && w == S.base)) return R6PW_L_LIMITED;
    if (pw_inflight) return R6PW_L_OPEN;
    if (S.pend && k != R6PW_K_RESTORE_PEND) return R6PW_L_PEND;
    if (S.offer && k != R6PW_K_BACK) return R6PW_L_OFFER;
    if (!S.boot_done) return R6PW_L_BOOT;
    if (!wr && !S.armed && (S.marker == 2 || S.marker == 3)) return R6PW_L_MKEPT;
    /* 3 */
    if (r6pw_sig()) return R6PW_L_SIG;
    /* 4, 5, 6, 7 (a write only; row 7's card check runs in r6pw_write once every row holds) */
    if (wr && !pw_task()) return R6PW_L_TASK;
    if (wr && !pw_ctx_admits(k, w)) return R6PW_L_ARMED;
    if (wr && all && pw_ncalls && (now - (int) pw_t_last < R6PW_SPACING_MS || now < (int) pw_t_last)) return R6PW_L_SPACING;
    if (wr && (S.armed == R6PW_CTX_BKT ? S.marker != 1 : S.marker < 1)) return R6PW_L_MARKER;
    /* 8 */
    if (_r6iv_cc_gate(c) != 0 || !(c->valid & R6IV_V_P) || c->cc != g->cc || (g->v & PW_V_ALL) != PW_V_ALL
        || g->cpy != g->ds + 4 || g->rec != g->dsrec || g->ccg != 0x1F) return R6PW_L_PTR;
    /* 9, 10 */
    if (g->sm != SHOOTMODE_M || g->smc != SHOOTMODE_M || g->dial != 3 || g->ae != 3) return R6PW_L_MODE;
    if (g->movie || g->c2c0) return R6PW_L_MOVIE;
    if (all)
    {
        /* 11: LV still table, LV kind 1, shoot type 3, dispatch 1 (_r6iv_lvstill), state 1 / 3, [cc+0xA84] 0, event flag */
        if (!_r6iv_lvstill(c) || (c->state != 1 && c->state != 3) || g->ca84 || !c->evflag || (c->evflag & 1))
            return R6PW_L_CCSTATE;
        /* 12 */
        if (pw_gui_bad(now)) return R6PW_L_GUI;
        /* 13: bytes, SW events, self-release macro, MPU switch status */
        if (c->df || c->e0 || c->snap_df || c->snap_e0 || ((c->pend | c->work) & R6IV_SW_EVENT_MASK) || c->relcb
            || !(c->valid & R6IV_V_STS) || c->sts[0] || c->sts[1]) return R6PW_L_PRESS;
        /* 14 */
        if (c->jobs || c->jdone != c->jid || c->relseq || c->b54 || c->faneo[0] != R6IV_RAM_FANEO
            || c->faneo[1] != R6IV_RAM_FANEO || burst_count == 0) return R6PW_L_JOB;
    }
    /* 15 .. 35 */
    if (!g->iso) return R6PW_L_ISO;
    int seen = 0, aebh = r6_iv_aeb(&seen);
    if ((seen && (int16_t) aebh) || (int16_t) g->aeb || (g->aeb & 0xFFFFFF)) return R6PW_L_AEB;
    if (g->wbb_ba || g->wbb_gm) return R6PW_L_WBB;
    if (g->hdr) return R6PW_L_HDR;
    if (g->me) return R6PW_L_ME;
    if (g->fbkt) return R6PW_L_FBKT;
    if (g->msnr == 4) return R6PW_L_MSNR;
    if (g->shtype == 3) return R6PW_L_ESHUT;
    if (c->p_itimer || c->p_fbrk || c->d30 || c->ba0 || c->ta || c->tb || c->sec || c->b98 || c->c830 || g->drive
        || drive_mode != DRIVE_SINGLE) return R6PW_L_TIMER;
    if (g->d4 != 2 || g->d8 != 2) return R6PW_L_STEP;
    if (g->sub) return R6PW_L_SUBST;
    if (g->c860 != 1) return R6PW_L_INIT;
    if (g->c62c > 1) return R6PW_L_C62C;
    if (g->c5e8 || g->c2e0 || g->c304 || g->c31c) return R6PW_L_CTX;
    if (g->cb78 || g->cb74 || g->p5b8) return R6PW_L_B78;
    if (g->sl28) return R6PW_L_SLOT28;
    if (g->sl59 != 1) return R6PW_L_SLOT59;
    if (g->sl0f != 1) return R6PW_L_SLOT0F;
    if (g->sl2d != 1 || (int16_t) g->comp) return R6PW_L_SLOT2D;
    int a8 = g->alo & 0xFF, a9 = (g->alo >> 8) & 0xFF, aa = (g->alo >> 16) & 0xFF;
    int f1 = g->sl20 == 1 && a8 == 3 && aa != 0, f0 = g->sl20 == 0 && aa == 0 && a8 == a9;
    if (!f1 && !f0) return R6PW_L_SLOT20;
    if (g->sl66 || g->flash) return R6PW_L_SLOT66;
    /* 36: stable-M (wait class); the one read of the HARD class only gives old for row 37 */
    int16_t old = pw_tv(e->a);
    if (all)
    {
        if (!e->stab || !pw_views(e->a, e->sv) || !pw_views(e->b, e->sv) || pw_recent_other(e->sv, now))
        {
            S.uns++;
            return R6PW_L_VIEWS;
        }
        old = e->sv;
    }
    e->old = old;
    /* 37 */
    if (!pw_tv_in(old, g)) return R6PW_L_OLD;
    /* 42 */
    if (g->mecha) return R6PW_L_MECHA;
    /* 43: not faster than 1/200 unless the flash inputs show no flash in both reads (K37, D1) */
    if (newv && w > R6PW_XSYNC_CAP && !(g->fa30 == 0 && g->f7a8 == 0 && h->fa30 == 0 && h->f7a8 == 0)) return R6PW_L_FLASH;
    /* 44 */
    if (wr && pw_restore_kind(k) && !e->nolast && pw_last_bad(e, old)) return R6PW_L_LAST;
    /* 44 for STEP (review round 1, F1 / L1; plan 2.5 "your dial turn wins"): Canon's Tv must still be ML's last value
     * (S.last: the base at the event start, the previous member after its write; also in the wait before the write).
     * With user evidence the user's value wins (LAST: TOOK_OVER, no call, the series stops TVUSER, nothing restored);
     * without it the views are unsettled (wait class; a mismatch without evidence ends as a VIEWS stop, 3.6a) */
    if (wr && k == R6PW_K_STEP && old != S.last)
    {
        e->evid = pw_user_ev(S.last, &e->u);
        if (e->evid) return R6PW_L_LAST;
        S.uns++;
        return R6PW_L_VIEWS;
    }
    /* 38, 39 (D1) */
    if ((g->aebbak & ~0x00FF0000u) != g->aeb) return R6PW_L_BKUP_AEB;
    if (g->sl20 == 1)
    {
        int b0 = g->alobak & 0xFF, b1 = (g->alobak >> 8) & 0xFF, b2 = (g->alobak >> 16) & 0xFF;
        if (b1 != a9 || b2 != aa || b0 != b1) return R6PW_L_BKUP_ALO;
    }
    /* 40 */
    if (newv && (w == old || !pw_tv_in(w, g))) return R6PW_L_NEW;
    /* 41 (wait class) */
    if (all && (memcmp(&e->a->g, &e->b->g, sizeof(struct pw_g)) || !_r6iv_cc_same(e->ca, e->cb))) return R6PW_L_TORN;
    return 0;
}

static int pw_wait_row(int l)
{
    return l == R6PW_L_SPACING || l == R6PW_L_CCSTATE || l == R6PW_L_GUI || l == R6PW_L_PRESS || l == R6PW_L_JOB
        || l == R6PW_L_VIEWS || l == R6PW_L_TORN;
}

/* the task's pair (locks, window); r6_iv_task only */
static struct pw_snap pw_s1, pw_s2;
static struct r6_iv_cc pw_c1, pw_c2;

/* every row on fresh reads (ALL): two consumer samples for stable-M, then a pair */
static int pw_eval_all(struct pw_ev * e)
{
    pw_fresh2();
    e->cls = R6PW_CLASS_ALL;
    e->stab = pw_stable_m(&e->sv);
    pw_snap_read(&pw_s1);
    r6_iv_cc_read(&pw_c1);
    pw_snap_read(&pw_s2);
    r6_iv_cc_read(&pw_c2);
    e->a = &pw_s1; e->b = &pw_s2; e->ca = &pw_c1; e->cb = &pw_c2;
    e->old = 0x7FFF;
    return pw_rows(e);
}

int _r6pw_locks(int want, int cls, int * lock)
{
    struct pw_ev e;
    memset(&e, 0, sizeof(e));
    e.want = want;
    int l;
    if (cls == R6PW_CLASS_ALL && pw_task()) l = pw_eval_all(&e);
    else
    {
        struct pw_snap s;                /* one read on the caller's stack (menu task, log task) */
        struct r6_iv_cc c;
        pw_snap_read(&s);
        r6_iv_cc_read(&c);
        e.cls = R6PW_CLASS_HARD;
        e.a = e.b = &s;
        e.ca = e.cb = &c;
        l = pw_rows(&e);
    }
    if (lock) *lock = l;
    return l;
}

int _r6pw_wait_ready(int want, int kind)
{
    if (!pw_task()) return R6PW_L_TASK;
    int t0 = get_ms_clock(), l = 0, evid = 0;
    int16_t u = 0;
    for (;;)
    {
        int now = get_ms_clock();
        _r6iv_sample(now);
        /* M10.2b review (S1, user decision 2026-10-06: no live view, then dropped with a notice, no late write): both holds
         * of row 12 were 0 when iv_back_tick called _r6pw_back, so for the BACK any hold now means Canon's LV stopped or
         * restarted (Canon MENU, playback, screen off) or the ML-menu hold came back during the arm's marker I/O: the wait
         * ends at once, before any ring entry or call, and _r6pw_back drops the command (it does not wait for LV again) */
        if (kind == R6PW_K_BACK && _r6iv_holds(now)) { pw_back_nolv = 1; return R6PW_L_GUI; }
        struct pw_ev e;
        memset(&e, 0, sizeof(e));
        e.want = want; e.kind = kind; e.nolast = 1;   /* row 44 of the restore kinds: in r6pw_write (TOOK_OVER / offer /
                                                        * pending); STEP's row 44 runs here too (review round 1, F1) */
        l = pw_eval_all(&e);
        evid = l == R6PW_L_LAST && e.evid;
        u = e.u;
        /* a restore whose value M already holds stably (row 40: new == old) is no lock: r6pw_write says DONE (no call)
         * instead of a refusal and a pending restore (QEMU m102: the event stop's DONE check ran on an unsettled pair) */
        if (l == R6PW_L_NEW && pw_restore_kind(kind) && e.old == want) { l = 0; break; }
        if (!l || !pw_wait_row(l)) break;
        now = get_ms_clock();
        if (now - t0 >= R6IV_BKT_WAIT_MS || now < t0) break;   /* the wait row is the reason */
        msleep(R6PW_WAIT_POLL_MS);
    }
    if (evid)                            /* STEP, lock 44 with user evidence: the user's value wins, TOOK_OVER, no call */
    {
        struct r6pw_rec * r = pw_rec_new(want, kind, 0xFF, S.ev_no);
        r->fl |= R6PW_FL_USER;
        r->rep = u;
        pw_rec_end(r, R6PW_R_TOOK_OVER, l);
        return l;
    }
    if (l) pw_refused(want, kind, l);
    return l;
}

int _r6pw_ranges(struct r6pw_rng * r)
{
    struct pw_snap s, s2;
    memset(r, 0, sizeof(*r));
    pw_snap_read(&s);
    pw_snap_read(&s2);
    if ((s.g.v & (PW_V_CC | PW_V_P)) != (PW_V_CC | PW_V_P)) return 0;
    r->fast = (int16_t) s.g.rng;
    r->slow = (int16_t) (s.g.rng >> 16);
    r->cf_fast = (int16_t) s.g.cfnr;
    r->cf_slow = (int16_t) (s.g.cfnr >> 16);
    r->tv = pw_tv(&s);
    r->noflash = s.g.fa30 == 0 && s.g.f7a8 == 0 && s2.g.fa30 == 0 && s2.g.f7a8 == 0;
    r->ok = 1;
    return 1;
}

/* ---- the single call site (gates 7) ---- */

/* the ONLY caller of _prop_request_change in the camera build; static (not exported, K29); noinline + noipa: no
 * clone (.isra / .constprop), so the gates see exactly one symbol; called only by r6pw_write. 0 = posted, else the
 * lock that refused it */
static int __attribute__((noinline, noipa)) r6pw_post(int want)
{
    if (strcmp(get_current_task_name(), "r6_iv_task") != 0) return R6PW_L_TASK;   /* never PropMgr / GUI / a module */
    if (pw_inflight) return R6PW_L_OPEN;                                            /* one in flight (K3) */
    if (pw_dead) return R6PW_L_DEAD;
    if (r6pw_sig()) return R6PW_L_SIG;
    int now = get_ms_clock();
    if (pw_ncalls && (now - (int) pw_t_last < R6PW_SPACING_MS || now < (int) pw_t_last)) return R6PW_L_SPACING;
    if ((want & (R6PW_GRID - 1)) || want < R6PW_TV_MIN || want > R6PW_TV_MAX) return R6PW_L_NEW;   /* defence in depth */
    int16_t v = (int16_t) want;          /* copied by Canon at call time (K1) */
    pw_inflight = 1;
    pw_t_last = now;
    pw_ncalls++;
    _prop_request_change(R6PW_ID, &v, R6PW_LEN);                                    /* return value ignored (K1) */
    return 0;
}

/* ---- one write (3.6) ---- */
struct pw_win {
    uint32_t hc;                         /* next delivery to read */
    int seen, t_h, nfor, ndel, uok, t_u, sig, ovf, mode_left, movie_left;
    int16_t u, fval, rep, lastv;
    int lastlen, rep_t;
    int oth, oth_ae, land;               /* a non-want delivery outside M (the mode's burst) and its AE; want after it */
    uint32_t dep;
};

static void pw_sticky_reset(int now, int t_ev)
{
    S.t_res = now;
    S.t_ev = t_ev;
    S.fgn_m = S.rec_moved = S.ovf = 0;
}

/* a restore that is not needed (M already holds the base): no call. ML's value is gone from M, so last = base and the
 * sticky summary starts again, as a resolution would (review round 1, L7; also what lock 44 for STEP expects at the next
 * event start) */
static int pw_done(struct r6pw_rec * e)
{
    int now = get_ms_clock();
    pw_rec_end(e, R6PW_R_DONE, 0);
    S.last = S.base;
    pw_sticky_reset(now, now);
    return R6PW_R_DONE;
}

static void pw_lim_end(void)
{
    if (!pw_lim) return;
    pw_lim = 0;
    pw_dead = 1;
    S.dead_why = pw_lim_why;
}

/* classification done: resolve or keep open, the consequences of 3.7, the notice */
static int pw_resolve(struct r6pw_rec * e, int res, int want, const struct pw_win * w, int ae)
{
    int now = get_ms_clock();
    char a[12], b[12];
    e->res = res;
    S.last_res = res;
    S.last_lock = 0;
    pw_cn = w->hc;                       /* the window's deliveries are handed over: the consumer starts after them */
    if (w->rep_t) { S.u_last = w->rep; S.u_t = w->rep_t; S.u_ae = ae; }   /* its newest delivery counts (RR2-8) */
    if (res == R6PW_R_SIG || res == R6PW_R_NOCONF || res == R6PW_R_FOREIGN_OPEN || res == R6PW_R_MODE_OPEN)
    {
        /* open: Canon may still apply it (K4); the watch starts at the window end */
        e->fl |= R6PW_FL_OPEN;
        S.open_want = want;
        S.open_t = e->t;
        S.close_v = w->rep;
        S.close_rec = pw_rec(&pw_s1);
        S.watch = 1;
        S.w_other = S.w_recv = S.w_want = S.w_land = 0;
        S.ovf = 0;
        pw_w_n = 0;
        if (w->lastlen == 2 && !pw_inm(&pw_s1)) { pw_w_pv = w->lastv; pw_w_pae = ae; pw_w_n = 1; }
        pw_open_rn = pw_rn - 1;
        if (res != R6PW_R_SIG) { pw_dead = 1; S.dead_why = res; }
        if (res == R6PW_R_NOCONF)
            pw_note(1, "ML's Tv %s may still apply.\nNo ML pictures until restart.\nRestart, check Tv (yours %s).",
                _r6pw_tvs(a, want), _r6pw_tvs(b, S.base));
        else if (res == R6PW_R_FOREIGN_OPEN)
            pw_note(1, "ML's Tv %s may still apply over your %s.\nNo ML pictures until restart.\nRestart, check Tv.",
                _r6pw_tvs(a, want), _r6pw_tvs(b, w->u));
        else if (res == R6PW_R_MODE_OPEN)
            pw_note(1, "Mode left in Tv write: ML's %s may\napply in M/Tv/B/Fv. No ML shots till\nrestart (yours %s)",
                _r6pw_tvs(a, want), _r6pw_tvs(b, S.base));
        return res;
    }
    pw_inflight = 0;
    pw_sticky_reset(now, res == R6PW_R_FOREIGN ? w->t_h : now);
    if (res == R6PW_R_CLAMP) S.last = w->u;
    else if (res != R6PW_R_LANDED) S.last = want;
    if (res == R6PW_R_DEP)
    {
        pw_dead = 1;
        S.dead_why = res;
        pw_note(1, "Stopped: Canon changed 0x%x (comp/AEB/ALO/\nflash...). Tv is %s, yours was %s.",
            (int) w->dep, _r6pw_tvs(a, want), _r6pw_tvs(b, S.base));
    }
    else if (res == R6PW_R_CLAMP)
    {
        pw_lim = 1;
        pw_lim_why = res;
        pw_note(1, "Stopped: Canon changed ML's Tv to %s.", _r6pw_tvs(a, w->u));
    }
    else if (res == R6PW_R_LANDED)
    {
        pw_lim = 1;
        pw_lim_why = res;
        pw_land_rec(ae, want);           /* the marker line gets ae / v (3.9), an older open record kept (round 2, S2-1) */
        if (ae == 4) pw_note(1, "ML's Tv %s landed in mode B. Set it there.\nTurn the dial off B and back; check\nthe bulb timer.",
            _r6pw_tvs(a, want));
        else pw_note(1, "ML's Tv %s landed in mode %s.\nSet it there by hand.", _r6pw_tvs(a, want), pw_ae_name(ae));
    }
    else if (res == R6PW_R_FOREIGN) pw_note(1, "Stopped: Tv changed by user (%s).", _r6pw_tvs(a, w->u));
    else if (res == R6PW_R_INVOLVED)
        pw_note(1, "Tv %s: your dial turn and ML's write\ncrossed. Tv back to %s.", _r6pw_tvs(a, want), _r6pw_tvs(b, S.base));
    else if (res == R6PW_R_MODE && want != S.base)
        pw_note(1, "Tv %s set by ML stays in M; it returns\nto %s when you are back in M.", _r6pw_tvs(a, want), _r6pw_tvs(b, S.base));
    return res;
}

/* one write: locks, the call, the window, the classification (3.6); r6_iv_task only (r6pw_post checks it again).
 * Its only callers: _r6pw_write (STEP, RESTORE), _r6pw_event_stop (RESTORE_STOP, CLAMPFIX), _r6pw_tick
 * (RESTORE_PEND), _r6pw_back (BACK); each runs it at most once per invocation (no loop, no retry); all but the per-tick
 * RESTORE_PEND first wait in _r6pw_wait_ready (M10.2b: BACK too) */
static int __attribute__((noinline, noipa)) r6pw_write(int want, int kind, int k, int ev)
{
    struct r6pw_rec * e = pw_rec_new(want, kind, k, ev);
    int16_t v;
    /* 1. a restore that is not needed: M's Tv is stably the value already (RR2-12, RR2-15); want == base for every
     * restore kind */
    if (pw_restore_kind(kind) && !pw_inflight)
    {
        pw_fresh2();
        if (pw_stable_m(&v) && v == want) return pw_done(e);
    }
    /* row 7, the card part, before the lock pair (review round 1, safety L1: nothing but the step-3 Tv check between
     * the lock pair and the post): the marker file at the first write of every event / before a pending restore;
     * pw_card_ok: no picture, Canon's job count 0 (3.9). The state part of row 7 stays in row order */
    int mk = (S.armed == R6PW_CTX_BKT && pw_mk_ev != ev) || S.armed == R6PW_CTX_PEND;
    if (mk && (!pw_card_ok() || pw_mk_read(0, 0) != pw_mk_len)) return pw_rec_end(e, R6PW_R_REFUSED, R6PW_L_MARKER);
    /* 2. n0 before the first lock read (RR2-13), then every row on fresh reads */
    uint32_t n0 = pw_hn;
    __sync_synchronize();
    struct pw_ev q;
    memset(&q, 0, sizeof(q));
    q.want = want;
    q.kind = kind;
    int lock = pw_eval_all(&q);
    e->old = q.old;
    e->state = pw_c1.state;
    e->sl0 = pw_slots(&pw_s1.g);
    e->c62c = MIN(pw_s1.g.c62c, 255u);
    if (!lock && mk && S.armed == R6PW_CTX_BKT) pw_mk_ev = ev;   /* this event's file check is done */
    if (lock == R6PW_L_LAST && q.evid)
    {
        e->fl |= R6PW_FL_USER;
        e->rep = q.u;
        return pw_rec_end(e, R6PW_R_TOOK_OVER, lock);
    }
    if (lock == R6PW_L_LAST && q.sticky) e->fl |= R6PW_FL_STICKY;
    if (lock) return pw_rec_end(e, R6PW_R_REFUSED, lock);
    int16_t old = q.old;
    uint32_t d0[PW_NDEP];
    pw_dep_vals(&pw_s1.g, d0);
    /* 3. nothing but a republish of old since n0 (a click between the lock reads and the post: refused); the gap
     * left between this check and CC's processing is the residual race of 10.9 */
    struct r6pw_h hb[R6PW_HRING];
    int nb = 0;
    uint32_t hc = n0;
    int bad = pw_hcopy(&hc, hb, &nb);
    for (int i = 0; i < nb; i++) if (hb[i].len != 2 || hb[i].v != old) bad = 1;
    if (bad) return pw_rec_end(e, R6PW_R_REFUSED, R6PW_L_VIEWS);
    int t_call = get_ms_clock();
    int pr = r6pw_post(want);
    if (pr) return pw_rec_end(e, R6PW_R_REFUSED, pr);
    int t_ret = get_ms_clock();
    e->t = t_call;
    e->d_ret = pw_dms(t_ret, t_call);
    S.calls++;
    /* 4. the window: every R6PW_POLL_MS up to R6PW_CONFIRM_MS */
    struct pw_win w;
    memset(&w, 0, sizeof(w));
    w.hc = hc;
    w.rep = old;
    int conf = 0, sfor = 0, mrec = 0, smode = 0, prev_c = 0, prev_t = t_call, mlfirst = 0;
    int ae = 3, t_mr = 0, t_lm = 0, t_fu = 0, lm_ae = -1, lm_dial = -1;   /* start (| 1, 0 = none) of the mm_ok / lm / fu runs */
    int16_t fu_v = 0;
    uint32_t dprev = 0;
    for (;;)
    {
        msleep(R6PW_POLL_MS);
        int now = get_ms_clock();
        if (r6pw_sig()) { w.sig = 1; break; }
        int ov = pw_hcopy(&w.hc, hb, &nb);   /* the deliveries since the last poll */
        pw_snap_read(&pw_s1);                /* then the mode / value sample after them */
        pw_snap_read(&pw_s2);
        const struct pw_g * g = &pw_s1.g;
        int16_t tv1 = pw_tv(&pw_s1), tv2 = pw_tv(&pw_s2);
        ae = g->ae;
        int inm = pw_inm(&pw_s1) && pw_inm(&pw_s2);
        if (ov) { w.nfor++; w.ovf = 1; }
        for (int i = 0; i < nb; i++)
        {
            const struct r6pw_h * d = &hb[i];
            w.ndel++;
            if (d->len == 2 && d->v == want)
            {
                if (!w.seen) { w.seen = 1; w.t_h = d->t; e->d_h = pw_dms(d->t, t_call); }
                if (!inm && w.oth && w.oth_ae == ae) w.land = ae;     /* after another value in this mode (its burst) */
            }
            else if (d->len == 2 && d->v == old && !w.seen)
            {
                if (!inm) { w.oth = 1; w.oth_ae = ae; w.land = 0; }   /* in M a republish of the current value (K15) */
            }
            else
            {
                w.nfor++;
                if (!w.fval) w.fval = d->v ? d->v : 0x7FFF;
                if (d->len == 2) { w.u = d->v; w.t_u = d->t; w.uok = 1; }
                if (!inm && d->len == 2) { w.oth = 1; w.oth_ae = ae; w.land = 0; }
            }
            if (d->len == 2) { w.rep = d->v; w.rep_t = d->t; }
            w.lastv = d->v;
            w.lastlen = d->len;
        }
        int cc_ok = tv1 == want && tv2 == want;
        int mm_ok = pw_mrec_ok(&pw_s1) && pw_mrec_ok(&pw_s2) && pw_cpy(&pw_s1) == want && pw_rec(&pw_s1) == want
            && pw_cpy(&pw_s2) == want && pw_rec(&pw_s2) == want;
        if (cc_ok && e->d_cc == 0xFFFF) e->d_cc = pw_dms(now, t_call);
        if (mm_ok && e->d_mm == 0xFFFF) e->d_mm = pw_dms(now, t_call);
        if (!inm) w.mode_left = 1;
        if (g->movie || g->c2c0 || pw_s2.g.movie || pw_s2.g.c2c0) w.movie_left = 1;   /* the movie position of the dial */
        int far = now - prev_t >= R6PW_STABLE_MS;
        /* confirmed: in two consecutive polls >= R6PW_STABLE_MS apart (K11) */
        int cnow = w.seen && cc_ok && mm_ok && inm;
        uint32_t dm = cnow ? pw_dep_cmp(d0, &pw_s1.g) | pw_dep_cmp(d0, &pw_s2.g) : 0;
        if (cnow && prev_c && far && !conf) { conf = 1; e->d_conf = pw_dms(now, t_call); w.dep = dm | dprev; }
        /* M record == want (MODE, any mode, 3.6a) and stable-mode(ae) want (LANDED_OTHER): a run of polls that all
         * agree, its first and the current one >= R6PW_STABLE_MS apart; not just the last two polls, because msleep
         * can end a poll < R6PW_STABLE_MS after the previous one and the window's last poll decides (QEMU m102) */
        if (!mm_ok) t_mr = 0;
        else if (!t_mr) t_mr = now | 1;
        mrec = t_mr && now - t_mr >= R6PW_STABLE_MS;
        int lm = pw_s1.hok && pw_s1.hlen == 2 && pw_s1.hv == want && cc_ok && pw_s2.hlen == 2 && pw_s2.hv == want
            && g->ae == pw_s2.g.ae && g->dial == pw_s2.g.dial;
        if (!lm) t_lm = 0;
        else if (!t_lm || lm_ae != (int) g->ae || lm_dial != (int) g->dial) { t_lm = now | 1; lm_ae = g->ae; lm_dial = g->dial; }
        smode = t_lm && now - t_lm >= R6PW_STABLE_MS;
        /* stable foreign: the newest delivery is u (len 2) and [p+0x61E] == u (RR2-14), a run of polls for the same u
         * >= R6PW_STABLE_MS like mrec / smode. Review round 1 (L3): not a latch, so ML's value delivered after u ends it
         * and the window goes on to its confirmation (INVOLVED, restore through lock 44) */
        int fu = w.uok && w.lastlen == 2 && w.lastv == w.u && tv1 == w.u && tv2 == w.u;
        if (!fu) t_fu = 0;
        else if (!t_fu || fu_v != w.u) { t_fu = now | 1; fu_v = w.u; }
        sfor = t_fu && now - t_fu >= R6PW_STABLE_MS;
        mlfirst = w.seen && w.uok && w.t_u - w.t_h >= 0;   /* ML's value delivered before u's last delivery (3.6 5.5) */
        prev_c = cnow; dprev = dm; prev_t = now;
        if (conf && !w.mode_left) break;
        if (sfor && mlfirst && !w.mode_left) break;
        if (now - t_call >= R6PW_CONFIRM_MS || now < t_call) break;   /* after mode_left: to the end (evidence for 5.2) */
    }
    /* 5. classification, mode first (RV-4) */
    mlfirst = w.seen && w.uok && w.t_u - w.t_h >= 0;
    int res;
    if (w.sig) res = R6PW_R_SIG;
    else if (w.mode_left)
    {
        if (mrec) res = R6PW_R_MODE;                                    /* ML's value is in M's memory (K34) */
        else if (!w.movie_left && pw_ae_other(ae) && smode && w.land == ae) res = R6PW_R_LANDED;
        else res = R6PW_R_MODE_OPEN;            /* also every movie case (13.1: the movie position, never fixed) */
    }
    else if (conf && w.nfor) res = R6PW_R_INVOLVED;
    else if (conf) res = w.dep ? R6PW_R_DEP : R6PW_R_OK;
    else if (sfor)
    {
        int fast = (int16_t) pw_s1.g.rng, slow = (int16_t) (pw_s1.g.rng >> 16);
        int cl = want > fast ? fast : want < slow ? slow : want;
        if (w.u == cl && w.u != want) res = R6PW_R_CLAMP;
        else if (mlfirst) res = R6PW_R_FOREIGN;
        else res = R6PW_R_FOREIGN_OPEN;  /* ML's value not delivered, or not before u's last delivery (3.6 step 5.5) */
    }
    else res = R6PW_R_NOCONF;
    /* 6. the entry */
    e->rep = w.rep;
    e->cc = pw_tv(&pw_s1);
    e->mm = pw_rec(&pw_s1);
    e->ds4 = pw_cpy(&pw_s1);
    e->bak = pw_bak(&pw_s1);
    e->sl1 = pw_slots(&pw_s1.g);
    e->ae = ae;
    e->ndel = MIN(w.ndel, 255);
    e->nfor = MIN(w.nfor, 255);
    e->fval = w.fval;
    e->dep = w.dep;
    e->fl |= (w.mode_left ? R6PW_FL_MODE : 0) | (w.ovf ? R6PW_FL_OVF : 0) | (w.movie_left ? R6PW_FL_MOVIE : 0)
           | (mlfirst ? R6PW_FL_MLFIRST : 0) | (res == R6PW_R_FOREIGN ? R6PW_FL_USER : 0);
    return pw_resolve(e, res, want, &w, ae);
}

/* ---- states and restore paths (3.8) ---- */

static void pw_pend_start(int why)
{
    char a[12], b[12], c[12];
    S.pend = 1;
    S.pend_why = why;
    S.pend_t0 = get_ms_clock();
    S.pend_cnt = total_shots_count;
    S.pend_fn = pw_fileno();
    if (pw_mk_len < 0) return;                    /* the marker line was lost: its notice stays, the next tick ends the
                                                   * pending with the offer (review round 2, L2-4) */
    if (S.last_res == R6PW_R_LANDED)
        pw_note(1, "ML's %s landed in %s: set it there.\nM has ML's %s; back to your %s\nin M (2 min).",
            _r6pw_tvs(a, S.landed_v), pw_ae_name(S.landed_ae), _r6pw_tvs(b, S.last), _r6pw_tvs(c, S.base));
    else if (S.last_res == R6PW_R_MODE) return;   /* pw_resolve's notice: "... returns to %s when you are back in M" */
    else if (S.last_res == R6PW_R_REFUSED && S.last_lock)
        pw_note(1, "Tv restore waits: %s.\nTv %s -> %s when back in LV (2 min).",
            _r6pw_lock_name(S.last_lock), _r6pw_tvs(a, S.last), _r6pw_tvs(b, S.base));
    else pw_note(1, "R6 Interval: Tv %s returns to %s\nwhen you are back in LV (2 min).", _r6pw_tvs(a, S.last), _r6pw_tvs(b, S.base));
}

/* the "Tv back" offer: rec_tv now (a fresh read) */
static void pw_offer(void)
{
    char a[12];
    _r6pw_consume(get_ms_clock());
    S.offer = 1;
    S.boot_rec = pw_cs[1].mrec ? pw_cs[1].rec : S.last;
    pw_note(1, "Tv not restored. Shoot > R6 Interval >\nTv back to %s, or turn the dial.", _r6pw_tvs(a, S.base));
}

static void pw_rule(void);

static void pw_disarm_int(void)
{
    S.armed = 0;
    S.ev_on = 0;
    if (S.marker == 1) S.marker = 2;      /* kept until the marker rule holds (3.8) */
}

void _r6pw_disarm(int clean)
{
    pw_disarm_int();
    if (pw_task()) { pw_fresh2(); pw_rule(); }
    (void) clean;
}

static void pw_pend_end(void)
{
    S.pend = 0;
    if (S.armed == R6PW_CTX_PEND) pw_disarm_int();
    pw_lim_end();
}

/* after an immediate restore attempt (event end, event stop): pending, offer or done (3.7 / 3.8) */
static void pw_after_restore(int r, int kind, int why)
{
    char a[12];
    if (r == R6PW_R_REFUSED)
    {
        const struct r6pw_rec * e = &pw_ring[(pw_rn - 1) % R6PW_RING];
        if (e->fl & R6PW_FL_STICKY) { pw_offer(); pw_lim_end(); }   /* lock 44 can never hold: no 2-min wait */
        else pw_pend_start(why);         /* also a refused CLAMPFIX (D3); pw_lim ends with the pending */
        return;
    }
    if (r == R6PW_R_LANDED) pw_mk_landed();                         /* the marker records the mode and value (3.9, F3) */
    if (r == R6PW_R_LANDED && S.last != S.base) { pw_pend_start(why); return; }   /* D3 (a): ML's member value in M */
    if (r == R6PW_R_TOOK_OVER) pw_note(1, "Tv %s is yours; ML did not restore it.", _r6pw_tvs(a, pw_ring[(pw_rn - 1) % R6PW_RING].rep));
    if (r == R6PW_R_OK) pw_note(0, "Tv back to %s.", _r6pw_tvs(a, S.base));
    S.pend = 0;
    pw_lim_end();
    (void) kind;
}

/* an event is over without its restore: M already base (DONE), or the restore decision of 3.7 / 3.8 */
static int pw_done_now(int kind)
{
    int16_t v;
    if (pw_inflight) return 0;
    pw_fresh2();
    /* stable-M base, or the M record base in any mode (review round 1, L4: e.g. M -> Av right after ML's base) */
    if ((pw_stable_m(&v) && v == S.base) || pw_mrec_base()) { pw_done(pw_rec_new(S.base, kind, 0xFF, S.ev_no)); return 1; }
    return 0;
}

int __attribute__((noinline)) _r6pw_write(int want, int kind, int k, int ev)
{
    if (!pw_task()) return R6PW_R_REFUSED;
    if (kind == R6PW_K_STEP) return r6pw_write(want, R6PW_K_STEP, k, ev);
    if (kind != R6PW_K_RESTORE || want != S.base) return pw_refused(want, kind, R6PW_L_ARMED);
    /* the event's restore (4.4 step 4): wait rows, then the one call; the event is over whatever the result */
    S.busy = 1;
    int r = R6PW_R_DONE;
    if (!pw_done_now(R6PW_K_RESTORE))
    {
        int l = _r6pw_wait_ready(want, R6PW_K_RESTORE);
        r = l ? R6PW_R_REFUSED : r6pw_write(want, R6PW_K_RESTORE, 0xFF, ev);
    }
    S.ev_on = 0;
    pw_after_restore(r, kind, R6IV_STOP_TVW);
    S.busy = 0;
    return r;
}

void __attribute__((noinline)) _r6pw_event_stop(int why)
{
    if (!S.ev_on) return;
    S.ev_on = 0;                         /* first: a second call (iv_end twice) does nothing (RR2-12) */
    if (!pw_task() || S.armed != R6PW_CTX_BKT) return;
    int r = S.last_res;
    if (pw_inflight || r6pw_sig()) return;                       /* open: no restore, no pending (3.7); a signal: none */
    if (pw_dead || r == R6PW_R_DEP || r == R6PW_R_FOREIGN) return;   /* D5 / the user's value wins */
    if (r == R6PW_R_TOOK_OVER)                                   /* a member write found the user's value (lock 44 for STEP,
                                                                  * review round 1, F1): it wins, nothing is restored */
    {
        char a[12];
        pw_note(1, "Tv %s is yours; ML did not restore it.", _r6pw_tvs(a, pw_ring[(pw_rn - 1) % R6PW_RING].rep));
        return;
    }
    if (pw_lim && r == R6PW_R_LANDED)
    {
        pw_mk_landed();                                          /* the marker records the mode and value (3.9) */
        if (S.last != S.base) pw_pend_start(why);                /* D3 (a): restore M when the user is back in M */
        else pw_lim_end();                                       /* M is the base: nothing to restore */
        return;
    }
    if (r == R6PW_R_MODE)                                        /* restore when back in M (lock 44); a MODE of ML's base (the
                                                                  * '0' member of '- 0 +') left nothing to restore (review
                                                                  * round 1, L4) */
    {
        if (S.last != S.base) pw_pend_start(why);
        else { S.pend = 0; pw_lim_end(); }
        return;
    }
    int kind = pw_lim ? R6PW_K_CLAMPFIX : R6PW_K_RESTORE_STOP;
    S.busy = 1;
    if (pw_done_now(kind)) { S.pend = 0; pw_lim_end(); S.busy = 0; return; }
    int l = _r6pw_wait_ready(S.base, kind);
    r = l ? R6PW_R_REFUSED : r6pw_write(S.base, kind, 0xFF, S.ev_no);
    pw_after_restore(r, kind, why);
    S.busy = 0;
}

void _r6pw_series_end(int stop)
{
    if (S.armed != R6PW_CTX_BKT || S.se_done) return;
    S.se_done = 1;
    _r6pw_event_stop(stop);
    if (S.pend) S.armed = R6PW_CTX_PEND;   /* only RESTORE_PEND to base (row 5), the marker stays (row 7) */
    else _r6pw_disarm(1);
}

void _r6pw_begin(int base)
{
    if (S.armed) return;
    S.base = base;
    S.last = base;
    S.last_res = R6PW_R_NONE;
    S.last_lock = 0;
}

void _r6pw_ev_begin(int no)
{
    S.ev_on = 1;
    S.ev_no = no;
    S.last_res = R6PW_R_NONE;
    S.last_lock = 0;
}

int _r6pw_ev_on(void) { return S.ev_on || S.busy; }

int _r6pw_arm(int ctx, int base)
{
    char l[96], buf[96];
    int t_in = get_ms_clock();               /* before the marker file I/O (review round 2, L2-3) */
    if (!pw_task()) return R6PW_L_TASK;
    if (!S.boot_done) return R6PW_L_BOOT;
    if (pw_dead) return R6PW_L_DEAD;
    if (pw_inflight) return R6PW_L_OPEN;
    if (S.armed) return R6PW_L_ARMED;
    if (r6pw_sig()) return R6PW_L_SIG;
    if (!pw_card_ok()) return R6PW_L_JOB;
    if (ctx == R6PW_CTX_BKT)
    {
        if (pw_lim) return R6PW_L_LIMITED;
        if (S.pend) return R6PW_L_PEND;
        if (S.offer) return R6PW_L_OFFER;
        if (S.marker == 2 || S.marker == 3) return R6PW_L_MKEPT;
        if (!_r6iv_bkt_armable()) return R6PW_L_ARMED;
        if (pw_mk_read(0, 0) >= 0) return R6PW_L_MKEPT;   /* a marker file this arm did not write (the boot check reads it) */
        int n = pw_mk_line(l, sizeof(l), base, ctx, 0);
        /* a failed write can leave the created file (empty or short, e.g. a full card): removed, since no file existed
         * before (checked just above), so the next Start writes a new one instead of finding MARKER_KEPT (review round
         * 2, L2-5) */
        if (pw_mk_write(l, n)) { pw_mk_delete(); S.marker = -1; return R6PW_L_MARKER; }
        S.marker = 1;
        S.mk_boot = 1;
        pw_mk_len = n;
        pw_mk_ev = -1;
        S.base = base;
        S.last = base;
        S.landed_ae = S.landed_ae2 = 0;
        S.landed_v = S.landed_v2 = 0;
        S.landed_ok = S.landed_ok2 = 0;
    }
    else if (ctx == R6PW_CTX_BACK)
    {
        if (!S.offer || r6_iv_running()) return R6PW_L_ARMED;      /* the only arm that requires an offer (4.9) */
        /* review round 2 (S2-1): ML's Tv may be set in two other modes: a third one could not be recorded, so the user
         * sets those modes first (one of them resolved admits "Tv back" again) */
        if (pw_land_open1() && pw_land_open2()) return R6PW_L_MKEPT;
        int b = 0, ae = 0, v = 0, ae2 = 0, v2 = 0, n = pw_mk_read(buf, sizeof(buf) - 1);
        if (n < 0 || !pw_mk_parse(buf, &b, &ae, &v, &ae2, &v2) || b != S.base)
        {
            n = pw_mk_line(l, sizeof(l), S.base, ctx, 1);   /* missing: re-written (with the landed records) and confirmed */
            if (pw_mk_write(l, n)) return R6PW_L_MARKER;
            S.mk_boot = 1;
        }
        if (S.marker < 1) S.marker = 2;
    }
    else return R6PW_L_ARMED;
    S.armed = ctx;
    S.se_done = 0;
    /* evidence reference: the consumer sample the caller's decision rested on (this tick's, fresh), else the time before
     * the marker file I/O; a dial click delivered during that I/O is the user's (no request open, ML wrote nothing since)
     * and counts as evidence (review round 2, L2-3). Minus 1 ms: a click in the same millisecond as that sample counts
     * too (QEMU m102 F34d: ms clock). A delivery that sample already folded is no ML value either (no request open, no
     * ML write since its last resolution); for a series it was the base (the decision needed a stable base), and the
     * base is never evidence there (the expected value and pw.last are the base until the first resolution) */
    int t_ref = pw_cs[1].t;
    if (t_in - t_ref > R6PW_FRESH_MS || t_ref - t_in > 0) t_ref = t_in;
    pw_sticky_reset(get_ms_clock(), t_ref - 1);
    return 0;
}

int __attribute__((noinline)) _r6pw_back(void)
{
    if (!pw_task()) return R6PW_R_REFUSED;
    int r;
    if (!S.offer) { r = pw_refused(S.base, R6PW_K_BACK, R6PW_L_LAST); S.back_res = r; S.back_lock = R6PW_L_LAST; return r; }
    int l = _r6pw_arm(R6PW_CTX_BACK, S.base);
    if (l) r = pw_refused(S.base, R6PW_K_BACK, l);
    else
    {
        S.busy = 1;                      /* the ML menu waits for the wait and the window (review round 1, safety L5) */
        /* M10.2b (change B, camera M10.2 LVP00102): the wait rows first (row 12's holds, CC state, a press, Canon's job, an
         * unsettled Tv), bounded like the event-end and event-stop restores (_r6pw_wait_ready: no call, <= R6IV_BKT_WAIT_MS;
         * row 44 stays in r6pw_write); a hard row or the timeout refuses with that row (its ring entry), no call.
         * M10.2b review (S1): live view lost in the wait (pw_back_nolv): dropped, R6PW_R_NONE (no call, no ring entry) */
        pw_back_nolv = 0;
        l = _r6pw_wait_ready(S.base, R6PW_K_BACK);
        r = pw_back_nolv ? R6PW_R_NONE : l ? R6PW_R_REFUSED : r6pw_write(S.base, R6PW_K_BACK, 0xFF, 0);
        pw_back_nolv = 0;
    }
    S.back_res = r;                          /* NONE (dropped): 0, no stale "refused" menu line */
    S.back_lock = r ? S.last_lock : 0;
    if (r == R6PW_R_OK || r == R6PW_R_DONE)
    {
        char a[12];
        S.offer = 0;
        pw_note(1, "Tv back: ok (%s).", _r6pw_tvs(a, S.base));
    }
    else if (r == R6PW_R_REFUSED && pw_wait_row(S.back_lock))
    {
        /* M10.2b: the bounded wait ran out on a wait row (transient: GUI / ML menu / LV, CC state, a press, Canon's job, an
         * unsettled Tv, spacing, a torn read), so far only visible in the menu line: the notice outside the ML menu. A hard
         * row keeps the menu line only (a setting the user sees, or the user's own Tv: the offer is gone) */
        char a[12], t[48];
        _r6pw_lock_text(S.back_lock, t, sizeof(t));
        pw_note(1, "Tv back refused: %s.\nSet %s by dial or select it again.", t, _r6pw_tvs(a, S.base));
    }
    else if (r == R6PW_R_NONE) _r6pw_back_dropped(1);   /* M10.2b review (S1): the drop notice; iv_back_tick the menu line */
    if (r == R6PW_R_LANDED) pw_mk_landed();   /* the marker records the mode and value (3.9, review round 1 F3) */
    /* LANDED_OTHER / CLAMP set pw_lim, but ctx BACK admits nothing after it (no pending, no CLAMPFIX): the limited
     * state ends here, writes off until restart (3.7; review round 2, L2-2) */
    if (r == R6PW_R_LANDED || r == R6PW_R_CLAMP) pw_lim_end();
    S.busy = 0;
    _r6pw_disarm(1);                     /* every attempt (RR2-10): the offer watcher and the marker rule run again */
    return r;
}

/* M10.2b: command 5 dropped before its write (r6_interval.c iv_back_tick, user decision 2026-10-06): the notice outside the
 * ML menu; no call, nothing armed, the offer and the marker stay. why 1: no live view once the ML menu hold was over (or
 * gone during the LV hold, or during change B's wait in _r6pw_back: M10.2b review S1); 2: live view not steady within
 * R6IV_BACK_MAX_MS, or the clock went back */
void _r6pw_back_dropped(int why)
{
    char a[12];
    if (!pw_task()) return;
    if (why == 1) pw_note(1, "Tv back dropped: no live view.\nIn live view select R6 Interval >\nTv back to %s again.", _r6pw_tvs(a, S.base));
    else pw_note(1, "Tv back dropped: live view not steady.\nSelect R6 Interval > Tv back to %s\nagain.", _r6pw_tvs(a, S.base));
}

/* ---- the tick (3.10 order: consumer, boot check, open-request watch, pending restore, offer, marker rule) ---- */

/* the boot check's landed part for one record (3.10): 0 resolved (that mode shows another value), 1 "still ML's", 2 "may
 * still be set" (not readable from here; 0xFF: the modes Tv, B and Fv) */
static int pw_boot_land(int ae, int16_t lv)
{
    const struct pw_ms * c = &pw_cs[1];
    int16_t v;
    if (ae == 0xFF) return 2;
    if ((c->ae == ae && pw_stable_mode(ae, &v) && v == lv) || (c->inm && pw_cs[0].inm && c->bak == lv && pw_cs[0].bak == lv))
        return 1;
    if (c->ae == ae && pw_stable_mode(ae, &v)) return 0;
    return 2;
}

/* boot check (once, from boot + 20 s, read-only; no write at boot, ever) */
static void pw_boot(int now)
{
    char a[12], b[12], l[96];
    if (S.boot_done || now < R6PW_BOOT_MS) return;
    if (!pw_bk_read)
    {
        const struct pw_ms * c = &pw_cs[1];
        if (!pw_gap_ok() || !pw_card_ok()) return;
        int16_t x;
        if (c->ae == 4 && pw_cs[0].ae == 4 && pw_stable_mode(4, &x) && (uint16_t) x != 0xDF00)   /* B check (K34 / K35) */
            pw_note(1, "Mode B shows a timed Tv. Turn the dial\noff B and back; check the bulb timer.");
        pw_bk_read = 1;
        int n = pw_mk_read(l, sizeof(l) - 1);
        if (n < 0 || S.mk_boot) { S.boot_done = 1; return; }   /* no marker (or one this boot wrote) */
        int base = 0, ae = 0, v = 0, ae2 = 0, v2 = 0;
        if (!pw_mk_parse(l, &base, &ae, &v, &ae2, &v2))
        {
            pw_note(1, "R6TV.LCK unreadable: check Tv in M.");
            pw_mk_delete();
            S.boot_done = 1;
            return;
        }
        S.base = base;
        S.landed_ae = ae;
        S.landed_v = v;
        S.landed_ok = 0;
        S.landed_ae2 = ae2;                  /* review round 2 (S2-1): the older record of another mode */
        S.landed_v2 = v2;
        S.landed_ok2 = 0;
        S.marker = 3;
    }
    /* wait until the M record is readable and stable (boot_done stays 0) */
    int16_t r;
    if (!pw_mrec(&r)) return;
    int mok = r == S.base;
    if (!mok) { S.offer = 1; S.boot_rec = r; }
    /* the landed part per record (review round 2, S2-1), independent of the M part */
    int la = S.landed_ae ? pw_boot_land(S.landed_ae, S.landed_v) : 0;
    int lb = S.landed_ae2 ? pw_boot_land(S.landed_ae2, S.landed_v2) : 0;
    if (S.landed_ae && !la) S.landed_ok = 1;
    if (S.landed_ae2 && !lb) S.landed_ok2 = 1;
    /* the notice (<= 98 characters): one open part in full; several parts one line each (M, then the records) */
    char t[100];
    t[0] = 0;
    int parts = !mok + !!la + !!lb;
    if (parts == 1 && !mok)
        snprintf(t, sizeof(t), "Tv in M %s, not yours %s:\nML bracket cut off. R6 Interval >\nTv back, or turn the dial.",
            _r6pw_tvs(a, r), _r6pw_tvs(b, S.base));
    else if (parts == 1)
    {
        int ae = la ? S.landed_ae : S.landed_ae2, k = la ? la : lb;
        int16_t lv = la ? S.landed_v : S.landed_v2;
        if (ae == 0xFF) snprintf(t, sizeof(t), "ML's Tv may still be set in mode Tv,\nB or Fv. Check them there.");
        else if (k == 1) snprintf(t, sizeof(t), "Tv in mode %s is still ML's %s.\nSet it by hand.", pw_ae_name(ae), _r6pw_tvs(a, lv));
        else snprintf(t, sizeof(t), "ML's Tv %s may still be set in mode %s.\nCheck it there.", _r6pw_tvs(a, lv), pw_ae_name(ae));
    }
    else if (parts > 1)
    {
        if (!mok) snprintf(t, sizeof(t), "Tv in M %s, not yours %s.", _r6pw_tvs(a, r), _r6pw_tvs(b, S.base));
        for (int i = 0; i < 2; i++)
        {
            int k = i ? lb : la, ae = i ? S.landed_ae2 : S.landed_ae;
            int16_t lv = i ? S.landed_v2 : S.landed_v;
            if (!k) continue;
            int o = strlen(t);
            if (ae == 0xFF) snprintf(t + o, sizeof(t) - o, "%sTv/B/Fv: ML's Tv may be set.", o ? "\n" : "");
            else snprintf(t + o, sizeof(t) - o, "%s%s: ML's %s %s.", o ? "\n" : "", pw_ae_name(ae), _r6pw_tvs(a, lv),
                k == 1 ? "still set" : "may be set");
        }
    }
    if (t[0]) { snprintf(pw_msg, sizeof(pw_msg), "%s", t); NotifyBox(10000, "%s", pw_msg); }
    if (mok && !pw_land_open1() && !pw_land_open2() && pw_card_ok())
    {
        pw_mk_delete();
        S.marker = 0;
        S.landed_ae = S.landed_ae2 = 0;
    }
    S.boot_done = 1;
}

/* the open-request watch (3.7): a delivery of the open want is ML's apply only if it cannot be the user's dial */
static void pw_watch(void)
{
    char a[12], b[12];
    if (!pw_inflight || !S.watch || !S.w_want) return;
    const struct pw_ms * c = &pw_cs[1];
    int16_t v;
    int res = 0;
    if (pw_stable_m(&v))
    {
        if (v != S.open_want) return;
        res = !S.w_other && !S.w_recv && !S.ovf && ABS(v - S.close_rec) > R6PW_GRID ? R6PW_R_LATE_APPLIED : R6PW_R_LATE_SEEN;
    }
    else if (pw_ae_other(c->ae) && pw_stable_mode(c->ae, &v))
    {
        if (v != S.open_want) return;
        res = S.w_land && !S.ovf ? R6PW_R_LANDED : R6PW_R_LATE_SEEN;
    }
    else return;
    struct r6pw_rec * e = pw_rn - pw_open_rn <= R6PW_RING ? &pw_ring[pw_open_rn % R6PW_RING] : 0;
    if (e) { e->res = res; e->d_h = pw_dms(S.u_t, e->t); }
    S.watch = 0;
    if (res == R6PW_R_LATE_SEEN)
    {
        if (e) e->fl |= R6PW_FL_OPEN;
        pw_note(1, "Tv %s seen: ML's late write or your dial.\nNo ML pictures until restart.\nRestart, check Tv.", _r6pw_tvs(a, v));
        return;                          /* stays open */
    }
    if (e) e->fl &= ~R6PW_FL_OPEN;
    pw_inflight = 0;                     /* resolved: the press block ends; writes stay off (pw_dead) */
    pw_sticky_reset(get_ms_clock(), get_ms_clock());
    if (res == R6PW_R_LANDED)
    {
        pw_land_rec(c->ae, v);               /* an older open record of another mode is kept (review round 2, S2-1) */
        pw_mk_landed();
        pw_note(1, "ML's Tv %s landed late in mode %s.\nSet it there by hand.", _r6pw_tvs(a, v), pw_ae_name(c->ae));
    }
    else
    {
        S.last = v;
        pw_note(1, "ML's Tv %s was applied late.\nTurn the dial to %s.", _r6pw_tvs(a, v), _r6pw_tvs(b, S.base));
    }
}

/* the deferred restore (D3): 1 = make the one call now; ends the pending otherwise when it can never run */
static int pw_pend_due(int now)
{
    char a[12];
    if (!S.pend || S.armed != R6PW_CTX_PEND || r6_iv_running() || !S.boot_done || pw_inflight) return 0;
    int16_t u;
    /* M already holds the base: stable-M, or the M record in any mode (K34), before the cancel branch (review round 1,
     * L4: no offer for a base that M already has) */
    if (pw_mrec_base())
    {
        pw_done(pw_rec_new(S.base, R6PW_K_RESTORE_PEND, 0xFF, S.ev_no));
        pw_pend_end();
        return 0;
    }
    if (now - (int) S.pend_t0 >= R6PW_PEND_MS || total_shots_count != (int) S.pend_cnt || pw_fileno() != (int) S.pend_fn)
    {
        pw_offer();                      /* cancelled: the "Tv back" offer, marker kept */
        pw_pend_end();
        return 0;
    }
    if (pw_user_ev(S.last, &u))          /* the user took over (in M, also after a mode visit) */
    {
        struct r6pw_rec * e = pw_rec_new(S.base, R6PW_K_RESTORE_PEND, 0xFF, S.ev_no);
        e->rep = u;
        e->fl |= R6PW_FL_USER;
        pw_rec_end(e, R6PW_R_TOOK_OVER, R6PW_L_LAST);
        pw_note(1, "Tv %s is yours; ML did not restore it.", _r6pw_tvs(a, u));
        pw_pend_end();
        return 0;
    }
    /* row 7's card part can never hold once the marker line was lost (pw_mk_landed: -2, review round 1 L5): one visible
     * refusal, the offer at once, the pending restore ends (pw_lim -> pw_dead), instead of a refused call attempt every
     * tick until the bound (review round 2, L2-4) */
    if (pw_mk_len < 0)
    {
        pw_rec_end(pw_rec_new(S.base, R6PW_K_RESTORE_PEND, 0xFF, S.ev_no), R6PW_R_REFUSED, R6PW_L_MARKER);
        pw_offer();
        pw_note(1, "R6TV.LCK lost: Tv not restored.\nCheck Tv in M (Tv back) and in\nmode %s.", pw_ae_name(S.landed_ae));
        pw_pend_end();
        return 0;
    }
    struct pw_ev q;
    memset(&q, 0, sizeof(q));
    q.want = S.base;
    q.kind = R6PW_K_RESTORE_PEND;
    int l = pw_eval_all(&q);             /* every row, lock 44 included (no ring entry while it waits) */
    if (l == R6PW_L_LAST && q.sticky && !q.evid) { pw_offer(); pw_pend_end(); return 0; }
    return l == 0;
}

/* after the one deferred call: OK / DONE / MODE (= the restore itself) end it; REFUSED keeps the attempt (RR2-15);
 * FOREIGN and TOOK_OVER: the user's value wins (3.7), no offer (review round 2, L2-1: the offer's value would be the
 * user's own, which its watcher can never clear). The M test reads a fresh pair (the consumer did not run since the
 * window), so INVOLVED / DEP with M at the base offer nothing either */
static void pw_pend_after(int r)
{
    int16_t v;
    char a[12];
    if (r == R6PW_R_REFUSED) return;
    if (r == R6PW_R_LANDED) pw_mk_landed();   /* the marker records the mode and value (3.9, review round 1 F3) */
    if (r == R6PW_R_OK) pw_note(1, "Tv back to %s.", _r6pw_tvs(a, S.base));
    if (r != R6PW_R_OK && r != R6PW_R_DONE && r != R6PW_R_MODE && r != R6PW_R_TOOK_OVER && r != R6PW_R_FOREIGN && !pw_inflight)
    {
        pw_fresh2();
        if (!(pw_stable_m(&v) && v == S.base)) pw_offer();
    }
    pw_pend_end();
}

/* the offer watcher: only while nothing is armed and no request is open; user evidence clears it */
static void pw_offer_watch(void)
{
    int16_t u;
    if (S.offer && !S.armed && !pw_inflight && pw_user_ev(S.boot_rec, &u)) S.offer = 0;
}

/* the marker rule (3.8): the only way the marker is deleted */
static void pw_rule(void)
{
    int16_t v, u;
    if (S.marker < 1 || S.armed || S.ev_on || S.busy || r6_iv_running() || pw_inflight || S.pend || S.offer) return;
    if (!S.boot_done || !pw_card_ok()) return;
    if (r6pw_sig()) return;              /* spec 3.11: after a shutdown / abort signal no marker delete (QEMU m102 F11) */
    /* M part: M stably the base, the M record the base in any mode (as the boot check, review round 1 F2 / L4), or the
     * user's own value in M */
    int mok = (pw_stable_m(&v) && v == S.base) || pw_mrec_base() || pw_user_ev(S.base, &u);
    if (!mok || pw_land_open1() || pw_land_open2()) return;      /* every landed record resolved (review round 2, S2-1) */
    pw_mk_delete();
    S.marker = 0;
    S.landed_ae = S.landed_ae2 = 0;
    S.landed_v = S.landed_v2 = 0;
    S.landed_ok = S.landed_ok2 = 0;
}

void __attribute__((noinline)) _r6pw_tick(int now)
{
    if (!pw_task()) return;
    _r6pw_consume(now);
    pw_boot(now);
    pw_watch();
    if (pw_pend_due(now))
    {
        S.busy = 1;                      /* the ML menu waits for the window (review round 1, safety L5) */
        int r = r6pw_write(S.base, R6PW_K_RESTORE_PEND, 0xFF, S.ev_no);   /* exactly one call (D3) */
        pw_pend_after(r);
        S.busy = 0;
    }
    pw_offer_watch();
    pw_mk_landed();                      /* a landed mode / value the marker line does not hold yet (card was busy, F3) */
    pw_rule();
}

/* ---- read-only API ---- */
int _r6pw_press_ok(void) { return !pw_inflight; }
int _r6pw_sig(void) { return r6pw_sig(); }   /* a shutdown / abort signal (3.11): no further press of an event */
int _r6pw_on(void) { return pw_cfg_on; }
void _r6pw_toggle(void) { pw_cfg_on = !pw_cfg_on; }

const struct r6pw_state * _r6pw_state(void)
{
    S.open = pw_inflight;
    S.dead = pw_dead;
    S.lim = pw_lim;
    S.sig = pw_sig;
    return &S;
}

void _r6pw_log(int * hn, int16_t * hv, int * hlen, int * ht)
{
    uint32_t n = pw_hn;
    __sync_synchronize();
    *hn = n;
    *hv = 0; *hlen = 0; *ht = 0;
    if (!n) return;
    const volatile struct r6pw_h * h = &pw_h[(n - 1) & (R6PW_HRING - 1)];
    *hv = h->v; *hlen = h->len; *ht = h->t;
}

static const char * const pw_lock_names[R6PW_NLOCKS] = {
    "-", "switch OFF", "off until restart", "shutdown / abort", "task", "not armed", "spacing", "marker file",
    "Canon pointers", "mode", "movie", "CC state", "GUI / ML menu / LV", "shutter held", "Canon job", "Auto ISO",
    "AEB", "WB bracketing", "HDR mode", "multiple exposure", "focus bracketing", "multi-shot NR",
    "electronic shutter", "Canon timer / drive", "step not 1/3", "Tv substitution", "CC not ready", "CC 0x62C",
    "CC context", "CC 0xB78 / movie HDR", "slot 0x28 (step)", "slot 0x59 (Tv)", "slot 0x0F (AEB)",
    "slot 0x2D / comp", "slot 0x20 (ALO)", "slot 0x66 / flash", "Tv not settled", "Tv out of range", "AEB backup",
    "ALO backup", "new Tv not allowed", "torn read", "mechaless", "faster than 1/200 (flash?)", "Tv changed",
    "only the Tv restore", "write open", "restore pending", "Tv back offered", "boot check", "marker kept",
};
static const char * const pw_res_names[R6PW_NRES] = {
    "-", "ok", "refused", "done", "no confirmation", "shutdown", "user", "user, ML open", "clamped", "dependent",
    "mode", "mode, open", "landed", "crossed", "took over", "late applied", "late seen",
};
static const char * const pw_kind_names[R6PW_NKINDS] = { "-", "step", "restore", "restore_stop", "restore_pend", "clampfix", "back" };

const char * _r6pw_lock_name(int l) { return (l >= 0 && l < R6PW_NLOCKS) ? pw_lock_names[l] : "?"; }
const char * _r6pw_res_name(int r) { return (r >= 0 && r < R6PW_NRES) ? pw_res_names[r] : "?"; }
const char * _r6pw_kind_name(int k) { return (k >= 0 && k < R6PW_NKINDS) ? pw_kind_names[k] : "?"; }

void _r6pw_lock_text(int lock, char * buf, int n)
{
    char a[12];
    int o1 = pw_land_open1(), o2 = pw_land_open2();               /* the open landed records (review round 2, S2-1) */
    int ae = o1 ? S.landed_ae : S.landed_ae2;
    int16_t v = o1 ? S.landed_v : S.landed_v2;
    if (lock == R6PW_L_MKEPT && ((o1 && o2 && S.landed_ae2 == 0xFF) || (!o1 && o2 && ae == 0xFF)))
        snprintf(buf, n, "ML's Tv may still be set in modes Tv, B and Fv: set them by hand");
    else if (lock == R6PW_L_MKEPT && o1 && o2)
        snprintf(buf, n, "ML's Tv may still be set in modes %s and %s: set them by hand", pw_ae_name(S.landed_ae), pw_ae_name(S.landed_ae2));
    else if (lock == R6PW_L_MKEPT && (o1 || o2))
        snprintf(buf, n, "ML's Tv %s may still be set in mode %s: set it there by hand", _r6pw_tvs(a, v), pw_ae_name(ae));
    else if (lock == R6PW_L_MKEPT) snprintf(buf, n, "Tv in M is ML's %s; set it or use Tv back", _r6pw_tvs(a, pw_cs[1].rec));
    else snprintf(buf, n, "Tv writes: %s", _r6pw_lock_name(lock));
}

#endif /* CONFIG_R6_PROPW_TV */
