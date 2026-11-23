/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Loader.c
 * @brief Firmware Loader, Image Parser, and Stream DMA Staging Implementation.
 *
 * @details
 * Orchestrates the full lifecycle of loading, validating, staging, and executing
 * Sound Open Firmware binaries on Intel cAVS and ACE hardware:
 *  1. Image Parsing: Validates the extended manifest header ($AE1 magic), verifies
 *     bounds, strips the host manifest, and inspects module/version headers.
 *  2. DMA Memory Management: Allocates physical memory using KMDF Common Buffer
 *     using the device-visible logical address and controller address width.
 *  3. Orders coherent x64 common-buffer stores before publishing DMA descriptors.
 *  4. Decoupled Code Loader DMA: Programs Intel HD-A stream registers (SD_CTL, SD_CBL,
 *     SD_FORMAT, SD_BDLPL/PU, SD_LVI) and decouples the stream from the physical link
 *     via Processing Pipe Control (PPCTL) and Software Position in Buffer (SPIB).
 *  5. DSP Power & Boot Sequencing: Powers down existing DSP context, asserts host IP
 *     ownership, powers on subsystem (HFDSSCS) and domain gating (HFPWRCTL), transmits
 *     ROM PURGE IPC with stream tag, powers Core 0 (DSP2CXCTL), and polls ROM status
 *     until FW_ENTERED and IPC4 FW_READY notifications are received.
 * Copyright 2026 The SOF Authors
 */

#ifdef _KERNEL_MODE
#include "Device.h"
#include <ntstrsafe.h>
#if defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT) && defined(PUJJO_EXPLICIT_IPC4_FIRMWARE_REVIEWED)
#include <bcrypt.h>
#endif
void SofLogBreadcrumb(const char* text);
#else
#include <windows.h>
#include <strsafe.h>
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_INVALID_IMAGE_FORMAT
#define STATUS_INVALID_IMAGE_FORMAT      ((NTSTATUS)0xC000007BL)
#endif
#ifndef STATUS_INVALID_BUFFER_SIZE
#define STATUS_INVALID_BUFFER_SIZE       ((NTSTATUS)0xC0000206L)
#endif
#define DbgPrintEx(id, level, fmt, ...)  printf(fmt, ##__VA_ARGS__)
#define DPFLTR_DEFAULT_ID 0
#define DPFLTR_ERROR_LEVEL 0
#define DPFLTR_WARNING_LEVEL 0
#define DPFLTR_INFO_LEVEL 0
#endif
#include "Loader.h"
#include "Hw.h"
#include "Ipc.h"
#include "PujjoDmaPlan.h"
#include "PujjoIpc4Protocol.h"

/**
 * @brief Parses an SOF firmware binary and extracts manifest and version metadata.
 *
 * Checks:
 *  - Buffer length against minimum extended manifest header size.
 *  - Magic number equality with SOF_EXT_MAN4_MAGIC_NUMBER ($AE1 / 0x31454124).
 *  - Manifest length boundary within the binary image size.
 *  - cAVS2.5 binary-header bounds at offset 0x2000 within the payload.
 * Extracts the payload offset, payload length, module count, version numbers,
 * and ASCII binary name from the embedded base firmware header.
 *
 * @param[in]  Buffer Pointer to raw binary image in memory.
 * @param[in]  Size   Total size of binary image in bytes.
 * @param[out] Info   Pointer to SOF_FW_IMAGE_INFO structure to receive parsed metadata.
 * @return STATUS_SUCCESS on success; STATUS_INVALID_PARAMETER, STATUS_INVALID_IMAGE_FORMAT,
 *         or STATUS_INVALID_BUFFER_SIZE on validation error.
 */
NTSTATUS
SofParseFirmwareImage(
    _In_reads_bytes_(Size) const void *Buffer,
    _In_ size_t Size,
    _Out_ PSOF_FW_IMAGE_INFO Info
)
{
    const struct sof_ext_manifest4_hdr *extMan;
    const struct sof_man4_fw_binary_header *fwHeader;
    size_t minSize = sizeof(struct sof_ext_manifest4_hdr);
    size_t payloadSize, moduleOffset, configOffset, configCapacity;
    uint32_t moduleIndex;

    if (!Info) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Info, sizeof(SOF_FW_IMAGE_INFO));
    if (!Buffer || Size < minSize) return STATUS_INVALID_PARAMETER;
    /* The public result and DMA lengths are 32-bit. Never silently truncate. */
    if (Size > (size_t)0xffffffffu) return STATUS_INVALID_BUFFER_SIZE;

    extMan = (const struct sof_ext_manifest4_hdr *)Buffer;

    /*
     * Verify Extended Manifest Magic Number:
     * SOF v4 manifests start with 4-byte ASCII string "$AE1" (0x31454124).
     */
    if (extMan->id != SOF_EXT_MAN4_MAGIC_NUMBER) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] Invalid extended manifest magic: 0x%08x\n", extMan->id);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    /* Verify declared manifest length does not exceed overall binary size */
    if (extMan->len < minSize || extMan->len >= Size) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] Manifest length (0x%x) exceeds binary size (0x%zx)\n",
                   extMan->len, Size);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    /* Pujjo ADL-N/cAVS2.5 IPC4 layout: the binary header is at a
     * 0x2000 offset inside the signed payload, not at its first byte.
     * This function is a bounded metadata parser, NOT signature validation.
     * Older cAVS1.5 layouts need an explicit separate policy, not guessing. */
    payloadSize = Size - extMan->len;
    if (payloadSize < SOF_MAN4_FW_HDR_OFFSET ||
        payloadSize - SOF_MAN4_FW_HDR_OFFSET < sizeof(*fwHeader))
        return STATUS_INVALID_BUFFER_SIZE;
    fwHeader = (const struct sof_man4_fw_binary_header *)
        ((const UCHAR *)Buffer + extMan->len + SOF_MAN4_FW_HDR_OFFSET);
    /* rimage SOF_MAN_FW_HDR_ID: ASCII $AM1. */
    if (fwHeader->id != 0x314d4124u ||
        fwHeader->len < sizeof(*fwHeader) ||
        fwHeader->len > payloadSize - SOF_MAN4_FW_HDR_OFFSET)
        return STATUS_INVALID_IMAGE_FORMAT;

    /* Divide before multiplying: reject truncated/overflowing module tables.
     * Firmware signatures are still the ROM/trust policy's responsibility. */
    moduleOffset = SOF_MAN4_FW_HDR_OFFSET + (size_t)fwHeader->len;
    if (fwHeader->num_module_entries == 0 ||
        fwHeader->num_module_entries >
            (payloadSize - moduleOffset) / sizeof(struct sof_man4_module))
        return STATUS_INVALID_IMAGE_FORMAT;
    configOffset = moduleOffset + (size_t)fwHeader->num_module_entries *
        sizeof(struct sof_man4_module);
    configCapacity = (payloadSize - configOffset) /
        sizeof(struct sof_man4_module_config);
    for (moduleIndex = 0; moduleIndex < fwHeader->num_module_entries; ++moduleIndex) {
        const struct sof_man4_module *module = (const struct sof_man4_module *)
            ((const UCHAR *)Buffer + extMan->len + moduleOffset +
             (size_t)moduleIndex * sizeof(struct sof_man4_module));
        /* rimage SOF_MAN_MOD_ID: ASCII $AME. */
        if (module->id != 0x454d4124u)
            return STATUS_INVALID_IMAGE_FORMAT;
        if (module->cfg_count &&
            ((size_t)module->cfg_offset > configCapacity ||
             (size_t)module->cfg_count > configCapacity - module->cfg_offset))
            return STATUS_INVALID_IMAGE_FORMAT;
    }

    Info->RawData = (PVOID)Buffer;
    Info->RawSize = (ULONG)Size;
    Info->PayloadOffset = extMan->len;
    Info->PayloadSize = (ULONG)payloadSize;
    Info->NumModules = fwHeader->num_module_entries;
    Info->MajorVersion = fwHeader->major_version;
    Info->MinorVersion = fwHeader->minor_version;
    Info->HotfixVersion = fwHeader->hotfix_version;
    Info->BuildVersion = fwHeader->build_version;
    Info->PreloadPageCount = fwHeader->preload_page_count;
    RtlCopyMemory(Info->BinaryName, fwHeader->name,
        min(sizeof(Info->BinaryName) - 1, sizeof(fwHeader->name)));
    Info->BinaryName[sizeof(Info->BinaryName) - 1] = '\0';

    return STATUS_SUCCESS;
}

#ifdef _KERNEL_MODE

#define SOF_ADLN_IPC4_IMAGE_BYTES 707036u

/**
 * @brief Reads a complete firmware file from the filesystem into non-paged kernel pool.
 *
 * Uses ZwOpenFile, ZwQueryInformationFile, and ZwReadFile. Opens the file with
 * SYNCHRONIZE and FILE_SYNCHRONOUS_IO_NONALERT flags to read synchronously at PASSIVE_LEVEL.
 *
 * @param[in]  FilePath   Fully-qualified NT object path to firmware file.
 * @param[out] Buffer     Pointer to receive allocated memory buffer containing file data.
 * @param[out] Size       Pointer to receive file size in bytes.
 * @return STATUS_SUCCESS on success, or an NTSTATUS failure code.
 */
NTSTATUS
SofLoadFirmwareFile(
    _In_ PCWSTR FilePath,
    _Out_ PVOID *Buffer,
    _Out_ PULONG Size
)
{
    UNICODE_STRING uniName;
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK ioStatus;
    HANDLE fileHandle = NULL;
    FILE_STANDARD_INFORMATION fileInfo;
    PVOID rawBuffer = NULL;
    ULONG fileSize = 0;
    NTSTATUS status;

    if (!FilePath || !Buffer || !Size)
        return STATUS_INVALID_PARAMETER;

    *Buffer = NULL;
    *Size = 0;

    RtlInitUnicodeString(&uniName, FilePath);
    InitializeObjectAttributes(&objAttr,
                               &uniName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    status = ZwOpenFile(&fileHandle,
                        GENERIC_READ | SYNCHRONIZE,
                        &objAttr,
                        &ioStatus,
                        FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_NONALERT);

    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Failed to open firmware file %ls: 0x%08x\n",
                   FilePath, status);
        return status;
    }

    status = ZwQueryInformationFile(fileHandle,
                                    &ioStatus,
                                    &fileInfo,
                                    sizeof(fileInfo),
                                    FileStandardInformation);

    if (!NT_SUCCESS(status)) {
        ZwClose(fileHandle);
        return status;
    }

    /* The only bootable file is the pinned ADL-N image. Bound nonpaged
     * pool use before allocating or reading an untrusted file length. */
    if (fileInfo.EndOfFile.QuadPart != SOF_ADLN_IPC4_IMAGE_BYTES) {
        ZwClose(fileHandle);
        return STATUS_FILE_INVALID;
    }
    fileSize = SOF_ADLN_IPC4_IMAGE_BYTES;

    rawBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, fileSize, SOF_POOL_TAG);
    if (!rawBuffer) {
        ZwClose(fileHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwReadFile(fileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &ioStatus,
                        rawBuffer,
                        fileSize,
                        NULL,
                        NULL);

    ZwClose(fileHandle);

    if (!NT_SUCCESS(status) || ioStatus.Information != fileSize) {
        ExFreePoolWithTag(rawBuffer, SOF_POOL_TAG);
        return NT_SUCCESS(status) ? STATUS_END_OF_FILE : status;
    }

    *Buffer = rawBuffer;
    *Size = fileSize;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Successfully loaded firmware file %ls (%u bytes)\n",
               FilePath, fileSize);

    return STATUS_SUCCESS;
}

