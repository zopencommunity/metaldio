#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include "metaldio.h"
#include "wrappers.h"

#if AMODE == 64
	// msf - eventually move the DSA allocation and parms allocation to a per-thread allocation and re-use
	#define MAX_DSA_SIZE 4096

	#pragma linkage(CALL31A, OS)
	int CALL31A(int* fn, char* dsa, unsigned int* parms);

	/*
	 * TEMPORARY DIAGNOSTICS (0C1 in call31asm, devZoauPython build 427).
	 *
	 * This file is compiled by xlc (EBCDIC) but the process stderr is read as
	 * ASCII, so each message is formatted into a buffer, converted with
	 * __e2a_l() and written with write(2).  write() is unbuffered, so the
	 * LAST "[call31] CALL" line printed before the CEE3201S abend identifies
	 * the exact failing call (function name, addresses, parameters).
	 *
	 * Controlled by the DEBUG_CALL31ASM compile-time macro; default ON for
	 * now.  Build with -DCALL31_NODEBUG to compile it out.  Only the first
	 * CALL31_MAX_TRACE calls are traced; anomalies are always reported.
	 */
	#ifndef CALL31_NODEBUG
	#define CALL31_DEBUG 1
	#define CALL31_MAX_TRACE 60
	#define BAR_2GB 0x80000000UL

	extern int __e2a_l(char *, size_t);

	static unsigned long call31_seq = 0;

	static void call31_dbg(const char* fmt, ...) {
		char buf[512];
		va_list ap;
		int n;
		va_start(ap, fmt);
		n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
		va_end(ap);
		if (n < 0) return;
		if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
		buf[n++] = '\n';
		__e2a_l(buf, (size_t)n);
		write(2, buf, (size_t)n);
	}
	#endif

	int call31asm(const char* fn_name, int* fn, unsigned int num_parms, ...) {
		   va_list args;
		   unsigned int i;
		   int rc;
		   unsigned int* r1_31bit_parms = __malloc31(num_parms*sizeof(unsigned int));
		   char* r13_31bit_dsa = __malloc31(MAX_DSA_SIZE);
#ifdef CALL31_DEBUG
		   unsigned long seq = ++call31_seq;
		   int trace = (seq <= CALL31_MAX_TRACE);
		   int anomaly = 0;
		   va_list args_full;
#endif

		   va_start(args, num_parms);
#ifdef CALL31_DEBUG
		   va_copy(args_full, args);

		   /* Anomaly 1: target routine is not addressable in 31 bits.
		      CALL31A does OILH 15,X'8000' + BASSM, which truncates the
		      address to 31 bits; a target >= 2GB branches to a wrong place. */
		   if (((unsigned long)fn) >= BAR_2GB) {
			   anomaly = 1;
			   call31_dbg("[call31] ANOMALY #%lu %s: target fn=%p is ABOVE the 2GB bar; "
			              "BASSM will truncate it -> bad branch",
			              seq, fn_name, (void*)fn);
		   }
		   /* Anomaly 2: __malloc31 failed. */
		   if (r1_31bit_parms == NULL && num_parms > 0) {
			   anomaly = 1;
			   call31_dbg("[call31] ANOMALY #%lu %s: __malloc31(parms) returned NULL",
			              seq, fn_name);
		   }
		   if (r13_31bit_dsa == NULL) {
			   anomaly = 1;
			   call31_dbg("[call31] ANOMALY #%lu %s: __malloc31(dsa) returned NULL",
			              seq, fn_name);
		   }
		   if (trace || anomaly) {
			   call31_dbg("[call31] ENTER #%lu name=%s fn=%p CALL31A=%p num_parms=%u "
			              "parms31=%p dsa31=%p",
			              seq, fn_name, (void*)fn, (void*)CALL31A, num_parms,
			              (void*)r1_31bit_parms, (void*)r13_31bit_dsa);
		   }
#endif
		   for (i=0; i<num_parms; ++i) {
			   unsigned int parm = va_arg(args, unsigned int);
			   r1_31bit_parms[i] = parm;
#ifdef CALL31_DEBUG
			   {
				   /* Re-read the same slot as a full 64-bit value.  If the
				      caller passed a pointer above the bar, the 32-bit copy
				      stored in the parm list is truncated (silent corruption).
				      NOTE: for genuine 'int' args (ttr, len, rname_len) the
				      upper 32 bits are undefined, so a TRUNCATED flag on those
				      parms is a false positive - only trust it for pointers. */
				   unsigned long full = va_arg(args_full, unsigned long);
				   int truncated = (full > 0xFFFFFFFFUL);

				   if (trace || truncated) {
					   call31_dbg("[call31]   #%lu %s parm[%u]=0x%08x full64=0x%016lx%s",
					              seq, fn_name, i, parm, full,
					              truncated ? "  <-- upper32 set (pointer above bar? ignore if int arg)" : "");
				   }
			   }
#endif
		   }
		   va_end(args);
#ifdef CALL31_DEBUG
		   va_end(args_full);
		   if (trace || anomaly) {
			   call31_dbg("[call31] CALL  #%lu %s -> CALL31A(fn=%p, dsa=%p, parms=%p)",
			              seq, fn_name, (void*)fn, (void*)r13_31bit_dsa,
			              (void*)r1_31bit_parms);
		   }
#endif

		   rc = CALL31A(fn, r13_31bit_dsa, r1_31bit_parms);

#ifdef CALL31_DEBUG
		   if (trace || anomaly) {
			   call31_dbg("[call31] RET   #%lu %s rc=%d", seq, fn_name, rc);
		   }
#endif

		   if (r1_31bit_parms) free(r1_31bit_parms);
		   free(r13_31bit_dsa);

		   return rc;
	}
#endif
