#!/usr/bin/env python3
"""Exercise the actual production capture/adoption wrappers, including an IRQ
shutdown occurring before the sleep callback. No real GPU/MMIO is accessed."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
text=(ROOT/'WhateverXe/kern_xe.cpp').read_text()
def function(marker):
    a=text.index(marker);b=text.index('{',a+len(marker));d=0
    for i in range(b,len(text)):
        if text[i]=='{':d+=1
        elif text[i]=='}':
            d-=1
            if not d:return text[a:i+1]
    raise AssertionError(marker)
markers=['static bool tglS0CanonicalBridge(void *bridge,void *accelerator)', 'static void tglS0InvalidateIRQCandidate(void *bridge) ', 'static bool tglS0RecordUnchanged(const TGLS0SleepRecord &record,const TGLCSBEngine &engine)', 'static void tglS0CaptureBeforeIRQDisable(void *accelerator,uint32_t ticket)', 'static void tglS0CaptureIRQDisable(void *bridge) ', 'static void tglS0WrapBridgeSystemWillSleep(void *bridge)']
funcs=[function(m) for m in markers]
host=function('void XE::wrapTGLHostIRQClassDisable(void *bridge)')
wake=function('void XE::wrapTGLCSBWake(void *streamer)')
assert host.index('xelpIRQStop(bridge)')<host.index('tglS0CaptureIRQDisable(bridge)')<host.index('orgTGLHostIRQClassDisable)(bridge)')
assert 'TGLCSBIO' not in funcs[-1]
harness=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "kern_tgl_csb.hpp"
using mach_vm_address_t=uintptr_t;
#define arrsize(x) (sizeof(x)/sizeof((x)[0]))
struct TGLCSBEngine {void *streamer{}; TGLCSB::State state; TGLCSB::Activity activity; bool hardwareReady{};uint32_t modeAtInit{UINT32_MAX};};
struct TGLS0SleepRecord {TGLCSB::WakeBaseline before{};uintptr_t slots[4]{};void *streamer{};uint32_t epoch{},ticket{};uint64_t revision{};};
static TGLCSBEngine tglCSBEngines[6];static TGLS0SleepRecord tglS0Sleep[6];
static TGLCSB::SleepCandidate tglS0Candidate;
static uint32_t tglS0Epoch=0,tglS0CaptureEvidence[6][6]{},tglS0ResetEvidence[6][3]{},tglS0WakeTrace[6][13]{},tglS0WakeCount=0;
static bool tglCSB12Enabled=true;
static bool checkKernelArgument(const char *){return true;}
static mach_vm_address_t tglS0OrgBridgeSystemWillSleep=0;
static bool tglS0SleepHookRouted=true;
static struct {void *owner=nullptr;} tglHost;
namespace TGLS0Evidence {enum {IRQDisableEnter,IRQDisableExit,CaptureEnter,CaptureExit,BridgeSleepEnter,BridgeSleepExit,ResetEnter0,ResetExit0=ResetEnter0+6};}
static void tglS0Mark(void *,unsigned){}
alignas(8) static uint8_t owner[0x1300],otherOwner[0x1300],bridge[0x900],foreign[0x900],sameOwnerForeign[0x900],streamer[0x1d00];
static uint32_t pointer=0x8b08,tail=8,mode=8;
static unsigned acquired=0,reads=0,originalIRQ=0,originalSleep=0,stops=0,properties=0,initializations=0,verifiedInitializations=0;
static bool mutateAfterDisable=false;
static std::vector<std::string> events;
struct TGLCSBIO {
 uint8_t *streamer;void *owner;unsigned engine=0;TGLCSBIO(void *s,unsigned):streamer(static_cast<uint8_t *>(s)),owner(*reinterpret_cast<void **>(streamer+0x10)){}
 void acquire(){++acquired;assert(bridge[0x8a8]);events.emplace_back("snapshot");}
 uint32_t readMMIOPointer(){++reads;return pointer;}
 uint32_t readTail(){++reads;return tail;}
 uint32_t readMode(){++reads;return mode;}
 void writeMode(uint32_t value){mode=(mode&~(value>>16))|(value&(value>>16));}
};
@HELPERS@
struct XE{void *tglObservationController=nullptr;mach_vm_address_t orgTGLHostIRQClassDisable=0,orgTGLCSBWake=0;static void wrapTGLHostIRQClassDisable(void *bridge);static void wrapTGLCSBWake(void *streamer);};
static XE driver;static XE *callbackXE=&driver;
static void *tglFBIRQBegin(int,void *,uintptr_t,uintptr_t){return nullptr;}
static void tglFBIRQEnd(void *,int){}
static void xelpIRQStop(void *){++stops;}
struct IORegistryEntry{static IORegistryEntry *getRegistryRoot(){static IORegistryEntry r;return &r;}void setProperty(const char *k,bool v){assert(std::string(k)=="XELPVideoIRQReady"&&!v);++properties;}};
#define FunctionCast(wrapper,original) reinterpret_cast<void (*)(void *)>(original)
@HOST@
static void tglCSBInitialize(void *s,uint32_t=0,bool verify=false){++initializations;verifiedInitializations+=verify;assert(s==streamer);auto &e=tglCSBEngines[0];e.state.head=TGLCSB::ResetIndex;*reinterpret_cast<uint32_t *>(streamer+0x1c04)=e.state.head;}
@WAKE@
static void nativeIRQ(void *){++originalIRQ;assert(stops==originalIRQ&&properties==originalIRQ);events.emplace_back("native-irq-off");}
static void fullDisable(uint8_t *b){if(b[0x8a8]){XE::wrapTGLHostIRQClassDisable(b);b[0x8a8]=0;if(mutateAfterDisable)tglCSBEngines[0].activity.change();}}
static void nativeSleep(void *b){++originalSleep;events.emplace_back("native-sleep");fullDisable(static_cast<uint8_t *>(b));}
static void init(){
 memset(owner,0,sizeof(owner));memset(otherOwner,0,sizeof(otherOwner));memset(bridge,0,sizeof(bridge));memset(foreign,0,sizeof(foreign));memset(sameOwnerForeign,0,sizeof(sameOwnerForeign));memset(streamer,0,sizeof(streamer));
 for(auto &e:tglCSBEngines)e={};for(auto &r:tglS0Sleep)r={};tglS0Candidate={};tglS0Epoch=0;
 acquired=reads=originalIRQ=originalSleep=stops=properties=initializations=verifiedInitializations=tglS0WakeCount=0;events.clear();pointer=0x8b08;tail=8;mode=8;mutateAfterDisable=false;tglS0SleepHookRouted=true;
 tglHost.owner=owner;*reinterpret_cast<void **>(owner+0x1248)=bridge;*reinterpret_cast<void **>(bridge+0x10)=owner;*reinterpret_cast<void **>(foreign+0x10)=otherOwner;*reinterpret_cast<void **>(sameOwnerForeign+0x10)=owner;
 bridge[0x8a8]=foreign[0x8a8]=sameOwnerForeign[0x8a8]=1;
 *reinterpret_cast<void **>(streamer+0x10)=owner;*reinterpret_cast<uint32_t *>(streamer+0x50)=1;*reinterpret_cast<uint32_t *>(streamer+0x1c04)=8;
 auto &e=tglCSBEngines[0];e.streamer=streamer;e.hardwareReady=true;e.modeAtInit=8;e.state.head=8;e.state.error=TGLCSB::Error::None;
 driver.orgTGLHostIRQClassDisable=reinterpret_cast<uintptr_t>(&nativeIRQ);tglS0OrgBridgeSystemWillSleep=reinterpret_cast<uintptr_t>(&nativeSleep);
}
static bool bound(){return tglS0Sleep[0].epoch&&tglS0Sleep[0].epoch==tglS0Epoch;}
int main(){
 init();fullDisable(bridge);assert(acquired==1&&!bound());tglS0WrapBridgeSystemWillSleep(bridge);assert(bound()&&originalIRQ==1&&originalSleep==1&&reads==3);
 assert((events==std::vector<std::string>{"snapshot","native-irq-off","native-sleep"}));
 // One early native shutdown is consumed once. Second sleep without a new
 // disable cannot reuse the same candidate.
 tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound()&&acquired==1);
 init();tglS0WrapBridgeSystemWillSleep(bridge);assert(bound()&&originalIRQ==1&&acquired==1);
 assert((events==std::vector<std::string>{"native-sleep","snapshot","native-irq-off"}));
 init();fullDisable(foreign);fullDisable(sameOwnerForeign);assert(acquired==0);fullDisable(bridge);tglS0WrapBridgeSystemWillSleep(foreign);assert(!bound());tglS0WrapBridgeSystemWillSleep(bridge);assert(bound()&&acquired==1);
 init();fullDisable(bridge);tglS0InvalidateIRQCandidate(bridge);tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound());
 init();fullDisable(bridge);tglS0InvalidateIRQCandidate(sameOwnerForeign);tglS0WrapBridgeSystemWillSleep(bridge);assert(bound());tglS0InvalidateIRQCandidate(bridge);assert(bound());
 init();fullDisable(bridge);tglCSBEngines[0].activity.change();tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound());
 init();fullDisable(bridge);*reinterpret_cast<uint32_t *>(streamer+0x1c04)=9;tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound());
 init();fullDisable(bridge);*reinterpret_cast<uintptr_t *>(streamer+0x30)=0x1000;tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound());
 init();mutateAfterDisable=true;tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound());
 init();*reinterpret_cast<uintptr_t *>(streamer+0x48)=0x1234;tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound()&&acquired==1);
 init();tail=9;tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound());
 init();mode=UINT32_MAX;tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound());
 init();{TGLCSB::ActivityScope busy(&tglCSBEngines[0].activity);tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound()&&acquired==0);}
 init();tglS0SleepHookRouted=false;tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound()&&acquired==0&&originalIRQ==1);
 init();bridge[0x8a8]=0;XE::wrapTGLHostIRQClassDisable(bridge);tglS0WrapBridgeSystemWillSleep(bridge);assert(!bound()&&acquired==0);
 init();fullDisable(bridge);tglS0WrapBridgeSystemWillSleep(bridge);assert(bound());bridge[0x8a8]=1;fullDisable(bridge);tglS0WrapBridgeSystemWillSleep(bridge);assert(bound()&&acquired==2&&tglS0Epoch==2);
 // Reproduce the real witness: earlier IRQ-off, retained 0x8b08/write=8.
 // Enable before resetOnWake, as on the real system; the bound candidate
 // survives enable. Production wake must NOT reset to 11 and traverse old
 // indexed CSB entries 0..8 on the next tail=9 event.
 init();fullDisable(bridge);tglS0WrapBridgeSystemWillSleep(bridge);assert(bound());
 tglS0InvalidateIRQCandidate(bridge);bridge[0x8a8]=1;XE::wrapTGLCSBWake(streamer);
 assert(initializations==0&&tglCSBEngines[0].state.head==8&&tglS0WakeTrace[0][9]==static_cast<unsigned>(TGLCSB::WakeAction::Preserve)&&!bound());
 init();bridge[0x8a8]=0;tglS0WrapBridgeSystemWillSleep(bridge);bridge[0x8a8]=1;XE::wrapTGLCSBWake(streamer);assert(initializations==1&&verifiedInitializations==1&&tglCSBEngines[0].state.head==11);
 init();fullDisable(bridge);tglS0WrapBridgeSystemWillSleep(bridge);tglCSBEngines[0].activity.change();bridge[0x8a8]=1;XE::wrapTGLCSBWake(streamer);assert(initializations==1&&verifiedInitializations==1);
 init();fullDisable(bridge);tglS0WrapBridgeSystemWillSleep(bridge);pointer=0x8b09;tail=9;bridge[0x8a8]=1;XE::wrapTGLCSBWake(streamer);assert(initializations==1);
 init();fullDisable(bridge);tglS0WrapBridgeSystemWillSleep(bridge);mode=UINT32_MAX;bridge[0x8a8]=1;XE::wrapTGLCSBWake(streamer);assert(initializations==0&&!tglCSBEngines[0].hardwareReady&&tglCSBEngines[0].state.error==TGLCSB::Error::Unavailable);
 std::cout<<"PASS real wrappers: early/inside sleep capture, native-only foreign routes, one-use binding, enable/mutation/busy/slot/head/mode guards\n";
}
'''.replace('@HELPERS@','\n'.join(funcs)).replace('@HOST@',host).replace('@WAKE@',wake)
with tempfile.TemporaryDirectory(prefix='wex-s0-candidate-') as td:
    p=Path(td);(p/'window.cpp').write_text(harness)
    subprocess.run(['clang++','-std=c++17','-O1','-g','-fsanitize=address,undefined','-I'+str(ROOT/'WhateverXe'),str(p/'window.cpp'),'-o',str(p/'window')],check=True)
    subprocess.run([str(p/'window')],check=True)
