/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PJ_WDK_HOST_MOCK_H
#define PJ_WDK_HOST_MOCK_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#ifdef PJ_HOST_THREADS
#include <pthread.h>
#endif
typedef int NTSTATUS;
typedef uint32_t ULONG;
typedef uintptr_t ULONG_PTR;
typedef unsigned char UCHAR;
typedef int BOOLEAN;
typedef void *PVOID;
typedef void *WDFDEVICE;
typedef struct target {int open;} *WDFIOTARGET;
typedef struct wait_lock {
#ifdef PJ_HOST_THREADS
 pthread_mutex_t mutex;
#else
 int depth;
#endif
} *WDFWAITLOCK;
typedef struct {void *ParentObject;} WDF_OBJECT_ATTRIBUTES;
typedef struct {int access;} WDF_IO_TARGET_OPEN_PARAMS;
typedef struct {void *buffer;size_t size;} WDF_MEMORY_DESCRIPTOR;
typedef struct {long long Timeout;} WDF_REQUEST_SEND_OPTIONS;
typedef struct {int direction;void *buffer;ULONG length;} SPB_TRANSFER_LIST_ENTRY;
#define _IRQL_requires_(x)
#define PASSIVE_LEVEL 0
#define PAGED_CODE() ((void)0)
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_INVALID_DEVICE_STATE -2
#define NT_SUCCESS(x) ((x)>=0)
#define WDF_NO_HANDLE NULL
#define WDF_NO_OBJECT_ATTRIBUTES NULL
#define RESOURCE_HUB_PATH_SIZE 128
#define DECLARE_UNICODE_STRING_SIZE(name,n) int name=0
#define GENERIC_READ 1
#define GENERIC_WRITE 2
#define SpbTransferDirectionToDevice 1
#define SpbTransferDirectionFromDevice 2
#define IOCTL_SPB_EXECUTE_SEQUENCE 1
#define IOCTL_GPIO_WRITE_PINS 2
#define WDF_REQUEST_SEND_OPTION_TIMEOUT 1
#define WDF_REL_TIMEOUT_IN_MS(x) (-(long long)(x)*10000)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define WDF_OBJECT_ATTRIBUTES_INIT(p) memset(p,0,sizeof(*(p)))
#define WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(p,b,n) ((p)->buffer=(b),(p)->size=(n))
#define WDF_REQUEST_SEND_OPTIONS_INIT(p,f) ((p)->Timeout=0)
#define WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(p,path,a) ((void)(path),(p)->access=(a))
#define SPB_TRANSFER_LIST_AND_ENTRIES(n) struct {struct {SPB_TRANSFER_LIST_ENTRY Transfers[n];} List;}
#define SPB_TRANSFER_LIST_INIT(p,n) memset(p,0,sizeof(*(p)))
static SPB_TRANSFER_LIST_ENTRY SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(int direction,int delay,void *buffer,ULONG length) {
 SPB_TRANSFER_LIST_ENTRY e={direction,buffer,length};(void)delay;return e;
}
static int fail_create, fail_open, fail_send;
static ULONG_PTR transferred=4;
static unsigned live_targets, sends, closes;
static NTSTATUS RESOURCE_HUB_CREATE_PATH_FROM_ID(int *path,ULONG low,ULONG high) {
 *path=(int)(low^high);return 0;
}
static NTSTATUS WdfWaitLockCreate(WDF_OBJECT_ATTRIBUTES *a,WDFWAITLOCK *lock) {
 assert(a->ParentObject);*lock=calloc(1,sizeof(**lock));assert(*lock);
#ifdef PJ_HOST_THREADS
 assert(!pthread_mutex_init(&(*lock)->mutex,NULL));
#endif
 return 0;
}
static void WdfWaitLockAcquire(WDFWAITLOCK lock,void *timeout) {
 assert(lock && !timeout);
#ifdef PJ_HOST_THREADS
 assert(!pthread_mutex_lock(&lock->mutex));
#else
 assert(!lock->depth);lock->depth=1;
#endif
}
static void WdfWaitLockRelease(WDFWAITLOCK lock) {
#ifdef PJ_HOST_THREADS
 assert(!pthread_mutex_unlock(&lock->mutex));
#else
 assert(lock->depth==1);lock->depth=0;
#endif
}
static void destroy_lock(WDFWAITLOCK lock) {
#ifdef PJ_HOST_THREADS
 assert(!pthread_mutex_destroy(&lock->mutex));
#endif
 free(lock);
}
static NTSTATUS WdfIoTargetCreate(WDFDEVICE device,void *attributes,WDFIOTARGET *target) {
 (void)attributes;assert(device);if(fail_create)return -10;
 *target=calloc(1,sizeof(**target));assert(*target);++live_targets;return 0;
}
static NTSTATUS WdfIoTargetOpen(WDFIOTARGET target,WDF_IO_TARGET_OPEN_PARAMS *p) {
 assert(target && p->access);if(fail_open)return -11;target->open=1;return 0;
}
static void WdfIoTargetClose(WDFIOTARGET target) {assert(target && target->open);target->open=0;++closes;}
static void WdfObjectDelete(WDFIOTARGET target) {assert(target && !target->open);free(target);--live_targets;}
#ifdef PJ_HOST_THREADS
static pthread_mutex_t rendezvous=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static int block_send, entered, release_send;
#endif
static UCHAR last_gpio_value;
static NTSTATUS WdfIoTargetSendIoctlSynchronously(WDFIOTARGET target,void *request,int code,
 WDF_MEMORY_DESCRIPTOR *input,void *output,WDF_REQUEST_SEND_OPTIONS *opts,ULONG_PTR *bytes) {
 (void)request;(void)output;assert(target && target->open);assert(opts->Timeout==-10000000);
 if(code==IOCTL_SPB_EXECUTE_SEQUENCE) {
  SPB_TRANSFER_LIST_ENTRY *e=input->buffer;
  assert(e[0].direction==SpbTransferDirectionToDevice && e[0].length==2);
  assert(e[1].direction==SpbTransferDirectionFromDevice && e[1].length==2);
  const uint8_t *index=e[0].buffer;
  assert(index[0]==0 && (index[1]==0xfd || index[1]==0xfe || index[1]==0xff));
  /* Model a controller that modifies the output even on short/error returns. */
  ((uint8_t*)e[1].buffer)[0]=0x67;
  ((uint8_t*)e[1].buffer)[1]=0x49;
 } else if(code==IOCTL_GPIO_WRITE_PINS) {
  assert(input->size==1 && *(UCHAR*)input->buffer<=1);
  last_gpio_value=*(UCHAR*)input->buffer;
 }
#ifdef PJ_HOST_THREADS
 assert(!pthread_mutex_lock(&rendezvous));
 if(block_send) {
  entered=1;assert(!pthread_cond_broadcast(&condition));
  while(!release_send) assert(!pthread_cond_wait(&condition,&rendezvous));
 }
 assert(!pthread_mutex_unlock(&rendezvous));
#endif
 assert(target->open);++sends;if(bytes)*bytes=transferred;return fail_send?-12:0;
}
#endif
