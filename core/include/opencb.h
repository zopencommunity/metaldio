#ifndef __OPENCB__
#define __OPENCB__ 1

#include "metaldio.h"

#pragma pack(1)
struct opencb {
  int last_entry:1;
  int disp:3;
  int mode:4;
  int reserved:24;
  void* PTR32 dcb24;
};

#define OPEN_INPUT (0)
#define OPEN_OUTPUT (0xF)
/*
 * OPEN UPDAT: data set is updated in place (see z/OS DFSMS Macro Instructions
 * for Data Sets, SC23-6852, OPEN macro).
 * FIXME: the numeric option-byte value for UPDAT has NOT been verified.
 * Verify by assembling OPEN (dcb,(UPDAT)),MF=L or reading SYS1.MACLIB(OPEN)
 * before relying on this value.
 */
#define OPEN_UPDATE (0x4)
#pragma pack(pop)

#endif
