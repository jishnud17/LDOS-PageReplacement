/*
 * page_stats.c - Per-Page Statistics Collection
 * 
 * Hash table for tracking per-page access statistics.
 * These statistics serve as ML features for migration decisions.
 * 
 * LDOS Research Project, UT Austin
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <inttypes.h>
#include "tiered_memory.h"

/*============================================================================
 * UTILITIES
 *===========================================================================*/

uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void* page_align(void *addr) {
    return (void*)((uintptr_t)addr & ~(PAGE_SIZE - 1));
}

static inline size_t hash_page_addr(void *addr) {
    uintptr_t page_num = (uintptr_t)addr >> 12;
    const uint64_t golden = 0x9E3779B97F4A7C15ULL;
    return (size_t)((page_num * golden) % PAGE_STATS_HASH_SIZE);
}

/*============================================================================
 * PAGE STATISTICS MANAGEMENT
 *===========================================================================*/

page_stats_t* get_page_stats(void *page_addr) {
    void *aligned = page_align(page_addr);
    size_t bucket = hash_page_addr(aligned);
    
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    page_stats_t *entry = g_manager.page_stats_table[bucket];
    while (entry != NULL) {
        if (entry->page_addr == aligned) {
            pthread_rwlock_unlock(&g_manager.stats_lock);
            return entry;
        }
        entry = entry->next;
    }
    pthread_rwlock_unlock(&g_manager.stats_lock);
    return NULL;
}

page_stats_t* get_or_create_page_stats(void *page_addr) {
    void *aligned = page_align(page_addr);
    size_t bucket = hash_page_addr(aligned);
    
    /* Try read-only lookup first */
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    page_stats_t *entry = g_manager.page_stats_table[bucket];
    while (entry != NULL) {
        if (entry->page_addr == aligned) {
            pthread_rwlock_unlock(&g_manager.stats_lock);
            return entry;
        }
        entry = entry->next;
    }
    pthread_rwlock_unlock(&g_manager.stats_lock);
    
    /* Create new entry */
    pthread_rwlock_wrlock(&g_manager.stats_lock);
    
    /* Double-check after acquiring write lock */
    entry = g_manager.page_stats_table[bucket];
    while (entry != NULL) {
        if (entry->page_addr == aligned) {
            pthread_rwlock_unlock(&g_manager.stats_lock);
            return entry;
        }
        entry = entry->next;
    }
    
    entry = (page_stats_t*)calloc(1, sizeof(page_stats_t));
    if (entry == NULL) {
        TM_ERROR("Failed to allocate page_stats_t");
        pthread_rwlock_unlock(&g_manager.stats_lock);
        return NULL;
    }
    
    uint64_t now = get_time_ns();
    entry->page_addr = aligned;
    entry->first_access_ns = now;
    atomic_store(&entry->last_access_ns, now);
    entry->allocation_ns = now;
    entry->current_tier = TIER_UNKNOWN;
    
    entry->next = g_manager.page_stats_table[bucket];
    g_manager.page_stats_table[bucket] = entry;
    atomic_fetch_add(&g_manager.total_pages_tracked, 1);
    
    pthread_rwlock_unlock(&g_manager.stats_lock);
    return entry;
}

void record_page_access(void *page_addr, bool is_write) {
    page_stats_t *stats = get_or_create_page_stats(page_addr);
    if (stats == NULL) return;
    
    atomic_fetch_add(&stats->access_count, 1);
    if (is_write) {
        atomic_fetch_add(&stats->write_count, 1);
    } else {
        atomic_fetch_add(&stats->read_count, 1);
    }
    atomic_store(&stats->last_access_ns, get_time_ns());
}

/*============================================================================
 * FEATURE COMPUTATION
 *===========================================================================*/

void compute_page_features(page_stats_t *stats) {
    uint64_t now = get_time_ns();
    uint64_t access_count = atomic_load(&stats->access_count);
    uint64_t last_access = atomic_load(&stats->last_access_ns);
    
    /* Access rate (accesses per second) */
    uint64_t lifetime_ns = now - stats->allocation_ns;
    if (lifetime_ns > 0) {
        stats->access_rate = (double)access_count * 1e9 / (double)lifetime_ns;
    }
    
    /* Heat score using exponential decay (~10 second half-life) */
    double decay_seconds = (double)(now - last_access) / 1e9;
    double recency_factor = exp(-0.07 * decay_seconds);
    double frequency_factor = fmin(stats->access_rate / 1000.0, 1.0);
    
    stats->heat_score = 0.6 * recency_factor + 0.4 * frequency_factor;
    stats->heat_score = fmax(0.0, fmin(1.0, stats->heat_score));
}

