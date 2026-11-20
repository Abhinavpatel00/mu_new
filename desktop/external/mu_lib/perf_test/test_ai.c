/* test_ai.c — gate for the mu/ai stack (mu_utility, mu_steering, mu_senses,
 * mu_director, mu_bandit, mu_micro, mu_goap, mu_elo, mu_replay, mu_mood,
 * mu_playermodel, mu_rhea).
 *
 * Structure per module: invariants that must hold for ANY inputs (bounds,
 * monotonicity, convergence, determinism), then the cases the research says
 * the module exists for (kiting reduces damage, UCB finds the best arm,
 * the GOAP plan validates, rollback detects a flipped bit).
 *
 *   cc -std=c99 -O2 -I.. -I../mu -o test_ai test_ai.c -lm && ./test_ai
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "mu/ai/mu_utility.h"
#include "mu/ai/mu_steering.h"
#include "mu/ai/mu_senses.h"
#include "mu/ai/mu_director.h"
#include "mu/ai/mu_bandit.h"
#include "mu/ai/mu_micro.h"
#include "mu/ai/mu_goap.h"
#include "mu/ai/mu_elo.h"
#include "mu/ai/mu_replay.h"
#include "mu/ai/mu_mood.h"
#include "mu/ai/mu_playermodel.h"
#include "mu/ai/mu_rhea.h"

static long g_checks = 0;
static long g_fails  = 0;

static void check_i(const char* what, long long got, long long want)
{
    ++g_checks;
    if (got != want)
    {
        ++g_fails;
        if (g_fails <= 20)
            printf("  FAIL %-52s got=%lld want=%lld\n", what, got, want);
    }
}

static void check_f(const char* what, float got, float want, float eps)
{
    ++g_checks;
    if (fabsf(got - want) > eps)
    {
        ++g_fails;
        if (g_fails <= 20)
            printf("  FAIL %-52s got=%.5f want=%.5f\n", what, got, want);
    }
}

static void check_b(const char* what, int cond)
{
    ++g_checks;
    if (!cond)
    {
        ++g_fails;
        if (g_fails <= 20)
            printf("  FAIL %-52s\n", what);
    }
}

/* ============================== mu_utility ============================== */

static void test_utility(void)
{
    mu_curve lin = mu_curve_make(MU_CURVE_LINEAR, 0, 0);
    mu_curve pow2 = mu_curve_make(MU_CURVE_POWER, 2.0f, 0);
    mu_curve logi = mu_curve_make(MU_CURVE_LOGISTIC, 0.5f, 10.0f);
    mu_curve sine = mu_curve_make(MU_CURVE_SINE, 0, 0);
    mu_curve step = mu_curve_make(MU_CURVE_STEP, 0.5f, 0);
    mu_curve inv  = mu_curve_make(MU_CURVE_INVERSE, 0, 0);
    int i;
    float prev;

    /* bounds + endpoints for every curve */
    {
        mu_curve all[6]; all[0]=lin; all[1]=pow2; all[2]=logi;
        all[3]=sine; all[4]=step; all[5]=inv;
        for (i = 0; i < 6; ++i)
        {
            int k;
            for (k = 0; k <= 100; ++k)
            {
                float x = (float)k / 100.0f;
                float y = mu_curve_eval(&all[i], x);
                check_b("curve output in [0,1]", y >= 0.0f && y <= 1.0f);
            }
        }
    }
    check_f("linear f(0.25)", mu_curve_eval(&lin, 0.25f), 0.25f, 1e-6f);
    check_f("power2 f(0.5)", mu_curve_eval(&pow2, 0.5f), 0.25f, 1e-6f);
    check_f("logistic f(0)", mu_curve_eval(&logi, 0.0f), 0.0f, 1e-5f);
    check_f("logistic f(1)", mu_curve_eval(&logi, 1.0f), 1.0f, 1e-5f);
    check_f("logistic f(mid)=.5", mu_curve_eval(&logi, 0.5f), 0.5f, 1e-5f);
    check_f("sine f(0)", mu_curve_eval(&sine, 0.0f), 0.0f, 1e-6f);
    check_f("sine f(1)", mu_curve_eval(&sine, 1.0f), 1.0f, 1e-6f);
    check_f("step f(0.49)", mu_curve_eval(&step, 0.49f), 0.0f, 1e-6f);
    check_f("step f(0.5)", mu_curve_eval(&step, 0.5f), 1.0f, 1e-6f);
    check_f("inverse f(0.3)", mu_curve_eval(&inv, 0.3f), 0.7f, 1e-6f);
    check_f("clamp low", mu_curve_eval(&lin, -3.0f), 0.0f, 1e-6f);
    check_f("clamp high", mu_curve_eval(&lin, 3.0f), 1.0f, 1e-6f);

    /* monotone non-decreasing for rising curves */
    prev = -1.0f;
    for (i = 0; i <= 20; ++i)
    {
        float x = (float)i / 20.0f;
        float y = mu_curve_eval(&pow2, x);
        check_b("power curve monotone", y >= prev - 1e-6f);
        prev = y;
    }

    /* aggregation */
    {
        float v[3] = { 0.5f, 0.5f, 0.5f };
        check_f("mul agg", mu_utility_aggregate_mul(v, 3), 0.125f, 1e-6f);
        check_f("min agg", mu_utility_aggregate_min(v, 3), 0.5f, 1e-6f);
        check_f("mean agg", mu_utility_aggregate_mean(v, 3), 0.5f, 1e-6f);
    }
    check_f("compensate identity k=1", mu_utility_compensate(0.3f, 1.0f), 0.3f, 1e-6f);
    check_f("compensate(1)=1", mu_utility_compensate(1.0f, 3.0f), 1.0f, 1e-6f);
    check_f("compensate(0)=0", mu_utility_compensate(0.0f, 3.0f), 0.0f, 1e-6f);
    check_b("compensate lifts", mu_utility_compensate(0.2f, 3.0f) > 0.2f);
    check_f("lowpass mid", mu_utility_lowpass(0.0f, 1.0f, 0.5f), 0.5f, 1e-6f);

    /* one-shot score: product of three linear 0.5 inputs */
    {
        mu_curve cs[3]; float ins[3] = { 0.5f, 0.5f, 0.5f };
        cs[0]=lin; cs[1]=lin; cs[2]=lin;
        check_f("utility_score", mu_utility_score(cs, ins, 3, 0.0f), 0.125f, 1e-6f);
    }
}

