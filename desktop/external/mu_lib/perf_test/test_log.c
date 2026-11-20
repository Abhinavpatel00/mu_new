/* test_log.c -- gate for mu_log.h (the slog-style logger).
 *
 * A logger's failure modes are quiet: a filtered record that should have
 * appeared, a level that is checked one way and enforced another, a named
 * logger whose settings leak into its neighbors. Each of those gets an
 * assertion here.
 *
 * NOTE: this gate was rewritten for the current slog-style API (named
 * loggers, log_set_* configuration, dump_fn capture). The previous gate
 * tested the older per-instance mu_log API (callbacks, ring sink) that
 * mu_log.h no longer provides.
 */

#include <stdio.h>
#include <string.h>

#include "mu_log.h"

static int g_checks, g_fails;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) { ++g_fails; if (g_fails <= 20) printf("  FAIL %s\n", what); }
}

/* ---- capture sink: dump_fn receives the formatted line ---- */

static char  g_cap[4096];
static size_t g_cap_len;
static int   g_cap_n;

static void capture(void *user, const char *data, size_t len)
{
    (void)user;
    ++g_cap_n;
    if (g_cap_len + len < sizeof g_cap)
    {
        memcpy(g_cap + g_cap_len, data, len);
        g_cap_len += len;
        g_cap[g_cap_len] = '\0';
    }
}

static void capture_reset(void)
{
    g_cap_n = 0;
    g_cap_len = 0;
    g_cap[0] = '\0';
}

static void test_levels(void)
{
    log_logger *lg = slog__find_or_create("t_levels");
    log_set_level_ptr(lg, LOG_WARN);
    log_set_quiet_ptr(lg, 0);
    log_set_dump_fn_ptr(lg, capture, NULL);
    capture_reset();

    log_message(lg, LOG_DEBUG, __FILE__, __LINE__, __func__, "debug line");
    log_message(lg, LOG_INFO,  __FILE__, __LINE__, __func__, "info line");
    check(g_cap_n == 0, "levels: records below the logger level are dropped");
    check(slog__enabled(lg, LOG_DEBUG) == 0, "levels: enabled(DEBUG) false");
    check(slog__enabled(lg, LOG_ERROR) == 1, "levels: enabled(ERROR) true");

    log_message(lg, LOG_ERROR, __FILE__, __LINE__, __func__, "error %d", 7);
    check(g_cap_n == 1, "levels: the first record at the level arrives");
    check(strstr(g_cap, "error 7") != NULL, "levels: formatted with arguments");

    log_set_level_ptr(lg, LOG_TRACE);
    log_message(lg, LOG_DEBUG, __FILE__, __LINE__, __func__, "now visible");
    check(g_cap_n == 2, "levels: raising the level admits the record");
}

static void test_quiet(void)
{
    log_logger *lg = slog__find_or_create("t_quiet");
    log_set_level_ptr(lg, LOG_TRACE);
    log_set_dump_fn_ptr(lg, capture, NULL);
    capture_reset();

    log_set_quiet_ptr(lg, 1);
    log_message(lg, LOG_FATAL, __FILE__, __LINE__, __func__, "muted");
    check(g_cap_n == 0, "quiet: even FATAL is suppressed");
    check(slog__enabled(lg, LOG_ERROR) == 0, "quiet: enabled() is false");

    log_set_quiet_ptr(lg, 0);
    log_message(lg, LOG_ERROR, __FILE__, __LINE__, __func__, "unmuted");
    check(g_cap_n == 1, "quiet: clearing quiet admits records");
}

static void test_named_isolation(void)
{
    log_logger *a = slog__find_or_create("t_net");
    log_logger *b = slog__find_or_create("t_gfx");
    log_set_dump_fn_ptr(a, capture, NULL);
    log_set_dump_fn_ptr(b, capture, NULL);
    log_set_level_ptr(a, LOG_ERROR);
    log_set_level_ptr(b, LOG_TRACE);
    log_set_quiet_ptr(a, 0);
    log_set_quiet_ptr(b, 0);
    capture_reset();

    log_message(a, LOG_WARN, __FILE__, __LINE__, __func__, "net warn");
    check(g_cap_n == 0, "named: strict logger filters its own record");
    log_message(b, LOG_WARN, __FILE__, __LINE__, __func__, "gfx warn");
    check(g_cap_n == 1, "named: loose logger still emits");
    check(slog__find_or_create("t_net") == a, "named: find_or_create returns the same slot");
    check(a != b, "named: distinct names get distinct loggers");
}

static void test_format(void)
{
    log_logger *lg = slog__find_or_create("t_fmt");
    log_set_level_ptr(lg, LOG_TRACE);
    log_set_quiet_ptr(lg, 0);
    log_set_dump_fn_ptr(lg, capture, NULL);
    capture_reset();

    log_message(lg, LOG_WARN, __FILE__, __LINE__, __func__, "watch %s %d", "out", 42);
    check(g_cap_n == 1, "fmt: one dump call per record");
    check(strstr(g_cap, "watch out 42") != NULL, "fmt: printf substitution");
    check(strstr(g_cap, "WARN") != NULL, "fmt: level name present");
    check(strstr(g_cap, "t_fmt") != NULL, "fmt: logger name present");
}

static void test_level_names(void)
{
    check(strcmp(slog__level_name(LOG_TRACE), "TRACE") == 0, "names: TRACE");
    check(strcmp(slog__level_name(LOG_DEBUG), "DEBUG") == 0, "names: DEBUG");
    check(strncmp(slog__level_name(LOG_INFO),  "INFO",  4) == 0, "names: INFO");
    check(strncmp(slog__level_name(LOG_WARN),  "WARN",  4) == 0, "names: WARN");
    check(strcmp(slog__level_name(LOG_ERROR), "ERROR") == 0, "names: ERROR");
    check(strcmp(slog__level_name(LOG_FATAL), "FATAL") == 0, "names: FATAL");
}

int main(void)
{
    test_levels();
    test_quiet();
    test_named_isolation();
    test_format();
    test_level_names();

    if (g_fails) { printf("FAILED: %d checks, %d failures\n", g_checks, g_fails); return 1; }
    printf("PASSED: %d checks, 0 failures\n", g_checks);
    return 0;
}
