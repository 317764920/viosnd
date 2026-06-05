#include "precomp.h"

static void
ViosndVirtioDebugPrint(
    _In_z_ const char *Format,
    ...)
{
    UNREFERENCED_PARAMETER(Format);
}

extern "C" {
typedef void (*tDebugPrintFunc)(const char *format, ...);

int virtioDebugLevel = 0;
int bDebugPrint = 0;
tDebugPrintFunc VirtioDebugPrintProc = ViosndVirtioDebugPrint;
}
