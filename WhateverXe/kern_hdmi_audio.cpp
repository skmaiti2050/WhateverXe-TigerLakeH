// HDMI/DP audio for the Gen12 (TGL/ADL-P) Intel display codec through AppleHDA
// (-wex-hdmi-audio). Tested on ADL-P 46A6 with the iDisp codec 0x8086281C and
// AppleHDA loaded by SimpleHDA on macOS 26.
//
// The codec answers on the HDA link, but nothing in AppleHDA or Apple's
// framebuffer builds a working output for it:
//  1 the codec only enumerates after the display-side codec wake;
//  2 Apple's framebuffer leaves the external display's audio flag clear;
//  3 AppleHDA's function-group factory maps only 0x80862805-0x8086280B to Intel
//    classes, so 281C gets a generic group that refuses discovery;
//  4 with discovery allowed, the generic walker rejects the ADL-P topology and
//    the generic widget masks the converters' PCM capabilities to 0;
//  5 the framebuffer advertises its own port numbering in audio-codec-info,
//    not the codec pin the DDI is routed to;
//  6 the framebuffer enables audio output for the transcoder but never writes
//    an ELD;
//  7 nothing selects the streaming converter on the pin or sends the DP audio
//    InfoFrame when playback starts.
// Each step below follows what Linux i915/snd-hda-intel do for the same hardware.
//
// Known limitation: after the display is unplugged and replugged, the picture
// returns but the codec does not re-enumerate on the HDA link until reboot.
#include "kern_hdmi_audio.hpp"
#include <Headers/kern_util.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_compat.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/clock.h>
#include <kern/thread_call.h>

// AppleHDA kexts are linked at runtime by SimpleHDA. Lilu resolves their symbols
// but cannot route them (routeMultiple is a silent no-op, routeFunction reports
// DisasmFailure). Hook by hand: each target's prologue is a fixed,
// RIP-independent push/mov/sub sequence, checked byte for byte against the live
// code before anything is written. A naked stub here re-executes that prologue
// and jumps to target+len; the target's first 14 bytes become an absolute jmp to
// the wrapper. Mismatch => no write, the hook is reported as failed.
#define WEX_HDA_STUB(name,...) \
 extern "C" uint64_t wexHdaRet_##name; uint64_t wexHdaRet_##name; \
 extern "C" void wexHdaStub_##name(); \
 __asm__(".text\n.globl _wexHdaStub_" #name "\n_wexHdaStub_" #name ":\n.byte " #__VA_ARGS__ "\njmpq *_wexHdaRet_" #name "(%rip)\n");
WEX_HDA_STUB(handler,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x18,0x03,0x00,0x00)
WEX_HDA_STUB(edid,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38)
WEX_HDA_STUB(cgstart,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28)
WEX_HDA_STUB(engpstart,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x58)
WEX_HDA_STUB(reset,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38)

namespace {
void publish(const char *key,const void *bytes,unsigned length) {
 IORegistryEntry::getRegistryRoot()->setProperty(key,const_cast<void *>(bytes),length);
}

// ---- 1. Display-side codec wake -------------------------------------------
// Before AppleHDA probes its link: set AUDIO_PIN_BUF_CTL bit 31 (0x48414) and
// pulse the codec-wake override (0x65F10 bit 15), as i915 does around
// intel_audio_codec_enable on Gen12; the override is restored after 300 s.
// WEXHDMIAudioWake: {version, stage (2 FB start, 3 restored, 4 processKernel, 8 no
//  mapping, 9 unreadable), 0x65F10 before, after, after restore, set ns, restore ns}.
// WEXHDMIAudioPinBuf: {AUDIO_PIN_BUF_CTL before, after}.
volatile uint8_t *wakeBase;
uint32_t wakeOrig,pinBuf[2];
uint64_t wakeRec[7] {1};
thread_call_t wakeRestoreCall;
IOMemoryMap *wakeEarlyMap;
void wakeRestore(thread_call_param_t,thread_call_param_t) {
 if(!wakeBase)return;
 auto reg=reinterpret_cast<volatile uint32_t *>(wakeBase+0x65F10);
 *reg=(*reg&~(1U<<15))|(wakeOrig&(1U<<15));
 wakeRec[1]=3;wakeRec[4]=*reg;uint64_t t;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&wakeRec[6]);
 publish("WEXHDMIAudioWake",wakeRec,sizeof(wakeRec));
}
void wakeAt(volatile uint8_t *base,uint64_t stage) {
 auto buf=reinterpret_cast<volatile uint32_t *>(base+0x48414);
 pinBuf[0]=*buf;if(pinBuf[0]!=UINT32_MAX)*buf=pinBuf[0]|0x80000000U;pinBuf[1]=*buf;
 publish("WEXHDMIAudioPinBuf",pinBuf,sizeof(pinBuf));
 auto reg=reinterpret_cast<volatile uint32_t *>(base+0x65F10);
 wakeOrig=*reg;wakeRec[2]=wakeOrig;
 if(wakeOrig==UINT32_MAX) {wakeRec[1]=9;publish("WEXHDMIAudioWake",wakeRec,sizeof(wakeRec));return;}
 wakeBase=base;
 *reg=wakeOrig&~(1U<<15);IODelay(1000);
 *reg=(*reg)|(1U<<15);IODelay(1000);
 wakeRec[1]=stage;wakeRec[3]=*reg;uint64_t t;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&wakeRec[5]);
 publish("WEXHDMIAudioWake",wakeRec,sizeof(wakeRec));
 wakeRestoreCall=thread_call_allocate(wakeRestore,nullptr);
 uint64_t deadline;clock_interval_to_deadline(300,NSEC_PER_SEC,&deadline);
 if(wakeRestoreCall)thread_call_enter_delayed(wakeRestoreCall,deadline);
}

