// HDMI/DP audio for the Gen12 (TGL/ADL-P) Intel display codec through AppleHDA.
// Opt-in with -wex-hdmi-audio. See kern_hdmi_audio.cpp.
#ifndef KERN_HDMI_AUDIO_HPP
#define KERN_HDMI_AUDIO_HPP
#include <Headers/kern_patcher.hpp>
#include <IOKit/pci/IOPCIDevice.h>

namespace WEXHDMIAudio {
bool requested();
// processKernel: wake the display codec before AppleHDA probes the HDA link.
void wakeEarly(IOPCIDevice *gpu);
// Framebuffer start: the same wake, if processKernel could not map the GPU.
void wakeFramebuffer(volatile uint8_t *mmio);
// AppleIntel*Framebuffer image: setupDisplayResources audio flag.
bool processFramebuffer(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size);
// AppleHDAController and AppleHDA images (linked at runtime by SimpleHDA on Tahoe).
bool processController(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size);
bool processMain(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size);
}
#endif
