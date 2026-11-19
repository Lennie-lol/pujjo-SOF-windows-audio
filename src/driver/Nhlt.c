/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Nhlt.c
 * @brief Intel Non-HD Audio Configuration Table (NHLT) ACPI Parser Implementation.
 *
 * @details
 * Decodes the in-memory or firmware-queried ACPI NHLT table to extract audio hardware topology:
 *  - Validates NHLT signature ('NHLT' / 0x544C484E) and length fields.
 *  - Parses array of endpoint descriptors (DMIC, SSP/I2S, and SoundWire).
 *  - Discovers DMIC microphone array geometry, channel counts, and supported formats.
 *  - Queries ACPI firmware tables through the Windows firmware-table API.
 * Copyright 2026 The SOF Authors
 */

#include "Nhlt.h"

#ifdef _KERNEL_MODE
#include <aux_klib.h>
#define SOF_NHLT_TAG 'TLHN'
#endif

#define SOF_NHLT_MAX_TABLE_BYTES (1024u * 1024u)

/**
 * @brief Parses an in-memory ACPI NHLT table and extracts all audio endpoint metadata.
 *
 * @param[in]  NhltBuffer Pointer to raw ACPI NHLT table bytes.
 * @param[in]  NhltSize   Total size of NhltBuffer in bytes.
 * @param[out] Info       Pointer to SOF_NHLT_INFO structure receiving parsed metadata.
 * @return STATUS_SUCCESS on success; STATUS_NOT_FOUND if signature doesn't match;
 *         STATUS_BUFFER_TOO_SMALL or STATUS_INVALID_PARAMETER on malformed table.
 */