// ---- 2. External display audio flag ---------------------------------------
// AppleIntelFramebuffer::setupDisplayResources takes the audio flag (fb+0x424c)
// from the display's AGDC resource configuration. Without AGDC policy it stays
// 0, so hwSetMode never enables pipe audio and setAudioProperties publishes
// audio-codec-info = 0. For an external display with the flag clear, read the
// first EDID extension through Apple's fbGetDDCBlock and set it only for a CEA
// block with basic audio and an HDMI vendor block. Apple runs setAudioProperties
// before this point and not again, so re-run it once the flag is set.
// WEXHDMIAudioFB: per FB index 0..3 {calls, external, flag before, hdmiSink, converter,
//  ddcResult | audio-codec-info<<32, CEA flags (1 basic audio, 2 HDMI VSDB),
//  applied(1) | propsRerun(2) | resource audio<<8 | entry<<40}.
mach_vm_address_t orgSetupRes,fbGetDDC,fbSetAudioProps;
uint64_t fbRec[4][8];
uint64_t wrapSetupRes(void *fb) {
 const auto r=FunctionCast(wrapSetupRes,orgSetupRes)(fb);
 auto object=static_cast<OSObject *>(fb);
 if(!fb || object->getMetaClass()->getClassSize()<0x4A10)return r;
 auto p=static_cast<uint8_t *>(fb);
 const auto idx=*reinterpret_cast<uint32_t *>(p+0x1DC);
 uint64_t scratch[8] {};auto &rec=idx<4 ? fbRec[idx] : scratch;
 const auto entry=*reinterpret_cast<int32_t *>(p+0x3FD8);
 rec[0]++;rec[1]=p[0x1E2];rec[2]=p[0x424C];rec[3]=p[0x4289];rec[4]=p[0x428A];
 rec[7]=static_cast<uint64_t>(static_cast<uint32_t>(entry))<<40;
 if(entry>=0 && entry<16)rec[7]|=static_cast<uint64_t>(*reinterpret_cast<uint32_t *>(p+0x225+0x3DC*entry))<<8;
 if(p[0x1E2] && !p[0x424C] && fbGetDDC) {
  auto path=*reinterpret_cast<void **>(p+(entry==-1 ? 0x49F0 : 0x4A08));
  uint8_t ext[128] {};uint64_t length=sizeof(ext);
  using DDC=uint32_t (*)(void *,uint32_t,uint8_t *,uint64_t *,uint32_t,void *);
  const auto res=path ? reinterpret_cast<DDC>(fbGetDDC)(fb,2,ext,&length,0,path) : 0xE00002C0U;
  rec[5]=res;uint64_t flags=0;
  if(res==0 && length>=128 && ext[0]==2 && ext[1]>=3) {
   if(ext[3]&0x40)flags|=1;
   const unsigned end=ext[2]<=127 ? ext[2] : 127;
   for(unsigned i=4;i<end && i<127;) {
    const unsigned tag=ext[i]>>5,len=ext[i]&31;
    if(tag==3 && len>=3 && i+3<128 && ext[i+1]==0x03 && ext[i+2]==0x0C && ext[i+3]==0x00)flags|=2;
    i+=1+len;
   }
  }
  rec[6]=flags;
  if(flags==3) {p[0x424C]=1;rec[7]|=1;}
 }
 if(p[0x1E2] && p[0x424C] && fbSetAudioProps) {
  reinterpret_cast<uint64_t (*)(void *)>(fbSetAudioProps)(fb);
  uint32_t info=0;
  if(auto d=OSDynamicCast(OSData,static_cast<IOService *>(fb)->getProperty("audio-codec-info")))
   if(d->getLength()>=4)info=*static_cast<const uint32_t *>(d->getBytesNoCopy());
  rec[7]|=2;rec[5]=(rec[5]&0xFFFFFFFFULL)|(static_cast<uint64_t>(info)<<32);
 }
 publish("WEXHDMIAudioFB",fbRec,sizeof(fbRec));
 return r;
}

// ---- Manual AppleHDA hooks --------------------------------------------------
struct HookSpec {const char *symbol;const uint8_t *prologue;size_t len;void (*stub)();uint64_t *ret;};
const uint8_t pro20[] {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x18,0x03,0x00,0x00};
const uint8_t pro17_38[] {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38};
const uint8_t pro17_28[] {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28};
const uint8_t pro17_58[] {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x58};
const HookSpec hookSpecs[] {
 {"__ZN18AppleHDAController23framebufferEventHandlerEPvP13IOFramebufferiS0_",pro20,sizeof(pro20),wexHdaStub_handler,&wexHdaRet_handler},
 {"__ZN18AppleHDAController19edidIsValidForAudioEP6OSDatai",pro17_38,sizeof(pro17_38),wexHdaStub_edid,&wexHdaRet_edid},
 {"__ZN20AppleHDACodecGeneric5startEP9IOService",pro17_28,sizeof(pro17_28),wexHdaStub_cgstart,&wexHdaRet_cgstart},
 {"__ZN14AppleHDAEngine23performAudioEngineStartEv",pro17_58,sizeof(pro17_58),wexHdaStub_engpstart,&wexHdaRet_engpstart},
 {"__ZN18AppleHDAController20resetAudioControllerEbb",pro17_38,sizeof(pro17_38),wexHdaStub_reset,&wexHdaRet_reset},
};
// WEXHDMIAudioHooks: {attempts, then per attempt: 1 hooked | 2 symbol missing | 3 prologue mismatch | 4 write failed}.
uint64_t hookRec[9];
bool hookAddress(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size,
 const char *symbol,void *wrapper,mach_vm_address_t &org) {
 const auto n=hookRec[0]<8 ? ++hookRec[0] : 8;auto &rec=hookRec[n];
 struct Publish {~Publish(){publish("WEXHDMIAudioHooks",hookRec,sizeof(hookRec));}} done;
 const HookSpec *spec=nullptr;
 for(auto &h:hookSpecs)if(!strcmp(h.symbol,symbol))spec=&h;
 const auto from=patcher.solveSymbol<mach_vm_address_t>(index,symbol,address,size);patcher.clearError();
 if(!spec || !from) {rec=2;return false;}
 if(memcmp(reinterpret_cast<const void *>(from),spec->prologue,spec->len)) {rec=3;return false;}
 *spec->ret=from+spec->len;
 uint8_t jump[14] {0xFF,0x25,0,0,0,0};
 const auto target=reinterpret_cast<uint64_t>(wrapper);memcpy(jump+6,&target,sizeof(target));
 if(MachInfo::setKernelWriting(true,KernelPatcher::kernelWriteLock)!=KERN_SUCCESS) {rec=4;return false;}
 lilu_os_memcpy(reinterpret_cast<void *>(from),jump,sizeof(jump));
 MachInfo::setKernelWriting(false,KernelPatcher::kernelWriteLock);
 if(memcmp(reinterpret_cast<const void *>(from),jump,sizeof(jump))) {rec=4;return false;}
 org=reinterpret_cast<mach_vm_address_t>(spec->stub);rec=1;return true;
}