/* ============================== mu_steering ============================= */

static void test_steering(void)
{
    mu_steering_agent a;
    float f[2];
    int i;

    /* seek points at the target */
    memset(&a, 0, sizeof a);
    a.pos[0] = 0; a.pos[1] = 0;
    a.max_speed = 5.0f; a.max_accel = 10.0f;
    {
        float t[2] = { 10.0f, 0.0f };
        mu_steering_seek(&a, t, f);
        check_b("seek x>0", f[0] > 0.0f);
        check_f("seek y=0", f[1], 0.0f, 1e-5f);
    }

    /* arrive converges: simulate 600 ticks toward target */
    {
        float t[2] = { 10.0f, 3.0f };
        float dx, dy;
        for (i = 0; i < 600; ++i)
        {
            mu_steering_arrive(&a, t, 3.0f, f);
            mu_steering_integrate(&a, f, 1.0f / 60.0f);
        }
        dx = a.pos[0] - t[0]; dy = a.pos[1] - t[1];
        check_b("arrive converged", dx * dx + dy * dy < 0.25f);
        check_b("arrive stopped", sqrtf(a.vel[0]*a.vel[0] + a.vel[1]*a.vel[1]) < 1.0f);
    }

    /* separation pushes apart two agents */
    {
        mu_steering_agent b;
        float nb[2];
        memset(&b, 0, sizeof b);
        a.pos[0] = 0; a.pos[1] = 0; a.vel[0] = 0; a.vel[1] = 0;
        b.pos[0] = 0.5f; b.pos[1] = 0;
        nb[0] = b.pos[0]; nb[1] = b.pos[1];
        mu_steering_separation(&a, nb, 1, 2.0f, f);
        check_b("separation pushes -x", f[0] < 0.0f);
    }

    /* wander determinism: same seed => same trajectory */
    {
        mu_steering_agent w1, w2;
        uint32_t r1 = 12345u, r2 = 12345u;
        float f1[2], f2[2];
        int same = 1;
        memset(&w1, 0, sizeof w1);
        memset(&w2, 0, sizeof w2);
        w1.max_speed = w2.max_speed = 3.0f;
        w1.max_accel = w2.max_accel = 8.0f;
        w1.vel[0] = w2.vel[0] = 1.0f;
        for (i = 0; i < 100; ++i)
        {
            mu_steering_wander(&w1, &r1, 1.0f, 0.5f, f1);
            mu_steering_wander(&w2, &r2, 1.0f, 0.5f, f2);
            mu_steering_integrate(&w1, f1, 0.05f);
            mu_steering_integrate(&w2, f2, 0.05f);
            if (w1.pos[0] != w2.pos[0] || w1.pos[1] != w2.pos[1]) same = 0;
        }
        check_i("wander deterministic", same, 1);
    }

    /* formation slot: goal-side offset lands ahead of leader */
    {
        float lpos[2] = { 0, 0 }, lhead[2] = { 0, 1 }, off[2] = { 0, 2 };
        mu_steering_agent c;
        float got[2];
        memset(&c, 0, sizeof c);
        c.max_speed = 5; c.max_accel = 10;
        c.pos[0] = 0; c.pos[1] = -3;
        mu_steering_formation_slot(&c, lpos, lhead, off, got);
        check_b("formation slot aims ahead", got[1] > 0.0f);
    }

    /* path follow reaches the end */
    {
        float pts[6] = { 0,0, 5,0, 10,0 };
        int end = 0;
        mu_steering_agent p;
        memset(&p, 0, sizeof p);
        p.max_speed = 5; p.max_accel = 10;
        for (i = 0; i < 800 && !end; ++i)
        {
            mu_steering_path_follow(&p, pts, 3, 1.0f, f, &end);
            mu_steering_integrate(&p, f, 1.0f / 60.0f);
        }
        check_i("path follow ends", end, 1);
    }

    /* speed clamp invariant */
    {
        float big[2] = { 1000.0f, 0.0f };
        a.max_speed = 4.0f;
        mu_steering_integrate(&a, big, 1.0f);
        check_b("speed clamped", sqrtf(a.vel[0]*a.vel[0] + a.vel[1]*a.vel[1]) <= 4.001f);
    }
}

