/* SPDX-License-Identifier: BSD-3-Clause */
#include "wdk_host_mock.h"
#include "../src/driver/peripheral/PujjoRt5682sSpbWindows.c"
#include "../src/driver/peripheral/PujjoRt1019pGpioWindows.c"
#ifdef PJ_HOST_THREADS
static void *read_worker(void *ctx) {
 uint8_t tx[2]={0,0xff},rx[2];
 assert(Pj5682sSpbRead(ctx,tx,2,rx,2)==0);return NULL;
}
static void *close_worker(void *ctx) {Pj5682sSpbClose(ctx);return NULL;}
#endif
int main(void) {
 PJ5682S_SPB_WINDOWS spb={0};PJ1019_GPIO_WINDOWS gpio={0};pj5682s_bus bus;
 uint8_t tx[4]={0,0xff},rx[2];int owner;unsigned before;
 assert(Pj5682sSpbOpen(&owner,1,0,&spb)==STATUS_INVALID_PARAMETER);
 assert(Pj5682sSpbInitialize(&owner,&spb)==0);
 assert(Pj5682sSpbInitialize(&owner,&spb)==STATUS_INVALID_PARAMETER);
 assert(Pj5682sSpbSetPower(&spb,TRUE)==STATUS_INVALID_DEVICE_STATE);
 fail_create=1;assert(Pj5682sSpbOpen(&owner,1,0,&spb)==-10);assert(!live_targets);
 fail_create=0;fail_open=1;assert(Pj5682sSpbOpen(&owner,1,0,&spb)==-11);assert(!live_targets);
 fail_open=0;assert(Pj5682sSpbOpen(&owner,1,0,&spb)==0);
 assert(Pj5682sSpbOpen(&owner,1,0,&spb)==STATUS_INVALID_DEVICE_STATE);
 Pj5682sSpbBindCallbacks(&spb,&bus);assert(bus.write_then_read && !bus.write_only);
 before=sends;assert(bus.write_then_read(bus.context,tx,2,rx,2)==-1);assert(sends==before);
 assert(Pj5682sSpbSetPower(&spb,TRUE)==0);
 for(transferred=0;transferred<6;++transferred) {
  assert((bus.write_then_read(bus.context,tx,2,rx,2)==0)==(transferred==4));
 }
 transferred=4;
 before=sends;
 for(unsigned reg=0;reg<65536u;++reg) {
  tx[0]=(uint8_t)(reg>>8);tx[1]=(uint8_t)reg;
  int allowed=(reg==0xfd || reg==0xfe || reg==0xff);
  assert((bus.write_then_read(bus.context,tx,2,rx,2)==0)==allowed);
 }
 assert(sends==before+3);tx[0]=0;tx[1]=0xff;
 fail_send=1;assert(bus.write_then_read(bus.context,tx,2,rx,2)==-1);fail_send=0;
 for(unsigned i=0;i<1000;++i) {
  assert(Pj5682sSpbSetPower(&spb,FALSE)==0);
  before=sends;assert(bus.write_then_read(bus.context,tx,2,rx,2)==-1);assert(before==sends);
  assert(Pj5682sSpbSetPower(&spb,TRUE)==0);assert(bus.write_then_read(bus.context,tx,2,rx,2)==0);
 }
#ifdef PJ_HOST_THREADS
 {
  pthread_t reader,closer;
  block_send=1;assert(!pthread_create(&reader,NULL,read_worker,&spb));
  assert(!pthread_mutex_lock(&rendezvous));
  while(!entered)assert(!pthread_cond_wait(&condition,&rendezvous));
  assert(!pthread_create(&closer,NULL,close_worker,&spb));
  release_send=1;assert(!pthread_cond_broadcast(&condition));
  assert(!pthread_mutex_unlock(&rendezvous));
  assert(!pthread_join(reader,NULL));assert(!pthread_join(closer,NULL));block_send=0;
  assert(!spb.Opened && !spb.Target && !live_targets);
 }
#else
 Pj5682sSpbClose(&spb);
 puts("SKIP: POSIX concurrent close test unavailable on this host");
#endif
 before=sends;assert(bus.write_then_read(bus.context,tx,2,rx,2)==-1);assert(sends==before);
 Pj5682sSpbClose(&spb); /* idempotent, persistent lock for reprepare */
 assert(Pj5682sSpbOpen(&owner,1,0,&spb)==0 && !spb.Powered);Pj5682sSpbClose(&spb);
 assert(Pj1019GpioInitialize(&owner,&gpio)==0);
 assert(Pj1019GpioOpen(&owner,2,0,&gpio)==0);
 before=sends;assert(Pj1019GpioWrite(&gpio,TRUE)==STATUS_INVALID_DEVICE_STATE);assert(sends==before);
 assert(Pj1019GpioSetPower(&gpio,TRUE)==0);assert(Pj1019GpioWrite(&gpio,FALSE)==0);
 fail_send=1;assert(Pj1019GpioWrite(&gpio,TRUE)==-12);fail_send=0;
 assert(Pj1019GpioSetPower(&gpio,FALSE)==0);assert(Pj1019GpioWrite(&gpio,TRUE)==STATUS_INVALID_DEVICE_STATE);
 Pj1019GpioClose(&gpio);Pj1019GpioClose(&gpio);assert(!live_targets);
 destroy_lock(spb.Lock);destroy_lock(gpio.Lock);
 puts("PASS: full Windows transport sources with host WDF doubles, lifecycle/failures and close serialization");
 return 0;
}
