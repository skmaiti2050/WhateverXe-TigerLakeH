// CPU-only tests: clang++ -std=c++14 -fsanitize=address,undefined tests/tgl_csb.cpp
#include "../WhateverXe/kern_tgl_csb.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace TGLCSB;

struct IO {
	std::array<uint64_t, Capacity> slots {};
	std::array<uint64_t, 16> mmio {};
	uint32_t tail {ResetIndex}, tailReads {}, entryReads {}, mmioReads {}, writes {}, delays {};
	uint64_t clock {}, publishAt {UINT64_MAX}, publishValue {0x123456789abcdef0ULL};
	uint32_t publishIndex {};
	bool fenced {}, advanceClock {true};
	std::vector<unsigned> resetOrder;
	void resetPointers(uint32_t n) { assert(n == 11); resetOrder.push_back(1); }
	void writeTail(uint32_t n) { tail = n; resetOrder.push_back(2); }
	void flushEntriesAndTail() { resetOrder.push_back(3); }
	uint32_t readTail() { ++tailReads; fenced = false; return tail; }
	void readFence() { fenced = true; }
	uint64_t nowNs() { return clock; }
	void delayUs(unsigned n) {
		assert(n == 1); ++delays;
		if (advanceClock) clock += 1000;
		if (clock >= publishAt) slots[publishIndex] = publishValue;
	}
	uint64_t readEntry(uint32_t i) { assert(fenced); ++entryReads; return slots.at(i); }
	void writeEntry(uint32_t i, uint64_t n) { slots.at(i) = n; ++writes; }
	uint64_t readMMIO64(uint32_t offset) {
		assert(fenced && offset % 8 == 0); ++mmioReads;
		return mmio.at(offset / 8);
	}
};

struct Consumer {
	IO &io;
	explicit Consumer(IO &source) : io(source) {}
	std::vector<Sample> samples;
	unsigned rejectAt {99};
	bool operator()(const Sample &sample) {
		assert(io.slots.at(sample.index) == Sentinel);
		samples.push_back(sample);
		return samples.size() != rejectAt;
	}
};

static void fresh(State &state, IO &io) {
	initialize(state, io);
	assert(state.head == 11 && state.error == Error::None && io.tail == 11);
	assert((io.resetOrder == std::vector<unsigned>{1, 2, 3, 1}));
	for (auto v : io.slots) assert(v == Sentinel);
	io.writes = 0;
}

struct TailSourceIO {
    bool mmioTail {};
    uint32_t hwspTail {}, mmioPointer {};
    unsigned acquireCalls {}, hwspReads {}, mmioReads {};
    void acquire() { ++acquireCalls; }
    uint32_t readHWSPLastWrite() { ++hwspReads; return hwspTail; }
    uint32_t readMMIOPointer() { ++mmioReads; return mmioPointer; }
};

struct ModeIO {
    uint32_t mode {}, writes {}, lastWrite {};
    bool latchWrite {true};
    void writeMode(uint32_t value) {
        ++writes; lastWrite=value;
        if (latchWrite) {
            const uint32_t mask=value>>16;
            mode=(mode&~mask)|(value&mask);
        }
    }
    uint32_t readMode() { return mode; }
};

static void sleepCandidateTests() {
    SleepCandidate c;
    const uintptr_t a=0x1000,b=0x2000;
    auto t=c.begin(a);assert(t&&c.begin(a)==0);c.publish(a,t);
    assert(c.claim(b,true)==0&&c.claim(a,true)==t&&c.claim(a,true)==0);
    assert(!c.begin(a)&&c.completeClaim(t)&&!c.claim(a,true));
    t=c.begin(a);c.invalidate(a);assert(c.begin(a)==0);c.publish(a,t);assert(!c.claim(a,true));
    t=c.begin(a);c.publish(a,t);c.invalidate(b);assert(c.claim(a,true)==t&&c.completeClaim(t));
    t=c.begin(a);c.publish(a,t);c.invalidate(a);assert(!c.claim(a,true));
    t=c.begin(a);c.publish(a,t);assert(!c.claim(a,false)&&!c.claim(a,true));
    t=c.begin(a);c.publish(a,t);auto next=c.begin(a);assert(next>t);c.publish(a,t);assert(!c.claim(a,true));c.publish(a,next);assert(c.claim(a,true)==next&&c.completeClaim(next));
    t=c.begin(a);c.publish(a,t);assert(c.claim(a,true)==t);c.invalidate(a);assert(!c.begin(a)&&!c.completeClaim(t));
    next=c.begin(a);assert(next>t);c.publish(a,next);assert(c.claim(a,true)==next&&c.completeClaim(next));
    Activity activity;auto version=activity.version();assert(activity.stable(version));
    {ActivityScope read(&activity);assert(!activity.stable(version));}assert(activity.stable(version));
    {ActivityScope outer(&activity,true);auto v=activity.version();{ActivityScope nested(&activity,true);assert(!activity.stable(v));}assert(!activity.stable(activity.version()));}
    assert(!activity.stable(version)&&activity.stable(activity.version()));
    assert(resetMatches(0x8b0b,11)&&resetMatches(0x0b0b,11));
    assert(!resetMatches(0x8b08,11)&&!resetMatches(0x880b,11)&&!resetMatches(UINT32_MAX,11)&&!resetMatches(0x8b0b,8));
    assert(!canSubmit(false,Error::ResetMismatch)&&!canSubmit(true,Error::ResetMismatch));
}

