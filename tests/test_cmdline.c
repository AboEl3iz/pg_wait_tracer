/* test_cmdline.c — Unit tests for cmdline parser (backend type names) */
#include "cmdline.h"

#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

static void test_backend_type_names(void)
{
    printf("--- Backend Type Names ---\n");

    struct { enum pgwt_backend_type bt; const char *name; } cases[] = {
        { PGWT_BT_CLIENT,           "client" },
        { PGWT_BT_CHECKPOINTER,     "checkpointer" },
        { PGWT_BT_BG_WRITER,        "bgwriter" },
        { PGWT_BT_WAL_WRITER,       "walwriter" },
        { PGWT_BT_AUTOVAC_LAUNCHER, "autovac_launcher" },
        { PGWT_BT_AUTOVAC_WORKER,   "autovac_worker" },
        { PGWT_BT_WAL_SENDER,       "walsender" },
        { PGWT_BT_WAL_RECEIVER,     "walreceiver" },
        { PGWT_BT_STARTUP,          "startup" },
        { PGWT_BT_LOGICAL_LAUNCHER, "logical_launcher" },
        { PGWT_BT_LOGICAL_WORKER,   "logical_worker" },
        { PGWT_BT_ARCHIVER,         "archiver" },
        { PGWT_BT_LOGGER,           "logger" },
        { PGWT_BT_PARALLEL_WORKER,  "parallel_worker" },
        { PGWT_BT_IO_WORKER,        "io_worker" },
        { PGWT_BT_UNKNOWN,          "unknown" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *got = pgwt_backend_type_name(cases[i].bt);
        CHECK(strcmp(got, cases[i].name) == 0,
              "bt=%d: expected \"%s\", got \"%s\"",
              cases[i].bt, cases[i].name, got);
    }
}

static void test_enum_coverage(void)
{
    printf("--- Enum Coverage ---\n");
    /* PGWT_BT_UNKNOWN should be the last enum value */
    CHECK(PGWT_BT_UNKNOWN == 15,
          "PGWT_BT_UNKNOWN should be 15, got %d", PGWT_BT_UNKNOWN);
    /* All values 0..PGWT_BT_UNKNOWN should return non-NULL */
    for (int i = 0; i <= PGWT_BT_UNKNOWN; i++) {
        const char *name = pgwt_backend_type_name((enum pgwt_backend_type)i);
        CHECK(name != NULL && strlen(name) > 0,
              "bt=%d returned NULL or empty", i);
    }
    /* Out of range should return "unknown" */
    const char *oob = pgwt_backend_type_name((enum pgwt_backend_type)99);
    CHECK(strcmp(oob, "unknown") == 0,
          "out-of-range bt=99 should return \"unknown\", got \"%s\"", oob);
}

int main(void)
{
    printf("=== test_cmdline ===\n");
    test_backend_type_names();
    test_enum_coverage();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
