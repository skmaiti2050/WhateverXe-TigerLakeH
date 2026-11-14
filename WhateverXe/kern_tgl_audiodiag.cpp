// Tiger Lake HDMI/DP audio diagnostics (port of jkbuha/WhateverXe-contrib 4bd5cb9,
// which targets the ADL-H Display13 path; see b00t0x/WhateverXe#124).
//
// Inactive unless -wex-audio-diag (read-only) or -wex-audio-wake is set. Read-only unless -wex-audio-wake:
//  - WEXTGLAudio (registry root, read on demand): pass-through call counters on
//    Apple's display-audio entry points and the display-side audio registers.
//    The ELD FIFO (AUD_HDMIW_HDMIEDID) is never read: reading advances it.
//  - WEXHDAReadout (registry root, read on demand): HDA controller status,
//    capability chain and CORB/RIRB state. Never writes.
//  - -wex-audio-wake (opt-in): i915 codec_wake_override on HSW_AUD_CHICKENBIT
//    (0x65F10 bit 15: clear, 1 ms, set, 1 ms) from processKernel, i.e. before the
//    HDA controller's link reset. Original bit restored after 300 s.
//    Power wells (PWR_WELL_CTL2) are NEVER written here.
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <kern/thread_call.h>

namespace {
volatile uint32_t audEnableCalls,audDisableCalls,audSupportCalls,audSupportTrue,audPropsCalls,audBufDisableCalls;
volatile uint64_t audEnableRet,audDisableRet;
mach_vm_address_t orgAudEnable,orgAudDisable,orgAudSupport,orgAudProps,orgAudBufDisable;

uint64_t wrapAudEnable(void *c,void *fb,void *path) {
 __atomic_add_fetch(&audEnableCalls,1U,__ATOMIC_RELAXED);
 const auto r=FunctionCast(wrapAudEnable,orgAudEnable)(c,fb,path);audEnableRet=r;return r;
}
uint64_t wrapAudDisable(void *c,void *fb,void *path) {
 __atomic_add_fetch(&audDisableCalls,1U,__ATOMIC_RELAXED);
 const auto r=FunctionCast(wrapAudDisable,orgAudDisable)(c,fb,path);audDisableRet=r;return r;
}
bool wrapAudSupport(void *fb,uint8_t *edid) {
 __atomic_add_fetch(&audSupportCalls,1U,__ATOMIC_RELAXED);
 const bool r=FunctionCast(wrapAudSupport,orgAudSupport)(fb,edid);
 if(r)__atomic_add_fetch(&audSupportTrue,1U,__ATOMIC_RELAXED);
 return r;
}
uint64_t wrapAudProps(void *fb) {
 __atomic_add_fetch(&audPropsCalls,1U,__ATOMIC_RELAXED);
 return FunctionCast(wrapAudProps,orgAudProps)(fb);
}
uint64_t wrapAudBufDisable(void *c) {
 __atomic_add_fetch(&audBufDisableCalls,1U,__ATOMIC_RELAXED);
 return FunctionCast(wrapAudBufDisable,orgAudBufDisable)(c);
}

IOMemoryMap *gpuMap;   // own uncached BAR0 mapping of the iGPU
volatile uint8_t *gpuBase;

// WEXTGLAudio: {version, reads, valid, enableCalls, enableLastRet, disableCalls, disableLastRet,
//  supportCalls, supportTrue, propsCalls, bufDisableCalls,
//  AUDIO_PIN_BUF_CTL 0x48414, AUD_PIN_ELD_CP_VLD 0x650C0, AUD_CHICKENBIT 0x65F10,
//  AUD_FREQ_CNTRL 0x65900, PWR_WELL_CTL1 0x45400, PWR_WELL_CTL2 0x45404, PWR_WELL_CTL4 0x4540C,
//  DC_STATE_EN 0x45504, AUD_CONFIG A..D 0x65000+0x100*t, AUD_CNTL_ST A..D 0x650B4+0x100*t,
//  AUD_CONFIG_BE 0x65EF0}
bool audioSerialize(void *,void *,OSSerialize *serializer) {
 static uint32_t reads=0;uint32_t q[11+8+8+1] {1};
 q[1]=__atomic_add_fetch(&reads,1U,__ATOMIC_RELAXED);
 q[3]=audEnableCalls;q[4]=static_cast<uint32_t>(audEnableRet);q[5]=audDisableCalls;q[6]=static_cast<uint32_t>(audDisableRet);
 q[7]=audSupportCalls;q[8]=audSupportTrue;q[9]=audPropsCalls;q[10]=audBufDisableCalls;
 if(q[1]<4096 && gpuBase) {
  auto rd=[](uint32_t r){return *reinterpret_cast<volatile uint32_t *>(gpuBase+r);};
  static constexpr uint32_t regs[] {0x48414,0x650C0,0x65F10,0x65900,0x45400,0x45404,0x4540C,0x45504};
  q[2]=1;for(unsigned i=0;i<8;++i)q[11+i]=rd(regs[i]);
  for(unsigned t=0;t<4;++t) {q[19+t]=rd(0x65000+0x100*t);q[23+t]=rd(0x650B4+0x100*t);}
  q[27]=rd(0x65EF0);
 }
 auto data=OSData::withBytes(q,sizeof(q));if(!data)return false;
 const bool ok=data->serialize(serializer);data->release();return ok;
}

// -wex-audio-wake. WEXTGLAudioWake: {version, stage, before, afterSet, afterRestore, setNs, restoreNs}
// stage: 4 = pulsed from processKernel, 3 = restored, 8 = no BAR0 map, 9 = register reads all-ones.
uint32_t wakeOrig;uint64_t wakeRec[7] {1};thread_call_t wakeRestoreCall;
void wakePublish() {IORegistryEntry::getRegistryRoot()->setProperty("WEXTGLAudioWake",wakeRec,sizeof(wakeRec));}
void wakeRestore(thread_call_param_t,thread_call_param_t) {
 if(!gpuBase)return;
 auto reg=reinterpret_cast<volatile uint32_t *>(gpuBase+0x65F10);
 *reg=(*reg&~(1U<<15))|(wakeOrig&(1U<<15));
 wakeRec[1]=3;wakeRec[4]=*reg;uint64_t t;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&wakeRec[6]);
 wakePublish();
}
void audioWake() {
 if(!gpuBase) {wakeRec[1]=8;wakePublish();return;}
 auto reg=reinterpret_cast<volatile uint32_t *>(gpuBase+0x65F10);
 wakeOrig=*reg;wakeRec[2]=wakeOrig;
 if(wakeOrig==UINT32_MAX) {wakeRec[1]=9;wakePublish();return;}
 *reg=wakeOrig&~(1U<<15);IODelay(1000);
 *reg=(*reg)|(1U<<15);IODelay(1000);
 wakeRec[1]=4;wakeRec[3]=*reg;uint64_t t;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&wakeRec[5]);
 wakePublish();
 wakeRestoreCall=thread_call_allocate(wakeRestore,nullptr);
 uint64_t deadline;clock_interval_to_deadline(300,NSEC_PER_SEC,&deadline);
 if(wakeRestoreCall)thread_call_enter_delayed(wakeRestoreCall,deadline);
}

