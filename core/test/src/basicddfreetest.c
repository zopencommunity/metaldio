#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bpamio.h"
#include "dio.h"
#include "mem.h"
#include "metaldio.h"
#include "ihadcb.h"
#include "iosvcs.h"
#include "s99.h"
/*
 * basicddfreetest.c  -  Regression tests for DYNFREE fixes in iosvcs.c /
 *                        s99.c / bpamio.c
 *
 * Background
 * ----------
 * ddfree() (iosvcs.c) previously passed stderr (a FILE*) as the first
 * argument to s99_fmt_dmp() and s99_prt_msg(), both of which expect a
 * const DBG_Opts*.  msg.c reads opts->info_buffer from the first bytes of
 * whatever pointer is passed.  The FILE struct bytes are non-zero in that
 * position, so msg.c treated the value as a valid buffer pointer and wrote
 * into it, causing an S0C4 Protection Exception.
 *
 * Additionally, close_pds() (bpamio.c) did not call ddfree() when CLOSE
 * failed, did not free the BPAMHandle on the CLOSE failure path, and did
 * not emit any diagnostic when ddfree() itself failed, leaving PDS/PDSE
 * datasets allocated after a write.
 *
 * Fixes applied
 * -------------
 * iosvcs.c  ddfree():      errmsg(opts,...) replaces fprintf(stderr,...);
 *                           s99_fmt_dmp(opts,...) / s99_prt_msg(opts,...);
 *                           s99_free(parms) called on S99() error path.
 * s99.c     s99_prt_msg(): if (opts && opts->debug) { s99_em_fmt_dmp(...) }
 *           s99_em_fmt_dmp() new helper dumps the s99_em parameter block.
 * bpamio.c  close_pds():   on CLOSE failure: ddfree()+free(bh) before return;
 *                           on DYNFREE failure: errmsg() diagnostic emitted,
 *                           free(bh) always called, rc propagated to caller.
 *
 * Tests
 * -----
 * Test 1 - dsdd_alloc / ddfree round-trip (success path)
 *   Allocates SYS1.MACLIB DISP=SHR and frees it.  Confirms the success
 *   path is unaffected by the change.
 *
 * Test 2 - ddfree() error path: NULL opts must not S0C4
 *   Forces SVC99 UNFREE to fail by calling ddfree() a second time on an
 *   already-freed DDname.  Before the fix this abended with S0C4.
 *   After the fix ddfree() must return non-zero and execution continues.
 *
 * Test 3 - s99_prt_msg() with opts==NULL: NULL guard must hold
 *   Calls s99_prt_msg() directly with opts==NULL and a synthetic s99rb
 *   (verb=S99VRBUN so emidnum=EMFREE, matching the ddfree() error path).
 *   Confirms that the if (opts && opts->debug) guard prevents any
 *   dereference of opts and that s99_em_fmt_dmp() is not reached.
 *
 * Test 4 - close_pds() DYNFREE-failure path: rc propagated, no abend
 *   Opens SYS1.MACLIB for read, steals the DD via a direct ddfree() call
 *   so that the DD no longer exists when close_pds() tries to UNFREE it.
 *   Asserts that close_pds() returns non-zero (failure propagated) and
 *   that execution reaches the assertion line (no abend).
 */

/* SYS1.MACLIB is present on every z/OS system and allocatable DISP=SHR. */
#define TEST_DSN "SYS1.MACLIB"

/* ------------------------------------------------------------------ */
static int g_passed = 0;
static int g_failed = 0;

static void record_pass(const char *name)
{
    ++g_passed;
    fprintf(stdout, "PASS: %s\n", name);
}

static void record_fail(const char *name, const char *reason)
{
    ++g_failed;
    fprintf(stderr, "FAIL: %s - %s\n", name, reason);
}

/* ------------------------------------------------------------------ */
/*
 * Test 1 - dsdd_alloc / ddfree round-trip
 *
 * Pattern taken from basicalloc.c:
 *   init_dsnam_text_unit  dsdd_alloc  capture DDname  ddfree
 * opts==NULL throughout.  ddfree() must return 0.
 */
