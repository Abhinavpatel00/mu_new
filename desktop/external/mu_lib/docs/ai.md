# The mu/ai stack

The AI modules live in `mu/ai/`. They are single headers like the rest of the
library: C99, no allocation where avoidable, no globals, and every random
decision runs through a caller-owned rng state so a replay reproduces it
bit-for-bit (the `mu_fixed` determinism story, applied to behavior).

This page is the tour: how the pieces compose into an agent.

## The stack, bottom to top

```
        mu_director          pacing: WHEN the world acts (tension curve)
        mu_playermodel       WHO the player is (features, drift)
        mu_mood              HOW the agent feels (appraisal -> mood bias)
   ┌───────────────────────────────────────────────────────────┐
   │  decisions:  mu_utility (score) -> mu_behavior/mu_htn/    │
   │              mu_goap/mu_production/mu_petri (structure),  │
   │              mu_mcts / mu_rhea (search)                   │
   ├───────────────────────────────────────────────────────────┤
   │  knowledge:  mu_blackboard (shared), mu_influence (maps)  │
   ├───────────────────────────────────────────────────────────┤
   │  perception: mu_senses (vision cone x light x motion,     │
   │              hearing, awareness ladder, last-known pos)   │
   ├───────────────────────────────────────────────────────────┤
   │  action:     mu_steering (seek/arrive/flock/formations),  │
   │              mu_micro (target ledger, kiting, flee)       │
   └───────────────────────────────────────────────────────────┘
        mu_replay            every tick above is recordable/rollback-able
        mu_bandit/mu_genetic tune the knobs in the layers above
        mu_elo               score the agents against each other
```

## One agent, one tick

```c
/* perceive (no cheating: only caller-supplied signals) */
float vis = mu_sense_vision(e->pos, e->fwd, 1.0f, 12.0f, p->pos,
                            world.light_at(p->pos), p->speed01, 0.1f);
mu_awareness_stimulate(&e->aw, vis, MU_SENSE_CAUSE_VISION, p->pos);
mu_awareness_update(&e->aw, dt);

/* decide: score candidate actions, structured by a BT/HTN if you have one */
float u_fight = mu_utility_score(curves, inputs, 3, 2.0f);
float u_flee  = ...;
int   action  = (u_fight > u_flee) ? ACT_FIGHT : ACT_FLEE;

/* act */
mu_steering_arrive(&e->agent, target, 2.0f, force);
mu_steering_integrate(&e->agent, force, dt);

/* record for rollback/replays */
mu_replay_record(&replay, tick, packed_inputs);
```

## Composition notes

- **`mu_utility` under everything.** BT leaves, HTN methods, GOAP operator
  costs, and MCTS evaluations can all call the same curve+aggregation scorer,
  so tuning lives in one place. Mood biases its inputs (`mu_mood_bias`).
- **`mu_director` is not the agent.** It is the layer that paces when threats
  and relief arrive (L4D's formula, RimWorld's "story watchers", the Alien
  menace gauge). Its events spawn or retire agents; it never moves them.
- **`mu_senses` never cheats.** Everything is computed from signals the game
  actually has. The `why` bitmask is the light-gem hook: expose it to the
  player or the system is unplayable.
- **`mu_micro` is the RTS layer.** The future-damage ledger turns focus fire
  from overkill into deletions; the three kiting knobs (D_k, s_t, D_kb) and
  the flee threshold are exactly the parameters `mu_bandit`/`mu_genetic`
  should be tuning, not you.
- **`mu_replay` under all of it.** Inputs + state digests + rollback: the
  netcode contract in its smallest honest form. Pair with `mu_fixed` for
  deterministic math and `mu_save` for persistence.
- **Determinism rule (AGENTS.md 3.18):** AI modules take an explicit
  `uint32_t* rng` (or a stream) and never call the platform RNG. Same seed,
  same behavior, across `-O0/-O2/-O3` and across the C99/C++14 TUs.

## The gate

`perf_test/test_ai.c` (`make -C perf_test ai-test`) asserts the invariants
research says these modules exist for: response-curve bounds and monotonicity,
arrive convergence, no-collision separation, awareness ladder rise/decay,
pacing min-rest + determinism, UCB/Thompson finding the best arm, kiting
reducing taken damage, GOAP plan validity + optimality on a known domain,
Elo convergence, rollback detecting a one-bit flip, mood decay, CUSUM ignoring
noise but tripping on shift, and RHEA finding the gradient peak.

## Sources (the research behind the shapes)

Utility response curves (Dave Mark, GDC 2013); steering (Reynolds, GDC 1999);
kiting parameters and the tune-don't-hand-set rule (CIG 2014 ECSLBot,
arXiv:1803.10314); overkill/focus-fire (SMAC, arXiv:1902.04043); GOAP (Orkin,
GDC 2005); the pacing formula (Valve's L4D AI Director, GDC 2009) and
storyteller clamps (RimWorld); the menace gauge (Alien: Isolation); the
awareness ladder and light-gem legibility (Thief: The Dark Project); EMA-lite
appraisal (Marsella & Gratch); player features (Yannakakis & Togelius);
RHEA (Gaina et al., IEEE ToG 2020); rollback/prediction (QuakeWorld; Gambetta;
Fiedler).
