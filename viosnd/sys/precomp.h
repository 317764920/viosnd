#pragma once

#include <ntddk.h>
#include <wdm.h>
#include <portcls.h>
#include <ks.h>
#include <ksmedia.h>
#include <ntstrsafe.h>

#include "trace.h"
#include "viosnd.h"
#include "ViosndPcm.h"
#include "ViosndVirtio.h"
#include "ViosndTopology.h"
#include "ViosndWaveRT.h"

#ifndef VIOSND_ENABLE_LOG
#define VIOSND_ENABLE_LOG 0
#endif

#if VIOSND_ENABLE_LOG
#define VIOSND_LOG(...) DbgPrintEx(__VA_ARGS__)
#else
#define VIOSND_LOG(...) ((void)0)
#endif

inline void *__cdecl operator new(size_t, void *Address)
{
    return Address;
}

inline void __cdecl operator delete(void *, void *)
{
}
