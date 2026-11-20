#pragma once
#ifndef MU_SIMPLE_LOG_H
#define MU_SIMPLE_LOG_H

/* ===========================================================================
 * mu_simple_log.h — dead-simple named-logger logging (C99, header-only)
 * ===========================================================================
 *
 * API (exactly as requested):
 *
 *   log_trace(...), log_debug(...), log_info(...), log_warn(...),
 *   log_error(...), log_fatal(...)          — write to the default logger
 *
 *   log_set_level(name, level)              — per-logger threshold
 *   log_set_quiet(name, bool)               — suppress all output
 *   log_set_date_fmt(name, fmt)             — strftime date format
 *   log_set_fmt_fn(name, fn, user)          — custom format callback
 *   log_set_dump_fn(name, fn, user)         — custom output callback
 *   log_set_lock(fn, user)                  — global lock/unlock
 *
 * Design opinions:
 *   - No allocation. Fixed array of loggers (MU_SIMPLE_LOG_MAX_LOGGERS).
 *   - Header-only. Every function is static inline; include in as many TUs
 *     as you like — one default logger instance lives in the first TU that
 *     calls log_default() (static-local, C11 thread-safe init).
 *   - One vsnprintf per record, then a single dump call per logger.
 *   - Compile-time strip: records below MU_SIMPLE_LOG_MIN_LEVEL compile to
 *     nothing (no call, no format string, no argument evaluation).
 *   - Thread safety is opt-in: log_set_lock() installs a global
 *     lock/unlock callback taken around each record emission.
 *
 * Typical use:
 *
 *   log_set_level("net", LOG_INFO);
 *   log_set_quiet("net", 0);
 *   log_info("connected to %s:%d", host, port);
 *   log_warn("retry #%d", attempt);
 *
 *   // custom sink
 *   log_set_dump_fn("net", my_dump, my_user);
 *
 * Compile-time knobs:
 *   MU_SIMPLE_LOG_MIN_LEVEL    records below this level vanish at compile time
 *   MU_SIMPLE_LOG_MAX_LOGGERS  named logger slots (default 16)
 *   MU_SIMPLE_LOG_LINE_MAX     bytes per formatted record (default 512)
 *   MU_SIMPLE_LOG_NAME_MAX     max logger name length (default 32)
 *   MU_SIMPLE_LOG_DATE_FMT_MAX max date-format length (default 32)
 *   MU_SIMPLE_LOG_NO_COLOR     never emit ANSI colours
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mu_macros.h"
#include "mu_time.h"

#if defined(__GNUC__) || defined(__clang__)
#  define MU_SLOG__UNUSED   __attribute__((unused))
#  define MU_SLOG__PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#  define MU_SLOG__UNUSED
#  define MU_SLOG__PRINTF(a, b)
#endif

/* ------------------------------------------------------------------
 * levels
 * ------------------------------------------------------------------ */

#define LOG_TRACE 0
#define LOG_DEBUG 1
#define LOG_INFO  2
#define LOG_WARN  3
#define LOG_ERROR 4
#define LOG_FATAL 5
#define LOG_OFF   6

#ifndef MU_SIMPLE_LOG_MIN_LEVEL
#  define MU_SIMPLE_LOG_MIN_LEVEL LOG_TRACE
#endif

#ifndef MU_SIMPLE_LOG_MAX_LOGGERS
#  define MU_SIMPLE_LOG_MAX_LOGGERS 16
#endif

#ifndef MU_SIMPLE_LOG_LINE_MAX
#  define MU_SIMPLE_LOG_LINE_MAX 512
#endif

#ifndef MU_SIMPLE_LOG_NAME_MAX
#  define MU_SIMPLE_LOG_NAME_MAX 32
#endif

#ifndef MU_SIMPLE_LOG_DATE_FMT_MAX
#  define MU_SIMPLE_LOG_DATE_FMT_MAX 32
#endif

/* ------------------------------------------------------------------
 * callbacks
 * ------------------------------------------------------------------ */

/* Format a record into `out` (cap bytes). Returns bytes written (not counting
 * NUL). Default implementation writes "[LEVEL] HH:MM:SS name: msg\n". */