/* =============================== mu_senses ============================== */

static void test_senses(void)
{
    float o[2] = { 0, 0 }, fwd[2] = { 1, 0 };
    float ahead[2] = { 5, 0 }, behind[2] = { -5, 0 }, side[2] = { 0, 5 };
    mu_awareness aw;
    float pos[2] = { 3, 0 };

    /* vision: ahead lit still = strong; behind = 0 */
    check_f("vision ahead full", mu_sense_vision(o, fwd, 1.0f, 10.0f, ahead, 1.0f, 0.0f, 0.1f), 0.5f, 0.1f);
    check_f("vision behind", mu_sense_vision(o, fwd, 1.0f, 10.0f, behind, 1.0f, 0.0f, 0.1f), 0.0f, 1e-6f);
    check_f("vision out of range", mu_sense_vision(o, fwd, 1.0f, 2.0f, ahead, 1.0f, 0.0f, 0.1f), 0.0f, 1e-6f);
    /* light monotonicity */
    check_b("vision monotone in light",
            mu_sense_vision(o, fwd, 1.0f, 10.0f, ahead, 1.0f, 0.0f, 0.1f) >
            mu_sense_vision(o, fwd, 1.0f, 10.0f, ahead, 0.2f, 0.0f, 0.1f));
    check_b("vision cone excludes side",
            mu_sense_vision(o, fwd, 0.5f, 10.0f, side, 1.0f, 0.0f, 0.1f) == 0.0f);
    /* motion raises visibility */
    check_b("motion raises visibility",
            mu_sense_vision(o, fwd, 1.0f, 10.0f, ahead, 0.6f, 1.0f, 0.1f) >
            mu_sense_vision(o, fwd, 1.0f, 10.0f, ahead, 0.6f, 0.0f, 0.1f));

    /* hearing: distance falloff + occlusion */
    check_b("hearing near > far",
            mu_sense_hearing(1.0f, 1.0f, 0.0f, 10.0f) >
            mu_sense_hearing(1.0f, 9.0f, 0.0f, 10.0f));
    check_f("hearing full occlusion", mu_sense_hearing(1.0f, 1.0f, 1.0f, 10.0f), 0.0f, 1e-6f);
    check_f("hearing beyond range", mu_sense_hearing(1.0f, 11.0f, 0.0f, 10.0f), 0.0f, 1e-6f);

    /* awareness ladder: rises through thresholds, decays down */
    mu_awareness_init(&aw);
    check_i("aware starts idle", aw.level, MU_AWARE_IDLE);
    mu_awareness_stimulate(&aw, 0.3f, MU_SENSE_CAUSE_VISION, pos);
    check_i("aware suspicious", aw.level, MU_AWARE_SUSPICIOUS);
    mu_awareness_stimulate(&aw, 0.4f, MU_SENSE_CAUSE_HEARING, pos);
    check_i("aware searching", aw.level, MU_AWARE_SEARCHING);
    mu_awareness_stimulate(&aw, 0.4f, MU_SENSE_CAUSE_VISION, pos);
    check_i("aware alert", aw.level, MU_AWARE_ALERT);
    check_b("last known stored", aw.has_last_known && aw.last_known[0] == 3.0f);
    check_i("why records cause", (int)(aw.why & MU_SENSE_CAUSE_VISION), (int)MU_SENSE_CAUSE_VISION);
    /* decay: after long idle, back to idle */
    {
        int t;
        for (t = 0; t < 200; ++t) mu_awareness_update(&aw, 0.5f);
    }
    check_i("aware decays to idle", aw.level, MU_AWARE_IDLE);
    /* forget clears last-known */
    mu_awareness_stimulate(&aw, 1.0f, MU_SENSE_CAUSE_CORPSE, pos);
    mu_awareness_forget(&aw);
    check_i("forget clears last known", aw.has_last_known, 0);
}

/* ============================== mu_director ============================= */

