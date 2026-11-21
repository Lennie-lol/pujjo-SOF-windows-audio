/* SPDX-License-Identifier: BSD-3-Clause */
#define PJ5682S_ENABLE_CONTROL
#include "wdk_host_mock.h"
#include "../src/driver/experimental/PujjoRt5682sCodec.c"
#include "../src/driver/experimental/PujjoHeadsetClock.c"
#include "../src/driver/peripheral/PujjoRt5682sControl.c"
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
  rx[0]=0xaa;rx[1]=0xbb;
  assert((bus.write_then_read(bus.context,tx,2,rx,2)==0)==(transferred==4));
  assert(rx[0]==(transferred==4?0x67:0xaa));
  assert(rx[1]==(transferred==4?0x49:0xbb));
 }
 transferred=4;
 before=sends;
 for(unsigned reg=0;reg<65536u;++reg) {
  tx[0]=(uint8_t)(reg>>8);tx[1]=(uint8_t)reg;
  int allowed=(reg==0xfd || reg==0xfe || reg==0xff);
  assert((bus.write_then_read(bus.context,tx,2,rx,2)==0)==allowed);
 }
 assert(sends==before+3);tx[0]=0;tx[1]=0xff;
 rx[0]=0xaa;rx[1]=0xbb;
 fail_send=1;assert(bus.write_then_read(bus.context,tx,2,rx,2)==-1);fail_send=0;
 assert(rx[0]==0xaa && rx[1]==0xbb);
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
 /* Runtime transport uses exact-length SPB reads and writes. The
  * diagnostic callbacks exercised above remain identity-only. */
 const pj5682s_control_profile codec_profile={1,1,1,1,24576000,3072000,48000,32,0,PJ5682S_ROUTE_DAC,0,0};
 pj5682s_analog_program codec_program={1,{{0x0063,0xa000,0xa000,15,20}}};
 emulate_codec=1;
 codec_registers[0xff]=PJ5682S_EXPECTED_DEVICE_ID;
 assert(Pj5682sSpbOpen(&owner,1,0,&spb)==0 && Pj5682sSpbSetPower(&spb,TRUE)==0);
 /* D0 does not establish cold calibration: no control bus writes allowed. */
 before=sends;
 assert(!pj5682s_cold_is_ready(&spb.Cold));
 assert(Pj5682sSpbStartControl(&spb,&codec_profile,&codec_program)==STATUS_DEVICE_NOT_READY);
 assert(sends==before);
 /* Host mock ONLY: stand in for an independently qualified cold sequence.
  * Production must never set this phase directly or infer it from D0. */
 spb.Cold.phase=PJ5682S_COLD_READY;
 codec_program.steps[0].mask=1;
 before=sends;
 assert(Pj5682sSpbStartControl(&spb,&codec_profile,&codec_program)==STATUS_ACCESS_DENIED);
 assert(sends==before);
 codec_program.steps[0].mask=0xa000;
 assert(Pj5682sSpbStartControl(&spb,&codec_profile,&codec_program)==0);
 assert(codec_registers[0x63]==0xa000 && delays==1);
 pj5682s_jack jack=PJ5682S_JACK_ABSENT;
 assert(Pj5682sSpbGetJack(&spb,Pj5682sJackPresence,&jack)==0 && jack==PJ5682S_JACK_PRESENT_UNCLASSIFIED);
 codec_registers[0xf0]=0x10;
 assert(Pj5682sSpbGetJack(&spb,Pj5682sJackPresence,&jack)==0 && jack==PJ5682S_JACK_ABSENT);
 assert(Pj5682sSpbSetPower(&spb,FALSE)==0);
 assert(codec_registers[0x63]==0 && codec_registers[0x61]==0 && delays==2);
 assert(Pj5682sSpbGetJack(&spb,Pj5682sJackPresence,&jack)==STATUS_DEVICE_NOT_READY);
 assert(Pj5682sSpbSetPower(&spb,TRUE)==0);
 before=sends;
 assert(!pj5682s_cold_is_ready(&spb.Cold));
 assert(Pj5682sSpbStartControl(&spb,&codec_profile,&codec_program)==STATUS_DEVICE_NOT_READY);
 assert(sends==before);
 /* Host-only model of another fully qualified cold initialization. */
 spb.Cold.phase=PJ5682S_COLD_READY;
 /* Exercise the new clocked path through the actual SPB transport code. */
 pj5682s_control_profile pll_profile=codec_profile;
 pll_profile.mclk_hz=19200000;
 pll_profile.bclk_hz=2400000;
 pll_profile.slot_bits=25;
 before=sends;
 assert(Pj5682sSpbStartControl(&spb,&pll_profile,&codec_program)==STATUS_ACCESS_DENIED);
 assert(sends==before);
 pll_profile.pll2_qualified=1;
 pll_profile.pll2_settle_ms=25;
 const int delays_before=delays;
 assert(Pj5682sSpbStartControl(&spb,&pll_profile,&codec_program)==0);
 assert(delays==delays_before+2);
 assert(codec_registers[0x9b]==0x0203 && codec_registers[0x65]==0x50a0);
 assert(codec_registers[0x80]==0x4000 && codec_registers[0x73]==0x1021);
 assert(Pj5682sSpbStopControl(&spb)==0 && delays==delays_before+3);
 assert(codec_registers[0x9b]==0 && codec_registers[0x65]==0 &&
        codec_registers[0x80]==0 && codec_registers[0x63]==0);
 spb_written_count=3;
 assert(Pj5682sSpbStartControl(&spb,&codec_profile,&codec_program)==STATUS_IO_DEVICE_ERROR);
 assert(spb.Control.faulted);
 spb_written_count=4;
 assert(Pj5682sSpbClose(&spb)==0 && !live_targets);
 emulate_codec=0;
 delays=0;
 assert(Pj1019GpioInitialize(&owner,&gpio)==0);
 assert(Pj1019GpioOpen(&owner,2,0,Pj1019SdbUnverified,&gpio)==STATUS_INVALID_PARAMETER);
 assert(Pj1019GpioOpen(&owner,2,0,Pj1019SdbActiveLow,&gpio)==0);
 const PJ1019_RUNTIME_PROFILE approved={TRUE,TRUE,TRUE,300};
 PJ1019_RUNTIME_PROFILE denied=approved;
 denied.GainLimitApplied=FALSE;
 before=sends;
 assert(Pj1019GpioStartMuted(&gpio,&approved)==STATUS_INVALID_DEVICE_STATE && sends==before);
 assert(Pj1019GpioSetPower(&gpio,TRUE)==0 && last_gpio_value==1 && gpio.ShutdownConfirmed);
 before=sends;
 assert(Pj1019GpioStartMuted(&gpio,&denied)==STATUS_ACCESS_DENIED && sends==before);
 assert(Pj1019GpioStartMuted(&gpio,&approved)==0 && last_gpio_value==0 && delays==1);
 assert(Pj1019GpioShutdown(&gpio)==0 && last_gpio_value==1 && gpio.ShutdownConfirmed);
 assert(Pj1019GpioStartMuted(&gpio,&approved)==0 && delays==1);
 fail_send=1;assert(Pj1019GpioSetPower(&gpio,FALSE)==-12);fail_send=0;
 assert(!gpio.Powered && !gpio.ShutdownConfirmed && gpio.Faulted);
 assert(Pj1019GpioClose(&gpio)==STATUS_IO_DEVICE_ERROR);
 assert(Pj1019GpioOpen(&owner,2,0,Pj1019SdbActiveHigh,&gpio)==0);
 assert(Pj1019GpioSetPower(&gpio,TRUE)==0 && last_gpio_value==0 && !gpio.Faulted);
 fail_delay=1;
 assert(Pj1019GpioStartMuted(&gpio,&approved)==-13 && last_gpio_value==0 && gpio.Faulted);
 assert(Pj1019GpioStartMuted(&gpio,&approved)==STATUS_INVALID_DEVICE_STATE);
 fail_delay=0;
 assert(Pj1019GpioSetPower(&gpio,FALSE)==0);
 assert(Pj1019GpioSetPower(&gpio,TRUE)==0);
 assert(Pj1019GpioStartMuted(&gpio,&approved)==0 && last_gpio_value==1);
 assert(Pj1019GpioClose(&gpio)==0 && last_gpio_value==0);
 assert(Pj1019GpioClose(&gpio)==0 && !live_targets);
 destroy_lock(spb.Lock);destroy_lock(gpio.Lock);
 puts("PASS: full Windows transport sources with host WDF doubles, lifecycle/failures and close serialization");
 return 0;
}