// ---- Codec commands through AppleHDAController::executeCodecCommand ---------
mach_vm_address_t execCmdFn;
IOService *findChildByClass(IOService *parent,const char *cls) {
 IOService *found=nullptr;
 if(auto it=parent->getChildIterator(gIOServicePlane)) {
  while(auto o=it->getNextObject()) {
   auto svc=OSDynamicCast(IOService,o);
   if(svc && svc->getMetaClass() && !strcmp(svc->getMetaClass()->getClassName(),cls)) {found=svc;break;}
  }
  it->release();
 }
 return found;
}
bool codecCommand(uint32_t nid,uint32_t verb,uint32_t payload,uint32_t &resp) {
 resp=0xFFFFFFFF;
 if(!execCmdFn)return false;
 IOService *controller=nullptr;
 if(auto match=IOService::serviceMatching("AppleHDAController")) {
  if(auto it=IOService::getMatchingServices(match)) {controller=OSDynamicCast(IOService,it->getNextObject());if(controller)controller->retain();it->release();}
  match->release();
 }
 if(!controller)return false;
 bool ok=false;
 if(auto device=findChildByClass(controller,"IOHDACodecDevice")) {
  uint32_t cad=2;
  if(auto n=OSDynamicCast(OSNumber,device->getProperty("IOHDACodecAddress")))cad=n->unsigned32BitValue()&0xF;
  using Exec=IOReturn (*)(IOService *,IOService *,uint32_t,uint32_t *);
  ok=reinterpret_cast<Exec>(execCmdFn)(controller,device,(cad<<28)|((nid&0xFF)<<20)|((verb&0xFFF)<<8)|(payload&0xFF),&resp)==kIOReturnSuccess;
 }
 controller->release();
 return ok;
}
bool codecGet(uint32_t nid,uint32_t verb,uint32_t payload,uint32_t &resp) {
 // GET verbs: 0xF00 parameter, 0xF01 conn select, 0xF02 conn list, 0xF06 stream,
 // 0xF07 pin ctl, 0xF09 pin sense, 0xF1C config default, 0xF81 Intel vendor,
 // 0xA00 converter format, 0xBA0 output amp, 0xF2D channel count, 0xF32 DIP transmit.
 return codecCommand(nid,verb,payload,resp);
}
bool codecSet(uint32_t nid,uint32_t verb,uint32_t payload) {
 // SET verbs used only by the steps below (Linux hdmi_init_pin, hdmi_setup_audio_infoframe,
 // intel_haswell_enable_all_pins): 0x701 conn select, 0x707 pin ctl, 0x3B0 amp unmute,
 // 0x72D channel count, 0x734 ASP map, 0x730/0x731/0x732 DIP index/data/transmit, 0x781 vendor.
 switch(verb){case 0x701:case 0x707:case 0x3B0:case 0x72D:case 0x734:case 0x730:case 0x731:case 0x732:case 0x781:break;default:return false;}
 uint32_t resp=0;
 return codecCommand(nid,verb,payload,resp);
}

// Widget table of the Intel display codec, read once when discovery reaches it.
// WEXHDMIAudioCodec: [0] status (1 done | 2 no exec fn | 3 no controller, codec or answer),
// [1] codec address, [2] root subordinate, [3] AFG subordinate, [4] Intel vendor reg 0xF81,
// [5] reserved, then per node (max 24): {nid, widget caps, pin config or 0, pin caps or 0}.
uint32_t codecTable[6+24*4];
bool codecProbed;
void probeCodec() {
 if(codecProbed)return;codecProbed=true;
 struct Publish {~Publish(){publish("WEXHDMIAudioCodec",codecTable,sizeof(codecTable));}} done;
 if(!execCmdFn) {codecTable[0]=2;return;}
 codecGet(0,0xF00,0x04,codecTable[2]);
 if(!codecGet(1,0xF00,0x04,codecTable[3])) {codecTable[0]=3;return;} // no controller, codec or answer
 codecGet(2,0xF81,0,codecTable[4]);
 const uint32_t start=(codecTable[3]>>16)&0xFF,count=codecTable[3]&0xFF;
 if(codecTable[3]!=0xFFFFFFFF)
  for(uint32_t i=0;i<count && i<24;i++) {
   const uint32_t nid=start+i;auto e=codecTable+6+i*4;
   e[0]=nid;codecGet(nid,0xF00,0x09,e[1]);
   if(((e[1]>>20)&0xF)==4) {codecGet(nid,0xF1C,0,e[2]);codecGet(nid,0xF00,0x0C,e[3]);}
  }
 codecTable[0]=1;
}