void update_all_page_features(void) {
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    for (size_t i = 0; i < PAGE_STATS_HASH_SIZE; i++) {
        page_stats_t *entry = g_manager.page_stats_table[i];
        while (entry != NULL) {
            compute_page_features(entry);
            entry = entry->next;
        }
    }
    pthread_rwlock_unlock(&g_manager.stats_lock);
}

/*============================================================================
 * PREDICTIVE SIGNALS (phase-1 ML feature discovery)
 *
 * 19 technical-analysis-style signals computed per page over a rolling window
 * of "interval access-rate" bars.  One bar = accesses/sec measured over the
 * last SIGNAL_SAMPLE_MS (50 ms); SIGNAL_WINDOW (64) bars are retained.
 *
 * Single-series adaptation: classic OHLC indicators are applied to a series
 * where High = Low = Close = the bar's access rate.  True Range therefore
 * reduces to |Δrate| and directional movement to the signed bar-to-bar change.
 *
 * Window-lookback signals clamp their lookback to the bars available so far,
 * so early rows carry meaningful (shorter-horizon) values rather than zeros.
 * Recursive signals (TRIX, STC, PSAR, Supertrend, ASI) warm up from their
 * first bar.  Any genuinely undefined value is reported as 0.0, never NaN.
 *===========================================================================*/

#define SIG_EPS 1e-9

/* Ordered-window helpers operate on v[0..n-1] with v[n-1] the most recent bar. */
static double sig_max(const double *v, int start, int n) {
    double m = v[start];
    for (int i = start + 1; i < n; i++) if (v[i] > m) m = v[i];
    return m;
}
static double sig_min(const double *v, int start, int n) {
    double m = v[start];
    for (int i = start + 1; i < n; i++) if (v[i] < m) m = v[i];
    return m;
}
static int sig_argmax(const double *v, int start, int n) {
    int idx = start;
    for (int i = start + 1; i < n; i++) if (v[i] > v[idx]) idx = i;
    return idx;
}
static int sig_argmin(const double *v, int start, int n) {
    int idx = start;
    for (int i = start + 1; i < n; i++) if (v[i] < v[idx]) idx = i;
    return idx;
}
static double sig_sma(const double *v, int start, int n) {
    if (n <= start) return 0.0;
    double s = 0.0;
    for (int i = start; i < n; i++) s += v[i];
    return s / (double)(n - start);
}

/* Exponential moving average step: ema += alpha * (x - ema). */
static inline double ema_step(double ema, double x, double alpha) {
    return ema + alpha * (x - ema);
}

/* Push a value into a small recursive ring used by STC's two stochastics. */
static void ring_push(double *buf, uint32_t *count, double x) {
    if (*count < STC_CYCLE) {
        buf[*count] = x;
        (*count)++;
    } else {
        for (uint32_t i = 1; i < STC_CYCLE; i++) buf[i - 1] = buf[i];
        buf[STC_CYCLE - 1] = x;
    }
}
static double ring_min(const double *buf, uint32_t count) {
    double m = buf[0];
    for (uint32_t i = 1; i < count; i++) if (buf[i] < m) m = buf[i];
    return m;
}
static double ring_max(const double *buf, uint32_t count) {
    double m = buf[0];
    for (uint32_t i = 1; i < count; i++) if (buf[i] > m) m = buf[i];
    return m;
}

/*----------------------------------------------------------------------------
 * Recursive (per-bar) signals: must be advanced exactly once per new bar.
 *---------------------------------------------------------------------------*/

/* (1,2) Swing Index + Accumulative Swing Index.
 * SI = magnitude of this bar's move relative to recent typical move (ATR),
 * scaled to ~[-100,100].  ASI accumulates SI over the page lifetime. */
