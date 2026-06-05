#pragma once

#define VIOSND_WAVEOUT_NAME L"XCBVirtioAudioWaveOut"
#define VIOSND_WAVEIN_NAME  L"XCBVirtioAudioWaveIn"

enum {
    VIOSND_PIN_SYSTEM = 0,
    VIOSND_PIN_BRIDGE = 1
};

NTSTATUS
ViosndCreateWaveRTMiniport(
    _In_ PVIOSND_DEVICE Device,
    _In_ ULONG StreamId,
    _In_ BOOLEAN Capture,
    _Outptr_ PMINIPORT *Miniport);