static void test_director(void)
{
    mu_director d;
    int i, fired = 0, threats = 0, reliefs = 0;

    mu_director_init_preset(&d, 0, 42u);
    mu_director_add_event(&d, 1.0f, 10.0f, MU_DIRECTOR_THREAT, 0.15f);
    mu_director_add_event(&d, 1.0f, 15.0f, MU_DIRECTOR_RELIEF, -0.25f);
    mu_director_add_event(&d, 0.5f, 5.0f, MU_DIRECTOR_AMBIENT, 0.0f);

    check_b("time to event >= min_rest",
            mu_director_time_to_event(&d) >= d.min_rest - 1e-4f);

    /* min_rest is honored even with a starving player */
    mu_director_feed(&d, 0.0f, 1.0f, 0.1f);
    check_i("no fire inside min_rest", mu_director_should_fire(&d, 0.1f), 0);

    /* drive 20 minutes of tension and count fires */
    for (i = 0; i < 12000; ++i)
    {
        float well = 0.4f + 0.3f * sinf((float)i * 0.002f);
        mu_director_feed(&d, well, 0.5f, 0.1f);
        if (mu_director_should_fire(&d, 0.1f))
        {
            int idx = mu_director_pick(&d);
            if (idx >= 0)
            {
                mu_director_notify_fired(&d, idx);
                ++fired;
                if (d.events[idx].kind == MU_DIRECTOR_THREAT) ++threats;
                if (d.events[idx].kind == MU_DIRECTOR_RELIEF) ++reliefs;
            }
        }
        /* tension stays in [0,1] under all feeds */
        if (d.tension < 0.0f || d.tension > 1.0f) { check_b("tension bounded", 0); }
    }
    check_b("director fired events", fired > 3);
    check_b("director fired threats", threats >= 1);
    check_b("tension bounded after run", d.tension >= 0.0f && d.tension <= 1.0f);

    /* determinism: same seed and feed => same fire count */
    {
        mu_director d2;
        int fired2 = 0;
        mu_director_init_preset(&d2, 0, 42u);
        mu_director_add_event(&d2, 1.0f, 10.0f, MU_DIRECTOR_THREAT, 0.15f);
        mu_director_add_event(&d2, 1.0f, 15.0f, MU_DIRECTOR_RELIEF, -0.25f);
        mu_director_add_event(&d2, 0.5f, 5.0f, MU_DIRECTOR_AMBIENT, 0.0f);
        for (i = 0; i < 12000; ++i)
        {
            float well = 0.4f + 0.3f * sinf((float)i * 0.002f);
            mu_director_feed(&d2, well, 0.5f, 0.1f);
            if (mu_director_should_fire(&d2, 0.1f))
            {
                int idx = mu_director_pick(&d2);
                if (idx >= 0) { mu_director_notify_fired(&d2, idx); ++fired2; }
            }
        }
        check_i("director deterministic", fired2, fired);
        check_f("director tension matches", d2.tension, d.tension, 0.0f);
    }
}

/* =============================== mu_bandit ============================== */

static void test_bandit(void)
{
    /* arm means: arm 3 is best (0.8) */
    static const float means[6] = { 0.1f, 0.2f, 0.3f, 0.8f, 0.4f, 0.25f };
    mu_bandit b;
    uint32_t rng = 777u;
    int i, counts[6];
    int best_arm;

    mu_bandit_init(&b, MU_BANDIT_UCB1, 6);
    for (i = 0; i < 6000; ++i)
    {
        int arm = mu_bandit_choose(&b, &rng);
        /* deterministic Bernoulli from the rng */
        float r = mu_bandit_randf(&rng) < means[arm] ? 1.0f : 0.0f;
        mu_bandit_update(&b, arm, r);
    }
    for (i = 0; i < 6; ++i) counts[i] = (int)b.pulls[i];
    best_arm = 0;
    for (i = 1; i < 6; ++i) if (counts[i] > counts[best_arm]) best_arm = i;
    check_i("UCB1 finds best arm", best_arm, 3);
    check_b("UCB1 dominates on best arm", counts[3] > counts[0] * 3);

    /* Thompson also concentrates */
    {
        mu_bandit t;
        uint32_t r2 = 555u;
        int tcounts[6];
        mu_bandit_init(&t, MU_BANDIT_THOMPSON, 6);
        for (i = 0; i < 4000; ++i)
        {
            int arm = mu_bandit_choose(&t, &r2);
            float r = mu_bandit_randf(&r2) < means[arm] ? 1.0f : 0.0f;
            mu_bandit_update(&t, arm, r);
        }
        for (i = 0; i < 6; ++i) tcounts[i] = (int)t.pulls[i];
        best_arm = 0;
        for (i = 1; i < 6; ++i) if (tcounts[i] > tcounts[best_arm]) best_arm = i;
        check_i("Thompson finds best arm", best_arm, 3);
    }

    /* epsilon-greedy with epsilon 0 exploits the established leader */
    {
        mu_bandit e;
        uint32_t r3 = 1u;
        mu_bandit_init(&e, MU_BANDIT_EPS_GREEDY, 3);
        e.epsilon = 0.0f;
        mu_bandit_update(&e, 0, 0.0f);
        mu_bandit_update(&e, 1, 1.0f);
        mu_bandit_update(&e, 2, 0.0f);
        check_i("eps0 greedy picks leader", mu_bandit_choose(&e, &r3), 1);
    }

    /* rewards clamp */
    mu_bandit_init(&b, MU_BANDIT_UCB1, 2);
    mu_bandit_update(&b, 0, 5.0f);
    mu_bandit_update(&b, 0, -5.0f);
    check_f("reward clamped to [0,1]", b.reward_sum[0], 1.0f, 1e-6f);

    /* Beta sampler sanity: mean of Beta(2,2) ~ 0.5 */
    {
        uint32_t r4 = 99u;
        float s = 0.0f;
        for (i = 0; i < 2000; ++i) s += mu_bandit_sample_beta(2.0f, 2.0f, &r4);
        s /= 2000.0f;
        check_b("Beta(2,2) mean ~0.5", s > 0.42f && s < 0.58f);
    }
}

