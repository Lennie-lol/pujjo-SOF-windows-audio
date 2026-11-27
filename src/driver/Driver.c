/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Driver.c
 * @brief DriverEntry, SafeBootPolicy Engine, and Device Creation for SOF Windows Driver.
 *
 * Implements the driver lifecycle, the 3-Tier Safety Canary watchdog to prevent
 * boot-loop crashes and WDF device initialization.
 *
 * Safety Canary Architecture:
 * - Tier 0 (FORCED_SAFE): Audio DSP hardware is bypassed; driver acts as a stub.
 * - Tier 1 (SINGLE_SHOT): Armed only if BootSafetyToken=1 and \SystemRoot\sof_canary.bin
 *   is present. Atomically clears token on boot to guarantee safe recovery on panic.
 * - Tier 2 (PRODUCTION): Automatic watchdog. Increments ConsecutiveCrashCount before
 *   device start. If the driver operates in D0 for 10 seconds without a kernel panic,
 *   the WatchdogHealthTimer fires and resets the counter to 0. If a crash occurs before
 *   10 seconds, ConsecutiveCrashCount remains elevated, triggering Safe Mode on reboot.
 *
 * Copyright 2026 The SOF Authors
 * Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
 */

#include "Driver.h"
#include "HdaVerb.h"
#include "HdaBusInterface.h"
#include <ntstrsafe.h>

extern void __cdecl __security_init_cookie(void);

NTSYSAPI NTSTATUS NTAPI ZwFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock);

/**
 * @brief Appends a synchronous diagnostic breadcrumb log entry to C:\sof_entry.txt.
 *
 * Operates before KMDF tracing or debuggers are ready, using native ZwCreateFile
 * with FILE_APPEND_DATA and SYNCHRONOUS_IO.
 *
 * @param[in] text Null-terminated ASCII string to log.
 */
