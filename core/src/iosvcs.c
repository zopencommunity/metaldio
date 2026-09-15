#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dio.h"
#include "ihadcb.h"
#include "iosvcs.h"
#include "mem.h"
#include "metaldio.h"
#include "msg.h"
#include "s99.h"
#include "msg.h"


#define DD_SYSTEM "????????"
#define ERRNO_NONEXISTANT_FILE (67)
#define DIO_MSG_BUFF_LEN (4095)

/* s99_init unconditionally overwrites eid/ever/eopts; template values are backstop only. */
static const struct s99_rbx s99rbxtemplate = {S99RBXID, S99RBXVR, {0}, 0, 0, 0};

int dsdd_alloc(struct s99_common_text_unit* dsn, struct s99_common_text_unit* dd, struct s99_common_text_unit* disp, const DBG_Opts* opts)
{
  struct s99rb* PTR32 parms;
  enum s99_verb verb = S99VRBAL;
  struct s99_flag1 s99flag1 = {0};
  struct s99_flag2 s99flag2 = {0};
  size_t num_text_units = 3;
  int rc;
  struct s99_rbx s99rbx = s99rbxtemplate;

  parms = s99_init(verb, s99flag1, s99flag2, &s99rbx, num_text_units, dsn, dd, disp );
  if (!parms) {
    return IOSVC_ERR_SVC99INIT_ALLOC_FAILURE;
  }
  rc = S99(parms);
  if (rc) {
#ifdef DEBUG
    s99_fmt_dmp(opts, parms);
#endif
    s99_prt_msg(opts, parms, rc);
    s99_free(parms); /* free all s99_init allocations on error path */
    return IOSVC_ERR_SVC99_ALLOC_FAILURE;
  }

  struct s99_common_text_unit* ddout = (struct s99_common_text_unit*) parms->s99txtpp[1];
  dd->s99tulng = ddout->s99tulng;
  memcpy(dd->s99tupar, ddout->s99tupar, dd->s99tulng);

  s99_free(parms);
  return IOSVC_ERR_NOERROR;
}

/*
 * Function: ddfree
 *
 * Description:
 *   Frees the allocation represented by the supplied DD text unit by issuing
 *   the SVC 99 DYNFREE request.
 *
 * Parameters:
 *   dd   - DD text unit describing the allocation to free.
 *   opts - Diagnostic output options used for error reporting.
 *
 * Returns:
 *   0 on success, 16 if SVC 99 control block initialization fails, or the
 *   SVC 99 return code 12 when the DYNFREE request fails.
 */
int ddfree(struct s99_common_text_unit* dd, const DBG_Opts* opts)
{
  struct s99rb* PTR32 parms;
  enum s99_verb verb = S99VRBUN;
  struct s99_flag1 s99flag1 = {0};
  struct s99_flag2 s99flag2 = {0};
  size_t num_text_units = 1;
  int rc;
  struct s99_rbx s99rbx = s99rbxtemplate;

  parms = s99_init(verb, s99flag1, s99flag2, &s99rbx, num_text_units, dd );
  if (!parms) {
    errmsg(opts, "Unable to initialize SVC99 (DYNFREE) control blocks\n");
    return 16;
  }

  rc = S99(parms);
  if (rc) {
#ifdef DEBUG
    s99_fmt_dmp(opts, parms); /* hex dump only in debug builds */
#endif
    s99_prt_msg(opts, parms, rc);
    s99_free(parms);
    return rc;
  }

  s99_free(parms);
  return 0;
}

int init_dsnam_text_unit(const char* dsname, struct s99_common_text_unit* dsn, const DBG_Opts* opts)
{
  size_t dsname_len = (dsname == NULL) ? 0 : strlen(dsname);
  if (dsname == NULL || dsname_len == 0 || dsname_len > DS_MAX) {
    errmsg(opts, "Dataset Name <%.*s> is invalid\n", dsname_len, dsname);
    return 8;
  }

  dsn->s99tulng = dsname_len;
  memcpy(dsn->s99tupar, dsname, dsname_len);
  return 0;
}