/* =============================== mu_micro =============================== */

static void test_micro(void)
{
    mu_damage_ledger l;
    mu_kite k;
    mu_flee fl;
    float move_d;
    int i;

    /* ledger: effective HP accounts for committed damage */
    mu_ledger_clear(&l);
    mu_ledger_commit(&l, 2, 30.0f);
    check_f("effective hp", mu_ledger_effective_hp(&l, 2, 50.0f), 20.0f, 1e-6f);
    check_f("effective hp floor", mu_ledger_effective_hp(&l, 2, 10.0f), 0.0f, 1e-6f);
    mu_ledger_release(&l, 2, 30.0f);
    check_f("release restores", mu_ledger_effective_hp(&l, 2, 50.0f), 50.0f, 1e-6f);

    /* target selection skips the already-doomed enemy */
    {
        float hp[3]  = { 30.0f, 5.0f, 80.0f };
        float dps[3] = { 10.0f, 50.0f, 8.0f };
        float mm[3]  = { 10.0f, 10.0f, 10.0f };
        float dist[3]= { 5.0f, 5.0f, 5.0f };
        int pick;
        mu_ledger_clear(&l);
        mu_ledger_commit(&l, 1, 5.0f);   /* enemy 1 is dead already */
        pick = mu_micro_select_target(hp, dps, mm, dist, 3, &l);
        check_b("skips doomed enemy", pick != 1);
        /* kill-fastest: enemy 0 (30hp) vs 2 (80hp) at same dps -> 0 */
        check_i("kill fastest", pick, 0);
    }

    /* kiting: Hold->Back->Pause->Hold with the documented knobs */
    mu_kite_init(&k, 6.0f, 0.2f, 2.5f);
    check_i("kite starts hold", k.state, MU_KITE_HOLD);
    (void)mu_kite_tick(&k, 10.0f, 1, 0.016f, &move_d);   /* far: stays hold */
    check_i("kite stays hold far", k.state, MU_KITE_HOLD);
    (void)mu_kite_tick(&k, 3.0f, 1, 0.016f, &move_d);    /* close: -> BACK */
    check_i("kite back", k.state, MU_KITE_BACK);
    (void)mu_kite_tick(&k, 5.5f, 1, 0.016f, &move_d);    /* retreat tick */
    check_f("kite retreat distance", move_d, 2.5f, 1e-5f);
    check_i("kite fires after retreat", mu_kite_tick(&k, 5.5f, 1, 0.016f, &move_d), 1);
    check_i("kite pause", k.state, MU_KITE_PAUSE);
    for (i = 0; i < 30; ++i) (void)mu_kite_tick(&k, 10.0f, 1, 0.016f, &move_d);
    check_i("kite returns to hold when far", k.state, MU_KITE_HOLD);

    /* flee hysteresis: no oscillation at the boundary */
    mu_flee_init(&fl, 0.3f, 0.15f);
    check_i("flee off initially", mu_flee_tick(&fl, 0.9f), 0);
    check_i("flee on below threshold", mu_flee_tick(&fl, 0.2f), 1);
    check_i("flee stays on at boundary", mu_flee_tick(&fl, 0.35f), 1);
    check_i("flee stays on mid-band", mu_flee_tick(&fl, 0.40f), 1);
    check_i("flee off above hysteresis", mu_flee_tick(&fl, 0.6f), 0);
}

/* ================================ mu_goap =============================== */

