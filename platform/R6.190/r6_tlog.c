/*
 * R6.190 M14.0: the Tor 0g read-only log (docs/m14_plan.md, spec m14/m140 spec_m140.md Rev. 2; user answers of 2026-10-15:
 * E1..E12 as recommended, E12 a log only, E8 without GPS). Tor 0g (approved 2026-10-15, read only, every pointer checked):
 *  - Canon's TempSvc table 0xFD9EC + 12 i (i = 0..7: DRAM, MIMIL, MIMIR, A, TFT, SH, IISA, IISB): +4 int16 t16 in 1/16 degC,
 *    +8 int32 pv = round(degC) + 128 (Canon: floor((t16 + 8) / 16) + 128), its copy 0xFDA80 (8 x int16), the error counters
 *    0xFDAB0 (8 x u32), the period [0x4824] (ms) and the over-temperature status [0x484C] (PROP_TEMP_STATUS);
 *  - the EXIF byte [[0x5480] + 0x50C4] (byte 0 = sensor A + 128), the pointer checked over its whole extent;
 *  - the time-zone struct q = [0x6F38]: +0x20 table length, +0x24 table pointer (len x 8 B {int32 offset min, int32 zone}),
 *    +0x28 DST 0 / 1, +0x2C free offset min, +0x30 zone index; the table only for 1 <= len <= 33 (Tor 0g: 33 x 8 B).
 * Every Canon load goes through r6tl_rd (aligned 32-bit, the whole extent inside one region of r6_fy_ram_ok: RAM, or the
 * uncached alias with a verified window); a pointer that fails is logged as invalid(...) and never dereferenced. No GPS.
 *
 * The task r6_tlog_task polls the 8 t16 and the status every second (one change counter per poll), keeps a ring of 32
 * samples, one per 10 s (sensor A, SH, IISA, IISB, status, polls with a change, polls), and once a minute (from its first
 * ring entry on) one RTC pair: LoadCalendarFromRTC (ML's wrapper, already called by the core: menu keys, clock, config
 * save) and ML's ms clock after it. That call waits for Canon's RTC task without a limit, so it stays in this task, never
 * in the snapshot or a drawing path. The snapshot (r6tl_ast from lp_capture) reads the table twice (equal: ok 1, up to 4
 * tries), the time-zone struct and table the same way, copies the task's state under a seqlock (no wait) and prints.
 *
 * Writes: only this file's own static r6tl_* variables, locals and the AST text buffer it is given. No Canon RAM, MMIO,
 * property or file write; no property handler; no menu entry, no setting. Canon calls: none new (msleep and vsnprintf
 * stubs, ML's LoadCalendarFromRTC wrapper).
 *
 * M14.1 (spec m14/m141 spec_m141.md Rev. 2, 2.1; nothing new at Canon): each poll also keeps its 8 t16, the read mask, the
 * status and its time in the task's state (same seqlock), and, with CONFIG_R6_TEMP, the warning state of r6_temp_poll
 * (ML's own arithmetic on ML's settings; this task is its only caller). Read-only exports for r6_temp.c / r6_aclock.c:
 * r6tl_now (the newest poll) and r6tl_rtc (the newest RTC pair), both seqlock copies without waiting, and r6tl_tz_now (the
 * time-zone struct through the snapshot's evaluation r6tl_tz_eval, with its own buffers r6tl_qa / r6tl_qb; only
 * the astro clock's step calls it). The snapshot ends with the tb.* / ac.* lines of r6_temp.c / r6_aclock.c (copies only),
 * printed with r6tl_pf / r6tl_i / r6tl_u.
 *
 * M15.0 (E-b, spec m15/m150 1; nothing new at Canon): the astro clock has no task of its own any more (its 4 KB stack go
 * back to Canon's task memory sys_mem). r6_tlog_task runs its step r6_ac_step after each poll; the loop body is r6tl_poll,
 * and while the clock computes a night, its chunk callback polls through r6tl_poll_due when a poll is due. Everything
 * stays in this one task: r6tl_seq and r6ac_seq keep one writer each, LoadCalendarFromRTC stays in r6tl_poll only.
 */
