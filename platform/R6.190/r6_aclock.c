/*
 * R6.190 M14.1: the astro clock (spec m14/m141 spec_m141.md Rev. 2, 2.3 / 2.4 / 4.3 / 5.3 / 8; docs/m14_plan.md E7 a + c,
 * E8 without GPS, E9, E10 b, E13, approved 2026-10-15, "M14.1 starten" 2026-10-16). Read only; nothing new at Canon:
 *  - time: r6_tlog_task's RTC pair (LoadCalendarFromRTC once a minute, only there) plus ML's ms clock, whole seconds, never
 *    backwards by up to 2 s (a later pair may be up to 1 s behind the extrapolation); valid for 2020..2059;
 *  - UTC = local time - offset: Canon's total offset (zone + 60 DST from the time-zone struct, Tor 0g, r6tl_tz_now every
 *    10 s, valid only when read twice equal and found in Canon's table; one failed read keeps the last zone for at most
 *    30 s) or the manual offset of the menu. After a change of Canon's zone / DST the clock waits for an RTC pair taken
 *    after the change (Canon moved its clock too; skeptic M4);
 *  - the night (local noon .. noon) is computed by r6_acore.c in the task r6_aclock_task only: once per new key (noon,
 *    location, offset, Milky Way criteria) that stayed 1 s unchanged, in chunks of at most AC_CHUNK evaluations with a
 *    10 ms sleep between them (r6_tlog_task and r6_hist_task share priority 0x1f; skeptic H3); a newer key aborts it;
 *  - "now" every 10 s at the location of the night's key; phases with each night and when one is passed;
 *  - results go to r6ac_pub under a seqlock; the menu updates, the LV line (r6_grid.c) and the log only copy them.
 * Location by hand only (no GPS, E8), 0.01 deg, saved, default 50.60 N / 11.30 E.
 * Writes: this file's own state and settings. No Canon RAM / MMIO / property write, no new Canon call; msleep only here.
 * Texts: no "%%" in a format (Canon's vsnprintf takes an argument for it, QEMU q141a): the percent sign is a "%s" argument.
 */
#include "dryos.h"
#include "menu.h"
#include "config.h"
#include "bmp.h"                /* SYM_DEGREE */
#include "tasks.h"
#include "r6_tlog.h"
#include "r6_acore.h"
#include "r6_aclock.h"
#include "r6_ettr.h"            /* r6_ettr_lv_wanted: R6 ETTR's line first (E13) */
#include "r6_interval.h"        /* r6_iv_in_pic */

#ifdef CONFIG_R6_ACLOCK
#if !defined(CONFIG_R6_TLOG) || !defined(CONFIG_R6_GRID) || !defined(CONFIG_R6_ETTR)
#error "r6_aclock.c: CONFIG_R6_ACLOCK needs CONFIG_R6_TLOG (clock, zone), CONFIG_R6_GRID (the line) and CONFIG_R6_ETTR (its priority)"
#endif

extern int get_global_draw_setting(void);   /* src/zebra.c */

static CONFIG_INT("aclock.lat",   r6ac_lat, 5060);     /* 0.01 deg, N positive */
static CONFIG_INT("aclock.lon",   r6ac_lon, 1130);     /* 0.01 deg, E positive */
static CONFIG_INT("aclock.tz",    r6ac_tzsrc, 0);      /* 0 Canon, 1 Manual */
static CONFIG_INT("aclock.utcq",  r6ac_utcq, 4);       /* manual offset, quarter hours (-12:00 .. +14:00) */
static CONFIG_INT("aclock.view",  r6ac_view, 0);       /* Polaris clock: 0 sky, 1 scope 180 deg, 2 mirrored */
static CONFIG_INT("aclock.mwgc",  r6ac_mwgc, 0);       /* 5 / 10 / 15 deg */
static CONFIG_INT("aclock.mwsun", r6ac_mwsun, 0);      /* -18 / -15 / -12 deg */
static CONFIG_INT("aclock.lv",    r6ac_lv, 0);         /* Show in LV */

static const int8_t r6ac_gc_deg[3] = { 5, 10, 15 };
static const int8_t r6ac_sun_deg[3] = { -18, -15, -12 };

/* the night's key */
struct r6ac_key { int32_t t0, off; struct ac_loc loc; struct ac_crit crit; };
static int r6ac_keq(const struct r6ac_key * a, const struct r6ac_key * b)
{
    return a->t0 == b->t0 && a->off == b->off && a->loc.lat_e2 == b->loc.lat_e2 && a->loc.lon_e2 == b->loc.lon_e2
        && a->crit.gc_min_deg == b->crit.gc_min_deg && a->crit.sun_max_deg == b->crit.sun_max_deg;
}

struct r6ac_stat
{
    uint32_t n_night, n_now, us_last, us_max, us_now_max, us_chunk_max, chunks, aborts, key_changes, debounced;
    uint32_t back_jumps, zchanges;
    int calc_ms, pair_age, fails_row;
};

/* the published state (writer r6_aclock_task, seqlock r6ac_seq) */
struct r6ac_res
{
    uint8_t state, night_ok, now_ok, zvalid, pending, src, eu, pad;
    struct r6ac_key want, nk;          /* the key of the time and settings now; the key of n */
    struct ac_night n;
    struct ac_now w;
    struct r6tl_tz z;                  /* the last zone read (the log's ac.tz) */
    int16_t zu_zone, zu_dst, zu_off, zu_total;   /* the zone in use (the last valid one; valid with zvalid): the menu (review R1 F3) */
    int32_t off;                       /* the offset in use (min) */
    int32_t local_s, utc_s;            /* at tick_ms */
    int tick_ms;
    int32_t eu_date;                   /* the EU change date of the hint (UTC s) */
    struct r6ac_stat st;
};
static struct r6ac_res r6ac_r;         /* the task's working copy */
static struct r6ac_res r6ac_pub;       /* the published copy */
static volatile uint32_t r6ac_seq = 0; /* odd while the task writes r6ac_pub */

static struct ac_work r6ac_wk;         /* the core's buffers: static, not on the 4 KB stack */
static struct ac_night r6ac_nb;
static struct ac_now r6ac_wb;

static void r6ac_publish(void)
{
    r6ac_seq++;
    __sync_synchronize();
    memcpy(&r6ac_pub, &r6ac_r, sizeof(r6ac_pub));
    __sync_synchronize();
    r6ac_seq++;
}