static void test_alloc_free_roundtrip(void)
{
    const char *tname = "test_alloc_free_roundtrip";
    int rc;
    char ddname[8+1];

    struct s99_common_text_unit dsn   = { DALDSNAM, 1, 0, {0} };
    struct s99_common_text_unit dd    = { DALRTDDN, 1, sizeof(DD_SYSTEM)-1, DD_SYSTEM };
    struct s99_common_text_unit stats = { DALSTATS, 1, 1, {DALSTATS_SHR} };

    rc = init_dsnam_text_unit(TEST_DSN, &dsn, NULL);
    if (rc) {
        record_fail(tname, "init_dsnam_text_unit failed");
        return;
    }

    rc = dsdd_alloc(&dsn, &dd, &stats, NULL);
    if (rc) {
        record_fail(tname, "dsdd_alloc failed - SYS1.MACLIB not accessible");
        return;
    }

    memcpy(ddname, dd.s99tupar, dd.s99tulng);
    ddname[dd.s99tulng] = '\0';
    fprintf(stdout, "  [info] allocated DDname: %s -> %s\n", TEST_DSN, ddname);

    rc = ddfree(&dd, NULL);
    if (rc != 0) {
        record_fail(tname, "ddfree returned non-zero on a valid DDname");
        return;
    }

    record_pass(tname);
}

/* ------------------------------------------------------------------ */
/*
 * Test 2 - ddfree() NULL-opts error path (S0C4 regression)
 *
 * 1. Allocate a DDname.
 * 2. Free it once  (rc must be 0).
 * 3. Free it again - SVC99 UNFREE must fail (DDname gone).
 *    Before the fix: abend S0C4 inside s99_prt_msg / msg.c.
 *    After the fix:  non-zero rc returned, program survives.
 */
static void test_ddfree_null_opts_on_error(void)
{
    const char *tname = "test_ddfree_null_opts_on_error";
    int rc;

    struct s99_common_text_unit dsn   = { DALDSNAM, 1, 0, {0} };
    struct s99_common_text_unit dd    = { DALRTDDN, 1, sizeof(DD_SYSTEM)-1, DD_SYSTEM };
    struct s99_common_text_unit stats = { DALSTATS, 1, 1, {DALSTATS_SHR} };

    rc = init_dsnam_text_unit(TEST_DSN, &dsn, NULL);
    if (rc) {
        record_fail(tname, "init_dsnam_text_unit failed");
        return;
    }

    rc = dsdd_alloc(&dsn, &dd, &stats, NULL);
    if (rc) {
        record_fail(tname, "dsdd_alloc failed - cannot set up double-free scenario");
        return;
    }

    /* First free - must succeed */
    rc = ddfree(&dd, NULL);
    if (rc != 0) {
        record_fail(tname, "first ddfree failed unexpectedly");
        return;
    }

    /*
     * Second free - SVC99 UNFREE should fail.
     * This call exercised the S0C4 path before the fix.
     * Execution reaching the next line proves no abend occurred.
     */
    rc = ddfree(&dd, NULL);
    if (rc == 0) {
        record_fail(tname, "second ddfree returned 0 - SVC99 UNFREE should have failed");
        return;
    }

    fprintf(stdout, "  [info] ddfree on freed DDname rc=%d - no abend\n", rc);
    record_pass(tname);
}

/* ------------------------------------------------------------------ */
/*
 * Test 3 - s99_prt_msg() with opts==NULL: guard in s99_prt_msg
 *
 * Calls s99_prt_msg() directly with opts==NULL.  Uses S99VRBUN so
 * emidnum=EMFREE, the same path taken by ddfree() on error.
 *
 * All SVC99 control blocks are allocated below the bar (MALLOC31) as
 * required for SVC99 / S99MSG.  The text-unit pointer array has the
 * high-bit set on its single entry to mark end-of-list.
 *
 * Assertion: s99_prt_msg returns without abending.
 * The return value may be 0 (S99MSG retrieved a message) or non-zero
 * (S99MSG failed but debug output suppressed by NULL guard) - both
 * outcomes are acceptable; the absence of abend is what is verified.
 */