/**
 * @brief Frees a kernel memory buffer allocated by SofLoadFirmwareFile.
 *
 * @param[in] Buffer Pointer to buffer to free.
 */
VOID
SofFreeFirmwareBuffer(
    _In_ PVOID Buffer
)
{
    if (Buffer) {
        ExFreePoolWithTag(Buffer, SOF_POOL_TAG);
    }
}

/**
 * @brief Allocates a physically contiguous or IOMMU-mapped DMA buffer for bus mastering.
 *
 * WDF-only DMA allocation (v0.0.16):
 *  1. Primary: Uses KMDF Common Buffer (WdfCommonBufferCreate). Essential on systems with
 *     IOMMU / Intel VT-d enabled (e.g. DMA Protection, Virtualization-Based Security), as
 *     it guarantees device-visible logical address programming.
 *  2. Refuses allocation if the DMA enabler/common-buffer allocation fails.
 *     Physical-address fallbacks are unsafe with DMA remapping.
 *
 * @param[in]  Ctx    Pointer to the SOF device context.
 * @param[in]  Size   Allocation size in bytes.
 * @param[out] DmaBuf Pointer to SOF_DMA_BUFFER structure receiving memory handles and addresses.
 * @return STATUS_SUCCESS on success, or STATUS_INSUFFICIENT_RESOURCES on failure.
 */
NTSTATUS
SofAllocateDmaBuffer(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ ULONG Size,
    _Out_ PSOF_DMA_BUFFER DmaBuf
)
{
    NTSTATUS status;

    if (!Ctx || !DmaBuf || Size == 0)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(DmaBuf, sizeof(SOF_DMA_BUFFER));
    DmaBuf->Size = Size;

    /* Do not substitute MmGetPhysicalAddress for a DMA logical address.
     * Under DMA remapping the former can address unrelated device memory. */
    if (!Ctx->DmaEnabler) {
        RtlZeroMemory(DmaBuf, sizeof(*DmaBuf));
        return STATUS_INVALID_DEVICE_STATE;
    }
    status = WdfCommonBufferCreate(Ctx->DmaEnabler, Size,
                                   WDF_NO_OBJECT_ATTRIBUTES, &DmaBuf->CommonBuffer);
    if (!NT_SUCCESS(status)) {
        RtlZeroMemory(DmaBuf, sizeof(*DmaBuf));
        return status;
    }
    DmaBuf->VirtualAddress = WdfCommonBufferGetAlignedVirtualAddress(DmaBuf->CommonBuffer);
    DmaBuf->PhysicalAddress = WdfCommonBufferGetAlignedLogicalAddress(DmaBuf->CommonBuffer);
    if (!pj_hda_dma_span_valid(DmaBuf->PhysicalAddress.LowPart,
            (ULONG)DmaBuf->PhysicalAddress.HighPart, Size, Ctx->HdaDma64Bit)) {
        WdfObjectDelete(DmaBuf->CommonBuffer);
        RtlZeroMemory(DmaBuf, sizeof(*DmaBuf));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    RtlZeroMemory(DmaBuf->VirtualAddress, Size);
    return STATUS_SUCCESS;
}

/**
 * @brief Releases memory associated with a SOF_DMA_BUFFER structure.
 *
 * Handles both KMDF Common Buffer deletion and MmFreeContiguousMemory.
 *
 * @param[in]    Ctx    Pointer to the SOF device context.
 * @param[in,out] DmaBuf Pointer to SOF_DMA_BUFFER descriptor to free.
 */
VOID
SofFreeDmaBuffer(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _Inout_ PSOF_DMA_BUFFER DmaBuf
)
{
    UNREFERENCED_PARAMETER(Ctx);

    if (!DmaBuf || !DmaBuf->VirtualAddress)
        return;

    if (DmaBuf->CommonBuffer != NULL) {
        WdfObjectDelete(DmaBuf->CommonBuffer);
        DmaBuf->CommonBuffer = NULL;
    } else {
        MmFreeContiguousMemory(DmaBuf->VirtualAddress);
    }

    DmaBuf->VirtualAddress = NULL;
    DmaBuf->PhysicalAddress.QuadPart = 0;
    DmaBuf->Size = 0;
}

/* Core 0 only, using the cAVS sequence in src/core/intel/hda-dsp.c.
 * Testable independently of WDF. Caller owns the cold boot/power transition.
 * Never write back an inaccessible register or advance on an unacknowledged step. */
static NTSTATUS SofCavsCoreTransition(PSOF_DEVICE_CONTEXT Ctx,
    ULONG mask, ULONG value, ULONG expectedMask, ULONG expected)
{
    ULONG current, attempt;
    if (!Ctx || !Ctx->HwDesc || Ctx->HwDesc->IsAce || !Ctx->Bar4VirtualAddress)
        return STATUS_DEVICE_NOT_READY;
    if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, HDA_DSP_REG_ADSPCS, 4u))
        return STATUS_INVALID_BUFFER_SIZE;
    current = SofDspRead32(Ctx->Bar4VirtualAddress, HDA_DSP_REG_ADSPCS);
    if (current == 0xFFFFFFFFu) return STATUS_DEVICE_NOT_READY;
    if ((current & mask) != value)
        SofDspWrite32(Ctx->Bar4VirtualAddress, HDA_DSP_REG_ADSPCS,
                     (current & ~mask) | value);
    for (attempt = 0; attempt < 5000u; ++attempt) {
        current = SofDspRead32(Ctx->Bar4VirtualAddress, HDA_DSP_REG_ADSPCS);
        if (current == 0xFFFFFFFFu) return STATUS_DEVICE_NOT_READY;
        if ((current & (expectedMask | mask)) == (expected | value))
            return STATUS_SUCCESS;
        KeStallExecutionProcessor(10);
    }
    return STATUS_IO_TIMEOUT;
}

/**
 * @brief Powers down the DSP subsystem, core 0, and gates domain power on Intel ACE.
 *
 * Power Down Sequence on ACE:
 *  1. Reclaims IP & SHIM ownership for the Host CPU (OSEL = 0x2).
 *  2. Powers down Primary Core (clears SPA in MTL_DSP2CXCTL_PRIMARY_CORE) and polls CPA.
 *  3. Gates domain power (clears WPDSPHPXPG in PTL_HFPWRCTL2 / MTL_HFPWRCTL) and polls HFPWRSTS.
 *  4. Powers down DSP subsystem (clears SPA in MTL_HFDSSCS) and polls CPA.
 *
 * @param[in] Ctx Pointer to the SOF device context.
 */
NTSTATUS
SofPowerDownDsp(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    PUCHAR bar4;
    ULONG timeout;
    char bmsg[128];

    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc || SofDeviceRemoved(Ctx))
        return STATUS_DEVICE_NOT_READY;

    bar4 = (PUCHAR)Ctx->Bar4VirtualAddress;

    if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->RomStatusReg, 4u))
        return STATUS_INVALID_BUFFER_SIZE;
    if (Ctx->HwDesc->IsAce) {
    ULONG pwrCtl = (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_4_0 || Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_3_0) ? PTL_HFPWRCTL2 : MTL_HFPWRCTL;
    ULONG pwrSts = (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_4_0 || Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_3_0) ? PTL_HFPWRSTS2 : MTL_HFPWRSTS;
    if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, pwrCtl, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, pwrSts, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, MTL_HFDSSCS, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, MTL_DSP2CXCTL_PRIMARY_CORE, 4u))
        return STATUS_INVALID_BUFFER_SIZE;

    /*
     * 1. Set Host CPU ownership and power down Primary Core:
     * OSEL = 0x2 gives Host CPU ownership of IP & shim registers.
     * Clearing SPA requests core power-off.
     */
    SofDspUpdateBits(bar4, MTL_DSP2CXCTL_PRIMARY_CORE,
                     MTL_DSP2CXCTL_PRIMARY_CORE_OSEL,
                     0x2 << MTL_DSP2CXCTL_PRIMARY_CORE_OSEL_SHIFT);
    SofDspUpdateBits(bar4, MTL_DSP2CXCTL_PRIMARY_CORE,
                     MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK, 0);
    KeStallExecutionProcessor(1000);
    for (timeout = 0; timeout < 5000u; ++timeout) {
        ULONG ctl = SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE);
        if (ctl != 0xFFFFFFFF && !(ctl & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK))
            break;
        KeStallExecutionProcessor(10);
    }
    if (timeout == 5000u) return STATUS_IO_TIMEOUT;

    /*
     * 2. Gate domain power:
     * Clear WPDSPHPXPG bit to gate power to the DSP high-performance domain.
     */
    SofDspUpdateBits(bar4, pwrCtl, MTL_HFPWRCTL_WPDSPHPXPG, 0);
    KeStallExecutionProcessor(1000);
    for (timeout = 0; timeout < 5000u; ++timeout) {
        ULONG psts = SofDspRead32(bar4, pwrSts);
        if (psts != 0xFFFFFFFF && !(psts & MTL_HFPWRSTS_DSPHPXPGS_MASK))
            break;
        KeStallExecutionProcessor(10);
    }
    if (timeout == 5000u) return STATUS_IO_TIMEOUT;

    /*
     * 3. Power down DSP Subsystem:
     * Clear SPA in HFDSSCS to power down the entire audio DSP subsystem.
     */
    SofDspUpdateBits(bar4, MTL_HFDSSCS, MTL_HFDSSCS_SPA_MASK, 0);
    KeStallExecutionProcessor(1000);
    for (timeout = 0; timeout < 5000u; ++timeout) {
        ULONG dsscs = SofDspRead32(bar4, MTL_HFDSSCS);
        if (dsscs != 0xFFFFFFFF && !(dsscs & MTL_HFDSSCS_CPA_MASK))
            break;
        KeStallExecutionProcessor(10);
    }
    if (timeout == 5000u) return STATUS_IO_TIMEOUT;

    RtlStringCbPrintfA(bmsg, sizeof(bmsg),
        "[12-PWR] DSP powered down: Core0 CPA=%s, Subsystem CPA=%s, ROM_STS=0x%08x\n",
        (SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE) & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK) ? "1" : "0",
        (SofDspRead32(bar4, MTL_HFDSSCS) & MTL_HFDSSCS_CPA_MASK) ? "1" : "0",
        SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg));
    SofLogBreadcrumb(bmsg);
    } else {
        if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, HDA_DSP_REG_ADSPCS, 4u))
            return STATUS_INVALID_BUFFER_SIZE;
        NTSTATUS coreStatus;
        /* Stall -> reset -> power off; require each acknowledgement. */
        coreStatus = SofCavsCoreTransition(Ctx, HDA_DSP_ADSPCS_CSTALL_MASK(1),
            HDA_DSP_ADSPCS_CSTALL_MASK(1), 0, 0);
        if (!NT_SUCCESS(coreStatus)) return coreStatus;
        coreStatus = SofCavsCoreTransition(Ctx, HDA_DSP_ADSPCS_CRST_MASK(1),
            HDA_DSP_ADSPCS_CRST_MASK(1), HDA_DSP_ADSPCS_CSTALL_MASK(1),
            HDA_DSP_ADSPCS_CSTALL_MASK(1));
        if (!NT_SUCCESS(coreStatus)) return coreStatus;
        coreStatus = SofCavsCoreTransition(Ctx, HDA_DSP_ADSPCS_SPA_MASK(1), 0,
            HDA_DSP_ADSPCS_CPA_MASK(1) | HDA_DSP_ADSPCS_CRST_MASK(1) | HDA_DSP_ADSPCS_CSTALL_MASK(1),
            HDA_DSP_ADSPCS_CRST_MASK(1) | HDA_DSP_ADSPCS_CSTALL_MASK(1));
        if (!NT_SUCCESS(coreStatus)) return coreStatus;

        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12-PWR] cAVS DSP powered down: ADSPCS=0x%08x, ROM_STS=0x%08x\n",
            SofDspRead32(bar4, HDA_DSP_REG_ADSPCS),
            SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg));
        SofLogBreadcrumb(bmsg);
    }
    Ctx->DspNeedsPowerDown = FALSE;
    return STATUS_SUCCESS;
}