void SofLogBreadcrumb(const char* text)
{
    IO_STATUS_BLOCK ioStatus;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING fn;
    HANDLE hFile;
    ULONG len = 0;

    /* Zw file operations require PASSIVE_LEVEL and special APCs enabled. */
    if (!text || KeGetCurrentIrql() != PASSIVE_LEVEL || KeAreAllApcsDisabled())
        return;
    while (text[len]) len++;

    RtlInitUnicodeString(&fn, L"\\DosDevices\\C:\\sof_entry.txt");
    InitializeObjectAttributes(&oa, &fn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    if (NT_SUCCESS(ZwCreateFile(&hFile, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &ioStatus, NULL,
                                FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_OPEN_IF,
                                FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
        ZwWriteFile(hFile, NULL, NULL, NULL, &ioStatus, (PVOID)text, len, NULL, NULL);
        ZwFlushBuffersFile(hFile, &ioStatus);
        ZwClose(hFile);
    }
}

/* Entry is provided by the WDK KMDF toolset. It initializes the security
 * cookie/framework before calling DriverEntry below. No manual /ENTRY shim. */


NTSYSAPI NTSTATUS NTAPI ZwFlushKey(_In_ HANDLE KeyHandle);

/** @brief Global flag indicating if the audio DSP stack is actively armed. */
#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC
volatile LONG g_DiagnosticPreparePermit = 0;
#endif
BOOLEAN g_CanaryArmed = FALSE;

/** @brief Active safe boot policy governing driver recovery behavior. */
/* Boot-only diagnostics fail closed even if the Parameters key is missing. */
#if defined(PUJJO_DSP_BOOT_DIAGNOSTIC) || defined(PUJJO_DMIC_ENUMERATION_CANDIDATE)
ULONG g_SafeBootPolicy = SOF_SAFE_BOOT_FORCED_SAFE;
#else
ULONG g_SafeBootPolicy = SOF_SAFE_BOOT_PRODUCTION;
#endif

/**
 * @brief Reads a 32-bit DWORD value from an open registry key.
 *
 * @param[in] KeyHandle Handle to open registry key.
 * @param[in] ValueName Name of DWORD value to query.
 * @param[in] DefaultValue Value returned if key query fails.
 * @return ULONG Retrieved DWORD value or DefaultValue.
 */
static ULONG
SofReadRegistryDword(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR ValueName,
    _In_ ULONG DefaultValue
)
{
    UNICODE_STRING uName;
    union {
        KEY_VALUE_PARTIAL_INFORMATION Info;
        UCHAR Bytes[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    } buffer;
    PKEY_VALUE_PARTIAL_INFORMATION pInfo = &buffer.Info;
    ULONG resultLength = 0;
    ULONG value;
    NTSTATUS status;

    RtlInitUnicodeString(&uName, ValueName);
    status = ZwQueryValueKey(KeyHandle, &uName, KeyValuePartialInformation,
                             buffer.Bytes, sizeof(buffer.Bytes), &resultLength);
    if (status == STATUS_SUCCESS &&
        resultLength >= FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG) &&
        resultLength <= sizeof(buffer.Bytes) && pInfo->Type == REG_DWORD &&
        pInfo->DataLength == sizeof(ULONG)) {
        RtlCopyMemory(&value, pInfo->Data, sizeof(value));
        return value;
    }
    return DefaultValue;
}

/**
 * @brief Writes a 32-bit DWORD value to an open registry key.
 *
 * @param[in] KeyHandle Handle to open registry key.
 * @param[in] ValueName Name of DWORD value to write.
 * @param[in] Value 32-bit integer to write.
 * @return NTSTATUS Status of the registry write.
 */
static NTSTATUS
SofWriteRegistryDword(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR ValueName,
    _In_ ULONG Value
)
{
    UNICODE_STRING uName;
    RtlInitUnicodeString(&uName, ValueName);
    return ZwSetValueKey(KeyHandle, &uName, 0, REG_DWORD, &Value, sizeof(ULONG));
}

#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC
/* The diagnostic never interprets a partial disarm as authorization. Even
 * an incomplete/invalid request is retired where possible, and both writes
 * are attempted independently so one failure cannot prevent the other reset.
 * Readback is an additional check, NOT a substitute for a successful flush. */
static BOOLEAN
SofCommitDiagnosticDisarm(
    _In_opt_ HANDLE ParamsKey,
    _In_ ULONG RequestedPolicy,
    _In_ ULONG RequestedToken,
    _In_ BOOLEAN CanaryConsumed
)
{
    char msg[256];
    NTSTATUS tokenStatus, policyStatus, flushStatus;
    ULONG tokenReadback, policyReadback;
    BOOLEAN authorized;

    if (ParamsKey == NULL) {
        SofLogBreadcrumb("[Safety] Diagnostic parameters unavailable; no permit.\n");
        return FALSE;
    }
    if (RequestedPolicy == SOF_SAFE_BOOT_FORCED_SAFE && RequestedToken == 0 && !CanaryConsumed)
        return FALSE; /* Ordinary disarmed start does not flush the SYSTEM hive. */

    tokenStatus = SofWriteRegistryDword(ParamsKey, L"BootSafetyToken", 0);
    policyStatus = SofWriteRegistryDword(ParamsKey, L"SafeBootPolicy", SOF_SAFE_BOOT_FORCED_SAFE);
    /* Persist even a partial reset when possible. Never grant on failure. */
    flushStatus = ZwFlushKey(ParamsKey);
    tokenReadback = SofReadRegistryDword(ParamsKey, L"BootSafetyToken", MAXULONG);
    policyReadback = SofReadRegistryDword(ParamsKey, L"SafeBootPolicy", MAXULONG);
    authorized = RequestedPolicy == SOF_SAFE_BOOT_SINGLE_SHOT && RequestedToken == 1 &&
        CanaryConsumed && tokenStatus == STATUS_SUCCESS && policyStatus == STATUS_SUCCESS &&
        flushStatus == STATUS_SUCCESS && tokenReadback == 0 &&
        policyReadback == SOF_SAFE_BOOT_FORCED_SAFE;

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[Safety] Diagnostic disarm: Policy=%u Token=%u File=%u TokenWrite=0x%08x PolicyWrite=0x%08x Flush=0x%08x ReadbackToken=0x%08x ReadbackPolicy=0x%08x Permit=%u\n",
        RequestedPolicy, RequestedToken, CanaryConsumed ? 1u : 0u,
        (ULONG)tokenStatus, (ULONG)policyStatus, (ULONG)flushStatus,
        tokenReadback, policyReadback, authorized ? 1u : 0u);
    SofLogBreadcrumb(msg);
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, "%s", msg);
    return authorized;
}
#endif

/**
 * @brief Atomically detects and consumes the single-shot canary file.
 *
 * Looks for \\SystemRoot\\sof_canary.bin (C:\\Windows\\sof_canary.bin). If present,
 * it marks the file for deletion on close and returns TRUE.
 *
 * @return BOOLEAN TRUE if canary file was present and deleted; FALSE otherwise.
 */
static BOOLEAN
SofCheckAndConsumeCanaryFile(_In_ BOOLEAN LogAttempt)
{
    UNICODE_STRING canPath;
    OBJECT_ATTRIBUTES canOa;
    IO_STATUS_BLOCK canIoStatus;
    HANDLE canHandle = NULL;
    NTSTATUS canStatus;
    char msg[128];
    BOOLEAN filePresent = FALSE;

    /* Native NT kernel path to \SystemRoot\sof_canary.bin (C:\Windows\sof_canary.bin) */
    RtlInitUnicodeString(&canPath, L"\\SystemRoot\\sof_canary.bin");
    InitializeObjectAttributes(&canOa, &canPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    canStatus = ZwCreateFile(
        &canHandle,
        DELETE | FILE_READ_DATA | SYNCHRONIZE,
        &canOa,
        &canIoStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC
        0, /* Exclusive canary open: a sharing conflict denies the attempt. */
#else
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
#endif
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0
    );

    if (LogAttempt) {
        RtlStringCbPrintfA(msg, sizeof(msg), "[Safety] Canary open=0x%08x\n", (ULONG)canStatus);
        SofLogBreadcrumb(msg);
    }
    if (NT_SUCCESS(canStatus) && canHandle != NULL) {
        FILE_DISPOSITION_INFORMATION dispInfo;
        NTSTATUS closeStatus;
        dispInfo.DeleteFile = TRUE;
        canStatus = ZwSetInformationFile(canHandle, &canIoStatus, &dispInfo, sizeof(dispInfo), FileDispositionInformation);
        filePresent = NT_SUCCESS(canStatus);
        closeStatus = ZwClose(canHandle);
        if (!NT_SUCCESS(closeStatus)) filePresent = FALSE;
        if (LogAttempt) {
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[Safety] Canary delete=0x%08x close=0x%08x consumed=%u\n",
                (ULONG)canStatus, (ULONG)closeStatus, filePresent ? 1u : 0u);
            SofLogBreadcrumb(msg);
        }
    }

    return filePresent;
}

/**
 * @brief Primary KMDF DriverEntry callback.
 *
 * Evaluates the 3-Tier Safety Canary policy, consumes tokens, checks crash counts,
 * registers the EvtDeviceAdd callback, and creates the WDFDRIVER object.
 *
 * @param[in] DriverObject Pointer to driver object.
 * @param[in] RegistryPath Path to driver registry key.
 * @return NTSTATUS STATUS_SUCCESS on success, error code otherwise.
 */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
#ifdef PUJJO_DMIC_ENUMERATION_CANDIDATE
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    /* Compile-only route integration: never consume a canary or grant hardware access. */
    SofLogBreadcrumb("[Safety] DMIC enumeration candidate is compile-only; refusing driver load.\n");
    return STATUS_NOT_SUPPORTED;
#else

    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    char msg[256];
    OBJECT_ATTRIBUTES oa;
    HANDLE serviceKey = NULL;
    HANDLE paramsKey = NULL;
    UNICODE_STRING paramsSubkey;
    NTSTATUS regStatus;
    ULONG bootSafetyToken = 0;
    BOOLEAN canaryFilePresent = FALSE;

    SofLogBreadcrumb("[3] KMDF DriverEntry entered from FxDriverEntry!\n");

    /*
     * Bulletproof 3-Tier Safety Canary:
     * Check SafeBootPolicy and atomically consume single-shot token/file in DriverEntry
     * BEFORE any device or filter stack is initialized.
     */
    InitializeObjectAttributes(&oa, RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    regStatus = ZwOpenKey(&serviceKey, KEY_READ | KEY_WRITE, &oa);
    if (NT_SUCCESS(regStatus) && serviceKey != NULL) {
        RtlInitUnicodeString(&paramsSubkey, L"Parameters");
        InitializeObjectAttributes(&oa, &paramsSubkey, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, serviceKey, NULL);
        regStatus = ZwOpenKey(&paramsKey, KEY_READ | KEY_WRITE, &oa);
        ZwClose(serviceKey);
    }

#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC
    ULONG requestedPolicy = SOF_SAFE_BOOT_FORCED_SAFE;
    SofLogBreadcrumb("[Safety] Pujjo resource diagnostic v0.0.8.0\n");
#ifdef PUJJO_DMIC_DMA_PREFLIGHT
    SofLogBreadcrumb("[Safety] Pujjo firmware/DMA admission v0.0.9.2; recording locked.\n");
#endif
#ifdef PUJJO_DMIC_DETECTION_TEST
    SofLogBreadcrumb("[Safety] Pujjo microphone detection test v0.0.8.1; recording disabled.\n");
#endif
    if (!NT_SUCCESS(regStatus)) {
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[Safety] Open diagnostic parameters failed: 0x%08x\n", (ULONG)regStatus);
        SofLogBreadcrumb(msg);
        paramsKey = NULL;
    }
    if (paramsKey != NULL) {
        requestedPolicy = SofReadRegistryDword(paramsKey, L"SafeBootPolicy", SOF_SAFE_BOOT_FORCED_SAFE);
        bootSafetyToken = SofReadRegistryDword(paramsKey, L"BootSafetyToken", 0);
    }
    canaryFilePresent = SofCheckAndConsumeCanaryFile(
        requestedPolicy != SOF_SAFE_BOOT_FORCED_SAFE || bootSafetyToken != 0);

    /* No production/watchdog authorization exists in the diagnostic binary.
     * Only a durable disarm of a valid one-shot request can publish the permit. */
    g_CanaryArmed = SofCommitDiagnosticDisarm(paramsKey, requestedPolicy, bootSafetyToken, canaryFilePresent);
    if (g_CanaryArmed) {
        g_SafeBootPolicy = SOF_SAFE_BOOT_SINGLE_SHOT;
        InterlockedExchange(&g_DiagnosticPreparePermit, 1);
        SofLogBreadcrumb("[Safety] Diagnostic canary consumed and persistent disarm verified; one resource probe authorized.\n");
    } else {
        g_SafeBootPolicy = SOF_SAFE_BOOT_FORCED_SAFE;
        SofLogBreadcrumb("[Safety] SafeBootPolicy = FORCED_SAFE (0). Audio stack disabled.\n");
    }
#else
    ULONG crashCount = 0;
    ULONG crashMax = 1;

    if (paramsKey != NULL) {
        g_SafeBootPolicy = SofReadRegistryDword(paramsKey, L"SafeBootPolicy", SOF_SAFE_BOOT_PRODUCTION);
        bootSafetyToken = SofReadRegistryDword(paramsKey, L"BootSafetyToken", 0);
        crashCount = SofReadRegistryDword(paramsKey, L"ConsecutiveCrashCount", 0);
        crashMax = SofReadRegistryDword(paramsKey, L"CrashWatchdogMax", 1);
        if (crashMax == 0) crashMax = 1;
    }

    canaryFilePresent = SofCheckAndConsumeCanaryFile(FALSE);

    if (g_SafeBootPolicy == SOF_SAFE_BOOT_FORCED_SAFE) {
        g_CanaryArmed = FALSE;
        SofLogBreadcrumb("[Safety] SafeBootPolicy = FORCED_SAFE (0). Audio stack disabled.\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Safety: Forced safe mode active (SafeBootPolicy=0). Audio bypassed.\n");
    } else if (g_SafeBootPolicy == SOF_SAFE_BOOT_SINGLE_SHOT) {
        /* SOF_SAFE_BOOT_SINGLE_SHOT: Requires BOTH token==1 AND \SystemRoot\sof_canary.bin */
        if (bootSafetyToken == 1 && canaryFilePresent && paramsKey != NULL &&
            NT_SUCCESS(SofWriteRegistryDword(paramsKey, L"BootSafetyToken", 0)) &&
            NT_SUCCESS(ZwFlushKey(paramsKey))) {
            /* ATOMIC DISARM: Immediately clear token in registry and flush to guarantee safe next boot */
            g_CanaryArmed = TRUE;
            SofLogBreadcrumb("[Safety] Canary file consumed & token reset to 0 in DriverEntry! Armed for single boot.\n");
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] Safety Canary: Consumed & reset to 0 in DriverEntry. Armed for single boot.\n");
        } else {
            g_CanaryArmed = FALSE;
            char disMsg[160];
            RtlStringCbPrintfA(disMsg, sizeof(disMsg),
                "[Safety] Canary not armed (Token=%u, File=%u)! Safe Mode active (zero BSOD).\n",
                bootSafetyToken, canaryFilePresent ? 1 : 0);
            SofLogBreadcrumb(disMsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, "%s", disMsg);
        }
    } else {
        /* SOF_SAFE_BOOT_PRODUCTION (2): Automatic Crash Watchdog Guard */
        if (crashCount >= crashMax) {
            /* PREVIOUS BOOT CRASHED BEFORE CONFIRMING 10s HEALTHY D0 UPTIME! */
            g_CanaryArmed = FALSE;
            char wmsg[160];
            RtlStringCbPrintfA(wmsg, sizeof(wmsg),
                "[Watchdog] Consecutive crash count (%u) >= max (%u)! FORCING SAFE MODE (zero BSOD).\n",
                crashCount, crashMax);
            SofLogBreadcrumb(wmsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", wmsg);
        } else {
            /* Boot in progress: increment counter and flush immediately before device init */
            crashCount++;
            if (paramsKey != NULL) {
                SofWriteRegistryDword(paramsKey, L"ConsecutiveCrashCount", crashCount);
                ZwFlushKey(paramsKey);
            }
            g_CanaryArmed = TRUE;
            char wmsg[160];
            RtlStringCbPrintfA(wmsg, sizeof(wmsg),
                "[Watchdog] Boot in progress: ConsecutiveCrashCount=%u (max=%u). Audio stack ARMED.\n",
                crashCount, crashMax);
            SofLogBreadcrumb(wmsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", wmsg);
        }
    }

#endif
    if (paramsKey != NULL) {
        ZwClose(paramsKey);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "========================================================\n"
               " Sound Open Firmware (SOF) Windows DSP Driver Loading...\n"
               " Version: 1.0.0 (ABI 3.23.1)\n"
               "========================================================\n");

    WDF_DRIVER_CONFIG_INIT(&config, SofEvtDeviceAdd);
    config.DriverPoolTag = SOF_DEVICE_TAG;

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE
    );

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[4] WdfDriverCreate returned 0x%08x\n", status);
    SofLogBreadcrumb(msg);

    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] WdfDriverCreate failed with status 0x%08x\n", status);
        return status;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] DriverEntry succeeded.\n");
    return STATUS_SUCCESS;