static NTSTATUS SofNhltWalk(const void *NhltBuffer, size_t NhltSize,
                            PSOF_NHLT_INFO Info, SOF_NHLT_VISITOR Visitor,
                            void *Context)
{
    const ACPI_NHLT_HEADER *hdr;
    const UCHAR *curr;
    const UCHAR *end;
    SOF_NHLT_INFO parsed;
    UCHAR endpointCount;
    ULONG epIndex;
    ULONG byteIndex;
    UCHAR checksum = 0;

    if (!NhltBuffer) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&parsed, sizeof(parsed));
    if (NhltSize < sizeof(ACPI_NHLT_HEADER) + 1)
        return STATUS_BUFFER_TOO_SMALL;
    hdr = (const ACPI_NHLT_HEADER *)NhltBuffer;
    if (hdr->Signature != NHLT_ACPI_SIGNATURE) return STATUS_NOT_FOUND;
    if (hdr->Length > NhltSize || hdr->Length > SOF_NHLT_MAX_TABLE_BYTES ||
        hdr->Length < sizeof(ACPI_NHLT_HEADER) + 1)
        return STATUS_INVALID_PARAMETER;
    /* ACPI checksum covers the declared table only. A checksum is not trust. */
    for (byteIndex = 0; byteIndex < hdr->Length; ++byteIndex)
        checksum = (UCHAR)(checksum + ((const UCHAR *)NhltBuffer)[byteIndex]);
    if (checksum != 0) return STATUS_INVALID_PARAMETER;

    curr = (const UCHAR *)NhltBuffer + sizeof(ACPI_NHLT_HEADER);
    end = (const UCHAR *)NhltBuffer + hdr->Length;
    endpointCount = *curr++;
    parsed.TotalEndpoints = endpointCount;

    for (epIndex = 0; epIndex < endpointCount; ++epIndex) {
        const NHLT_ENDPOINT_DESCRIPTOR *ep;
        const UCHAR *cfg;
        const UCHAR *epEnd, *endpointConfig;
        ULONG epLen, capSize, endpointBytes, formatIndex;
        UCHAR formatCount;
        ULONG sampleRate = 0, channels = 0, bitDepth = 0;

        /* Never return a successful partial endpoint inventory. */
        if ((size_t)(end - curr) < sizeof(NHLT_ENDPOINT_DESCRIPTOR))
            return STATUS_INVALID_PARAMETER;
        ep = (const NHLT_ENDPOINT_DESCRIPTOR *)curr;
        epLen = ep->Length;
        if (epLen < sizeof(NHLT_ENDPOINT_DESCRIPTOR) ||
            (size_t)(end - curr) < epLen) return STATUS_INVALID_PARAMETER;
        epEnd = curr + epLen;
        cfg = curr + sizeof(NHLT_ENDPOINT_DESCRIPTOR);

        /* Every endpoint carries a length-prefixed vendor config then
         * a count of format records. Check before advancing any cursor. */
        if ((size_t)(epEnd - cfg) < sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
        RtlCopyMemory(&capSize, cfg, sizeof(capSize));
        cfg += sizeof(ULONG);
        if (capSize > (size_t)(epEnd - cfg)) return STATUS_INVALID_PARAMETER;
        endpointConfig = cfg;
        endpointBytes = capSize;
        cfg += endpointBytes;
        if (cfg == epEnd) return STATUS_INVALID_PARAMETER;
        formatCount = *cfg++;
        if (Visitor)
            Visitor(Context, ep, endpointConfig, endpointBytes, NULL, NULL, 0);
        for (formatIndex = 0; formatIndex < formatCount; ++formatIndex) {
            WAVEFORMATEXTENSIBLE wfe;
            if ((size_t)(epEnd - cfg) < sizeof(wfe) + sizeof(ULONG))
                return STATUS_INVALID_PARAMETER;
            RtlCopyMemory(&wfe, cfg, sizeof(wfe));
            cfg += sizeof(wfe);
            RtlCopyMemory(&capSize, cfg, sizeof(capSize));
            cfg += sizeof(ULONG);
            if (capSize > (size_t)(epEnd - cfg)) return STATUS_INVALID_PARAMETER;
            if (Visitor)
                Visitor(Context, ep, endpointConfig, endpointBytes, &wfe, cfg, capSize);
            cfg += capSize;
            if (formatIndex == 0) {
                sampleRate = wfe.Format.nSamplesPerSec;
                channels = wfe.Format.nChannels;
                bitDepth = wfe.Format.wBitsPerSample;
            }
        }
        /* Unknown extension/trailing bytes are not interpreted as formats. */
        switch (ep->LinkType) {
        case NHLT_LINK_DMIC:
            ++parsed.DmicEndpointsCount;
            parsed.Dmic.Present = TRUE;
            if (parsed.Dmic.ChannelCount == 0) {
                parsed.Dmic.ChannelCount = channels;
                parsed.Dmic.SampleRate = sampleRate;
                parsed.Dmic.BitDepth = bitDepth;
                if (endpointBytes >= 3u && endpointConfig[1] == 1u)
                    parsed.Dmic.ArrayType = endpointConfig[2] & 0x0fu;
            }
            break;
        case NHLT_LINK_SSP:
            ++parsed.SspEndpointsCount;
            break;
        case NHLT_LINK_SDW:
            if (parsed.SdwEndpointsCount >= SOF_MAX_SDW_ENDPOINTS)
                return STATUS_INVALID_PARAMETER;
            {
                PSOF_NHLT_SDW_ENDPOINT sdw = &parsed.SdwEndpoints[parsed.SdwEndpointsCount++];
                sdw->Present = TRUE;
                sdw->LinkIndex = ep->InstanceId;
                sdw->Direction = ep->Direction;
                sdw->VendorId = ep->HwVendorId;
                sdw->DeviceId = ep->HwDeviceId;
                sdw->DeviceType = ep->DeviceType;
                sdw->SampleRate = sampleRate;
                sdw->ChannelCount = channels;
                sdw->BitDepth = bitDepth;
            }
            break;
        default:
            break;
        }
        if ((ep->LinkType == NHLT_LINK_SSP || ep->LinkType == NHLT_LINK_SDW) &&
            ep->HwVendorId != 0 && ep->HwDeviceId != 0) {
            parsed.ExternalCodecPresent = TRUE;
            parsed.CodecVendorId = ep->HwVendorId;
            parsed.CodecDeviceId = ep->HwDeviceId;
            parsed.CodecLinkType = ep->LinkType;
            parsed.CodecInstanceId = ep->InstanceId;
        }
        curr += epLen;
    }
    parsed.IsValid = TRUE;
    if (Info) *Info = parsed;
    return STATUS_SUCCESS;
}

NTSTATUS SofNhltParse(
    _In_reads_bytes_(NhltSize) const void *NhltBuffer,
    _In_ size_t NhltSize,
    _Out_ PSOF_NHLT_INFO Info
)
{
    if (!Info) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Info, sizeof(*Info));
    return SofNhltWalk(NhltBuffer, NhltSize, Info, NULL, NULL);
}

