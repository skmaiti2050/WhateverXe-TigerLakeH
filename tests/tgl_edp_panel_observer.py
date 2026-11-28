#!/usr/bin/env python3
"""Compile and test the actual passive PPS/HPD observer code in userspace.
No IOKit/real GPU/MMIO access. Sanitizers detect memory/locking errors.
"""
from pathlib import Path
import re
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[1]
src=(repo/"WhateverXe/kern_xe.cpp").read_text()
header=re.search(r"struct TGLAUXNativeMMIO \{.*?\n\};",src,re.S)
body=re.search(r"constexpr size_t tglEDPPanelSlots=6;.*?(?=static void tglEDPAUXNativeMMIORecord)",src,re.S)
aux=re.search(r"static void tglEDPAUXNativeMMIORecord\(.*?\n\}",src,re.S)
first=re.search(r"struct TGLAUXFirstCommandWitness \{.*?(?=static bool tglEDPAUXNativeMMIOSerialize)",src,re.S)
state=re.search(r"// Sticky first-AUX command identity.*?\nstatic uint32_t tglEDPAUXRecentData1Count\{\};",src,re.S)
marker=re.search(r"uint32_t XE::wrapTGLFramebufferWakeEpoch\(.*?\n\}",src,re.S)
assert first and state
assert header and body and aux and marker
# Regression gate: retire the disproven PWM re-arm path without weakening
# the existing native display-wake handoff and AUX diagnostic hooks.
wake = marker.group(0)
assert 'checkKernelArgument("-wex-tgl-displaywake-pwm")' not in src
assert "TGLDisplayWakePWM::" not in src
assert not (repo/"WhateverXe/kern_tgl_display_wake_pwm.hpp").exists()
assert not (repo/"tests/tgl_display_wake_pwm.cpp").exists()
assert "getPG(" not in wake and "write(" not in wake and "read(" not in wake
assert "FunctionCast(wrapTGLFramebufferWakeEpoch" in wake
# Source contract: opt-in first-AUX snapshots, no continuous PPS/HPD ring and
# no implicit ComboClock stage trace just because refdiag is active.
assert "tglEDPPanelMMIOFirst" not in src
assert "tglEDPPanelMMIOLast" not in src
assert "tglEDPPanelNativeMMIOSerialize" not in src
assert "tglInitTransitionTrace(tglDisplayTraceEnabled)" in src
assert "TGLComboClockTraceReady" not in src
for check in (
    "tglEDPPanelNativeMMIORecord(access,reg,false,value,",
    "tglEDPPanelNativeMMIORecord(access,reg,true,value,",
    "WhateverXeEDPPanelPreAUX",
    "WhateverXeEDPCustomAUXPreSend",
    "WhateverXeEDPLowPowerPreSend",
    "route(symbol,wrapTGLFramebufferWakeEpoch,orgTGLFramebufferWakeEpoch)",
):
    assert check in src,check

