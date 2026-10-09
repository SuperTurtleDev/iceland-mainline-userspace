// SPDX-License-Identifier: MIT
/*
 * fastcharged -- profile-driven USB PD fast-charge daemon for the SM8850
 * (OnePlus Pad 4 / iceland), driving the charge_boost_lite kernel module.
 *
 * Two operating modes, picked from the adapter contract:
 *
 *   PPS  (adapter pd_type == 8, PD 3.0 APDO): precise control following
 *        the official OPLUS third-party strategy (vendor/oplus/kernel/
 *        charger/oplus_pps.c of the SM8850 OSS drop):
 *          vbus_ask = round100(vbat * 4 + 450 mV)     (4:1 charge pump)
 *          clamped  to [5500 .. 10500] mV             (PPS_VOL_CURVE_LMAX,
 *                                                     PPS_3RD_ASK_VOLT_MAX)
 *        ramped towards the target in <= 500 mV steps and re-requested
 *        every poll (PPS keepalive; spec limit 10 s). Adapter current
 *        comes from the profile (<= 3000 mA).
 *
 *   PDO  (adapter pd_type == 6, fixed PDOs): request 5000/9000/12000 mV
 *        with the profile current.
 *
 * Profile (/etc/fastcharge/fastcharge.conf, one rule per line):
 *
 *     soc_lo soc_hi temp_lo temp_hi pdo_mv icl_ma
 *
 *   soc_lo/soc_hi   battery SoC window [lo, hi) in percent; per temperature
 *                   band the rules must tile [0, 100] exactly
 *   temp_lo/temp_hi battery temperature window [lo, hi) in degrees C;
 *                   "-" and "+" mean unbounded (outermost rules)
 *   pdo_mv          fixed-PDO voltage 5000/9000/12000 (PDO mode; in PPS
 *                   mode this column only gates boosting: 5000 = profile
 *                   current only at the adapter floor)
 *   icl_ma          adapter current limit, 1..3000 (both modes)
 *
 * The default profile is laid out on the official grid: temperature bands
 * 0/5/12/20/35/44/51 C (oplus_pps rang_temp_tmp) and SoC breakpoints
 * 15/30/50/75/85/95 (soc_tmp); warm-band charging only below 50 % SoC
 * mirrors pps_warm_allow_soc = 50.
 *
 * Safety properties (enforced in code, independent of the file):
 *   * hard caps: 12000 mV / 3000 mA, PPS ask within [5500, 10500] mV
 *   * the SoC x temperature domain must be covered exactly once; gaps or
 *     overlaps abort the daemon -- charging then stays at the 5 V baseline
 *   * lower-power rules apply immediately; stepping up in power needs the
 *     new rule to hold for STABLE_TICKS consecutive polls
 *   * any surprise (charger gone, vbus out of range, temp unreadable,
 *     status != Charging) falls back to the 5 V baseline
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- invariants ---------------------------------------------------------- */
#define HARD_MAX_MV     12000       /* user cap: 12 V */
#define HARD_MAX_MA     3000        /* user cap: 3 A */
#define RENEGOTIATE_UA  500000      /* ICL during any renegotiation */
#define VBUS_OK_UV      7500000     /* vendor threshold for a boosted PDO */
#define VBUS_MIN_UV     4500000     /* sanity window while boosted */
#define VBUS_MAX_UV     12500000
#define BASELINE_UA     2000000     /* module base_icl default */
#define MAX_RULES       256
#define STABLE_TICKS    2           /* polls before stepping up in power */
#define MAX_PD_WAIT_S   15          /* seconds to wait for the PD contract */

/* conservative third-party PPS constants (oplus_pps.c/h, reverted from the
 * official-head values by decision: the 10.5V ask cap keeps PPS engaging
 * on every adapter -- an ask above an adapter's APDO maximum is rejected
 * and drops vbus back to 5V) */