/* SD_CTL is 24 bits and shares a DWORD with the write-one-to-clear
 * SD_STS byte. Preserve the all-ones read sentinel and write zero to
 * SD_STS when updating control. Status is acknowledged separately. */
static ULONG SofCodeLoaderReadControl(_In_ PUCHAR StreamRegs)
{
    ULONG value = READ_REGISTER_ULONG((PULONG)(StreamRegs + SOF_HDA_ADSP_REG_SD_CTL));
    return value == 0xFFFFFFFFu ? value : value & 0x00FFFFFFu;
}

static VOID SofCodeLoaderWriteControl(_In_ PUCHAR StreamRegs, _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(StreamRegs + SOF_HDA_ADSP_REG_SD_CTL),
                         Value & 0x00FFFFFFu);
}

/**
 * @brief Configures HD-A Code Loader decoupled DMA registers and Buffer Descriptor List.
 *
 * Implementation Steps:
 *  1. Verifies HD-A Global Controller is active (GCTL.CRST == 1).
 *  2. Determines Code Loader stream index (first playback stream = NumCaptureStreams).
 *  3. Decouples the stream from the physical HD-A link in PPCTL.
 *  4. Builds a 2-entry Buffer Descriptor List (BDL) mapping the firmware payload into
 *     two halves (HD-A requires LVI >= 1). Leaves completion interrupts masked for polling.
 *  5. Orders CPU stores to the x64 common buffers before publishing the BDL.
 *  6. Performs hardware stream engine reset cycle (assert SRST, poll, clear SRST, poll).
 *  7. Clears status flags (BCIS, FIFOE, DESE).
 *  8. Programs Cyclic Buffer Length (SD_CBL), Stream Format (SD_FORMAT = 0x40, 32-bit,
 *     48kHz, mono), Last Valid Index (SD_LVI = 1), and BDL base addresses (SD_BDLPL/PU).
 *  9. Programs stream tag (tag 1) and enables stream interrupt in SD_CTL and INTCTL.
 * 10. Programs Software Position in Buffer (SPIB) credit to allow decoupled DMA engine
 *     to stream the entire buffer without early wrap-around.
 *
 * @param[in]  Ctx            Pointer to the SOF device context.
 * @param[in]  FwDmaBuf       Pointer to DMA buffer containing stripped firmware payload.
 * @param[in]  BdlDmaBuf      Pointer to DMA buffer containing the 2-entry BDL table.
 * @param[in]  StreamTag      Stream tag number to program in SD_CTL (typically 1).
 * @param[out] OutStreamIndex Optional pointer to receive selected stream index.
 * @return STATUS_SUCCESS on success, or an NTSTATUS failure code.
 */
NTSTATUS
SofConfigureCodeLoaderDma(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ PSOF_DMA_BUFFER FwDmaBuf,
    _In_ PSOF_DMA_BUFFER BdlDmaBuf,
    _In_ ULONG StreamTag,
    _Out_opt_ PULONG OutStreamIndex
)
{
    PUCHAR bar0;
    PUCHAR streamRegs;
    PSOF_INTEL_DSP_BDL bdl;
    ULONG timeout;
    ULONG spibRelative;
    ULONG capOffset;
    ULONG ctl;
    ULONG halfSize;
    pj_dma_plan plan;
    ULONG streamIndex;
    ULONG sdOffset;
    ULONG gctl;
    ULONG ppctl = 0;
    ULONG intctl;
    ULONG spbfcctl = 0;
    char bmsg[256];

    if (OutStreamIndex) *OutStreamIndex = 0;
    if (!Ctx || !Ctx->Bar0VirtualAddress || !FwDmaBuf || !BdlDmaBuf ||
        !FwDmaBuf->CommonBuffer || !BdlDmaBuf->CommonBuffer ||
        !FwDmaBuf->VirtualAddress || !BdlDmaBuf->VirtualAddress ||
        BdlDmaBuf->Size < 2u * sizeof(SOF_INTEL_DSP_BDL) ||
        (BdlDmaBuf->PhysicalAddress.LowPart & 127u) != 0u ||
        StreamTag == 0u || StreamTag > 15u)
        return STATUS_INVALID_PARAMETER;
    halfSize = (FwDmaBuf->Size / 2u) & ~127u;
    if (!pj_dma_plan_pair(FwDmaBuf->PhysicalAddress.LowPart,
                          (ULONG)FwDmaBuf->PhysicalAddress.HighPart,
                          FwDmaBuf->Size, halfSize, &plan))
        return STATUS_INVALID_BUFFER_SIZE;
    if (!pj_hda_stream_slot(Ctx->NumCaptureStreams, Ctx->NumPlaybackStreams,
                            0u, 0u, Ctx->Bar0Length, &streamIndex))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    sdOffset = SOF_HDA_ADSP_LOADER_BASE + (streamIndex * SOF_HDA_ADSP_SD_ENTRY_SIZE);
    if (Ctx->Bar0Length < SOF_HDA_ADSP_SD_ENTRY_SIZE ||
        sdOffset > Ctx->Bar0Length - SOF_HDA_ADSP_SD_ENTRY_SIZE)
        return STATUS_INVALID_BUFFER_SIZE;

    /* All offsets used below must fit in the mapped BAR, including optional
     * capabilities. Subtract before adding offsets to prevent wraparound. */
    if (!pj_ipc4_mmio_span_valid(Ctx->Bar0Length, SOF_HDA_GCTL, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar0Length, SOF_HDA_INTCTL, 4u))
        return STATUS_INVALID_BUFFER_SIZE;
    capOffset = Ctx->PpCapOffset;
    if (capOffset && (capOffset > Ctx->Bar0Length ||
        SOF_HDA_REG_PP_PPCTL > Ctx->Bar0Length - capOffset ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar0Length,
            capOffset + SOF_HDA_REG_PP_PPCTL, 4u)))
        return STATUS_INVALID_BUFFER_SIZE;
    capOffset = Ctx->SpbCapOffset;
    spibRelative = SOF_HDA_SPIB_BASE + streamIndex * SOF_HDA_SPIB_INTERVAL + SOF_HDA_SPIB_SPIB;
    if (capOffset && (capOffset > Ctx->Bar0Length ||
        spibRelative > Ctx->Bar0Length - capOffset ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar0Length, capOffset + spibRelative, 4u) ||
        SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL > Ctx->Bar0Length - capOffset ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar0Length,
            capOffset + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL, 4u)))
        return STATUS_INVALID_BUFFER_SIZE;

    bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;

    /*
     * Ensure HD-A Controller is out of reset (GCTL.CRST == 1).
     * If the controller is in reset, DMA engines cannot accept commands or cycle.
     */
    gctl = readl(bar0 + SOF_HDA_GCTL);
    if (gctl == 0xFFFFFFFFu || !(gctl & SOF_HDA_GCTL_RESET))
        return STATUS_DEVICE_NOT_READY;

    /*
     * Select Code Loader DMA Stream:
     * In Intel HD-A architectures, the Code Loader engine is mapped to the first
     * Playback stream descriptor, which immediately follows the capture streams
     * (streamIndex = NumCaptureStreams).
     */
    streamRegs = bar0 + sdOffset;

    /*
     * Decouple Stream from Link DMA:
     * Set decouple bit (1 << streamIndex) in Processing Pipe Control (PPCTL) so the
     * stream routes directly to the DSP internal FIFO instead of the external audio link.
     */

    /*
     * Build 2-entry Buffer Descriptor List (BDL):
     * The Intel HD-A specification strictly requires Last Valid Index (LVI) >= 1.
     * We divide the firmware payload into two equal halves (128-byte aligned).
     */
    bdl = (PSOF_INTEL_DSP_BDL)BdlDmaBuf->VirtualAddress;

    /* Entry 0: first segment, validated before MMIO. */
    bdl[0].AddrLow = plan.first.low;
    bdl[0].AddrHigh = plan.first.high;
    bdl[0].Size = plan.first.length;
    bdl[0].Ioc = 0;

    /* Entry 1: remaining firmware bytes. ROM progress is polled. */
    bdl[1].AddrLow = plan.second.low;
    bdl[1].AddrHigh = plan.second.high;
    bdl[1].Size = plan.second.length;
    bdl[1].Ioc = 0;

    /* The ADL-N/x64 WDF common buffers are coherent. Publish the completed
     * payload and both descriptors before any MMIO exposes the BDL. */
    KeMemoryBarrier();

    /*
     * 1. Reset Stream Engine:
     * Assert SRST in SD_CTL, poll for hardware confirmation, then clear SRST and poll.
     */
    ctl = SofCodeLoaderReadControl(streamRegs);
    if (ctl == MAXULONG) return STATUS_DEVICE_NOT_READY;
    if ((ctl & (SOF_HDA_SD_CTL_DMA_START | SOF_HDA_SD_CTL_DMA_RESET)) ||
        readl(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPL) != 0 ||
        readl(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPU) != 0)
        return STATUS_DEVICE_BUSY;
    SofCodeLoaderWriteControl(streamRegs, SOF_HDA_SD_CTL_DMA_RESET);
    for (timeout = 0; timeout < 500u; ++timeout) {
        ctl = SofCodeLoaderReadControl(streamRegs);
        if (ctl == 0xFFFFFFFFu) return STATUS_DEVICE_NOT_READY;
        if (ctl & SOF_HDA_SD_CTL_DMA_RESET) break;
        KeStallExecutionProcessor(10);
    }
    if (timeout == 500u) return STATUS_IO_TIMEOUT;

    SofCodeLoaderWriteControl(streamRegs, 0);
    for (timeout = 0; timeout < 500u; ++timeout) {
        ctl = SofCodeLoaderReadControl(streamRegs);
        if (ctl == 0xFFFFFFFFu) return STATUS_DEVICE_NOT_READY;
        if (!(ctl & SOF_HDA_SD_CTL_DMA_RESET)) break;
        KeStallExecutionProcessor(10);
    }
    if (timeout == 500u || (ctl & SOF_HDA_SD_CTL_DMA_START))
        return STATUS_IO_TIMEOUT;

    /* Snapshot the control registers before publishing an address to DMA.
     * An inaccessible capability must never become an all-ones write. */
    intctl = readl(bar0 + SOF_HDA_INTCTL);
    if (Ctx->PpCapOffset)
        ppctl = readl(bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL);
    if (Ctx->SpbCapOffset)
        spbfcctl = readl(bar0 + Ctx->SpbCapOffset + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL);
    if (intctl == 0xFFFFFFFFu || ppctl == 0xFFFFFFFFu || spbfcctl == 0xFFFFFFFFu)
        return STATUS_DEVICE_NOT_READY;
    if (Ctx->PpCapOffset && !(ppctl & SOF_HDA_PPCTL_GPROCEN))
        return STATUS_DEVICE_NOT_READY;
    if ((ppctl | spbfcctl | intctl) & (1u << streamIndex))
        return STATUS_DEVICE_BUSY;

    /* Only decouple a stream after the controller acknowledged its reset. */
    if (Ctx->PpCapOffset) {
        Ctx->BootPpBitsAdded |= ~ppctl & SOF_HDA_PPCTL_PIE;
        writel(ppctl | (1u << streamIndex) | SOF_HDA_PPCTL_GPROCEN | SOF_HDA_PPCTL_PIE,
               bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL);
    }

    /* 2. Clear Status Flags: BCIS (buffer completion), FIFOE (FIFO error), DESE (descriptor error) */
    WRITE_REGISTER_UCHAR((PUCHAR)(streamRegs + SOF_HDA_ADSP_REG_SD_STS), 0x1C);

    /* 3. Program Cyclic Buffer Length (total size of payload) */
    WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CBL), FwDmaBuf->Size);

    /* 4. Program Stream Format: HDA_CL_STREAM_FORMAT = 0x40 (32-bit, 48kHz, 1 channel) */
    WRITE_REGISTER_USHORT((PUSHORT)(streamRegs + SOF_HDA_ADSP_REG_SD_FORMAT), SOF_HDA_CL_STREAM_FORMAT);

    /* 5. Program Last Valid Index (2 entries => index 1) */
    WRITE_REGISTER_USHORT((PUSHORT)(streamRegs + SOF_HDA_ADSP_REG_SD_LVI), 1);

    /* 6. Program BDL Physical Base Address (64-bit split across BDLPL and BDLPU) */
    WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPL),
                         BdlDmaBuf->PhysicalAddress.LowPart);
    WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPU),
                         BdlDmaBuf->PhysicalAddress.HighPart);

    /* The ISR owns only DSP IPC. The loader polls stream status. */
    ctl = StreamTag << 20;
    SofCodeLoaderWriteControl(streamRegs, ctl);

    /* Keep the controller/DSP IRQ route enabled and mask this stream. */
    Ctx->BootIntBitsAdded |= ~intctl & (SOF_HDA_INT_GLOBAL_EN | SOF_HDA_INT_CTRL_EN);
    writel((intctl & ~(1u << streamIndex)) | SOF_HDA_INT_GLOBAL_EN | SOF_HDA_INT_CTRL_EN,
           bar0 + SOF_HDA_INTCTL);

    /*
     * 9. Configure Software Position in Buffer (SPIB) credit for decoupled DMA:
     * For decoupled streams, HD-A hardware uses SPIB to track available FIFO credits.
     * We enable SPIB in SPBFCCTL and write the entire buffer size into the SPIB register.
     */
    if (Ctx->SpbCapOffset) {
        PUCHAR spibBar = bar0 + Ctx->SpbCapOffset;
        PUCHAR spibAddr = spibBar + SOF_HDA_SPIB_BASE + (streamIndex * SOF_HDA_SPIB_INTERVAL) + SOF_HDA_SPIB_SPIB;

        /* Enable SPIB in SPBFCCTL (bit = 1 << streamIndex) */
        writel(spbfcctl | (1u << streamIndex),
               spibBar + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL);

        /* Program software position in buffer to full buffer size */
        writel(FwDmaBuf->Size, spibAddr);

        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12-SPIB] SPBFCCTL=0x%08x, SPIB[%u]=0x%x (addr=%p)\n",
            readl(spibBar + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL),
            streamIndex, readl(spibAddr), spibAddr);
        SofLogBreadcrumb(bmsg);
    }

    RtlStringCbPrintfA(bmsg, sizeof(bmsg),
        "[12b1] DMA Stream: GCTL=0x%08x, PPCTL=0x%08x, INTCTL=0x%08x, SD_CTL=0x%08x, CBL=0x%x, LVI=%u, BDL=0x%llx\n",
        gctl,
        Ctx->PpCapOffset ? readl(bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL) : 0,
        readl(bar0 + SOF_HDA_INTCTL),
        SofCodeLoaderReadControl(streamRegs),
        READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_CBL)),
        READ_REGISTER_USHORT((PUSHORT)(streamRegs + SOF_HDA_ADSP_REG_SD_LVI)),
        BdlDmaBuf->PhysicalAddress.QuadPart);
    SofLogBreadcrumb(bmsg);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Code Loader DMA programmed: StreamIndex=%u (offset 0x%x), StreamTag=%u, BufferSize=0x%x\n",
               streamIndex, sdOffset, StreamTag, FwDmaBuf->Size);

    if (OutStreamIndex) *OutStreamIndex = streamIndex;
    return STATUS_SUCCESS;
}

