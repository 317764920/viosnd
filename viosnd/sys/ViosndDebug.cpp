#include "precomp.h"

static void
ViosndVirtioDebugPrint(
    _In_z_ const char *Format,
    ...)
{
#if DBG
    va_list args;

    va_start(args, Format);
    vDbgPrintExWithPrefix("viosnd: ", DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, Format, args);
    va_end(args);
#else
    UNREFERENCED_PARAMETER(Format);
#endif
}

extern "C" {
typedef void (*tDebugPrintFunc)(const char *format, ...);

#if DBG
int virtioDebugLevel = 1;
int bDebugPrint = 1;
#else
int virtioDebugLevel = 0;
int bDebugPrint = 0;
#endif
tDebugPrintFunc VirtioDebugPrintProc = ViosndVirtioDebugPrint;
}