static void test_s99_prt_msg_null_opts(void)
{
    const char *tname = "test_s99_prt_msg_null_opts";
    int rc;

    struct s99rb* PTR32              fake_rb;
    struct s99_rbx* PTR32            fake_rbx;
    struct s99_text_unit* PTR32 * PTR32 txtpp;

    fake_rb  = MALLOC31(sizeof(struct s99rb));
    fake_rbx = MALLOC31(sizeof(struct s99_rbx));
    txtpp    = MALLOC31(sizeof(struct s99_text_unit* PTR32));

    if (!fake_rb || !fake_rbx || !txtpp) {
        record_fail(tname, "MALLOC31 failed for synthetic s99rb");
        FREE31(fake_rb);
        FREE31(fake_rbx);
        FREE31(txtpp);
        return;
    }

    memset(fake_rb,  0, sizeof(struct s99rb));
    memset(fake_rbx, 0, sizeof(struct s99_rbx));

    fake_rb->s99rbln  = (unsigned char) sizeof(struct s99rb);
    fake_rb->s99verb  = S99VRBUN;      /* UNFREE  emidnum = EMFREE (51) */
    fake_rb->s99error = 0x0238;        /* plausible SVC99 error code      */
    fake_rb->s99txtpp = txtpp;
    fake_rb->s99s99x  = fake_rbx;

    /* Mark the single NULL slot as end of text-unit list */
    txtpp[0] = NULL;
    {
        unsigned int* PTR32 pp = (unsigned int* PTR32) &txtpp[0];
        *pp |= 0x80000000U;
    }

    /* RBX eye-catcher required by s99_fmt_dmp (DEBUG path only) */
    memcpy(fake_rbx->s99eid, "S99RBX", 6);
    fake_rbx->s99ever = S99RBXVR;

    /* Call under test: opts==NULL must not abend */
    rc = s99_prt_msg(NULL, fake_rb, 8);
    fprintf(stdout, "  [info] s99_prt_msg(NULL,...) rc=%d - no abend\n", rc);

    FREE31(txtpp);
    FREE31(fake_rbx);
    FREE31(fake_rb);

    record_pass(tname);
}

/* ------------------------------------------------------------------ */
/*
 * Test 4 - close_pds() DYNFREE-failure path: rc propagated, no abend
 *
 * Opens SYS1.MACLIB for read, steals the DDname by calling ddfree()
 * directly, then calls close_pds().  CLOSE succeeds (DCB still open)
 * but the subsequent DYNFREE fails because the DD no longer exists.
 *
 * FM_BPAMHandle places char ddname[8+1] at offset 0 (bpamint.h), so
 * the DDname can be read by casting the handle pointer to const char*.
 *
 * Assertions:
 *   a) close_pds() returns non-zero  (DYNFREE failure is propagated)
 *   b) execution reaches the assert line  (no abend)
 */
static void test_close_pds_dynfree_failure(void)
{
    const char *tname = "test_close_pds_dynfree_failure";
    int rc;

    FM_BPAMHandle* bh = open_pds_for_read(TEST_DSN, NULL);
    if (!bh) {
        record_fail(tname, "open_pds_for_read failed - cannot set up test");
        return;
    }

    char stolen_ddname[8+1];
    memcpy(stolen_ddname, (const char*) bh, 8);
    stolen_ddname[8] = '\0';
    /* Trim trailing spaces that may be present in the 8-byte field */
    for (int i = 7; i >= 0 && stolen_ddname[i] == ' '; --i) {
        stolen_ddname[i] = '\0';
    }
    fprintf(stdout, "  [info] stealing DDname: %s\n", stolen_ddname);

    /* Build DUNDDNAM text unit and call ddfree() directly to steal the DD */
    struct s99_common_text_unit dd = { DUNDDNAM, 1, 0, {0} };
    int ddname_len = strlen(stolen_ddname);
    dd.s99tulng = ddname_len;
    memcpy(dd.s99tupar, stolen_ddname, ddname_len);

    rc = ddfree(&dd, NULL);
    if (rc != 0) {
        record_fail(tname, "pre-steal ddfree() failed - DDname was not valid");
        /* bh is now in an inconsistent state; avoid calling close_pds() */
        return;
    }
    fprintf(stdout, "  [info] DD stolen successfully; calling close_pds() now\n");

    rc = close_pds(bh, NULL);

    /* Execution reaching this line proves no abend occurred */
    if (rc == 0) {
        record_fail(tname, "close_pds() returned 0 - DYNFREE failure should propagate as non-zero");
        return;
    }

    fprintf(stdout, "  [info] close_pds() returned rc=%d after stolen DD - no abend\n", rc);
    record_pass(tname);
}

/* ------------------------------------------------------------------ */
static void test_dcb_init_free(void)
{
    const char *tname = "test_dcb_init_free";
    struct ihadcb* PTR32 dcb = dcb_init(NULL);
    if (!dcb) {
        record_fail(tname, "dcb_init returned NULL");
        return;
    }
    dcb_free(dcb);
    record_pass(tname);
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    fprintf(stdout, "=== basicddfreetest: DYNFREE / close_pds regression tests ===\n");

    test_dcb_init_free();
    test_alloc_free_roundtrip();
    test_ddfree_null_opts_on_error();
    test_s99_prt_msg_null_opts();
    test_close_pds_dynfree_failure();

    fprintf(stdout, "=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return (g_failed > 0) ? 1 : 0;
}