#if defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT) && defined(PUJJO_EXPLICIT_IPC4_FIRMWARE_REVIEWED)
static NTSTATUS SofVerifyAdlNIpc4Candidate(PUCHAR Image, ULONG ImageBytes)
{
    /* sof-bin v2.12.x/sof-ipc4-v2.12/adl-n/intel-signed/sof-adl-n.ri
     * Git blob 47bb69dd2e22435e6da37ccee17103e0f6452638. The digest
     * pins the artifact. The hardware ROM still owns signature acceptance. */
    static const UCHAR expected[32] = {
        0xbc,0x83,0xcf,0xfa,0x1b,0xb5,0xa4,0xa8,0xda,0x24,0xbb,0xc7,0xbb,0x70,0x40,0xbd,
        0x74,0x4f,0x1e,0x0c,0x63,0xe3,0xd1,0x61,0x51,0xd1,0x76,0x9a,0xc8,0xae,0x82,0x14
    };
    UCHAR digest[sizeof(expected)];
    BCRYPT_ALG_HANDLE algorithm = NULL;
    NTSTATUS status;

    if (ImageBytes != SOF_ADLN_IPC4_IMAGE_BYTES) return STATUS_INVALID_IMAGE_FORMAT;
    status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) return status;
    status = BCryptHash(algorithm, NULL, 0, Image, ImageBytes, digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!NT_SUCCESS(status)) return status;
    return RtlEqualMemory(expected, digest, sizeof(digest)) ?
           STATUS_SUCCESS : STATUS_INVALID_IMAGE_HASH;
}
#endif

/* Remove only bits raised by this boot, after IPC, the cores and both DMA
 * engines are quiet. Preserve unrelated fields on the shared controller. */
NTSTATUS SofRestoreBootController(_In_ PSOF_DEVICE_CONTEXT Ctx)
{
    if (Ctx->BootDmaStopUnconfirmed) return STATUS_DEVICE_NOT_READY;
    if (!Ctx->BootIntBitsAdded && !Ctx->BootPpBitsAdded) return STATUS_SUCCESS;
    if (Ctx->DspNeedsPowerDown || Ctx->IrqMaskNeeded ||
        !Ctx->Bar0VirtualAddress || SofDeviceRemoved(Ctx))
        return STATUS_DEVICE_NOT_READY;
    PUCHAR bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;
    PUCHAR regs[] = {bar0 + SOF_HDA_INTCTL, bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL};
    ULONG *owned[] = {&Ctx->BootIntBitsAdded, &Ctx->BootPpBitsAdded};
    for (ULONG i = 0; i < RTL_NUMBER_OF(regs); ++i) {
        ULONG mask = *owned[i];
        if (mask) {
            ULONG value = readl(regs[i]);
            if (value == MAXULONG) return STATUS_DEVICE_NOT_READY;
            writel(value & ~mask, regs[i]);
            value = readl(regs[i]);
            if (value == MAXULONG || (value & mask)) return STATUS_DEVICE_NOT_READY;
            *owned[i] = 0;
        }
    }
    return STATUS_SUCCESS;
}

