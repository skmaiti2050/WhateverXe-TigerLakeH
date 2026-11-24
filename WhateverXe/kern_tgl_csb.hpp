// Tiger Lake host-scheduler CSB intake. Opt-in donor hooks use -tglcsb12.
// Contract: Intel TGL PRM vol.8 / vol.2c and Linux v6.12 execlists CSB handling.
#ifndef kern_tgl_csb_hpp
#define kern_tgl_csb_hpp

#include <stdint.h>

namespace TGLCSB {
constexpr uint32_t Capacity = 12;
constexpr uint32_t ResetIndex = Capacity - 1;
constexpr uint64_t Sentinel = UINT64_MAX;

// The hardware write pointer is separate from the driver's consumed head.
// A wake can preserve the CSB only when the idle tail and software head are
// unchanged from the observed pre-sleep state. No queue entries are dropped.
enum class WakeAction : uint8_t { Initialize, Preserve, RearmAndInitialize };
// CPU-side ownership proof across the early IRQ-off -> system sleep -> wake
// interval. The native streamer provides serialization of its actual queues;
// these counters only reject a sample if another native operation intervenes.
struct Activity {
    uint64_t revision {};
    uint32_t busy {};
    void change() { __atomic_add_fetch(&revision,1ULL,__ATOMIC_ACQ_REL); }
    uint64_t version() const { return __atomic_load_n(&revision,__ATOMIC_ACQUIRE); }
    bool stable(uint64_t expected) const {
        return !__atomic_load_n(&busy,__ATOMIC_ACQUIRE) && version()==expected;
    }
};
struct ActivityScope {
    Activity *activity {};
    explicit ActivityScope(Activity *a=nullptr,bool changes=false) { enter(a,changes); }
    void enter(Activity *a,bool changes) {
        activity=a;
        if(a) { __atomic_add_fetch(&a->busy,1U,__ATOMIC_ACQ_REL); if(changes)a->change(); }
    }
    ~ActivityScope() { if(activity)__atomic_sub_fetch(&activity->busy,1U,__ATOMIC_RELEASE); }
    ActivityScope(const ActivityScope &)=delete;
    ActivityScope &operator=(const ActivityScope &)=delete;
};
// A candidate is produced while the native bridge still owns powered MMIO.
// Only its exact bridge may claim it once, and only after that bridge is off.
// Enable invalidates an unclaimed candidate, not an already bound snapshot.
struct SleepCandidate {
    enum Phase : uint64_t { Idle, Capturing, Canceled, Ready, Claiming, ClaimCanceled };
    static constexpr uint64_t PhaseMask=7;
    uint64_t state {}; // ticket in upper bits, phase in low three bits
    uintptr_t bridge {};
    uint32_t begin(uintptr_t b) {
        if(!b)return 0;
        auto old=__atomic_load_n(&state,__ATOMIC_ACQUIRE);
        for(;;) {
            const auto phase=old&PhaseMask;
            if(phase!=Idle && phase!=Ready)return 0;
            const uint32_t n=static_cast<uint32_t>(old>>3)+1;
            if(!n)return 0; // never reuse a wrapped ticket
            const uint64_t next=(static_cast<uint64_t>(n)<<3)|Capturing;
            if(__atomic_compare_exchange_n(&state,&old,next,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
                __atomic_store_n(&bridge,b,__ATOMIC_RELEASE);return n;
            }
        }
    }
    void publish(uintptr_t b,uint32_t n) {
        if(!b || !n || __atomic_load_n(&bridge,__ATOMIC_ACQUIRE)!=b)return;
        const uint64_t token=static_cast<uint64_t>(n)<<3;
        auto expected=token|Capturing;
        if(!__atomic_compare_exchange_n(&state,&expected,token|Ready,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) && expected==(token|Canceled))
            __atomic_compare_exchange_n(&state,&expected,token|Idle,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    }
    // Only the canonical native bridge calls this. Cancel a concurrent writer
    // or adopter without releasing its exclusive receipt access prematurely.
    void invalidate(uintptr_t b) {
        if(!b)return;
        auto old=__atomic_load_n(&state,__ATOMIC_ACQUIRE);
        for(;;) {
            const auto phase=old&PhaseMask;
            if(phase!=Ready && phase!=Capturing && phase!=Claiming)return;
            if(phase!=Capturing && __atomic_load_n(&bridge,__ATOMIC_ACQUIRE)!=b)return;
            const auto next=(old&~PhaseMask)|(phase==Capturing?Canceled:phase==Claiming?ClaimCanceled:Idle);
            if(__atomic_compare_exchange_n(&state,&old,next,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
        }
    }
    uint32_t claim(uintptr_t b,bool disabled) {
        auto old=__atomic_load_n(&state,__ATOMIC_ACQUIRE);
        if(!b || (old&PhaseMask)!=Ready || __atomic_load_n(&bridge,__ATOMIC_ACQUIRE)!=b)return 0;
        const auto next=(old&~PhaseMask)|(disabled?Claiming:Idle);
        if(!__atomic_compare_exchange_n(&state,&old,next,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
        return disabled?static_cast<uint32_t>(old>>3):0;
    }
    bool completeClaim(uint32_t n) {
        if(!n)return false;
        const uint64_t token=static_cast<uint64_t>(n)<<3;
        auto expected=token|Claiming;
        if(__atomic_compare_exchange_n(&state,&expected,token|Idle,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return true;
        if(expected==(token|ClaimCanceled))__atomic_compare_exchange_n(&state,&expected,token|Idle,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        return false;
    }
};
inline bool emptyBanks(const uintptr_t (&slots)[4]) {
    return !(slots[0] || slots[1] || slots[2] || slots[3]);
}

struct WakeBaseline {
    uint32_t pointer {}, tail {}, head {}, active {};
    bool valid {};
};
struct WakePlan {
    WakeAction action {WakeAction::Initialize};
    bool restoreMode {};
};
constexpr bool hasCSB12Mode(uint32_t mode) {
    return mode != UINT32_MAX && (mode & 8);
}
constexpr WakePlan planMode(uint32_t mode, uint32_t modeAtInit) {
    if (mode == UINT32_MAX || hasCSB12Mode(mode))
        return {WakeAction::Initialize, false};
    return {WakeAction::RearmAndInitialize, hasCSB12Mode(modeAtInit)};
}
inline WakePlan planWake(const WakeBaseline &prior, uint32_t pointer,
                         uint32_t tail, uint32_t mode,
                         uint32_t softwareHead, uint32_t active,
                         uint32_t modeAtInit) {
    const auto modePlan=planMode(mode,modeAtInit);
    if (modePlan.action==WakeAction::RearmAndInitialize ||
        mode==UINT32_MAX)
        return modePlan;
    if (pointer == UINT32_MAX)
        return {WakeAction::Initialize, false};
    const uint32_t write = pointer & 0xF;
    if (!prior.valid || write >= Capacity || tail >= Capacity ||
        softwareHead >= Capacity || pointer != prior.pointer ||
        write != prior.tail || write != tail ||
        write != softwareHead || softwareHead != prior.head ||
        active != prior.active)
        return {WakeAction::Initialize, false};
    return {WakeAction::Preserve, false};
}

// Use the same tail source as the normal CSB drain. In MMIO-tail mode the
// indexed write pointer is authoritative; HWSP+0xBC is not the live tail.
template<class IO>
uint32_t readTail(IO &io) {
    if (!io.mmioTail)
        return io.readHWSPLastWrite();
    io.acquire();
    return io.readMMIOPointer() & 0xF;
}

// GFX_MODE uses a masked write. Re-arm only when the last successful
// initialization proved that bit 3 was enabled, then verify the readback.
template<class IO>
bool ensureCSB12Mode(const WakePlan &plan, IO &io, uint32_t &modeAfter) {
    if (plan.restoreMode) {
        io.writeMode(0x00080008U);
        modeAfter = io.readMode();
    }
    return hasCSB12Mode(modeAfter);
}

// Mask only the Gen11+ read/write fields defined in intel_lrc_reg.h.
// r92's broad 0xffff mask also cleared bits15:12 (observed 7 -> 0),
// after which HWSP and indexed-MMIO events appeared one slot apart.
// Preserve the other fields; their TGL meaning is not established here.
constexpr uint32_t pointerResetWrite(uint32_t index) {
	return 0x0F0F0000U | (index << 8) | index;
}

// TGL format only; Xe_HP has different bit positions. A context identity has
// both an 11-bit ID and a 6-bit submission counter. Never infer lite restore
// from the ID alone, or from a zero low nibble in the donor's translated flags.
struct Identity {
	uint16_t id;
	uint8_t counter;
	bool idle() const { return id == 0x7FF; }
	bool operator==(const Identity &other) const {
		return id == other.id && counter == other.counter;
	}
};

enum class Kind : uint8_t {
	Invalid, IdleToActive, PreemptToIdle, IdlePreempt,
	NewQueueComplete, NewQueueWait, NewQueuePreempt, LiteRestore,
	ElementComplete, ElementWait,
	ActiveToIdleComplete, ActiveToIdleWait, ActiveToIdlePreempt
};

struct Event {
	uint64_t raw;
	Identity away, to;
	uint8_t detail, waitDetail, engineClass, engineInstance;
	bool newQueue, semaphoreWaitMode;
	Kind kind {Kind::Invalid};
};

// Intel TGL PRM vol.8 Context Status table, cases1..10. This classifies the
// immutable raw sample only. It DOES NOT retire work, alter pending queues,
// normalize identities, or select a legacy handler. In particular preempt to
// idle must not be turned into new-queue promotion or inferred completion.
inline Event classify(uint64_t raw) {
	const auto lo = static_cast<uint32_t>(raw);
	const auto hi = static_cast<uint32_t>(raw >> 32);
	Event event {raw,
		{static_cast<uint16_t>((hi >> 15) & 0x7FF), static_cast<uint8_t>(hi >> 26)},
		{static_cast<uint16_t>((lo >> 15) & 0x7FF), static_cast<uint8_t>(lo >> 26)},
		static_cast<uint8_t>(hi & 15), static_cast<uint8_t>((hi >> 5) & 127),
		static_cast<uint8_t>((lo >> 3) & 7), static_cast<uint8_t>((lo >> 6) & 63),
		(lo & 1) != 0, (lo & 4) != 0, Kind::Invalid};
	if (raw == Sentinel)
		return event;
	if (event.newQueue) {
		if (event.away.idle()) {
			// PRM3 switch detail is X; no active context can be retired.
			if (event.to.idle()) event.kind = Kind::IdlePreempt;
			else if (!event.detail) event.kind = Kind::IdleToActive;
		} else if (event.to.idle()) {
			if (event.detail <= 5) event.kind = Kind::PreemptToIdle;
		} else if (event.away == event.to) {
			// PRM7 switch detail is N/A; same submission continues.
			event.kind = Kind::LiteRestore;
		} else if (!event.detail) event.kind = Kind::NewQueueComplete;
		else if (event.detail <= 4) event.kind = Kind::NewQueueWait;
		else if (event.detail == 5) event.kind = Kind::NewQueuePreempt;
	} else if (!event.away.idle()) {
		if (event.to.idle()) {
			if (!event.detail) event.kind = Kind::ActiveToIdleComplete;
			else if (event.detail <= 4) event.kind = Kind::ActiveToIdleWait;
			else if (event.detail == 5) event.kind = Kind::ActiveToIdlePreempt;
		} else if (!(event.away == event.to)) {
			if (!event.detail) event.kind = Kind::ElementComplete;
			else if (event.detail <= 4) event.kind = Kind::ElementWait;
		}
	}
	return event;
}

struct LegacyEntry { uint32_t flags, identity; };

// Preserve the exact donor entry ABI, but derive every field from ONE snapshot.
// A handler is selected from Event::kind, never BSF(flags & 15). This conversion
// is not permission to dispatch: the raw identity and queue state need validation.
inline LegacyEntry legacyEntry(const Event &event) {
	const uint32_t lo = static_cast<uint32_t>(event.raw);
	const uint32_t hi = static_cast<uint32_t>(event.raw >> 32);
	const bool awayValid = !event.away.idle(), toValid = !event.to.idle();
	const bool sameSubmission = event.away == event.to;
	uint32_t flags = !awayValid && toValid ? 1 : 0;
	if (event.detail == 5 || (awayValid && event.newQueue)) flags |= 2;
	if (awayValid && toValid && !sameSubmission && !event.newQueue) flags |= 4;
	if (awayValid && !toValid && !event.newQueue) flags |= 8;
	if (awayValid && !event.detail) flags |= 0x10;
	if (event.detail == 1) flags |= 0x20;
	if (event.detail == 2) flags |= 0x40;
	if (event.detail == 3) flags |= 0x100;
	if (event.detail == 4) flags |= 0x80;
	if (awayValid && sameSubmission) flags |= 0x8000;
	flags |= (lo & 4) << 9;
	const uint32_t identity = ((lo & 0xF8) << 26) | ((lo << 10) & 0x3F0000)
		| ((hi >> 3) & 0x1F800000) | ((hi >> 10) & 0xFFE0) | ((hi >> 12) & 7);
	return {flags, identity};
}

enum class Handler : uint8_t {
	Invalid, IdleToActive, NewQueue, ElementSwitch, ActiveToIdle,
	IdlePreempt, IncompleteToIdle, IncompleteElementSwitch
};

// The native handlers retain detail4 in their dependency wait queue and skip
// queue-capacity retirement. Only qualify signal mode; polling switches and
// other unfinished causes require separate evidence. The caller also checks
// owner flags: 0x18000 disables that native wait-queue path.
inline bool nativeSemaphoreWait(const Event &event) {
	return event.detail == 4 && event.semaphoreWaitMode &&
		(event.kind == Kind::ActiveToIdleWait || event.kind == Kind::ElementWait);
}

inline Handler handlerFor(const Event &event) {
	switch (event.kind) {
		case Kind::IdleToActive: return Handler::IdleToActive;
		case Kind::NewQueueComplete: case Kind::NewQueueWait:
		case Kind::NewQueuePreempt: case Kind::LiteRestore: return Handler::NewQueue;
		case Kind::ElementComplete: return Handler::ElementSwitch;
		case Kind::ElementWait: return nativeSemaphoreWait(event)
			? Handler::ElementSwitch : Handler::IncompleteElementSwitch;
		case Kind::ActiveToIdleComplete: return Handler::ActiveToIdle;
		case Kind::IdlePreempt: return Handler::IdlePreempt;
		// Existing ActiveToIdle retires work for waits other than semaphore wait;
		// existing Preempted promotes a pending bank. Neither is a general handler
		// for these cases. They need explicit unfinished-work/idle accounting.
		case Kind::ActiveToIdleWait: return nativeSemaphoreWait(event)
			? Handler::ActiveToIdle : Handler::IncompleteToIdle;
		case Kind::ActiveToIdlePreempt:
		case Kind::PreemptToIdle: return Handler::IncompleteToIdle;
		case Kind::Invalid: return Handler::Invalid;
	}
	return Handler::Invalid;
}

enum class Error : uint8_t { None, NotInitialized, InvalidPointer, Unavailable, Rejected, ResetMismatch };
// Native submitExecList returns void: an unready CSB12 queue has no valid
// completion/error path at this call boundary, so it must not be submitted.
constexpr bool canSubmit(bool hardwareReady, Error error) {
    return hardwareReady && error == Error::None;
}
enum class Source : uint8_t { HWSP, RetriedHWSP, MMIO };

struct Sample {
	uint64_t raw {Sentinel};
	uint32_t index {};
	uint8_t retries {};
	Source source {Source::HWSP};
};

struct State {
	uint32_t head {ResetIndex};
	Error error {Error::NotInitialized};
};

// Offset relative to the first CSB MMIO bank, not the engine base.
constexpr uint32_t mmioOffset(uint32_t index) {
	return index < 6 ? index * 8 : 0x50 + (index - 6) * 8;
}

// Caller holds the streamer's existing serialization and has quiesced the GPU.
// IO resetPointers must use the masked register write and a posting read; flush
// must finish CPU writes to HWSP. It must NOT reset contexts or retire work.
// The native reset's context ownership operations still have to run separately.
template<class IO>
void initialize(State &state, IO &io) {
	state.error = Error::NotInitialized;
	io.resetPointers(ResetIndex);
	io.writeTail(ResetIndex);
	for (uint32_t i = 0; i < Capacity; ++i)
		io.writeEntry(i, Sentinel);
	io.flushEntriesAndTail();
	io.resetPointers(ResetIndex);
	state.head = ResetIndex;
	state.error = Error::None;
}

// An S0 fallback reset is not proof of GPU reset. Confirm the two documented
// pointer nibbles and the CPU-initialized HWSP tail before admitting work.
constexpr bool resetMatches(uint32_t pointer,uint32_t hwspTail) {
    return pointer!=UINT32_MAX && (pointer&0x0F0FU)==((ResetIndex<<8)|ResetIndex) && hwspTail==ResetIndex;
}

// IO readEntry is one aligned 64-bit HWSP load. nowNs is monotonic; delayUs(1)
// must not sleep while an interrupt lock is held. Both a time limit and a poll
// limit bound retry; this is at most ten 1us delays, not a wall-clock guarantee.
// IO readMMIO64 uses the indexed register pair, NEVER the FIFO/pop register.
template<class IO>
Sample read(uint32_t index, IO &io) {
	Sample sample;
	sample.index = index;
	if (index >= Capacity)
		return sample;
	sample.raw = io.readEntry(index);
	if (sample.raw != Sentinel)
		return sample;
	const uint64_t start = io.nowNs();
	while (sample.raw == Sentinel && sample.retries < 10) {
		const uint64_t elapsed = io.nowNs() - start;
		if (elapsed >= 10000)
			break;
		io.delayUs(1);
		++sample.retries;
		sample.raw = io.readEntry(index);
	}
	sample.source = Source::RetriedHWSP;
	if (sample.raw == Sentinel) {
		sample.source = Source::MMIO;
		sample.raw = io.readMMIO64(mmioOffset(index));
	}
	return sample;
}

// readTail returns a decoded index (low 4 bits for the MMIO pointer path).
// readFence must order the tail load before the first entry load (x86 lfence).
// accept consumes the immutable Sample, validates identity/queue ownership, and
// then dispatches the native handler. False means terminal inconsistency: the
// caller must take a justified native failure/reset path, never retry dispatch.
// This helper neither translates an event nor chooses a native handler.
template<class IO, class Consumer>
Error drain(State &state, IO &io, Consumer &accept) {
	if (state.error != Error::None)
		return state.error;
	const uint32_t tail = io.readTail();
	if (tail >= Capacity || state.head >= Capacity)
		return state.error = Error::InvalidPointer;
	if (state.head == tail)
		return Error::None;
	io.readFence();
	while (state.head != tail) {
		const uint32_t next = state.head + 1 == Capacity ? 0 : state.head + 1;
		const auto sample = read(next, io);
		if (sample.raw == Sentinel)
			return state.error = Error::Unavailable;
		// Poison BEFORE dispatch can submit more GPU work and cause slot reuse.
		// The integration must preserve the submission-side write barrier.
		io.writeEntry(next, Sentinel);
		state.head = next;
		if (!accept(sample))
			return state.error = Error::Rejected;
	}
	return Error::None;
}
}
#endif
