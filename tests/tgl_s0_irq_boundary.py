#!/usr/bin/env python3
"""Static callsite and ABI qualification for the opt-in S0 CSB snapshot.

Only the exact AppleIntelTGLGraphics donor is supported. This checks native
machine-code ordering; it cannot establish sleep power-state runtime safety.
"""
import hashlib
from pathlib import Path
import struct
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: tgl_s0_irq_boundary.py <exact AppleIntelTGLGraphics donor>")
source = Path(sys.argv[1]).read_bytes()
sha = hashlib.sha256(source).hexdigest()
EXPECTED = "6c8ab6cce50b9529679ee4fdf5b1c37d091677d945076f41cb4b7f8196c51449"
assert sha == EXPECTED, f"unqualified accelerator donor SHA256: {sha}"

def branch(offset: int, opcode: int) -> int:
    assert source[offset] == opcode, f"unexpected branch opcode @{offset:#x}"
    disp = struct.unpack_from("<i", source, offset + 1)[0]
    return offset + 5 + disp

entry = bytes.fromhex("55 48 89 e5 53 50 48 89 fb")
assert source[0x4AB14:0x4AB1D] == entry, "changed bridge sleep entry"
assert source[0x4A778:0x4A781] == entry, "changed bridge IRQ disable entry"
assert source[0x4A781:0x4A785] == bytes.fromhex("48 8b 7f 10"), "owner must remain bridge+0x10"

assert branch(0x28967, 0xE8) == 0x4AB14, "accelerator sleep must call bridge sleep"
assert source[0x28960:0x28967] == bytes.fromhex("48 8b bb 48 12 00 00"), "canonical bridge must be accelerator+0x1248"
assert source[0x4A6A0:0x4A6A7] == bytes.fromhex("80 bf a8 08 00 00 00"), "disable checks bridge enabled flag"
assert source[0x4A769:0x4A770] == bytes.fromhex("c6 83 a8 08 00 00 00"), "disable clears enabled flag"
assert branch(0x4AB2D, 0xE8) == 0x15DB0, "bridge sleep must first finish stamps"
assert branch(0x4AB3B, 0xE9) == 0x4A69A, "bridge sleep must delegate native bridge disable"
assert branch(0x26847, 0xE8) == 0x4A69A, "stopGraphicsEngine can disable the bridge before sleep"
assert branch(0x4A6C0, 0xE8) == 0x15DB0, "bridge disable must finish remaining stamps"
for addr in (0x4A6D1, 0x4A70F):
    assert branch(addr, 0xE8) == 0x4A778, "both native branches must disable interrupts"
    assert addr > 0x4A6C0
assert branch(0x4A78F, 0xE8) == 0x28C2A, "native IRQ disable obtains its own ForceWake"

# The two accelerator wake entry points use different native IRQ/reset orders.
# Validate both exact entry ABIs and the original validator argument identity.
assert source[0x28916:0x2891F] == entry
assert source[0x2897A:0x28983] == entry
assert source[0x2A9B6:0x2A9C6] == bytes.fromhex("55 48 89 e5 41 57 41 56 53 50 49 89 f6 48 89 fb")
assert source[0x2AB41:0x2AB43] == bytes.fromhex("31 c0")
assert branch(0x289A0,0xE8) == 0x4AB40
assert source[0x289AF:0x289B5] == bytes.fromhex("ff 90 20 01 00 00")
assert source[0x2AAEA:0x2AAF0] == bytes.fromhex("ff 90 20 01 00 00")
assert branch(0x2AAF7,0xE8) == 0x4AB40
assert branch(0x3809D,0xE8) == 0x3A292
assert source[0x3C868:0x3C874] == bytes.fromhex("8b 47 50 48 c1 e0 04 48 8d 7c 07 30")
assert branch(0x3C874,0xE8) == 0x3C3E6
assert source[0x3C408:0x3C40B] == bytes.fromhex("48 89 f7")
assert branch(0x3C40B,0xE8) == 0x90A41
print("PASS exact native power callback ABIs, alternate IRQ/reset order and validator entry identity")

source_code = (Path(__file__).resolve().parents[1] / "WhateverXe" / "kern_xe.cpp").read_text()
for item in (
    '__ZN17IGInterruptBridge15systemWillSleepEv',
    '__ZN17IGInterruptBridge17disableInterruptsEv',
    'tglS0WrapBridgeSystemWillSleep',
    'tglS0CaptureIRQDisable',
    'wrapTGLHostIRQClassDisable',
    'tglS0CaptureBeforeIRQDisable',
    'tglS0Candidate.claim',
    'tglS0CanonicalBridge',
):
    assert item in source_code, f"missing opt-in S0 native boundary contract: {item}"

print("PASS exact donor:", sha)
print("PASS accelerator sleep -> bridge sleep -> first finishAllStamps")
print("PASS bridge disable -> second finishAllStamps -> both IRQ disable branches")
print("PASS native disableInterrupts owner+0x10 and own SafeForceWake")
print("NOTE: hardware ForceWake/IRQ safety and wake recovery still unverified")
