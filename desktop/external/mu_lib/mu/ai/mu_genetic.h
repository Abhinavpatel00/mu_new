#pragma once

/*
===============================================================================

mu_genetic.h — genetic algorithm (fixed-length u32 chromosomes).

- Population of chromosomes stored SoA-ish: flat array of genes plus a
  parallel fitness array.
- Selection: tournament selection.
- Variation: single-point crossover + per-gene mutation.
- Generational evolution: evaluate -> select -> crossover/mutate -> replace.

The caller provides: fitness function, gene initializer, and a mutation
perturb. All flat arrays; allocation only at init/resize.

===============================================================================
*/

#include "mu_common.h"

MU_BEGIN_EXTERN_C

/* Fitness of a chromosome: higher is better. `genes` points to
 * chromosome_length u32 values. */
typedef float (*mu_genetic_fitness)(const uint32_t* genes,
                                    uint32_t length, void* ctx);

/* Initialize a gene at index `i` of `genes` (called at population init and
 * on mutation). */
typedef void (*mu_genetic_init_gene)(uint32_t* genes, uint32_t i,
                                     void* ctx, uint32_t seed);

/* Mutate one gene in place. */
typedef void (*mu_genetic_mutate)(uint32_t* genes, uint32_t i,
                                  void* ctx, uint32_t seed);

typedef struct mu_genetic
{
    mu_genetic_fitness fitness;
    mu_genetic_init_gene init_gene;
    mu_genetic_mutate  mutate;
    void*              ctx;

    uint32_t*  genes;      /* [population * chromosome_length] */
    float*     fitnesses;  /* [population] */
    uint32_t*  scratch;    /* [chromosome_length] for crossover */

    uint32_t population;
    uint32_t chromosome_length;
    uint32_t gene_domain;   /* genes are in [0, gene_domain) */

    uint64_t rng_state;
    uint64_t rng_inc;
} mu_genetic;

/* ---------- lifecycle ---------- */

MU_INLINE uint32_t mu_genetic_rand(mu_genetic* g)
{
    /* simple pcg-like step */
    g->rng_state = g->rng_state * 6364136223846793005ULL + g->rng_inc;
    uint32_t x = (uint32_t)(((g->rng_state >> 18u) ^ g->rng_state) >> 27u);
    uint32_t r = (uint32_t)(g->rng_state >> 59u);
    return (x >> r) | (x << ((-r) & 31));
}

MU_INLINE bool mu_genetic_init(mu_genetic* g,
                               mu_genetic_fitness fitness,
                               mu_genetic_init_gene init_gene,
                               mu_genetic_mutate mutate,
                               void* ctx,
                               uint32_t population,
                               uint32_t chromosome_length,
                               uint32_t gene_domain,
                               uint32_t seed)
{
    MU_ASSERT(g != NULL);
    MU_ASSERT(fitness != NULL);
    MU_ASSERT(init_gene != NULL);
    MU_ASSERT(population > 0u && chromosome_length > 0u && gene_domain > 0u);

    g->fitness = fitness;
    g->init_gene = init_gene;
    g->mutate = mutate;
    g->ctx = ctx;
    g->population = population;
    g->chromosome_length = chromosome_length;
    g->gene_domain = gene_domain;
    g->rng_state = seed;
    g->rng_inc = (seed << 1u) | 1u;

    size_t gbytes = (size_t)population * (size_t)chromosome_length *
                    sizeof(uint32_t);
    g->genes = (uint32_t*)MU_MALLOC(gbytes);
    g->fitnesses = (float*)MU_MALLOC(sizeof(float) * population);
    g->scratch = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * chromosome_length);

    bool ok = g->genes && g->fitnesses && g->scratch;
    if(!ok)
    {
        MU_FREE(g->genes);
        MU_FREE(g->fitnesses);
        MU_FREE(g->scratch);
        g->genes = NULL;
        g->fitnesses = NULL;
        g->scratch = NULL;
        return false;
    }

    /* initialize population */
    for(uint32_t i = 0; i < population; ++i)
    {
        uint32_t* chrom = g->genes + (size_t)i * (size_t)chromosome_length;
        for(uint32_t j = 0; j < chromosome_length; ++j)
            g->init_gene(chrom, j, g->ctx, mu_genetic_rand(g));
    }
    return true;
}

MU_INLINE void mu_genetic_destroy(mu_genetic* g)
{
    if(g == NULL)
        return;
    MU_FREE(g->genes);
    MU_FREE(g->fitnesses);
    MU_FREE(g->scratch);
    g->genes = NULL;
    g->fitnesses = NULL;
    g->scratch = NULL;
    g->population = 0;
    g->chromosome_length = 0;
    g->gene_domain = 0;
}

/* Evaluate the whole population. */
MU_INLINE void mu_genetic_evaluate(mu_genetic* g)
{
    MU_ASSERT(g != NULL);
    for(uint32_t i = 0; i < g->population; ++i)
    {
        const uint32_t* chrom = g->genes + (size_t)i *
                                (size_t)g->chromosome_length;
        g->fitnesses[i] = g->fitness(chrom, g->chromosome_length, g->ctx);
    }
}

