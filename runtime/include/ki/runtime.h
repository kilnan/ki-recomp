// Guest runtime for statically recompiled Killer Instinct (R4600, little-endian) code.
//
// Model (see docs/TECHNICAL_NOTE.md for the evidence behind each choice):
// - 32 GPRs + HI/LO are 64-bit; 32-bit operations sign-extend their result.
// - Effective addresses are 32-bit (the game runs in 32-bit kernel mode).
// - KSEG0/KSEG1 map to physical by masking with 0x1fffffff. Of the TLB-mapped
//   segments only the single page the game maps at boot is modelled (VA
//   0x0000-0x0fff -> PA 0x08090000); other mapped addresses raise Fault::Tlb.
// - Physical map from MAME src/mame/rare/kinst.cpp (kinst_map):
//     0x00000000-0x0007ffff  SRAM 512 KiB (also holds the two framebuffers)
//     0x08000000-0x087fffff  DRAM 8 MiB (game code decompressed here)
//     0x10000080-0x1000017f  I/O (inputs, DIP, sound latch, IDE)
//     0x1fc00000-0x1fc7ffff  boot EPROM
//   Unmapped reads return all ones (map.unmap_value_high()).
// - Guest memory is stored in guest byte order (little-endian). All supported
//   hosts are little-endian too, so accesses are plain memcpy.
#pragma once

#include <bit>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>

static_assert(std::endian::native == std::endian::little,
              "the runtime stores guest (little-endian) memory in host order");

namespace ki {

constexpr uint32_t kSramBase = 0x00000000, kSramSize = 0x00080000;
constexpr uint32_t kDramBase = 0x08000000, kDramSize = 0x00800000;
constexpr uint32_t kZeroPagePhys = 0x08090000, kZeroPageSize = 0x1000;

struct Context;

// The board around the CPU: I/O space (physical addresses outside SRAM/DRAM;
// size is 1, 2, 4 or 8) and the external interrupt lines.
struct IoHandler {
    virtual ~IoHandler() = default;
    virtual uint64_t read(Context& ctx, uint32_t phys, int size) = 0;
    virtual void write(Context& ctx, uint32_t phys, int size, uint64_t value) = 0;
    // Cause.IP bits (mask 0x7c00 for Int0..Int4) driven by the board at ctx.cycles.
    virtual uint32_t interrupt_lines(Context&) { return 0; }
    // Cycle count at which the board next needs attention (line changes, frames).
    virtual uint64_t next_event(Context&) { return ~0ull; }
    // Called from safe points once ctx.cycles reaches next_event().
    virtual void update(Context&) {}
};

enum class Fault { AddressErrorLoad, AddressErrorStore, Overflow, Tlb, Unimplemented, Syscall, Break, Trap };

struct Context {
    uint64_t r[32] = {};
    uint64_t hi = 0, lo = 0;
    // COP1 with Status.FR = 1 (set by the game at boot): 32 64-bit FPRs. Only
    // single precision is used by the game; a single lives in the low 32 bits
    // and writes leave the high half untouched (as MAME's DRC does).
    uint64_t f[32] = {};
    uint32_t fcsr = 0;  // FCR31: rounding mode in bits 0-1, condition bit 23
    // COP0 registers (Count and Cause are derived on read, see mfc0).
    uint64_t cp0[32] = {};
    // Guest time: one cycle per executed instruction (added by generated code).
    uint64_t cycles = 0;
    uint64_t count_base = 0;           // Count = count_base + cycles / 2
    uint32_t last_count = 0;           // Count at the last timer check
    uint64_t next_event = ~0ull;       // poll() calls service() when cycles reach it
    std::vector<uint8_t> sram = std::vector<uint8_t>(kSramSize);
    std::vector<uint8_t> dram = std::vector<uint8_t>(kDramSize);
    IoHandler* io = nullptr;
};

// Raised for guest exceptions. Exception delivery to the guest's own vector is
// not modelled yet; the default implementation throws ki::GuestFault.
[[noreturn]] void raise_fault(Context& ctx, Fault fault, uint32_t pc, uint32_t addr = 0);
// Calls through a computed address (jalr, jr to non-ra, jal to an
// untranslated function). Looks the address up in the function table.
void call_indirect(Context& ctx, uint32_t target, uint32_t pc);

struct GuestFault {
    Fault fault;
    uint32_t pc;
    uint32_t addr;
};

using RecompFunc = void (*)(Context&);
struct FuncEntry {
    uint32_t vaddr;
    RecompFunc fn;
};
// Defined by generated code.
extern const FuncEntry kFunctionTable[];
extern const std::size_t kFunctionTableSize;
RecompFunc lookup_function(uint32_t vaddr);

// ---- arithmetic helpers (no undefined behaviour) ---------------------------

inline uint64_t sx32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }
inline uint64_t sx16(uint16_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(v))); }
inline uint64_t sx8(uint8_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(v))); }
inline uint32_t lo32(uint64_t v) { return static_cast<uint32_t>(v); }
inline int64_t s64(uint64_t v) { return static_cast<int64_t>(v); }

