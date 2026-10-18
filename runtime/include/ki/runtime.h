// Guest runtime for statically recompiled Killer Instinct (R4600, little-endian) code.
//
// Model (see docs/TECHNICAL_NOTE.md for the evidence behind each choice):
// - 32 GPRs + HI/LO are 64-bit; 32-bit operations sign-extend their result.
// - Effective addresses are 32-bit (the game runs in 32-bit kernel mode).
// - KSEG0/KSEG1 map to physical by masking with 0x1fffffff. KUSEG/KSEG2 would
//   need the TLB, which the game programs at boot; not modelled yet.
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
#include <cstring>
#include <vector>

static_assert(std::endian::native == std::endian::little,
              "the runtime stores guest (little-endian) memory in host order");

namespace ki {

constexpr uint32_t kSramBase = 0x00000000, kSramSize = 0x00080000;
constexpr uint32_t kDramBase = 0x08000000, kDramSize = 0x00800000;

struct Context;

// Called for physical addresses outside SRAM/DRAM. size is 1, 2, 4 or 8.
struct IoHandler {
    virtual ~IoHandler() = default;
    virtual uint64_t read(Context& ctx, uint32_t phys, int size) = 0;
    virtual void write(Context& ctx, uint32_t phys, int size, uint64_t value) = 0;
};

enum class Fault { AddressErrorLoad, AddressErrorStore, Overflow, Tlb, Unimplemented, Syscall, Break, Trap };

struct Context {
    uint64_t r[32] = {};
    uint64_t hi = 0, lo = 0;
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
    if (vaddr - 0x80000000u >= 0x40000000u)
        raise_fault(c, Fault::Tlb, pc, vaddr);  // KUSEG/KSEG2: TLB not modelled yet
    uint32_t phys = vaddr & 0x1fffffffu;
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

// ---- coprocessor 0 / system (not needed by the first proof; trap loudly) ---
uint64_t mfc0(Context& c, unsigned reg, uint32_t pc);
void mtc0(Context& c, unsigned reg, uint64_t value, uint32_t pc);
void cop0_op(Context& c, uint32_t instr, uint32_t pc);  // tlbwi, tlbr, eret, ...
void cache_op(Context& c, unsigned op, uint32_t vaddr, uint32_t pc);

}  // namespace ki
