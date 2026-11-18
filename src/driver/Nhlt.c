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
 *  - Queries ACPI firmware tables via ZwQuerySystemInformation (SystemFirmwareTableInformation).
 * Copyright 2026 The SOF Authors
 */

#include "Nhlt.h"

#ifdef _KERNEL_MODE
#define SOF_NHLT_TAG 'TLHN'

#define SystemFirmwareTableInformation 76

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Inout_ PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
);
#endif

/**
 * @brief Parses an in-memory ACPI NHLT table and extracts all audio endpoint metadata.
 *
 * @param[in]  NhltBuffer Pointer to raw ACPI NHLT table bytes.
 * @param[in]  NhltSize   Total size of NhltBuffer in bytes.
 * @param[out] Info       Pointer to SOF_NHLT_INFO structure receiving parsed metadata.
 * @return STATUS_SUCCESS on success; STATUS_NOT_FOUND if signature doesn't match;
 *         STATUS_BUFFER_TOO_SMALL or STATUS_INVALID_PARAMETER on malformed table.
 */
NTSTATUS SofNhltParse(
    _In_reads_bytes_(NhltSize) const void *NhltBuffer,
    _In_ size_t NhltSize,
    _Out_ PSOF_NHLT_INFO Info
)
{
    const ACPI_NHLT_HEADER *hdr;
    const UCHAR *curr;
    const UCHAR *end;
    SOF_NHLT_INFO parsed;
    UCHAR endpointCount;
    ULONG epIndex;
    ULONG byteIndex;
    UCHAR checksum = 0;

    if (!Info) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Info, sizeof(*Info));
    if (!NhltBuffer) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&parsed, sizeof(parsed));
    if (NhltSize < sizeof(ACPI_NHLT_HEADER) + 1)
        return STATUS_BUFFER_TOO_SMALL;
    hdr = (const ACPI_NHLT_HEADER *)NhltBuffer;
    if (hdr->Signature != NHLT_ACPI_SIGNATURE) return STATUS_NOT_FOUND;
    if (hdr->Length > NhltSize || hdr->Length < sizeof(ACPI_NHLT_HEADER) + 1)
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
        const UCHAR *epEnd;
        ULONG epLen, capSize, formatIndex;
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
        cfg += capSize;
        if (cfg == epEnd) return STATUS_INVALID_PARAMETER;
        formatCount = *cfg++;
        for (formatIndex = 0; formatIndex < formatCount; ++formatIndex) {
            WAVEFORMATEXTENSIBLE wfe;
            if ((size_t)(epEnd - cfg) < sizeof(wfe) + sizeof(ULONG))
                return STATUS_INVALID_PARAMETER;
            RtlCopyMemory(&wfe, cfg, sizeof(wfe));
            cfg += sizeof(wfe);
            RtlCopyMemory(&capSize, cfg, sizeof(capSize));
            cfg += sizeof(ULONG);
            if (capSize > (size_t)(epEnd - cfg)) return STATUS_INVALID_PARAMETER;
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
    *Info = parsed;
    return STATUS_SUCCESS;
}

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
        SYSTEM_FIRMWARE_TABLE_INFORMATION queryInfo = {0};
        PSYSTEM_FIRMWARE_TABLE_INFORMATION pTableInfo;
        ULONG querySize;
        ULONG tableSize;
        ULONG retLen = 0;
        NTSTATUS status;

        queryInfo.ProviderSignature = ACPI_PROVIDER_SIGNATURE; /* 'ACPI' */
        queryInfo.Action = SystemFirmwareTable_Get;
        queryInfo.TableID = NHLT_ACPI_SIGNATURE; /* 'NHLT' */
        queryInfo.TableBufferLength = 0;

        /* First query to determine buffer size */
        status = ZwQuerySystemInformation(
            SystemFirmwareTableInformation,
            &queryInfo,
            sizeof(queryInfo),
            &retLen
        );

        tableSize = queryInfo.TableBufferLength;
        if (tableSize == 0) {
            tableSize = retLen;
        }

        if (tableSize == 0) {
            return STATUS_NOT_FOUND;
        }

        /* Bound allocation and prevent integer overflow. */
        if (tableSize > 1024u * 1024u) return STATUS_INVALID_PARAMETER;
        querySize = sizeof(SYSTEM_FIRMWARE_TABLE_INFORMATION) + tableSize;
        pTableInfo = (PSYSTEM_FIRMWARE_TABLE_INFORMATION)ExAllocatePool2(
            POOL_FLAG_NON_PAGED,
            querySize,
            SOF_NHLT_TAG
        );
        if (pTableInfo == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(pTableInfo, querySize);
        pTableInfo->ProviderSignature = ACPI_PROVIDER_SIGNATURE;
        pTableInfo->Action = SystemFirmwareTable_Get;
        pTableInfo->TableID = NHLT_ACPI_SIGNATURE;
        pTableInfo->TableBufferLength = tableSize;

        status = ZwQuerySystemInformation(
            SystemFirmwareTableInformation,
            pTableInfo,
            querySize,
            &retLen
        );

        if (!NT_SUCCESS(status)) {
            ExFreePoolWithTag(pTableInfo, SOF_NHLT_TAG);
            return status;
        }

        if (pTableInfo->TableBufferLength == 0 ||
            pTableInfo->TableBufferLength > tableSize) {
            ExFreePoolWithTag(pTableInfo, SOF_NHLT_TAG);
            return STATUS_INVALID_PARAMETER;
        }
        *NhltBufferOut = pTableInfo->TableBuffer;
        *NhltSizeOut = pTableInfo->TableBufferLength;
        return STATUS_SUCCESS;
    }
#else
    {
        /* User-mode implementation via GetSystemFirmwareTable */
        DWORD needed = GetSystemFirmwareTable('ACPI', 'NHLT', NULL, 0);
        PVOID buffer;
        DWORD readBytes;

        if (needed == 0 || needed > 1024u * 1024u) {
            return STATUS_NOT_FOUND;
        }

        buffer = malloc(needed);
        if (buffer == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        readBytes = GetSystemFirmwareTable('ACPI', 'NHLT', buffer, needed);
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
    {
        /* Buffer is offset within SYSTEM_FIRMWARE_TABLE_INFORMATION */
        PSYSTEM_FIRMWARE_TABLE_INFORMATION pTableInfo =
            CONTAINING_RECORD(NhltBuffer, SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer);
        ExFreePoolWithTag(pTableInfo, SOF_NHLT_TAG);
    }
#else
    free(NhltBuffer);
#endif
}