// ---- 3/4. Discovery, path map and converter capabilities --------------------
// WEXHDMIAudioDiscover: {status (1 patched | 2 symbols | 3 vtable mismatch | 4 write),
//  can calls, last vendor, last result, construct calls, last result, path count, forced}.
uint64_t discoverRec[8];
mach_vm_address_t orgCanDiscover,orgConstructDiscover;
constexpr size_t fgSlotVendor=0x158,fgSlotCan=0x230,fgSlotConstruct=0x238;
uint32_t fgVendor(void *fg) {
 auto vt=*reinterpret_cast<uintptr_t **>(fg);
 return reinterpret_cast<uint32_t (*)(void *)>(vt[fgSlotVendor/8])(fg);
}
// The generic AppleHDAWidget never sets its capability mask (+0x64), so the cached
// PCM caps (+0x28 param 0x0A, +0x2C param 0x0B) read back as 0. Set the mask and
// reload both from the converter. Function group vtable slot 0x218 is getWidget.
// WEXHDMIAudioConverters: {count, then per converter: nid|old caps<<8|old mask<<40, pcm, formats}.
uint64_t cvtRec[1+8*3];
void fixConverterCaps(void *fg) {
 if(cvtRec[0] || codecTable[0]!=1)return;
 auto vt=*reinterpret_cast<uintptr_t **>(fg);
 auto getWidget=reinterpret_cast<uint8_t *(*)(void *,uint16_t)>(vt[0x218/8]);
 uint32_t n=0;
 for(uint32_t i=0;i<24 && n<8;i++) {
  auto e=codecTable+6+i*4;
  if(!e[0] || e[1]==0xFFFFFFFF || ((e[1]>>20)&0xF)!=0)continue; // converters (type 0) only
  auto w=getWidget(fg,static_cast<uint16_t>(e[0]));
  if(!w || *reinterpret_cast<uint32_t *>(w+0x20)!=0)continue;
  uint32_t pcm=0,fmts=0;
  if(!codecGet(e[0],0xF00,0x0A,pcm) || !codecGet(e[0],0xF00,0x0B,fmts) || !pcm || pcm==0xFFFFFFFF)continue;
  auto r=cvtRec+1+n*3;
  r[0]=e[0]|(static_cast<uint64_t>(*reinterpret_cast<uint32_t *>(w+0x28))<<8)|(static_cast<uint64_t>(*reinterpret_cast<uint32_t *>(w+0x64))<<40);
  *reinterpret_cast<uint32_t *>(w+0x64)=0xFFFFFFFF;
  *reinterpret_cast<uint32_t *>(w+0x28)=pcm;
  *reinterpret_cast<uint32_t *>(w+0x2C)=fmts;
  r[1]=pcm;r[2]=fmts;n++;
 }
 cvtRec[0]=n ? n : UINT64_MAX;
 publish("WEXHDMIAudioConverters",cvtRec,sizeof(cvtRec));
}
bool wrapCanDiscover(void *fg) {
 bool r=FunctionCast(wrapCanDiscover,orgCanDiscover)(fg);
 const auto vendor=fgVendor(fg);
 if((vendor>>16)==0x8086) {probeCodec();fixConverterCaps(fg);}
 if(!r && (vendor>>16)==0x8086) {r=true;discoverRec[7]++;}
 discoverRec[1]++;discoverRec[2]=vendor;discoverRec[3]=r;
 publish("WEXHDMIAudioDiscover",discoverRec,sizeof(discoverRec));
 return r;
}
// With all pins enabled the ADL-P codec exposes 4 converters (0x03/05/07/09) and
// 9 pins (0x04/06/08/0A-0F); the generic discovery walker rejects that topology.
// Apple's Intel classes build a fixed {pin, converter} map in the same nesting the
// walker produces: paths[ set[ alt[ path[ {NodeID=pin}, {NodeID=cvt} ] ] ] ].
// Build it from the widget table: framebuffer-advertised pins first, then the
// Type-C pins 0x0A-0x0D (Linux TGL map; external displays on these laptops),
// then the rest, one distinct converter per pin.
// WEXHDMIAudioPathMap: {used, generic ret, pairs, pin0, cvt0, ..., pin3, cvt3}.
uint32_t pathRec[11];
OSArray *buildIntelPathMap() {
 if(codecTable[0]!=1)return nullptr;
 uint32_t cvts[8],pins[16],ncvt=0,npin=0;
 for(uint32_t i=0;i<24;i++) {
  auto e=codecTable+6+i*4;
  if(!e[0] || e[1]==0xFFFFFFFF)continue;
  const uint32_t type=(e[1]>>20)&0xF,digital=(e[1]>>9)&1;
  if(type==0 && digital && ncvt<8)cvts[ncvt++]=e[0];
  if(type==4 && digital && npin<16)pins[npin++]=e[0];
 }
 if(!ncvt || !npin)return nullptr;
 uint32_t order[16],n=0;
 auto add=[&](uint32_t pin){for(uint32_t i=0;i<n;i++)if(order[i]==pin)return;for(uint32_t i=0;i<npin;i++)if(pins[i]==pin && n<16){order[n++]=pin;return;}};
 if(auto match=IOService::serviceMatching("AppleIntelFramebuffer")) {
  if(auto it=IOService::getMatchingServices(match)) {
   while(auto o=it->getNextObject())
    if(auto fb=OSDynamicCast(IOService,o))
     if(auto d=OSDynamicCast(OSData,fb->getProperty("audio-codec-info")))
      if(d->getLength()>=3) {auto b=static_cast<const uint8_t *>(d->getBytesNoCopy());if(b[2])add(b[2]);}
   it->release();
  }
  match->release();
 }
 for(uint32_t pin=0x0A;pin<=0x0D;pin++)add(pin);
 for(uint32_t i=0;i<npin;i++)add(pins[i]);
 const uint32_t pairs=n<ncvt ? n : ncvt;
 auto paths=OSArray::withCapacity(pairs);
 if(!paths)return nullptr;
 // AppleHDAPath::initPathFromXML takes stream limits only from these optional keys;
 // without them every format is rejected. Values match the ADL-P converters
 // (param 0x0A 0x001A07F0: 32-192 kHz); 24-bit is the HDMI/DP LPCM ceiling.
 auto node=[](uint32_t nid)->OSDictionary * {
  auto d=OSDictionary::withCapacity(5);if(!d)return nullptr;
  auto put=[&](const char *key,uint32_t value){if(auto num=OSNumber::withNumber(value,32)) {d->setObject(key,num);num->release();}};
  put("NodeID",nid);put("MinimumSampleRate",32000);put("MaximumSampleRate",192000);put("MinimumBitDepth",16);put("MaximumBitDepth",24);
  return d;
 };
 for(uint32_t i=0;i<pairs;i++) {
  auto path=OSArray::withCapacity(2),alt=OSArray::withCapacity(1),set=OSArray::withCapacity(1);
  auto pin=node(order[i]),cvt=node(cvts[i]);
  if(path && alt && set && pin && cvt) {
   path->setObject(pin);path->setObject(cvt);alt->setObject(path);set->setObject(alt);paths->setObject(set);
   if(i<4) {pathRec[3+i*2]=order[i];pathRec[4+i*2]=cvts[i];}
  }
  OSSafeReleaseNULL(path);OSSafeReleaseNULL(alt);OSSafeReleaseNULL(set);OSSafeReleaseNULL(pin);OSSafeReleaseNULL(cvt);
 }
 pathRec[2]=paths->getCount();
 if(!paths->getCount())OSSafeReleaseNULL(paths);
 return paths;
}
uint32_t wrapConstructDiscover(void *fg,OSArray **out) {
 auto r=FunctionCast(wrapConstructDiscover,orgConstructDiscover)(fg,out);
 if(r!=kIOReturnSuccess && out && (fgVendor(fg)>>16)==0x8086) {
  pathRec[1]=r;
  if(auto built=buildIntelPathMap()) {
   OSSafeReleaseNULL(*out);
   *out=built;r=kIOReturnSuccess;pathRec[0]=1;
  }
  publish("WEXHDMIAudioPathMap",pathRec,sizeof(pathRec));
 }
 discoverRec[4]++;discoverRec[5]=r;
 discoverRec[6]=out && *out ? (*out)->getCount() : UINT64_MAX;
 publish("WEXHDMIAudioDiscover",discoverRec,sizeof(discoverRec));
 return r;
}
bool patchSlot(uintptr_t slot,uintptr_t expected,uintptr_t replacement) {
 if(*reinterpret_cast<uintptr_t *>(slot)!=expected) {discoverRec[0]=3;return false;}
 if(MachInfo::setKernelWriting(true,KernelPatcher::kernelWriteLock)!=KERN_SUCCESS) {discoverRec[0]=4;return false;}
 lilu_os_memcpy(reinterpret_cast<void *>(slot),&replacement,sizeof(replacement));
 MachInfo::setKernelWriting(false,KernelPatcher::kernelWriteLock);
 if(*reinterpret_cast<uintptr_t *>(slot)!=replacement) {discoverRec[0]=4;return false;}
 return true;
}
// AppleHDAFunctionGroupFactory gives 0x8086281C the base AppleHDAFunctionGroup,
// whose pathMapCanBeConstructedByDiscovery is a stub. Patch the base vtable's
// discovery slots; the wrappers act only for Intel vendor codecs.
void installDiscover(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size) {
 if(orgCanDiscover)return;
 auto solve=[&](const char *n){auto a=patcher.solveSymbol<mach_vm_address_t>(index,n,address,size);patcher.clearError();return a;};
 const auto vtable=solve("__ZTV21AppleHDAFunctionGroup");
 const auto can=solve("__ZN21AppleHDAFunctionGroup34pathMapCanBeConstructedByDiscoveryEv");
 const auto construct=solve("__ZN21AppleHDAFunctionGroup27constructPathMapByDiscoveryEPP7OSArray");
 struct Publish {~Publish(){publish("WEXHDMIAudioDiscover",discoverRec,sizeof(discoverRec));}} done;
 if(!vtable || !can || !construct) {discoverRec[0]=2;return;}
 const auto base=static_cast<uintptr_t>(vtable)+0x10;
 if(*reinterpret_cast<uintptr_t *>(base+fgSlotCan)!=can || *reinterpret_cast<uintptr_t *>(base+fgSlotConstruct)!=construct) {
  discoverRec[0]=3;return;
 }
 orgCanDiscover=can;orgConstructDiscover=construct;
 if(patchSlot(base+fgSlotConstruct,construct,reinterpret_cast<uintptr_t>(wrapConstructDiscover)) &&
    patchSlot(base+fgSlotCan,can,reinterpret_cast<uintptr_t>(wrapCanDiscover)))discoverRec[0]=1;
}

