#pragma once

/*
===============================================================================

mu_rhea.h — rolling-horizon evolutionary planning (RHEA).

The ~50-line planner from Gaina/Devlin/Pérez-Liébana/Lucas (IEEE ToG 2020):
a population of fixed-length action sequences, evaluated by rolling a forward
model from the current state, evolved for a few generations, first action
executed, everything shifts.

Sibling to mu_mcts: same contract (pick an action for this tick), different
machinery. Where MCTS needs a tree and UCT tuning, RHEA needs one callback —
the forward-model evaluation — and a genome. With a good forward model it
matches MCTS at a fraction of the code (the paper's measured result).

- Genomes: uint16 action ids, horizon H (e.g. 8-16).
- Evolution: elitist (keep best half), uniform crossover, single-gene mutation
  (one random gene re-rolled with probability `mutation_rate` — per-gene
  independent mutation destroys multi-gene gains faster than selection can
  accumulate them; the gate's gradient objective is the proof).
- Shift buffer: after execution, drop the first gene of every genome and
  append a random action — the plan warm-starts from last tick.

Deterministic: rng state is caller-owned xorshift32.

===============================================================================
*/

#include "mu_common.h"

MU_BEGIN_EXTERN_C

#define MU_RHEA_MAX_HORIZON 32
#define MU_RHEA_MAX_POP     64

typedef float (*mu_rhea_eval)(const uint16_t* actions, int horizon, void* ctx);

typedef struct mu_rhea
{
    int horizon;
    int population;
    int generations;
    float mutation_rate;      /* per-gene flip probability, e.g. 0.2 */
    uint16_t genome[MU_RHEA_MAX_POP][MU_RHEA_MAX_HORIZON];
    float fitness[MU_RHEA_MAX_POP];
    uint32_t rng_state;
} mu_rhea;

MU_INLINE uint32_t mu_rhea_rand32(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x9E3779B9u;
    return *s;
}

MU_INLINE void mu_rhea_init(mu_rhea* r, int horizon, int population,
                            int generations, uint16_t n_actions,
                            uint32_t seed)
{
    int i, j;
    if (horizon > MU_RHEA_MAX_HORIZON) horizon = MU_RHEA_MAX_HORIZON;
    if (population > MU_RHEA_MAX_POP) population = MU_RHEA_MAX_POP;
    if (horizon < 1) horizon = 1;
    if (population < 2) population = 2;
    r->horizon = horizon;
    r->population = population;
    r->generations = (generations < 1) ? 1 : generations;
    r->mutation_rate = 0.2f;
    r->rng_state = seed ? seed : 0x2545F491u;
    for (i = 0; i < population; ++i)
        for (j = 0; j < horizon; ++j)
            r->genome[i][j] = (uint16_t)(mu_rhea_rand32(&r->rng_state) %
                                         (n_actions ? n_actions : 1u));
}

/* Shift-buffer warm start: drop the executed first action of every genome. */
MU_INLINE void mu_rhea_shift(mu_rhea* r, uint16_t n_actions)
{
    int i, j;
    for (i = 0; i < r->population; ++i)
    {
        for (j = 0; j + 1 < r->horizon; ++j)
            r->genome[i][j] = r->genome[i][j + 1];
        r->genome[i][r->horizon - 1] =
            (uint16_t)(mu_rhea_rand32(&r->rng_state) % (n_actions ? n_actions : 1u));
    }
}

/* Solve: evolve `generations` x population, output the best action sequence
 * (up to `max_out`), return the best fitness. */
MU_INLINE float mu_rhea_solve(mu_rhea* r, mu_rhea_eval eval, void* ctx,
                              uint16_t n_actions,
                              uint16_t* out_actions, int max_out)
{
    int gen, i, j;
    int pop = r->population;
    int hz = r->horizon;

    /* evaluate initial population */
    for (i = 0; i < pop; ++i)
        r->fitness[i] = eval(r->genome[i], hz, ctx);

    for (gen = 0; gen < r->generations; ++gen)
    {
        /* elitist step: keep the true top half by fitness (ties: lowest
         * index — deterministic). Offspring fill the rest. */
        uint16_t elites[MU_RHEA_MAX_POP / 2][MU_RHEA_MAX_HORIZON];
        float elite_fit[MU_RHEA_MAX_POP / 2];
        int used[MU_RHEA_MAX_POP];
        int half = pop / 2;

        for (i = 0; i < pop; ++i) used[i] = 0;
        for (i = 0; i < half; ++i)
        {
            int best = -1;
            for (j = 0; j < pop; ++j)
                if (!used[j] && (best < 0 || r->fitness[j] > r->fitness[best]))
                    best = j;
            used[best] = 1;
            for (j = 0; j < hz; ++j) elites[i][j] = r->genome[best][j];
            elite_fit[i] = r->fitness[best];
        }

        /* rebuild population: elites first */
        for (i = 0; i < half; ++i)
        {
            for (j = 0; j < hz; ++j) r->genome[i][j] = elites[i][j];
            r->fitness[i] = elite_fit[i];
        }
        /* offspring: uniform crossover of two random elites + single-gene
         * mutation (re-roll ONE random gene with probability mutation_rate) */
        for (i = half; i < pop; ++i)
        {
            int pa = (int)(mu_rhea_rand32(&r->rng_state) % (uint32_t)half);
            int pb = (int)(mu_rhea_rand32(&r->rng_state) % (uint32_t)half);
            for (j = 0; j < hz; ++j)
            {
                r->genome[i][j] = (mu_rhea_rand32(&r->rng_state) & 1u)
                                ? elites[pa][j] : elites[pb][j];
            }
            if ((mu_rhea_rand32(&r->rng_state) & 0xFFFFu) <
                (uint32_t)(r->mutation_rate * 65535.0f))
            {
                int mg = (int)(mu_rhea_rand32(&r->rng_state) % (uint32_t)hz);
                r->genome[i][mg] = (uint16_t)(mu_rhea_rand32(&r->rng_state) %
                                              (n_actions ? n_actions : 1u));
            }
            r->fitness[i] = eval(r->genome[i], hz, ctx);
        }
    }

    /* output best */
    {
        int best = 0;
        int n = (max_out < hz) ? max_out : hz;
        for (i = 1; i < pop; ++i)
            if (r->fitness[i] > r->fitness[best]) best = i;
        for (j = 0; j < n; ++j) out_actions[j] = r->genome[best][j];
        return r->fitness[best];
    }
}

MU_END_EXTERN_C