static void sig_update_swing(page_signals_t *s, const double *v, int n) {
    if (n < 2) { s->si = 0.0; s->asi = s->asi_accum; return; }
    double move = v[n - 1] - v[n - 2];
    /* ATR proxy = mean |Δrate| over the window. */
    double atr = 0.0;
    for (int i = 1; i < n; i++) atr += fabs(v[i] - v[i - 1]);
    atr /= (double)(n - 1);
    double si = (atr > SIG_EPS) ? 50.0 * move / atr : 0.0;
    s->si = si;
    s->asi_accum += si;
    s->asi = s->asi_accum;
}

/* (15) TRIX: rate-of-change of a triple-smoothed EMA of the rate series. */
static void sig_update_trix(page_signals_t *s, double x) {
    const double alpha = 2.0 / (15.0 + 1.0); /* 15-period EMA */
    if (!s->trix_init) {
        s->trix_ema1 = s->trix_ema2 = s->trix_ema3 = s->trix_prev_ema3 = x;
        s->trix_init = true;
        s->trix = 0.0;
        return;
    }
    s->trix_ema1 = ema_step(s->trix_ema1, x, alpha);
    s->trix_ema2 = ema_step(s->trix_ema2, s->trix_ema1, alpha);
    double prev = s->trix_ema3;
    s->trix_ema3 = ema_step(s->trix_ema3, s->trix_ema2, alpha);
    s->trix = (prev > SIG_EPS) ? (s->trix_ema3 - prev) / prev * 100.0 : 0.0;
    s->trix_prev_ema3 = prev;
}

/* (11,12) Schaff Trend Cycle and its smoothed signal line.
 * MACD(23,50) -> stochastic(10) -> smooth -> stochastic(10) -> smooth. */
static void sig_update_stc(page_signals_t *s, double x) {
    const double af = 2.0 / (23.0 + 1.0);
    const double asl = 2.0 / (50.0 + 1.0);
    const double smooth = 0.5;
    if (!s->stc_init) {
        s->stc_ema_fast = s->stc_ema_slow = x;
        s->stc_pf = s->stc_pff = 0.0;
        s->stc_init = true;
    } else {
        s->stc_ema_fast = ema_step(s->stc_ema_fast, x, af);
        s->stc_ema_slow = ema_step(s->stc_ema_slow, x, asl);
    }
    double macd = s->stc_ema_fast - s->stc_ema_slow;
    ring_push(s->stc_macd_hist, &s->stc_macd_n, macd);

    double lo = ring_min(s->stc_macd_hist, s->stc_macd_n);
    double hi = ring_max(s->stc_macd_hist, s->stc_macd_n);
    double k1 = (hi - lo > SIG_EPS) ? 100.0 * (macd - lo) / (hi - lo) : 0.0;
    s->stc_pf = ema_step(s->stc_pf, k1, smooth);   /* smoothed %K */

    ring_push(s->stc_pf_hist, &s->stc_pf_n, s->stc_pf);
    double lo2 = ring_min(s->stc_pf_hist, s->stc_pf_n);
    double hi2 = ring_max(s->stc_pf_hist, s->stc_pf_n);
    double k2 = (hi2 - lo2 > SIG_EPS) ? 100.0 * (s->stc_pf - lo2) / (hi2 - lo2) : 0.0;
    s->stc_pff = ema_step(s->stc_pff, k2, smooth); /* = STC */

    s->stc = s->stc_pff;
    /* Signal line: extra 3-period EMA smoothing pass. */
    s->stc_signal = ema_step(s->stc_signal, s->stc, 2.0 / (3.0 + 1.0));
}