// ---- Enable all pins (Linux intel_haswell_enable_all_pins) ------------------
// Before AppleHDACodecGeneric::start, set Intel vendor verb 0x781 bit 0 on nid 0x02:
// the AFG then exposes every converter and pin. The codec can lose the bit when it
// is reset (system wake, link power-down), so it is re-asserted on display events
// and at playback start.
// WEXHDMIAudioAllPins: {calls, status (1 set | 2 already set | 3 no exec fn | 4 not an
//  Intel codec | 5 read failed), F81 before, set result, F81 after, AFG subordinate}.
// WEXHDMIAudioAllPinsReassert: {checks, re-asserts, last F81}.
uint32_t allPins[6],allPinsReassert[3];
mach_vm_address_t orgCGStart;
bool wrapCGStart(void *self,IOService *provider) {
 allPins[0]++;
 if(!execCmdFn)allPins[1]=3;
 else {
  // provider IOHDACodecFunction -> IOHDACodecDriver -> IOHDACodecDevice -> AppleHDAController.
  IOService *device=provider;
  for(int depth=0;device && depth<4;depth++,device=device->getProvider())
   if(device->getMetaClass() && !strcmp(device->getMetaClass()->getClassName(),"IOHDACodecDevice"))break;
  IOService *controller=device ? device->getProvider() : nullptr;
  uint32_t cad=0xF,vendor=0;
  if(device) {
   if(auto n=OSDynamicCast(OSNumber,device->getProperty("IOHDACodecAddress")))cad=n->unsigned32BitValue()&0xF;
   if(auto n=OSDynamicCast(OSNumber,device->getProperty("IOHDACodecVendorID")))vendor=n->unsigned32BitValue();
  }
  if(!controller || cad>0xE || (vendor>>16)!=0x8086)allPins[1]=4;
  else {
   using Exec=IOReturn (*)(IOService *,IOService *,uint32_t,uint32_t *);
   auto exec=reinterpret_cast<Exec>(execCmdFn);
   auto cmd=[&](uint32_t nid,uint32_t verb,uint32_t payload,uint32_t *resp)->IOReturn {
    return exec(controller,device,(cad<<28)|((nid&0xFF)<<20)|((verb&0xFFF)<<8)|(payload&0xFF),resp);
   };
   uint32_t before=0xFFFFFFFF;
   if(cmd(0x02,0xF81,0,&before)!=kIOReturnSuccess || before==0xFFFFFFFF)allPins[1]=5;
   else {
    allPins[2]=before;
    if(before&1)allPins[1]=2;
    else {
     uint32_t ignored=0;
     allPins[3]=static_cast<uint32_t>(cmd(0x02,0x781,(before|1)&0xFF,&ignored));
     allPins[1]=1;
    }
    uint32_t after=0xFFFFFFFF,sub=0xFFFFFFFF;
    cmd(0x02,0xF81,0,&after);cmd(0x01,0xF00,0x04,&sub);
    allPins[4]=after;allPins[5]=sub;
   }
  }
 }
 publish("WEXHDMIAudioAllPins",allPins,sizeof(allPins));
 return FunctionCast(wrapCGStart,orgCGStart)(self,provider);
}
bool allPinsEnabled() {return allPins[1]==1 || allPins[1]==2;}
void assertAllPins() {
 if(!allPinsEnabled())return;
 uint32_t v=0;
 if(!codecGet(0x02,0xF81,0,v) || v==0xFFFFFFFF)return;
 allPinsReassert[0]++;allPinsReassert[2]=v;
 if(!(v&1)) {codecSet(0x02,0x781,(v|1)&0xFF);allPinsReassert[1]++;}
 if(!(v&1) || allPinsReassert[0]<=1)publish("WEXHDMIAudioAllPinsReassert",allPinsReassert,sizeof(allPinsReassert));
}

// ---- 5. Codec pin from the DDI routing --------------------------------------
// Apple's framebuffer numbers the audio pin from its own port table, but Gen12
// routes each DDI to a fixed codec pin (Linux TGL map: A 0x04, B 0x06, C 0x08,
// TC1-TC4 0x0A-0x0D, 0x0E, 0x0F). Before AppleHDA reads audio-codec-info, take
// the port from TRANS_DDI_FUNC_CTL of the framebuffer's pipe and correct byte 2.
// WEXHDMIAudioPin: {calls, corrections, last pipe, last TRANS_DDI_FUNC_CTL, last port,
//  last pin, last advertised pin}.
uint32_t pinRec[7];
IOPCIDevice *gpuOf(IOService *fb) {
 IOPCIDevice *pci=nullptr;
 for(IOService *p=fb;p && !pci;p=p->getProvider())pci=OSDynamicCast(IOPCIDevice,p);
 return pci;
}
void fixAudioCodecInfo(IOService *fb) {
 if(!fb || !allPinsEnabled())return;
 auto info=OSDynamicCast(OSData,fb->getProperty("audio-codec-info"));
 auto pipeData=OSDynamicCast(OSData,fb->getProperty("AAPL,DisplayPipe"));
 if(!info || info->getLength()<4 || !pipeData || pipeData->getLength()<4)return;
 uint8_t aci[4];memcpy(aci,info->getBytesNoCopy(),4);
 uint32_t pipe=0;memcpy(&pipe,pipeData->getBytesNoCopy(),4);
 if(!aci[0] || pipe>3)return; // no audio codec advertised for this framebuffer
 auto pci=gpuOf(fb);
 if(!pci)return;
 uint32_t ddi=0;
 if(auto map=pci->mapDeviceMemoryWithRegister(0x10,kIOMapInhibitCache)) {
  if(map->getLength()>=0x70000 && map->getVirtualAddress())
   ddi=*reinterpret_cast<volatile uint32_t *>(reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress())+0x60400+0x1000*pipe);
  map->release();
 }
 pinRec[0]++;pinRec[2]=pipe;pinRec[3]=ddi;
 struct Publish {~Publish(){publish("WEXHDMIAudioPin",pinRec,sizeof(pinRec));}} done;
 const uint32_t select=(ddi>>27)&0xF; // TGL+ TRANS_DDI_PORT_SELECT = port + 1
 if(!(ddi>>31) || !select || select>9)return;
 static constexpr uint8_t portPin[9] {0x04,0x06,0x08,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F};
 const uint8_t pin=portPin[select-1];
 pinRec[4]=select-1;pinRec[5]=pin;pinRec[6]=aci[2];
 if(aci[2]!=pin) {aci[2]=pin;fb->setProperty("audio-codec-info",aci,4);pinRec[1]++;}
}