/* a copy of the published state (no wait, at most 4 tries); 0 = busy */
static int r6ac_get(struct r6ac_res * o)
{
    for (int k = 0; k < 4; k++)
    {
        uint32_t s1 = r6ac_seq;
        __sync_synchronize();
        memcpy(o, &r6ac_pub, sizeof(*o));
        __sync_synchronize();
        uint32_t s2 = r6ac_seq;
        if (s1 == s2 && !(s1 & 1)) return 1;
    }
    return 0;
}

/* ---- the zone (r6_aclock_task only) ---- */
static struct r6tl_tz r6ac_zv;         /* the last valid zone */
static int r6ac_zv_ok, r6ac_zv_t, r6ac_fails, r6ac_pend, r6ac_pend_t;

static void r6ac_zone(int now)
{
    struct r6tl_tz z;
    int ok = r6tl_tz_now(&z);
    r6ac_r.z = z;
    if (ok)
    {
        if (r6ac_zv_ok && (z.zone != r6ac_zv.zone || z.dst != r6ac_zv.dst || z.total != r6ac_zv.total))
        {
            r6ac_pend = 1;                  /* Canon moved its clock: wait for a pair taken after this */
            r6ac_pend_t = now;
            r6ac_r.st.zchanges++;
        }
        r6ac_zv = z;
        r6ac_zv_ok = 1;
        r6ac_zv_t = now;
        r6ac_fails = 0;
    }
    else if (r6ac_fails < 99) r6ac_fails++;
    r6ac_r.st.fails_row = r6ac_fails;
}

/* ---- the chunk callback of the core: sleeps, measures, aborts on new settings ---- */
struct r6ac_ctx { int32_t lat, lon; int gc, sun, src, utcq; uint64_t t_us; uint32_t us_sum, us_chunk; };
static struct r6ac_ctx r6ac_cx;

static int r6ac_yield(void * ctx)
{
    struct r6ac_ctx * c = ctx;
    uint32_t d = (uint32_t) (get_us_clock() - c->t_us);
    c->us_sum += d;
    if (d > c->us_chunk) c->us_chunk = d;
    msleep(R6AC_YIELD_MS);
    c->t_us = get_us_clock();
    return c->lat != r6ac_lat || c->lon != r6ac_lon || c->gc != r6ac_mwgc || c->sun != r6ac_mwsun || c->src != r6ac_tzsrc
        || c->utcq != r6ac_utcq;
}

static void r6ac_cx_start(void)
{
    r6ac_cx.lat = r6ac_lat; r6ac_cx.lon = r6ac_lon; r6ac_cx.gc = r6ac_mwgc; r6ac_cx.sun = r6ac_mwsun;
    r6ac_cx.src = r6ac_tzsrc; r6ac_cx.utcq = r6ac_utcq;
    r6ac_cx.us_sum = r6ac_cx.us_chunk = 0;
    r6ac_cx.t_us = get_us_clock();
}

static void r6ac_cx_end(int night)
{
    struct r6ac_stat * s = &r6ac_r.st;
    uint32_t d = (uint32_t) (get_us_clock() - r6ac_cx.t_us);
    r6ac_cx.us_sum += d;
    if (d > r6ac_cx.us_chunk) r6ac_cx.us_chunk = d;
    if (r6ac_cx.us_chunk > s->us_chunk_max) s->us_chunk_max = r6ac_cx.us_chunk;
    s->chunks += r6ac_wk.chunks + 1;
    if (night)
    {
        s->us_last = r6ac_cx.us_sum;
        if (r6ac_cx.us_sum > s->us_max) s->us_max = r6ac_cx.us_sum;
    }
    else if (r6ac_cx.us_sum > s->us_now_max) s->us_now_max = r6ac_cx.us_sum;
}

/* the last Sunday of month m (March / October) at 01:00 UTC */
static int32_t r6ac_last_sun(int y, int m)
{
    int Y, M, D, h, mi, s, wd;
    int32_t t = ac_utc_s(y, m, 31, 1, 0, 0, 0);
    ac_civil(t, 0, &Y, &M, &D, &h, &mi, &s, &wd);
    return t - wd * 86400;
}

/* the EU DST hint (only for Canon's zones 19 Paris, 20 London, 21 Azores; skeptic M11): 1 = DST on but EU summer time
 * ended (date), 2 = DST off but EU summer time began (date), 0 = none */
static void r6ac_eu(int32_t utc)
{
    int Y, M, D, h, mi, s, wd;
    r6ac_r.eu = 0;
    if (r6ac_r.src || !r6ac_r.zvalid || r6ac_zv.zone < 19 || r6ac_zv.zone > 21) return;
    ac_civil(utc, 0, &Y, &M, &D, &h, &mi, &s, &wd);
    int32_t a = r6ac_last_sun(Y, 3), b = r6ac_last_sun(Y, 10);
    int summer = utc >= a && utc < b;
    if (r6ac_zv.dst && !summer) { r6ac_r.eu = 1; r6ac_r.eu_date = utc >= b ? b : r6ac_last_sun(Y - 1, 10); }
    if (!r6ac_zv.dst && summer) { r6ac_r.eu = 2; r6ac_r.eu_date = a; }
}

static int r6ac_mdays(int y, int m)
{
    static const uint8_t ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return ml[m - 1] + (m == 2 && !(y & 3));
}