/* (8) Parabolic SAR over the single rate series (High=Low=value). */
static void sig_update_psar(page_signals_t *s, const double *v, int n) {
    const double af_step = 0.02, af_max = 0.2;
    double price = v[n - 1];
    if (!s->psar_init) {
        if (n < 2) { s->psar_out = price; s->psar_dir = 1; return; }
        s->psar_trend_up = v[n - 1] >= v[n - 2];
        s->psar = s->psar_trend_up ? sig_min(v, 0, n) : sig_max(v, 0, n);
        s->psar_ep = price;
        s->psar_af = af_step;
        s->psar_init = true;
    }
    /* Advance SAR toward the extreme point. */
    s->psar += s->psar_af * (s->psar_ep - s->psar);

    if (s->psar_trend_up) {
        if (price > s->psar_ep) {
            s->psar_ep = price;
            s->psar_af = fmin(s->psar_af + af_step, af_max);
        }
        if (price < s->psar) { /* reversal to downtrend */
            s->psar_trend_up = false;
            s->psar = s->psar_ep;
            s->psar_ep = price;
            s->psar_af = af_step;
        }
    } else {
        if (price < s->psar_ep) {
            s->psar_ep = price;
            s->psar_af = fmin(s->psar_af + af_step, af_max);
        }
        if (price > s->psar) { /* reversal to uptrend */
            s->psar_trend_up = true;
            s->psar = s->psar_ep;
            s->psar_ep = price;
            s->psar_af = af_step;
        }
    }
    s->psar_out = s->psar;
    s->psar_dir = s->psar_trend_up ? 1 : -1;
}

/* (13) Supertrend (period 10, multiplier 3) over the single rate series. */
static void sig_update_supertrend(page_signals_t *s, const double *v, int n) {
    const int period = 10;
    const double mult = 3.0;
    double price = v[n - 1];
    int start = (n > period) ? n - period : 1;
    double atr = 0.0;
    int cnt = 0;
    for (int i = start; i < n; i++) { atr += fabs(v[i] - v[i - 1]); cnt++; }
    atr = (cnt > 0) ? atr / cnt : 0.0;

    double basic_upper = price + mult * atr;
    double basic_lower = price - mult * atr;

    if (!s->st_init) {
        s->st_upper = basic_upper;
        s->st_lower = basic_lower;
        s->st_dir = 1;
        s->st_value = basic_lower;
        s->st_init = true;
    } else {
        /* Carry the tighter band forward unless price breaks through. */
        s->st_upper = (basic_upper < s->st_upper || v[n - 2] > s->st_upper)
                          ? basic_upper : s->st_upper;
        s->st_lower = (basic_lower > s->st_lower || v[n - 2] < s->st_lower)
                          ? basic_lower : s->st_lower;
        if (s->st_dir == 1 && price < s->st_lower) s->st_dir = -1;
        else if (s->st_dir == -1 && price > s->st_upper) s->st_dir = 1;
        s->st_value = (s->st_dir == 1) ? s->st_lower : s->st_upper;
    }
    s->supertrend = s->st_value;
    s->supertrend_dir = s->st_dir;
}

/*----------------------------------------------------------------------------
 * Window-recomputed signals.
 *---------------------------------------------------------------------------*/

/* (3) Aroon up/down/oscillator over min(n,25) bars. */
static void sig_compute_aroon(page_signals_t *s, const double *v, int n) {
    int period = (n - 1 < 25) ? n - 1 : 25;
    if (period < 1) { s->aroon_up = s->aroon_down = s->aroon_osc = 0.0; return; }
    int start = n - 1 - period;
    int hi = sig_argmax(v, start, n);
    int lo = sig_argmin(v, start, n);
    int since_hi = (n - 1) - hi;
    int since_lo = (n - 1) - lo;
    s->aroon_up = 100.0 * (period - since_hi) / period;
    s->aroon_down = 100.0 * (period - since_lo) / period;
    s->aroon_osc = s->aroon_up - s->aroon_down;
}

/* +DI, -DI and DX over the `period` bars ending at index `end` (single series:
 * +DM/-DM are the positive/negative bar moves, TR = |Δrate|). */
static double sig_di_dx(const double *v, int end, int period,
                        double *out_pdi, double *out_mdi) {
    double sm_plus = 0.0, sm_minus = 0.0, sm_tr = 0.0;
    for (int i = end - period + 1; i <= end; i++) {
        double up = v[i] - v[i - 1];
        double dn = v[i - 1] - v[i];
        sm_plus  += (up > dn && up > 0) ? up : 0.0;
        sm_minus += (dn > up && dn > 0) ? dn : 0.0;
        sm_tr    += fabs(v[i] - v[i - 1]);
    }
    double pdi = (sm_tr > SIG_EPS) ? 100.0 * sm_plus / sm_tr : 0.0;
    double mdi = (sm_tr > SIG_EPS) ? 100.0 * sm_minus / sm_tr : 0.0;
    if (out_pdi) *out_pdi = pdi;
    if (out_mdi) *out_mdi = mdi;
    double sum = pdi + mdi;
    return (sum > SIG_EPS) ? 100.0 * fabs(pdi - mdi) / sum : 0.0;
}