#include "dryos.h"
#include "tasks.h"
#include "propvalues.h"
#include <stdarg.h>
#include "r6_faceyuv.h"
#include "r6_tlog.h"
#ifdef CONFIG_R6_TEMP
#include "r6_temp.h"
#endif
#ifdef CONFIG_R6_ACLOCK
#include "r6_aclock.h"
#endif

#ifdef CONFIG_R6_TLOG
#ifndef CONFIG_R6_ASTRO
#error "r6_tlog.c: CONFIG_R6_TLOG needs CONFIG_R6_ASTRO (the section goes into ASTnnnnn.LOG)"
#endif

/* Canon RAM, FW 1.9.0 (spec m14/m140 1.1) */
#define R6TL_TAB      0xFD9EC   /* TempSvc table, 8 x 12 B */
#define R6TL_COPY     0xFDA80   /* the copy, 8 x int16 */
#define R6TL_ERR      0xFDAB0   /* error counters, 8 x u32 */
#define R6TL_PERIOD   0x4824    /* measuring period, ms */
#define R6TL_STATUS   0x484C    /* over-temperature status */
#define R6TL_FCS      0x5480    /* pointer p: [p + 0x50C4] byte 0 = sensor A + 128 */
#define R6TL_TZ       0x6F38    /* pointer q: the time-zone struct */

#define R6TL_POLL_MS  1000      /* the task's poll */
#define R6TL_RING_MS  10000     /* one ring entry per this */
#define R6TL_RTC_MS   60000     /* one RTC pair per this (the first with the first ring entry) */
#define R6TL_RING_N   32        /* ring entries (320 s) */
#define R6TL_SLEEP_MIN_MS 10    /* M15.0: the shortest sleep of the task's loop (after a long step of the astro clock) */
#define R6TL_TZ_MAX   33        /* the zone table is read only for 1 <= len <= this (Tor 0g: 33 x 8 B, Canon's table) */

/* the only Canon load of this file: aligned, off <= ext - 4 (off + 4 could wrap), [base, base + ext) inside one allowed region */
#define R6TL_RD(a) (*(volatile const uint32_t *) (a))
static int r6tl_rd(uint32_t base, uint32_t ext, uint32_t off, uint32_t * v)
{
    if ((off & 3) || ext < 4 || off > ext - 4 || !r6_fy_ram_ok(base, ext)) return 0;
    *v = R6TL_RD(base + off);
    return 1;
}

/* ---- the task's state: the ring, the counters since boot, the last RTC pair (writer r6tl_task, seqlock r6tl_seq) ---- */
struct r6tl_e { uint32_t t; int16_t a, sh, isa, isb; uint32_t st; uint8_t nchg, npoll, pad[2]; };   /* 20 B */
struct r6tl_s
{
    struct r6tl_e r[R6TL_RING_N];
    uint32_t n;                         /* ring entries since boot */
    int polls, pchg;                    /* polls, polls with a change of the 8 t16 */
    int gmax, gend, tchg;               /* the longest time without a change, its end, the last change */
    int pgmax, tpoll;                   /* the largest gap between two polls, the last poll */
    struct tm tm;                       /* the last RTC pair: Canon's calendar (zeroed before the call) ... */
    int rtc_t, rtc_dur, rtc_n, rtc_max; /* ... ML's ms clock after it, the call's duration, count, longest */
    /* M14.1: the newest poll (spec m14/m141 2.1) */
    int16_t cur[8];                     /* t16 of the 8 sensors */
    uint8_t cur_ok, cur_st_ok;          /* bit i: sensor i read; the status read */
    int8_t cur_warn, cur_hyst;          /* CONFIG_R6_TEMP: r6_temp_poll's warning and threshold state of this poll */
    uint32_t cur_st;                    /* the status [0x484C] */
    int cur_t;                          /* ML's ms clock of the poll */
};
static struct r6tl_s r6tl_s;            /* written by r6tl_task only */
static struct r6tl_s r6tl_cp;           /* the snapshot's copy (r6tl_ast only; lp_snap_busy: one reader at a time) */
static volatile uint32_t r6tl_seq = 0;  /* odd while r6tl_task writes r6tl_s */
static int16_t r6tl_last[8];            /* the task's previous poll */
/* M15.0 (E-b): the poll's own state, touched only in r6_tlog_task (up to M14.1c the locals of the task's loop): the last ring
 * entry, the last RTC pair, the polls and the polls with a change in the ring window; the start of the last poll (the loop's
 * deadline, r6tl_poll_due); 1 while r6tl_poll_due polls (no nesting) */