// Arithmetic right shifts: C++20 defines >> on negative signed values as arithmetic.
inline uint64_t sra32(uint64_t v, unsigned sa) { return sx32(static_cast<uint32_t>(static_cast<int32_t>(lo32(v)) >> (sa & 31))); }
inline uint64_t dsra(uint64_t v, unsigned sa) { return static_cast<uint64_t>(s64(v) >> (sa & 63)); }

inline bool add32_overflows(uint32_t a, uint32_t b) {
    uint32_t s = a + b;
    return ((~(a ^ b) & (a ^ s)) >> 31) != 0;
}
inline bool sub32_overflows(uint32_t a, uint32_t b) {
    uint32_t d = a - b;
    return (((a ^ b) & (a ^ d)) >> 31) != 0;
}
inline bool add64_overflows(uint64_t a, uint64_t b) {
    uint64_t s = a + b;
    return ((~(a ^ b) & (a ^ s)) >> 63) != 0;
}
inline bool sub64_overflows(uint64_t a, uint64_t b) {
    uint64_t d = a - b;
    return (((a ^ b) & (a ^ d)) >> 63) != 0;
}

// MULT/MULTU/DIV/DIVU operate on the low 32 bits; HI/LO are sign-extended.
// Division by zero leaves HI/LO unchanged and INT_MIN/-1 wraps, matching MAME
// src/devices/cpu/mips/mips3.cpp (architecturally the result is undefined).
inline void mult(Context& c, uint64_t a, uint64_t b) {
    int64_t p = static_cast<int64_t>(static_cast<int32_t>(lo32(a))) * static_cast<int32_t>(lo32(b));
    c.lo = sx32(static_cast<uint32_t>(p));
    c.hi = sx32(static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32));
}
inline void multu(Context& c, uint64_t a, uint64_t b) {
    uint64_t p = static_cast<uint64_t>(lo32(a)) * lo32(b);
    c.lo = sx32(static_cast<uint32_t>(p));
    c.hi = sx32(static_cast<uint32_t>(p >> 32));
}
inline void div32(Context& c, uint64_t a, uint64_t b) {
    int32_t n = static_cast<int32_t>(lo32(a)), d = static_cast<int32_t>(lo32(b));
    if (lo32(a) == 0x80000000u && lo32(b) == 0xffffffffu) { c.lo = sx32(0x80000000u); c.hi = 0; }
    else if (d != 0) { c.lo = sx32(static_cast<uint32_t>(n / d)); c.hi = sx32(static_cast<uint32_t>(n % d)); }
}
inline void divu32(Context& c, uint64_t a, uint64_t b) {
    uint32_t n = lo32(a), d = lo32(b);
    if (d != 0) { c.lo = sx32(n / d); c.hi = sx32(n % d); }
}
void dmult(Context& c, uint64_t a, uint64_t b);
void dmultu(Context& c, uint64_t a, uint64_t b);
inline void ddiv(Context& c, uint64_t a, uint64_t b) {
    if (a == 0x8000000000000000ull && b == ~0ull) { c.lo = a; c.hi = 0; }
    else if (b != 0) { c.lo = static_cast<uint64_t>(s64(a) / s64(b)); c.hi = static_cast<uint64_t>(s64(a) % s64(b)); }
}
inline void ddivu(Context& c, uint64_t a, uint64_t b) {
    if (b != 0) { c.lo = a / b; c.hi = a % b; }
}

// ---- memory ----------------------------------------------------------------

inline uint32_t ea(uint64_t base, int32_t offset) { return lo32(base + static_cast<uint64_t>(static_cast<int64_t>(offset))); }

// Returns a host pointer for a RAM access, or nullptr for I/O / unmapped.
inline uint8_t* ram_ptr(Context& c, uint32_t vaddr, int size, uint32_t pc, bool store) {
    if ((vaddr & static_cast<uint32_t>(size - 1)) != 0)
        raise_fault(c, store ? Fault::AddressErrorStore : Fault::AddressErrorLoad, pc, vaddr);
    uint32_t phys;
    if (vaddr - 0x80000000u < 0x40000000u) {
        phys = vaddr & 0x1fffffffu;  // KSEG0/KSEG1
    } else if (vaddr < kZeroPageSize) {
        // The only TLB mapping the game creates (0x8800b2e0, called once at
        // boot): VA 0x0000-0x0fff -> PA 0x08090000, used for $zero-based
        // globals. Verified in MAME after boot; every other mapped address
        // misses, which the game's exception handler treats as fatal.
        phys = kZeroPagePhys + vaddr;
    } else {
        raise_fault(c, Fault::Tlb, pc, vaddr);
    }
    if (phys - kSramBase < kSramSize) return c.sram.data() + (phys - kSramBase);
    if (phys - kDramBase < kDramSize) return c.dram.data() + (phys - kDramBase);
    return nullptr;
}