static void test_goap(void)
{
    /* domain: 3 facts — armed(0), at_enemy(1), enemy_dead(2).
     * ops: pick_up_weapon (pre: none, add: 0), approach (pre: 0, add: 1),
     *      attack (pre: 0,1, add: 2), flee (pre: none, add: none, cost 0.5) */
    mu_goap_op ops[4];
    mu_goap_state start, goal, armed;
    int plan[MU_GOAP_MAX_PLAN];
    int len;

    memset(ops, 0, sizeof ops);
    mu_goap_state_clear(&ops[0].pre);
    mu_goap_state_clear(&ops[0].add);
    mu_goap_state_clear(&ops[0].del);
    mu_goap_state_set(&ops[0].add, 0);
    ops[0].cost = 1.0f;

    mu_goap_state_clear(&ops[1].pre);
    mu_goap_state_set(&ops[1].pre, 0);
    mu_goap_state_clear(&ops[1].add);
    mu_goap_state_set(&ops[1].add, 1);
    mu_goap_state_clear(&ops[1].del);
    ops[1].cost = 1.0f;

    mu_goap_state_clear(&ops[2].pre);
    mu_goap_state_set(&ops[2].pre, 0);
    mu_goap_state_set(&ops[2].pre, 1);
    mu_goap_state_clear(&ops[2].add);
    mu_goap_state_set(&ops[2].add, 2);
    mu_goap_state_clear(&ops[2].del);
    ops[2].cost = 1.0f;

    mu_goap_state_clear(&ops[3].pre);
    mu_goap_state_clear(&ops[3].add);
    mu_goap_state_clear(&ops[3].del);
    ops[3].cost = 0.5f;

    mu_goap_state_clear(&start);
    mu_goap_state_clear(&goal);
    mu_goap_state_set(&goal, 2);

    len = mu_goap_plan(ops, 4, &start, &goal, plan, MU_GOAP_MAX_PLAN);
    check_i("goap plan length", len, 3);
    check_i("goap op sequence", (len == 3 && plan[0] == 0 && plan[1] == 1 && plan[2] == 2), 1);
    check_i("goap plan validates", mu_goap_check_plan(ops, &start, plan, len, &goal), 1);

    /* optimal when already armed: 2 steps, not 3 */
    mu_goap_state_clear(&armed);
    mu_goap_state_set(&armed, 0);
    len = mu_goap_plan(ops, 4, &armed, &goal, plan, MU_GOAP_MAX_PLAN);
    check_i("goap skips unneeded ops", len, 2);

    /* unreachable goal */
    {
        mu_goap_state impossible;
        mu_goap_state_clear(&impossible);
        mu_goap_state_set(&impossible, 63);   /* no op ever adds fact 63 */
        len = mu_goap_plan(ops, 4, &start, &impossible, plan, MU_GOAP_MAX_PLAN);
        check_i("goap fails unreachable", len, -1);
    }

    /* start == goal => empty plan */
    len = mu_goap_plan(ops, 4, &start, &start, plan, MU_GOAP_MAX_PLAN);
    check_i("goap empty plan at goal", len, 0);
}

/* ================================ mu_elo ================================ */

static void test_elo(void)
{
    float a = 1500.0f, b = 1500.0f;
    check_f("equal elo => 0.5", mu_elo_expected(1500.0f, 1500.0f), 0.5f, 1e-5f);
    check_b("higher elo favored", mu_elo_expected(1800.0f, 1500.0f) > 0.8f);
    check_f("expected sums to 1", mu_elo_expected(1400.0f, 1600.0f) +
            mu_elo_expected(1600.0f, 1400.0f), 1.0f, 1e-5f);

    mu_elo_update(&a, &b, 1.0f, 32.0f);
    check_b("winner gains", a > 1500.0f);
    check_f("zero sum", (a - 1500.0f) + (b - 1500.0f), 0.0f, 1e-4f);
    check_f("equal upset shift", a - 1500.0f, 16.0f, 1e-4f);

    /* K schedule halves once per full decay period */
    check_f("k young", mu_elo_k_schedule(32.0f, 5, 20), 32.0f, 1e-6f);
    check_f("k one period", mu_elo_k_schedule(32.0f, 20, 20), 16.0f, 1e-6f);
    check_f("k two periods", mu_elo_k_schedule(32.0f, 40, 20), 8.0f, 1e-6f);

    /* convergence: better player (p=0.7) climbs */
    {
        float ra = 1500.0f, rb = 1500.0f;
        uint32_t rng = 2024u;
        int i;
        for (i = 0; i < 2000; ++i)
        {
            float r = (float)((rng = rng * 1664525u + 1013904223u) >> 8) / 16777216.0f;
            mu_elo_update(&ra, &rb, r < 0.7f ? 1.0f : 0.0f, 16.0f);
        }
        check_b("elo converges up", ra > 1550.0f);
        check_b("elo separates", ra - rb > 100.0f);
    }
}

/* =============================== mu_replay ============================== */

