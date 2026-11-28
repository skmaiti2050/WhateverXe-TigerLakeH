// Bounded CPU-only evidence for the opt-in S0 experiment. Never read GPU MMIO
// here, change scheduler state, or treat the receipt as a completion event.
#ifndef KERN_TGL_S0_EVIDENCE_HPP
#define KERN_TGL_S0_EVIDENCE_HPP
#include <stdint.h>
namespace TGLS0Evidence {
enum Stage : unsigned { SleepEnter, SleepExit, WakeEnter, WakeExit,
    Power0Enter, Power0Exit, Power2Enter, Power2Exit,
    BridgeSleepEnter, BridgeSleepExit, CaptureEnter, CaptureExit,
    IRQDisableEnter, IRQDisableExit, IRQEnableEnter, IRQEnableExit,
    ResetEnter0, ResetExit0=ResetEnter0+6, StageCount=ResetExit0+6 };
struct Timeline {
    uint64_t sequence {}, stages[StageCount] {};
    uint64_t next() { return __atomic_add_fetch(&sequence,1ULL,__ATOMIC_RELAXED); }
    void mark(unsigned stage) {
        if(stage<StageCount) __atomic_store_n(&stages[stage],next(),__ATOMIC_RELEASE);
    }
    uint64_t at(unsigned stage) const {
        return stage<StageCount?__atomic_load_n(&stages[stage],__ATOMIC_ACQUIRE):0;
    }
};
enum Field : unsigned { Ordinal, Thread, Streamer, Raw, Engine, Index, Head,
    Tail, Source, Retries, Flags, Identity, Active, Slot0, Slot1, Slot2, Slot3,
    InitMode, SnapshotEpoch, PreviousOrdinal, PreviousRaw, PreviousIndex,
    PreviousActive, PreviousSlot0, PreviousSlot1, PreviousSlot2, PreviousSlot3,
    FieldCount };
struct Receipt { uint64_t q[FieldCount] {}; };
// An exact live entry address and thread, not a historical ring index, identify
// the native validator's input. The producer is the serialized native engine.
// A reentrant producer cannot overwrite a still-owned record; it fails open
// for diagnostics only and proceeds with the original native handler.
struct Dispatch {
    uintptr_t key {};
    uint64_t words[FieldCount] {}, previous[8] {};
    bool begin(uintptr_t entry, const Receipt &r) {
        uintptr_t expected=0;
        if(entry<=1 || !__atomic_compare_exchange_n(&key,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        for(unsigned i=0;i<PreviousOrdinal;++i)__atomic_store_n(&words[i],r.q[i],__ATOMIC_RELAXED);
        for(unsigned i=0;i<8;++i)__atomic_store_n(&words[PreviousOrdinal+i],__atomic_load_n(&previous[i],__ATOMIC_RELAXED),__ATOMIC_RELAXED);
        __atomic_store_n(&key,entry,__ATOMIC_RELEASE);return true;
    }
    bool find(uintptr_t entry,uint64_t thread,Receipt &r) const {
        if(entry<=1 || __atomic_load_n(&key,__ATOMIC_ACQUIRE)!=entry)return false;
        for(unsigned i=0;i<FieldCount;++i)r.q[i]=__atomic_load_n(&words[i],__ATOMIC_RELAXED);
        return r.q[Thread]==thread && __atomic_load_n(&key,__ATOMIC_ACQUIRE)==entry;
    }
    void complete(uintptr_t entry, const Receipt &r, uint32_t active, const uintptr_t (&slots)[4]) {
        if(__atomic_load_n(&key,__ATOMIC_ACQUIRE)!=entry)return;
        const uint64_t p[8]={r.q[Ordinal],r.q[Raw],r.q[Index],active,slots[0],slots[1],slots[2],slots[3]};
        for(unsigned i=0;i<8;++i)__atomic_store_n(&previous[i],p[i],__ATOMIC_RELAXED);
        __atomic_store_n(&key,static_cast<uintptr_t>(0),__ATOMIC_RELEASE);
    }
    // New streamer lifetime; native serialization excludes active dispatch.
    void clear() {
        __atomic_store_n(&key,static_cast<uintptr_t>(0),__ATOMIC_RELEASE);
        for(auto &v:previous)__atomic_store_n(&v,0ULL,__ATOMIC_RELAXED);
    }
};
}
#endif
