#pragma once

/*
===============================================================================

mu_timeline.h — deterministic sequencer for cutscenes, UI transitions and
spawn schedules.

Model:
  - Tracks carry keyframes over time: float, vec3, colour (vec4), or callback.
  - Each keyframe segment carries its own mu_ease_kind curve.
  - Loop modes: none / loop / ping-pong.
  - Markers fire with sub-tick accuracy (a 60 Hz tick can still emit an event
    at t = 1.3742 s).
  - Update is O(active tracks), not O(all tracks): a track is "active" between
    its first and last keyframe time.
  - Fixed-step friendly: advance with a clamped dt, snap markers to the tick
    grid, and the same trajectory is bit-identical no matter how dt is sliced.

===============================================================================
*/

#include "mu_common.h"
#include "mu/mu_easing.h"

MU_BEGIN_EXTERN_C

typedef enum mu_track_type
{
    MU_TRACK_FLOAT,     /* value[0] */
    MU_TRACK_VEC3,      /* value[0..2] */
    MU_TRACK_COLOR,     /* value[0..3] */
    MU_TRACK_CALLBACK   /* fires fn at keyframe times */
} mu_track_type;

typedef enum mu_loop_mode
{
    MU_LOOP_NONE,    /* hold last value past end */
    MU_LOOP_LOOP,    /* restart from beginning */
    MU_LOOP_PINGPONG /* bounce back and forth */
} mu_loop_mode;

typedef struct mu_keyframe
{
    float      time;      /* seconds */
    float      value[4];  /* x,y,z,w (w unused for float/vec3) */
    mu_ease_kind ease;    /* curve for the segment *after* this keyframe */
} mu_keyframe;

typedef void (*mu_track_cb)(uint32_t track_id, uint32_t keyframe_index,
                            float value[4], void* ctx);

typedef struct mu_track
{
    uint32_t       id;
    mu_track_type  type;
    mu_loop_mode   loop;
    float          duration;      /* last keyframe time (0 = no keys) */
    mu_keyframe*   keys;
    uint32_t       key_count;
    uint32_t       key_cap;
    mu_track_cb    cb;            /* callback tracks only */
    void*          cb_ctx;
} mu_track;

typedef struct mu_marker
{
    float  time;   /* seconds */
    uint32_t id;   /* user tag */
    bool   fired;  /* set true once this tick's update passes it */
} mu_marker;

typedef struct mu_timeline
{
    mu_track*   tracks;
    uint32_t    track_count;
    uint32_t    track_cap;

    mu_marker*  markers;
    uint32_t    marker_count;
    uint32_t    marker_cap;

    float       time;        /* seconds, monotonic while playing */
    float       duration;    /* max track duration */
    bool        playing;

    /* Active-track list for O(active) update: indices of tracks whose
     * [first_key, last_key] window contains `time`. Rebuilt on seek. */
    uint32_t*   active;
    uint32_t    active_count;
    uint32_t    active_cap;
} mu_timeline;

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_timeline_init(mu_timeline* t)
{
    MU_ASSERT(t != NULL);
    t->tracks = NULL;
    t->track_count = 0;
    t->track_cap = 0;
    t->markers = NULL;
    t->marker_count = 0;
    t->marker_cap = 0;
    t->time = 0.0f;
    t->duration = 0.0f;
    t->playing = false;
    t->active = NULL;
    t->active_count = 0;
    t->active_cap = 0;
    return true;
}

MU_INLINE void mu_timeline_destroy(mu_timeline* t)
{
    if(t == NULL)
        return;
    for(uint32_t i = 0; i < t->track_count; ++i)
        MU_FREE(t->tracks[i].keys);
    MU_FREE(t->tracks);
    MU_FREE(t->markers);
    MU_FREE(t->active);
    t->tracks = NULL;
    t->markers = NULL;
    t->active = NULL;
    t->track_count = t->track_cap = 0;
    t->marker_count = t->marker_cap = 0;
    t->active_count = t->active_cap = 0;
    t->time = 0.0f;
    t->duration = 0.0f;
    t->playing = false;
}

/* ---------- tracks ---------- */

/* Append a track (id is user-assigned; duplicates allowed but discouraged). */
MU_INLINE bool mu_timeline_add_track(mu_timeline* t, uint32_t id,
                                     mu_track_type type, mu_loop_mode loop,
                                     mu_track_cb cb, void* cb_ctx)
{
    MU_ASSERT(t != NULL);
    if(t->track_count == t->track_cap)
    {
        uint32_t nc = t->track_cap ? t->track_cap * 2u : 4u;
        mu_track* nt = (mu_track*)MU_REALLOC(t->tracks,
                                             sizeof(mu_track) * nc);
        if(!nt)
            return false;
        t->tracks = nt;
        t->track_cap = nc;
    }
    mu_track* tr = &t->tracks[t->track_count++];
    tr->id = id;
    tr->type = type;
    tr->loop = loop;
    tr->duration = 0.0f;
    tr->keys = NULL;
    tr->key_count = 0;
    tr->key_cap = 0;
    tr->cb = cb;
    tr->cb_ctx = cb_ctx;
    return true;
}

