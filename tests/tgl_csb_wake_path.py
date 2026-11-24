#!/usr/bin/env python3
"""Structural wiring checks for the production TGL CSB sleep/wake hooks."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "WhateverXe" / "kern_xe.cpp"
text = SOURCE.read_text()


def body(marker):
    start = text.find(marker)
    if start < 0:
        raise SystemExit(f"missing production function: {marker}")
    brace = text.find("{", start + len(marker))
    if brace < 0:
        raise SystemExit(f"missing function body: {marker}")
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1:index]
    raise SystemExit(f"unterminated function body: {marker}")


io = body("struct TGLCSBIO")
streamer_init = body("bool XE::wrapTGLCSBInit(")
sleep = body("static void tglS0WrapBridgeSystemWillSleep(void *bridge)")
snapshot = body("static void tglS0CaptureBeforeIRQDisable(void *accelerator,uint32_t ticket)")
bridge = body("static void tglS0CaptureIRQDisable(void *bridge) ")
host = body("void XE::wrapTGLHostIRQClassDisable(void *bridge)")
installer = body("bool XE::installTGLCSB12(")
wake = body("void XE::wrapTGLCSBWake(void *streamer)")
reset = body("void XE::wrapTGLCSBReset(void *streamer)")
process = body("void XE::wrapTGLCSBProcess(void *streamer,uint32_t flags)")
initialize = body("void tglCSBInitialize(void *streamer,uint32_t flags=0,bool verifyWakeReset=false)")
submit = body("void XE::wrapTGLHostSubmit(void *streamer,uint32_t flags)")

checks = {
    "new streamer lifetime clears the previous GFX_MODE proof before rebinding":
        streamer_init.index("e.modeAtInit=UINT32_MAX;")
        < streamer_init.index("e.streamer=streamer"),
    "streamer construction invalidates any previous sleep snapshot":
        "__atomic_store_n(&tglS0Sleep[io.engine].epoch,0U,__ATOMIC_RELEASE);" in streamer_init,
    "TGLCSBIO delegates tail selection to the shared production helper":
        "sampledTail=TGLCSB::readTail(*this)" in io,
    "sleep snapshot uses the selected HWSP/MMIO tail source":
        "io.readTail()" in snapshot and "io.hwsp+0xBC" not in snapshot,
    "sleep wrapper adopts only the exact disabled bridge and does not access MMIO":
        "TGLCSBIO" not in sleep and "io.acquire()" not in sleep
        and sleep.index("original(bridge);",sleep.index("const auto epoch=")) < sleep.index("tglS0Candidate.claim(")
        and "!b[0x8A8]" in sleep and "tglS0RecordUnchanged(record,engine)" in sleep,
    "capture occurs before native IRQ disable, including an earlier shutdown":
        "tglS0CaptureBeforeIRQDisable(owner,ticket)" in bridge
        and host.index("xelpIRQStop(bridge)") < host.index("tglS0CaptureIRQDisable(bridge)")
        < host.index("orgTGLHostIRQClassDisable)(bridge)"),
    "canonical bridge, live power state, and one-shot candidate guard":
        "tglS0CanonicalBridge(bridge,owner)" in bridge and "!b[0x8A8]" in bridge
        and "tglS0Candidate.begin(key)" in bridge and "tglS0Candidate.publish(key,ticket)" in bridge
        and "tglS0InvalidateIRQCandidate(bridge)" in body("void XE::wrapTGLHostIRQClassEnable(void *bridge)"),
    "snapshot skips foreign or in-flight streamer and requires drained empty banks":
        "engine.streamer)+0x10" in snapshot and "!=accelerator)continue" in snapshot
        and "TGLCSB::emptyBanks(record.slots)" in snapshot and "engine.activity.stable(revision)" in snapshot,
    "all submission, consumption and reinitialization paths invalidate the candidate revision":
        "activity.enter(&e.activity,true)" in submit and "engine.activity.change()" in process
        and "ActivityScope activity(" in initialize and "e.activity.change()" in streamer_init,
    "wake revalidates snapshot ownership and epoch and preserves without rewriting heads":
        "tglS0RecordUnchanged(snap,e)" in wake and "epoch==__atomic_load_n(&tglS0Epoch" in wake
        and "e.state.head=pointer&0xF" not in wake,
    "failed fallback pointer readback blocks submission without retrying destructive init":
        "TGLCSB::resetMatches(pointer,tail)" in initialize and "TGLCSB::Error::ResetMismatch" in initialize
        and "e.state.error!=TGLCSB::Error::ResetMismatch" in submit,
    "IRQ entry is qualified before host routing and never routed a second time":
        text.index("const bool s0IRQEntryQualified=") < text.index("patcher.routeMultiple(index,host,arrsize(host)")
        < text.index("installTGLCSB12(patcher,index,address,size,s0IRQEntryQualified)")
        and text.count('{"__ZN17IGInterruptBridge17disableInterruptsEv",') == 1
        and "s0IRQEntryQualified && orgTGLHostIRQClassDisable" in installer
        and "RouteRequest route {irqSymbol" not in installer
        and "!memcmp(reinterpret_cast<void *>(address+0x4A778)" not in installer
        and installer.index("patcher.routeMultiple(index,&route,1,address,size)")
        < installer.index("tglS0SleepHookRouted,sleepOK"),
    "wake path uses the selected HWSP/MMIO tail source":
        "io.readTail()" in wake and "io.hwsp+0xBC" not in wake,
    "wake path uses the tested plan with last successful mode":
        "TGLCSB::planWake(" in wake and "e.modeAtInit" in wake,
    "wake path verifies the masked GFX_MODE re-arm":
        "TGLCSB::ensureCSB12Mode(plan,io,modeAfter)" in wake,
    "shared initializer reads GFX_MODE while forcewake is held":
        initialize.index("io.acquire()") < initialize.index("io.readMode()"),
    "shared initializer verifies mode before touching the 12-entry queue":
        initialize.index("TGLCSB::ensureCSB12Mode(modePlan,io,mode)")
        < initialize.index("TGLCSB::initialize(engine.state,io)"),
    "failed mode verification returns before queue initialization":
        "engine.state.error=TGLCSB::Error::Unavailable;\n  return;\n }\n TGLCSB::initialize(engine.state,io)" in initialize,
    "shared initializer invalidates a stale sleep baseline before CSB reset":
        initialize.index("__atomic_store_n(&tglS0Sleep[io.engine].epoch,0U,__ATOMIC_RELEASE);")
        < initialize.index("TGLCSB::initialize(engine.state,io)"),
    "initializer clears readiness before mode verification":
        initialize.index("engine.hardwareReady=false;") < initialize.index("io.readMode()"),
    "last known-good mode is updated only after successful initialization":
        initialize.index("TGLCSB::initialize(engine.state,io)")
        < initialize.index("engine.modeAtInit=mode"),
    "submit retry goes through the guarded shared initializer":
        "if (!e.hardwareReady && e.state.error!=TGLCSB::Error::ResetMismatch) tglCSBInitialize(streamer,flags)" in submit,
    "failed mode/queue admission blocks native submit (never drop stamp silently)":
        submit.index("if (!TGLCSB::canSubmit(e.hardwareReady,e.state.error))")
        < submit.index("orgTGLHostSubmit)(streamer,flags)"),
    "GPU reset blocks failed CSB12 mode before native processGPUReset resubmits":
        reset.index("if (!TGLCSB::canSubmit(engine.hardwareReady,engine.state.error))")
        < reset.rindex("orgTGLCSBReset)(streamer)"),
    "partially installed CSB hooks fall back to original native implementations":
        all("if (!tglCSB12Enabled)" in v for v in (streamer_init,wake,reset,process)),
    "only the guarded initializer invokes the 12-entry reset":
        text.count("TGLCSB::initialize(engine.state,io)") == 1,
    "unverified wake mode blocks immediate 12-entry initialization":
        "e.state.error=TGLCSB::Error::Unavailable;" in wake
        and "if (modeReady && !preserve) tglCSBInitialize(streamer,0,verifyWakeReset);" in wake,
    "old snapshot-validity gate is absent":
        "action==TGLCSB::WakeAction::RearmAndInitialize && before.valid" not in wake,
}

failed = [name for name, passed in checks.items() if not passed]
for name, passed in checks.items():
    print(("PASS" if passed else "FAIL") + ": " + name)
if failed:
    raise SystemExit(1)