static int t_ring, t_rtc, win_n, win_chg;
static int r6tl_t_poll, r6tl_in_poll;

/* one poll: M14.0's loop body. M15.0 (E-b): from r6_tlog_task's loop and, while the astro clock computes there (r6_ac_step),
 * from its chunk callback through r6tl_poll_due - always this task, so r6tl_seq keeps its one writer */
static void r6tl_poll(void)
{
    int now = get_ms_clock();
    int16_t t[8];
    uint32_t st = 0;
    int okm = 0;
    r6tl_t_poll = now;
    for (int i = 0; i < 8; i++)
    {
        uint32_t w = 0;
        okm |= r6tl_rd(R6TL_TAB, 0x60, 12 * i + 4, &w) << i;
        t[i] = (int16_t) (w & 0xFFFF);
    }
    int st_ok = r6tl_rd(R6TL_STATUS, 4, 0, &st);
    int warn = 0, hyst = 0;
#ifdef CONFIG_R6_TEMP
    warn = r6_temp_poll(t[3], (okm >> 3) & 1, st, st_ok, &hyst);   /* M14.1: the warning state, ML's own arithmetic */
#endif
    int chg = r6tl_s.polls && memcmp(t, r6tl_last, sizeof(t));
    memcpy(r6tl_last, t, sizeof(t));
    win_n += win_n < 255;
    win_chg += chg && win_chg < 255;
    int ring = now - t_ring >= R6TL_RING_MS;
    r6tl_seq++;
    __sync_synchronize();
    if (r6tl_s.polls) r6tl_s.pgmax = MAX(r6tl_s.pgmax, now - r6tl_s.tpoll);
    else r6tl_s.tchg = now;
    r6tl_s.tpoll = now;
    r6tl_s.polls++;
    memcpy(r6tl_s.cur, t, sizeof(t));
    r6tl_s.cur_ok = okm;
    r6tl_s.cur_st = st;
    r6tl_s.cur_st_ok = st_ok;
    r6tl_s.cur_warn = warn;
    r6tl_s.cur_hyst = hyst;
    r6tl_s.cur_t = now;
    if (chg)
    {
        r6tl_s.pchg++;
        if (now - r6tl_s.tchg > r6tl_s.gmax) { r6tl_s.gmax = now - r6tl_s.tchg; r6tl_s.gend = now; }
        r6tl_s.tchg = now;
    }
    if (ring)
    {
        struct r6tl_e * e = &r6tl_s.r[r6tl_s.n % R6TL_RING_N];
        e->t = now; e->a = t[3]; e->sh = t[5]; e->isa = t[6]; e->isb = t[7]; e->st = st;
        e->nchg = win_chg; e->npoll = win_n;
        r6tl_s.n++;
        t_ring = now;
        win_n = 0;
        win_chg = 0;
    }
    __sync_synchronize();
    r6tl_seq++;
    if (ring && (!r6tl_s.rtc_n || now - t_rtc >= R6TL_RTC_MS))
    {
        /* the RTC pair: outside the seqlock (the call waits for Canon's RTC task), stored under it */
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        int t0 = get_ms_clock();
        LoadCalendarFromRTC(&tm);
        int t1 = get_ms_clock();
        t_rtc = now;
        r6tl_seq++;
        __sync_synchronize();
        r6tl_s.tm = tm;
        r6tl_s.rtc_t = t1;
        r6tl_s.rtc_dur = t1 - t0;
        r6tl_s.rtc_n++;
        r6tl_s.rtc_max = MAX(r6tl_s.rtc_max, t1 - t0);
        __sync_synchronize();
        r6tl_seq++;
    }
}

/* M15.0 (E-b): a poll if R6TL_POLL_MS passed since the start of the last one. Only from the astro clock's chunk callback
 * r6ac_yield, which runs in r6_tlog_task (r6_ac_step): the temperature and the RTC pair keep their pace during a night */