static void test_replay(void)
{
    mu_replay r;
    uint32_t outs[MU_REPLAY_RING];
    int i, n;
    uint64_t d1;

    mu_replay_init(&r);
    for (i = 0; i < 100; ++i)
        mu_replay_record(&r, (uint64_t)i, (uint32_t)(i * 7 + 1));
    d1 = r.digest;

    /* rollback after tick 89: 10 inputs (90..99) */
    n = mu_replay_rollback(&r, 89, outs, 64);
    check_i("rollback count", n, 10);
    check_i("rollback first input", (int)outs[0], 90 * 7 + 1);
    check_i("rollback last input", (int)outs[9], 99 * 7 + 1);

    /* rollback after newest tick: nothing to replay */
    n = mu_replay_rollback(&r, 99, outs, 64);
    check_i("rollback empty at head", n, 0);

    /* target older than the ring => error */
    {
        mu_replay small;
        int j;
        mu_replay_init(&small);
        for (j = 0; j < 20; ++j) mu_replay_record(&small, (uint64_t)(1000 + j), 0);
        n = mu_replay_rollback(&small, 900, outs, 64);
        check_i("rollback too old refused", n, -1);
    }

    /* digest: identical streams match, one flipped bit diverges */
    {
        mu_replay ra, rb;
        int j;
        mu_replay_init(&ra);
        mu_replay_init(&rb);
        for (j = 0; j < 50; ++j)
        {
            mu_replay_record(&ra, (uint64_t)j, (uint32_t)j);
            mu_replay_record(&rb, (uint64_t)j, (uint32_t)j);
        }
        check_i("digests match", ra.digest == rb.digest, 1);
        mu_replay_record(&ra, 50u, 0xAAAAu);
        mu_replay_record(&rb, 50u, 0xAAABu);   /* one bit flipped */
        check_i("digests detect flip", ra.digest != rb.digest, 1);
    }

    /* fold_state participates in the digest */
    {
        mu_replay r2;
        uint64_t base;
        mu_replay_init(&r2);
        mu_replay_record(&r2, 0, 1);
        base = r2.digest;
        mu_replay_init(&r2);
        mu_replay_record(&r2, 0, 1);
        mu_replay_fold_state(&r2, 0xDEADBEEFu);
        check_i("state fold changes digest", r2.digest != base, 1);
    }

    /* input_digest cross-check */
    d1 = mu_replay_input_digest(&r);
    check_i("input digest stable", d1 == mu_replay_input_digest(&r), 1);
}

/* ================================ mu_mood =============================== */