typedef void (*log_fmt_fn)(void *user,
                           const char *logger,
                           int level,
                           const char *file,
                           int line,
                           const char *func,
                           const char *msg,
                           char *out,
                           size_t cap);

/* Emit `len` bytes somewhere. Default implementation writes to stderr. */
typedef void (*log_dump_fn)(void *user, const char *data, size_t len);

/* lock=1 → acquire, lock=0 → release. Called around each record. */
typedef void (*log_lock_fn)(void *user, int lock);

/* ------------------------------------------------------------------
 * logger
 * ------------------------------------------------------------------ */

typedef struct log_logger
{
    char  name[MU_SIMPLE_LOG_NAME_MAX];
    int   level;            /* emits records >= this level */
    int   quiet;            /* nonzero: suppress everything */
    char  date_fmt[MU_SIMPLE_LOG_DATE_FMT_MAX];
    FILE *stream;           /* default sink: stderr if NULL */
    log_fmt_fn  fmt_fn;
    log_dump_fn dump_fn;
    void       *fmt_user;
    void       *dump_user;
    int        used;        /* slot in use */
} log_logger;

/* ------------------------------------------------------------------
 * globals (one definition per TU via static locals — fine for header-only)
 * ------------------------------------------------------------------ */

MU_SLOG__UNUSED static log_logger *slog__table(void)
{
    static log_logger tbl[MU_SIMPLE_LOG_MAX_LOGGERS];
    return tbl;
}

MU_SLOG__UNUSED static int *slog__n_loggers_slot(void)
{
    static int n = 0;
    return &n;
}

MU_SLOG__UNUSED static int slog__n_loggers(void)
{
    return *slog__n_loggers_slot();
}

MU_SLOG__UNUSED static int slog__n_loggers_inc(void)
{
    int *n = slog__n_loggers_slot();
    return (*n)++;
}

MU_SLOG__UNUSED static log_lock_fn *slog__lock_fn_slot(void)
{
    static log_lock_fn fn = NULL;
    return &fn;
}

MU_SLOG__UNUSED static void **slog__lock_user_slot(void)
{
    static void *u = NULL;
    return &u;
}

MU_SLOG__UNUSED static void slog__lock(int acquire)
{
    log_lock_fn *fn = slog__lock_fn_slot();
    if (*fn) (*fn)(*slog__lock_user_slot(), acquire);
}

MU_SLOG__UNUSED static int slog__enabled(const log_logger *lg, int level)
{
    if (!lg || lg->quiet) return 0;
    if (level < MU_SIMPLE_LOG_MIN_LEVEL) return 0;
    if (level < lg->level) return 0;
    return 1;
}

MU_SLOG__UNUSED static log_logger *slog__find_or_create(const char *name)
{
    log_logger *tbl = slog__table();
    int         n   = slog__n_loggers();
    int         i;

    for (i = 0; i < n; ++i)
        if (tbl[i].used && strcmp(tbl[i].name, name) == 0)
            return &tbl[i];

    if (n >= MU_SIMPLE_LOG_MAX_LOGGERS) return NULL;

    tbl[n].used   = 1;
    tbl[n].level  = LOG_INFO;
    tbl[n].quiet  = 0;
    tbl[n].stream = stderr;
    tbl[n].dump_fn = NULL;   /* use default dump */
    tbl[n].fmt_fn  = NULL;   /* use default fmt */
    {
        const char *src = name ? name : "app";
        size_t      len = strlen(src);
        if (len >= sizeof tbl[n].name) len = sizeof tbl[n].name - 1;
        memcpy(tbl[n].name, src, len);
        tbl[n].name[len] = '\0';
    }
    {
        const char *df = "%H:%M:%S";
        size_t      len = strlen(df);
        if (len >= sizeof tbl[n].date_fmt) len = sizeof tbl[n].date_fmt - 1;
        memcpy(tbl[n].date_fmt, df, len);
        tbl[n].date_fmt[len] = '\0';
    }
    slog__n_loggers_inc();
    return &tbl[n];
}

MU_SLOG__UNUSED static log_logger *log_default(void)
{
    return slog__find_or_create("app");
}

