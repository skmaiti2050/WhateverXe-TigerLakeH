#!/usr/bin/env python3
"""Execute the passive lifecycle installer and original callback ABI wrappers."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'WhateverXe/kern_xe.cpp').read_text()
def extract(marker):
    a=source.index(marker); b=source.index('{',a+len(marker)); depth=0
    for i in range(b,len(source)):
        if source[i]=='{': depth+=1
        elif source[i]=='}':
            depth-=1
            if depth==0:return source[a:i+1]
    raise AssertionError(marker)
functions='\n'.join(extract(x) for x in (
    'static void tglS0Mark(void *owner,unsigned stage)',
    'static void tglS0TraceAccelSleep(void *owner)',
    'static void tglS0TraceAccelWake(void *owner)',
    'static IOReturn tglS0TracePower(void *owner,unsigned long state,IOService *service)',
    'static void tglS0InstallEvidence(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size)'))
process=extract('void XE::wrapTGLCSBProcess(void *streamer,uint32_t flags)')
assert process.index('tglS0Dispatch[io.engine].begin(key,witness)') < process.index('tglCSBMethods[handler](streamer,&entry)') < process.index('tglS0Dispatch[io.engine].complete(key,witness')
assert 'reinterpret_cast<uintptr_t>(&entry)' in process
fatal=extract('static void tglHostCSBFatal(const char *reason,const uint32_t *entry,void *failedStreamer)')
assert 'slot.find(reinterpret_cast<uintptr_t>(entry),reinterpret_cast<uintptr_t>(current_thread()),receipt)' in fatal
assert fatal.index('live={%s}') < fatal.index('trail(early)=%s')
code=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include "kern_tgl_s0_evidence.hpp"
using mach_vm_address_t=uintptr_t;
using IOReturn=int;
struct IOService{};
struct OSSerializer { static OSSerializer *forTarget(void *,bool (*)(void *,void *,OSSerializer *)){return nullptr;} void release(){} };
static bool tglS0TimelineSerialize(void *,void *,OSSerializer *){return true;}
struct IORegistryEntry {
 static IORegistryEntry *getRegistryRoot(){static IORegistryEntry r;return &r;}
 void setProperty(const char *k,uint32_t v,int){assert(std::string(k)=="TGLS0EvidenceRoutes");mask=v;}
 void setProperty(const char *,OSSerializer *){}
 uint32_t mask=0;
};
static TGLS0Evidence::Timeline tglS0Timeline;
static bool tglS0EvidenceEnabled=false;
static uint32_t tglS0EvidenceRoutes=0;
static mach_vm_address_t tglS0OrgAccelSleep=0,tglS0OrgAccelWake=0,tglS0OrgPower=0;
static int owner,other;
static struct{void *owner;}tglHost{&owner};
static int sleepCalls=0,wakeCalls=0,powerCalls=0;
static IOService service;
static void nativeSleep(void *p){++sleepCalls;if(p==&owner)assert(tglS0Timeline.at(TGLS0Evidence::SleepEnter));}
static void nativeWake(void *p){++wakeCalls;if(p==&owner)assert(tglS0Timeline.at(TGLS0Evidence::WakeEnter));}
static IOReturn nativePower(void *p,unsigned long state,IOService *s){++powerCalls;assert(s==&service);if(p==&owner&&state==2)assert(tglS0Timeline.at(TGLS0Evidence::Power2Enter));return 0x1234;}
struct KernelPatcher {
 struct RouteRequest { const char *symbol;uintptr_t replacement;mach_vm_address_t *org;
  template<class T>RouteRequest(const char *s,T f,mach_vm_address_t &o):symbol(s),replacement(reinterpret_cast<uintptr_t>(f)),org(&o){}
 };
 const char *names[3]={"__ZN16IntelAccelerator15systemWillSleepEv","__ZN16IntelAccelerator13systemDidWakeEv","__ZN16IntelAccelerator13setPowerStateEmP9IOService"};
 uint32_t offsets[3]={0x28916,0x2897A,0x2A9B6};
 int fail=-1,wrong=-1;unsigned attempted=0;
 int id(const char *s){for(int i=0;i<3;++i)if(std::string(s)==names[i])return i;assert(false);return -1;}
 template<class T>T solveSymbol(size_t,const char *s,mach_vm_address_t base,size_t){int i=id(s);return base+offsets[i]+(i==wrong?1:0);}
 bool routeMultiple(size_t,RouteRequest *r,size_t n,mach_vm_address_t,size_t){assert(n==1);int i=id(r->symbol);attempted|=1U<<i;if(i==fail)return false;
  const uintptr_t originals[]={reinterpret_cast<uintptr_t>(nativeSleep),reinterpret_cast<uintptr_t>(nativeWake),reinterpret_cast<uintptr_t>(nativePower)};
  *r->org=originals[i];return true;}
 void clearError(){}
};
@FUNCTIONS@
int main(){
 const uint8_t h[]={0x55,0x48,0x89,0xE5,0x53,0x50,0x48,0x89,0xFB};
 const uint8_t p[]={0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x53,0x50,0x49,0x89,0xF6,0x48,0x89,0xFB};
 for(int scenario=0;scenario<4;++scenario){
  tglS0EvidenceEnabled=false;tglS0EvidenceRoutes=0;tglS0Timeline={};
  tglS0OrgAccelSleep=tglS0OrgAccelWake=tglS0OrgPower=0;
  KernelPatcher patcher;std::vector<uint8_t> image(0x2B000,0);
  memcpy(image.data()+0x28916,h,sizeof(h));memcpy(image.data()+0x2897A,h,sizeof(h));memcpy(image.data()+0x2A9B6,p,sizeof(p));
  if(scenario==1)patcher.fail=1;
  if(scenario==2)image[0x28916]=0xcc;
  if(scenario==3)patcher.wrong=2;
  tglS0InstallEvidence(patcher,0,reinterpret_cast<uintptr_t>(image.data()),image.size());
  const unsigned expected[]={7,5,6,3};assert(tglS0EvidenceRoutes==expected[scenario]);assert(IORegistryEntry::getRegistryRoot()->mask==expected[scenario]);assert(tglS0EvidenceEnabled);
  if(expected[scenario]&1){tglS0TraceAccelSleep(&owner);assert(tglS0Timeline.at(TGLS0Evidence::SleepExit)>tglS0Timeline.at(TGLS0Evidence::SleepEnter));auto old=tglS0Timeline.at(TGLS0Evidence::SleepExit);tglS0TraceAccelSleep(&other);assert(tglS0Timeline.at(TGLS0Evidence::SleepExit)==old);}
  if(expected[scenario]&2){tglS0TraceAccelWake(&owner);assert(tglS0Timeline.at(TGLS0Evidence::WakeExit)>tglS0Timeline.at(TGLS0Evidence::WakeEnter));}
  if(expected[scenario]&4){assert(tglS0TracePower(&owner,2,&service)==0x1234);assert(tglS0Timeline.at(TGLS0Evidence::Power2Exit)>tglS0Timeline.at(TGLS0Evidence::Power2Enter));auto old=tglS0Timeline.at(TGLS0Evidence::Power2Exit);assert(tglS0TracePower(&other,2,&service)==0x1234);assert(tglS0Timeline.at(TGLS0Evidence::Power2Exit)==old);assert(tglS0TracePower(&owner,1,&service)==0x1234);assert(tglS0Timeline.at(TGLS0Evidence::Power0Enter)==0);}
 }
 assert(sleepCalls==6&&wakeCalls==3&&powerCalls==9);
 std::cout<<"PASS native callback ABI, exact donor guard, partial routing, owner gate and unchanged return values\n";
}
'''.replace('@FUNCTIONS@',functions)
with tempfile.TemporaryDirectory(prefix='wex-s0-evidence-route-') as d:
    p=Path(d);(p/'test.cpp').write_text(code)
    subprocess.run(['clang++','-std=c++17','-O1','-g','-fsanitize=address,undefined','-I'+str(ROOT/'WhateverXe'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