#endif /* PUJJO_DMIC_ENUMERATION_CANDIDATE */
}

/**
 * @brief Inspects PnP Hardware ID property to verify this is a supported PCI audio DSP.
 *
 * @param[in] DeviceInit Pointer to device initialization structure.
 * @return BOOLEAN TRUE if hardware ID contains PCI\\VEN_8086, FALSE otherwise.
 */
static BOOLEAN
SofIsPciDspDevice(
    _In_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;
    WDFMEMORY memory = NULL;
    PWCHAR pBuffer;
    BOOLEAN isPci = FALSE;

    status = WdfFdoInitAllocAndQueryProperty(
        DeviceInit,
        DevicePropertyHardwareID,
        NonPagedPoolNx,
        WDF_NO_OBJECT_ATTRIBUTES,
        &memory
    );
    if (NT_SUCCESS(status) && memory != NULL) {
        pBuffer = (PWCHAR)WdfMemoryGetBuffer(memory, NULL);
        if (pBuffer != NULL) {
            PWCHAR pCurrent = pBuffer;
            while (*pCurrent != L'\0') {
#if defined(PUJJO_DSP_BOOT_DIAGNOSTIC) || defined(PUJJO_DMIC_ENUMERATION_CANDIDATE)
                if (_wcsicmp(pCurrent, L"PCI\\VEN_8086&DEV_54C8&SUBSYS_00000000") == 0) {
#else
                if (wcsstr(pCurrent, L"PCI\\VEN_8086&DEV_E428") != NULL ||
                    wcsstr(pCurrent, L"PCI\\VEN_8086&DEV_4F80") != NULL ||
                    wcsstr(pCurrent, L"PCI\\VEN_8086") != NULL) {
#endif
                    isPci = TRUE;
                    break;
                }
                pCurrent += wcslen(pCurrent) + 1;
            }
        }
        WdfObjectDelete(memory);
        memory = NULL;
    }

    return isPci;
}

/**
 * @brief KMDF EvtDeviceAdd callback.
 *
 * Handles PnP device addition, registers PnP/Power callbacks, sets up WDM IRP
 * preprocessing for PortCls KS audio interfaces, allocates the SOF_DEVICE_CONTEXT,
 * creates the WDFINTERRUPT and WDFDMAENABLER objects, and configures runtime S0-Idle power management.
 *
 * @param[in] Driver Handle to WDFDRIVER object.
 * @param[in,out] DeviceInit Pointer to device initialization structure.
 * @return NTSTATUS STATUS_SUCCESS or error code.
 */
NTSTATUS
SofEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE device;
    PSOF_DEVICE_CONTEXT deviceContext;
    char msg[128];

    UNREFERENCED_PARAMETER(Driver);

    SofLogBreadcrumb("[5] SofEvtDeviceAdd entered!\n");

#ifndef PUJJO_DSP_BOOT_DIAGNOSTIC
#ifndef PUJJO_DMIC_ENUMERATION_CANDIDATE
    /* Check if this is a filter device (e.g. LowerFilter on SDCA Audio Function) */
    if (SofIsSdcaFunctionDevice(DeviceInit)) {
        if (!g_CanaryArmed) {
            SofLogBreadcrumb("[5-Filter] Safe Mode active: Canary not armed -> refusing SDCA filter attach.\n");
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                       "[SOF] Safe Mode: Refusing SDCA lower filter attach (Canary not armed).\n");
            return STATUS_NOT_SUPPORTED;
        }
        SofLogBreadcrumb("[5-Filter] Canary Armed: SDCA Audio Function device detected! Attaching lower filter.\n");
        return SofSdcaFilterDeviceAdd(Driver, DeviceInit);
    }
