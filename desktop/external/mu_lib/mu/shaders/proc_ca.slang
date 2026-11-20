/* mu/proc_ca.slang — extended cellular automata.
 * ============================================================================
 *  Elementary 1D CA (Rule 30, Rule 110), 2D cyclic CA, Wireworld, and
 *  sandpile. Each step function maps a local neighborhood to the next
 *  state; the caller ping-pongs two RWTexture2D/RWStructuredBuffers.
 *
 *  Sources: Wolfram (elementary CA), Brian Silverman (Wireworld),
 *  Per Bak et al. (sandpile / self-organized criticality).
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 * ==========================================================================*/
#ifndef MU_PROC_CA_SLANG
#define MU_PROC_CA_SLANG

#include "common.slang"

/* =========================================================================
 *  ELEMENTARY 1D CA — Wolfram rules.
 *  `rule` is the 8-bit Wolfram code (0..255); bit k = output for
 *  neighborhood pattern k (left-center-right, 3 bits). Returns next state.
 * =========================================================================*/

int mu_ca_rule110(int left, int center, int right, int rule)
{
    /* Neighborhood index = 4*left + 2*center + right. */
    int idx = (left << 2) | (center << 1) | right;
    /* Extract bit idx from rule. */
    int bit = (rule >> idx) & 1u;
    return bit;
}

/* Rule 30: chaotic, good for random-looking texture generation. */
int mu_ca_rule30(int left, int center, int right)
{
    return mu_ca_rule110(left, center, right, 30);
}

/* Rule 110: Turing-complete elementary CA. */
int mu_ca_rule110_step(int left, int center, int right)
{
    return mu_ca_rule110(left, center, right, 110);
}

/* =========================================================================
 *  2D CYCLIC CA — colors cycle through states; a cell advances if a
 *  neighbor has the next state. Good for spiral-wave patterns.
 * =========================================================================*/

int mu_ca_cyclic(int state, int n_states,
                 int n0, int n1, int n2, int n3)
{
    int next = (state + 1) % n_states;
    int   fire = (n0 == next || n1 == next || n2 == next || n3 == next) ? 1 : 0;
    return select(fire, next, state);
}

/* =========================================================================
 *  WIREWORLD — 4-state electron CA.
 *  States: 0=empty, 1=electron head, 2=electron tail, 3=conductor.
 *  Rules:
 *   head -> tail
 *   tail -> conductor
 *   conductor -> head if exactly 1 or 2 heads in neighborhood, else conductor
 *   empty -> empty
 * =========================================================================*/

int mu_ca_wireworld(int state, int n_heads)
{
    int nxt = state;
    int is_head = (state == 1) ? 1 : 0;
    int is_tail = (state == 2) ? 1 : 0;
    int is_cond = (state == 3) ? 1 : 0;

    /* head -> tail */
    nxt = select(is_head, 2, nxt);
    /* tail -> conductor */
    nxt = select(is_tail, 3, nxt);
    /* conductor -> head if 1 or 2 neighbors */
    int spark = (n_heads == 1 || n_heads == 2) ? 1 : 0;
    nxt = select(is_cond && spark, 1, nxt);
    return nxt;
}

/* =========================================================================
 *  SANDPILE — 2D Abelian sandpile (self-organized criticality).
 *  Each cell holds a grain count; if >= 4 it topples, sending 1 grain
 *  to each neighbor and keeping the remainder. Returns new grain count
 *  and (via out params conceptually) the toppled amount is added by
 *  the caller's neighbor accumulation pass.
 * =========================================================================*/

/* Topple one cell: returns grains kept (0..3) and toppled count (0..3).
 * The toppled grains are distributed to neighbors by the caller. */
int2 mu_sandpile_topple(int grains)
{
    int toppled = grains / 4;           /* 0 or 1 for grains < 8 */
    int kept    = grains - toppled * 4;
    return int2(kept, toppled);
}

#endif /* MU_PROC_CA_SLANG */