// ---- 6. ELD (Linux hsw_audio_codec_enable) ----------------------------------
// Apple's framebuffer sets output-enable for the transcoder in AUD_PIN_ELD_CP_VLD
// but never writes an ELD or sets ELD-valid, so the pin reports presence without
// ELD and AppleHDA creates no output. Build an ELD v2 (drm_edid_to_eld layout)
// from the EDID AppleHDA validated, write it to AUD_EDID_DATA and set ELD-valid,
// only for transcoders with output enabled and ELD-valid clear.
// WEXHDMIAudioEld: {writes, last transcoder, ELD length, CP_VLD before, after, SAD count, conn type, EDID length}.
uint8_t edidCopy[256];
uint32_t edidLen,eldRec[8];
mach_vm_address_t orgEdidValid,orgFBEvent;
uint32_t buildEld(uint8_t *eld,uint32_t cap,uint32_t &sads) {
 sads=0;
 if(edidLen<256 || cap<84)return 0;
 const uint8_t *e=edidCopy,*cea=edidCopy+128;
 if(cea[0]!=0x02)return 0;
 memset(eld,0,cap);
 char name[16];uint32_t mnl=0; // monitor name descriptor (0xFC), up to 16 chars
 for(int d=0;d<4;d++) {
  const uint8_t *desc=e+54+d*18;
  if(desc[0]==0 && desc[1]==0 && desc[3]==0xFC) {
   for(int i=0;i<13 && mnl<16;i++){char ch=static_cast<char>(desc[5+i]);if(ch=='\n')break;name[mnl++]=ch;}
   while(mnl && name[mnl-1]==' ')mnl--;
   break;
  }
 }
 uint8_t sadBuf[30];uint8_t spk=0;
 const uint32_t dtd=cea[2];
 for(uint32_t i=4;dtd>4 && i<dtd && i<127;) {
  const uint8_t tag=cea[i]>>5,len=cea[i]&0x1F;
  if(i+1+len>127)break;
  if(tag==1)for(uint32_t k=0;k+2<len && sads<10;k+=3){memcpy(sadBuf+sads*3,cea+i+1+k,3);sads++;}
  if(tag==4 && len>=1)spk=cea[i+1];
  i+=1+len;
 }
 if(!sads)return 0;
 eld[0]=2<<3;                                    // ELD version 2
  eld[4]=static_cast<uint8_t>((static_cast<uint32_t>(cea[1])<<5)|mnl);  // CEA EDID version, monitor name length
 eld[5]=static_cast<uint8_t>((sads<<4)|(1<<2));  // SAD count, connection type DisplayPort
 eld[7]=spk;                                     // speaker allocation
 eld[16]=e[8];eld[17]=e[9];eld[18]=e[10];eld[19]=e[11]; // manufacturer, product code
 memcpy(eld+20,name,mnl);
 uint32_t len=20+mnl;
 if(len+sads*3>cap)sads=(cap-len)/3;
 memcpy(eld+len,sadBuf,sads*3);len+=sads*3;
 eld[2]=static_cast<uint8_t>((len-4+3)/4);       // baseline ELD length in dwords
 return (len+3)&~3U;
}
void writeEld(IOService *fb) {
 if(!fb)return;
 uint8_t eld[84];uint32_t sads=0;
 const uint32_t len=buildEld(eld,sizeof(eld),sads);
 eldRec[2]=len;eldRec[5]=sads;eldRec[6]=1;eldRec[7]=edidLen;
 struct Publish {~Publish(){publish("WEXHDMIAudioEld",eldRec,sizeof(eldRec));}} done;
 if(!len)return;
 auto pci=gpuOf(fb);
 if(!pci)return;
 if(auto map=pci->mapDeviceMemoryWithRegister(0x10,kIOMapInhibitCache)) {
  if(map->getLength()>=0x70000 && map->getVirtualAddress()) {
   auto b=reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
   auto reg=[&](uint32_t o)->volatile uint32_t &{return *reinterpret_cast<volatile uint32_t *>(b+o);};
   const uint32_t vld=reg(0x650C0);eldRec[3]=vld;
   for(uint32_t t=0;t<4;t++) {
    const uint32_t outEnable=1U<<(4*t+2),eldValid=1U<<(4*t);
    if(!(vld&outEnable) || (vld&eldValid))continue;
    reg(0x650B4+0x100*t)=reg(0x650B4+0x100*t)&~(0x1FU<<5);   // reset ELD write address
    for(uint32_t i=0;i<len/4;i++){uint32_t d;memcpy(&d,eld+i*4,4);reg(0x65050+0x100*t)=d;}
    reg(0x650C0)=reg(0x650C0)|eldValid;
    eldRec[0]++;eldRec[1]=t;
   }
   eldRec[4]=reg(0x650C0);
  }
  map->release();
 }
}
bool wrapEdidValid(void *self,OSData *edid,int arg) {
 const bool r=FunctionCast(wrapEdidValid,orgEdidValid)(self,edid,arg);
 if(r && edid && edid->getLength()>=256) {memcpy(edidCopy,edid->getBytesNoCopy(),256);edidLen=256;}
 return r;
}
// Event 2 is the display-mode change on which AppleHDA evaluates the head.
IOReturn wrapFBEvent(void *self,void *ref,IOService *fb,int event,void *info) {
 if(event==2)assertAllPins();
 fixAudioCodecInfo(fb);
 if(event==2)writeEld(fb); // before AppleHDA evaluates pin sense / ELD
 const auto ret=FunctionCast(wrapFBEvent,orgFBEvent)(self,ref,fb,event,info);
 if(event==2)writeEld(fb); // output enable may only be set during the handler; no-op once ELD-valid
 return ret;
}