/* (4) ADX with +DI / -DI (period 14).  +DI/-DI are reported for the current
 * bar; ADX is the average of DX over the most recent `period` bars (the
 * smoothing that distinguishes ADX from a raw DX reading). */
static void sig_compute_adx(page_signals_t *s, const double *v, int n) {
    const int period = 14;
    if (n < period + 1) { s->adx = s->plus_di = s->minus_di = 0.0; return; }
    /* Current +DI / -DI. */
    sig_di_dx(v, n - 1, period, &s->plus_di, &s->minus_di);
    /* ADX = mean DX over the last min(period, available) ending bars. */
    int avail = (n - 1) - period + 1;        /* # of valid DX end-points */
    int span = (avail < period) ? avail : period;
    double sum_dx = 0.0;
    for (int e = n - span; e < n; e++) sum_dx += sig_di_dx(v, e, period, NULL, NULL);
    s->adx = sum_dx / span;
}

/* (5) GAPO: log(range)/log(period) over min(n,14) bars. */
static void sig_compute_gapo(page_signals_t *s, const double *v, int n) {
    int period = (n < 14) ? n : 14;
    if (period < 2) { s->gapo = 0.0; return; }
    int start = n - period;
    double range = sig_max(v, start, n) - sig_min(v, start, n);
    s->gapo = (range > SIG_EPS) ? log(range) / log((double)period) : 0.0;
}

/* (6) Ichimoku Cloud components (tenkan 9, kijun 26, senkou-B 52). */
static void sig_compute_ichimoku(page_signals_t *s, const double *v, int n) {
    int p9 = (n < 9) ? n : 9;
    int p26 = (n < 26) ? n : 26;
    int p52 = (n < 52) ? n : 52;
    s->ich_tenkan = (sig_max(v, n - p9, n) + sig_min(v, n - p9, n)) / 2.0;
    s->ich_kijun = (sig_max(v, n - p26, n) + sig_min(v, n - p26, n)) / 2.0;
    s->ich_senkou_a = (s->ich_tenkan + s->ich_kijun) / 2.0;
    s->ich_senkou_b = (sig_max(v, n - p52, n) + sig_min(v, n - p52, n)) / 2.0;
    s->ich_chikou = v[n - 1]; /* lagging span = current close */
}

/* (7) Linear regression slope/intercept over min(n,25) bars (x = bar index). */
static void sig_compute_linreg(page_signals_t *s, const double *v, int n) {
    int period = (n < 25) ? n : 25;
    if (period < 2) { s->linreg_slope = 0.0; s->linreg_intercept = v[n - 1]; return; }
    int start = n - period;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < period; i++) {
        double x = i, y = v[start + i];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double denom = period * sxx - sx * sx;
    if (fabs(denom) < SIG_EPS) { s->linreg_slope = 0.0; s->linreg_intercept = sy / period; return; }
    s->linreg_slope = (period * sxy - sx * sy) / denom;
    s->linreg_intercept = (sy - s->linreg_slope * sx) / period;
}

/* (9) Random Walk Index over min(n,14) bars. */
static void sig_compute_rwi(page_signals_t *s, const double *v, int n) {
    int period = (n - 1 < 14) ? n - 1 : 14;
    if (period < 2) { s->rwi_high = s->rwi_low = 0.0; return; }
    double best_high = 0.0, best_low = 0.0;
    for (int k = 2; k <= period; k++) {
        /* mean TR over the k-bar span ending at the latest bar */
        double atr = 0.0;
        for (int i = n - k; i < n; i++) atr += fabs(v[i] - v[i - 1]);
        atr /= (double)k;
        double denom = atr * sqrt((double)k);
        if (denom < SIG_EPS) continue;
        double hi = (v[n - 1] - v[n - k]) / denom;
        double lo = (v[n - k] - v[n - 1]) / denom;
        if (hi > best_high) best_high = hi;
        if (lo > best_low) best_low = lo;
    }
    s->rwi_high = best_high;
    s->rwi_low = best_low;
}