NTSTATUS SofNhltEnumerate(
    _In_reads_bytes_(NhltSize) const void *NhltBuffer,
    _In_ size_t NhltSize,
    _In_ SOF_NHLT_VISITOR Visitor,
    _In_opt_ void *Context
)
{
    NTSTATUS status;

    if (!Visitor) return STATUS_INVALID_PARAMETER;
    /* Validate the entire input before exposing even its first record. */
    status = SofNhltWalk(NhltBuffer, NhltSize, NULL, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;
    return SofNhltWalk(NhltBuffer, NhltSize, NULL, Visitor, Context);
}

typedef struct _SOF_NHLT_MATCH {
    const SOF_NHLT_CONFIG_KEY *Key;
    const NHLT_ENDPOINT_DESCRIPTOR *Endpoint;
    const UCHAR *EndpointConfig;
    ULONG EndpointBytes;
    WAVEFORMATEXTENSIBLE Format;
    const UCHAR *Config;
    ULONG ConfigBytes;
    BOOLEAN Conflict;
} SOF_NHLT_MATCH;

static void SofNhltMatchPcm(
    _In_opt_ void *Context,
    _In_ const NHLT_ENDPOINT_DESCRIPTOR *Endpoint,
    _In_reads_bytes_(EndpointBytes) const UCHAR *EndpointConfig,
    _In_ ULONG EndpointBytes,
    _In_opt_ const WAVEFORMATEXTENSIBLE *Format,
    _In_reads_bytes_opt_(ConfigBytes) const UCHAR *Config,
    _In_ ULONG ConfigBytes
)
{
    /* KSDATAFORMAT_SUBTYPE_PCM, serialized in the NHLT wave format. */
    static const UCHAR pcm[] = {
        1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71
    };
    SOF_NHLT_MATCH *match = (SOF_NHLT_MATCH *)Context;
    const SOF_NHLT_CONFIG_KEY *key = match->Key;
    ULONG frameBytes;

    if (!Format || Endpoint->LinkType != key->LinkType ||
        Endpoint->VirtualBusId != key->VirtualBusId ||
        Endpoint->Direction != key->Direction || Endpoint->DeviceType != key->DeviceType ||
        Format->Format.wFormatTag != 0xfffeu || Format->Format.cbSize != 22u ||
        !RtlEqualMemory(&Format->SubFormat, pcm, sizeof(pcm)) ||
        Format->Format.nSamplesPerSec != key->SampleRate ||
        Format->Format.nChannels != key->Channels ||
        Format->Format.wBitsPerSample != key->ContainerBits ||
        Format->Samples.wValidBitsPerSample != key->ValidBits)
        return;

    frameBytes = (ULONG)key->Channels * (key->ContainerBits / 8u);
    if (Format->Format.nBlockAlign != frameBytes ||
        key->SampleRate > 0xffffffffu / frameBytes ||
        Format->Format.nAvgBytesPerSec != key->SampleRate * frameBytes)
        return;

    if (match->Endpoint) {
        /* Ignore the enclosing descriptor length, which may include other
         * formats. Compare the endpoint identity and both configuration blobs. */
        if (!RtlEqualMemory(&match->Endpoint->LinkType, &Endpoint->LinkType,
                            sizeof(*Endpoint) - sizeof(Endpoint->Length)) ||
            !RtlEqualMemory(&match->Format, Format, sizeof(*Format)) ||
            match->EndpointBytes != EndpointBytes || match->ConfigBytes != ConfigBytes ||
            !RtlEqualMemory(match->EndpointConfig, EndpointConfig, EndpointBytes) ||
            !RtlEqualMemory(match->Config, Config, ConfigBytes))
            match->Conflict = TRUE;
        return;
    }

    match->Endpoint = Endpoint;
    match->EndpointConfig = EndpointConfig;
    match->EndpointBytes = EndpointBytes;
    match->Format = *Format;
    match->Config = Config;
    match->ConfigBytes = ConfigBytes;
}

NTSTATUS SofNhltCopyConfiguration(
    _In_reads_bytes_(NhltSize) const void *NhltBuffer,
    _In_ size_t NhltSize,
    _In_ const SOF_NHLT_CONFIG_KEY *Key,
    _Out_writes_bytes_opt_(Capacity) UCHAR *Config,
    _In_ ULONG Capacity,
    _Out_ PULONG ConfigBytes
)
{
    SOF_NHLT_MATCH match = {0};
    NTSTATUS status;

    if (!ConfigBytes) return STATUS_INVALID_PARAMETER;
    *ConfigBytes = 0;
    if (!Key || (!Config && Capacity) || !Key->SampleRate || !Key->Channels ||
        Key->ContainerBits < 8u || (Key->ContainerBits & 7u) != 0u ||
        !Key->ValidBits || Key->ValidBits > Key->ContainerBits)
        return STATUS_INVALID_PARAMETER;

    match.Key = Key;
    status = SofNhltEnumerate(NhltBuffer, NhltSize, SofNhltMatchPcm, &match);
    if (!NT_SUCCESS(status)) return status;
    if (match.Conflict) return STATUS_OBJECT_NAME_COLLISION;
    if (!match.Endpoint || !match.ConfigBytes) return STATUS_NOT_FOUND;

    *ConfigBytes = match.ConfigBytes;
    if (Capacity < match.ConfigBytes) return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(Config, match.Config, match.ConfigBytes);
    return STATUS_SUCCESS;
}

/* The generated header is reviewed as driver source, never read from a
 * registry path or replaced independently of the signed driver. */
#ifdef _KERNEL_MODE
#ifdef PUJJO_REVIEWED_TOPOLOGY_NHLT
#include "PujjoReviewedTopologyNhlt.h"
#endif
static NTSTATUS SofNhltCopyFallback(NTSTATUS AcpiStatus, const UCHAR *Table,
                                    ULONG Size, PVOID *Buffer, PULONG Bytes)
{
    SOF_NHLT_INFO info;
    PVOID copy;
    NTSTATUS status;
    if (!Buffer || !Bytes) return STATUS_INVALID_PARAMETER;
    *Buffer = NULL;
    *Bytes = 0;
    if (NT_SUCCESS(AcpiStatus)) return STATUS_INVALID_PARAMETER;
    if (AcpiStatus != STATUS_NOT_FOUND && AcpiStatus != STATUS_OBJECT_NAME_NOT_FOUND)
        return AcpiStatus;
    if (!Table || !Size) return STATUS_NOT_FOUND;
    status = SofNhltParse(Table, Size, &info);
    if (!NT_SUCCESS(status)) return status;
    copy = ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, SOF_NHLT_TAG);
    if (!copy) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(copy, Table, Size);
    *Buffer = copy;
    *Bytes = Size;
    return STATUS_SUCCESS;
}

