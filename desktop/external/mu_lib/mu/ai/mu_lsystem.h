#pragma once

/*
===============================================================================

mu_lsystem.h — L-systems (Lindenmayer systems) for procedural modeling.

Texturing & Modeling: A Procedural Approach — "Algorithmic Botany" chapter
(Prusinkiewicz & Lindenmayer).

- An axiom string plus productions (symbol -> replacement string).
- Iterative rewriting expands the string; each symbol is interpreted by a
  2D turtle into line segments:
      F = forward and draw
      f = forward without drawing
      + = turn left by the system angle
      - = turn right by the system angle
      [ = push turtle state, ] = pop turtle state
  All other symbols are no-ops (reserved for leaves/flowers/etc.).
- Output is a flat array of segments {x0,y0,x1,y1}; the caller supplies the
  buffer. Expansion strings are bounded by a caller-provided max length so no
  allocation happens after init.

All flat arrays; no per-symbol allocation after init.

===============================================================================
*/

#include "mu_common.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

MU_BEGIN_EXTERN_C

/* One production: `symbol` -> `replacement`. */
typedef struct mu_lsystem_rule
{
    char symbol;
    char* replacement;   /* null-terminated replacement string   */
    uint32_t replacement_len;
} mu_lsystem_rule;

/* Turtle state interpreted from the final string. */
typedef struct mu_lsystem_turtle
{
    float x;
    float y;
    float angle;   /* radians; +angle = turn left (counter-clockwise) */
} mu_lsystem_turtle;

/* One output segment produced while interpreting. */
typedef struct mu_lsystem_segment
{
    float x0, y0;
    float x1, y1;
} mu_lsystem_segment;

typedef struct mu_lsystem
{
    char* axiom;                /* null-terminated starting string      */
    uint32_t axiom_len;

    mu_lsystem_rule* rules;
    uint32_t rule_count;
    uint32_t rule_cap;

    /* working string, rewritten each iteration                    */
    char* current;
    uint32_t current_len;
    uint32_t current_cap;

    /* absolute cap on the working string to bound memory         */
    uint32_t max_len;
} mu_lsystem;

/* ---------- lifecycle ---------- */

/* `max_len` hard-caps the expanded string; init fails if axiom is longer. */
MU_INLINE bool mu_lsystem_init(mu_lsystem* l, uint32_t max_len)
{
    MU_ASSERT(l != NULL);
    MU_ASSERT(max_len > 0u);

    l->axiom = (char*)MU_CALLOC(max_len + 1u, sizeof(char));
    l->rules = (mu_lsystem_rule*)MU_CALLOC(16u, sizeof(mu_lsystem_rule));
    l->current = (char*)MU_CALLOC(max_len + 1u, sizeof(char));

    bool ok = l->axiom && l->rules && l->current;
    if(!ok)
    {
        MU_FREE(l->axiom);
        MU_FREE(l->rules);
        MU_FREE(l->current);
        l->axiom = NULL;
        l->rules = NULL;
        l->current = NULL;
        return false;
    }

    l->axiom[0] = '\0';
    l->axiom_len = 0u;
    l->rule_count = 0u;
    l->rule_cap = 16u;
    l->current[0] = '\0';
    l->current_len = 0u;
    l->current_cap = max_len;
    l->max_len = max_len;
    return true;
}

MU_INLINE void mu_lsystem_destroy(mu_lsystem* l)
{
    if(l == NULL)
        return;
    for(uint32_t i = 0; i < l->rule_count; ++i)
        MU_FREE(l->rules[i].replacement);
    MU_FREE(l->axiom);
    MU_FREE(l->rules);
    MU_FREE(l->current);
    l->axiom = NULL;
    l->rules = NULL;
    l->current = NULL;
    l->axiom_len = l->rule_count = l->rule_cap = 0u;
    l->current_len = l->current_cap = l->max_len = 0u;
}