static void r6ac_task(void * unused)
{
    int t_tz = 0, t_now = 0, key_t = 0, first = 1, have_loc = 0, last_n = 0, pend_ok = 0;
    int32_t last_loc = 0;
    struct r6ac_key pend;
    memset(&pend, 0, sizeof(pend));
    TASK_LOOP
    {
        msleep(R6AC_TASK_MS);
        const int now = get_ms_clock();
        struct r6ac_res * r = &r6ac_r;
        struct r6ac_stat * s = &r->st;
        const int src = COERCE(r6ac_tzsrc, 0, 1);
        struct r6ac_key want;
        want.loc.lat_e2 = COERCE(r6ac_lat, -9000, 9000);
        want.loc.lon_e2 = COERCE(r6ac_lon, -18000, 18000);
        want.crit.gc_min_deg = r6ac_gc_deg[COERCE(r6ac_mwgc, 0, 2)];
        want.crit.sun_max_deg = r6ac_sun_deg[COERCE(r6ac_mwsun, 0, 2)];

        /* the zone */
        if (first || now - t_tz >= R6AC_TZ_MS) { t_tz = now; r6ac_zone(now); }
        first = 0;
        const int zuse = r6ac_zv_ok && (r6ac_fails == 0 || (r6ac_fails < 2 && now - r6ac_zv_t <= R6AC_ZONE_KEEP_MS));

        /* the clock: the newest RTC pair, extrapolated */
        struct tm tm;
        int t_ms = 0, n = 0, st = R6AC_OK;
        int32_t locs = 0;
        memset(&tm, 0, sizeof(tm));
        if (!r6tl_rtc(&tm, &t_ms, &n)) st = R6AC_WAIT;
        else
        {
            const int y = tm.tm_year + 1900, mo = tm.tm_mon + 1, age = now - t_ms;
            s->pair_age = age;
            if (mo < 1 || mo > 12 || tm.tm_mday < 1 || tm.tm_mday > r6ac_mdays(y, mo) || tm.tm_hour < 0 || tm.tm_hour > 23
                || tm.tm_min < 0 || tm.tm_min > 59 || tm.tm_sec < 0 || tm.tm_sec > 60 || age < 0 || age > R6AC_PAIR_MAX_MS)
                st = R6AC_CLOCK_BAD;
            else if (y < R6AC_Y_MIN || y > R6AC_Y_MAX) st = R6AC_RANGE;
            else
            {
                locs = ac_utc_s(y, mo, tm.tm_mday, tm.tm_hour, tm.tm_min, MIN(tm.tm_sec, 59), 0) + age / 1000;
                if (have_loc && locs < last_loc && last_loc - locs <= 2)
                {
                    if (n != last_n) s->back_jumps++;   /* a new pair a little behind the extrapolation: hold */
                    locs = last_loc;
                }
                last_loc = locs;
                have_loc = 1;
            }
            if (r6ac_pend && t_ms - r6ac_pend_t > 0) r6ac_pend = 0;   /* a pair taken after the zone change */
        }
        last_n = n;

        /* the offset */
        int32_t off = 0;
        if (st == R6AC_OK)
        {
            if (src) off = 15 * COERCE(r6ac_utcq, -48, 56);
            else if (!zuse) st = R6AC_TZ_BAD;
            else if (r6ac_pend) st = R6AC_PENDING;
            else off = r6ac_zv.total;
        }
        r->src = src;
        r->zvalid = zuse;
        r->pending = r6ac_pend;
        r->off = off;
        r->local_s = locs;
        r->utc_s = locs - off * 60;
        r->tick_ms = now;
        r->zu_zone = r6ac_zv.zone; r->zu_dst = r6ac_zv.dst; r->zu_off = r6ac_zv.off; r->zu_total = r6ac_zv.total;
        /* the EU DST hint needs today's UTC date: only with a valid clock and offset (review R1 F1: not while waiting, with an
         * invalid clock or after a zone change) */
        if (st == R6AC_OK) r6ac_eu(r->utc_s);
        else r->eu = 0;

        if (st == R6AC_OK)
        {
            /* the night's key: from the local noon of today (from 12:00 on) or of yesterday */
            int Y, M, D, h, mi, sc, wd;
            ac_civil(locs, 0, &Y, &M, &D, &h, &mi, &sc, &wd);
            if (h < 12) ac_civil(locs - 86400, 0, &Y, &M, &D, &h, &mi, &sc, &wd);
            want.t0 = ac_utc_s(Y, M, D, 12, 0, 0, off);
            want.off = off;
            r->want = want;
            int do_now = 0, phases = 0;
            if (!r->night_ok || !r6ac_keq(&r->nk, &want))
            {
                st = R6AC_COMPUTING;
                if (!pend_ok || !r6ac_keq(&pend, &want)) { pend = want; pend_ok = 1; key_t = now; s->key_changes++; }
                else if (now - key_t >= R6AC_DEBOUNCE_MS)
                {
                    s->debounced++;
                    r->state = st;
                    r6ac_publish();
                    r6ac_cx_start();
                    int rc = ac_night(&want.loc, &want.crit, want.t0, &r6ac_wk, &r6ac_nb, r6ac_yield, &r6ac_cx);
                    r6ac_cx_end(1);
                    if (rc == 0)
                    {
                        r->n = r6ac_nb;
                        r->nk = want;
                        r->night_ok = 1;
                        s->n_night++;
                        s->calc_ms = get_ms_clock();
                        pend_ok = 0;
                        do_now = phases = 1;
                        st = R6AC_OK;
                    }
                    else s->aborts++;
                }
            }
            if (r->night_ok && (do_now || !r->now_ok || now - t_now >= R6AC_NOW_MS))
            {
                const int32_t u = r->utc_s;
                if (!r->now_ok || u >= r->w.next_new || u >= r->w.next_full || r->w.next_new == AC_NONE || r->w.next_full == AC_NONE)
                    phases = 1;
                r6ac_wb = r->w;
                r6ac_cx_start();
                int rc = ac_now(&r->nk.loc, u, phases, &r6ac_wk, &r6ac_wb, r6ac_yield, &r6ac_cx);
                r6ac_cx_end(0);
                if (rc == 0) { r->w = r6ac_wb; r->now_ok = 1; s->n_now++; t_now = now; }
                else s->aborts++;
            }
        }
        r->state = st;
        r6ac_publish();
    }
}

TASK_CREATE("r6_aclock_task", r6ac_task, 0, 0x1f, 0x1000);

/* ---- text (copies only; no division by 10 / 60: small loops) ---- */
static int r6ac_dm(int32_t v, int32_t d, int32_t * rest)
{
    int q = 0;
    while (v >= d) { v -= d; q++; }
    *rest = v;
    return q;
}

/* one decimal of a 0.1 value, never "-0.0" */
static const char * r6ac_fix1(char * s, int n, int e1)
{
    int32_t r;
    int q = r6ac_dm(e1 < 0 ? -e1 : e1, 10, &r);
    snprintf(s, n, "%s%d.%d", e1 < 0 && (q || r) ? "-" : "", q, (int) r);
    return s;
}

/* hh:mm local, rounded to the minute (as USNO) */
static const char * r6ac_hhmm(char * s, int n, int32_t t, int32_t off)
{
    int Y, M, D, h, mi, sc, wd;
    if (t == AC_NONE) { snprintf(s, n, "--:--"); return s; }
    ac_civil(t + 30, off, &Y, &M, &D, &h, &mi, &sc, &wd);
    snprintf(s, n, "%02d:%02d", h, mi);
    return s;
}