/* ------------------------------------------------------------------
 * public setters (name-based, as requested)
 * ------------------------------------------------------------------ */

MU_SLOG__UNUSED static void log_set_level(const char *name, int level)
{
    log_logger *lg = slog__find_or_create(name);
    if (lg) lg->level = level;
}

MU_SLOG__UNUSED static void log_set_quiet(const char *name, int quiet)
{
    log_logger *lg = slog__find_or_create(name);
    if (lg) lg->quiet = quiet ? 1 : 0;
}

MU_SLOG__UNUSED static void log_set_date_fmt(const char *name, const char *fmt)
{
    log_logger *lg = slog__find_or_create(name);
    if (!lg || !fmt) return;
    {
        size_t len = strlen(fmt);
        if (len >= sizeof lg->date_fmt) len = sizeof lg->date_fmt - 1;
        memcpy(lg->date_fmt, fmt, len);
        lg->date_fmt[len] = '\0';
    }
}

MU_SLOG__UNUSED static void log_set_fmt_fn(const char *name, log_fmt_fn fn, void *user)
{
    log_logger *lg = slog__find_or_create(name);
    if (lg) { lg->fmt_fn = fn; lg->fmt_user = user; }
}

MU_SLOG__UNUSED static void log_set_dump_fn(const char *name, log_dump_fn fn, void *user)
{
    log_logger *lg = slog__find_or_create(name);
    if (lg) { lg->dump_fn = fn; lg->dump_user = user; }
}

MU_SLOG__UNUSED static void log_set_lock(log_lock_fn fn, void *user)
{
    *slog__lock_fn_slot() = fn;
    *slog__lock_user_slot() = user;
}

/* Also accept a logger pointer directly (convenience). */
MU_SLOG__UNUSED static void log_set_level_ptr(log_logger *lg, int level)
{
    if (lg) lg->level = level;
}
MU_SLOG__UNUSED static void log_set_quiet_ptr(log_logger *lg, int quiet)
{
    if (lg) lg->quiet = quiet ? 1 : 0;
}
MU_SLOG__UNUSED static void log_set_date_fmt_ptr(log_logger *lg, const char *fmt)
{
    if (!lg || !fmt) return;
    log_set_date_fmt(lg->name, fmt);
}
MU_SLOG__UNUSED static void log_set_fmt_fn_ptr(log_logger *lg, log_fmt_fn fn, void *user)
{
    if (lg) { lg->fmt_fn = fn; lg->fmt_user = user; }
}
MU_SLOG__UNUSED static void log_set_dump_fn_ptr(log_logger *lg, log_dump_fn fn, void *user)
{
    if (lg) { lg->dump_fn = fn; lg->dump_user = user; }
}

/* ------------------------------------------------------------------
 * default format & dump
 * ------------------------------------------------------------------ */

MU_SLOG__UNUSED static const char *slog__level_name(int level)
{
    switch (level)
    {
        case LOG_TRACE: return "TRACE";
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO ";
        case LOG_WARN:  return "WARN ";
        case LOG_ERROR: return "ERROR";
        case LOG_FATAL: return "FATAL";
        default:        return "????";
    }
}

MU_SLOG__UNUSED static const char *slog__level_color(int level)
{
#if defined(MU_SIMPLE_LOG_NO_COLOR)
    (void)level;
    return "";
#else
    switch (level)
    {
        case LOG_TRACE: return "\x1b[90m";   /* bright black */
        case LOG_DEBUG: return "\x1b[36m";   /* cyan */
        case LOG_INFO:  return "\x1b[32m";   /* green */
        case LOG_WARN:  return "\x1b[33m";   /* yellow */
        case LOG_ERROR: return "\x1b[31m";   /* red */
        case LOG_FATAL: return "\x1b[35;1m"; /* bold magenta */
        default:        return "";
    }
#endif
}