#define PPS_MIN_MV      5500        /* PPS_VOL_CURVE_LMAX */
#define PPS_MAX_MV      10500       /* PPS_3RD_ASK_VOLT_MAX */
#define PPS_VBAT_GAIN   4           /* 4:1 charge pump (SC mode) */
#define PPS_VBAT_OFF_MV 450         /* PPS_ACTION_START_DIFF_VOLT_3RD */
#define PPS_STEP_MV     500         /* ramp step towards the target */
#define PPS_DEADBAND_MV 100         /* re-aim only when target moved this far */

static const char *PARM_DIR = "/sys/module/charge_boost_lite/parameters";
static const char *PS_USB   = "/sys/class/power_supply/qcom-battmgr-usb";
static const char *PS_BAT   = "/sys/class/power_supply/qcom-battmgr-bat";

static const char *opt_config  = "/etc/fastcharge/fastcharge.conf";
static const char *opt_usb     = NULL;   /* override PS_USB */
static const char *opt_bat     = NULL;   /* override PS_BAT */
static int  opt_interval = 5;            /* seconds (< PPS 10 s keepalive) */
static int  opt_verbose  = 0;
static int  opt_test     = 0;            /* parse + validate + dump, then exit */

struct rule {
    int soc_lo, soc_hi;       /* percent, soc_lo <= x < soc_hi */
    int t_lo, t_hi;           /* deci-degrees C, t_lo <= x < t_hi; INT_MIN/MAX for -/+ */
    int mv, ma;               /* pdo_mv (PDO mode), icl_ma (both modes) */
};

static struct rule rules[MAX_RULES];
static int nrules;

static int soc_in(const struct rule *r, int soc);
static int rule_matches(const struct rule *r, int soc, int t_dc);

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