#ifdef PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT
static NTSTATUS SofAcquireBootProcessing(PSOF_DEVICE_CONTEXT Ctx)
{
    PUCHAR bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;
    if (Ctx->BootDmaStopUnconfirmed || Ctx->BootIntBitsAdded || Ctx->BootPpBitsAdded ||
        SofDeviceRemoved(Ctx)) return STATUS_DEVICE_NOT_READY;
    if (!pj_ipc4_mmio_span_valid(Ctx->Bar0Length, SOF_HDA_GCTL, sizeof(ULONG)) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar0Length, SOF_HDA_INTCTL, sizeof(ULONG)) ||
        !Ctx->PpCapOffset || Ctx->PpCapOffset > Ctx->Bar0Length ||
        SOF_HDA_REG_PP_PPCTL + sizeof(ULONG) > Ctx->Bar0Length - Ctx->PpCapOffset)
        return STATUS_INVALID_BUFFER_SIZE;
    ULONG gctl = readl(bar0 + SOF_HDA_GCTL);
    if (gctl == MAXULONG || !(gctl & SOF_HDA_GCTL_RESET)) {
        SofLogBreadcrumb("[HDA] Controller unavailable or held in reset before DSP access.\n");
        return STATUS_DEVICE_NOT_READY;
    }
    PUCHAR reg = bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL;
    ULONG value = readl(reg);
    if (value == MAXULONG) return STATUS_DEVICE_NOT_READY;
    if (!(value & SOF_HDA_PPCTL_GPROCEN)) {
        Ctx->BootPpBitsAdded = SOF_HDA_PPCTL_GPROCEN;
        writel(value | SOF_HDA_PPCTL_GPROCEN, reg);
        value = readl(reg);
        if (value == MAXULONG || !(value & SOF_HDA_PPCTL_GPROCEN))
            return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

/* Confirm both halves of the descriptor address, failing closed on loss. */
static NTSTATUS SofLoaderDmaAddressesMatch(PUCHAR registers, ULONG low, ULONG high)
{
    ULONG actualLow, actualHigh;
    if (!registers) return STATUS_DEVICE_NOT_READY;
    actualLow = readl(registers + SOF_HDA_ADSP_REG_SD_BDLPL);
    actualHigh = readl(registers + SOF_HDA_ADSP_REG_SD_BDLPU);
    if (actualLow == MAXULONG || actualHigh == MAXULONG ||
        actualLow != low || actualHigh != high)
        return STATUS_DEVICE_NOT_READY;
    return STATUS_SUCCESS;
}

/* cAVS 2.5 ICCMAX boot workaround, as used by the Linux ADL-N
 * descriptor. A temporary capture descriptor keeps the platform guardband
 * active during boot. It is unrelated to microphone routing. */
#define SOF_ADLN_LTRP_OFFSET 0x1048u
#define SOF_ADLN_LTRP_GUARDBAND_MASK 0x3fu
#define SOF_ADLN_LTRP_BOOT_GUARDBAND (95u & SOF_ADLN_LTRP_GUARDBAND_MASK)

typedef struct _SOF_ICCMAX_DMA {
    SOF_DMA_BUFFER Data;
    SOF_DMA_BUFFER Bdl;
    PUCHAR Registers;
    PUCHAR Ppctl;
    PUCHAR Ltrp;
    UCHAR OriginalGuardband;
    BOOLEAN OriginalDecoupled;
    BOOLEAN Published;
} SOF_ICCMAX_DMA;

static NTSTATUS SofStartIccmaxDma(PSOF_DEVICE_CONTEXT Ctx, SOF_ICCMAX_DMA *Dma)
{
    ULONG slot, ppctl, ctl;
    UCHAR ltrp;
    pj_dma_plan plan;
    PSOF_INTEL_DSP_BDL bdl;
    NTSTATUS status;
    PUCHAR bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;

    if (!bar0 || Ctx->Bar0Length <= SOF_ADLN_LTRP_OFFSET ||
        !pj_hda_stream_slot(Ctx->NumCaptureStreams, Ctx->NumPlaybackStreams, 1u, 0u,
                            Ctx->Bar0Length, &slot) ||
        !Ctx->PpCapOffset || Ctx->PpCapOffset > Ctx->Bar0Length ||
        SOF_HDA_REG_PP_PPCTL + sizeof(ULONG) > Ctx->Bar0Length - Ctx->PpCapOffset)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Dma->Registers = bar0 + SOF_HDA_ADSP_LOADER_BASE + slot * SOF_HDA_ADSP_SD_ENTRY_SIZE;
    Dma->Ppctl = bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL;
    Dma->Ltrp = bar0 + SOF_ADLN_LTRP_OFFSET;
    ppctl = readl(Dma->Ppctl);
    ltrp = READ_REGISTER_UCHAR(Dma->Ltrp);
    ctl = SofCodeLoaderReadControl(Dma->Registers);
    if (ppctl == MAXULONG || ltrp == 0xffu || ctl == MAXULONG ||
        (ctl & (SOF_HDA_SD_CTL_DMA_START | SOF_HDA_SD_CTL_DMA_RESET)) ||
        readl(Dma->Registers + SOF_HDA_ADSP_REG_SD_BDLPL) != 0 ||
        readl(Dma->Registers + SOF_HDA_ADSP_REG_SD_BDLPU) != 0)
        return STATUS_DEVICE_BUSY;
    /* This capture slot is reserved for the workaround. Refuse stale
     * stream errors before publishing either common buffer. */
    UCHAR dmaStatus = READ_REGISTER_UCHAR(Dma->Registers + SOF_HDA_ADSP_REG_SD_STS);
    if (dmaStatus == 0xffu) return STATUS_DEVICE_NOT_READY;
    if (dmaStatus & SOF_HDA_SD_STS_DMA_ERRORS) return STATUS_IO_DEVICE_ERROR;

    status = SofAllocateDmaBuffer(Ctx, PAGE_SIZE, &Dma->Data);
    if (!NT_SUCCESS(status)) return status;
    status = SofAllocateDmaBuffer(Ctx, PAGE_SIZE, &Dma->Bdl);
    if (!NT_SUCCESS(status)) return status;
    if ((Dma->Bdl.PhysicalAddress.LowPart & 127u) ||
        !pj_dma_plan_pair(Dma->Data.PhysicalAddress.LowPart,
                          (ULONG)Dma->Data.PhysicalAddress.HighPart,
                          PAGE_SIZE, PAGE_SIZE / 2u, &plan))
        return STATUS_INVALID_BUFFER_SIZE;
    bdl = Dma->Bdl.VirtualAddress;
    bdl[0].AddrLow = plan.first.low;
    bdl[0].AddrHigh = plan.first.high;
    bdl[0].Size = plan.first.length;
    bdl[1].AddrLow = plan.second.low;
    bdl[1].AddrHigh = plan.second.high;
    bdl[1].Size = plan.second.length;

    KeMemoryBarrier();
    Dma->OriginalGuardband = ltrp & SOF_ADLN_LTRP_GUARDBAND_MASK;
    Dma->OriginalDecoupled = (ppctl & 1u) != 0;
    Dma->Published = TRUE;
    writel(Dma->Bdl.PhysicalAddress.LowPart, Dma->Registers + SOF_HDA_ADSP_REG_SD_BDLPL);
    writel((ULONG)Dma->Bdl.PhysicalAddress.HighPart, Dma->Registers + SOF_HDA_ADSP_REG_SD_BDLPU);
    writel(PAGE_SIZE, Dma->Registers + SOF_HDA_ADSP_REG_SD_CBL);
    writew(1, Dma->Registers + SOF_HDA_ADSP_REG_SD_LVI);
    writel(ppctl | 1u, Dma->Ppctl);
    WRITE_REGISTER_UCHAR(Dma->Ltrp,
        (UCHAR)((ltrp & ~SOF_ADLN_LTRP_GUARDBAND_MASK) | SOF_ADLN_LTRP_BOOT_GUARDBAND));
    ltrp = READ_REGISTER_UCHAR(Dma->Ltrp);
    ppctl = readl(Dma->Ppctl);
    if (ltrp == 0xffu || (ltrp & SOF_ADLN_LTRP_GUARDBAND_MASK) != SOF_ADLN_LTRP_BOOT_GUARDBAND ||
        ppctl == MAXULONG || !(ppctl & 1u) ||
        readl(Dma->Registers + SOF_HDA_ADSP_REG_SD_CBL) != PAGE_SIZE ||
        readw(Dma->Registers + SOF_HDA_ADSP_REG_SD_LVI) != 1u)
        return STATUS_DEVICE_NOT_READY;
    status = SofLoaderDmaAddressesMatch(Dma->Registers,
        Dma->Bdl.PhysicalAddress.LowPart, (ULONG)Dma->Bdl.PhysicalAddress.HighPart);
    if (!NT_SUCCESS(status)) return status;
    SofCodeLoaderWriteControl(Dma->Registers, SOF_HDA_SD_CTL_DMA_START);
    for (ULONG i = 0; i < 500u; ++i) {
        ctl = SofCodeLoaderReadControl(Dma->Registers);
        if (ctl == MAXULONG) return STATUS_DEVICE_NOT_READY;
        if (ctl & SOF_HDA_SD_CTL_DMA_START) return STATUS_SUCCESS;
        KeStallExecutionProcessor(10);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS SofStopIccmaxDma(PSOF_DEVICE_CONTEXT Ctx, SOF_ICCMAX_DMA *Dma)
{
    NTSTATUS status = STATUS_SUCCESS;
    if (Dma->Published) {
        ULONG ctl = SofCodeLoaderReadControl(Dma->Registers), attempt;
        ULONG ppctl;
        UCHAR ltrp, dmaStatus;
        if (ctl == MAXULONG) return STATUS_DEVICE_NOT_READY;
        SofCodeLoaderWriteControl(Dma->Registers, 0);
        for (attempt = 0; attempt < 500u; ++attempt) {
            ctl = SofCodeLoaderReadControl(Dma->Registers);
            if (ctl == MAXULONG) return STATUS_DEVICE_NOT_READY;
            if (!(ctl & SOF_HDA_SD_CTL_DMA_START)) break;
            KeStallExecutionProcessor(10);
        }
        if (attempt == 500u) return STATUS_IO_TIMEOUT;
        writel(0, Dma->Registers + SOF_HDA_ADSP_REG_SD_BDLPL);
        writel(0, Dma->Registers + SOF_HDA_ADSP_REG_SD_BDLPU);
        status = SofLoaderDmaAddressesMatch(Dma->Registers, 0, 0);
        if (!NT_SUCCESS(status)) return status;
        Dma->Published = FALSE;
        dmaStatus = READ_REGISTER_UCHAR(Dma->Registers + SOF_HDA_ADSP_REG_SD_STS);
        if (dmaStatus == 0xffu) status = STATUS_DEVICE_NOT_READY;
        else if (dmaStatus & SOF_HDA_SD_STS_DMA_ERRORS) status = STATUS_IO_DEVICE_ERROR;
        WRITE_REGISTER_UCHAR(Dma->Registers + SOF_HDA_ADSP_REG_SD_STS, SOF_HDA_CL_DMA_SD_INT_MASK);
        ppctl = readl(Dma->Ppctl);
        if (ppctl != MAXULONG) {
            writel((ppctl & ~1u) | (Dma->OriginalDecoupled ? 1u : 0u), Dma->Ppctl);
            ppctl = readl(Dma->Ppctl);
        }
        ltrp = READ_REGISTER_UCHAR(Dma->Ltrp);
        if (ltrp != 0xffu) {
            WRITE_REGISTER_UCHAR(Dma->Ltrp,
                (UCHAR)((ltrp & ~SOF_ADLN_LTRP_GUARDBAND_MASK) | Dma->OriginalGuardband));
            ltrp = READ_REGISTER_UCHAR(Dma->Ltrp);
        }
        if (ppctl == MAXULONG || ((ppctl & 1u) != 0) != Dma->OriginalDecoupled ||
            ltrp == 0xffu || (ltrp & SOF_ADLN_LTRP_GUARDBAND_MASK) != Dma->OriginalGuardband)
            status = STATUS_DEVICE_NOT_READY;
    }
    SofFreeDmaBuffer(Ctx, &Dma->Bdl);
    SofFreeDmaBuffer(Ctx, &Dma->Data);
    return status;
}

static NTSTATUS SofQueryBootedFirmwareVersion(PSOF_DEVICE_CONTEXT Ctx,
                                             const SOF_FW_IMAGE_INFO *Info)
{
    USHORT version[4];
    NTSTATUS status = SofIpcGetBootFirmwareVersion(Ctx, version);

    if (!NT_SUCCESS(status)) return status;
    if (version[0] != Info->MajorVersion || version[1] != Info->MinorVersion ||
        version[2] != Info->HotfixVersion || version[3] != Info->BuildVersion)
        return STATUS_REVISION_MISMATCH;
    return STATUS_SUCCESS;
}
#endif

/**
 * @brief Complete DSP firmware load, hardware initialization, and execution orchestrator.
 *
 * Sequence of Operations:
 *  1. Determines target binary file path based on microarchitecture family (PTL, LNL, MTL, etc.).
 *  2. Loads raw image from filesystem and parses extended manifest ($AE1).
 *  3. Allocates physical DMA staging buffer and 4KB BDL descriptor buffer.
 *  4. Powers down any active DSP state (SofPowerDownDsp).
 *  5. Programs HD-A Code Loader stream registers and SPIB credit.
 *  6. Powers on DSP Subsystem (HFDSSCS) and un-gates domain power (HFPWRCTL).
 *  7. Sends ROM IPC PURGE command with stream tag 1 via HIPCIDR and waits for ROM ACK.
 *  8. Powers up Primary Core 0 (DSP2CXCTL) and sets Host CPU IP ownership.
 *  9. Unmasks Host IPC interrupts in DSP SHIM.
 * 10. Awaits ROM status transition to FSR_STATE_INIT_DONE (0x1).
 * 11. Triggers Code Loader DMA stream RUN bit (SD_CTL.DMA_START).
 * 12. Polls ROM status until FSR_STATE_FW_ENTERED (0x5) is reached.
 * 13. Halts Code Loader DMA and cleans up staging DMA buffers.
 * 14. Awaits SOF_IPC4_NOTIFY_FW_READY notification from running firmware.
 *
 * @param[in,out] Ctx             Pointer to the SOF device context.
 * @param[in,opt] OptionalFwPath  Optional custom path to firmware binary; NULL for default path.
 * @return STATUS_SUCCESS on successful boot, or an NTSTATUS error code.
 */
NTSTATUS
SofStartFirmwareBoot(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_opt_ PCWSTR OptionalFwPath
)
{
#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)
    /* Deliberately no DSP power, ROM commands, or DMA on source-only builds. */
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(OptionalFwPath);
    return STATUS_NOT_SUPPORTED;
#else
    SOF_ICCMAX_DMA iccmax = {0};
    SOF_DMA_BUFFER fwDma = {0};
    SOF_DMA_BUFFER bdlDma = {0};
    SOF_FW_IMAGE_INFO fwInfo = {0};
    PVOID rawFw = NULL;
    ULONG rawFwSize = 0;
    PCWSTR targetPath = OptionalFwPath;
    PUCHAR bar4;
    PUCHAR bar0;
    ULONG streamTag = 1;
    ULONG streamIndex = 0;
    ULONG sdOffset;
    PUCHAR streamRegs = NULL;
    BOOLEAN dmaConfigured = FALSE;
    ULONG ipcHdr;
    ULONG statusReg = 0;
    ULONG timeout;
    char bmsg[128];
    NTSTATUS status;
    KIRQL oldIrql;
    LARGE_INTEGER readyTimeout;
    SOF_FW_BOOT_STATE bootState;

    if (!Ctx || !Ctx->Bar0VirtualAddress || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return STATUS_DEVICE_NOT_READY;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    if (!Ctx->PpCapOffset || !Ctx->SpbCapOffset ||
        !Ctx->NumCaptureStreams || !Ctx->NumPlaybackStreams)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcIda, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcIdr, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcCtl, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcTdr, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, HDA_DSP_REG_ADSPIS, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->RomStatusReg, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->RomErrorReg, 4u))
        return STATUS_INVALID_BUFFER_SIZE;

    /* Do not reset active ownership or a latched IPC fault. */
    KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
    if (Ctx->IpcTxBusy || Ctx->IpcFaulted ||
        Ctx->FwState != SOF_FW_BOOT_NOT_STARTED) {
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
        return STATUS_DEVICE_NOT_READY;
    }
    KeClearEvent(&Ctx->FwReadyEvent);
    KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);

    bar4 = (PUCHAR)Ctx->Bar4VirtualAddress;
    bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;

    /* Only the Pujjo ADL-N controller has a candidate reviewed here. */
    if (Ctx->VendorId != 0x8086 || Ctx->DeviceId != 0x54C8 ||
        Ctx->HwDesc->Family != SOF_HW_INTEL_CAVS_2_5)
        return STATUS_NOT_SUPPORTED;
#if !defined(PUJJO_EXPLICIT_IPC4_FIRMWARE_REVIEWED)
    return STATUS_NOT_SUPPORTED;
#endif
    if (!targetPath)
        targetPath = L"\\SystemRoot\\System32\\drivers\\intel\\sof-ipc4\\adl-n\\sof-adl-n.ri";

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Initiating firmware boot from: %ls\n", targetPath);

    status = SofLoadFirmwareFile(targetPath, &rawFw, &rawFwSize);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Firmware binary not found at %ls (Status: 0x%08x)\n",
                   targetPath, status);
        return status;
    }