#endif

#endif
    /* Reject any device that is not our PCI DSP */
    if (!SofIsPciDspDevice(DeviceInit)) {
        SofLogBreadcrumb("[5-Reject] Unrecognized non-PCI device in SofEvtDeviceAdd -> STATUS_NOT_SUPPORTED\n");
        return STATUS_NOT_SUPPORTED;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] SofEvtDeviceAdd: Found matching SOF DSP device!\n");

#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC
    /*
     * Forced-safe PnP-only diagnostic. Do not register ANY driver PnP/power
     * callbacks or allocate a device context, DMA enabler, interrupt, timer,
     * PortCls endpoint or mapped PCI BAR. KMDF owns the default PnP/power
     * transitions, so Windows can report the FDO as started without ever
     * executing PrepareHardware or D0Entry. There is NO DSP boot and no audio.
     *
     * This cannot be reached by SINGLE_SHOT/canary-armed configurations.
     * The guarded hardware startup implementation below stays unchanged.
     */
    if (g_SafeBootPolicy == SOF_SAFE_BOOT_FORCED_SAFE && !g_CanaryArmed) {
        SofLogBreadcrumb("[5-SafeStub] Forced-safe KMDF-only device creation; no DSP callbacks.\n");
        status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
        if (!NT_SUCCESS(status)) {
            SofLogBreadcrumb("[5-SafeStub] WdfDeviceCreate failed.\n");
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                       "[PUJJO] Forced-safe KMDF-only WdfDeviceCreate failed: 0x%08x\n",
                       status);
        } else {
            SofLogBreadcrumb("[5-SafeStub] KMDF device created without DMA, IRQ or firmware boot.\n");
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[PUJJO] Forced-safe KMDF-only device started; DSP not accessed.\n");
        }
        return status;
    }