/* ---- small helpers -------------------------------------------------------- */
static void ts_log(const char *fmt, ...)
{
    va_list ap;
    char stamp[32];
    time_t t = time(NULL);
    struct tm tm;
    va_start(ap, fmt);
    localtime_r(&t, &tm);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    fprintf(stderr, "fastcharged[%ld]: %s ", (long)getpid(), stamp);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

#define vlog(...) do { if (opt_verbose) ts_log(__VA_ARGS__); } while (0)

static int read_file_int(const char *path, long *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int rc = fscanf(f, "%ld", out) == 1 ? 0 : -1;
    fclose(f);
    return rc;
}

static int read_file_str(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, len, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static int write_file_int(const char *path, long val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld", val);
    ssize_t w = write(fd, buf, n);
    close(fd);
    return w == n ? 0 : -1;
}

static long clamp_long(long v, long lo, long hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- profile parsing + validation ------------------------------------------ */
static int parse_bound(const char *tok, int *out)
{
    if (!strcmp(tok, "-")) { *out = INT_MIN; return 0; }
    if (!strcmp(tok, "+")) { *out = INT_MAX; return 0; }
    char *end;
    errno = 0;
    long v = strtol(tok, &end, 10);
    if (errno || *end || end == tok || v < -1000 || v > 1000)
        return -1;
    *out = (int)v * 10;     /* file is degrees C, rules are deci-degrees */
    return 0;
}

static int parse_token(const char *tok, long lo, long hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(tok, &end, 10);
    if (errno || *end || end == tok || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

static int load_profile(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ts_log("cannot open profile %s: %s", path, strerror(errno));
        return -1;
    }
    char line[512];
    int lineno = 0;
    nrules = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *s = strchr(line, '#');
        if (s)
            *s = '\0';
        char *tok[6], *save = NULL;
        int ntok = 0;
        for (char *p = strtok_r(line, " \t\r\n", &save); p && ntok < 6;
             p = strtok_r(NULL, " \t\r\n", &save))
            tok[ntok++] = p;
        if (ntok == 0)
            continue;
        if (ntok != 6) {
            ts_log("%s:%d: expected 6 fields, got %d", path, lineno, ntok);
            goto bad;
        }
        if (nrules >= MAX_RULES) {
            ts_log("%s: too many rules (max %d)", path, MAX_RULES);
            goto bad;
        }
        struct rule *r = &rules[nrules];
        if (parse_token(tok[0], 0, 100, &r->soc_lo) ||
            parse_token(tok[1], 0, 100, &r->soc_hi) ||
            parse_bound(tok[2], &r->t_lo) ||
            parse_bound(tok[3], &r->t_hi) ||
            parse_token(tok[4], 5000, HARD_MAX_MV, &r->mv) ||
            parse_token(tok[5], 1, HARD_MAX_MA, &r->ma)) {
            ts_log("%s:%d: bad field (caps: pdo 5000..%d mV, icl 1..%d mA)",
                   path, lineno, HARD_MAX_MV, HARD_MAX_MA);
            goto bad;
        }
        if (r->mv != 5000 && r->mv != 9000 && r->mv != 12000) {
            ts_log("%s:%d: pdo_mv must be 5000/9000/12000 (fixed PDO)", path, lineno);
            goto bad;
        }
        if (r->soc_lo >= r->soc_hi || r->t_lo >= r->t_hi) {
            ts_log("%s:%d: empty range", path, lineno);
            goto bad;
        }
        nrules++;
    }
    fclose(f);
    if (!nrules) {
        ts_log("%s: no rules", path);
        return -1;
    }
    return 0;
bad:
    fclose(f);
    return -1;
}

/*
 * Coverage: sample the whole domain. Every (soc, temp) must match exactly
 * one rule -- gaps would leave charging unmanaged and overlaps would make
 * the choice ambiguous, both are fatal.
 */
static int validate_coverage(void)
{
    for (int t = -400; t <= 850; t += 5) {          /* deci-C, 0.5 C steps */
        for (int soc = 0; soc <= 100; soc++) {
            int hits = 0;
            for (int i = 0; i < nrules; i++)
                if (rule_matches(&rules[i], soc, t))
                    hits++;
            if (hits != 1) {
                ts_log("coverage error at soc=%d%% temp=%d.%dC: %d rules match",
                       soc, t / 10, t % 10, hits);
                return -1;
            }
        }
    }
    return 0;
}

static void dump_profile(void)
{
    for (int i = 0; i < nrules; i++) {
        const struct rule *r = &rules[i];
        char lo[16], hi[16];
        if (r->t_lo == INT_MIN) snprintf(lo, sizeof(lo), "-inf");
        else snprintf(lo, sizeof(lo), "%d.%d", r->t_lo / 10, abs(r->t_lo % 10));
        if (r->t_hi == INT_MAX) snprintf(hi, sizeof(hi), "+inf");
        else snprintf(hi, sizeof(hi), "%d.%d", r->t_hi / 10, abs(r->t_hi % 10));
        printf("soc [%3d,%3d) temp [%6s,%6s) -> %5d mV %4d mA\n",
               r->soc_lo, r->soc_hi, lo, hi, r->mv, r->ma);
    }
}

/* soc window [lo, hi); the top band (hi == 100) is closed so soc 100 % is
 * covered -- the battery reporting "100" is still charging (topping off) */
static int soc_in(const struct rule *r, int soc)
{
    if (soc < r->soc_lo)
        return 0;
    if (r->soc_hi == 100)
        return soc <= 100;
    return soc < r->soc_hi;
}

static int rule_matches(const struct rule *r, int soc, int t_dc)
{
    return soc_in(r, soc) && t_dc >= r->t_lo && t_dc < r->t_hi;
}

/* ---- rule selection with hysteresis ---------------------------------------- */
static const struct rule *pick_rule(int soc, int temp_dc)
{
    for (int i = 0; i < nrules; i++)
        if (rule_matches(&rules[i], soc, temp_dc))
            return &rules[i];
    return NULL;    /* cannot happen after validation, handled defensively */
}

static long power_rank(const struct rule *r)
{
    return (long)r->mv * 10000 + r->ma;   /* voltage first, then current */
}

/* ---- charge_boost_lite interaction ----------------------------------------- */
static char pbuf[128], upath[160], bpath[160];

/* vbus_now can briefly read garbage right after events (observed 47000 uV
 * during boot-with-cable); retry and require a plausible value */
static int read_vbus_valid(long *out)
{
    long v = 0;
    for (int i = 0; i < 3; i++) {
        if (read_file_int(upath, &v) == 0 && v >= 4200000 && v <= 13000000) {
            *out = v;
            return 0;
        }
        usleep(400 * 1000);
    }
    *out = v;
    return -1;
}

static int module_ready(void)
{
    struct stat st;
    return stat(pbuf, &st) == 0 && S_ISDIR(st.st_mode);
}

static int modprobe_module(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execl("/sbin/modprobe", "modprobe", "charge_boost_lite", "apply=0",
              (char *)NULL);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* negotiated vbus check for a PDO request */
static int pdo_reached(int mv, long vbus_uv)
{
    if (mv == 5000)
        return vbus_uv <= 5500000;
    return vbus_uv >= VBUS_OK_UV;
}

/* PPS operating point check, same window as the reference pd-boost.sh */
static int pps_reached(int mv, long vbus_uv)
{
    return vbus_uv >= (long)(mv - 1500) * 1000 &&
           vbus_uv <= (long)(mv + 1000) * 1000;
}

static void parm_path(char *out, size_t len, const char *parm)
{
    snprintf(out, len, "%s/%s", pbuf, parm);
}

/* fixed-PDO application: ICL drop -> PDO -> verify -> ICL raise */
static int apply_pdo(const struct rule *r)
{
    char path[256];
    long icl_ua = (long)r->ma * 1000;

    parm_path(path, sizeof(path), "curr_uv");
    if (write_file_int(path, RENEGOTIATE_UA)) {
        ts_log("cannot drop ICL (%s)", strerror(errno));
        return -1;
    }
    sleep(1);

    parm_path(path, sizeof(path), "pdo_mv");
    if (write_file_int(path, r->mv)) {
        ts_log("cannot request PDO %d (%s)", r->mv, strerror(errno));
        return -1;
    }
    sleep(2);

    long vbus = 0;
    int ok = read_vbus_valid(&vbus) == 0 && pdo_reached(r->mv, vbus);
    if (!ok) {   /* one retry, mirroring the reference sequence */
        write_file_int(path, r->mv);
        sleep(2);
        ok = read_vbus_valid(&vbus) == 0 && pdo_reached(r->mv, vbus);
    }
    if (!ok) {
        ts_log("PDO %d did not engage (vbus=%ld uV)", r->mv, vbus);
        return -1;
    }

    parm_path(path, sizeof(path), "curr_uv");
    if (write_file_int(path, icl_ua)) {
        ts_log("cannot raise ICL to %ld uA (%s)", icl_ua, strerror(errno));
        return -1;
    }
    ts_log("PDO engaged: %d mV / %d mA (vbus=%ld uV)", r->mv, r->ma, vbus);
    return 0;
}

/* PPS entry/renegotiation: ICL drop -> APDO point -> verify -> ICL raise */
static int apply_pps_point(int mv, int ma)
{
    char path[256];
    long vbus = 0;

    parm_path(path, sizeof(path), "curr_uv");
    write_file_int(path, RENEGOTIATE_UA);
    sleep(1);

    parm_path(path, sizeof(path), "pps_mv");
    if (write_file_int(path, mv)) {
        ts_log("cannot request PPS %d mV (%s)", mv, strerror(errno));
        return -1;
    }
    parm_path(path, sizeof(path), "pps_ma");
    if (write_file_int(path, ma)) {
        ts_log("cannot request PPS %d mA (%s)", ma, strerror(errno));
        return -1;
    }
    sleep(2);

    int ok = read_vbus_valid(&vbus) == 0 && pps_reached(mv, vbus);
    if (!ok) {   /* one retry */
        parm_path(path, sizeof(path), "pps_mv");
        write_file_int(path, mv);
        parm_path(path, sizeof(path), "pps_ma");
        write_file_int(path, ma);
        sleep(2);
        ok = read_vbus_valid(&vbus) == 0 && pps_reached(mv, vbus);
    }
    if (!ok) {
        ts_log("PPS %d mV did not engage (vbus=%ld uV)", mv, vbus);
        return -1;
    }

    parm_path(path, sizeof(path), "curr_uv");
    write_file_int(path, (long)ma * 1000);
    ts_log("PPS engaged: %d mV / %d mA (vbus=%ld uV)", mv, ma, vbus);
    return 0;
}

/* PPS keepalive: re-request the operating point (spec: within 10 s) */
static void pps_keepalive(int mv, int ma)
{
    char path[256];
    parm_path(path, sizeof(path), "pps_mv");
    if (write_file_int(path, mv))
        return;                 /* module gone/unloaded; main loop handles */
    parm_path(path, sizeof(path), "pps_ma");
    write_file_int(path, ma);
}

static int request_baseline(void)
{
    char path[256];
    parm_path(path, sizeof(path), "pdo_mv");
    write_file_int(path, 5000);
    parm_path(path, sizeof(path), "curr_uv");
    write_file_int(path, BASELINE_UA);
    return 0;
}

/* ---- main loop ---------------------------------------------------------------- */
enum mode { MODE_NONE = 0, MODE_PDO, MODE_PPS };

static const struct rule *cur_rule;      /* currently applied rule, NULL = baseline */
static const struct rule *cand_rule;     /* candidate for stepping up in power */
static int cand_ticks;
static int apply_fails;                  /* consecutive failed applies -> re-detect */
static int drop_count;                   /* charger-gone events in a row -> backoff */
static enum mode cur_mode = MODE_NONE;
static int pps_ask_mv;                   /* current PPS operating point */

/* official formula: vbus = round100(vbat*4 + 450), clamped to [PPS_MIN, PPS_MAX] */
static int pps_target_mv(long vbat_uv)
{
    long mv = (vbat_uv / 1000) * PPS_VBAT_GAIN + PPS_VBAT_OFF_MV;
    mv = mv / 100 * 100;     /* PPS 100 mV landing grid */
    return (int)clamp_long(mv, PPS_MIN_MV, PPS_MAX_MV);
}

int main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "c:u:b:i:tvh")) != -1) {
        switch (c) {
        case 'c': opt_config = optarg; break;
        case 'u': opt_usb = optarg; break;
        case 'b': opt_bat = optarg; break;
        case 'i': opt_interval = atoi(optarg); break;
        case 't': opt_test = 1; break;
        case 'v': opt_verbose = 1; break;
        default:
            fprintf(stderr,
                "usage: fastcharged [-c profile] [-u usbpsy] [-b batpsy]"
                " [-i interval_s] [-t] [-v]\n"
                "  -t  validate profile and dump the rule table, then exit\n");
            return c == 'h' ? 0 : 2;
        }
    }
    if (opt_interval < 1)
        opt_interval = 1;

    snprintf(pbuf, sizeof(pbuf), "%s", PARM_DIR);
    snprintf(upath, sizeof(upath), "%s/voltage_now", opt_usb ? opt_usb : PS_USB);
    snprintf(bpath, sizeof(bpath), "%s", opt_bat ? opt_bat : PS_BAT);

    if (load_profile(opt_config) || validate_coverage()) {
        ts_log("invalid profile, refusing to touch charging (5 V baseline)");
        return 1;
    }
    ts_log("profile ok: %d rules, caps %d mV / %d mA, PPS %d..%d mV",
           nrules, HARD_MAX_MV, HARD_MAX_MA, PPS_MIN_MV, PPS_MAX_MV);
    if (opt_test) {
        dump_profile();
        return 0;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    if (!module_ready() && modprobe_module() != 0)
        ts_log("modprobe charge_boost_lite failed; will keep retrying");
    int pd_seen = 0, pd_waited = 0;
    enum mode mode = MODE_NONE;

    while (running) {
        char q[192], sbuf[64];
        long online = 0, soc = 0, temp = 0, vbus = 0, vbat = 0;

        snprintf(q, sizeof(q), "%s/online", opt_usb ? opt_usb : PS_USB);
        /* "online" (not vbus>3V) is the presence gate on purpose: it is
         * role-aware -- in OTG/host mode the tablet drives its own 5V VBUS
         * for peripherals and online stays 0, while a bare vbus threshold
         * would false-positive and hammer the ADSP with PD writes in host
         * role. vbus only feeds the sanity window below */
        int have_online = read_file_int(q, &online) == 0 && online == 1;

        if (!have_online) {
            if (cur_rule) {
                ts_log("charger gone -> 5 V baseline");
                request_baseline();
                cur_rule = NULL;
                cur_mode = MODE_NONE;
            }
            pd_seen = pd_waited = 0;
            /* exponential backoff: after 3 rapid dropouts start waiting
             * 30s between re-engagement attempts to avoid hammering the
             * ADSP/adapter into the PPS error state (observed as result
             * 512 after ~28 rapid cycles) */
            drop_count++;
            int wait_s = opt_interval;
            if (drop_count >= 3)
                wait_s = 30;
            if (drop_count >= 6)
                wait_s = 120;
            if (drop_count >= 10) {
                ts_log("%d charger dropouts -- backing off to 5 min", drop_count);
                wait_s = 300;
            }
            sleep(wait_s);
            continue;
        }
        /* charger is back: reset the dropout counter */
        drop_count = 0;

        if (!module_ready() && modprobe_module() != 0) {
            sleep(opt_interval);
            continue;
        }

        /* wait for a PD contract: 6 = fixed-PD, 8 = PD_PPS. The module is
         * loaded with apply=0, so nothing probes on plug events -- poke the
         * write-only "check" parameter each round to (re)run detection,
         * exactly like the reference pd-boost.sh does */
        if (!pd_seen) {
            parm_path(q, sizeof(q), "check");
            write_file_int(q, 1);
            parm_path(q, sizeof(q), "pd_type");
            long pdt = 0;
            if (read_file_int(q, &pdt) == 0 && (pdt == 6 || pdt == 8)) {
                pd_seen = 1;
                pd_waited = 0;
                mode = (pdt == 8) ? MODE_PPS : MODE_PDO;
                cur_mode = MODE_NONE;
                ts_log("PD contract up (type %ld, %s mode)", pdt,
                       mode == MODE_PPS ? "PPS" : "fixed PDO");
            } else if (++pd_waited >= MAX_PD_WAIT_S) {
                vlog("no PD contract, passive");
                sleep(opt_interval);
                continue;
            } else {
                sleep(1);
                continue;
            }
        }

        /* gates + measurements. NOTE: battery status is deliberately NOT a
         * gate: at boot-with-cable the 5V baseline input can be below the
         * system load, the battery net-drains and reports "Discharging" --
         * gating on it deadlocks exactly when raising input power is the
         * fix. status is only logged */
        snprintf(q, sizeof(q), "%s/status", bpath);
        int have_status = read_file_str(q, sbuf, sizeof(sbuf)) == 0;
        snprintf(q, sizeof(q), "%s/capacity", bpath);
        int have_soc = read_file_int(q, &soc) == 0;
        snprintf(q, sizeof(q), "%s/temp", bpath);
        int have_temp = read_file_int(q, &temp) == 0;
        snprintf(q, sizeof(q), "%s/voltage_now", bpath);
        int have_vbat = read_file_int(q, &vbat) == 0;
        int have_vbus = read_file_int(upath, &vbus) == 0;

        if (!have_soc || !have_temp || !have_vbus ||
            (mode == MODE_PPS && !have_vbat) ||
            vbus < VBUS_MIN_UV || vbus > VBUS_MAX_UV) {
            if (cur_rule) {
                ts_log("gate trip (status=%s soc=%ld temp=%ld vbus=%ld)"
                       " -> 5 V baseline", have_status ? sbuf : "?",
                       soc, temp, vbus);
                request_baseline();
                cur_rule = NULL;
                cur_mode = MODE_NONE;
            }
            sleep(opt_interval);
            continue;
        }

        const struct rule *want = pick_rule((int)clamp_long(soc, 0, 100), (int)temp);
        if (!want) {    /* defensive: validation guarantees coverage */
            if (cur_rule) { request_baseline(); cur_rule = NULL; cur_mode = MODE_NONE; }
            sleep(opt_interval);
            continue;
        }

        /* PPS: recompute the operating point every poll and keep alive */
        if (mode == MODE_PPS && cur_rule && want == cur_rule) {
            int target = pps_target_mv(vbat);
            if (abs(target - pps_ask_mv) >= PPS_DEADBAND_MV) {
                /* ramp in <= PPS_STEP_MV steps */
                int step = target - pps_ask_mv;
                if (step > PPS_STEP_MV)
                    step = PPS_STEP_MV;
                if (step < -PPS_STEP_MV)
                    step = -PPS_STEP_MV;
                pps_ask_mv += step;
                pps_keepalive(pps_ask_mv, cur_rule->ma);
                vlog("PPS ramp -> %d mV (target %d, vbat %ld uV)",
                     pps_ask_mv, target, vbat);
            } else {
                pps_keepalive(pps_ask_mv, cur_rule->ma);
            }
            sleep(opt_interval);
            continue;
        }

        /* rule transition handling */
        if (cur_rule && want == cur_rule) {
            cand_rule = NULL;
            cand_ticks = 0;
        } else if (!cur_rule || power_rank(want) <= power_rank(cur_rule)) {
            /* first apply or stepping down: immediately */
            int rc;
            if (mode == MODE_PPS) {
                pps_ask_mv = pps_target_mv(vbat);
                rc = apply_pps_point(pps_ask_mv, want->ma);
            } else {
                rc = apply_pdo(want);
            }
            if (rc == 0) {
                apply_fails = 0;
                cur_rule = want;
                cur_mode = mode;
            } else {
                request_baseline();
                cur_rule = NULL;
                cur_mode = MODE_NONE;
                if (++apply_fails >= 3) {
                    /* the ADSP contract state is stale (e.g. garbage vbus
                     * reads right after boot-with-cable): fall back to the
                     * detection loop instead of waiting for a replug */
                    ts_log("3 failed applies -> re-running PD detection");
                    pd_seen = 0;
                    pd_waited = 0;
                    apply_fails = 0;
                }
            }
            cand_rule = NULL;
            cand_ticks = 0;
        } else {
            /* stepping up in power: demand stability */
            if (cand_rule == want) {
                if (++cand_ticks >= STABLE_TICKS) {
                    int rc;
                    if (mode == MODE_PPS) {
                        pps_ask_mv = pps_target_mv(vbat);
                        rc = apply_pps_point(pps_ask_mv, want->ma);
                    } else {
                        rc = apply_pdo(want);
                    }
                    if (rc == 0) {
                        apply_fails = 0;
                        cur_rule = want;
                        cur_mode = mode;
                    } else {
                        request_baseline();
                        cur_rule = NULL;
                        cur_mode = MODE_NONE;
                        if (++apply_fails >= 3) {
                            ts_log("3 failed applies -> re-running PD detection");
                            pd_seen = 0;
                            pd_waited = 0;
                            apply_fails = 0;
                        }
                    }
                    cand_rule = NULL;
                    cand_ticks = 0;
                }
            } else {
                cand_rule = want;
                cand_ticks = 1;
            }
        }

        sleep(opt_interval);
    }

    if (cur_rule) {
        ts_log("shutdown -> 5 V baseline");
        request_baseline();
    }
    return 0;
}