#if defined(PUJJO_EXPLICIT_IPC4_FIRMWARE_REVIEWED)
    status = SofVerifyAdlNIpc4Candidate(rawFw, rawFwSize);
    if (!NT_SUCCESS(status)) {
        SofFreeFirmwareBuffer(rawFw);
        return status;
    }
#endif

    status = SofParseFirmwareImage(rawFw, rawFwSize, &fwInfo);
    if (!NT_SUCCESS(status)) {
        SofFreeFirmwareBuffer(rawFw);
        return status;
    }

    RtlStringCbPrintfA(bmsg, sizeof(bmsg),
        "[12a] Firmware file loaded: %ls (raw: %u bytes, payload: 0x%x)\n",
        targetPath, rawFwSize, fwInfo.PayloadSize);
    SofLogBreadcrumb(bmsg);

    /* SOF uuid-registry.txt, in manifest GUID byte order. A firmware image
     * can boot without these modules. Graph operations require a match. */
    {
        static const PJ_BYTE copierUuid[16] = {
            0x83,0x0c,0xa0,0x9b,0x12,0xca,0x83,0x4a,0x94,0x3c,0x1f,0xa2,0xe8,0x2f,0x9d,0xda
        };
        static const PJ_BYTE gainUuid[16] = {
            0xa8,0xa9,0xbc,0x61,0xd0,0x18,0x18,0x4a,0x8e,0x7b,0x26,0x39,0x21,0x98,0x04,0xb7
        };
        RtlZeroMemory(&Ctx->CopierModule, sizeof(Ctx->CopierModule));
        RtlZeroMemory(&Ctx->GainModule, sizeof(Ctx->GainModule));
        /* Cache bounded records before releasing the raw firmware image.
         * On missing or oversized catalogs, leave the role unavailable. */
        RtlZeroMemory(&Ctx->CopierProfile, sizeof(Ctx->CopierProfile));
        RtlZeroMemory(&Ctx->GainProfile, sizeof(Ctx->GainProfile));
        if (pj_firmware_load_profile(rawFw, rawFwSize, copierUuid, &Ctx->CopierProfile) == PJ_CATALOG_OK)
            Ctx->CopierModule = Ctx->CopierProfile.module;
        if (pj_firmware_load_profile(rawFw, rawFwSize, gainUuid, &Ctx->GainProfile) == PJ_CATALOG_OK)
            Ctx->GainModule = Ctx->GainProfile.module;
    }

    /* Allocate Contiguous DMA Buffer for Firmware Payload */
    status = SofAllocateDmaBuffer(Ctx, fwInfo.PayloadSize, &fwDma);
    if (!NT_SUCCESS(status)) {
        char err[128];
        RtlStringCbPrintfA(err, sizeof(err), "[12-ERR] Failed to allocate FW DMA buffer: 0x%08x\n", status);
        SofLogBreadcrumb(err);
        SofFreeFirmwareBuffer(rawFw);
        return status;
    }

    /* Allocate BDL Descriptor List Buffer (4096 bytes) */
    status = SofAllocateDmaBuffer(Ctx, 4096, &bdlDma);
    if (!NT_SUCCESS(status)) {
        char err[128];
        RtlStringCbPrintfA(err, sizeof(err), "[12-ERR] Failed to allocate BDL DMA buffer: 0x%08x\n", status);
        SofLogBreadcrumb(err);
        SofFreeDmaBuffer(Ctx, &fwDma);
        SofFreeFirmwareBuffer(rawFw);
        return status;
    }

    /* Copy stripped payload into DMA staging memory */
    RtlCopyMemory(fwDma.VirtualAddress,
                  (PUCHAR)rawFw + fwInfo.PayloadOffset,
                  fwInfo.PayloadSize);

    /* Free raw file copy */
    SofFreeFirmwareBuffer(rawFw);

    /* ROM ACK is polled by this loader, never by the normal IPC DPC.
     * Disable channel interrupts only after the firmware image is accepted. */
    status = SofAcquireBootProcessing(Ctx);
    if (!NT_SUCCESS(status)) goto boot_failed;
    if (!SofIpcDisableInterrupts(Ctx)) {
        status = STATUS_DEVICE_NOT_READY;
        goto boot_failed;
    }

    /* 0. Ensure DSP is powered down and reset before configuring DMA */
    Ctx->DspNeedsPowerDown = TRUE;
    status = SofPowerDownDsp(Ctx);
    if (!NT_SUCCESS(status)) goto boot_failed;

    /* Configure Code Loader stream on BAR0 */
    status = SofConfigureCodeLoaderDma(Ctx, &fwDma, &bdlDma, streamTag, &streamIndex);
    if (!NT_SUCCESS(status)) {
        char err[128];
        RtlStringCbPrintfA(err, sizeof(err), "[12-ERR] SofConfigureCodeLoaderDma failed: 0x%08x\n", status);
        SofLogBreadcrumb(err);
        goto boot_failed;
    }

    sdOffset = SOF_HDA_ADSP_LOADER_BASE + (streamIndex * SOF_HDA_ADSP_SD_ENTRY_SIZE);
    streamRegs = bar0 + sdOffset;
    dmaConfigured = TRUE;
    status = SofLoaderDmaAddressesMatch(streamRegs,
        bdlDma.PhysicalAddress.LowPart, (ULONG)bdlDma.PhysicalAddress.HighPart);
    if (!NT_SUCCESS(status)) goto boot_failed;
    Ctx->DspNeedsPowerDown = TRUE;
    status = SofStartIccmaxDma(Ctx, &iccmax);
    if (!NT_SUCCESS(status)) goto boot_failed;

    /*
     * 1. Power Up DSP Subsystem & Domain Gating on ACE:
     * Step 1a: Power ON DSP Subsystem (MTL_HFDSSCS: SPA bit 16).
     * Step 1b: Un-gate DSP-0 Domain (PTL_HFPWRCTL2 / MTL_HFPWRCTL: bit 0).
     */
    if (Ctx->HwDesc->IsAce) {
        ULONG pwrCtl = (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_4_0 || Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_3_0) ? PTL_HFPWRCTL2 : MTL_HFPWRCTL;
        ULONG pwrSts = (Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_4_0 || Ctx->HwDesc->Family == SOF_HW_INTEL_ACE_3_0) ? PTL_HFPWRSTS2 : MTL_HFPWRSTS;

        /* Step 1a: Power ON DSP Subsystem */
        SofDspUpdateBits(bar4, MTL_HFDSSCS, MTL_HFDSSCS_SPA_MASK, MTL_HFDSSCS_SPA_MASK);
        KeStallExecutionProcessor(1000);
        for (timeout = 0; timeout < 5000u; ++timeout) {
            ULONG dsscs = SofDspRead32(bar4, MTL_HFDSSCS);
            if (dsscs != 0xFFFFFFFF && (dsscs & MTL_HFDSSCS_CPA_MASK))
                break;
            KeStallExecutionProcessor(10);
        }
        if (timeout == 5000u) {
            status = STATUS_IO_TIMEOUT;
            goto boot_failed;
        }

        /* Step 1b: Un-gate DSP-0 Domain */
        SofDspUpdateBits(bar4, pwrCtl, MTL_HFPWRCTL_WPDSPHPXPG, MTL_HFPWRCTL_WPDSPHPXPG);
        KeStallExecutionProcessor(1000);
        for (timeout = 0; timeout < 5000u; ++timeout) {
            ULONG psts = SofDspRead32(bar4, pwrSts);
            if (psts != 0xFFFFFFFF && (psts & MTL_HFPWRSTS_DSPHPXPGS_MASK))
                break;
            KeStallExecutionProcessor(10);
        }
        if (timeout == 5000u) {
            status = STATUS_IO_TIMEOUT;
            goto boot_failed;
        }

        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12b0] Subsystem & domain power: HFDSSCS=0x%08x, PWRSTS=0x%08x, ROM_STS=0x%08x\n",
            SofDspRead32(bar4, MTL_HFDSSCS), SofDspRead32(bar4, pwrSts),
            SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg));
        SofLogBreadcrumb(bmsg);
    }

    /* cAVS ROM requires core 0 powered while still stalled and reset
     * before the host publishes PURGE. */
    if (!Ctx->HwDesc->IsAce) {
        const ULONG held = HDA_DSP_ADSPCS_CRST_MASK(1) | HDA_DSP_ADSPCS_CSTALL_MASK(1);
        const ULONG ready = HDA_DSP_ADSPCS_SPA_MASK(1) | HDA_DSP_ADSPCS_CPA_MASK(1) | held;
        status = SofCavsCoreTransition(Ctx, HDA_DSP_ADSPCS_SPA_MASK(1),
            HDA_DSP_ADSPCS_SPA_MASK(1), ready, ready);
        if (!NT_SUCCESS(status)) goto boot_failed;
    }

    /*
     * 2. Send ROM Control PURGE command with stream tag:
     * Informs DSP ROM which HD-A stream tag contains the firmware binary.
     */
    ipcHdr = HDA_DSP_ROM_IPC_CONTROL | HDA_DSP_ROM_IPC_PURGE_FW |
             ((streamTag - 1) << 9) | Ctx->HwDesc->HipcIdrBusy;

    {
        ULONG doorbell = SofDspRead32(bar4, Ctx->HwDesc->HipcIdr);
        ULONG ack = SofDspRead32(bar4, Ctx->HwDesc->HipcIda);
        if (doorbell == 0xFFFFFFFFu || ack == 0xFFFFFFFFu ||
            (doorbell & Ctx->HwDesc->HipcIdrBusy) || (ack & Ctx->HwDesc->HipcIdaDone)) {
            status = STATUS_DEVICE_NOT_READY;
            goto boot_failed;
        }
        SofDspWrite32(bar4, Ctx->HwDesc->HipcIdr, ipcHdr);
    }

    /*
     * 3. Power Up DSP Primary Core:
     * On ACE: Set Host CPU IP ownership (OSEL = 0x2) and set SPA bit.
     * On cAVS: Set Core 0 SPA and release Reset/Stall in ADSPCS.
     */
    if (Ctx->HwDesc->IsAce) {
        /* ACE: Program owner of IP & shim registers to Host CPU (OSEL = 0x2) */
        SofDspUpdateBits(bar4, MTL_DSP2CXCTL_PRIMARY_CORE,
                         MTL_DSP2CXCTL_PRIMARY_CORE_OSEL,
                         0x2 << MTL_DSP2CXCTL_PRIMARY_CORE_OSEL_SHIFT);

        /* Enable Primary Core SPA bit */
        SofDspUpdateBits(bar4, MTL_DSP2CXCTL_PRIMARY_CORE,
                         MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK,
                         MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK);

        /* Wait 1ms for CPA to settle */
        KeStallExecutionProcessor(1000);

        /* Poll for CPA confirmation (bit 8, ensure not 0xFFFFFFFF) */
        for (timeout = 0; timeout < 5000u; ++timeout) {
            ULONG ctlVal = SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE);
            if (ctlVal != 0xFFFFFFFF && (ctlVal & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK))
                break;
            KeStallExecutionProcessor(10);
        }
        if (timeout == 5000u) {
            status = STATUS_IO_TIMEOUT;
            goto boot_failed;
        }

        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12b] Core 0 power up: DSP2CXCTL=0x%08x (CPA=%s)\n",
            SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE),
            (SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE) != 0xFFFFFFFF &&
             (SofDspRead32(bar4, MTL_DSP2CXCTL_PRIMARY_CORE) & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK)) ? "SET" : "TIMEOUT");
        SofLogBreadcrumb(bmsg);
    } else {
        /* Match the bundled cAVS reference: leave reset while
         * stalled, then unstall. Check SPA/CPA and reset/stall readback. */
        const ULONG powered = HDA_DSP_ADSPCS_SPA_MASK(1) | HDA_DSP_ADSPCS_CPA_MASK(1);
        const ULONG reset = HDA_DSP_ADSPCS_CRST_MASK(1);
        const ULONG stall = HDA_DSP_ADSPCS_CSTALL_MASK(1);
        status = SofCavsCoreTransition(Ctx, reset, 0, powered | stall, powered | stall);
        if (!NT_SUCCESS(status)) goto boot_failed;
        status = SofCavsCoreTransition(Ctx, stall, 0, powered | reset, powered);
        if (!NT_SUCCESS(status)) goto boot_failed;
    }

    /* Wait for ROM IPC ACK in HIPCIDA */
    for (timeout = 0; timeout < 2000u; ++timeout) {
        ULONG ack = SofDspRead32(bar4, Ctx->HwDesc->HipcIda);
        if (ack == 0xFFFFFFFFu) {
            status = STATUS_DEVICE_NOT_READY;
            goto boot_failed;
        }
        if (ack & Ctx->HwDesc->HipcIdaDone)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &(LARGE_INTEGER){.QuadPart = -10000LL}); /* 1ms */
    }
    if (timeout == 2000u) {
        /* Until the new ROM acknowledges PURGE its SRAM can still contain
         * the previous boot's state. Sample it at the timeout boundary. */
        statusReg = SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg);
        ULONG romError = SofDspRead32(bar4, Ctx->HwDesc->RomErrorReg);
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12-ROM] PURGE ACK timeout: FSR=0x%08x ROM_ERR=0x%08x\n",
            statusReg, romError);
        SofLogBreadcrumb(bmsg);
        status = statusReg == MAXULONG ? STATUS_DEVICE_NOT_READY :
            (statusReg & SOF_FSR_HALTED_MASK ? STATUS_IO_DEVICE_ERROR : STATUS_IO_TIMEOUT);
        goto boot_failed;
    }

    /* Acknowledge ROM */
    SofDspWrite32(bar4, Ctx->HwDesc->HipcIda, Ctx->HwDesc->HipcIdaDone);

    RtlStringCbPrintfA(bmsg, sizeof(bmsg),
        "[12c] ROM PURGE ACK: IDA=0x%08x, ROM_STS=0x%08x\n",
        SofDspRead32(bar4, Ctx->HwDesc->HipcIda),
        SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg));
    SofLogBreadcrumb(bmsg);

    /* Wait for ROM status FSR_STATE_INIT_DONE (0x1) before triggering DMA */
    for (timeout = 0; timeout < 1000u; ++timeout) {
        statusReg = SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg);
        if (statusReg == 0xFFFFFFFFu) {
            status = STATUS_DEVICE_NOT_READY;
            goto boot_failed;
        }
        /* ROM HALTED is a terminal fault, not a slow INIT_DONE transition.
         * Fail before starting the code-loader DMA on a halted DSP. */
        if (statusReg & SOF_FSR_HALTED_MASK) {
            ULONG romError = SofDspRead32(bar4, Ctx->HwDesc->RomErrorReg);
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[12-ROM] ROM halted before INIT_DONE: FSR=0x%08x ROM_ERR=0x%08x\n",
                statusReg, romError);
            SofLogBreadcrumb(bmsg);
            status = STATUS_IO_DEVICE_ERROR;
            goto boot_failed;
        }
        if (FSR_TO_STATE_CODE(statusReg) == FSR_STATE_INIT_DONE)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &(LARGE_INTEGER){.QuadPart = -10000LL}); /* 1ms */
    }
    if (timeout == 1000u) {
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12-ROM] INIT_DONE timeout: FSR=0x%08x ROM_ERR=0x%08x\n",
            statusReg, SofDspRead32(bar4, Ctx->HwDesc->RomErrorReg));
        SofLogBreadcrumb(bmsg);
        status = STATUS_IO_TIMEOUT;
        goto boot_failed;
    }

    /* ROM ACK belongs to the loader. Refuse an uncleared ACK or a stale
     * incoming notification before handing ownership to the runtime IPC ISR. */
    {
        ULONG ack = SofDspRead32(bar4, Ctx->HwDesc->HipcIda);
        ULONG incoming = SofDspRead32(bar4, Ctx->HwDesc->HipcTdr);
        if (ack == 0xFFFFFFFFu || incoming == 0xFFFFFFFFu ||
            (ack & Ctx->HwDesc->HipcIdaDone) || (incoming & Ctx->HwDesc->HipcTdrBusy)) {
            status = STATUS_DEVICE_NOT_READY;
            goto boot_failed;
        }
    }

    /*
     * 5. Trigger Code Loader DMA Start:
     * Set DMA_START (RUN bit) in SD_CTL. The HD-A engine begins reading descriptors
     * and pushing firmware binary data into the DSP memory window.
     */
    {
        ULONG ctlReadback = SofCodeLoaderReadControl(streamRegs);
        ULONG route = readl(bar0 + SOF_HDA_INTCTL);
        ULONG ppctl = readl(bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL);
        ULONG required = SOF_HDA_PPCTL_GPROCEN | SOF_HDA_PPCTL_PIE | (1u << streamIndex);
        ULONG spbctl = readl(bar0 + Ctx->SpbCapOffset + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL);
        ULONG spib = readl(bar0 + Ctx->SpbCapOffset + SOF_HDA_SPIB_BASE +
                          streamIndex * SOF_HDA_SPIB_INTERVAL + SOF_HDA_SPIB_SPIB);
        if (ctlReadback != (streamTag << 20) ||
            route == MAXULONG || (route & (SOF_HDA_INT_GLOBAL_EN | SOF_HDA_INT_CTRL_EN |
                (1u << streamIndex))) != (SOF_HDA_INT_GLOBAL_EN | SOF_HDA_INT_CTRL_EN) ||
            ppctl == MAXULONG || (ppctl & required) != required ||
            spbctl == MAXULONG || !(spbctl & (1u << streamIndex)) ||
            spib != fwInfo.PayloadSize ||
            readw(streamRegs + SOF_HDA_ADSP_REG_SD_LVI) != 1u ||
            readw(streamRegs + SOF_HDA_ADSP_REG_SD_FORMAT) != SOF_HDA_CL_STREAM_FORMAT ||
            readl(streamRegs + SOF_HDA_ADSP_REG_SD_CBL) != fwInfo.PayloadSize ||
            readl(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPL) != bdlDma.PhysicalAddress.LowPart ||
            readl(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPU) != (ULONG)bdlDma.PhysicalAddress.HighPart) {
            status = STATUS_DEVICE_NOT_READY;
            goto stop_dma;
        }
    }
    {
        ULONG ctlVal = SofCodeLoaderReadControl(streamRegs);
        if (ctlVal == 0xFFFFFFFFu || (ctlVal & SOF_HDA_SD_CTL_DMA_START)) {
            status = STATUS_DEVICE_NOT_READY;
            goto dma_unconfirmed;
        }
        /* IRQs are still masked. Reject an incoming doorbell that arrived
         * during descriptor validation, before this image can execute. */
        ULONG incoming = SofDspRead32(bar4, Ctx->HwDesc->HipcTdr);
        if (incoming == MAXULONG || (incoming & Ctx->HwDesc->HipcTdrBusy) ||
            SofDeviceRemoved(Ctx)) {
            status = STATUS_DEVICE_NOT_READY;
            goto stop_dma;
        }
        ctlVal |= SOF_HDA_SD_CTL_DMA_START;
        SofCodeLoaderWriteControl(streamRegs, ctlVal);

        KeStallExecutionProcessor(50);
        ULONG ctlAfter = SofCodeLoaderReadControl(streamRegs);

        if (ctlAfter == 0xFFFFFFFFu || !(ctlAfter & SOF_HDA_SD_CTL_DMA_START)) {
            status = STATUS_DEVICE_NOT_READY;
            goto stop_dma;
        }

        /* A fast response remains latched in HIPCTDR until acknowledged.
         * Admit the DPC only after the stream has acknowledged RUN. */
        KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
        if (Ctx->IpcFaulted || Ctx->FwState != SOF_FW_BOOT_NOT_STARTED) {
            KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
            status = STATUS_DEVICE_NOT_READY;
            goto stop_dma;
        }
        Ctx->FwState = SOF_FW_BOOT_IN_PROGRESS;
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
        if (!SofIpcEnableInterrupts(Ctx)) {
            status = STATUS_DEVICE_NOT_READY;
            goto stop_dma;
        }

        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Code loader DMA triggered on stream %u (offset 0x%x, SD_CTL=0x%08x RUN=%s), streaming firmware...\n",
                   streamIndex, sdOffset, ctlAfter, (ctlAfter & SOF_HDA_SD_CTL_DMA_START) ? "SET" : "FAILED");
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12d] Code loader DMA triggered on stream %u (offset 0x%x), SD_CTL=0x%08x (RUN=%s)\n",
            streamIndex, sdOffset, ctlAfter, (ctlAfter & SOF_HDA_SD_CTL_DMA_START) ? "SET" : "FAILED");
        SofLogBreadcrumb(bmsg);
    }

    /*
     * 6. Wait for ROM status FSR_STATE_FW_ENTERED (0x5).
     * Polls ROM Status register (HFDSC) for up to 4 seconds.
     */
    timeout = 4000; /* 4 seconds */
    while (timeout--) {
        UCHAR dmaStatus = READ_REGISTER_UCHAR(streamRegs + SOF_HDA_ADSP_REG_SD_STS);
        if (dmaStatus == 0xffu || (dmaStatus & SOF_HDA_SD_STS_DMA_ERRORS)) {
            status = dmaStatus == 0xffu ? STATUS_DEVICE_NOT_READY : STATUS_IO_DEVICE_ERROR;
            goto stop_dma;
        }
        statusReg = SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg);
        KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
        bootState = Ctx->FwState;
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
        /* A halted ROM cannot be promoted to success, even if an IPC
         * FW_READY notification raced with the final DMA poll. */
        if (statusReg != 0xFFFFFFFFu && (statusReg & SOF_FSR_HALTED_MASK)) {
            ULONG romError = SofDspRead32(bar4, Ctx->HwDesc->RomErrorReg);
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[12-ROM] ROM halted during firmware DMA: FSR=0x%08x ROM_ERR=0x%08x\n",
                statusReg, romError);
            SofLogBreadcrumb(bmsg);
            status = STATUS_IO_DEVICE_ERROR;
            goto stop_dma;
        }
        if (bootState == SOF_FW_BOOT_FAILED || statusReg == 0xFFFFFFFFu)
            break;
        if (FSR_TO_STATE_CODE(statusReg) == FSR_STATE_FW_ENTERED) {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] *** FIRMWARE SUCCESSFULLY ENTERED (ROM Status: 0x%08x) ***\n",
                       statusReg);
            break;
        }
        if (timeout % 1000 == 0) {
            ULONG lpib = READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_LPIB));
            UCHAR sdsts = READ_REGISTER_UCHAR((PUCHAR)(streamRegs + SOF_HDA_ADSP_REG_SD_STS));
            ULONG romErr = SofDspRead32(bar4, Ctx->HwDesc->RomErrorReg);
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[12-POLL] timeout=%u: ROM_STS=0x%08x, ROM_ERR=0x%08x, LPIB=0x%x, SD_STS=0x%02x\n",
                timeout, statusReg, romErr, lpib, sdsts);
            SofLogBreadcrumb(bmsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] Boot poll: ROM_STS=0x%08x, ROM_ERR=0x%08x, LPIB=0x%x, SD_STS=0x%02x\n",
                       statusReg, romErr, lpib, sdsts);
        }
        KeDelayExecutionThread(KernelMode, FALSE, &(LARGE_INTEGER){.QuadPart = -10000LL}); /* 1ms */
    }