/* Return index of the best individual. */
MU_INLINE uint32_t mu_genetic_best(const mu_genetic* g)
{
    MU_ASSERT(g != NULL);
    uint32_t best = 0;
    float best_f = g->fitnesses[0];
    for(uint32_t i = 1; i < g->population; ++i)
    {
        if(g->fitnesses[i] > best_f)
        {
            best_f = g->fitnesses[i];
            best = i;
        }
    }
    return best;
}

/* Copy chromosome `src` to `dst` index. */
MU_INLINE void mu_genetic_copy(mu_genetic* g, uint32_t dst, uint32_t src)
{
    MU_ASSERT(g != NULL);
    MU_ASSERT(dst < g->population && src < g->population);
    uint32_t* d = g->genes + (size_t)dst * (size_t)g->chromosome_length;
    const uint32_t* s = g->genes + (size_t)src * (size_t)g->chromosome_length;
    MU_MEMCPY(d, s, (size_t)g->chromosome_length * sizeof(uint32_t));
    g->fitnesses[dst] = g->fitnesses[src];
}

/* Tournament selection: pick `k` random individuals, return the best. */
MU_INLINE uint32_t mu_genetic_select(mu_genetic* g, uint32_t tournament_size)
{
    MU_ASSERT(g != NULL);
    if(tournament_size < 2u)
        tournament_size = 2u;
    if(tournament_size > g->population)
        tournament_size = g->population;

    uint32_t best = mu_genetic_rand(g) % g->population;
    float best_f = g->fitnesses[best];
    for(uint32_t t = 1; t < tournament_size; ++t)
    {
        uint32_t c = mu_genetic_rand(g) % g->population;
        if(g->fitnesses[c] > best_f)
        {
            best_f = g->fitnesses[c];
            best = c;
        }
    }
    return best;
}

/* ---------- evolution ---------- */

/* Evolve one generation:
 *   1. keep the elite (best individual) unchanged
 *   2. fill the rest by: select two parents, crossover, mutate, evaluate
 *
 * mutation_rate: probability [0,1] each gene mutates.
 * crossover_rate: probability [0,1] crossover happens (else copy parent). */
MU_INLINE void mu_genetic_evolve(mu_genetic* g, float mutation_rate,
                                 float crossover_rate)
{
    MU_ASSERT(g != NULL);
    if(mutation_rate < 0.0f) mutation_rate = 0.0f;
    if(mutation_rate > 1.0f) mutation_rate = 1.0f;
    if(crossover_rate < 0.0f) crossover_rate = 0.0f;
    if(crossover_rate > 1.0f) crossover_rate = 1.0f;

    uint32_t pop = g->population;
    uint32_t len = g->chromosome_length;

    uint32_t elite = mu_genetic_best(g);

    /* new population buffer */
    uint32_t* new_genes = (uint32_t*)MU_MALLOC((size_t)pop * (size_t)len *
                                               sizeof(uint32_t));
    float* new_fit = (float*)MU_MALLOC(sizeof(float) * pop);
    if(!new_genes || !new_fit)
    {
        MU_FREE(new_genes);
        MU_FREE(new_fit);
        return;
    }

    /* copy elite */
    MU_MEMCPY(new_genes, g->genes + (size_t)elite * (size_t)len,
              (size_t)len * sizeof(uint32_t));
    new_fit[0] = g->fitnesses[elite];

    uint32_t filled = 1u;
    while(filled < pop)
    {
        uint32_t* child = new_genes + (size_t)filled * (size_t)len;
        float child_f = 0.0f;

        /* select parents */
        uint32_t p1 = mu_genetic_select(g, 3u);
        uint32_t p2 = mu_genetic_select(g, 3u);

        const uint32_t* cp1 = g->genes + (size_t)p1 * (size_t)len;
        const uint32_t* cp2 = g->genes + (size_t)p2 * (size_t)len;

        if(mu_genetic_rand(g) / 4294967296.0f < crossover_rate && pop > 1u)
        {
            uint32_t cut = mu_genetic_rand(g) % len;
            for(uint32_t i = 0; i < cut; ++i)
                child[i] = cp1[i];
            for(uint32_t i = cut; i < len; ++i)
                child[i] = cp2[i];
        }
        else
        {
            MU_MEMCPY(child, cp1, (size_t)len * sizeof(uint32_t));
        }

        /* mutate */
        for(uint32_t i = 0; i < len; ++i)
        {
            if(mu_genetic_rand(g) / 4294967296.0f < mutation_rate)
            {
                if(g->mutate)
                    g->mutate(child, i, g->ctx, mu_genetic_rand(g));
                else
                    child[i] = mu_genetic_rand(g) % g->gene_domain;
            }
        }

        child_f = g->fitness(child, len, g->ctx);
        new_fit[filled] = child_f;
        ++filled;
    }

    /* swap in new generation */
    MU_MEMCPY(g->genes, new_genes, (size_t)pop * (size_t)len * sizeof(uint32_t));
    MU_MEMCPY(g->fitnesses, new_fit, sizeof(float) * pop);

    MU_FREE(new_genes);
    MU_FREE(new_fit);
}

MU_END_EXTERN_C