/* (10) RAVI: short vs long SMA divergence (7 vs 50). */
static void sig_compute_ravi(page_signals_t *s, const double *v, int n) {
    int sp = (n < 7) ? n : 7;
    int lp = (n < 50) ? n : 50;
    if (lp < 2) { s->ravi = 0.0; return; }
    double sma_s = sig_sma(v, n - sp, n);
    double sma_l = sig_sma(v, n - lp, n);
    s->ravi = (fabs(sma_l) > SIG_EPS) ? 100.0 * fabs(sma_s - sma_l) / fabs(sma_l) : 0.0;
}

/* (14) SQN: sqrt(N) * mean(R)/std(R) over bar-to-bar changes R. */
static void sig_compute_sqn(page_signals_t *s, const double *v, int n) {
    if (n < 3) { s->sqn = 0.0; return; }
    int N = n - 1;
    double mean = 0.0;
    for (int i = 1; i < n; i++) mean += (v[i] - v[i - 1]);
    mean /= N;
    double var = 0.0;
    for (int i = 1; i < n; i++) { double d = (v[i] - v[i - 1]) - mean; var += d * d; }
    var /= N;
    double sd = sqrt(var);
    s->sqn = (sd > SIG_EPS) ? sqrt((double)N) * mean / sd : 0.0;
}

/* (16) VHF: directional range / total movement over min(n,28) bars. */
static void sig_compute_vhf(page_signals_t *s, const double *v, int n) {
    int period = (n < 28) ? n : 28;
    if (period < 2) { s->vhf = 0.0; return; }
    int start = n - period;
    double range = sig_max(v, start, n) - sig_min(v, start, n);
    double total = 0.0;
    for (int i = start + 1; i < n; i++) total += fabs(v[i] - v[i - 1]);
    s->vhf = (total > SIG_EPS) ? range / total : 0.0;
}

/* (19) Recency-weighted frequency: rate series decayed toward the newest bar. */
static void sig_compute_recency_weighted(page_signals_t *s, const double *v, int n) {
    const double lambda = 0.9;
    double acc = 0.0, w = 1.0, wsum = 0.0;
    for (int i = n - 1; i >= 0; i--) { acc += v[i] * w; wsum += w; w *= lambda; }
    s->recency_weighted_freq = (wsum > SIG_EPS) ? acc / wsum : 0.0;
}

/* (17,18) Inter-access interval mean + variance (burstiness).
 * Mean uses true access timestamps; variance approximates per-bar gaps from
 * the implied interval 1000/rate (ms) of each bar that saw activity. */
static void sig_compute_inter_access(page_stats_t *stats, const double *v, int n) {
    page_signals_t *s = &stats->sig;
    uint64_t ac = atomic_load(&stats->access_count);
    uint64_t span_ns = atomic_load(&stats->last_access_ns) - stats->first_access_ns;
    uint64_t gaps = (ac > 1) ? (ac - 1) : 1;
    s->inter_access_interval_ms = (double)span_ns / 1e6 / (double)gaps;

    /* Variance of implied per-bar intervals (ms) across active bars. */
    double sum = 0.0, sumsq = 0.0; int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (v[i] > SIG_EPS) {
            double iv = 1000.0 / v[i]; /* ms between accesses in that bar */
            sum += iv; sumsq += iv * iv; cnt++;
        }
    }
    if (cnt >= 2) {
        double mean = sum / cnt;
        double var = sumsq / cnt - mean * mean;
        s->inter_access_variance_ms2 = (var > 0.0) ? var : 0.0;
    } else {
        s->inter_access_variance_ms2 = 0.0;
    }
}

/*----------------------------------------------------------------------------
 * Per-page signal driver: sample one bar, then refresh all 19 signals.
 * Called at the SIGNAL_SAMPLE_MS cadence by the policy thread.
 *---------------------------------------------------------------------------*/
