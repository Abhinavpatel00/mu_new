#pragma once

/*
===============================================================================

mu_stats.h — running statistics + fixed-bucket histogram (percentiles).

DOD profiling primitive (from the DOD skill's profiling chapter):
- Welford's online algorithm: min/max/mean/variance in one pass, no sample
  storage.
- Fixed-bucket log-spaced histogram for percentiles (p50/p90/p99) without
  storing individual samples — constant memory, O(buckets) query.

No allocation after init. Canonical surface: init / init_static / destroy /
reset, push, percentile, count/min/max/mean/variance/stddev.

===============================================================================
*/

#include "mu_common.h"

#include <math.h>

MU_BEGIN_EXTERN_C

typedef struct mu_stats
{
    uint64_t count;
    double   mean;
    double   m2;        /* sum of squares of differences from the mean      */
    double   min;
    double   max;

    /* log-spaced histogram buckets */
    uint32_t*  hist_counts;
    double*    hist_edges;   /* bucket upper bounds, size hist_cap+1 */
    uint32_t   hist_cap;
    double     hist_lo;
    double     hist_hi;
} mu_stats;

MU_INLINE bool mu_stats_init(mu_stats* s, uint32_t hist_buckets,
                             double hist_lo, double hist_hi)
{
    MU_ASSERT(s != NULL);
    MU_ASSERT(hist_buckets > 0u);
    MU_ASSERT(hist_hi > hist_lo);

    s->count = 0;
    s->mean = 0.0;
    s->m2 = 0.0;
    s->min = 0.0;
    s->max = 0.0;

    s->hist_cap = hist_buckets;
    s->hist_lo = hist_lo;
    s->hist_hi = hist_hi;

    s->hist_counts = (uint32_t*)MU_CALLOC(hist_buckets, sizeof(uint32_t));
    s->hist_edges = (double*)MU_MALLOC(sizeof(double) * (hist_buckets + 1u));

    bool ok = s->hist_counts && s->hist_edges;
    if(!ok)
    {
        MU_FREE(s->hist_counts);
        MU_FREE(s->hist_edges);
        s->hist_counts = NULL;
        s->hist_edges = NULL;
        s->hist_cap = 0;
        return false;
    }

    /* log-spaced edges between lo and hi */
    double ratio = pow(hist_hi / hist_lo, 1.0 / (double)hist_buckets);
    double edge = hist_lo;
    for(uint32_t i = 0; i <= hist_buckets; ++i)
    {
        s->hist_edges[i] = edge;
        edge *= ratio;
    }
    return true;
}

/* Static variant: caller supplies histogram storage, zero malloc. */
MU_INLINE bool mu_stats_init_static(mu_stats* s, uint32_t hist_buckets,
                                    double hist_lo, double hist_hi,
                                    uint32_t* hist_counts, double* hist_edges)
{
    MU_ASSERT(s != NULL);
    MU_ASSERT(hist_buckets > 0u && hist_hi > hist_lo);
    MU_ASSERT(hist_counts && hist_edges);

    s->count = 0;
    s->mean = 0.0;
    s->m2 = 0.0;
    s->min = 0.0;
    s->max = 0.0;
    s->hist_cap = hist_buckets;
    s->hist_lo = hist_lo;
    s->hist_hi = hist_hi;
    s->hist_counts = hist_counts;
    s->hist_edges = hist_edges;
    MU_MEMSET(s->hist_counts, 0, sizeof(uint32_t) * hist_buckets);

    double ratio = pow(hist_hi / hist_lo, 1.0 / (double)hist_buckets);
    double edge = hist_lo;
    for(uint32_t i = 0; i <= hist_buckets; ++i)
    {
        s->hist_edges[i] = edge;
        edge *= ratio;
    }
    return true;
}