MU_INLINE uint32_t mu_lsystem_max_len(const mu_lsystem* l) { return l->max_len; }

/* ---------- axiom / rules ---------- */

/* Set the axiom (copied). Returns false if it exceeds max_len. */
MU_INLINE bool mu_lsystem_set_axiom(mu_lsystem* l, const char* axiom)
{
    MU_ASSERT(l != NULL);
    MU_ASSERT(axiom != NULL);
    uint32_t len = (uint32_t)strlen(axiom);
    if(len > l->max_len)
        return false;
    MU_MEMCPY(l->axiom, axiom, len + 1u);
    l->axiom_len = len;
    return true;
}

/* Add / override a production for `symbol`. `replacement` is copied.
 * Returns false on allocation failure. */
MU_INLINE bool mu_lsystem_add_rule(mu_lsystem* l, char symbol,
                                   const char* replacement)
{
    MU_ASSERT(l != NULL);
    MU_ASSERT(replacement != NULL);
    uint32_t len = (uint32_t)strlen(replacement);

    /* Override an existing rule for the same symbol. */
    for(uint32_t i = 0; i < l->rule_count; ++i)
    {
        if(l->rules[i].symbol == symbol)
        {
            char* rep = (char*)MU_REALLOC(
                l->rules[i].replacement, sizeof(char) * (len + 1u));
            if(!rep)
                return false;
            l->rules[i].replacement = rep;
            MU_MEMCPY(rep, replacement, len + 1u);
            l->rules[i].replacement_len = len;
            return true;
        }
    }

    if(l->rule_count == l->rule_cap)
    {
        uint32_t nc = l->rule_cap * 2u;
        mu_lsystem_rule* nr = (mu_lsystem_rule*)MU_REALLOC(
            l->rules, sizeof(mu_lsystem_rule) * nc);
        if(!nr)
            return false;
        l->rules = nr;
        l->rule_cap = nc;
    }
    uint32_t i = l->rule_count++;
    l->rules[i].symbol = symbol;
    l->rules[i].replacement = (char*)MU_MALLOC(sizeof(char) * (len + 1u));
    if(!l->rules[i].replacement)
        return false;
    MU_MEMCPY(l->rules[i].replacement, replacement, len + 1u);
    l->rules[i].replacement_len = len;
    return true;
}

/* Remove any production for `symbol`. */
MU_INLINE void mu_lsystem_remove_rule(mu_lsystem* l, char symbol)
{
    MU_ASSERT(l != NULL);
    for(uint32_t i = 0; i < l->rule_count; ++i)
    {
        if(l->rules[i].symbol == symbol)
        {
            MU_FREE(l->rules[i].replacement);
            if(i + 1u < l->rule_count)
                l->rules[i] = l->rules[l->rule_count - 1u];
            --l->rule_count;
            return;
        }
    }
}

/* ---------- rewriting ---------- */

/* Reset the working string to the axiom. */
MU_INLINE void mu_lsystem_reset(mu_lsystem* l)
{
    MU_ASSERT(l != NULL);
    MU_MEMCPY(l->current, l->axiom, l->axiom_len + 1u);
    l->current_len = l->axiom_len;
}

/* Rewrite once: every symbol is replaced by its production (or kept if none
 * exists). Returns false if the result would exceed max_len. */