void update_page_signals(page_stats_t *stats) {
    page_signals_t *s = &stats->sig;
    uint64_t now = get_time_ns();
    uint64_t ac = atomic_load(&stats->access_count);

    /* First call: prime the sampler, no bar yet. */
    if (s->last_sample_ns == 0) {
        s->last_sample_ns = now;
        s->last_sample_access = ac;
        return;
    }

    uint64_t dt_ns = now - s->last_sample_ns;
    if (dt_ns == 0) return;
    /* access_count is not strictly monotonic: record_page_access() increments it
     * while pebs_merge_with_page_stats() periodically re-baselines it via store.
     * Guard the unsigned subtraction so a downward re-baseline can never underflow
     * into a garbage bar; treat a backwards step as an idle (zero-rate) interval. */
    uint64_t prev = s->last_sample_access;
    double bar = (ac > prev) ? (double)(ac - prev) * 1e9 / (double)dt_ns : 0.0;
    s->last_sample_ns = now;
    s->last_sample_access = ac;

    /* Append the bar to the circular window. */
    s->rate_hist[s->hist_head] = bar;
    s->hist_head = (s->hist_head + 1) % SIGNAL_WINDOW;
    if (s->hist_count < SIGNAL_WINDOW) s->hist_count++;
    s->interval_access_rate = bar;

    /* Flatten the ring into chronological order v[0..n-1] (newest last). */
    int n = (int)s->hist_count;
    double v[SIGNAL_WINDOW];
    uint32_t idx = (s->hist_count < SIGNAL_WINDOW) ? 0 : s->hist_head;
    for (int i = 0; i < n; i++) {
        v[i] = s->rate_hist[idx];
        idx = (idx + 1) % SIGNAL_WINDOW;
    }

    /* Recursive signals advance once per bar. */
    sig_update_swing(s, v, n);
    sig_update_trix(s, bar);
    sig_update_stc(s, bar);
    sig_update_psar(s, v, n);
    sig_update_supertrend(s, v, n);

    /* Window-recomputed signals. */
    sig_compute_aroon(s, v, n);
    sig_compute_adx(s, v, n);
    sig_compute_gapo(s, v, n);
    sig_compute_ichimoku(s, v, n);
    sig_compute_linreg(s, v, n);
    sig_compute_rwi(s, v, n);
    sig_compute_ravi(s, v, n);
    sig_compute_sqn(s, v, n);
    sig_compute_vhf(s, v, n);
    sig_compute_recency_weighted(s, v, n);
    sig_compute_inter_access(stats, v, n);
}

void update_all_page_signals(void) {
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    for (size_t i = 0; i < PAGE_STATS_HASH_SIZE; i++) {
        page_stats_t *entry = g_manager.page_stats_table[i];
        while (entry != NULL) {
            update_page_signals(entry);
            entry = entry->next;
        }
    }
    pthread_rwlock_unlock(&g_manager.stats_lock);
}

void print_page_stats_summary(void) {
    pthread_rwlock_rdlock(&g_manager.stats_lock);
    
    uint64_t total = atomic_load(&g_manager.total_pages_tracked);
    uint64_t hot = 0, cold = 0;
    double total_heat = 0.0;
    
    for (size_t i = 0; i < PAGE_STATS_HASH_SIZE; i++) {
        page_stats_t *entry = g_manager.page_stats_table[i];
        while (entry != NULL) {
            total_heat += entry->heat_score;
            if (entry->heat_score > 0.5) hot++;
            else cold++;
            entry = entry->next;
        }
    }
    pthread_rwlock_unlock(&g_manager.stats_lock);
    
    TM_INFO("Pages: %" PRIu64 " total, %" PRIu64 " hot, %" PRIu64 " cold, avg heat: %.3f",
            total, hot, cold, total > 0 ? total_heat / total : 0.0);
}

void cleanup_page_stats(void) {
    pthread_rwlock_wrlock(&g_manager.stats_lock);
    for (size_t i = 0; i < PAGE_STATS_HASH_SIZE; i++) {
        page_stats_t *entry = g_manager.page_stats_table[i];
        while (entry != NULL) {
            page_stats_t *next = entry->next;
            free(entry);
            entry = next;
        }
        g_manager.page_stats_table[i] = NULL;
    }
    atomic_store(&g_manager.total_pages_tracked, 0);
    pthread_rwlock_unlock(&g_manager.stats_lock);
    TM_INFO("Page statistics cleaned up");
}