void r6tl_poll_due(void)
{
    if (r6tl_in_poll || get_ms_clock() - r6tl_t_poll < R6TL_POLL_MS) return;
    r6tl_in_poll = 1;
    r6tl_poll();
    r6tl_in_poll = 0;
}

static void r6tl_task(void * unused)
{
    t_ring = r6tl_t_poll = get_ms_clock();
    TASK_LOOP
    {
        /* M15.0: after a long step of the clock (R6TL_SLEEP_MIN_MS or more since the start of the last poll) the next poll comes
         * R6TL_POLL_MS after that start; otherwise the plain R6TL_POLL_MS sleep of M14.0 (the same pace and phase as before) */
        int w = R6TL_POLL_MS - (get_ms_clock() - r6tl_t_poll);
        msleep(w > R6TL_POLL_MS - R6TL_SLEEP_MIN_MS ? R6TL_POLL_MS : COERCE(w, R6TL_SLEEP_MIN_MS, R6TL_POLL_MS));
        r6tl_poll();
#ifdef CONFIG_R6_ACLOCK
        r6_ac_step();                   /* M15.0 (E-b): the astro clock, only here (r6_aclock.c; no task of its own) */
#endif
    }
}

TASK_CREATE("r6_tlog_task", r6tl_task, 0, 0x1f, 0x1000);

/* M14.1: the newest poll, a copy under the seqlock (no wait, at most 4 tries); 1 = consistent and at least one poll */
int r6tl_now(struct r6tl_now * o)
{
    for (int k = 0; k < 4; k++)
    {
        uint32_t s1 = r6tl_seq;
        __sync_synchronize();
        memcpy(o->t16, r6tl_s.cur, sizeof(o->t16));
        o->ok = r6tl_s.cur_ok;
        o->st_ok = r6tl_s.cur_st_ok;
        o->st = r6tl_s.cur_st;
        o->warn = r6tl_s.cur_warn;
        o->hyst = r6tl_s.cur_hyst;
        o->t = r6tl_s.cur_t;
        o->polls = r6tl_s.polls;
        __sync_synchronize();
        uint32_t s2 = r6tl_seq;
        if (s1 == s2 && !(s1 & 1)) return o->polls > 0;
    }
    return 0;
}

/* M14.1: the newest RTC pair (Canon's calendar, ML's ms clock after the call, the count); 1 = consistent and one pair */
int r6tl_rtc(struct tm * tm, int * t_ms, int * n)
{
    for (int k = 0; k < 4; k++)
    {
        uint32_t s1 = r6tl_seq;
        __sync_synchronize();
        *tm = r6tl_s.tm;
        *t_ms = r6tl_s.rtc_t;
        *n = r6tl_s.rtc_n;
        __sync_synchronize();
        uint32_t s2 = r6tl_seq;
        if (s1 == s2 && !(s1 & 1)) return *n > 0;
    }
    return 0;
}

/* ---- the snapshot section (spec 4) ---- */
/* as r6as_pf, but vsnprintf gets the room minus 1: Canon's vsnprintf writes up to n characters and its NUL at [n] (QEMU m140
 * T17: one guard byte behind a 2 / 100 B room changed), so the text never reaches past cap - 1 */
void r6tl_pf(struct r6tl_out * o, const char * fmt, ...)
{
    if (o->len >= o->cap - 1) { o->trunc = 1; return; }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o->p + o->len, o->cap - o->len - 1, fmt, ap);
    va_end(ap);
    o->p[o->cap - 1] = 0;
    o->len += strlen(o->p + o->len);
    if (o->len >= o->cap - 2) o->trunc = 1;
}

/* the decimal digits of m, at least min of them, into s; returns their count. Canon's vsnprintf prints neither %u nor INT_MIN
 * and no %0Nd wider than N (QEMU m140 T1 / T2 / T7), so every number from Canon RAM or the task's state goes through here;
 * no division (a division by 10 would add the constant 0xCCCCCCCD) */
