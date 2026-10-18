#include "ki/runtime.h"

#include <algorithm>
#include <cmath>

namespace ki {

void raise_fault(Context&, Fault fault, uint32_t pc, uint32_t addr) {
    throw GuestFault{fault, pc, addr};
}

RecompFunc lookup_function(uint32_t vaddr) {
    const FuncEntry* end = kFunctionTable + kFunctionTableSize;
    const FuncEntry* it = std::lower_bound(kFunctionTable, end, vaddr,
                                           [](const FuncEntry& e, uint32_t v) { return e.vaddr < v; });
    return (it != end && it->vaddr == vaddr) ? it->fn : nullptr;
}

void call_indirect(Context& c, uint32_t target, uint32_t pc) {
    if (RecompFunc fn = lookup_function(target)) {
        fn(c);
        return;
    }
    raise_fault(c, Fault::Unimplemented, pc, target);
}

// 64x64 -> 128 multiply split in 32-bit halves (portable, no __int128).
static void mul64(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) {
    uint64_t a0 = lo32(a), a1 = a >> 32, b0 = lo32(b), b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + lo32(p01) + lo32(p10);
    lo = (mid << 32) | lo32(p00);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

void dmultu(Context& c, uint64_t a, uint64_t b) { mul64(a, b, c.hi, c.lo); }

void dmult(Context& c, uint64_t a, uint64_t b) {
    uint64_t hi, lo;
    mul64(a, b, hi, lo);
    // Signed correction: (a*b)_signed_hi = unsigned_hi - (a<0 ? b : 0) - (b<0 ? a : 0)
    if (s64(a) < 0) hi -= b;
    if (s64(b) < 0) hi -= a;
    c.hi = hi;
    c.lo = lo;
}

uint64_t io_read(Context& c, uint32_t vaddr, int size) {
    uint32_t phys = vaddr & 0x1fffffffu;  // only reached for KSEG0/KSEG1 addresses
    if (c.io) return c.io->read(c, phys, size);
    return ~0ull;  // unmap_value_high
}

void io_write(Context& c, uint32_t vaddr, int size, uint64_t value) {
    uint32_t phys = vaddr & 0x1fffffffu;
    if (c.io) c.io->write(c, phys, size, value);
}

// Little-endian unaligned accesses. For LE, LWL at byte offset k loads the
// bytes [addr-k .. addr] into the top k+1 bytes of the register; LWR loads
// [addr .. addr+(3-k)] into the low 4-k bytes.
uint64_t lwl(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc) {
    unsigned k = vaddr & 3;
    uint32_t word = load<uint32_t>(c, vaddr & ~3u, pc);
    unsigned shift = 8 * (3 - k);
    uint32_t mask = 0xffffffffu << shift;
    return sx32((lo32(rt) & ~mask) | (word << shift));
}

uint64_t lwr(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc) {
    unsigned k = vaddr & 3;
    uint32_t word = load<uint32_t>(c, vaddr & ~3u, pc);
    unsigned shift = 8 * k;
    uint32_t mask = 0xffffffffu >> shift;
    // The upper 32 bits after a partial LWR are implementation-specific; this
    // sign-extends like MAME's mips3_device::lwr_le, which is our oracle.
    return sx32((lo32(rt) & ~mask) | (word >> shift));
}

uint64_t ldl(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc) {
    unsigned k = vaddr & 7;
    uint64_t dword = load<uint64_t>(c, vaddr & ~7u, pc);
    unsigned shift = 8 * (7 - k);
    uint64_t mask = ~0ull << shift;
    return (rt & ~mask) | (dword << shift);
}

uint64_t ldr(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc) {
    unsigned k = vaddr & 7;
    uint64_t dword = load<uint64_t>(c, vaddr & ~7u, pc);
    unsigned shift = 8 * k;
    uint64_t mask = ~0ull >> shift;
    return (rt & ~mask) | (dword >> shift);
}

void swl(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc) {
    unsigned k = vaddr & 3;
    uint32_t aligned = vaddr & ~3u;
    uint32_t word = load<uint32_t>(c, aligned, pc);
    unsigned shift = 8 * (3 - k);
    uint32_t mask = 0xffffffffu >> shift;
    store<uint32_t>(c, aligned, (word & ~mask) | (lo32(rt) >> shift), pc);
}

void swr(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc) {
    unsigned k = vaddr & 3;
    uint32_t aligned = vaddr & ~3u;
    uint32_t word = load<uint32_t>(c, aligned, pc);
    unsigned shift = 8 * k;
    uint32_t mask = 0xffffffffu << shift;
    store<uint32_t>(c, aligned, (word & ~mask) | (lo32(rt) << shift), pc);
}

void sdl(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc) {
    unsigned k = vaddr & 7;
    uint32_t aligned = vaddr & ~7u;
    uint64_t dword = load<uint64_t>(c, aligned, pc);
    unsigned shift = 8 * (7 - k);
    uint64_t mask = ~0ull >> shift;
    store<uint64_t>(c, aligned, (dword & ~mask) | (rt >> shift), pc);
}

void sdr(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc) {
    unsigned k = vaddr & 7;
    uint32_t aligned = vaddr & ~7u;
    uint64_t dword = load<uint64_t>(c, aligned, pc);
    unsigned shift = 8 * k;
    uint64_t mask = ~0ull << shift;
    store<uint64_t>(c, aligned, (dword & ~mask) | (rt << shift), pc);
}

int32_t cvt_w_s(const Context& c, float v) {
    if (std::isnan(v)) return 0;
    double d = v, r;
    switch (c.fcsr & 3) {
        case 0: {  // round to nearest, ties to even
            r = std::floor(d);
            double diff = d - r;
            if (diff > 0.5 || (diff == 0.5 && std::fmod(r, 2.0) != 0.0)) r += 1.0;
            break;
        }
        case 1: r = std::trunc(d); break;
        case 2: r = std::ceil(d); break;
        default: r = std::floor(d); break;
    }
    if (r >= 2147483648.0) return INT32_MAX;
    if (r < -2147483648.0) return INT32_MIN;
    return static_cast<int32_t>(r);
}

bool fcompare(unsigned cond, float a, float b) {
    bool unordered = std::isnan(a) || std::isnan(b);
    bool less = !unordered && a < b;
    bool equal = !unordered && a == b;
    return ((cond & 1) && unordered) || ((cond & 2) && equal) || ((cond & 4) && less);
}

uint64_t cfc1(const Context& c, unsigned reg) {
    if (reg == 31) return sx32(c.fcsr);
    if (reg == 0) return 0x2020;  // FCR0 = PRId for the R4600 in MAME (mips3com.cpp compute_prid_register)
    return 0;
}

void ctc1(Context& c, unsigned reg, uint64_t value) {
    if (reg == 31) c.fcsr = lo32(value);
}

uint64_t mfc0(Context& c, unsigned reg, uint32_t pc) { raise_fault(c, Fault::Unimplemented, pc, reg); }
void mtc0(Context& c, unsigned reg, uint64_t, uint32_t pc) { raise_fault(c, Fault::Unimplemented, pc, reg); }
void cop0_op(Context& c, uint32_t instr, uint32_t pc) { raise_fault(c, Fault::Unimplemented, pc, instr); }
void cache_op(Context&, unsigned, uint32_t, uint32_t) {}  // caches are not modelled; RAM is coherent

}  // namespace ki
