#!/usr/bin/env python3
"""Fault-inject the ACTUAL CSB12 installer body against a bounded Lilu stand-in.

Models routeMultiple partial success and both pre/post-write lookup failures.
It is not a real kernel/HW test; it checks production staging and fallback.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "WhateverXe" / "kern_xe.cpp").read_text()


def function_text(marker: str) -> str:
    p = SOURCE.find(marker)
    assert p >= 0, marker
    a = SOURCE.index("{", p + len(marker))
    depth = 0
    for b in range(a, len(SOURCE)):
        if SOURCE[b] == "{":
            depth += 1
        elif SOURCE[b] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[p:b + 1]
    raise AssertionError("unclosed: " + marker)


INSTALL = function_text("bool XE::installTGLCSB12(")
# Do not let a model-only installer pass while production references the old
# live patch order or lacks original-function fallback on partial routing.
assert INSTALL.index("!memcmp(resetSite,resetFind") < INSTALL.index("routeMultipleShort")
assert INSTALL.index("routeMultiple(index,routes,arrsize(routes)") < INSTALL.index("LookupPatch patch {currentGraphics,resetFind")
assert INSTALL.index("LookupPatch patch {currentGraphics,resetFind") < INSTALL.index("LookupPatch patch {currentGraphics,modeFind")
assert INSTALL.index("if(!ok && textPatchesAttempted) {") < INSTALL.index("tglCSB12Enabled=ok")
assert all("if (!tglCSB12Enabled)" in function_text(marker) for marker in (
    "bool XE::wrapTGLCSBInit(", "void XE::wrapTGLCSBWake(",
    "void XE::wrapTGLCSBReset(", "void XE::wrapTGLCSBProcess(",
))

HARNESS = r"""
#include <array>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

using mach_vm_address_t=uintptr_t;
template<class T,size_t N> constexpr size_t arrsize(const T (&)[N]) {return N;}
#define PANIC(...) throw std::runtime_error("fatal CSB12 incomplete installation")
static bool tglCSB12Enabled=false;
static bool tglS0SleepHookRouted=false;
static bool s0Requested=false;
static mach_vm_address_t tglS0OrgBridgeSystemWillSleep=0;
static mach_vm_address_t xelpRegularForceWakeOriginal=0;
static void *currentGraphics=reinterpret_cast<void *>(1);

struct OSSerializer {
 static OSSerializer *forTarget(void *, bool (*)(void *,void *,OSSerializer *)) {
  return new OSSerializer;
 }
 void release() {delete this;}
};
struct IORegistryEntry {
 static IORegistryEntry *getRegistryRoot() {static IORegistryEntry r;return &r;}
 void setProperty(const char *k,bool v) {flags[k]=v;}
 void setProperty(const char *,OSSerializer *) {}
 std::unordered_map<std::string,bool> flags;
 void reset() {flags.clear();}
};
bool checkKernelArgument(const char *) {return s0Requested;}
bool xelpForceWakeIRQSerialize(void *,void *,OSSerializer *) {return true;}
bool tglCSBSerialize(void *,void *,OSSerializer *) {return true;}
bool tglCSBModeSerialize(void *,void *,OSSerializer *) {return true;}
bool tglS0WakeTraceSerialize(void *,void *,OSSerializer *) {return true;}
void xelpRegularForceWakeProtected(void *,bool,unsigned) {}
void tglS0WrapBridgeSystemWillSleep(void *) {}
using TGLCSBMethod=void (*)(void *,const void *);
using TGLCSBForceWake=void (*)(void *,bool,unsigned);
TGLCSBMethod tglCSBMethods[4]{};
TGLCSBForceWake tglCSBForceWake[2]{};