#endif

    /* Register PnP and Power Event Callbacks */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = SofEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = SofEvtDeviceReleaseHardware;
#ifndef PUJJO_DSP_BOOT_DIAGNOSTIC
    pnpPowerCallbacks.EvtDeviceSurpriseRemoval = SofEvtDeviceSurpriseRemoval;
    /* FW_READY and stream shutdown require live WDF IRQ delivery. */
    pnpPowerCallbacks.EvtDeviceD0EntryPostInterruptsEnabled = SofEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0ExitPreInterruptsDisabled  = SofEvtDeviceD0Exit;
#endif

#ifdef PUJJO_DMIC_DETECTION_TEST
    pnpPowerCallbacks.EvtDeviceSurpriseRemoval = SofEvtDetectionSurpriseRemoval;
#endif
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    /* Allocate Device Context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, SOF_DEVICE_CONTEXT);
    deviceAttributes.SynchronizationScope = WdfSynchronizationScopeNone;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[5a] WdfDeviceCreate failed!\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] WdfDeviceCreate failed with status 0x%08x\n", status);
        return status;
    }

    SofLogBreadcrumb("[6] WdfDeviceCreate succeeded!\n");

    deviceContext = GetDeviceContext(device);
    RtlZeroMemory(deviceContext, sizeof(SOF_DEVICE_CONTEXT));
    deviceContext->WdfDevice = device;
    KeInitializeMutex(&deviceContext->AudioMutex, 0);
    KeInitializeSpinLock(&deviceContext->AudioPositionLock);
    deviceContext->CurrentPowerState = PowerDeviceD3;

#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC
    /* Read-only one-shot resource discovery only. DMA common buffers are
     * device-owned; retaining local handles cannot guarantee their lifetime
     * after a failed start or removal. No interrupt or DMA owner is created. */
    SofLogBreadcrumb("[Safety] DMA lifetime unqualified; read-only resource probe only.\n");
    return STATUS_SUCCESS;
