#include "precomp.h"

static const u64 ViosndDefaultFormatMask = (1ULL << VIRTIO_SND_PCM_FMT_S16);
static const u64 ViosndDefaultRateMask = (1ULL << VIRTIO_SND_PCM_RATE_48000);

VOID
ViosndGetDefaultPcmFormat(
    _Out_ PVIOSND_PCM_FORMAT Format)
{
    RtlZeroMemory(Format, sizeof(*Format));
    Format->SampleRate = VIOSND_DEFAULT_SAMPLE_RATE;
    Format->Channels = VIOSND_DEFAULT_CHANNELS;
    Format->BitsPerSample = VIOSND_DEFAULT_BITS_PER_SAMPLE;
    Format->BufferBytes = VIOSND_DEFAULT_BUFFER_BYTES;
    Format->PeriodBytes = VIOSND_DEFAULT_PERIOD_BYTES;
}

BOOLEAN
ViosndPcmInfoSupportsDefaultFormat(
    _In_ const VIRTIO_SND_PCM_INFO *Info)
{
    if (Info == NULL) {
        return FALSE;
    }

    if ((Info->formats & ViosndDefaultFormatMask) == 0) {
        return FALSE;
    }

    if ((Info->rates & ViosndDefaultRateMask) == 0) {
        return FALSE;
    }

    return Info->channels_min <= VIOSND_DEFAULT_CHANNELS &&
           Info->channels_max >= VIOSND_DEFAULT_CHANNELS;
}

VOID
ViosndBuildSetParams(
    _Out_ VIRTIO_SND_PCM_SET_PARAMS *Params,
    _In_ ULONG StreamId,
    _In_ const VIOSND_PCM_FORMAT *Format)
{
    RtlZeroMemory(Params, sizeof(*Params));
    Params->hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    Params->hdr.stream_id = StreamId;
    Params->buffer_bytes = Format->BufferBytes;
    Params->period_bytes = Format->PeriodBytes;
    Params->features = 0;
    Params->channels = Format->Channels;
    Params->format = VIRTIO_SND_PCM_FMT_S16;
    Params->rate = VIRTIO_SND_PCM_RATE_48000;
}

NTSTATUS
ViosndFindStreamPair(
    _In_reads_(InfoCount) const VIRTIO_SND_PCM_INFO *Info,
    _In_ ULONG InfoCount,
    _Out_ PVIOSND_STREAM_PAIR Pair)
{
    ULONG fallbackRender = MAXULONG;
    ULONG fallbackCapture = MAXULONG;

    RtlZeroMemory(Pair, sizeof(*Pair));

    for (ULONG i = 0; i < InfoCount; ++i) {
        if (Info[i].direction == VIRTIO_SND_D_OUTPUT && fallbackRender == MAXULONG) {
            fallbackRender = i;
        } else if (Info[i].direction == VIRTIO_SND_D_INPUT && fallbackCapture == MAXULONG) {
            fallbackCapture = i;
        }

        if (!ViosndPcmInfoSupportsDefaultFormat(&Info[i])) {
            continue;
        }

        if (!Pair->HasRender && Info[i].direction == VIRTIO_SND_D_OUTPUT) {
            Pair->RenderStreamId = i;
            Pair->HasRender = TRUE;
        } else if (!Pair->HasCapture && Info[i].direction == VIRTIO_SND_D_INPUT) {
            Pair->CaptureStreamId = i;
            Pair->HasCapture = TRUE;
        }

        if (Pair->HasRender && Pair->HasCapture) {
            return STATUS_SUCCESS;
        }
    }

    if (!Pair->HasRender && fallbackRender != MAXULONG) {
        Pair->RenderStreamId = fallbackRender;
        Pair->HasRender = TRUE;
    }

    if (!Pair->HasCapture && fallbackCapture != MAXULONG) {
        Pair->CaptureStreamId = fallbackCapture;
        Pair->HasCapture = TRUE;
    }

    if (!Pair->HasCapture && InfoCount > 1) {
        for (ULONG i = 0; i < InfoCount; ++i) {
            if (!Pair->HasRender || i != Pair->RenderStreamId) {
                Pair->CaptureStreamId = i;
                Pair->HasCapture = TRUE;
                break;
            }
        }
    }

    return Pair->HasRender || Pair->HasCapture ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}
