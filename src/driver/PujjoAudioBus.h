/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <ntddk.h>
#include <guiddef.h>

#define PUJJO_AUDIO_CHILD_ID L"PUJJO\\AudioDmic"
#define PUJJO_AUDIO_INTERFACE_VERSION 3
#if defined(PUJJO_ENABLE_EXPERIMENTAL_ANALOG) && !defined(PUJJO_ENABLE_EXPERIMENTAL_AUDIO)
#error The analog path requires the experimental audio guard
#endif

DEFINE_GUID(GUID_PUJJO_AUDIO_BUS, 0xd18a381b, 0xaa4b, 0x4565,
            0x99, 0x89, 0x85, 0x9a, 0xdf, 0x80, 0x01, 0xd3);

typedef enum {
    PujjoAudioDmic = 0,
    PujjoAudioHeadphone,
    PujjoAudioHeadsetMic,
    PujjoAudioSpeaker
} PUJJO_AUDIO_ROLE;

typedef struct _PUJJO_AUDIO_FORMAT {
    ULONG Rate;
    USHORT Channels;
    USHORT ContainerBits;
    USHORT ValidBits;
    PUJJO_AUDIO_ROLE Role;
} PUJJO_AUDIO_FORMAT;

typedef struct _PUJJO_AUDIO_BUFFER {
    PVOID Stream;
    PMDL Mdl;
    ULONG Bytes;
    MEMORY_CACHING_TYPE CacheType;
} PUJJO_AUDIO_BUFFER;

/* Context is referenced through the PDO query interface. A successful
 * Allocate transfers one buffer lease to the FDO; Free consumes it.
 * Only Position and Notification are callable at DISPATCH_LEVEL. */
typedef struct _PUJJO_AUDIO_BUS {
    INTERFACE Header;
    NTSTATUS (*GetFormat)(PVOID Context, PUJJO_AUDIO_FORMAT *Format);
    NTSTATUS (*Allocate)(PVOID Context, ULONG RequestedBytes, PUJJO_AUDIO_BUFFER *Buffer);
    VOID (*Free)(PVOID Context, PVOID Stream);
    NTSTATUS (*SetState)(PVOID Context, PVOID Stream, ULONG State);
    NTSTATUS (*Position)(PVOID Context, PVOID Stream, PULONG Position);
    NTSTATUS (*Notification)(PVOID Stream, PKEVENT Event, BOOLEAN Add);
    NTSTATUS (*JackPresent)(PVOID Context, PBOOLEAN Present);
} PUJJO_AUDIO_BUS;

#ifdef __cplusplus
extern "C" {
#endif
#ifdef _KERNEL_MODE
#ifndef PUJJO_AUDIO_PORTCLS
struct _SOF_DEVICE_CONTEXT;
NTSTATUS SofPublishAudioChild(struct _SOF_DEVICE_CONTEXT *Ctx);
NTSTATUS SofDisconnectAudioChild(struct _SOF_DEVICE_CONTEXT *Ctx);
#endif
#endif
#ifdef __cplusplus
}
#endif