static void wakeTailSourceTests() {
    TailSourceIO hwsp;
    hwsp.hwspTail=9; hwsp.mmioPointer=0x0000000B;
    assert(readTail(hwsp)==9);
    assert(hwsp.hwspReads==1 && hwsp.mmioReads==0 && hwsp.acquireCalls==0);

    TailSourceIO mmio;
    mmio.mmioTail=true; mmio.hwspTail=4; mmio.mmioPointer=0x0000090B;
    assert(readTail(mmio)==0xB);
    assert(mmio.hwspReads==0 && mmio.mmioReads==1 && mmio.acquireCalls==1);
}

static void wakeTests() {
    // A failed 12-entry mode guard must never be treated as a valid
    // native void submit or GPU-reset resubmission.
    assert(canSubmit(true,Error::None));
    assert(!canSubmit(false,Error::None));
    for(auto error : {Error::NotInitialized,Error::InvalidPointer,
                      Error::Unavailable,Error::Rejected})
        assert(!canSubmit(true,error));
    const WakeBaseline before{0x00000909, 9, 9, 1, true};
    const WakeBaseline noSnapshot{};

    // Every initialization entry point must verify the mode, including retries
    // reached from submit after a previous wake could not re-arm GFX_MODE.
    const auto initializedMode=planMode(0x8,UINT32_MAX);
    assert(initializedMode.action==WakeAction::Initialize && !initializedMode.restoreMode);
    const auto retryRearm=planMode(0x0,0x8);
    assert(retryRearm.action==WakeAction::RearmAndInitialize && retryRearm.restoreMode);
    ModeIO retry; uint32_t retryMode=0;
    assert(ensureCSB12Mode(retryRearm,retry,retryMode));
    assert(retry.writes==1 && retryMode==8);
    const auto retryUnknown=planMode(0x0,UINT32_MAX);
    assert(retryUnknown.action==WakeAction::RearmAndInitialize && !retryUnknown.restoreMode);
    ModeIO blockedRetry; retryMode=0;
    assert(!ensureCSB12Mode(retryUnknown,blockedRetry,retryMode));
    assert(blockedRetry.writes==0);
    const auto invalidMode=planMode(UINT32_MAX,0x8);
    assert(invalidMode.action==WakeAction::Initialize && !invalidMode.restoreMode);
    ModeIO invalid; retryMode=UINT32_MAX;
    assert(!ensureCSB12Mode(invalidMode,invalid,retryMode));
    assert(invalid.writes==0);

    auto plan=planWake(before, 0x00000909, 9, 0x8, 9, 1, 0x8);
    assert(plan.action==WakeAction::Preserve && !plan.restoreMode);
    assert(planWake(before, 0x00000809, 9, 0x8, 9, 1, 0x8).action==WakeAction::Initialize);
    assert(planWake(before, 0x0000090A, 10, 0x8, 9, 1, 0x8).action==WakeAction::Initialize);
    assert(planWake(before, 0x00000909, 8, 0x8, 9, 1, 0x8).action==WakeAction::Initialize);
    assert(planWake(before, 0x00000909, 9, 0x8, 8, 1, 0x8).action==WakeAction::Initialize);
    assert(planWake(before, 0x00000909, 9, 0x8, 9, 0, 0x8).action==WakeAction::Initialize);
    assert(planWake(noSnapshot, 0, 0, 0x8, 0, 0, 0x8).action==WakeAction::Initialize);
    assert(planWake(before, UINT32_MAX, 9, 0x8, 9, 1, 0x8).action==WakeAction::Initialize);
    assert(planWake(before, 0x00000909, 9, UINT32_MAX, 9, 1, 0x8).action==WakeAction::Initialize);

    // A missing sleep baseline must not suppress mode restoration when the
    // last successful initialization confirmed GFX_MODE bit 3.
    const auto rearm=planWake(noSnapshot, 0x00000909, 9, 0x0, 9, 1, 0x8);
    assert(rearm.action==WakeAction::RearmAndInitialize && rearm.restoreMode);
    ModeIO restored; restored.mode=0xA5A50002U; uint32_t modeAfter=0;
    assert(ensureCSB12Mode(rearm,restored,modeAfter));
    assert(restored.writes==1 && restored.lastWrite==0x00080008U &&
           modeAfter==0xA5A5000AU);

    // A failed readback is not treated as a successful re-arm.
    ModeIO failed; failed.latchWrite=false; modeAfter=0;
    assert(!ensureCSB12Mode(rearm,failed,modeAfter));
    assert(failed.writes==1 && modeAfter==0);

    // Never invent a prior 12-entry mode when initialization did not prove it.
    const auto unknown=planWake(noSnapshot,0x00000909,9,0x0,9,1,UINT32_MAX);
    assert(unknown.action==WakeAction::RearmAndInitialize && !unknown.restoreMode);
    ModeIO unknownMode; modeAfter=0;
    assert(!ensureCSB12Mode(unknown,unknownMode,modeAfter));
    assert(unknownMode.writes==0);

    const auto disabled=planWake(noSnapshot,0x00000909,9,0x0,9,1,0x0);
    assert(!disabled.restoreMode);
    ModeIO disabledMode; modeAfter=0;
    assert(!ensureCSB12Mode(disabled,disabledMode,modeAfter));
    assert(disabledMode.writes==0);

    // If the register already reports the 12-entry mode, no masked write is needed.
    const auto already=planWake(noSnapshot,0x00000909,9,0x8,9,1,UINT32_MAX);
    ModeIO alreadyMode; alreadyMode.mode=8; modeAfter=8;
    assert(ensureCSB12Mode(already,alreadyMode,modeAfter));
    assert(alreadyMode.writes==0 && modeAfter==8);
}