static const uint32_t r6tl_p10[10] = { 1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1 };
static int r6tl_dig(char * s, uint32_t m, int min)
{
    int k = 0;
    for (int i = 0; i < 10; i++)
    {
        int d = 0;
        while (m >= r6tl_p10[i]) { m -= r6tl_p10[i]; d++; }
        if (d || k || 10 - i <= min) s[k++] = '0' + d;
    }
    s[k] = 0;
    return k;
}
/* pre, then a signed (r6tl_i) / unsigned (r6tl_u) 32-bit number in decimal */
static void r6tl_n(struct r6tl_out * o, const char * pre, int neg, uint32_t m)
{
    char s[12];
    s[0] = '-';
    r6tl_dig(s + neg, m, 1);
    r6tl_pf(o, "%s%s", pre, s);
}
void r6tl_i(struct r6tl_out * o, const char * pre, int32_t v) { r6tl_n(o, pre, v < 0, v < 0 ? 0u - (uint32_t) v : (uint32_t) v); }
void r6tl_u(struct r6tl_out * o, const char * pre, uint32_t v) { r6tl_n(o, pre, 0, v); }

/* t16 (1/16 degC) as degC with one decimal: tenths = sign * floor((|5 t16| + 4) / 8), half away from zero, never "-0.0";
 * the tenths with at least 2 digits, the point put before the last one */
const char * r6tl_c(char * s, int t16)
{
    int x = 5 * t16, z = ((x < 0 ? -x : x) + 4) / 8;
    int k = x < 0 && z;
    s[0] = '-';
    int n = k + r6tl_dig(s + k, z, 2);
    s[n + 1] = 0;
    s[n] = s[n - 1];
    s[n - 1] = '.';
    return s;
}

/* the table, its copy and the error counters as 36 words (24 + 4 + 8) */
struct r6tl_tv { uint32_t w[36]; };
static struct r6tl_tv r6tl_ta, r6tl_tb;
static void r6tl_tab(struct r6tl_tv * t)
{
    for (int i = 0; i < 36; i++)
    {
        uint32_t v = 0;
        if (i < 24) r6tl_rd(R6TL_TAB, 0x60, 4 * i, &v);
        else if (i < 28) r6tl_rd(R6TL_COPY, 0x10, 4 * (i - 24), &v);
        else r6tl_rd(R6TL_ERR, 0x20, 4 * (i - 28), &v);
        t->w[i] = v;
    }
}

/* the time-zone struct (+0x20 .. +0x30: len, table, DST, free offset, zone) and, for 1 <= len <= 33, its table */
struct r6tl_zv { uint32_t f[5]; int tab; uint32_t t[2 * R6TL_TZ_MAX]; };
static struct r6tl_zv r6tl_za, r6tl_zb;     /* the snapshot's (r6tl_ast only) */
static struct r6tl_zv r6tl_qa, r6tl_qb;     /* M14.1: r6tl_tz_now's (the clock's step only; skeptic H2) */
static void r6tl_tz(uint32_t q, struct r6tl_zv * z)
{
    for (int k = 0; k < 5; k++)
    {
        uint32_t v = 0;
        r6tl_rd(q, 0x34, 0x20 + 4 * k, &v);
        z->f[k] = v;
    }
    uint32_t len = z->f[0], tb = z->f[1], v = 0;
    z->tab = 1 <= len && len <= R6TL_TZ_MAX && r6tl_rd(tb, len * 8, 0, &v);
    for (int k = 0; k < 2 * R6TL_TZ_MAX; k++)
    {
        v = 0;
        if (z->tab && k < (int) (2 * len)) r6tl_rd(tb, len * 8, 4 * k, &v);
        z->t[k] = v;
    }
}

/* M14.1: the evaluation of the time-zone struct, shared by the snapshot and r6tl_tz_now with the caller's buffers: q checked
 * over [q, q + 0x34), struct and table twice (equal = ok, at most 4 tries), Canon's search (the first match), the free
 * offset for a zone not in the table, the range and DST 0 / 1 (the M14.0 rules); 0 = q invalid */