stop_dma:
    /* 7. Stop Code Loader DMA and disable SPIB */
    {
        ULONG ctlVal = SofCodeLoaderReadControl(streamRegs);
        if (ctlVal == 0xFFFFFFFFu) {
            status = STATUS_DEVICE_NOT_READY;
            goto dma_unconfirmed;
        }
        ctlVal &= ~SOF_HDA_SD_CTL_DMA_START;
        SofCodeLoaderWriteControl(streamRegs, ctlVal);

        for (timeout = 0; timeout < 500u; ++timeout) {
            ctlVal = SofCodeLoaderReadControl(streamRegs);
            if (ctlVal != 0xFFFFFFFFu && !(ctlVal & SOF_HDA_SD_CTL_DMA_START))
                break;
            KeStallExecutionProcessor(10);
        }
        if (timeout == 500u) {
            status = STATUS_IO_TIMEOUT;
            goto dma_unconfirmed;
        }

        /* Catch transfer errors even if they appeared at the end of the poll. */
        UCHAR dmaStatus = READ_REGISTER_UCHAR(streamRegs + SOF_HDA_ADSP_REG_SD_STS);
        if (NT_SUCCESS(status)) {
            if (dmaStatus == 0xffu)
                status = STATUS_DEVICE_NOT_READY;
            else if (dmaStatus & SOF_HDA_SD_STS_DMA_ERRORS)
                status = STATUS_IO_DEVICE_ERROR;
        }

        /* Confirm RUN clear and address detachment before freeing buffers. */
        SofCodeLoaderWriteControl(streamRegs, 0);
        WRITE_REGISTER_UCHAR(streamRegs + SOF_HDA_ADSP_REG_SD_STS, SOF_HDA_CL_DMA_SD_INT_MASK);
        writel(0, streamRegs + SOF_HDA_ADSP_REG_SD_BDLPL);
        writel(0, streamRegs + SOF_HDA_ADSP_REG_SD_BDLPU);
        if (!NT_SUCCESS(SofLoaderDmaAddressesMatch(streamRegs, 0, 0))) {
            status = STATUS_DEVICE_NOT_READY;
            goto dma_unconfirmed;
        }
        dmaConfigured = FALSE;
        {
            ULONG intctl = readl(bar0 + SOF_HDA_INTCTL);
            if (intctl == MAXULONG) {
                status = STATUS_DEVICE_NOT_READY;
                goto boot_failed;
            }
            writel(intctl & ~(1u << streamIndex), bar0 + SOF_HDA_INTCTL);
            intctl = readl(bar0 + SOF_HDA_INTCTL);
            if (intctl == MAXULONG || (intctl & (1u << streamIndex))) {
                status = STATUS_DEVICE_NOT_READY;
                goto boot_failed;
            }
        }

        {
            PUCHAR ppControl = bar0 + Ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL;
            ULONG ppctl = readl(ppControl);
            if (ppctl == MAXULONG) {
                status = STATUS_DEVICE_NOT_READY;
                goto boot_failed;
            }
            writel(ppctl & ~(1u << streamIndex), ppControl);
            ppctl = readl(ppControl);
            if (ppctl == MAXULONG || (ppctl & (1u << streamIndex))) {
                status = STATUS_DEVICE_NOT_READY;
                goto boot_failed;
            }
        }

        if (Ctx->SpbCapOffset) {
            PUCHAR spibBar = bar0 + Ctx->SpbCapOffset;
            PUCHAR spibAddr = spibBar + SOF_HDA_SPIB_BASE + (streamIndex * SOF_HDA_SPIB_INTERVAL) + SOF_HDA_SPIB_SPIB;
            ULONG spbControl = readl(spibBar + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL);
            /* RUN is confirmed clear, but an inaccessible capability must
             * not be turned into a control write or a successful boot. */
            if (spbControl == 0xFFFFFFFFu) {
                status = STATUS_DEVICE_NOT_READY;
                goto boot_failed;
            }
            writel(0, spibAddr);
            writel(spbControl & ~(1u << streamIndex),
                   spibBar + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL);
            spbControl = readl(spibBar + SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL);
            if (spbControl == MAXULONG || (spbControl & (1u << streamIndex)) ||
                readl(spibAddr) != 0) {
                status = STATUS_DEVICE_NOT_READY;
                goto boot_failed;
            }
        }
    }

    if (!NT_SUCCESS(status)) goto boot_failed;

    /* 8. Cleanup staging DMA buffers */
    SofFreeDmaBuffer(Ctx, &bdlDma);
    SofFreeDmaBuffer(Ctx, &fwDma);

    status = SofStopIccmaxDma(Ctx, &iccmax);
    if (!NT_SUCCESS(status)) goto boot_failed;

    /* Check if FW entered successfully; a fault must never become success. */
    KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
    bootState = Ctx->FwState;
    KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
    if (bootState == SOF_FW_BOOT_FAILED || statusReg == 0xFFFFFFFFu ||
        FSR_TO_STATE_CODE(statusReg) != FSR_STATE_FW_ENTERED) {
        ULONG lpib = READ_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_LPIB));
        UCHAR sdsts = READ_REGISTER_UCHAR((PUCHAR)(streamRegs + SOF_HDA_ADSP_REG_SD_STS));
        ULONG romErr = SofDspRead32(bar4, Ctx->HwDesc->RomErrorReg);
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12e] Firmware boot timed out: ROM_STS=0x%08x, ROM_ERR=0x%08x, LPIB=0x%x, SD_STS=0x%02x\n",
            statusReg, romErr, lpib, sdsts);
        SofLogBreadcrumb(bmsg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Firmware boot timed out (ROM Status: 0x%08x, ERR: 0x%08x, LPIB: 0x%x, SD_STS: 0x%02x)\n",
                   statusReg, romErr, lpib, sdsts);

        /* Keep an explicit IPC4 boot fault distinct from a plain ROM
         * timeout so a future on-device trace can identify the stage. */
        status = statusReg == MAXULONG ? STATUS_DEVICE_NOT_READY :
            (bootState == SOF_FW_BOOT_FAILED ? STATUS_DEVICE_PROTOCOL_ERROR : STATUS_IO_TIMEOUT);
        goto boot_failed;
    }

    RtlStringCbPrintfA(bmsg, sizeof(bmsg),
        "[12e] *** FIRMWARE ENTERED (ROM Status: 0x%08x) ***\n", statusReg);
    SofLogBreadcrumb(bmsg);

    /* 9. Wait for DSP firmware initialization and SOF_IPC4_NOTIFY_FW_READY */
    SofLogBreadcrumb("[12f] Firmware entered ROM_EXT. Waiting for FW_READY notification...\n");
    /* Only the registered DPC consumes doorbells. The loader waits at
     * PASSIVE_LEVEL; loss of an IRQ fails closed instead of racing the DPC. */
    readyTimeout.QuadPart = -3000LL * 10000LL;
    status = KeWaitForSingleObject(&Ctx->FwReadyEvent, Executive, KernelMode,
                                   FALSE, &readyTimeout);
    KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
    bootState = Ctx->FwState;
    if (status != STATUS_SUCCESS || Ctx->IpcFaulted ||
        bootState != SOF_FW_BOOT_READY_RECEIVED) {
        Ctx->FwState = SOF_FW_BOOT_FAILED;
        Ctx->IpcFaulted = TRUE;
        bootState = SOF_FW_BOOT_FAILED;
    }
    KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);

    if (bootState == SOF_FW_BOOT_READY_RECEIVED) {
        status = SofQueryBootedFirmwareVersion(Ctx, &fwInfo);
        if (!NT_SUCCESS(status)) {
            SofLogBreadcrumb("[IPC4] Running firmware version query failed.\n");
            goto boot_failed;
        }
        KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
        if (!Ctx->IpcFaulted && Ctx->FwState == SOF_FW_BOOT_READY_RECEIVED)
            Ctx->FwState = SOF_FW_BOOT_COMPLETE;
        else
            status = STATUS_DEVICE_NOT_READY;
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
        if (!NT_SUCCESS(status)) goto boot_failed;
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12g] *** DSP FIRMWARE BOOT COMPLETED (FW_READY received)! ***\n");
        SofLogBreadcrumb(bmsg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Firmware boot sequence completed successfully (FW_READY).\n");

        return STATUS_SUCCESS;
    } else {
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[12g] FW_READY wait failed: wait=0x%08x ROM_STS=0x%08x\n",
            status, statusReg);
        SofLogBreadcrumb(bmsg);
        /* ROM entry is not IPC4 readiness. Never manufacture boot success. */
        status = status == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT :
            (status == STATUS_SUCCESS ? STATUS_DEVICE_PROTOCOL_ERROR : STATUS_DEVICE_NOT_READY);
        goto boot_failed;
    }
