#pragma once

#include "VirtioSnd.h"

#define VIOSND_DEFAULT_SAMPLE_RATE 48000u
#define VIOSND_FALLBACK_SAMPLE_RATE 16000u
#define VIOSND_DEFAULT_CHANNELS 2u
#define VIOSND_DEFAULT_BITS_PER_SAMPLE 16u
#define VIOSND_DEFAULT_PERIOD_BYTES 2048u
#define VIOSND_FALLBACK_PERIOD_BYTES 2048u
#define VIOSND_DEFAULT_BUFFER_BYTES (VIOSND_DEFAULT_PERIOD_BYTES * 16u)

typedef struct _VIOSND_PCM_FORMAT {
    ULONG SampleRate;
    UCHAR Channels;
    UCHAR BitsPerSample;
    ULONG BufferBytes;
    ULONG PeriodBytes;
} VIOSND_PCM_FORMAT, *PVIOSND_PCM_FORMAT;

typedef struct _VIOSND_STREAM_PAIR {
    ULONG RenderStreamId;
    ULONG CaptureStreamId;
    BOOLEAN HasRender;
    BOOLEAN HasCapture;
} VIOSND_STREAM_PAIR, *PVIOSND_STREAM_PAIR;

VOID
ViosndGetDefaultPcmFormat(
    _Out_ PVIOSND_PCM_FORMAT Format);

VOID
ViosndGetFallbackPcmFormat(
    _Out_ PVIOSND_PCM_FORMAT Format);

BOOLEAN
ViosndPcmInfoSupportsDefaultFormat(
    _In_ const VIRTIO_SND_PCM_INFO *Info);

VOID
ViosndBuildSetParams(
    _Out_ VIRTIO_SND_PCM_SET_PARAMS *Params,
    _In_ ULONG StreamId,
    _In_ const VIOSND_PCM_FORMAT *Format);

NTSTATUS
ViosndFindStreamPair(
    _In_reads_(InfoCount) const VIRTIO_SND_PCM_INFO *Info,
    _In_ ULONG InfoCount,
    _Out_ PVIOSND_STREAM_PAIR Pair);