static const char * const r6ac_wdn[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/* "Fri 16.10." local */
static const char * r6ac_day(char * s, int n, int32_t t, int32_t off)
{
    int Y, M, D, h, mi, sc, wd;
    ac_civil(t, off, &Y, &M, &D, &h, &mi, &sc, &wd);
    snprintf(s, n, "%s %02d.%02d.", r6ac_wdn[wd], D, M);
    return s;
}

/* "UTC+2:00" */
static const char * r6ac_offs(char * s, int n, int m)
{
    int32_t r;
    int h = r6ac_dm(m < 0 ? -m : m, 60, &r);
    snprintf(s, n, "UTC%s%d:%02d", m < 0 ? "-" : "+", h, (int) r);
    return s;
}

/* "50.60" SYM_DEGREE " N" */
static const char * r6ac_deg2(char * s, int n, int v, const char * pos, const char * neg)
{
    int32_t r;
    int q = r6ac_dm(v < 0 ? -v : v, 100, &r);
    snprintf(s, n, "%d.%02d" SYM_DEGREE " %s", q, (int) r, v < 0 ? neg : pos);
    return s;
}

/* minutes on the 12-h dial as h:mm, 12:xx instead of 0:xx */
static const char * r6ac_dial(char * s, int n, int p)
{
    int32_t r;
    int h = r6ac_dm(p, 60, &r);
    snprintf(s, n, "%d:%02d", h ? h : 12, (int) r);
    return s;
}

/* the view of the published state */
struct r6ac_v { struct r6ac_res r; int got, ok, night, now; int32_t utc, loc; };
static void r6ac_look(struct r6ac_v * v)
{
    v->got = r6ac_get(&v->r);
    v->ok = v->got && (v->r.state == R6AC_OK || v->r.state == R6AC_COMPUTING);
    v->night = v->ok && v->r.night_ok;
    v->now = v->night && v->r.now_ok;
    int32_t el = v->got ? (int32_t) (get_ms_clock() - v->r.tick_ms) / 1000 : 0;
    if (el < 0) el = 0;
    v->utc = v->r.utc_s + el;
    v->loc = v->r.local_s + el;
}

static const char * r6ac_state_text(const struct r6ac_v * v)
{
    if (!v->got) return "busy";
    switch (v->r.state)
    {
        case R6AC_OK:        return "ok";
        case R6AC_COMPUTING: return "computing";
        case R6AC_PENDING:   return "waiting: time zone changed";      /* < 34 characters (MENU_MAX_VALUE_LEN, review R3 F1) */
        case R6AC_CLOCK_BAD: return "clock invalid";
        case R6AC_RANGE:     return "date out of range (2020-2059)";
        case R6AC_TZ_BAD:    return "time zone unreadable: use Manual";
    }
    return "waiting for clock";
}

/* a sun threshold as a run: "19:04 - 07:19", "none (sun min -4.2 deg)", "all night", "from 19:04", "noon - 07:19" */
static void r6ac_run(char * s, int n, const struct r6ac_v * v, int k)
{
    char a[8], b[8], c[12];
    const struct ac_night * x = &v->r.n;
    const int32_t o = v->r.nk.off;
    switch (x->sun_state[k])
    {
        case AC_CROSS:       snprintf(s, n, "%s - %s", r6ac_hhmm(a, 8, x->sun_dn[k], o), r6ac_hhmm(b, 8, x->sun_up[k], o)); break;
        case AC_FROM_START:  snprintf(s, n, "noon - %s", r6ac_hhmm(b, 8, x->sun_up[k], o)); break;
        case AC_PARTIAL:     snprintf(s, n, "from %s", r6ac_hhmm(a, 8, x->sun_dn[k], o)); break;
        case AC_ALWAYS_DOWN: snprintf(s, n, "all night"); break;
        default:             snprintf(s, n, "none (sun min %s" SYM_DEGREE ")", r6ac_fix1(c, 12, x->sun_min_e1)); break;
    }
}

/* the Milky Way window */
static void r6ac_mw(char * s, int n, const struct r6ac_v * v)
{
    char a[8], b[8], c[12];
    const struct ac_night * x = &v->r.n;
    const int32_t o = v->r.nk.off;
    const char * more = x->mw_more ? " +" : "";
    switch (x->mw_state)
    {
        case AC_CROSS:       snprintf(s, n, "%s - %s (%d min)%s", r6ac_hhmm(a, 8, x->mw_from, o), r6ac_hhmm(b, 8, x->mw_to, o), x->mw_min, more); break;
        case AC_FROM_START:  snprintf(s, n, "noon - %s (%d min)%s", r6ac_hhmm(b, 8, x->mw_to, o), x->mw_min, more); break;
        case AC_PARTIAL:     snprintf(s, n, "from %s (%d min)%s", r6ac_hhmm(a, 8, x->mw_from, o), x->mw_min, more); break;
        case AC_ALWAYS_DOWN: snprintf(s, n, "all night"); break;
        default:
            if (x->mw_none == AC_MW_NONE_SUN) snprintf(s, n, "none (sun min %s" SYM_DEGREE ")", r6ac_fix1(c, 12, x->sun_min_e1));
            else if (x->mw_none == AC_MW_NONE_GC) snprintf(s, n, "none (centre max %s" SYM_DEGREE ")", r6ac_fix1(c, 12, x->gc_max_e1));
            else snprintf(s, n, "none (dark: centre max %s" SYM_DEGREE ")", r6ac_fix1(c, 12, x->mw_dark_gc_max_e1));   /* <= 30 characters (review R3 F2) */
            break;
    }
}

/* the next moon event after t: "set 20:31", "rise 14:40", else "up" / "down" */
static void r6ac_moon_next(char * s, int n, const struct r6ac_v * v, int32_t t, int sh)
{
    char a[8];
    const struct ac_night * x = &v->r.n;
    int32_t ri = x->moon_rise != AC_NONE && x->moon_rise > t ? x->moon_rise : AC_NONE;
    int32_t se = x->moon_set != AC_NONE && x->moon_set > t ? x->moon_set : AC_NONE;
    if (ri != AC_NONE && (se == AC_NONE || ri < se)) snprintf(s, n, sh ? "r%s" : "rise %s", r6ac_hhmm(a, 8, ri, v->r.nk.off));
    else if (se != AC_NONE) snprintf(s, n, sh ? "s%s" : "set %s", r6ac_hhmm(a, 8, se, v->r.nk.off));
    else snprintf(s, n, "%s", v->now && v->r.w.moon_up ? "up" : "down");
}

/* the Polaris clock of the selected view */
static int r6ac_pclock(const struct r6ac_v * v) { return v->r.w.pol_clock_min[COERCE(r6ac_view, 0, 2)]; }

/* the LV line (also the log's text) */
static int r6ac_lv_fmt(char * buf, int n, int sh, const struct r6ac_v * v)
{
    char ni[24], mo[16], po[16], a[8], b[8];
    if (!v->got) return 0;
    if (v->r.state == R6AC_TZ_BAD) { snprintf(buf, n, "Astro clock: time zone?"); return 1; }
    if (v->r.state == R6AC_WAIT || v->r.state == R6AC_PENDING) return 0;   /* only waiting: no text (spec 5.3, review R1 F2) */
    if (!v->ok) { snprintf(buf, n, "Astro clock: clock?"); return 1; }
    if (!v->now) return 0;
    const struct ac_night * x = &v->r.n;
    const int32_t o = v->r.nk.off;
    const int k = AC_H_ASTRO;
    switch (x->sun_state[k])
    {
        case AC_CROSS:       snprintf(ni, sizeof(ni), "%s-%s", r6ac_hhmm(a, 8, x->sun_dn[k], o), r6ac_hhmm(b, 8, x->sun_up[k], o)); break;
        case AC_FROM_START:  snprintf(ni, sizeof(ni), "-%s", r6ac_hhmm(b, 8, x->sun_up[k], o)); break;
        case AC_PARTIAL:     snprintf(ni, sizeof(ni), "%s-", r6ac_hhmm(a, 8, x->sun_dn[k], o)); break;
        case AC_ALWAYS_DOWN: snprintf(ni, sizeof(ni), "all"); break;
        default:             snprintf(ni, sizeof(ni), "none"); break;
    }
    r6ac_moon_next(mo, sizeof(mo), v, v->utc, sh);
    const int pm = (v->r.w.moon_pm + 5) / 10;
    if (v->r.nk.loc.lat_e2 >= 0) r6ac_dial(po, sizeof(po), r6ac_pclock(v));
    if (sh) snprintf(buf, n, "N %s M%d%s %s%s%s", ni, pm, "%", mo, v->r.nk.loc.lat_e2 >= 0 ? " P" : "", v->r.nk.loc.lat_e2 >= 0 ? po : "");
    else snprintf(buf, n, "Night %s  Moon %d%s %s%s%s", ni, pm, "%", mo, v->r.nk.loc.lat_e2 >= 0 ? "  Pol " : "", v->r.nk.loc.lat_e2 >= 0 ? po : "");
    return 1;
}

int r6_ac_lv_wanted(void) { return r6ac_lv != 0; }

int r6_ac_lv_text(char * buf, int n, int short_form)
{
    struct r6ac_v v;
    if (!r6ac_lv || r6_ettr_lv_wanted() || r6_iv_in_pic()) return 0;    /* ETTR's line first (E13); nothing in ML's picture */
    r6ac_look(&v);
    return r6ac_lv_fmt(buf, n, short_form, &v);
}

static uint32_t r6ac_lv_draws, r6ac_lv_erases;
static int r6ac_lv_shown;
uint32_t r6_ac_lv_count(int draw)
{
    r6ac_lv_shown = draw;
    if (draw) return ++r6ac_lv_draws;
    return ++r6ac_lv_erases;
}

/* ---- the log (r6tl_ast): copies only ---- */
/* pre, then the n values comma-separated (r6tl_i: own digits, INT32_MIN for AC_NONE) */
static void r6ac_ints(struct r6tl_out * o, const char * pre, const int32_t * v, int n)
{
    for (int i = 0; i < n; i++) r6tl_i(o, i ? "," : pre, v[i]);
}

void r6_ac_ast(struct r6tl_out * o)
{
    struct r6ac_v v;
    char t[64];
    int32_t a[24];
    r6ac_look(&v);
    const struct r6ac_res * r = &v.r;
    const struct r6ac_stat * s = &r->st;
    const struct ac_night * x = &r->n;
    const struct ac_now * w = &r->w;
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, sc = 0, wd = 0;
    ac_civil(v.utc, 0, &Y, &M, &D, &h, &mi, &sc, &wd);
    a[0] = r6ac_lat; a[1] = r6ac_lon; a[2] = r6ac_tzsrc; a[3] = r6ac_utcq; a[4] = r6ac_view; a[5] = r6ac_mwgc;
    a[6] = r6ac_mwsun; a[7] = r6ac_lv;
    r6ac_ints(o, "ac.cfg=", a, 8);
    a[0] = r->z.valid_ac; a[1] = r->z.valid; a[2] = r->z.zok; a[3] = r->z.zi; a[4] = r->z.zone; a[5] = r->z.dst; a[6] = r->z.off;
    a[7] = r->z.total; a[8] = r->off; a[9] = r->z.fail; a[10] = s->fails_row; a[11] = r->pending;
    r6ac_ints(o, "\nac.tz=", a, 12);
    a[0] = v.got ? r->state : -1; a[1] = Y; a[2] = M; a[3] = D; a[4] = h; a[5] = mi; a[6] = sc; a[7] = v.utc; a[8] = s->pair_age;
    a[9] = (int32_t) s->back_jumps;
    r6ac_ints(o, "\nac.utc=", a, 10);
    a[0] = s->n_night; a[1] = s->n_now; a[2] = s->calc_ms; a[3] = s->us_last; a[4] = s->us_max; a[5] = s->us_now_max;
    a[6] = s->us_chunk_max; a[7] = s->chunks; a[8] = s->aborts; a[9] = s->key_changes; a[10] = s->debounced;
    r6ac_ints(o, "\nac.calc=", a, 11);
    a[0] = r->night_ok ? x->t0 : AC_NONE;
    for (int k = 0; k < AC_NH; k++) { a[1 + k] = x->sun_dn[k]; a[6 + k] = x->sun_up[k]; }
    r6ac_ints(o, "\nac.night=", a, 11);
    r6tl_pf(o, ",%d%d%d%d%d", x->sun_state[0], x->sun_state[1], x->sun_state[2], x->sun_state[3], x->sun_state[4]);
    a[0] = x->sun_min_e1; a[1] = x->moon_rise; a[2] = x->moon_set; a[3] = x->moon_state; a[4] = x->mw_from; a[5] = x->mw_to;
    a[6] = x->mw_more; a[7] = x->mw_moon; a[8] = x->gc_max_e1;
    r6ac_ints(o, ",", a, 9);
    a[0] = r->now_ok ? w->t : AC_NONE; a[1] = w->sun_alt_e1; a[2] = w->moon_alt_e1; a[3] = w->gc_alt_e1; a[4] = w->moon_pm;
    a[5] = w->waxing; a[6] = w->prev_new; a[7] = w->next_new; a[8] = w->next_full; a[9] = w->pol_ha_min;
    for (int k = 0; k < 3; k++) a[10 + k] = w->pol_clock_min[k];
    r6ac_ints(o, "\nac.now=", a, 13);
    if (!r6ac_lv_fmt(t, sizeof(t), 0, &v)) t[0] = 0;
    a[0] = r6ac_lv_shown; a[1] = r6ac_lv_draws; a[2] = r6ac_lv_erases; a[3] = r6ac_lv_shown ? 2 : 0;
    r6ac_ints(o, "\nac.lv=", a, 4);
    r6tl_pf(o, ",%s\n", t);
}

/* ---- menu (Display > Astro clock) ---- */
#define R6AC_NA() do { MENU_SET_VALUE("--"); return; } while (0)

static MENU_UPDATE_FUNC(r6ac_head_update)
{
    struct r6ac_v v;
    char s[24];
    r6ac_look(&v);
    if (v.night)
    {
        r6ac_run(s, sizeof(s), &v, AC_H_ASTRO);
        MENU_SET_VALUE("Night %s", s);
    }
    else MENU_SET_VALUE("%s", v.got && v.r.state == R6AC_TZ_BAD ? "time zone?" : r6ac_state_text(&v));
    if (r6ac_lv && !get_global_draw_setting()) MENU_SET_WARNING(MENU_WARN_ADVICE, "Global Draw is OFF: no line in LV.");
}

/* the advice on Tonight / Now / Polar scope / Status */
static void r6ac_advice(struct menu_entry * entry, struct menu_display_info * info, const struct r6ac_v * v)
{
    if (v->got && v->r.state == R6AC_TZ_BAD) MENU_SET_WARNING(MENU_WARN_ADVICE, "Time zone unreadable: use Manual.");
}

static MENU_UPDATE_FUNC(r6ac_sub_update)
{
    struct r6ac_v v;
    r6ac_look(&v);
    r6ac_advice(entry, info, &v);
}

static MENU_UPDATE_FUNC(r6ac_nightof_update)
{
    struct r6ac_v v;
    char a[16], b[16];
    r6ac_look(&v);
    if (!v.night) R6AC_NA();
    MENU_SET_VALUE("%s - %s", r6ac_day(a, sizeof(a), v.r.n.t0, v.r.nk.off), r6ac_day(b, sizeof(b), v.r.n.t0 + 86400, v.r.nk.off));
}

static MENU_UPDATE_FUNC(r6ac_sun_update)
{
    struct r6ac_v v;
    char a[8], b[8];
    r6ac_look(&v);
    if (!v.night) R6AC_NA();
    const struct ac_night * x = &v.r.n;
    const int32_t o = v.r.nk.off;
    switch (x->sun_state[AC_H_RISE])
    {
        case AC_CROSS:       MENU_SET_VALUE("set %s  rise %s", r6ac_hhmm(a, 8, x->sun_dn[0], o), r6ac_hhmm(b, 8, x->sun_up[0], o)); break;
        case AC_FROM_START:  MENU_SET_VALUE("down at noon, rise %s", r6ac_hhmm(b, 8, x->sun_up[0], o)); break;
        case AC_PARTIAL:     MENU_SET_VALUE("set %s, no rise", r6ac_hhmm(a, 8, x->sun_dn[0], o)); break;
        case AC_ALWAYS_DOWN: MENU_SET_VALUE("down all day"); break;
        default:             MENU_SET_VALUE("up all night"); break;
    }
}

static MENU_UPDATE_FUNC(r6ac_twl_update)
{
    struct r6ac_v v;
    char s[48];
    r6ac_look(&v);
    if (!v.night) R6AC_NA();
    int k = !strcmp(entry->name, "Civil twilight") ? AC_H_CIVIL : !strcmp(entry->name, "Nautical") ? AC_H_NAUT : AC_H_ASTRO;
    r6ac_run(s, sizeof(s), &v, k);
    MENU_SET_VALUE("%s", s);
}

static MENU_UPDATE_FUNC(r6ac_moonn_update)
{
    struct r6ac_v v;
    char a[8], b[8];
    r6ac_look(&v);
    if (!v.night) R6AC_NA();
    const struct ac_night * x = &v.r.n;
    const int32_t o = v.r.nk.off;
    switch (x->moon_state)
    {
        case AC_M_BOTH:
            if (x->moon_rise < x->moon_set) MENU_SET_VALUE("rise %s  set %s", r6ac_hhmm(a, 8, x->moon_rise, o), r6ac_hhmm(b, 8, x->moon_set, o));
            else MENU_SET_VALUE("set %s  rise %s", r6ac_hhmm(a, 8, x->moon_set, o), r6ac_hhmm(b, 8, x->moon_rise, o));
            break;
        case AC_M_NORISE: MENU_SET_VALUE("no rise, set %s", r6ac_hhmm(b, 8, x->moon_set, o)); break;
        case AC_M_NOSET:  MENU_SET_VALUE("rise %s, no set", r6ac_hhmm(a, 8, x->moon_rise, o)); break;
        case AC_M_UP:     MENU_SET_VALUE("up all night"); break;
        default:          MENU_SET_VALUE("down all night"); break;
    }
}

static MENU_UPDATE_FUNC(r6ac_mw_update)
{
    struct r6ac_v v;
    char s[64];
    r6ac_look(&v);
    if (!v.night) R6AC_NA();
    r6ac_mw(s, sizeof(s), &v);
    MENU_SET_VALUE("%s", s);
    if (v.r.n.mw_state && v.r.n.mw_moon) MENU_SET_WARNING(MENU_WARN_INFO, "Moon up in the window (%d %s).", v.r.n.mw_moon_pct, "%");
}

static MENU_UPDATE_FUNC(r6ac_local_update)
{
    struct r6ac_v v;
    int Y, M, D, h, mi, sc, wd;
    char u[8];
    r6ac_look(&v);
    if (!v.got || !v.r.local_s || (v.r.state != R6AC_OK && v.r.state != R6AC_COMPUTING && v.r.state != R6AC_TZ_BAD
        && v.r.state != R6AC_PENDING)) R6AC_NA();
    ac_civil(v.loc, 0, &Y, &M, &D, &h, &mi, &sc, &wd);
    if (v.ok) MENU_SET_VALUE("%02d:%02d:%02d (UTC %s)", h, mi, sc, r6ac_hhmm(u, 8, v.utc - 30, 0));
    else MENU_SET_VALUE("%02d:%02d:%02d (UTC --:--)", h, mi, sc);
}

static MENU_UPDATE_FUNC(r6ac_alt_update)
{
    struct r6ac_v v;
    char c[12];
    r6ac_look(&v);
    if (!v.now) R6AC_NA();
    int g = !strcmp(entry->name, "Galactic centre");
    MENU_SET_VALUE("%s%s" SYM_DEGREE, g ? "alt " : "", r6ac_fix1(c, 12, g ? v.r.w.gc_alt_e1 : v.r.w.sun_alt_e1));
}

static MENU_UPDATE_FUNC(r6ac_moonw_update)
{
    struct r6ac_v v;
    char c[12];
    r6ac_look(&v);
    if (!v.now) R6AC_NA();
    MENU_SET_VALUE("alt %s" SYM_DEGREE ", %d%s %s", r6ac_fix1(c, 12, v.r.w.moon_alt_e1), (v.r.w.moon_pm + 5) / 10, "%",
                   v.r.w.waxing ? "waxing" : "waning");
}

static MENU_UPDATE_FUNC(r6ac_age_update)
{
    struct r6ac_v v;
    char c[12];
    int32_t r;
    r6ac_look(&v);
    if (!v.now || v.r.w.prev_new == AC_NONE || v.utc < v.r.w.prev_new) R6AC_NA();
    int d10 = r6ac_dm(v.utc - v.r.w.prev_new, 8640, &r);
    MENU_SET_VALUE("%s days", r6ac_fix1(c, 12, d10));
}

static MENU_UPDATE_FUNC(r6ac_phase_update)
{
    struct r6ac_v v;
    char a[16], b[8];
    r6ac_look(&v);
    if (!v.now) R6AC_NA();
    int32_t t = !strcmp(entry->name, "Next full moon") ? v.r.w.next_full : v.r.w.next_new;
    if (t == AC_NONE) R6AC_NA();
    MENU_SET_VALUE("%s %s", r6ac_day(a, sizeof(a), t + 30, v.r.nk.off), r6ac_hhmm(b, 8, t, v.r.nk.off));
}

static MENU_UPDATE_FUNC(r6ac_ha_update)
{
    struct r6ac_v v;
    int32_t r;
    r6ac_look(&v);
    r6ac_advice(entry, info, &v);
    if (!v.now) R6AC_NA();
    if (v.r.nk.loc.lat_e2 < 0) { MENU_SET_VALUE("below horizon (south)"); return; }
    int h = r6ac_dm(v.r.w.pol_ha_min, 60, &r);
    MENU_SET_VALUE("%02d:%02d", h, (int) r);
}

static MENU_UPDATE_FUNC(r6ac_pclock_update)
{
    struct r6ac_v v;
    char s[12];
    r6ac_look(&v);
    if (!v.now) R6AC_NA();
    if (v.r.nk.loc.lat_e2 < 0) { MENU_SET_VALUE("below horizon (south)"); return; }
    MENU_SET_VALUE("%s o'clock", r6ac_dial(s, sizeof(s), r6ac_pclock(&v)));
}

static MENU_UPDATE_FUNC(r6ac_lat_update)
{
    char s[16];
    MENU_SET_VALUE("%s", r6ac_deg2(s, sizeof(s), COERCE(r6ac_lat, -9000, 9000), "N", "S"));
}

static MENU_UPDATE_FUNC(r6ac_lon_update)
{
    char s[16];
    MENU_SET_VALUE("%s", r6ac_deg2(s, sizeof(s), COERCE(r6ac_lon, -18000, 18000), "E", "W"));
}

static MENU_UPDATE_FUNC(r6ac_tz_update)
{
    struct r6ac_v v;
    char a[12], b[16];
    r6ac_look(&v);
    if (COERCE(r6ac_tzsrc, 0, 1)) MENU_SET_VALUE("Manual: %s", r6ac_offs(a, sizeof(a), 15 * COERCE(r6ac_utcq, -48, 56)));
    /* the zone in use (the last valid read, kept after one failed read), not the raw read of the log (review R1 F3) */
    else if (v.got && v.r.zvalid) MENU_SET_VALUE("Canon: %s", r6ac_offs(a, sizeof(a), v.r.zu_total));
    else MENU_SET_VALUE("Canon: unreadable");
    if (v.got && v.r.eu)
        MENU_SET_WARNING(MENU_WARN_ADVICE, v.r.eu == 1 ? "Canon's DST is ON, EU summer time ended %s" : "Canon's DST is OFF, EU summer time began %s",
                         r6ac_day(b, sizeof(b), v.r.eu_date, 0) + 4);
    else if (v.got && v.r.zvalid)
        MENU_SET_WARNING(MENU_WARN_INFO, "Canon zone %d, DST %s: %d %s%d min.", v.r.zu_zone, v.r.zu_dst ? "on" : "off", v.r.zu_off,
                         v.r.zu_dst ? "+" : "", 60 * v.r.zu_dst);
}

static MENU_UPDATE_FUNC(r6ac_utcq_update)
{
    char a[12];
    MENU_SET_VALUE("%s", r6ac_offs(a, sizeof(a), 15 * COERCE(r6ac_utcq, -48, 56)));
    if (!COERCE(r6ac_tzsrc, 0, 1)) MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Used only with Time zone = Manual.");
}

static MENU_UPDATE_FUNC(r6ac_lv_update)
{
    if (r6ac_lv && r6_ettr_lv_wanted()) MENU_SET_WARNING(MENU_WARN_INFO, "R6 ETTR's line is shown instead (it has priority).");
}

static MENU_UPDATE_FUNC(r6ac_status_update)
{
    struct r6ac_v v;
    char a[8];
    r6ac_look(&v);
    r6ac_advice(entry, info, &v);
    if (v.got && v.r.state == R6AC_OK && v.r.night_ok)
        MENU_SET_VALUE("ok, computed %s (%d ms)", r6ac_hhmm(a, 8, v.r.utc_s - (int32_t) (v.r.tick_ms - v.r.st.calc_ms) / 1000, v.r.off),
                       (int) (v.r.st.us_last / 1000));
    else MENU_SET_VALUE("%s", r6ac_state_text(&v));
}

#define R6AC_INFO(nm, upd, h) { .name = nm, .update = upd, .icon_type = IT_ALWAYS_ON, .help = h }

static struct menu_entry r6ac_menu[] = {
    {
        .name   = "Astro clock",
        .select = menu_open_submenu,
        .update = r6ac_head_update,
        .help   = "Twilight, moon, Milky Way and Polaris for the location below. Read only.",
        .help2  = "M14.1: camera clock and Canon's time zone; location by hand (no GPS).",
        .children = (struct menu_entry[]) {
            {
                .name   = "Tonight",
                .select = menu_open_submenu,
                .update = r6ac_sub_update,
                .help   = "The night from local noon to the next noon.",
                .children = (struct menu_entry[]) {
                    R6AC_INFO("Night of", r6ac_nightof_update, "The night from local noon to the next noon."),
                    R6AC_INFO("Sun", r6ac_sun_update, "Rise and set at -0.833 deg (refraction, sun radius)."),
                    R6AC_INFO("Civil twilight", r6ac_twl_update, "Sun below -6 deg: from the end of evening to the start of morning civil twilight."),
                    R6AC_INFO("Nautical", r6ac_twl_update, "Sun below -12 deg."),
                    R6AC_INFO("Astro night", r6ac_twl_update, "Sun below -18 deg. None from end of May to mid July at 50 N."),
                    R6AC_INFO("Moon", r6ac_moonn_update, "Moon's upper limb at the horizon (USNO convention)."),
                    {
                        .name      = "Milky Way",
                        .update    = r6ac_mw_update,
                        .icon_type = IT_ALWAYS_ON,
                        .help      = "Galactic centre above / sun below the MW criteria. Dips under a few minutes may be missed.",
                    },
                    MENU_EOL
                },
            },
            {
                .name   = "Now",
                .select = menu_open_submenu,
                .update = r6ac_sub_update,
                .help   = "Sun, moon and galactic centre now.",
                .children = (struct menu_entry[]) {
                    {
                        .name      = "Local time",
                        .update    = r6ac_local_update,
                        .icon_type = IT_ALWAYS_ON,
                        .help      = "Camera clock (Canon RTC) and the UTC ML uses.",
                        .help2     = "Read once a minute: after setting the clock in Canon's menu ML follows within 1 min.",
                    },
                    R6AC_INFO("Sun altitude", r6ac_alt_update, "Geometric altitude, no refraction (all altitudes here)."),
                    R6AC_INFO("Moon", r6ac_moonw_update, "Topocentric altitude, illuminated part, waxing / waning."),
                    R6AC_INFO("Moon age", r6ac_age_update, "Since the last new moon (+- 1 h)."),
                    R6AC_INFO("Next full moon", r6ac_phase_update, "+- 1 h."),
                    R6AC_INFO("Next new moon", r6ac_phase_update, "+- 1 h."),
                    R6AC_INFO("Galactic centre", r6ac_alt_update, "Sgr A*, geometric altitude."),
                    MENU_EOL
                },
            },
            {
                .name   = "Polar scope",
                .select = menu_open_submenu,
                .update = r6ac_sub_update,
                .help   = "Polaris for the polar scope (northern sky).",
                .children = (struct menu_entry[]) {
                    R6AC_INFO("Polaris HA", r6ac_ha_update, "Hour angle of Polaris (mean place, +- 3 min)."),
                    R6AC_INFO("Polaris clock", r6ac_pclock_update, "Where Polaris sits on a 12-o'clock reticle (12 = up)."),
                    {
                        .name    = "Clock view",
                        .priv    = &r6ac_view,
                        .max     = 2,
                        .choices = CHOICES("Sky", "Scope 180" SYM_DEGREE, "Mirrored"),
                        .help    = "Sky: as seen by eye. Scope 180" SYM_DEGREE ": image rotated by 180 deg (most polar scopes).",
                        .help2   = "Mirrored: left-right flipped sky (mirror / diagonal). Saved.",
                    },
                    MENU_EOL
                },
            },
            {
                .name    = "Latitude",
                .priv    = &r6ac_lat,
                .min     = -9000,
                .max     = 9000,
                .unit    = UNIT_DEC,
                .update  = r6ac_lat_update,
                .help    = "Your location, 0.01 deg steps (N positive). No GPS. Saved.",
                .help2   = "While editing: the raw number in 0.01 deg. Past 90 N the editor wraps to S.",
            },
            {
                .name    = "Longitude",
                .priv    = &r6ac_lon,
                .min     = -18000,
                .max     = 18000,
                .unit    = UNIT_DEC,
                .update  = r6ac_lon_update,
                .help    = "Your location, 0.01 deg steps (E positive). No GPS. Saved.",
                .help2   = "While editing: the raw number in 0.01 deg.",
            },
            {
                .name    = "Time zone",
                .priv    = &r6ac_tzsrc,
                .max     = 1,
                .choices = CHOICES("Canon", "Manual"),
                .update  = r6ac_tz_update,
                .help    = "UTC = camera clock - Canon's zone and DST. Manual: your own offset below.",
                .help2   = "Canon never switches DST itself: set it in Canon's menu (zone, summer time).",
            },
            {
                .name    = "Manual offset",
                .priv    = &r6ac_utcq,
                .min     = -48,
                .max     = 56,
                .update  = r6ac_utcq_update,
                .help    = "Used only with Time zone = Manual. Steps of 15 min.",
            },
            {
                .name    = "MW: centre above",
                .priv    = &r6ac_mwgc,
                .max     = 2,
                .choices = CHOICES("5" SYM_DEGREE, "10" SYM_DEGREE, "15" SYM_DEGREE),
                .help    = "Lowest altitude of the galactic centre for the Milky Way window.",
            },
            {
                .name    = "MW: sun below",
                .priv    = &r6ac_mwsun,
                .max     = 2,
                .choices = CHOICES("-18" SYM_DEGREE, "-15" SYM_DEGREE, "-12" SYM_DEGREE),
                .help    = "Sun limit for the Milky Way window.",
            },
            {
                .name    = "Show in LV",
                .priv    = &r6ac_lv,
                .max     = 1,
                .depends_on = DEP_GLOBAL_DRAW,
                .update  = r6ac_lv_update,
                .help    = "One line on the LCD shooting screen (Global Draw). R6 ETTR's line has priority.",
                .help2   = "Saved.",
            },
            {
                .name      = "Status",
                .update    = r6ac_status_update,
                .icon_type = IT_ALWAYS_ON,
                .help      = "The clock's state and the last computation.",
            },
            MENU_EOL
        },
    },
};

static void r6ac_init()
{
    menu_add("Display", r6ac_menu, COUNT(r6ac_menu));
}

INIT_FUNC("r6_aclock", r6ac_init);

#endif /* CONFIG_R6_ACLOCK */