struct r6tl_ze { int ok, tries, tab, len, dst, fr, zone, zi, off, valid; };
static int r6tl_tz_eval(uint32_t q, struct r6tl_zv * za, struct r6tl_zv * zb, struct r6tl_ze * e)
{
    uint32_t v = 0;
    if (!r6tl_rd(q, 0x34, 0x20, &v)) return 0;
    int zt = 0, zok = 0;
    while (!zok && zt < 4)
    {
        zt++;
        r6tl_tz(q, za);
        r6tl_tz(q, zb);
        zok = !memcmp(za, zb, sizeof(*za));
    }
    const struct r6tl_zv * z = zb;
    int len = (int) z->f[0], dst = (int) z->f[2], fr = (int) z->f[3], zone = (int) z->f[4];
    int zi = -1;
    if (z->tab)
        for (int k = 0; k < len && zi < 0; k++) if ((int) z->t[2 * k + 1] == zone) zi = k;   /* Canon's search: the first match */
    int off = zi >= 0 ? (int) z->t[2 * zi] : fr;
    int valid = z->tab && off >= -1439 && off <= 1440 && (dst == 0 || dst == 1);
    e->ok = zok; e->tries = zt; e->tab = z->tab; e->len = len; e->dst = dst; e->fr = fr; e->zone = zone; e->zi = zi;
    e->off = off; e->valid = valid;
    return 1;
}

/* M14.1: the time-zone struct now, for the clock's step only (r6_ac_step in r6_tlog_task since M15.0; never a menu update, a
 * drawing path or the snapshot): its own
 * buffers; valid_ac = valid and read twice equal and the zone found in Canon's table (skeptic M3); 1 = valid_ac */
int r6tl_tz_now(struct r6tl_tz * o)
{
    uint32_t q = 0;
    struct r6tl_ze e;
    memset(o, 0, sizeof(*o));
    o->zi = -1;
    r6tl_rd(R6TL_TZ, 4, 0, &q);
    if (!r6tl_tz_eval(q, &r6tl_qa, &r6tl_qb, &e)) { o->fail = R6TL_TZF_PTR; return 0; }
    o->zone = e.zone; o->dst = e.dst; o->off = e.off; o->zi = e.zi; o->zok = e.ok ? 1 : -1; o->tries = e.tries;
    o->valid = e.valid;
    o->total = e.valid ? e.off + 60 * e.dst : 0;
    o->valid_ac = e.valid && e.ok && e.zi >= 0;
    o->fail = o->valid_ac ? 0 : !e.tab ? R6TL_TZF_TAB : !e.ok ? R6TL_TZF_TORN : e.zi < 0 ? R6TL_TZF_NOZONE
            : !(e.off >= -1439 && e.off <= 1440) ? R6TL_TZF_RANGE : R6TL_TZF_DST;
    return o->valid_ac;
}

/* tp.upd, tp.ring and the ring lines of the task's state s (lines < 0: torn, no ring line) */
static void r6tl_st_text(struct r6tl_out * o, const struct r6tl_s * s, int lines, int tries)
{
    char c0[12];
    if (lines < 0) r6tl_pf(o, "tp.upd=torn\n");
    else
    {
        r6tl_i(o, "tp.upd=", s->polls);
        r6tl_i(o, ",", s->pchg);
        r6tl_i(o, ",", s->gmax);
        r6tl_i(o, ",", s->gend);
        r6tl_i(o, ",", s->polls ? (int32_t) ((uint32_t) get_ms_clock() - (uint32_t) s->tchg) : 0);
        r6tl_i(o, ",", s->pgmax);
        r6tl_pf(o, "\n");
    }
    r6tl_u(o, "tp.ring=", s->n);
    r6tl_pf(o, ",%d,%d,%d,%d\n", lines, tries, R6TL_POLL_MS, R6TL_RING_MS);
    r6tl_pf(o, "# tp.r[i]=t,A16,AdegC,SH16,IISA16,IISB16,status,nchg,npoll (oldest first)\n");
    for (int j = 0; j < lines; j++)
    {
        const struct r6tl_e * e = &s->r[(s->n - lines + j) % R6TL_RING_N];
        r6tl_pf(o, "tp.r[%d]=", j);
        r6tl_i(o, "", (int32_t) e->t);
        r6tl_pf(o, ",%d,%s,%d,%d,%d,0x%08x,%d,%d\n", e->a, r6tl_c(c0, e->a), e->sh, e->isa, e->isb, (int) e->st, e->nchg, e->npoll);
    }
}

