#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compile the actual diagnostic persistent-disarm gate against fault-injected registry doubles."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
driver = (root / "src/driver/Driver.c").read_text()
helper = driver.split("static BOOLEAN\nSofCommitDiagnosticDisarm(", 1)[1].split("\n#endif", 1)[0]
assert "TokenWrite=0x%08x PolicyWrite=0x%08x Flush=0x%08x" in helper
assert "ReadbackToken=0x%08x ReadbackPolicy=0x%08x Permit=%u" in helper
assert 'tests/test_diagnostic_disarm.py' in (root / 'tests/run_offline_suite.py').read_text()
fixture = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef uint32_t ULONG;
typedef int32_t NTSTATUS;
typedef int BOOLEAN;
typedef void *HANDLE;
#define _In_
#define _In_opt_
#define TRUE 1
#define FALSE 0
#define MAXULONG UINT32_MAX
#define STATUS_SUCCESS 0
#define SOF_SAFE_BOOT_FORCED_SAFE 0
#define SOF_SAFE_BOOT_SINGLE_SHOT 1
#define RtlStringCbPrintfA snprintf
#define DbgPrintEx(...) ((void)0)
static ULONG mock_token, mock_policy;
static NTSTATUS status_token, status_policy, status_flush;
static int bad_token, bad_policy, step;
static char log_message[256];
static int key;
static void SofLogBreadcrumb(const char *message) {
    assert(strlen(message) < sizeof(log_message));
    strcpy(log_message, message);
}
static NTSTATUS SofWriteRegistryDword(HANDLE h, const wchar_t *name, ULONG value) {
    assert(h == &key && value == 0);
    if (wcscmp(name, L"BootSafetyToken") == 0) {
        assert(++step == 1);
        if (!status_token) mock_token = value;
        return status_token;
    }
    assert(wcscmp(name, L"SafeBootPolicy") == 0 && ++step == 2);
    if (!status_policy) mock_policy = value;
    return status_policy;
}
static NTSTATUS ZwFlushKey(HANDLE h) {
    assert(h == &key && ++step == 3);
    return status_flush;
}
static ULONG SofReadRegistryDword(HANDLE h, const wchar_t *name, ULONG default_value) {
    assert(h == &key && default_value == MAXULONG);
    if (wcscmp(name, L"BootSafetyToken") == 0) {
        assert(++step == 4);
        return bad_token == 1 ? MAXULONG : bad_token == 2 ? 1u : mock_token;
    }
    assert(wcscmp(name, L"SafeBootPolicy") == 0 && ++step == 5);
    return bad_policy == 1 ? MAXULONG : bad_policy == 2 ? 1u : mock_policy;
}
"""
cases = r"""
int main(void) {
    const NTSTATUS statuses[] = {0, (NTSTATUS)0xc0000001u, 1};
    unsigned count = 0;
    for (ULONG policy = 0; policy < 3; ++policy)
      for (ULONG token = 0; token < 3; ++token)
        for (int file = 0; file < 2; ++file)
          for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
              for (int c = 0; c < 3; ++c)
                for (bad_token = 0; bad_token < 3; ++bad_token)
                  for (bad_policy = 0; bad_policy < 3; ++bad_policy) {
                    mock_token = token; mock_policy = policy;
                    status_token = statuses[a]; status_policy = statuses[b]; status_flush = statuses[c];
                    step = 0; log_message[0] = 0;
                    BOOLEAN granted = SofCommitDiagnosticDisarm(&key, policy, token, file);
                    int idle = !policy && !token && !file;
                    assert(step == (idle ? 0 : 5));
                    assert(granted == (policy == 1 && token == 1 && file && !a && !b && !c && !bad_token && !bad_policy));
                    if (!idle) {
                        assert(strstr(log_message, "TokenWrite=0x") && strstr(log_message, "PolicyWrite=0x") &&
                               strstr(log_message, "Flush=0x") && strstr(log_message, granted ? "Permit=1" : "Permit=0"));
                        if (!a) assert(mock_token == 0);
                        if (!b) assert(mock_policy == 0);
                    }
                    ++count;
                  }
    step = 0;
    assert(!SofCommitDiagnosticDisarm(NULL, 1, 1, TRUE) && step == 0);
    printf("PASS: %u fault combinations, including failed/positive-status flush and mismatched readback; missing parameters denied\n", count);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="pj_disarm_") as tmp:
    src = Path(tmp) / "disarm.c"
    exe = Path(tmp) / "disarm"
    src.write_text(fixture + "\nstatic BOOLEAN\nSofCommitDiagnosticDisarm(" + helper + cases)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")), str(src), "-o", str(exe)], check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=10)

# Execute the actual DWORD reader with malformed lengths/types and errors.
reader = driver.split("static ULONG\nSofReadRegistryDword(", 1)[1].split("\n/**", 1)[0]
query_fixture = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef uint32_t ULONG;
typedef uint8_t UCHAR;
typedef int32_t NTSTATUS;
typedef const wchar_t *PCWSTR;
typedef void *HANDLE;
typedef struct { PCWSTR name; } UNICODE_STRING;
typedef struct {
    ULONG TitleIndex, Type, DataLength;
    UCHAR Data[1];
} KEY_VALUE_PARTIAL_INFORMATION, *PKEY_VALUE_PARTIAL_INFORMATION;
#define _In_
#define STATUS_SUCCESS 0
#define REG_DWORD 4
#define KeyValuePartialInformation 2
#define FIELD_OFFSET(t, f) offsetof(t, f)
#define RtlCopyMemory memcpy
static ULONG type, data_length, returned_length, query_value;
static NTSTATUS query_status;
static void RtlInitUnicodeString(UNICODE_STRING *u, PCWSTR name) { u->name = name; }
static NTSTATUS ZwQueryValueKey(HANDLE handle, UNICODE_STRING *u, int kind,
                                void *buffer, ULONG length, ULONG *result) {
    assert(handle && u->name && kind == KeyValuePartialInformation);
    assert(length >= offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG));
    *result = returned_length;
    if (query_status == STATUS_SUCCESS) {
        PKEY_VALUE_PARTIAL_INFORMATION info = buffer;
        info->TitleIndex = 0;
        info->Type = type;
        info->DataLength = data_length;
        memcpy(info->Data, &query_value, sizeof(query_value));
    }
    return query_status;
}
"""
query_cases = r"""
int main(void) {
    int key;
    unsigned count = 0;
    query_value = 1;
    for (int a = 0; a < 3; ++a)
      for (type = 3; type < 6; ++type)
        for (data_length = 0; data_length < 10; ++data_length)
          for (returned_length = 0; returned_length < 40; ++returned_length) {
            query_status = a == 0 ? 0 : a == 1 ? -1 : 1;
            ULONG expected = a == 0 && type == REG_DWORD && data_length == sizeof(ULONG) &&
                returned_length >= offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG) &&
                returned_length <= sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)
                    ? query_value : UINT32_MAX;
            assert(SofReadRegistryDword(&key, L"SafeBootPolicy", UINT32_MAX) == expected);
            ++count;
          }
    printf("PASS: %u registry DWORD query/type/length combinations\n", count);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="pj_dword_") as tmp:
    src = Path(tmp) / "dword.c"
    exe = Path(tmp) / "dword"
    src.write_text(query_fixture + "\nstatic ULONG\nSofReadRegistryDword(" + reader + query_cases)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")), str(src), "-o", str(exe)], check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=10)