MU_INLINE bool mu_lsystem_iterate(mu_lsystem* l)
{
    MU_ASSERT(l != NULL);
    if(l->current_len == 0u)
        return true;

    /* worst case: every symbol expands to its longest replacement */
    uint32_t max_repl = 0u;
    for(uint32_t i = 0; i < l->rule_count; ++i)
        if(l->rules[i].replacement_len > max_repl)
            max_repl = l->rules[i].replacement_len;
    uint32_t needed = l->current_len * (max_repl ? max_repl : 1u);
    if(needed > l->max_len)
        return false;

    char* next = (char*)MU_MALLOC(sizeof(char) * (l->max_len + 1u));
    if(!next)
        return false;

    uint32_t write = 0u;
    for(uint32_t i = 0; i < l->current_len; ++i)
    {
        char sym = l->current[i];
        bool found = false;
        for(uint32_t r = 0; r < l->rule_count; ++r)
        {
            if(l->rules[r].symbol == sym)
            {
                uint32_t rlen = l->rules[r].replacement_len;
                if(write + rlen > l->max_len)
                {
                    MU_FREE(next);
                    return false;
                }
                MU_MEMCPY(next + write, l->rules[r].replacement, rlen);
                write += rlen;
                found = true;
                break;
            }
        }
        if(!found)
        {
            if(write >= l->max_len)
            {
                MU_FREE(next);
                return false;
            }
            next[write++] = sym;
        }
    }
    next[write] = '\0';

    MU_FREE(l->current);
    l->current = next;
    l->current_len = write;
    return true;
}

/* Rewrite `times` times from the axiom. Stops early on overflow. Returns the
 * number of full iterations completed. */
MU_INLINE uint32_t mu_lsystem_iterate_n(mu_lsystem* l, uint32_t times)
{
    MU_ASSERT(l != NULL);
    mu_lsystem_reset(l);
    uint32_t done = 0u;
    while(done < times && mu_lsystem_iterate(l))
        ++done;
    return done;
}

/* Pointer to the current (expanded) null-terminated string. */
MU_INLINE const char* mu_lsystem_string(const mu_lsystem* l) { return l->current; }
MU_INLINE uint32_t mu_lsystem_length(const mu_lsystem* l) { return l->current_len; }

/* ---------- turtle interpretation ---------- */

/* Interpret the current string into segments.
 *
 * `step` is the forward distance per 'F'/'f'. `angle` overrides the
 * per-turn angle if non-zero (radians); otherwise the system's stored angle
 * is used (there is no stored angle field, so pass the angle here directly).
 *
 * Writes at most `max_segments` segments and returns the number written.
 * `[`/`']` push/pop (x,y,angle); an unbalanced ']' is ignored, an unbalanced
 * '[' is ignored once a small internal stack (depth 64) is full.
 */
MU_INLINE uint32_t mu_lsystem_interpret(const mu_lsystem* l,
                                        float step, float angle,
                                        mu_lsystem_segment* out,
                                        uint32_t max_segments)
{
    MU_ASSERT(l != NULL);
    MU_ASSERT(out != NULL || max_segments == 0u);

    float x = 0.0f, y = 0.0f, a = 0.0f;

    /* tiny fixed stack for bracket states */
    float sx[64], sy[64], sa[64];
    uint32_t sp = 0u;

    uint32_t n = 0u;
    const char* s = l->current;
    for(uint32_t i = 0; i < l->current_len && n < max_segments; ++i)
    {
        char c = s[i];
        switch (c)
        {
            case 'F':
            {
                float nx = x + step * cosf(a);
                float ny = y + step * sinf(a);
                if(n < max_segments)
                {
                    out[n].x0 = x;
                    out[n].y0 = y;
                    out[n].x1 = nx;
                    out[n].y1 = ny;
                    ++n;
                }
                x = nx;
                y = ny;
                break;
            }
            case 'f':
            {
                x += step * cosf(a);
                y += step * sinf(a);
                break;
            }
            case '+':
                a += angle;
                break;
            case '-':
                a -= angle;
                break;
            case '[':
                if(sp < 64u)
                {
                    sx[sp] = x;
                    sy[sp] = y;
                    sa[sp] = a;
                    ++sp;
                }
                break;
            case ']':
                if(sp > 0u)
                {
                    --sp;
                    x = sx[sp];
                    y = sy[sp];
                    a = sa[sp];
                }
                break;
            default:
                /* no-op: reserved for leaves, flowers, etc. */
                break;
        }
    }
    return n;
}

MU_END_EXTERN_C