fixture=r"""
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <vector>
#include <cstdio>
template<class T,size_t N> constexpr size_t arrsize(const T (&)[N]){return N;}
struct OSSerialize {std::vector<uint8_t> bytes;};
bool locked=false,failAlloc=false,failData=false;
struct IOSimpleLock {};
IOSimpleLock auxLock,*tglAUXLock=&auxLock;
int IOSimpleLockLockDisableInterrupt(IOSimpleLock *l) {
  assert(l==tglAUXLock&&!locked);locked=true;return 4;
}
void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *l,int f) {
  assert(l==tglAUXLock&&f==4&&locked);locked=false;
}
void *IOMalloc(size_t n) {assert(!locked);return failAlloc?nullptr:malloc(n);}
void IOFree(void *p,size_t) {assert(!locked);free(p);}
struct OSData {
  std::vector<uint8_t> bytes;
  static OSData *withBytes(const void *raw,size_t n) {
    assert(!locked);
    if(failData)return nullptr;
    auto p=new OSData;
    p->bytes.assign((const uint8_t*)raw,(const uint8_t*)raw+n);
    return p;
  }
  bool serialize(OSSerialize *out) {assert(!locked);out->bytes=bytes;return true;}
  void release() {assert(!locked);delete this;}
};
uint64_t ticks=0;
void clock_get_uptime(uint64_t *p) {*p=++ticks;}
void absolutetime_to_nanoseconds(uint64_t time,uint64_t *p){*p=time;}
uintptr_t currentThreadToken=0x12345678;
void *current_thread(){return reinterpret_cast<void *>(currentThreadToken);}
uint32_t tglConnectionCaller(void *){return 0x1234;}
uint32_t tglEDPAUXWakeEpoch=0;
///PRODUCTION_TYPE
static TGLAUXNativeMMIO tglEDPAUXMMIOFirst[64]{},tglEDPAUXMMIOLast[256]{};
static uint32_t tglEDPAUXMMIOCount{},tglEDPAUXMMIOEpochCount{};
///PRODUCTION_STATE
///PRODUCTION_PANEL
///PRODUCTION_AUX
///PRODUCTION_FIRST
struct IOService {
  alignas(8) uint8_t storage[0x4B90]{};
  struct MetaClass {
    size_t size=0x4B90;
    size_t getClassSize() const {return size;}
  } meta;
  const MetaClass *getMetaClass() const {return &meta;}
};
struct XE {
  bool tglEDPRefDiagRequested=true;
  uint32_t (*orgTGLFramebufferWakeEpoch)(IOService *,uint32_t)=nullptr;
  static uint32_t wrapTGLFramebufferWakeEpoch(IOService *,uint32_t);
};
struct {uint32_t word[28];} tglEDPAUXFirstFailure{};
uint32_t expectedEpoch,expectedRequest,nativeCalls;
IOService framebuffer;
uint32_t nativePowerState(IOService *fb,uint32_t requested) {
  assert(fb==&framebuffer && requested==expectedRequest && !locked);
  assert(tglEDPAUXWakeEpoch==expectedEpoch);
  ++nativeCalls;
  return 0xE00002BC;
}
XE observer{true,nativePowerState},*callbackXE=&observer;
#define FunctionCast(method, native) (native)
///PRODUCTION_MARKER
alignas(8) uint8_t controller[0xC80]{};
int fakeAccess1=7, fakeAccess2=8;
void *access=&fakeAccess1;
void putRead(uint32_t reg,uint32_t val,bool diag=true) {
  tglEDPPanelNativeMMIORecord(access,reg,false,val,(void*)0x999,diag,controller);
}
void putWrite(uint32_t reg,uint32_t val,bool diag=true) {
  tglEDPPanelNativeMMIORecord(access,reg,true,val,(void*)0x999,diag,controller);
}
void firstAUX(uint32_t val=0x8c60023f) {
  tglEDPAUXNativeMMIORecord(access,0x64014,true,0x91020200,(void*)0x999,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x64010,true,val,(void*)0x999,true,controller);
}
TGLAUXFirstCommandWitness firstCommand() {
  OSSerialize x;
  assert(tglEDPAUXFirstCommandSerialize(nullptr,nullptr,&x));
  assert(x.bytes.size()==sizeof(TGLAUXFirstCommandWitness));
  TGLAUXFirstCommandWitness out{};
  memcpy(&out,x.bytes.data(),sizeof(out));
  return out;
}
TGLPanelPreAUXWitness pre() {
  OSSerialize a;
  assert(tglEDPPanelPreAUXSerialize(nullptr,nullptr,&a));
  assert(a.bytes.size()==sizeof(TGLPanelPreAUXWitness));
  TGLPanelPreAUXWitness out{};
  memcpy(&out,a.bytes.data(),sizeof(out));
  return out;
}
TGLCustomAUXPreSendWitness custom() {
  OSSerialize a;assert(tglEDPCustomAUXPreSendSerialize(nullptr,nullptr,&a));
  assert(a.bytes.size()==sizeof(TGLCustomAUXPreSendWitness));
  TGLCustomAUXPreSendWitness out{};memcpy(&out,a.bytes.data(),sizeof(out));return out;
}
TGLLowPowerPreSendWitness lowPower() {
  OSSerialize a;assert(tglEDPLowPowerPreSendSerialize(nullptr,nullptr,&a));
  assert(a.bytes.size()==sizeof(TGLLowPowerPreSendWitness));
  TGLLowPowerPreSendWitness out{};memcpy(&out,a.bytes.data(),sizeof(out));return out;
}
void powerTransition(uint32_t prior,uint32_t requested,uint32_t index,uint32_t epoch) {
  memcpy(framebuffer.storage+0x4284,&prior,sizeof(prior));
  memcpy(framebuffer.storage+0x1DC,&index,sizeof(index));
  expectedEpoch=epoch;
  expectedRequest=requested;
  const auto before=nativeCalls;
  assert(XE::wrapTGLFramebufferWakeEpoch(&framebuffer,requested)==0xE00002BC);
  assert(nativeCalls==before+1);
}
void freshWake() {
  // The next FB0 Doze-to-Wake transition must discard a previously saved failure.
  tglEDPAUXFirstFailure.word[0]=7;
  powerTransition(1,2,0,tglEDPAUXWakeEpoch+1);
  assert(tglEDPAUXFirstFailure.word[0]==0);
}
int main() {
  *reinterpret_cast<void**>(controller+0xC40)=access;
  auto first=pre();
  assert(first.validMask==0 && first.auxEpoch==0);
  // Disabled, unknown register or wrong-owner manager do not record anything.
  putRead(0xC7200,0x80000000,false);
  putRead(0xDEAD,777);
  tglEDPPanelNativeMMIORecord(&fakeAccess2,0xC7200,false,8,nullptr,true,controller);
  assert(tglEDPPanelMMIOCount==0);
  // Other framebuffer transitions retain the current epoch and still delegate.
  powerTransition(0,2,0,0); // Full system sleep.
  powerTransition(2,1,0,0); // Entering Doze.
  powerTransition(1,2,1,0); // Another framebuffer.
  observer.tglEDPRefDiagRequested=false;
  powerTransition(1,2,0,0);
  observer.tglEDPRefDiagRequested=true;
  framebuffer.meta.size=0x4000;
  powerTransition(1,2,0,0);
  framebuffer.meta.size=0x4B90;
  freshWake();assert(tglEDPAUXWakeEpoch==1);
  putRead(0xC7200,0x80000000);
  putWrite(0xC7204,0x00000008); // Request, not committed ACK.
  putRead(0xC4000,0x00010000);
  putRead(0xC7208,0x12345678);
  putRead(0xC720C,0x87654321);
  auto noAux=pre();
  assert(noAux.epoch==1 && noAux.auxEpoch==0 && noAux.validMask==0);
  firstAUX();
  auto snap=pre();
  assert(snap.epoch==1 && snap.auxEpoch==1 && snap.auxSequence==2);
  auto firstCmd=firstCommand();
  assert(firstCmd.version==1 && firstCmd.epoch==1 && firstCmd.auxValid==1 && firstCmd.dataValid==1);
  assert(firstCmd.auxCtl.word[3]==0x64010 && firstCmd.auxCtl.word[4]==0x8c60023f);
  assert(firstCmd.data1.word[3]==0x64014 && firstCmd.data1.word[4]==0x91020200);
  assert(snap.validMask==0x1f && snap.firstAuxNS>snap.last[4].ns);
  assert(snap.last[0].word[3]==0xC7200 && snap.last[0].word[4]==0x80000000);
  assert(snap.last[1].word[3]==0xC7204 && snap.last[1].word[4]==8 && snap.last[1].word[2]==1);
  assert(snap.last[4].word[3]==0xC4000 && snap.last[4].word[4]==0x00010000);
  assert(snap.last[5].word[0]==0); // Missing: do NOT claim HPD good.
  putRead(0xC7200,0x00000000); // Post-AUX must not rewrite frozen snapshot.
  putRead(0x44470,0xffffffff);
  firstAUX();
  auto frozen=pre();
  assert(frozen.validMask==0x1f && frozen.last[0].word[4]==0x80000000);
  assert(frozen.last[5].word[0]==0 && frozen.auxSequence==snap.auxSequence);
  assert(tglEDPPanelMMIOCount==7);
  // No continuous PPS/HPD ring: only the immutable pre-SEND snapshot.
  for(unsigned i=0;i<350;++i)firstAUX();
  assert(tglEDPPanelMMIOCount==7 && tglEDPAUXMMIOCount==704);
  assert(tglEDPAUXMMIOFirst[0].word[3]==0x64014 && tglEDPAUXMMIOFirst[1].word[3]==0x64010);
  firstCmd=firstCommand();
  assert(firstCmd.epoch==1 && firstCmd.auxValid && firstCmd.dataValid);
  assert(firstCmd.data1.word[0]==1 && firstCmd.auxCtl.word[0]==2);
  for(unsigned i=0;i<190;++i)putRead(0xC7200,i);
  assert(tglEDPPanelMMIOCount==197);
  assert(pre().last[0].word[4]==0x80000000);
  OSSerialize out;
  failData=true;
  assert(!tglEDPPanelPreAUXSerialize(nullptr,nullptr,&out));
  assert(!tglEDPAUXFirstCommandSerialize(nullptr,nullptr,&out));
  failData=false;
  // New epoch starts with no inherited PPS/HPD witness.
  freshWake();
  firstAUX();
  snap=pre();
  assert(snap.epoch==2 && snap.auxEpoch==2 && snap.validMask==0);
  for(const auto &item:snap.last)assert(item.word[0]==0);
  firstCmd=firstCommand();
  assert(firstCmd.epoch==2 && firstCmd.auxValid && firstCmd.dataValid);
  freshWake();
  tglEDPAUXNativeMMIORecord(access,0x64010,true,0x8c60023f,(void*)0x999,true,controller);
  firstCmd=firstCommand();
  assert(firstCmd.epoch==3 && firstCmd.auxValid==1 && firstCmd.dataValid==0);
  // Two same-epoch AUX threads may interleave their DATA1 writes before
  // native SEND. The wrong-thread newest header must not mask the right one.
  freshWake();
  tglEDPAUXNativeMMIORecord(access,0x64014,true,0x80010001,(void*)0x999,true,controller);
  currentThreadToken=0x87654321;
  tglEDPAUXNativeMMIORecord(access,0x64014,true,0x80020201,(void*)0x999,true,controller);
  currentThreadToken=0x12345678;
  tglEDPAUXNativeMMIORecord(access,0x64010,true,0x8c60023f,(void*)0x999,true,controller);
  firstCmd=firstCommand();
  assert(firstCmd.epoch==4 && firstCmd.auxValid==1 && firstCmd.dataValid==1);
  assert(firstCmd.data1.word[4]==0x80010001 &&
         firstCmd.data1.thread==firstCmd.auxCtl.thread);
  // Fail closed when the same-thread header was evicted by other threads.
  freshWake();
  tglEDPAUXNativeMMIORecord(access,0x64014,true,0x80010001,(void*)0x999,true,controller);
  currentThreadToken=0x87654321;
  for(unsigned i=0;i<32;++i)
    tglEDPAUXNativeMMIORecord(access,0x64014,true,0x80020201,(void*)0x999,true,controller);
  currentThreadToken=0x12345678;
  tglEDPAUXNativeMMIORecord(access,0x64010,true,0x8c60023f,(void*)0x999,true,controller);
  firstCmd=firstCommand();
  assert(firstCmd.epoch==5 && firstCmd.auxValid==1 && firstCmd.dataValid==0);

  // New independent epoch: inspect both native hwConfigureCustomAUX
  // configuration banks without touching MMIO or fabricating a readback.
  freshWake();
  const auto countBeforeCustom=tglEDPAUXMMIOCount;
  const uint32_t customEpoch=tglEDPAUXWakeEpoch;
  tglEDPAUXNativeMMIORecord(access,0x86338,true,0x11,(void*)0x999,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x86364,true,0x8850023f,(void*)0x999,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x80040,true,0x22,(void*)0x999,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x8006C,true,0x33,(void*)0x999,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x80058,false,0x44,(void*)0x999,true,controller);
  // Reject misalignment, out-of-range addresses, disabled diagnostics and
  // foreign accessor ownership. No fallback/probing of unknown banks.
  for(uint32_t reg:{0x86337U,0x86339U,0x86368U,0x8003CU,0x80041U,0x80070U,0xDEADU})
    tglEDPAUXNativeMMIORecord(access,reg,true,0x777,(void*)0x999,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x8633C,true,0x777,(void*)0x999,false,controller);
  tglEDPAUXNativeMMIORecord(&fakeAccess2,0x8633C,true,0x777,(void*)0x999,true,controller);
  assert(tglEDPAUXMMIOCount==countBeforeCustom+5);
  for(uint32_t i=0;i<5;++i) {
    const auto &rec=tglEDPAUXMMIOFirst[i];
    assert(rec.word[0]==countBeforeCustom+i+1);
    assert(rec.word[1]==customEpoch);
    assert(rec.word[2]==(i==4 ? 0U : 1U));
  }
  assert(tglEDPAUXMMIOFirst[0].word[3]==0x86338);
  assert(tglEDPAUXMMIOFirst[1].word[3]==0x86364);
  assert(tglEDPAUXMMIOFirst[2].word[3]==0x80040);
  assert(tglEDPAUXMMIOFirst[3].word[3]==0x8006C);
  assert(tglEDPAUXMMIOFirst[4].word[3]==0x80058);
  assert(tglEDPAUXMMIOFirst[4].word[4]==0x44);
  firstAUX();
  firstCmd=firstCommand();
  assert(firstCmd.epoch==customEpoch && firstCmd.auxValid && firstCmd.dataValid);
  assert(tglEDPAUXMMIOFirst[4].ns<=firstCmd.auxCtl.ns);
  assert(firstCmd.auxCtl.word[3]==0x64010);
  // Post-SEND reads are not proof that the pre-SEND configuration succeeded.
  tglEDPAUXNativeMMIORecord(access,0x8633C,false,0x55,(void*)0x999,true,controller);
  assert(tglEDPAUXMMIOCount==countBeforeCustom+8);
  assert(tglEDPAUXMMIOFirst[4].word[4]==0x44);
  auto c=custom();
  assert(c.version==1 && c.epoch==customEpoch && c.auxEpoch==customEpoch);
  assert(c.auxSequence==firstCmd.auxCtl.word[0] && c.firstAuxNS==firstCmd.auxCtl.ns);
  assert(c.readMask==(1U<<18));
  assert(c.writeMask==((1U<<0)|(1U<<11)|(1U<<12)|(1U<<23)));
  assert(c.last[1][0].word[4]==0x11 && c.last[1][11].word[4]==0x8850023f);
  assert(c.last[0][18].word[4]==0x44 && c.last[0][1].word[0]==0);
  // Ring overflow after SEND cannot erase the independently frozen receipts.
  for(unsigned i=0;i<400;++i) {
    tglEDPAUXNativeMMIORecord(access,0x86338,true,i,nullptr,true,controller);
    tglEDPAUXNativeMMIORecord(access,0x64010,false,i,nullptr,true,controller);
  }
  auto frozenCustom=custom();assert(!memcmp(&c,&frozenCustom,sizeof(c)));
  failAlloc=true;assert(!tglEDPCustomAUXPreSendSerialize(nullptr,nullptr,&out));failAlloc=false;
  failData=true;assert(!tglEDPCustomAUXPreSendSerialize(nullptr,nullptr,&out));failData=false;
  freshWake();
  // Place Custom AUX in the actual gap between the first-64 and last-256
  // rings, and retain the latest READ and WRITE independently.
  for(unsigned i=0;i<80;++i)tglEDPAUXNativeMMIORecord(access,0x64010,false,i,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x80040,true,0x1111,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x80040,true,0x1234,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x80040,false,0x5678,nullptr,true,controller);
  for(unsigned i=0;i<400;++i)tglEDPAUXNativeMMIORecord(access,0x64010,false,i,nullptr,true,controller);
  assert(custom().auxEpoch==0 && custom().readMask==0 && custom().writeMask==0);
  firstAUX();c=custom();
  assert(c.readMask==(1U<<12) && c.writeMask==(1U<<12));
  assert(c.last[0][12].word[4]==0x5678 && c.last[1][12].word[4]==0x1234);
  for(const auto &r:tglEDPAUXMMIOFirst)
    assert(r.word[0]!=c.last[0][12].word[0] && r.word[0]!=c.last[1][12].word[0]);
  for(const auto &r:tglEDPAUXMMIOLast)
    assert(r.word[0]!=c.last[0][12].word[0] && r.word[0]!=c.last[1][12].word[0]);
  assert(c.last[1][0].word[0]==0);
  freshWake();
  // A fresh epoch has no inherited receipts.
  firstAUX();c=custom();assert(c.readMask==0 && c.writeMask==0);

  // Preserve read and write receipts independently across ring overflow.
  freshWake();
  for(unsigned i=0;i<80;++i)tglEDPAUXNativeMMIORecord(access,0x64010,false,i,nullptr,true,controller);
  const auto beforeLow=tglEDPAUXMMIOCount;
  tglEDPAUXNativeMMIORecord(access,0x60800,true,0x00101001,nullptr,false,controller);
  tglEDPAUXNativeMMIORecord(&fakeAccess2,0x60840,false,0,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x60804,false,0,nullptr,true,controller);
  assert(tglEDPAUXMMIOCount==beforeLow);
  tglEDPAUXNativeMMIORecord(access,0x60800,false,0x80101001,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x60800,true,0x00101001,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x60840,false,0x40000000,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x60840,false,0x20000000,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x45504,false,2,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x45504,true,0,nullptr,true,controller);
  for(unsigned i=0;i<400;++i)tglEDPAUXNativeMMIORecord(access,0x64010,false,i,nullptr,true,controller);
  auto lp=lowPower();assert(lp.auxEpoch==0 && !lp.readMask && !lp.writeMask);
  firstAUX();lp=lowPower();
  assert(lp.version==1 && lp.slots==3 && lp.itemSize==48 && lp.epoch==tglEDPAUXWakeEpoch);
  assert(lp.auxEpoch==lp.epoch && lp.auxSequence==firstCommand().auxCtl.word[0]);
  assert(lp.readMask==7 && lp.writeMask==5);
  assert(lp.last[0][0].word[4]==0x80101001 && lp.last[1][0].word[4]==0x00101001);
  assert(lp.last[0][1].word[4]==0x20000000 && !lp.last[1][1].word[0]);
  assert(lp.last[0][2].word[4]==2 && lp.last[1][2].word[4]==0);
  assert(lp.last[0][2].word[0]<lp.last[1][2].word[0]);
  for(const auto &r:tglEDPAUXMMIOFirst)assert(r.word[0]!=lp.last[0][1].word[0]);
  for(const auto &r:tglEDPAUXMMIOLast)assert(r.word[0]!=lp.last[0][1].word[0]);
  tglEDPAUXNativeMMIORecord(access,0x60840,false,0,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x45504,false,0,nullptr,true,controller);
  auto lpFrozen=lowPower();assert(!memcmp(&lp,&lpFrozen,sizeof(lp)));
  failAlloc=true;assert(!tglEDPLowPowerPreSendSerialize(nullptr,nullptr,&out));failAlloc=false;
  failData=true;assert(!tglEDPLowPowerPreSendSerialize(nullptr,nullptr,&out));failData=false;
  freshWake();
  firstAUX();lp=lowPower();assert(lp.readMask==0 && lp.writeMask==0);
  freshWake();
  tglEDPAUXNativeMMIORecord(access,0x45504,true,0,nullptr,true,controller);
  tglEDPAUXNativeMMIORecord(access,0x45504,false,0,nullptr,true,controller);
  firstAUX();lp=lowPower();
  assert(lp.readMask==4 && lp.writeMask==4 && lp.last[0][2].word[0]>lp.last[1][2].word[0]);

  // Exact Apple donor clamps requested>2 to Wake=2 before native dispatch.
  // A Doze -> invalid-high request still marks a new first-AUX epoch; WEX
  // passes the original argument to Apple unmodified.
  const auto nextEpoch=tglEDPAUXWakeEpoch+1;
  powerTransition(1,3,0,nextEpoch);
  assert(tglEDPAUXWakeEpoch==nextEpoch);
  firstAUX();
  assert(firstCommand().epoch==tglEDPAUXWakeEpoch);
  assert(pre().epoch==tglEDPAUXWakeEpoch);

  assert(!locked);
  std::puts("PASS: framebuffer epoch and native delegation, passive native Custom AUX/DC6 receipts, first-AUX PPS/HPD snapshot and serialization, no PPS event ring, failure cleanup");
}
"""
out=(fixture.replace("///PRODUCTION_TYPE",header.group()).replace("///PRODUCTION_STATE",state.group()).replace("///PRODUCTION_PANEL",body.group()).replace("///PRODUCTION_AUX",aux.group()).replace("///PRODUCTION_FIRST",first.group()).replace("///PRODUCTION_MARKER",marker.group()))
assert "///PRODUCTION" not in out
with tempfile.TemporaryDirectory(prefix="tgl-edp-panel-observer-") as tmp:
    p=Path(tmp)
    (p/"test.cpp").write_text(out)
    subprocess.run(["clang++","-std=c++14","-O1","-g","-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",str(p/"test.cpp"),"-o",str(p/"test")],check=True)
    subprocess.run([str(p/"test")],check=True)