// Read-only HDA controller readout (Intel, PCI class 0x0403 or DSP-capable 0x0401).
// WEXHDAReadout: {version, reads, valid, vendorDevice, GCAP|VMIN<<16|VMAJ<<24, GCTL,
//  WAKEEN|STATESTS<<16, INTCTL, LLCH, mlOffset, links, ppOffset, PPCTL,
//  [13..28] 0x1000..0x103C, [29..60] per link i<8: LCAP, LCTL, LOSIDV, LSDIID,
//  [61..72] CORBLBASE, CORBUBASE, CORBWP|CORBRP<<16, CORBCTL|STS|SIZE, RIRBLBASE, RIRBUBASE,
//  RIRBWP|RINTCNT<<16, RIRBCTL|STS|SIZE, ICIS, SSYNC 0x18, INTSTS 0x24, WALCLK 0x30,
//  [73..120] raw 0x500..0x53C, 0x800..0x83C, 0xC00..0xC3C}
IOMemoryMap *hdaMap;IOPCIDevice *hdaDevice;
IOPCIDevice *findHDA() {
 auto match=IOService::serviceMatching("IOPCIDevice");if(!match)return nullptr;
 IOPCIDevice *found=nullptr;
 if(auto it=IOService::getMatchingServices(match)) {
  while(auto o=it->getNextObject()) {
   auto pci=OSDynamicCast(IOPCIDevice,o);if(!pci)continue;
   auto cc=OSDynamicCast(OSData,pci->getProperty("class-code"));auto vid=OSDynamicCast(OSData,pci->getProperty("vendor-id"));
   if(cc && cc->getLength()>=4 && vid && vid->getLength()>=2) {
    auto c=static_cast<const uint8_t *>(cc->getBytesNoCopy());auto v=static_cast<const uint8_t *>(vid->getBytesNoCopy());
    if(c[2]==0x04 && (c[1]==0x03 || c[1]==0x01) && v[0]==0x86 && v[1]==0x80) {found=pci;found->retain();break;}
   }
  }
  it->release();
 }
 match->release();return found;
}
bool hdaSerialize(void *,void *,OSSerialize *serializer) {
 static uint32_t reads=0;uint32_t q[13+16+32+12+48] {2};
 q[1]=__atomic_add_fetch(&reads,1U,__ATOMIC_RELAXED);
 if(q[1]<4096) {
  if(!hdaDevice)hdaDevice=findHDA();
  if(hdaDevice && !hdaMap)hdaMap=hdaDevice->mapDeviceMemoryWithRegister(0x10,kIOMapInhibitCache);
  if(hdaMap && hdaMap->getLength()>=0x2000 && hdaMap->getVirtualAddress()) {
   auto b=reinterpret_cast<volatile uint8_t *>(hdaMap->getVirtualAddress());
   auto r32=[&](uint32_t o){return *reinterpret_cast<volatile uint32_t *>(b+o);};
   auto r16=[&](uint32_t o){return static_cast<uint32_t>(*reinterpret_cast<volatile uint16_t *>(b+o));};
   q[2]=1;q[3]=hdaDevice->configRead32(0);
   q[4]=r16(0)|(static_cast<uint32_t>(b[2])<<16)|(static_cast<uint32_t>(b[3])<<24);q[5]=r32(0x08);q[6]=r16(0x0C)|(r16(0x0E)<<16);
   q[7]=r32(0x20);q[8]=r32(0x14);
   uint32_t off=q[8]&0xFFFF;unsigned guard=0;
   while(off && off<0x2000-0x200 && guard++<10) {
    const auto hdr=r32(off);const auto id=(hdr>>16)&0xFFF;
    if(id==1 && !q[9])q[9]=off;
    if(id==3 && !q[11]) {q[11]=off;q[12]=r32(off+0x04);}
    const auto next=hdr&0xFFFF;if(next==off)break;off=next;
   }
   for(unsigned i=0;i<16;++i)q[13+i]=r32(0x1000+4*i);
   q[61]=r32(0x40);q[62]=r32(0x44);q[63]=r16(0x48)|(r16(0x4A)<<16);q[64]=r32(0x4C);
   q[65]=r32(0x50);q[66]=r32(0x54);q[67]=r16(0x58)|(r16(0x5A)<<16);q[68]=r32(0x5C);
   q[69]=r32(0x68);q[70]=r32(0x18);q[71]=r32(0x24);q[72]=r32(0x30);
   for(unsigned i=0;i<16;++i) {q[73+i]=r32(0x500+4*i);q[89+i]=r32(0x800+4*i);q[105+i]=r32(0xC00+4*i);}
   if(q[9]) {
    q[10]=(r32(q[9]+0x04)&0xF)+1;
    for(unsigned l=0;l<8 && l<q[10];++l) {
     const auto lb=q[9]+0x40+0x40*l;if(lb+0x10>=0x2000)break;
     q[29+4*l]=r32(lb);q[30+4*l]=r32(lb+4);q[31+4*l]=r32(lb+8);q[32+4*l]=r32(lb+0xC);
    }
   }
  }
 }
 auto data=OSData::withBytes(q,sizeof(q));if(!data)return false;
 const bool ok=data->serialize(serializer);data->release();return ok;
}
bool audioDiagRequested() {return checkKernelArgument("-wex-audio-diag") || checkKernelArgument("-wex-audio-wake");}
} // namespace