static void test_mood(void)
{
    mu_mood m;
    int i;
    mu_mood_init(&m);
    check_f("mood starts neutral", m.valence, 0.0f, 1e-6f);

    /* desirable event raises joy and valence */
    mu_mood_appraise(&m, 1.0f, 0.8f, 1.0f, 1.0f, 1.0f);
    check_b("joy rises", m.emotion[MU_EMOTION_JOY] > 0.0f);
    check_b("satisfaction rises", m.emotion[MU_EMOTION_SATISFACTION] > 0.0f);
    for (i = 0; i < 50; ++i) mu_mood_update(&m, 1.0f / 60.0f);
    check_b("valence positive after joy", m.valence > 0.0f);

    /* undesirable raises distress/fear */
    mu_mood_appraise(&m, 1.0f, -0.9f, 0.5f, 0.0f, 1.0f);
    check_b("distress rises", m.emotion[MU_EMOTION_DISTRESS] > 0.0f);
    check_b("fear rises (uncertain bad)", m.emotion[MU_EMOTION_FEAR] > 0.0f);

    /* irrelevant event does nothing */
    {
        float before = m.emotion[MU_EMOTION_JOY];
        mu_mood_appraise(&m, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        check_f("irrelevant ignored", m.emotion[MU_EMOTION_JOY], before, 1e-6f);
    }

    /* decay: emotions fall to ~0 eventually */
    for (i = 0; i < 2000; ++i) mu_mood_update(&m, 0.1f);
    check_b("emotions decay", m.emotion[MU_EMOTION_DISTRESS] < 1e-3f);

    /* emotion bounds: clamp at 1 across repeated appraisals */
    for (i = 0; i < 100; ++i) mu_mood_appraise(&m, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    check_b("emotion clamped at 1", m.emotion[MU_EMOTION_JOY] <= 1.0f + 1e-6f);
    check_b("valence bounded", m.valence <= 1.0f && m.valence >= -1.0f);

    /* mood bias helper */
    check_f("mood bias neutral", mu_mood_bias(0.5f, 0.0f, 0.3f), 0.5f, 1e-6f);
    check_b("mood bias positive lifts", mu_mood_bias(0.5f, 1.0f, 0.3f) > 0.5f);
}

/* ============================ mu_playermodel ============================ */

static void test_playermodel(void)
{
    mu_playermodel pm;
    int i;
    mu_playermodel_init(&pm);

    /* known stream: mean 10, var 0 (constant) */
    for (i = 0; i < 100; ++i) mu_playermodel_observe(&pm, 0, 10.0f);
    check_f("constant mean", mu_playermodel_mean(&pm, 0), 10.0f, 1e-5f);
    check_f("constant variance", mu_playermodel_variance(&pm, 0), 0.0f, 1e-6f);

    /* 0,1,2,3,4: mean 2, variance 2.5 */
    for (i = 0; i < 5; ++i) mu_playermodel_observe(&pm, 1, (float)i);
    check_f("stream mean", mu_playermodel_mean(&pm, 1), 2.0f, 1e-5f);
    check_f("stream variance", mu_playermodel_variance(&pm, 1), 2.5f, 1e-5f);

    /* z-score: sample far from mean is large */
    check_b("z far", fabsf(mu_playermodel_z(&pm, 1, 10.0f)) > 4.0f);
    check_b("z at mean", fabsf(mu_playermodel_z(&pm, 1, 2.0f)) < 1e-5f);

    /* drift: baseline at 10, sustained shift to 20 trips CUSUM up */
    for (i = 0; i < 50; ++i) mu_playermodel_observe(&pm, 2, 10.0f);
    mu_playermodel_reset_drift(&pm, 2);
    check_i("no drift initially", mu_playermodel_drift(&pm, 2, 30.0f), 0);
    for (i = 0; i < 20; ++i) mu_playermodel_observe(&pm, 2, 20.0f);
    check_i("drift up trips", mu_playermodel_drift(&pm, 2, 30.0f), 1);
    /* reset and shift down */
    mu_playermodel_reset_drift(&pm, 2);
    for (i = 0; i < 20; ++i) mu_playermodel_observe(&pm, 2, 0.0f);
    check_i("drift down trips", mu_playermodel_drift(&pm, 2, 30.0f), -1);
    /* noise alone does not trip (warm the baseline first) */
    for (i = 0; i < 100; ++i) mu_playermodel_observe(&pm, 3, 10.0f);
    mu_playermodel_reset_drift(&pm, 3);
    {
        uint32_t rng = 123u;
        for (i = 0; i < 200; ++i)
        {
            rng = rng * 1664525u + 1013904223u;
            mu_playermodel_observe(&pm, 3, 10.0f + (float)((rng >> 16) & 1) * 2.0f - 1.0f);
        }
    }
    check_i("noise does not trip", mu_playermodel_drift(&pm, 3, 30.0f), 0);
}

/* ================================ mu_rhea =============================== */

/* synthetic objective: prefer action id 7 everywhere (flat plateau),
 * horizon 8. Random genomes score ~0; the EA must concentrate on 7. */
static float rhea_eval_seven(const uint16_t* actions, int horizon, void* ctx)
{
    float s = 0.0f;
    int i;
    (void)ctx;
    for (i = 0; i < horizon; ++i)
        if (actions[i] == 7u) s += 1.0f;
    return s;
}

/* gradient objective: peak at action id 3 everywhere. */
static float rhea_eval_gradient(const uint16_t* actions, int horizon, void* ctx)
{
    float s = 0.0f;
    int i;
    (void)ctx;
    for (i = 0; i < horizon; ++i)
        s -= (float)((int)actions[i] - 3) * (float)((int)actions[i] - 3);
    return s;
}

static void test_rhea(void)
{
    mu_rhea r;
    uint16_t out[MU_RHEA_MAX_HORIZON];
    uint16_t out_first[MU_RHEA_MAX_HORIZON];
    float best;
    int i, sevens;

    mu_rhea_init(&r, 8, 32, 6, 16u, 2024u);
    best = mu_rhea_solve(&r, rhea_eval_seven, NULL, 16u, out, 8);
    check_b("rhea improves over random", best >= 5.0f);
    sevens = 0;
    for (i = 0; i < 8; ++i) { out_first[i] = out[i]; if (out[i] == 7u) ++sevens; }
    check_b("rhea output concentrated", sevens >= 5);

    /* shift buffer keeps horizon and moves the plan along */
    {
        mu_rhea_shift(&r, 16u);
        best = mu_rhea_solve(&r, rhea_eval_seven, NULL, 16u, out, 8);
        check_b("rhea after shift still good", best >= 5.0f);
    }

    /* determinism: same seed => same output as the FIRST solve */
    {
        mu_rhea r2;
        uint16_t out2[MU_RHEA_MAX_HORIZON];
        mu_rhea_init(&r2, 8, 32, 6, 16u, 2024u);
        mu_rhea_solve(&r2, rhea_eval_seven, NULL, 16u, out2, 8);
        for (i = 0; i < 8; ++i)
            check_i("rhea deterministic genome", out2[i], out_first[i]);
    }

    /* gradient case: fitness = -sum((a-3)^2): must find the all-3 genome.
     * 16 generations is the measured cold-start convergence budget for this
     * landscape (see PERF notes); RHEA in production warm-starts across ticks. */
    {
        mu_rhea g;
        uint16_t outg[MU_RHEA_MAX_HORIZON];
        float bg;
        int allthree = 1;
        mu_rhea_init(&g, 6, 48, 16, 8u, 7u);
        bg = mu_rhea_solve(&g, rhea_eval_gradient, NULL, 8u, outg, 6);
        for (i = 0; i < 6; ++i) if (outg[i] != 3u) allthree = 0;
        check_i("rhea finds gradient peak", allthree, 1);
        check_f("rhea gradient optimum", bg, 0.0f, 1e-6f);
    }
}

int main(void)
{
    test_utility();
    test_steering();
    test_senses();
    test_director();
    test_bandit();
    test_micro();
    test_goap();
    test_elo();
    test_replay();
    test_mood();
    test_playermodel();
    test_rhea();

    if (g_fails) { printf("FAILED: %ld checks, %ld failures\n", g_checks, g_fails); return 1; }
    printf("PASSED: %ld checks, 0 failures\n", g_checks);
    return 0;
}