#else
    /* Create KMDF DMA Enabler for IOMMU / VT-d Common Buffer mapping & Bus Mastering */
    {
        WDF_DMA_ENABLER_CONFIG dmaConfig;
        WdfDeviceSetAlignmentRequirement(device, FILE_128_BYTE_ALIGNMENT);
        WDF_DMA_ENABLER_CONFIG_INIT(&dmaConfig, WdfDmaProfileScatterGather64, 16 * 1024 * 1024);
        dmaConfig.WdmDmaVersionOverride = 3;
        status = WdfDmaEnablerCreate(device, &dmaConfig, WDF_NO_OBJECT_ATTRIBUTES, &deviceContext->DmaEnabler);
        RtlStringCbPrintfA(msg, sizeof(msg), "[6a] WdfDmaEnablerCreate: status=0x%08x (Enabler=%p)\n",
                           status, deviceContext->DmaEnabler);
        SofLogBreadcrumb(msg);
        if (!NT_SUCCESS(status)) {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                       "[SOF] WdfDmaEnablerCreate failed with status 0x%08x\n", status);
        } else {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] WdfDmaEnabler created successfully for IOMMU mapping.\n");
        }
    }

    /* Initialize HDA Verb SpinLock */
    KeInitializeSpinLock(&deviceContext->HdaVerbLock);

    /* Create Watchdog Health Timer (one-shot, runs at PASSIVE_LEVEL to reset ConsecutiveCrashCount) */
    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttributes;

        WDF_TIMER_CONFIG_INIT(&timerConfig, SofEvtWatchdogHealthTimer);
        timerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
        timerAttributes.ParentObject = device;
        timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;

        status = WdfTimerCreate(&timerConfig, &timerAttributes, &deviceContext->WatchdogHealthTimer);
        if (!NT_SUCCESS(status)) {
            char tmsg[128];
            RtlStringCbPrintfA(tmsg, sizeof(tmsg), "[Watchdog] Failed to create WatchdogHealthTimer: 0x%08X\n", status);
            SofLogBreadcrumb(tmsg);
            deviceContext->WatchdogHealthTimer = NULL;
        } else {
            SofLogBreadcrumb("[Watchdog] WatchdogHealthTimer created successfully\n");
        }
    }

    /*
     * Configure KMDF S0 Idle Settings (Runtime Power Management / D0i3 Idle Gating).
     * By default, enabled with 5000ms idle timeout. Configurable via registry:
     * EnableS0Idle (DWORD, 0 or 1, default 1)
     * S0IdleTimeoutMs (DWORD, ms, default 5000)
     */
    {
        WDFKEY paramKey = NULL;
        ULONG enableS0Idle = 0;
        ULONG idleTimeoutMs = 5000;
        UNICODE_STRING valEnable, valTimeout;

        RtlInitUnicodeString(&valEnable, L"EnableS0Idle");
        RtlInitUnicodeString(&valTimeout, L"S0IdleTimeoutMs");

        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &paramKey))) {
            WdfRegistryQueryULong(paramKey, &valEnable, &enableS0Idle);
            WdfRegistryQueryULong(paramKey, &valTimeout, &idleTimeoutMs);
            WdfRegistryClose(paramKey);
        }

        deviceContext->S0IdleTimeoutMs = (idleTimeoutMs >= 500) ? idleTimeoutMs : 5000;

        if (enableS0Idle) {
            WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idleSettings;
            WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idleSettings, IdleCannotWakeFromS0);
            idleSettings.IdleTimeout = deviceContext->S0IdleTimeoutMs;
            idleSettings.IdleTimeoutType = DriverManagedIdleTimeout;
            idleSettings.UserControlOfIdleSettings = IdleDoNotAllowUserControl;
            idleSettings.Enabled = WdfTrue;
            idleSettings.DxState = PowerDeviceD3;

            status = WdfDeviceAssignS0IdleSettings(device, &idleSettings);
            if (NT_SUCCESS(status)) {
                deviceContext->S0IdleEnabled = TRUE;
                RtlStringCbPrintfA(msg, sizeof(msg),
                    "[Power] S0 Idle configured: Timeout=%ums, DxState=D3 (status=0x%08X)\n",
                    idleSettings.IdleTimeout, status);
                SofLogBreadcrumb(msg);
                DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", msg);
            } else {
                RtlStringCbPrintfA(msg, sizeof(msg),
                    "[Power] WdfDeviceAssignS0IdleSettings failed: 0x%08X\n", status);
                SofLogBreadcrumb(msg);
            }
        } else {
            deviceContext->S0IdleEnabled = FALSE;
            SofLogBreadcrumb("[Power] S0 Idle disabled via registry\n");
        }
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Device successfully created and initialized.\n");
    return STATUS_SUCCESS;
#endif
}

/**
 * @brief KMDF EvtDriverUnload callback.
 *
 * Invoked during driver teardown to log final status.
 *
 * @param[in] Driver Handle to WDFDRIVER object.
 */
VOID
SofEvtDriverUnload(
    _In_ WDFDRIVER Driver
)
{
    UNREFERENCED_PARAMETER(Driver);
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Driver unloaded.\n");
}