// Called from XE::processKernel on TGL. Maps BAR0 (read-only use unless -wex-audio-wake),
// publishes the on-demand readouts, and pulses the codec wake if requested.
void tglAudioDiagEarly(IOPCIDevice *gpu) {
 if(!audioDiagRequested())return;
 if(gpu && !gpuMap) {
  auto map=gpu->mapDeviceMemoryWithRegister(0x10,kIOMapInhibitCache);
  if(map && map->getLength()>=0x80000 && map->getVirtualAddress()) {gpuMap=map;gpuBase=reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());}
  else OSSafeReleaseNULL(map);
 }
 auto root=IORegistryEntry::getRegistryRoot();
 if(auto s=OSSerializer::forTarget(nullptr,audioSerialize)) {root->setProperty("WEXTGLAudio",s);s->release();}
 if(auto s=OSSerializer::forTarget(nullptr,hdaSerialize)) {root->setProperty("WEXHDAReadout",s);s->release();}
 if(checkKernelArgument("-wex-audio-wake"))audioWake();
}

// Called from XE::processFramebufferKext on the TGL backend: pass-through counters only.
bool tglAudioProbeInstall(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size) {
 if(!audioDiagRequested())return false;
 KernelPatcher::RouteRequest routes[] {
  {"__ZN31AppleIntelFramebufferController13hwEnableAudioEP21AppleIntelFramebufferP21AppleIntelDisplayPath",wrapAudEnable,orgAudEnable},
  {"__ZN31AppleIntelFramebufferController14hwDisableAudioEP21AppleIntelFramebufferP21AppleIntelDisplayPath",wrapAudDisable,orgAudDisable},
  {"__ZN21AppleIntelFramebuffer23IsDisplayAudioSupportedEPh",wrapAudSupport,orgAudSupport},
  {"__ZN21AppleIntelFramebuffer18setAudioPropertiesEv",wrapAudProps,orgAudProps},
  {"__ZN31AppleIntelFramebufferController20hwDisableAudioBufferEv",wrapAudBufDisable,orgAudBufDisable}};
 const bool ready=patcher.routeMultiple(index,routes,arrsize(routes),address,size);patcher.clearError();
 IORegistryEntry::getRegistryRoot()->setProperty("WEXTGLAudioProbeReady",ready);
 return ready;
}