// ---- 1b. Codec wake right before AppleHDA's link reset (TGL) ----------------
// On TGL-H Apple's framebuffer starts after processKernel and keeps rewriting
// 0x65F10, so an early wake is gone by the time AppleHDA resets the link and reads
// STATESTS. Re-assert AUDIO_PIN_BUF_CTL bit 31 and pulse the codec-wake override
// immediately before every resetAudioController, keeping bit 15 set through it.
// WEXHDMIAudioResetWake: {calls, 0x65F10 before, after set, after reset, return,
//  AUDIO_PIN_BUF_CTL after set, bool args (a | b<<8)}.
uint64_t nowNs() {uint64_t t,ns;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);return ns;}
uint32_t rd(uint32_t off) {return wakeBase ? *reinterpret_cast<volatile uint32_t *>(wakeBase+off) : UINT32_MAX;}
// Re-assert AUDIO_PIN_BUF_CTL bit 31 and pulse 0x65F10 bit 15 (left set). Returns 0x65F10 after.
uint32_t pulseWake() {
 if(!wakeBase)return UINT32_MAX;
 auto buf=reinterpret_cast<volatile uint32_t *>(wakeBase+0x48414);
 if(*buf!=UINT32_MAX)*buf=*buf|0x80000000U;
 auto reg=reinterpret_cast<volatile uint32_t *>(wakeBase+0x65F10);
 const uint32_t v=*reg;
 if(v!=UINT32_MAX) {*reg=v&~(1U<<15);IODelay(1000);*reg=(*reg)|(1U<<15);IODelay(1000);}
 return *reg;
}
// WEXHDMIAudioResetWake (v3, 64-bit): {calls, ns, PWR_WELL_CTL1, PWR_WELL_CTL2, 0x65F10 before,
//  after our set, after reset, return, AUDIO_PIN_BUF_CTL, args a|b<<8} for the LAST call;
//  WEXHDMIAudioResetFirst: same layout for the FIRST call.
// v4 showed Apple's hwEnableAudio runs only ~5 s AFTER AppleHDA's first link reset
// (it follows AppleHDA's start), so the reset can never wait for it. v5: ~1 s after
// the first hwEnableAudio (wells up, wake bit held) ask AppleHDA to probe its link
// once more with AppleHDAController::probeAudioLink().
// WEXHDMIAudioReprobe: {calls, ns, PWR_WELL_CTL1, PWR_WELL_CTL2, 0x65F10 before,
//  after wake, after probe, return, 1 done | 2 no controller | 3 no symbol}.
volatile bool hwEnableDone;
bool tglFBHooked;
void *hdaCtrl;
mach_vm_address_t probeLinkFn;
uint64_t reprobeRec[9];
thread_call_t reprobeCall;
mach_vm_address_t orgReset;
uint64_t resetRec[10],resetFirst[10];
uint32_t wrapReset(void *self,bool a,bool b) {
 if(!hdaCtrl)hdaCtrl=self; // v5: kept for the reprobe after hwEnableAudio
 resetRec[0]++;resetRec[1]=nowNs();resetRec[2]=rd(0x45400);resetRec[3]=rd(0x45404);
 resetRec[4]=rd(0x65F10);resetRec[5]=pulseWake();resetRec[8]=rd(0x48414);
 resetRec[9]=static_cast<uint64_t>(a)|(static_cast<uint64_t>(b)<<8);
 const auto r=FunctionCast(wrapReset,orgReset)(self,a,b);
 resetRec[6]=rd(0x65F10);resetRec[7]=r;
 if(resetRec[0]==1){memcpy(resetFirst,resetRec,sizeof(resetRec));publish("WEXHDMIAudioResetFirst",resetFirst,sizeof(resetFirst));}
 publish("WEXHDMIAudioResetWake",resetRec,sizeof(resetRec));
 return r;
}

// ---- 1c. TGL: wake from inside Apple's hwEnableAudio -------------------------
// jkbuha (#124): if the audio well is down at AppleHDA's reset, try the wake while
// the framebuffer has the domain up, i.e. right after its own hwEnableAudio.
// WEXHDMIAudioHwEnable: {calls, first ns, last ns, PWR_WELL_CTL1, PWR_WELL_CTL2,
//  0x65F10 before, after original, after our wake, AUDIO_PIN_BUF_CTL, AUD_PIN_ELD_CP_VLD}.
void reprobe(thread_call_param_t,thread_call_param_t) {
 reprobeRec[0]++;reprobeRec[1]=nowNs();reprobeRec[2]=rd(0x45400);reprobeRec[3]=rd(0x45404);
 reprobeRec[4]=rd(0x65F10);reprobeRec[5]=pulseWake();
 if(!hdaCtrl)reprobeRec[8]=2;
 else if(!probeLinkFn)reprobeRec[8]=3;
 else {
  reprobeRec[7]=reinterpret_cast<uint32_t (*)(void *)>(probeLinkFn)(hdaCtrl);
  reprobeRec[6]=rd(0x65F10);reprobeRec[8]=1;
 }
 publish("WEXHDMIAudioReprobe",reprobeRec,sizeof(reprobeRec));
}
mach_vm_address_t orgHwEnable;
uint64_t hwRec[10];
uint64_t wrapHwEnable(void *ctrl,void *fb,void *path) {
 hwRec[0]++;const auto t=nowNs();if(hwRec[0]==1)hwRec[1]=t;hwRec[2]=t;
 hwRec[5]=rd(0x65F10);
 const auto r=FunctionCast(wrapHwEnable,orgHwEnable)(ctrl,fb,path);
 hwRec[3]=rd(0x45400);hwRec[4]=rd(0x45404);hwRec[6]=rd(0x65F10);
 hwRec[7]=pulseWake();hwRec[8]=rd(0x48414);hwRec[9]=rd(0x650C0);
 publish("WEXHDMIAudioHwEnable",hwRec,sizeof(hwRec));
 if(!hwEnableDone && !reprobeCall && tglFBHooked) {
  reprobeCall=thread_call_allocate(reprobe,nullptr);
  uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_SEC,&deadline);
  if(reprobeCall)thread_call_enter_delayed(reprobeCall,deadline);
 }
 hwEnableDone=true;
 return r;
}