static const char * const r6tl_nm[8] = { "DRAM", "MIMIL", "MIMIR", "A", "TFT", "SH", "IISA", "IISB" };

int r6tl_ast(char * p, int cap, int * trunc)
{
    struct r6tl_out o = { .p = p, .len = 0, .cap = cap, .trunc = 0 };
    char c0[12];
    uint32_t v = 0;
    if (!p || cap < 2) { if (trunc) *trunc = 1; return 0; }
    p[0] = 0;
    r6tl_pf(&o, "# tp/tz M14.0 Tor 0g (spec m14/m140 4): TempSvc 0xFD9EC+12*i: t16 +4 int16 1/16 degC, pv +8 = round(degC)+128; copy 0xFDA80, err 0xFDAB0\n");
    /* the table, twice: equal = ok 1 (at most 4 tries; else ok -1 and the last read) */
    int tries = 0, ok = 0;
    while (!ok && tries < 4)
    {
        tries++;
        r6tl_tab(&r6tl_ta);
        r6tl_tab(&r6tl_tb);
        ok = !memcmp(&r6tl_ta, &r6tl_tb, sizeof(r6tl_ta));
    }
    const uint32_t * w = r6tl_tb.w;
    r6tl_pf(&o, "tp.read=%d,%d,%d\n", ok ? 1 : -1, tries, get_ms_clock());
    r6tl_pf(&o, "tp.raw=");
    for (int i = 0; i < 24; i++) r6tl_pf(&o, "%s0x%08x", i ? "," : "", (int) w[i]);
    r6tl_pf(&o, "\n# tp.s[i]=name,t16,degC,pv,pv-128,copy16,err\n");
    for (int i = 0; i < 8; i++)
    {
        int t16 = (int16_t) (w[3 * i + 1] & 0xFFFF), cp = (int16_t) ((w[24 + i / 2] >> (16 * (i & 1))) & 0xFFFF);
        r6tl_pf(&o, "tp.s[%d]=%s,%d,%s", i, r6tl_nm[i], t16, r6tl_c(c0, t16));
        r6tl_i(&o, ",", (int32_t) w[3 * i + 2]);
        r6tl_i(&o, ",", (int32_t) (w[3 * i + 2] - 128u));
        r6tl_pf(&o, ",%d", cp);
        r6tl_u(&o, ",", w[28 + i]);
        r6tl_pf(&o, "\n");
    }
    /* status and period (one aligned word each) */
    uint32_t st = 0, per = 0;
    if (r6tl_rd(R6TL_STATUS, 4, 0, &st) && r6tl_rd(R6TL_PERIOD, 4, 0, &per))
    {
        r6tl_pf(&o, "tp.st=0x%08x", (int) st);
        r6tl_i(&o, ",", (int32_t) per);
        r6tl_pf(&o, "\n");
    }
    else r6tl_pf(&o, "tp.st=invalid\n");
    /* the EXIF byte: p = [0x5480], checked over [p, p + 0x50C8) */
    uint32_t fp = 0;
    r6tl_rd(R6TL_FCS, 4, 0, &fp);
    if (r6tl_rd(fp, 0x50C8, 0x50C4, &v)) r6tl_pf(&o, "tp.fcs=0x%08x,0x%08x,%d,%d\n", (int) fp, (int) v, (int) (v & 0xFF), (int) (v & 0xFF) - 128);
    else r6tl_pf(&o, "tp.fcs=invalid(0x%08x)\n", (int) fp);
    r6tl_i(&o, "tp.efic=", efic_temp);
    r6tl_pf(&o, "\n");
    /* the task's state: a copy under the seqlock (no wait; at most 4 tries), printed from the copy */
    int ctries = 0, cok = 0;
    while (!cok && ctries < 4)
    {
        ctries++;
        uint32_t s1 = r6tl_seq;
        __sync_synchronize();
        memcpy(&r6tl_cp, &r6tl_s, sizeof(r6tl_cp));
        __sync_synchronize();
        uint32_t s2 = r6tl_seq;
        cok = s1 == s2 && !(s1 & 1);
    }
    const struct r6tl_s * s = &r6tl_cp;
    r6tl_st_text(&o, s, cok ? (int) MIN(s->n, R6TL_RING_N) : -1, ctries);
    /* the time-zone struct: q = [0x6F38], checked over [q, q + 0x34); struct and table twice (at most 4 tries) */
    uint32_t q = 0;
    struct r6tl_ze ze;
    r6tl_rd(R6TL_TZ, 4, 0, &q);
    if (!r6tl_tz_eval(q, &r6tl_za, &r6tl_zb, &ze)) r6tl_pf(&o, "tz.p=invalid(0x%08x)\n", (int) q);
    else
    {
        const struct r6tl_zv * z = &r6tl_zb;
        int len = ze.len, dst = ze.dst, fr = ze.fr, zone = ze.zone, zi = ze.zi, off = ze.off, valid = ze.valid;
        r6tl_pf(&o, "tz.p=0x%08x,%d,%d\n", (int) q, ze.ok ? 1 : -1, ze.tries);
        r6tl_i(&o, "tz.f=", len);
        r6tl_pf(&o, ",0x%08x", (int) z->f[1]);
        r6tl_i(&o, ",", dst);
        r6tl_i(&o, ",", fr);
        r6tl_i(&o, ",", zone);
        r6tl_pf(&o, "\n");
        if (!z->tab)
        {
            r6tl_pf(&o, "tz.tab=invalid(0x%08x", (int) z->f[1]);
            r6tl_i(&o, ",", len);
            r6tl_pf(&o, ")\n");
        }
        else
        {
            for (int k = 0; k < len; k++)
            {
                r6tl_i(&o, k ? "," : "tz.tab=", (int32_t) z->t[2 * k]);
                r6tl_i(&o, ",", (int32_t) z->t[2 * k + 1]);
            }
            r6tl_pf(&o, "\n");
        }
        r6tl_pf(&o, "tz.sel=%d", zi);
        r6tl_i(&o, ",", off);
        r6tl_i(&o, ",", valid ? off + 60 * dst : 0);
        r6tl_pf(&o, ",%d\n", valid);
    }
    /* the last RTC pair (Canon's calendar as ML's struct tm: year + 1900, month + 1), from the copy */
    if (!cok) r6tl_pf(&o, "tz.rtc=torn\n");
    else if (!s->rtc_n) r6tl_pf(&o, "tz.rtc=none\n");
    else
    {
        r6tl_i(&o, "tz.rtc=", (int32_t) ((uint32_t) s->tm.tm_year + 1900u));
        r6tl_i(&o, ",", (int32_t) ((uint32_t) s->tm.tm_mon + 1u));
        r6tl_i(&o, ",", s->tm.tm_mday);
        r6tl_i(&o, ",", s->tm.tm_hour);
        r6tl_i(&o, ",", s->tm.tm_min);
        r6tl_i(&o, ",", s->tm.tm_sec);
        r6tl_i(&o, ",", s->tm.tm_wday);
        r6tl_i(&o, ",", s->tm.tm_yday);
        r6tl_i(&o, ",", s->tm.tm_isdst);
        r6tl_i(&o, ",", (int32_t) s->tm.tm_gmtoff);
        r6tl_i(&o, ",", s->rtc_t);
        r6tl_i(&o, ",", s->rtc_dur);
        r6tl_i(&o, ",", s->rtc_n);
        r6tl_i(&o, ",", s->rtc_max);
        r6tl_pf(&o, "\n");
    }
#if defined(CONFIG_R6_TEMP) || defined(CONFIG_R6_ACLOCK)
    /* M14.1: the tb / ac lines of r6_temp.c / r6_aclock.c (copies only: no computation, no RTC, no wait; spec m14/m141 8) */
    r6tl_pf(&o, "# tb/ac M14.1 (spec m14/m141 8)\n");
#endif
#ifdef CONFIG_R6_TEMP
    r6_temp_ast(&o);
#endif
#ifdef CONFIG_R6_ACLOCK
    r6_ac_ast(&o);
#endif
    if (trunc) *trunc = o.trunc;
    return o.len;
}

#endif /* CONFIG_R6_TLOG */