boot_failed:
    if (dmaConfigured) goto stop_dma;
    /* RUN was never issued or was confirmed clear. */
    SofFreeDmaBuffer(Ctx, &bdlDma);
    SofFreeDmaBuffer(Ctx, &fwDma);
dma_unconfirmed:
    /* On an unconfirmed stop the common buffers stay parented to the WDF
     * DMA enabler for device destruction. Latch the fault to exclude reuse. */
    KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
    Ctx->FwState = SOF_FW_BOOT_FAILED;
    Ctx->IpcFaulted = TRUE;
    KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
    /* ICCMAX must stop even when the code-loader descriptor was lost. */
    if (!NT_SUCCESS(SofStopIccmaxDma(Ctx, &iccmax)))
        SofLogBreadcrumb("[Power] ICCMAX rollback could not be confirmed.\n");
    if (dmaConfigured || iccmax.Published) {
        Ctx->BootDmaStopUnconfirmed = TRUE;
        SofLogBreadcrumb("[DMA] Stop or BDL detach unconfirmed. Retaining common buffers.\n");
    }
    if (Ctx->DspNeedsPowerDown) {
        NTSTATUS rollbackStatus;
        SofIpcDisableInterrupts(Ctx);
        rollbackStatus = SofPowerDownDsp(Ctx);
        if (!NT_SUCCESS(rollbackStatus))
            SofLogBreadcrumb("[Power] DSP rollback could not be confirmed.\n");
    }
    if (!NT_SUCCESS(SofRestoreBootController(Ctx)))
        SofLogBreadcrumb("[HDA] Controller rollback could not be confirmed.\n");
    return status;
#endif /* PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT */
}

#endif /* _KERNEL_MODE */