// ---- 7. Stream binding at playback start ------------------------------------
// Linux hdmi_init_pin / hdmi_setup_audio_infoframe: select the streaming
// converter on the connected pin, enable pin output, unmute, set the channel count
// and ASP map, and send the DP audio InfoFrame.
// WEXHDMIAudioRoute: {calls, pin, cvt, conn index, pin ctl before, after, conn select,
//  status (1 bound | 2 no streaming cvt | 3 no pin with ELD | 4 cvt not connectable),
//  amp before, after, channel count, DIP transmit, channels}.
uint32_t routeRec[13];
void bindStreamingPin() {
 routeRec[0]++;
 struct Publish {~Publish(){publish("WEXHDMIAudioRoute",routeRec,sizeof(routeRec));}} done;
 static constexpr uint32_t cvts[4] {0x03,0x05,0x07,0x09};
 uint32_t cvt=0;
 for(auto c:cvts) {uint32_t sc=0;if(codecGet(c,0xF06,0,sc) && (sc>>4)&0xF){cvt=c;break;}}
 if(!cvt) {routeRec[7]=2;return;}
 uint32_t pin=0;
 // All TGL/ADL-P port pins (DDI A-C combo and TC1-6): on TGL-H the HDMI and USB-C
 // ports can be routed outside 0x0A-0x0D.
 static constexpr uint8_t pins[9] {0x04,0x06,0x08,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F};
 for(auto p:pins) {uint32_t sense=0;if(codecGet(p,0xF09,0,sense) && (sense>>31) && ((sense>>30)&1)){pin=p;break;}}
 if(!pin) {routeRec[1]=0;routeRec[2]=cvt;routeRec[7]=3;return;}
 uint32_t len=0,list=0,index=0xFF;
 codecGet(pin,0xF00,0x0E,len);codecGet(pin,0xF02,0,list);
 for(uint32_t i=0;i<4 && i<(len&0x7F);i++)if(((list>>(8*i))&0xFF)==cvt)index=i;
 routeRec[1]=pin;routeRec[2]=cvt;routeRec[3]=index;
 if(index==0xFF) {routeRec[7]=4;return;}
 uint32_t ctl=0;codecGet(pin,0xF07,0,ctl);routeRec[4]=ctl;
 codecSet(pin,0x701,index);
 codecSet(pin,0x707,(ctl|0x40)&0xFF);
 codecGet(pin,0xF07,0,routeRec[5]);codecGet(pin,0xF01,0,routeRec[6]);
 uint32_t fmt=0;codecGet(cvt,0xA00,0,fmt);const uint32_t chans=(fmt&0xF)+1; // A00 bits 3:0 = channels-1
 codecGet(pin,0xBA0,0x00,routeRec[8]);
 codecSet(pin,0x3B0,0x00);                       // output amp: both channels, unmute, gain 0
 codecGet(pin,0xBA0,0x00,routeRec[9]);
 codecSet(cvt,0x72D,(chans-1)&0xFF);
 codecGet(cvt,0xF2D,0,routeRec[10]);
 for(uint32_t ch=0;ch<chans && ch<8;ch++)codecSet(pin,0x734,(ch<<4)|ch);
 // DP audio InfoFrame: type 0x84, length 0x1B, version 0x11<<2, CC = channels-1, CA 0.
 const uint8_t ai[11] {0x84,0x1B,0x44,static_cast<uint8_t>(chans-1),0,0,0,0,0,0,0};
 codecSet(pin,0x730,0x00);codecSet(pin,0x732,0x00);
 codecSet(pin,0x730,0x00);
 for(auto byte:ai)codecSet(pin,0x731,byte);
 codecSet(pin,0x730,0x00);codecSet(pin,0x732,0xC0);
 codecGet(pin,0xF32,0,routeRec[11]);
 routeRec[12]=chans;
 routeRec[7]=1;
}
mach_vm_address_t orgEngPStart;
uint32_t wrapEngPStart(void *self) {
 const auto r=FunctionCast(wrapEngPStart,orgEngPStart)(self);
 if(!r) {assertAllPins();bindStreamingPin();}
 return r;
}
} // namespace

bool WEXHDMIAudio::requested() {return checkKernelArgument("-wex-hdmi-audio");}

void WEXHDMIAudio::wakeEarly(IOPCIDevice *gpu) {
 if(wakeBase || !gpu || !requested())return;
 auto map=gpu->mapDeviceMemoryWithRegister(0x10,kIOMapInhibitCache);
 if(!map || map->getLength()<0x1000000 || !map->getVirtualAddress()) {
  OSSafeReleaseNULL(map);wakeRec[1]=8;publish("WEXHDMIAudioWake",wakeRec,sizeof(wakeRec));return;
 }
 wakeEarlyMap=map; // retained for the delayed restore
 wakeAt(reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress()),4);
}

void WEXHDMIAudio::wakeFramebuffer(volatile uint8_t *mmio) {
 if(wakeBase || !mmio || !requested())return;
 wakeAt(mmio,2);
}

bool WEXHDMIAudio::processFramebuffer(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size) {
 if(orgSetupRes || !requested())return false;
 fbGetDDC=patcher.solveSymbol<mach_vm_address_t>(index,"__ZN21AppleIntelFramebuffer13fbGetDDCBlockEjPhPyjP21AppleIntelDisplayPath",address,size);
 patcher.clearError();
 if(!fbGetDDC)return false;
 fbSetAudioProps=patcher.solveSymbol<mach_vm_address_t>(index,"__ZN21AppleIntelFramebuffer18setAudioPropertiesEv",address,size);
 patcher.clearError();
 KernelPatcher::RouteRequest route {"__ZN21AppleIntelFramebuffer21setupDisplayResourcesEv",wrapSetupRes,orgSetupRes};
 const bool ok=patcher.routeMultiple(index,&route,1,address,size);patcher.clearError();
 IORegistryEntry::getRegistryRoot()->setProperty("WEXHDMIAudioFBReady",ok);
 return ok;
}

bool WEXHDMIAudio::processTGLFramebuffer(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size) {
 if(orgHwEnable || !requested())return false;
 KernelPatcher::RouteRequest route {"__ZN31AppleIntelFramebufferController13hwEnableAudioEP21AppleIntelFramebufferP21AppleIntelDisplayPath",wrapHwEnable,orgHwEnable};
 const bool ok=patcher.routeMultiple(index,&route,1,address,size);patcher.clearError();
 IORegistryEntry::getRegistryRoot()->setProperty("WEXHDMIAudioTGLFBReady",ok);
 tglFBHooked=ok;
 uint64_t ns=nowNs();publish("WEXHDMIAudioTGLFBLoadNs",&ns,sizeof(ns));
 return ok;
}

bool WEXHDMIAudio::processController(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size) {
 if(!requested() || orgFBEvent)return false;
 execCmdFn=patcher.solveSymbol<mach_vm_address_t>(index,"__ZN18AppleHDAController19executeCodecCommandEP16IOHDACodecDevicejPj",address,size);
 patcher.clearError();
 bool ok=execCmdFn!=0;
 probeLinkFn=patcher.solveSymbol<mach_vm_address_t>(index,"__ZN18AppleHDAController14probeAudioLinkEv",address,size);
 patcher.clearError();
 ok=hookAddress(patcher,index,address,size,"__ZN18AppleHDAController23framebufferEventHandlerEPvP13IOFramebufferiS0_",reinterpret_cast<void *>(wrapFBEvent),orgFBEvent) && ok;
 ok=hookAddress(patcher,index,address,size,"__ZN18AppleHDAController19edidIsValidForAudioEP6OSDatai",reinterpret_cast<void *>(wrapEdidValid),orgEdidValid) && ok;
 ok=hookAddress(patcher,index,address,size,"__ZN18AppleHDAController20resetAudioControllerEbb",reinterpret_cast<void *>(wrapReset),orgReset) && ok;
 IORegistryEntry::getRegistryRoot()->setProperty("WEXHDMIAudioControllerReady",ok);
 return ok;
}

bool WEXHDMIAudio::processMain(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size) {
 if(!requested() || orgCGStart)return false;
 installDiscover(patcher,index,address,size);
 bool ok=discoverRec[0]==1;
 ok=hookAddress(patcher,index,address,size,"__ZN20AppleHDACodecGeneric5startEP9IOService",reinterpret_cast<void *>(wrapCGStart),orgCGStart) && ok;
 ok=hookAddress(patcher,index,address,size,"__ZN14AppleHDAEngine23performAudioEngineStartEv",reinterpret_cast<void *>(wrapEngPStart),orgEngPStart) && ok;
 IORegistryEntry::getRegistryRoot()->setProperty("WEXHDMIAudioMainReady",ok);
 return ok;
}
