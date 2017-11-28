#pragma once
#include "../../dokan/dokan.h"

//#define WIN10_ENABLE_LONG_PATH
#ifdef WIN10_ENABLE_LONG_PATH
//dirty but should be enough
#define DOKAN_MAX_PATH 32768
#else
#define DOKAN_MAX_PATH MAX_PATH
#endif // DEBUG

#define MirrorCheckFlag(val, flag)                                             \
  if (val & flag) {                                                            \
    DbgPrint(L"\t" L#flag L"\n");                                              \
  }

extern BOOL g_UseStdErr;
extern BOOL g_DebugMode;

int MirrorDevInit(LPWSTR PhysicalDrive, PDOKAN_OPTIONS DokanOptions, PDOKAN_OPERATIONS DokanOperations);
void MirrorDevTeardown();
void DbgPrint(LPCWSTR format, ...);