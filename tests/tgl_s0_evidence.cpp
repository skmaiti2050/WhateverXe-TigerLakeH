#include "kern_tgl_s0_evidence.hpp"
#include <cassert>
#include <iostream>
using namespace TGLS0Evidence;
int main() {
    Timeline timeline;
    Dispatch engine;
    Receipt in,out;
    const uintptr_t key=0x1000;
    const uintptr_t slots[4]={0,0,0x3000,0};
    in.q[Thread]=0x2000;in.q[Engine]=0;
    assert(!engine.find(key,0x2000,out));
    for(unsigned i=0;i<3000;++i) {
        in.q[Ordinal]=timeline.next();in.q[Raw]=0x10000+i;in.q[Index]=i%12;
        assert(engine.begin(key,in));
        assert(!engine.begin(key+8,in));
        assert(!engine.find(key+8,0x2000,out));
        assert(!engine.find(key,0x2001,out));
        assert(engine.find(key,0x2000,out));
        assert(out.q[Raw]==in.q[Raw] && out.q[Ordinal]==in.q[Ordinal]);
        if(i)assert(out.q[PreviousRaw]==in.q[Raw]-1 && out.q[PreviousOrdinal]+1==in.q[Ordinal] && out.q[PreviousSlot2]==0x3000);
        engine.complete(key+8,in,1,slots);
        assert(engine.find(key,0x2000,out));
        engine.complete(key,in,1,slots);
        assert(!engine.find(key,0x2000,out));
    }
    timeline.mark(SleepEnter);timeline.mark(BridgeSleepEnter);timeline.mark(CaptureEnter);timeline.mark(CaptureExit);timeline.mark(IRQDisableExit);timeline.mark(SleepExit);
    timeline.mark(WakeEnter);timeline.mark(IRQEnableEnter);timeline.mark(IRQEnableExit);
    in.q[Ordinal]=timeline.next();in.q[Raw]=0x3000003ff8000;assert(engine.begin(key,in));
    assert(engine.find(key,0x2000,out));
    assert(timeline.at(SleepEnter)<timeline.at(SleepExit));
    assert(timeline.at(CaptureEnter)<timeline.at(CaptureExit));
    assert(timeline.at(IRQEnableExit)<out.q[Ordinal]);
    assert(timeline.at(ResetEnter0)==0 && timeline.at(ResetExit0)==0);
    engine.complete(key,in,0,slots);timeline.mark(ResetEnter0);timeline.mark(ResetExit0);
    assert(timeline.at(ResetEnter0)>out.q[Ordinal]);
    engine.clear();in.q[Ordinal]=timeline.next();assert(engine.begin(key,in));assert(engine.find(key,0x2000,out));assert(out.q[PreviousOrdinal]==0);
    std::cout<<"PASS bounded live receipt after 3000 dispatches; exact pointer/thread, prior completion, nesting, lifecycle order and reuse\n";
}