static void eventTests() {
	const unsigned idle = 0x7FF;
	auto check = [](unsigned away, unsigned to, bool newQueue, unsigned detail,
	                Kind expected, unsigned awayCounter = 0, unsigned toCounter = 0) {
		const uint64_t raw = (static_cast<uint64_t>(away << 15 | awayCounter << 26 | detail) << 32)
			| to << 15 | toCounter << 26 | static_cast<unsigned>(newQueue);
		const auto event = classify(raw);
		assert(event.raw == raw && event.kind == expected);
		assert(event.away.id == away && event.to.id == to);
		assert(event.away.counter == awayCounter && event.to.counter == toCounter);
		assert(event.detail == detail && event.newQueue == newQueue);
	};
	check(idle, 0xAB, true, 0, Kind::IdleToActive); // PRM1
	for (unsigned detail = 0; detail <= 5; ++detail)
		check(0xAB, idle, true, detail, Kind::PreemptToIdle); // PRM2
	for (unsigned detail = 0; detail <= 15; ++detail) {
		check(idle, idle, true, detail, Kind::IdlePreempt); // PRM3 detail X
		check(0xAB, 0xAB, true, detail, Kind::LiteRestore); // PRM7 detail N/A
	}
	check(0xAB, 0x7BC, true, 0, Kind::NewQueueComplete); // PRM4
	check(0xAB, 0x7BC, true, 5, Kind::NewQueuePreempt); // PRM5
	for (unsigned detail = 1; detail <= 4; ++detail) {
		check(0xAB, 0x7BC, true, detail, Kind::NewQueueWait); // PRM6
		check(0xAB, 0x7AC, false, detail, Kind::ElementWait); // PRM9
		check(0xAB, idle, false, detail, Kind::ActiveToIdleWait); // PRM10
	}
	// Captured r117 RCS event: ID6 -> idle, signal semaphore wait, NOT complete.
	const auto semaphore = classify(0x0003000403FF8004ULL);
	assert(nativeSemaphoreWait(semaphore));
	assert(handlerFor(semaphore) == Handler::ActiveToIdle);
	assert(legacyEntry(semaphore).flags == 0x888);
	assert(legacyEntry(semaphore).identity == 0xC0);
	assert(handlerFor(classify(0x0003000400038004ULL)) == Handler::ElementSwitch);
	for (unsigned detail = 1; detail <= 5; ++detail) {
		const auto raw = (static_cast<uint64_t>(0x30000 | detail) << 32) | 0x03FF8004;
		assert(handlerFor(classify(raw)) == (detail == 4
			? Handler::ActiveToIdle : Handler::IncompleteToIdle));
	}
	check(0xAB, 0x7AC, false, 0, Kind::ElementComplete); // PRM8
	check(0xAB, idle, false, 0, Kind::ActiveToIdleComplete); // PRM10
	check(0xAB, idle, false, 5, Kind::ActiveToIdlePreempt); // PRM10 footnote
	// Same context ID with distinct submission counters is not lite restore.
	check(2, 2, true, 0, Kind::NewQueueComplete, 0, 1);
	check(2, 2, false, 0, Kind::ElementComplete, 0, 1);
	check(2, 2, true, 0, Kind::NewQueueComplete, 63, 0);
	// Captured r82 event3: keep an explicit invalid classification. No bypass.
	assert(classify(0x0001000000010000ULL).kind == Kind::Invalid);
	for (unsigned detail = 6; detail <= 15; ++detail) {
		check(1, 2, false, detail, Kind::Invalid);
		check(1, idle, true, detail, Kind::Invalid);
	}
	check(idle, 1, false, 0, Kind::Invalid);
	check(1, 2, false, 5, Kind::Invalid);
	assert(classify(Sentinel).kind == Kind::Invalid);
	// Payload metadata survives classification, even when a handler ignores it.
	const auto metadata = classify((0xABCDEF04ULL << 32) | 0xFC015BFCULL);
	assert(metadata.engineClass == 7 && metadata.engineInstance == 47);
	assert(metadata.semaphoreWaitMode && metadata.waitDetail == 120);
}