struct KernelPatcher {
 enum class Error {NoError=0,WriteFailure=1};
 Error error=Error::NoError;
 struct RouteRequest {
  const char *symbol;uintptr_t from=0;uintptr_t *org=nullptr;
  template<class T> RouteRequest(const char *s,T,uintptr_t &o):symbol(s),org(&o) {}
 };
 struct LookupPatch {
  void *kext;
  const uint8_t *find,*replace;
  size_t size,count;
 };
 std::unordered_map<std::string,uint32_t> symbolOffsets {
  {"__ZN26IGHardwareCommandStreamer54initEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler510IGHwCsType",0x39A78},
  {"__ZN26IGHardwareCommandStreamer511resetOnWakeEv",0x3A292},
  {"__ZN26IGHardwareCommandStreamer515processGPUResetEv",0x3A396},
  {"__ZN26IGHardwareCommandStreamer526processContextStatusBufferEj",0x3B1C2},
  {"__ZN26IGHardwareCommandStreamer522csbProcessIdleToActiveEPK28SGfxContextStatusBufferEntry",0x3C03C},
  {"__ZN26IGHardwareCommandStreamer519csbProcessPreemptedEPK28SGfxContextStatusBufferEntry",0x3C1CC},
  {"__ZN26IGHardwareCommandStreamer523csbProcessElementSwitchEPK28SGfxContextStatusBufferEntry",0x3C5D8},
  {"__ZN26IGHardwareCommandStreamer522csbProcessActiveToIdleEPK28SGfxContextStatusBufferEntry",0x3C854},
  {"__ZN16IntelAccelerator13SafeForceWakeEbj",0x28C2A},
  {"__ZN16IntelAccelerator22SafeForceWakeInterruptEbj",0x2DA26},
  {"__ZN16IntelAccelerator17initModeRegistersEv",0x28E1A},
  {"__ZN17IGInterruptBridge15systemWillSleepEv",0x4AB14},
  {"__ZN17IGInterruptBridge17disableInterruptsEv",0x4A778},
 };
 // Scenario: 0 = success; 1 = counted ForceWake hook failure;
 // 2...5 = Nth CSB route failure after earlier routes were applied;
 // 6 = initial reset patch fails before writing;
 // 7 = initial reset patch reports failure AFTER writing;
 // 8 = initial mode patch fails before writing;
 // 9 = initial mode patch reports failure AFTER writing;
 // 10 = rollback fails and must trip fatal guard;
 // 11 = unrecognized donor code (fail without writing/rollback);
 // 12 = partially corrupted text patch (fatal: cannot restore safely);
 // 13 = rollback bytes restored, but Lilu reports failure (also fatal);
 // 14 = host IRQ entry not qualified, 15 = S0 sleep hook route fails,
 // 16 = shared IRQ + sleep route succeeds, 17 = missing host trampoline,
 // 18 = unexpected sleep entry; core CSB12 remains valid.
 int scenario=0;
 int csbRoutesInstalled=0, lookupCalls=0, restoreCalls=0;
 std::vector<std::string> operations;
 template<class T>
 T solveSymbol(size_t,const char *name,mach_vm_address_t address,size_t) {
  auto it=symbolOffsets.find(name);assert(it!=symbolOffsets.end());
  operations.emplace_back("solve");
  return static_cast<T>(address+it->second);
 }
 bool routeMultipleShort(size_t,RouteRequest *req,size_t n,mach_vm_address_t,size_t) {
  assert(n==1);operations.emplace_back("wakeRoute");
  if(scenario==1) return false;
  if(req[0].org)*req[0].org=0xABCDEF;
  return true;
 }
 bool routeMultiple(size_t,RouteRequest *req,size_t n,mach_vm_address_t,size_t) {
  if(n==1) {
    const std::string symbol=req[0].symbol;
    assert(symbol=="__ZN17IGInterruptBridge15systemWillSleepEv");
    operations.emplace_back("sleepRoute");
    if(scenario==15)return false;
    if(req[0].org)*req[0].org=0xABCDEF;
    return true;
  }
  assert(n==4);operations.emplace_back("csbRoute");
  for(size_t i=0;i<n;++i) {
   if(scenario>=2 && scenario<=5 && int(i)+2==scenario) return false;
   ++csbRoutesInstalled;
   if(req[i].org)*req[i].org=0xABCDEF;
  }
  return true;
 }
 void applyLookupPatch(const LookupPatch *p,uint8_t *at,size_t) {
  assert(p->count==1);
  const bool restoring=(p->replace[0]==0xB8 && p->replace[1]==7) ||
                       (p->replace[0]==0xC7 && p->replace[3]==0);
  if(restoring) {
   ++restoreCalls;operations.emplace_back("restore");
   if(scenario==10)return;
  } else {
   ++lookupCalls;operations.emplace_back(lookupCalls==1?"resetPatch":"modePatch");
   if(scenario==6 && lookupCalls==1)return;
   if(scenario==8 && lookupCalls==2)return;
   if(scenario==12 && lookupCalls==2) {
     at[0]=0xFF;error=Error::WriteFailure;return;
   }
  }
  assert(std::memcmp(at,p->find,p->size)==0);
  std::memcpy(at,p->replace,p->size);
  if(restoring && scenario==13)error=Error::WriteFailure;
  if(!restoring && (scenario==7 && lookupCalls==1 ||
                    (scenario==9 || scenario==10 || scenario==13) && lookupCalls==2))
     error=Error::WriteFailure;
 }
 Error getError() const {return error;}
 void clearError() {error=Error::NoError;}
};

struct XE {
 bool adlnDetected=false;
 mach_vm_address_t orgTGLHostIRQClassDisable{};
 mach_vm_address_t orgTGLCSBInit{},orgTGLCSBWake{},orgTGLCSBReset{},orgTGLCSBProcess{};
 static bool wrapTGLCSBInit(void *,void *,void *,void *,unsigned) {return true;}
 static void wrapTGLCSBWake(void *) {}
 static void wrapTGLCSBReset(void *) {}
 static void wrapTGLCSBProcess(void *,uint32_t) {}
 bool installTGLCSB12(KernelPatcher &,size_t,mach_vm_address_t,size_t,bool);
};
@PRODUCTION_INSTALL@