MU_INLINE void mu_stats_destroy(mu_stats* s)
{
    if(s == NULL)
        return;
    MU_FREE(s->hist_counts);
    MU_FREE(s->hist_edges);
    s->hist_counts = NULL;
    s->hist_edges = NULL;
    s->hist_cap = 0;
    s->count = 0;
    s->mean = 0.0;
    s->m2 = 0.0;
    s->min = 0.0;
    s->max = 0.0;
}

MU_INLINE void mu_stats_reset(mu_stats* s)
{
    MU_ASSERT(s != NULL);
    s->count = 0;
    s->mean = 0.0;
    s->m2 = 0.0;
    s->min = 0.0;
    s->max = 0.0;
    if(s->hist_counts)
        MU_MEMSET(s->hist_counts, 0, sizeof(uint32_t) * s->hist_cap);
}

MU_INLINE uint64_t mu_stats_count(const mu_stats* s) { return s->count; }
MU_INLINE double mu_stats_min(const mu_stats* s)  { return s->min; }
MU_INLINE double mu_stats_max(const mu_stats* s)  { return s->max; }
MU_INLINE double mu_stats_mean(const mu_stats* s) { return s->count ? s->mean : 0.0; }

MU_INLINE double mu_stats_variance(const mu_stats* s)
{
    return s->count > 1u ? s->m2 / (double)(s->count - 1u) : 0.0;
}

MU_INLINE double mu_stats_stddev(const mu_stats* s)
{
    return sqrt(mu_stats_variance(s));
}

/* Push a sample. Welford update + histogram bucket. */
MU_INLINE void mu_stats_push(mu_stats* s, double x)
{
    MU_ASSERT(s != NULL);

    ++s->count;
    double delta = x - s->mean;
    s->mean += delta / (double)s->count;
    double delta2 = x - s->mean;
    s->m2 += delta * delta2;

    if(s->count == 1u || x < s->min)
        s->min = x;
    if(s->count == 1u || x > s->max)
        s->max = x;

    /* histogram bucket: find first edge >= x (clamped) */
    if(x <= s->hist_lo)
    {
        if(s->hist_counts)
            ++s->hist_counts[0];
    }
    else if(x >= s->hist_hi)
    {
        if(s->hist_counts)
            ++s->hist_counts[s->hist_cap - 1u];
    }
    else if(s->hist_counts)
    {
        /* binary search over edges */
        uint32_t lo = 0, hi = s->hist_cap - 1u;
        while(lo < hi)
        {
            uint32_t mid = (lo + hi) / 2u;
            if(x <= s->hist_edges[mid + 1u])
                hi = mid;
            else
                lo = mid + 1u;
        }
        ++s->hist_counts[lo];
    }
}

/* Histogram count in bucket i (0..hist_cap-1). */
MU_INLINE uint32_t mu_stats_hist_count(const mu_stats* s, uint32_t i)
{
    MU_ASSERT(s != NULL);
    if(i >= s->hist_cap || !s->hist_counts)
        return 0u;
    return s->hist_counts[i];
}

/* Percentile from the histogram (linear interpolation inside bucket).
 * p in [0,100]. Returns 0 if no samples. */
MU_INLINE double mu_stats_percentile(const mu_stats* s, double p)
{
    MU_ASSERT(s != NULL);
    if(s->count == 0u || !s->hist_counts || !s->hist_edges)
        return 0.0;
    if(p <= 0.0)
        return s->min;
    if(p >= 100.0)
        return s->max;

    double target = (double)s->count * (p / 100.0);
    double cum = 0.0;
    for(uint32_t i = 0; i < s->hist_cap; ++i)
    {
        double edge_lo = s->hist_edges[i];
        double edge_hi = s->hist_edges[i + 1u];
        double cnt = (double)s->hist_counts[i];
        if(cum + cnt >= target)
        {
            if(cnt <= 0.0)
                return edge_lo;
            double frac = (target - cum) / cnt;
            return edge_lo + frac * (edge_hi - edge_lo);
        }
        cum += cnt;
    }
    return s->max;
}

MU_END_EXTERN_C