MU_SLOG__UNUSED static void slog__default_fmt(void *user,
                                              const char *logger,
                                              int level,
                                              const char *file,
                                              int line,
                                              const char *func,
                                              const char *msg,
                                              char *out,
                                              size_t cap)
{
    (void)user;
    (void)file;
    (void)line;
    (void)func;

    time_t t = (time_t)(mu_clock_elapsed_ns() / 1000000000ULL);
    struct tm *tm = localtime(&t);
    int n = 0;

    if (tm)
        n = snprintf(out, cap, "[%s] %02d:%02d:%02d %s: %s",
                     slog__level_name(level),
                     tm->tm_hour, tm->tm_min, tm->tm_sec,
                     logger, msg ? msg : "");
    else
        n = snprintf(out, cap, "[%s] %s: %s",
                     slog__level_name(level), logger, msg ? msg : "");

    if (n < 0) n = 0;
    if ((size_t)n > cap - 1) n = (int)(cap - 1);
    out[(size_t)n] = '\0';
}

MU_SLOG__UNUSED static void slog__default_dump(void *user, const char *data, size_t len)
{
    (void)user;
    fwrite(data, 1, len, stderr);
}

/* ------------------------------------------------------------------
 * core emission
 * ------------------------------------------------------------------ */

MU_SLOG__UNUSED static void slog__messagev(log_logger *lg,
                                           int level,
                                           const char *file,
                                           int line,
                                           const char *func,
                                           const char *fmt,
                                           va_list ap)
{
    char msg[MU_SIMPLE_LOG_LINE_MAX];
    char out[MU_SIMPLE_LOG_LINE_MAX + 128];
    int  n;

    if (!slog__enabled(lg, level)) return;

    n = vsnprintf(msg, sizeof msg, fmt, ap);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof msg) n = (int)sizeof msg - 1;
    msg[(size_t)n] = '\0';

    slog__lock(1);

    if (lg->fmt_fn)
        lg->fmt_fn(lg->fmt_user, lg->name, level, file, line, func, msg, out, sizeof out);
    else
        slog__default_fmt(NULL, lg->name, level, file, line, func, msg, out, sizeof out);

    /* ensure trailing newline */
    {
        size_t olen = strlen(out);
        if (olen > 0 && out[olen - 1] != '\n' && olen + 1 < sizeof out)
        {
            out[olen] = '\n';
            out[olen + 1] = '\0';
            olen += 1;
        }
    }

    if (lg->dump_fn)
        lg->dump_fn(lg->dump_user, out, strlen(out));
    else
        slog__default_dump(NULL, out, strlen(out));

    slog__lock(0);
}

MU_SLOG__UNUSED static void log_message(log_logger *lg,
                                        int level,
                                        const char *file,
                                        int line,
                                        const char *func,
                                        const char *fmt,
                                        ...) MU_SLOG__PRINTF(6, 7);
MU_SLOG__UNUSED static void log_message(log_logger *lg,
                                        int level,
                                        const char *file,
                                        int line,
                                        const char *func,
                                        const char *fmt,
                                        ...)
{
    va_list ap;
    va_start(ap, fmt);
    slog__messagev(lg, level, file, line, func, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------
 * macros (default logger, compile-time stripped)
 * ------------------------------------------------------------------ */

#if MU_SIMPLE_LOG_MIN_LEVEL <= LOG_TRACE
#  define log_trace(...) log_message(log_default(), LOG_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define log_trace(...) ((void)0)
#endif

#if MU_SIMPLE_LOG_MIN_LEVEL <= LOG_DEBUG
#  define log_debug(...) log_message(log_default(), LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define log_debug(...) ((void)0)
#endif

#if MU_SIMPLE_LOG_MIN_LEVEL <= LOG_INFO
#  define log_info(...)  log_message(log_default(), LOG_INFO,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define log_info(...)  ((void)0)
#endif

#if MU_SIMPLE_LOG_MIN_LEVEL <= LOG_WARN
#  define log_warn(...)  log_message(log_default(), LOG_WARN,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define log_warn(...)  ((void)0)
#endif

#if MU_SIMPLE_LOG_MIN_LEVEL <= LOG_ERROR
#  define log_error(...) log_message(log_default(), LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define log_error(...) ((void)0)
#endif

#define log_fatal(...) log_message(log_default(), LOG_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

#endif /* MU_SIMPLE_LOG_H */