int main() {
 const std::array<uint8_t,7> modeFind {0xC7,0x04,0x30,0,0,8,0};
 const std::array<uint8_t,7> modeReplace {0xC7,0x04,0x30,8,0,8,0};
 const std::array<uint8_t,11> resetFind {0xB8,7,0,0,0,0x89,0x87,4,0x1C,0,0};
 const std::array<uint8_t,11> resetReplace {0xB8,11,0,0,0,0x89,0x87,4,0x1C,0,0};
 const std::array<uint8_t,14> wakeEntry {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50};
 for(int scenario=0;scenario<=18;++scenario) {
  s0Requested=scenario>=14;
  tglCSB12Enabled=false;
  tglS0SleepHookRouted=false;
  IORegistryEntry::getRegistryRoot()->reset();
  XE driver;KernelPatcher patcher;patcher.scenario=scenario;
  std::vector<uint8_t> image(0x4B000,0);
  auto base=reinterpret_cast<uintptr_t>(image.data());
  std::memcpy(image.data()+0x28C2A,wakeEntry.data(),wakeEntry.size());
  std::memcpy(image.data()+0x3A396,wakeEntry.data(),wakeEntry.size());
  const std::array<uint8_t,9> sleepEntry {0x55,0x48,0x89,0xE5,0x53,0x50,0x48,0x89,0xFB};
  std::memcpy(image.data()+0x4AB14,sleepEntry.data(),sleepEntry.size());
  // Host routing already rewrote the entry, as in the real installation.
  std::memset(image.data()+0x4A778,0xCC,sleepEntry.size());
  driver.orgTGLHostIRQClassDisable=scenario==17?0:0xABCDEF;
  if(scenario==18)image[0x4AB14]=0xCC;
  image[0x28C9A]=0xE8;image[0x28E14]=0xE9;
  std::memcpy(image.data()+0x28E45,modeFind.data(),modeFind.size());
  std::memcpy(image.data()+0x3A3A4,resetFind.data(),resetFind.size());
  if(scenario==11)image[0x28E45]=0xFF;
  const auto check=[&](size_t offset,const auto &bytes) {
   return std::memcmp(image.data()+offset,bytes.data(),bytes.size())==0;
  };
  bool ok=false,panicked=false;
  try {ok=driver.installTGLCSB12(patcher,7,base,image.size(),scenario!=14);}
  catch(const std::runtime_error &) {panicked=true;}
  if(scenario==0) {
    assert(ok && tglCSB12Enabled && !panicked);
    assert(check(0x28E45,modeReplace) && check(0x3A3A4,resetReplace));
    assert(patcher.csbRoutesInstalled==4 && patcher.lookupCalls==2);
    assert(patcher.operations.end()!=std::find(patcher.operations.begin(),patcher.operations.end(),"csbRoute"));
    std::cout<<"PASS successful commit of exact production installer\n";
  } else if(scenario>=14) {
    assert(ok && tglCSB12Enabled && !panicked);
    assert(tglS0SleepHookRouted==(scenario==16));
    assert(std::count(patcher.operations.begin(),patcher.operations.end(),"sleepRoute")== (scenario==15 || scenario==16));
    assert(image[0x4A778]==0xCC);
    assert(check(0x28E45,modeReplace) && check(0x3A3A4,resetReplace));
    std::cout<<"PASS S0 opt-in route transaction "<<scenario
             <<", snapshotHookReady="<<tglS0SleepHookRouted<<"\n";
  } else if(scenario==10 || scenario==12 || scenario==13) {
    assert(panicked);
    std::cout<<"PASS irreversible/partial text failure is fatal (no false success)\n";
  } else if(scenario==11) {
    assert(!ok && !panicked && !tglCSB12Enabled);
    assert(check(0x3A3A4,resetFind) && image[0x28E45]==0xFF);
    assert(patcher.csbRoutesInstalled==0 && patcher.lookupCalls==0);
    std::cout<<"PASS unsupported donor fails preflight without text writes\n";
  } else {
    assert(!ok && !tglCSB12Enabled && !panicked);
    assert(check(0x28E45,modeFind) && check(0x3A3A4,resetFind));
    assert(!IORegistryEntry::getRegistryRoot()->flags["TGLCSB12Routed"]);
    std::cout<<"PASS injected failure "<<scenario<<" restores native donor text\n";
  }
 }
 return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="wex-csb-install-") as work:
    source = Path(work) / "install.cpp"
    source.write_text(HARNESS.replace("@PRODUCTION_INSTALL@", INSTALL))
    exe = Path(work) / "install"
    subprocess.run(
        ["clang++", "-std=c++17", "-O1", "-g",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         str(source), "-o", str(exe)],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