int main(int argc, char **argv) {
	// The external exact-donor audit uses this to compare ABI translation.
	if (argc == 2) {
		const auto event = classify(strtoull(argv[1], nullptr, 16));
		const auto entry = legacyEntry(event);
		printf("%08x:%08x\n", entry.identity, entry.flags);
		return 0;
	}
	sleepCandidateTests();
	wakeTailSourceTests();
	wakeTests();
	eventTests();
	// Model masked-register semantics, including r89's observed 0x7007.
	// Only documented read/write fields may change. This does not model GPU
	// ring behavior; the predicted HWSP/MMIO alignment needs a live test.
	for (unsigned old = 0; old <= 0xFFFF; ++old) {
		const auto write = pointerResetWrite(ResetIndex);
		const auto mask = write >> 16;
		const auto after = (old & ~mask) | (write & mask);
		assert((after & 0x0F0F) == 0x0B0B);
		assert((after & 0xF0F0) == (old & 0xF0F0));
		if (old == 0x7007) assert(after == 0x7B0B);
	}
	assert(handlerFor(classify(0x0000800503FF8000ULL)) == Handler::IncompleteToIdle);
	assert(handlerFor(classify(0x03FF800003FF8001ULL)) == Handler::IdlePreempt);
	assert(handlerFor(classify(0x0001000000010000ULL)) == Handler::Invalid);
	for (unsigned detail = 1; detail <= 4; ++detail) {
		assert(handlerFor(classify((static_cast<uint64_t>(0x8000 | detail) << 32) | 0x10000))
			== Handler::IncompleteElementSwitch);
	}
	// Feed the historical sequence through intake and classification together.
	// Event3's unexplained same-submission transition must not be skipped or
	// promoted into a made-up lite restore; event4 remains undispatched.
	{
		State state; IO io; fresh(state, io); io.tail = 4;
		const uint64_t sequence[] {0x03FF800000008001ULL, 0x0000800003FF8000ULL,
			0x03FF800000010001ULL, 0x0001000000010000ULL, 0x0001000003FF8000ULL};
		for (unsigned i = 0; i < 5; ++i) io.slots[i] = sequence[i];
		unsigned accepted = 0, rejected = 0;
		auto accept = [&](const Sample &sample) {
			if (classify(sample.raw).kind == Kind::Invalid) { ++rejected; return false; }
			++accepted; return true;
		};
		assert(drain(state, io, accept) == Error::Rejected);
		assert(accepted == 3 && rejected == 1 && state.head == 3);
		assert(io.slots[4] == sequence[4]);
		assert(drain(state, io, accept) == Error::Rejected && rejected == 1);
	}
	// Independent expected modular order for every valid head/tail combination.
	for (unsigned head = 0; head < 12; ++head) {
		for (unsigned tail = 0; tail < 12; ++tail) {
			State state; IO io; fresh(state, io); Consumer accept {io};
			state.head = head; io.tail = tail;
			for (unsigned i = 0; i < 12; ++i) io.slots[i] = 0x100000000ULL + i;
			assert(drain(state, io, accept) == Error::None);
			assert(accept.samples.size() == (tail + 12 - head) % 12);
			for (unsigned i = 0; i < accept.samples.size(); ++i) {
				assert(accept.samples[i].index == (head + 1 + i) % 12);
				assert(accept.samples[i].raw == 0x100000000ULL + accept.samples[i].index);
			}
			assert(io.tailReads == 1 && io.mmioReads == 0 && io.delays == 0);
			assert(io.writes == accept.samples.size());
			const auto count = accept.samples.size();
			assert(drain(state, io, accept) == Error::None && accept.samples.size() == count);
		}
	}
	// Tail visible before the entry; bounded HWSP retry recovers the later write.
	for (unsigned delay = 1; delay <= 10; ++delay) {
		State state; IO io; fresh(state, io); Consumer accept {io};
		io.tail = 0; io.publishAt = delay * 1000;
		assert(drain(state, io, accept) == Error::None);
		assert(accept.samples[0].raw == io.publishValue);
		assert(accept.samples[0].source == Source::RetriedHWSP);
		assert(io.delays == delay && io.mmioReads == 0);
	}
	// Independent PRM bank offsets. Invalid gap contents must never be selected.
	const unsigned offsets[12] {0,8,16,24,32,40,80,88,96,104,112,120};
	for (unsigned index = 0; index < 12; ++index) {
		State state; IO io; fresh(state, io); Consumer accept {io};
		state.head = (index + 11) % 12; io.tail = index;
		io.mmio.fill(Sentinel); io.mmio[offsets[index] / 8] = 0x200000000ULL + index;
		assert(drain(state, io, accept) == Error::None);
		assert(accept.samples[0].raw == 0x200000000ULL + index);
		assert(accept.samples[0].source == Source::MMIO && io.delays == 10);
	}
	// Unavailable data is terminal until reset; no handler or repeated dispatch.
	{
		State state; IO io; fresh(state, io); Consumer accept {io};
		io.tail = 0; io.mmio.fill(Sentinel); io.advanceClock = false;
		assert(drain(state, io, accept) == Error::Unavailable && io.delays == 10);
		assert(state.head == 11 && accept.samples.empty() && io.writes == 0);
		const auto reads = io.entryReads;
		assert(drain(state, io, accept) == Error::Unavailable && io.entryReads == reads);
	}
	// Rejection must not process the rest of the batch or replay consumed work.
	{
		State state; IO io; fresh(state, io); Consumer accept {io};
		io.tail = 3; io.slots.fill(0x1234); accept.rejectAt = 2;
		assert(drain(state, io, accept) == Error::Rejected);
		assert(state.head == 1 && accept.samples.size() == 2 && io.slots[2] == 0x1234);
		assert(drain(state, io, accept) == Error::Rejected && accept.samples.size() == 2);
		io.resetOrder.clear(); fresh(state, io);
		assert(drain(state, io, accept) == Error::None && accept.samples.size() == 2);
	}
	for (unsigned bad : {12U, 15U, UINT32_MAX}) {
		State state; IO io; fresh(state, io); Consumer accept {io}; io.tail = bad;
		assert(drain(state, io, accept) == Error::InvalidPointer && io.entryReads == 0);
	}
	// Several wraps with reuse must deliver new immutable values exactly once.
	{
		State state; IO io; fresh(state, io); Consumer accept {io};
		for (unsigned sequence = 0; sequence < 48; ++sequence) {
			io.tail = sequence % 12;
			io.slots[io.tail] = 0x100000000ULL + sequence;
			assert(drain(state, io, accept) == Error::None);
			assert(accept.samples.size() == sequence + 1);
			assert(accept.samples.back().raw == 0x100000000ULL + sequence);
		}
	}
	// Tail is a snapshot: arrivals during dispatch belong to the next batch.
	{
		State state; IO io; fresh(state, io);
		io.tail = 0; io.slots[0] = 42;
		unsigned calls = 0;
		auto accept = [&](const Sample &sample) {
			assert(sample.index == calls && sample.raw == 42 + calls);
			++calls;
			if (calls == 1) { io.slots[1] = 43; io.tail = 1; }
			return true;
		};
		assert(drain(state, io, accept) == Error::None && calls == 1);
		assert(drain(state, io, accept) == Error::None && calls == 2);
	}
	{
		State state; IO io; fresh(state, io); Consumer accept {io}; state.head = 12;
		assert(drain(state, io, accept) == Error::InvalidPointer && io.entryReads == 0);
	}
	{
		State state; IO io; Consumer accept {io};
		assert(drain(state, io, accept) == Error::NotInitialized && io.tailReads == 0);
	}
	puts("CSB intake and PRM event classification passed, including r82 rejection without replay");
}
