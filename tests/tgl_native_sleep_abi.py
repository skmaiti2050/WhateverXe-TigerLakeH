#!/usr/bin/env python3
"""Audit the precise native Tiger Lake sleep/wake ABI and call ordering.

Source: unmodified AppleIntelTGLGraphics x86_64 Mach-O, Sequoia 24H232.
The qualified donor SHA makes symbol offsets and disassembly claims local to
the analyzed binary. Static order is NOT evidence of IRQ scheduling at runtime.
No kernel/hardware method is executed here.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('donor', type=Path)
args = parser.parse_args()
data = args.donor.read_bytes()
digest = hashlib.sha256(data).hexdigest()
EXPECTED = '6c8ab6cce50b9529679ee4fdf5b1c37d091677d945076f41cb4b7f8196c51449'
if digest != EXPECTED:
    parser.error('unqualified TGL accelerator binary SHA256 '+digest)

symbols = subprocess.check_output(['nm', '-an', str(args.donor)], text=True)
disassembly = subprocess.check_output(['otool', '-tvV', str(args.donor)], text=True)
routines = {
    'sleep': ('__ZN16IntelAccelerator15systemWillSleepEv', '0000000000028916'),
    'wake': ('__ZN16IntelAccelerator13systemDidWakeEv', '000000000002897a'),
    'power': ('__ZN16IntelAccelerator13setPowerStateEmP9IOService', '000000000002a9b6'),
    'bridge_sleep': ('__ZN17IGInterruptBridge15systemWillSleepEv', '000000000004ab14'),
    'bridge_wake': ('__ZN17IGInterruptBridge13systemDidWakeEv', '000000000004ab40'),
    'scheduler_wake': ('__ZN12IGScheduler513systemDidWakeEv', '000000000003914e'),
    'reset': ('__ZN26IGHardwareCommandStreamer511resetOnWakeEv', '000000000003a292'),
}
for name, (symbol, addr) in routines.items():
    assert re.search(r'^'+addr+r' [Tt] '+symbol+r'$', symbols, re.M), name+' symbol mismatch'

def asm(name):
    symbol = routines[name][0]
    match = re.search(r'^'+re.escape(symbol)+r':\n(.*?)(?=^\S[^\n]*:\n|\Z)', disassembly, re.M | re.S)
    assert match, name+' disassembly missing'
    return match.group(1)

def ordered(name, entries):
    """Require exact instruction addresses in the specified execution order."""
    s = asm(name)
    positions = []
    for address, pattern in entries:
        match = re.search(r'^'+address+r'\s+.*'+pattern, s, re.M)
        assert match, name+' missing instruction '+address+' / '+pattern
        positions.append(match.start())
    assert positions == sorted(positions), name+' instruction order changed'

ordered('sleep', [
    ('000000000002892b', r'disableCollection'),
    ('000000000002893a', r'callq\s+\*0x118\(%rax\)'),
    ('000000000002894b', r'IGStolenMemoryPool5purge'),
    ('000000000002895a', r'callq\s+\*0x9e8\(%rax\)'),
    ('0000000000028967', r'IGInterruptBridge15systemWillSleep'),
    ('000000000002896c', r'movb\s+\$0x0, 0x1c54\(%rbx\)'),
    ('0000000000028979', r'retq'),
])
ordered('wake', [
    ('00000000000289a0', r'IGInterruptBridge13systemDidWake'),
    ('00000000000289af', r'callq\s+\*0x120\(%rax\)'),
    ('00000000000289c1', r'enableCollection'),
    ('00000000000289d6', r'jmpq\s+\*0x9f0\(%rax\)'),
])
ordered('power', [
    ('000000000002aa45', r'callq\s+\*0x118\(%rax\)'),
    ('000000000002aa62', r'IGInterruptBridge15systemWillSleep'),
    ('000000000002aa6a', r'stopGraphicsEngine'),
    ('000000000002aaea', r'callq\s+\*0x120\(%rax\)'),
    ('000000000002aaf7', r'IGInterruptBridge13systemDidWake'),
    ('000000000002aaff', r'startGraphicsEngine'),
])
ordered('scheduler_wake', [
    ('000000000003915e', r'callq\s+\*0x130\(%rax\)'),
    ('000000000003916d', r'IGScheduler527resetCommandStreamersOnWake'),
])
bridge = asm('bridge_sleep')
assert bridge.index('IGAccelEventMachine15finishAllStamps') < bridge.index('IGInterruptBridge7disable'), 'bridge quiesce order changed'
assert 'IGInterruptBridge6enable' in asm('bridge_wake')
reset_asm = asm('reset')
assert re.search(r'\$0x7, %eax', reset_asm), 'native 8-entry head lost'
assert re.search(r'0x1c04', reset_asm) and re.search(r'0xbc', reset_asm), 'native head/tail stores moved'
print('PASS: analyzed donor '+digest)
print('PASS: void IntelAccelerator sleep ABI; bridge quiesce follows native scheduler + purge')
print('PASS: systemDidWake bridge enable precedes scheduler wake; setPowerState(2) uses reverse order')
print('PASS: scheduler wake leads to resetCommandStreamersOnWake; native CSB reset writes 7')
print('NOTE: call order is static; IRQ race and actual Deep Idle path remain unverified')