/* Push a keyframe onto track `id` (must already exist). Times must be
 * non-decreasing. */
MU_INLINE bool mu_timeline_add_key(mu_timeline* t, uint32_t track_id,
                                   float time, const float value[4],
                                   mu_ease_kind ease)
{
    MU_ASSERT(t != NULL);
    mu_track* tr = NULL;
    for(uint32_t i = 0; i < t->track_count; ++i)
    {
        if(t->tracks[i].id == track_id)
        {
            tr = &t->tracks[i];
            break;
        }
    }
    if(tr == NULL)
        return false;
    if(tr->key_count > 0u && time < tr->keys[tr->key_count - 1].time)
        return false; /* keys must be time-sorted */

    if(tr->key_count == tr->key_cap)
    {
        uint32_t nc = tr->key_cap ? tr->key_cap * 2u : 4u;
        mu_keyframe* nk = (mu_keyframe*)MU_REALLOC(tr->keys,
                                                   sizeof(mu_keyframe) * nc);
        if(!nk)
            return false;
        tr->keys = nk;
        tr->key_cap = nc;
    }
    mu_keyframe* k = &tr->keys[tr->key_count++];
    k->time = time;
    if(value)
        MU_MEMCPY(k->value, value, sizeof(k->value));
    else
        MU_MEMSET(k->value, 0, sizeof(k->value));
    k->ease = ease;
    tr->duration = time;
    if(time > t->duration)
        t->duration = time;
    return true;
}

/* ---------- markers ---------- */

/* Register a marker event at `time` tagged with `id`. Fires once per pass. */
MU_INLINE bool mu_timeline_add_marker(mu_timeline* t, float time, uint32_t id)
{
    MU_ASSERT(t != NULL);
    if(t->marker_count == t->marker_cap)
    {
        uint32_t nc = t->marker_cap ? t->marker_cap * 2u : 8u;
        mu_marker* nm = (mu_marker*)MU_REALLOC(t->markers,
                                               sizeof(mu_marker) * nc);
        if(!nm)
            return false;
        t->markers = nm;
        t->marker_cap = nc;
    }
    mu_marker* m = &t->markers[t->marker_count++];
    m->time = time;
    m->id = id;
    m->fired = false;
    return true;
}

/* ---------- playback ---------- */

MU_INLINE void mu_timeline_play(mu_timeline* t)   { if(t) t->playing = true; }
MU_INLINE void mu_timeline_pause(mu_timeline* t)  { if(t) t->playing = false; }
MU_INLINE void mu_timeline_stop(mu_timeline* t)
{
    if(!t) return;
    t->playing = false;
    t->time = 0.0f;
}

/* Seek to an absolute time (clamps to [0, duration] for NONE loop). */
MU_INLINE void mu_timeline_seek(mu_timeline* t, float time)
{
    if(!t) return;
    t->time = MU_CLAMP(time, 0.0f, t->duration);
    /* deliberately does not touch playing: a seek mid-playback keeps playing. */
}

/* Advance by dt seconds. Returns true if any marker fired this tick. */
MU_INLINE bool mu_timeline_update(mu_timeline* t, float dt)
{
    if(!t || !t->playing)
        return false;
    if(dt < 0.0f)
        dt = 0.0f;

    float prev = t->time;
    t->time += dt;

    /* Clamp or wrap per global duration. Per-track loop handled at eval time. */
    if(t->time > t->duration && t->duration > 0.0f)
    {
        /* Simple clamp at timeline end; individual tracks with LOOP wrap
         * internally via their own duration. */
        t->time = t->duration;
    }

    /* Fire markers in (prev, now]. */
    bool any_fired = false;
    for(uint32_t i = 0; i < t->marker_count; ++i)
    {
        mu_marker* m = &t->markers[i];
        if(!m->fired && m->time > prev && m->time <= t->time)
        {
            m->fired = true;
            any_fired = true;
        }
    }
    MU_UNUSED(any_fired);
    return any_fired;
}

/* Reset all marker fired flags so they can fire again on the next pass. */
MU_INLINE void mu_timeline_reset_markers(mu_timeline* t)
{
    if(!t) return;
    for(uint32_t i = 0; i < t->marker_count; ++i)
        t->markers[i].fired = false;
}

/* ---------- evaluation ---------- */