NTSTATUS SofNhltQueryPujjoFallback(NTSTATUS AcpiStatus, PVOID *Buffer, PULONG Bytes)
{
#ifdef PUJJO_REVIEWED_TOPOLOGY_NHLT
    return SofNhltCopyFallback(AcpiStatus, PujjoTopologyNhlt, (ULONG)sizeof(PujjoTopologyNhlt),
                               Buffer, Bytes);
#else
    return SofNhltCopyFallback(AcpiStatus, NULL, 0, Buffer, Bytes);
#endif
}
#endif

NTSTATUS SofNhltQueryAcpiTable(
    _Out_ PVOID *NhltBufferOut,
    _Out_ PULONG NhltSizeOut
)
{
    if (NhltBufferOut == NULL || NhltSizeOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *NhltBufferOut = NULL;
    *NhltSizeOut = 0;

#ifdef _KERNEL_MODE
    {
        ULONG needed = 0, actual = 0;
        PVOID buffer;
        NTSTATUS status = AuxKlibInitialize();
        if (!NT_SUCCESS(status)) return status;

        status = AuxKlibGetSystemFirmwareTable(ACPI_PROVIDER_SIGNATURE,
            NHLT_ACPI_SIGNATURE, NULL, 0, &needed);
        if (!NT_SUCCESS(status) && status != STATUS_BUFFER_TOO_SMALL)
            return status;
        if (needed == 0) return STATUS_NOT_FOUND;
        if (needed > SOF_NHLT_MAX_TABLE_BYTES) return STATUS_INVALID_BUFFER_SIZE;

        buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, needed, SOF_NHLT_TAG);
        if (!buffer) return STATUS_INSUFFICIENT_RESOURCES;
        status = AuxKlibGetSystemFirmwareTable(ACPI_PROVIDER_SIGNATURE,
            NHLT_ACPI_SIGNATURE, buffer, needed, &actual);
        if (!NT_SUCCESS(status) || actual == 0 || actual > needed) {
            ExFreePoolWithTag(buffer, SOF_NHLT_TAG);
            return NT_SUCCESS(status) ? STATUS_INVALID_BUFFER_SIZE : status;
        }
        *NhltBufferOut = buffer;
        *NhltSizeOut = actual;
        return STATUS_SUCCESS;
    }
#else
    {
        /* User-mode implementation via GetSystemFirmwareTable */
        DWORD needed = GetSystemFirmwareTable(ACPI_PROVIDER_SIGNATURE, NHLT_ACPI_SIGNATURE, NULL, 0);
        PVOID buffer;
        DWORD readBytes;

        if (needed == 0 || needed > SOF_NHLT_MAX_TABLE_BYTES) {
            return STATUS_NOT_FOUND;
        }

        buffer = malloc(needed);
        if (buffer == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        readBytes = GetSystemFirmwareTable(ACPI_PROVIDER_SIGNATURE, NHLT_ACPI_SIGNATURE, buffer, needed);
        if (readBytes == 0 || readBytes > needed) {
            free(buffer);
            return STATUS_NOT_FOUND;
        }

        *NhltBufferOut = buffer;
        *NhltSizeOut = readBytes;
        return STATUS_SUCCESS;
    }
#endif
}

VOID SofNhltFreeAcpiTable(
    _In_ PVOID NhltBuffer
)
{
    if (NhltBuffer == NULL) {
        return;
    }

#ifdef _KERNEL_MODE
    ExFreePoolWithTag(NhltBuffer, SOF_NHLT_TAG);
#else
    free(NhltBuffer);
#endif
}
