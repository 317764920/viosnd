#pragma once

#include "..\..\VirtIO\linux\types.h"

enum {
    VIRTIO_SND_F_CTLS = 0
};

typedef struct virtio_snd_config {
    u32 jacks;
    u32 streams;
    u32 chmaps;
    u32 controls;
} VIRTIO_SND_CONFIG, *PVIRTIO_SND_CONFIG;

enum {
    VIRTIO_SND_VQ_CONTROL = 0,
    VIRTIO_SND_VQ_EVENT,
    VIRTIO_SND_VQ_TX,
    VIRTIO_SND_VQ_RX,
    VIRTIO_SND_VQ_MAX
};

enum {
    VIRTIO_SND_D_OUTPUT = 0,
    VIRTIO_SND_D_INPUT
};

enum {
    VIRTIO_SND_R_JACK_INFO = 1,
    VIRTIO_SND_R_JACK_REMAP,

    VIRTIO_SND_R_PCM_INFO = 0x0100,
    VIRTIO_SND_R_PCM_SET_PARAMS,
    VIRTIO_SND_R_PCM_PREPARE,
    VIRTIO_SND_R_PCM_RELEASE,
    VIRTIO_SND_R_PCM_START,
    VIRTIO_SND_R_PCM_STOP,

    VIRTIO_SND_R_CHMAP_INFO = 0x0200,

    VIRTIO_SND_EVT_JACK_CONNECTED = 0x1000,
    VIRTIO_SND_EVT_JACK_DISCONNECTED,
    VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED = 0x1100,
    VIRTIO_SND_EVT_PCM_XRUN,

    VIRTIO_SND_S_OK = 0x8000,
    VIRTIO_SND_S_BAD_MSG,
    VIRTIO_SND_S_NOT_SUPP,
    VIRTIO_SND_S_IO_ERR
};

typedef struct virtio_snd_hdr {
    u32 code;
} VIRTIO_SND_HDR, *PVIRTIO_SND_HDR;

typedef struct virtio_snd_event {
    VIRTIO_SND_HDR hdr;
    u32 data;
} VIRTIO_SND_EVENT, *PVIRTIO_SND_EVENT;

typedef struct virtio_snd_query_info {
    VIRTIO_SND_HDR hdr;
    u32 start_id;
    u32 count;
    u32 size;
} VIRTIO_SND_QUERY_INFO, *PVIRTIO_SND_QUERY_INFO;

typedef struct virtio_snd_info {
    u32 hda_fn_nid;
} VIRTIO_SND_INFO, *PVIRTIO_SND_INFO;

typedef struct virtio_snd_pcm_hdr {
    VIRTIO_SND_HDR hdr;
    u32 stream_id;
} VIRTIO_SND_PCM_HDR, *PVIRTIO_SND_PCM_HDR;

enum {
    VIRTIO_SND_PCM_F_SHMEM_HOST = 0,
    VIRTIO_SND_PCM_F_SHMEM_GUEST,
    VIRTIO_SND_PCM_F_MSG_POLLING,
    VIRTIO_SND_PCM_F_EVT_SHMEM_PERIODS,
    VIRTIO_SND_PCM_F_EVT_XRUNS
};

enum {
    VIRTIO_SND_PCM_FMT_IMA_ADPCM = 0,
    VIRTIO_SND_PCM_FMT_MU_LAW,
    VIRTIO_SND_PCM_FMT_A_LAW,
    VIRTIO_SND_PCM_FMT_S8,
    VIRTIO_SND_PCM_FMT_U8,
    VIRTIO_SND_PCM_FMT_S16,
    VIRTIO_SND_PCM_FMT_U16,
    VIRTIO_SND_PCM_FMT_S18_3,
    VIRTIO_SND_PCM_FMT_U18_3,
    VIRTIO_SND_PCM_FMT_S20_3,
    VIRTIO_SND_PCM_FMT_U20_3,
    VIRTIO_SND_PCM_FMT_S24_3,
    VIRTIO_SND_PCM_FMT_U24_3,
    VIRTIO_SND_PCM_FMT_S20,
    VIRTIO_SND_PCM_FMT_U20,
    VIRTIO_SND_PCM_FMT_S24,
    VIRTIO_SND_PCM_FMT_U24,
    VIRTIO_SND_PCM_FMT_S32,
    VIRTIO_SND_PCM_FMT_U32,
    VIRTIO_SND_PCM_FMT_FLOAT,
    VIRTIO_SND_PCM_FMT_FLOAT64,
    VIRTIO_SND_PCM_FMT_DSD_U8,
    VIRTIO_SND_PCM_FMT_DSD_U16,
    VIRTIO_SND_PCM_FMT_DSD_U32,
    VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME
};

enum {
    VIRTIO_SND_PCM_RATE_5512 = 0,
    VIRTIO_SND_PCM_RATE_8000,
    VIRTIO_SND_PCM_RATE_11025,
    VIRTIO_SND_PCM_RATE_16000,
    VIRTIO_SND_PCM_RATE_22050,
    VIRTIO_SND_PCM_RATE_32000,
    VIRTIO_SND_PCM_RATE_44100,
    VIRTIO_SND_PCM_RATE_48000,
    VIRTIO_SND_PCM_RATE_64000,
    VIRTIO_SND_PCM_RATE_88200,
    VIRTIO_SND_PCM_RATE_96000,
    VIRTIO_SND_PCM_RATE_176400,
    VIRTIO_SND_PCM_RATE_192000,
    VIRTIO_SND_PCM_RATE_384000
};

typedef struct virtio_snd_pcm_info {
    VIRTIO_SND_INFO hdr;
    u32 features;
    u64 formats;
    u64 rates;
    u8 direction;
    u8 channels_min;
    u8 channels_max;
    u8 padding[5];
} VIRTIO_SND_PCM_INFO, *PVIRTIO_SND_PCM_INFO;

typedef struct virtio_snd_pcm_set_params {
    VIRTIO_SND_PCM_HDR hdr;
    u32 buffer_bytes;
    u32 period_bytes;
    u32 features;
    u8 channels;
    u8 format;
    u8 rate;
    u8 padding;
} VIRTIO_SND_PCM_SET_PARAMS, *PVIRTIO_SND_PCM_SET_PARAMS;

typedef struct virtio_snd_pcm_xfer {
    u32 stream_id;
} VIRTIO_SND_PCM_XFER, *PVIRTIO_SND_PCM_XFER;

typedef struct virtio_snd_pcm_status {
    u32 status;
    u32 latency_bytes;
} VIRTIO_SND_PCM_STATUS, *PVIRTIO_SND_PCM_STATUS;