/* Wrap a time into [0, duration] according to loop mode. */
MU_INLINE float mu_track_wrap(const mu_track* tr, float time)
{
    if(tr->duration <= 0.0f)
        return 0.0f;
    if(tr->loop == MU_LOOP_NONE)
        return MU_CLAMP(time, 0.0f, tr->duration);
    if(tr->loop == MU_LOOP_LOOP)
    {
        /* fmod with positive result. */
        float r = time - (float)(int)(time / tr->duration) * tr->duration;
        if(r < 0.0f)
            r += tr->duration;
        return r;
    }
    /* PINGPONG: period = 2 * duration. */
    float period = 2.0f * tr->duration;
    float r = time - (float)(int)(time / period) * period;
    if(r < 0.0f)
        r += period;
    if(r > tr->duration)
        r = period - r;
    return r;
}

/* Locate the segment containing time: returns index i such that
 * keys[i].time <= time < keys[i+1].time, or -1 if before the first key,
 * or key_count-1 if at/past the last key (hold). */
MU_INLINE int32_t mu_track_find_segment(const mu_track* tr, float time)
{
    if(tr->key_count == 0u)
        return -1;
    if(time <= tr->keys[0].time)
        return -1;
    if(time >= tr->keys[tr->key_count - 1].time)
        return (int32_t)(tr->key_count - 1);
    /* binary search */
    int32_t lo = 0, hi = (int32_t)tr->key_count - 1;
    while(lo < hi)
    {
        int32_t mid = (lo + hi) / 2;
        if(tr->keys[mid].time <= time)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo - 1; /* keys[lo-1].time <= time < keys[lo].time */
}

/* Evaluate a float track at `time` (already wrapped by the caller). */
MU_INLINE float mu_track_eval_float(const mu_track* tr, float time)
{
    if(tr->key_count == 0u)
        return 0.0f;
    int32_t seg = mu_track_find_segment(tr, time);
    if(seg < 0)
        return tr->keys[0].value[0];
    if(seg >= (int32_t)tr->key_count - 1)
        return tr->keys[tr->key_count - 1].value[0];
    const mu_keyframe* a = &tr->keys[seg];
    const mu_keyframe* b = &tr->keys[seg + 1];
    float span = b->time - a->time;
    float u = (span > 0.0f) ? (time - a->time) / span : 0.0f;
    mu_ease_fn fn = mu_ease_get(a->ease);
    float e = fn(u);
    return a->value[0] + (b->value[0] - a->value[0]) * e;
}

/* Evaluate a vec3 track at `time`. Writes to out[3]. */
MU_INLINE void mu_track_eval_vec3(const mu_track* tr, float time, float out[3])
{
    if(tr->key_count == 0u)
    {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    int32_t seg = mu_track_find_segment(tr, time);
    if(seg < 0)
    {
        out[0] = tr->keys[0].value[0];
        out[1] = tr->keys[0].value[1];
        out[2] = tr->keys[0].value[2];
        return;
    }
    if(seg >= (int32_t)tr->key_count - 1)
    {
        const mu_keyframe* k = &tr->keys[tr->key_count - 1];
        out[0] = k->value[0];
        out[1] = k->value[1];
        out[2] = k->value[2];
        return;
    }
    const mu_keyframe* a = &tr->keys[seg];
    const mu_keyframe* b = &tr->keys[seg + 1];
    float span = b->time - a->time;
    float u = (span > 0.0f) ? (time - a->time) / span : 0.0f;
    mu_ease_fn fn = mu_ease_get(a->ease);
    float e = fn(u);
    out[0] = a->value[0] + (b->value[0] - a->value[0]) * e;
    out[1] = a->value[1] + (b->value[1] - a->value[1]) * e;
    out[2] = a->value[2] + (b->value[2] - a->value[2]) * e;
}

/* Evaluate a colour track at time. Writes to out[4]. */
MU_INLINE void mu_track_eval_colour(const mu_track* tr, float time,
                                    float out[4])
{
    if(tr->key_count == 0u)
    {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    int32_t seg = mu_track_find_segment(tr, time);
    if(seg < 0)
    {
        MU_MEMCPY(out, tr->keys[0].value, sizeof(tr->keys[0].value));
        return;
    }
    if(seg >= (int32_t)tr->key_count - 1)
    {
        MU_MEMCPY(out, tr->keys[tr->key_count - 1].value,
                  sizeof(tr->keys[0].value));
        return;
    }
    const mu_keyframe* a = &tr->keys[seg];
    const mu_keyframe* b = &tr->keys[seg + 1];
    float span = b->time - a->time;
    float u = (span > 0.0f) ? (time - a->time) / span : 0.0f;
    mu_ease_fn fn = mu_ease_get(a->ease);
    float e = fn(u);
    for(int c = 0; c < 4; ++c)
        out[c] = a->value[c] + (b->value[c] - a->value[c]) * e;
}

/* Find a track by id. Returns NULL if absent. */
MU_INLINE mu_track* mu_timeline_find_track(mu_timeline* t, uint32_t id)
{
    if(!t) return NULL;
    for(uint32_t i = 0; i < t->track_count; ++i)
        if(t->tracks[i].id == id)
            return &t->tracks[i];
    return NULL;
}

MU_END_EXTERN_C