uint64_t io_read(Context& c, uint32_t vaddr, int size);
void io_write(Context& c, uint32_t vaddr, int size, uint64_t value);

template <typename T>
inline T load(Context& c, uint32_t vaddr, uint32_t pc) {
    if (uint8_t* p = ram_ptr(c, vaddr, sizeof(T), pc, false)) {
        T v;
        std::memcpy(&v, p, sizeof(T));
        return v;
    }
    return static_cast<T>(io_read(c, vaddr, sizeof(T)));
}

template <typename T>
inline void store(Context& c, uint32_t vaddr, T value, uint32_t pc) {
    if (uint8_t* p = ram_ptr(c, vaddr, sizeof(T), pc, true))
        std::memcpy(p, &value, sizeof(T));
    else
        io_write(c, vaddr, sizeof(T), value);
}

// Unaligned word/doubleword halves, little-endian semantics (MIPS III manual,
// LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR with BigEndianCPU = 0).
uint64_t lwl(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc);
uint64_t lwr(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc);
uint64_t ldl(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc);
uint64_t ldr(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc);
void swl(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc);
void swr(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc);
void sdl(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc);
void sdr(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc);

// ---- coprocessor 1 (single precision) --------------------------------------
// Host float arithmetic is IEEE-754 binary32 with no excess precision on the
// supported targets (FLT_EVAL_METHOD == 0 on arm64 and x86-64 SSE). Do not
// build with -ffast-math. NaN payloads may differ between host ISAs.

inline uint32_t fbits(const Context& c, unsigned n) { return lo32(c.f[n]); }
inline void set_fbits(Context& c, unsigned n, uint32_t v) { c.f[n] = (c.f[n] & 0xffffffff00000000ull) | v; }
inline float fs(const Context& c, unsigned n) {
    uint32_t b = fbits(c, n);
    float v;
    std::memcpy(&v, &b, sizeof v);
    return v;
}
inline void set_fs(Context& c, unsigned n, float v) {
    uint32_t b;
    std::memcpy(&b, &v, sizeof b);
    set_fbits(c, n, b);
}
inline bool fcc(const Context& c) { return (c.fcsr >> 23) & 1; }
inline void set_fcc(Context& c, bool v) { c.fcsr = (c.fcsr & ~(1u << 23)) | (static_cast<uint32_t>(v) << 23); }
// CVT.W.S rounds according to FCSR.RM, like the hardware and MAME's DRC
// (MAME's interpreter truncates instead). NaN and out-of-range inputs, which
// trap on the R4600, saturate here (as arm64 fcvt* does); see the note.
int32_t cvt_w_s(const Context& c, float v);
// C.cond.S: cond is the low 4 bits of the function field (signalling variants
// 8-15 compare like 0-7; FP exceptions are not modelled).
bool fcompare(unsigned cond, float a, float b);
uint64_t cfc1(const Context& c, unsigned reg);
void ctc1(Context& c, unsigned reg, uint64_t value);

// ---- coprocessor 0 / system (not needed by the first proof; trap loudly) ---
// COP0 register numbers used by the game.
enum : unsigned { kCp0Index = 0, kCp0EntryLo0 = 2, kCp0EntryLo1 = 3, kCp0PageMask = 5, kCp0Wired = 6,
                  kCp0BadVAddr = 8, kCp0Count = 9, kCp0EntryHi = 10, kCp0Compare = 11, kCp0Status = 12,
                  kCp0Cause = 13, kCp0Epc = 14, kCp0PRId = 15, kCp0Config = 16 };
// Exception vector handler the game installs at 0x80000180 jumps here (SRAM
// stub: sd sp,0x1f0($zero); sp = 0x880266c8; jr sp).
constexpr uint32_t kExceptionHandler = 0x880266c8;

// Safe point: called by generated code at function entry and backward jumps.
void service(Context& c, uint32_t pc);
inline void poll(Context& c, uint32_t pc) {
    if (c.cycles >= c.next_event) service(c, pc);
}
void schedule(Context& c);  // recompute next_event after state changes
void eret(Context& c);

uint64_t mfc0(Context& c, unsigned reg, uint32_t pc);
void mtc0(Context& c, unsigned reg, uint64_t value, uint32_t pc);
void cop0_op(Context& c, uint32_t instr, uint32_t pc);  // tlbwi, tlbr, eret, ...
void cache_op(Context& c, unsigned op, uint32_t vaddr, uint32_t pc);

}  // namespace ki
