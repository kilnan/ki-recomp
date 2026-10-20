#include "ki/runtime.h"
#include "ki/hooks.h"

#include <algorithm>
#include <cmath>

namespace ki {

namespace hooks {
Fn sprite = nullptr;
Fn sprite_after = nullptr;
Fn fight_setup = nullptr;
Fn panorama = nullptr;
Fn panorama_after = nullptr;
Fn prerendered = nullptr;
Fn prerendered_after = nullptr;
Fn prerendered_setup = nullptr;
Fn prerendered_setup_after = nullptr;
Fn screen_save_after = nullptr;
Fn screen_restore_after = nullptr;
}

void raise_fault(Context&, Fault fault, uint32_t pc, uint32_t addr) {
    throw GuestFault{fault, pc, addr};
}

namespace {
const FuncEntry* boot_table = nullptr;
std::size_t boot_table_size = 0;

RecompFunc find(const FuncEntry* table, std::size_t size, uint32_t vaddr) {
    const FuncEntry* end = table + size;
    const FuncEntry* it =
        std::lower_bound(table, end, vaddr, [](const FuncEntry& e, uint32_t v) { return e.vaddr < v; });
    return (it != end && it->vaddr == vaddr) ? it->fn : nullptr;
}
}  // namespace

void set_boot_function_table(const FuncEntry* table, std::size_t size) {
    boot_table = table;
    boot_table_size = size;
}

RecompFunc lookup_function(uint32_t vaddr) {
    if (RecompFunc fn = find(kFunctionTable, kFunctionTableSize, vaddr)) return fn;
    if (!boot_table) return nullptr;
    if ((vaddr & 0xfff00000u) == 0x9fc00000u) vaddr |= 0x20000000u;  // KSEG0 view of the EPROM
    return find(boot_table, boot_table_size, vaddr);
}

void call_indirect(Context& c, uint32_t target, uint32_t pc) {
    if (RecompFunc fn = lookup_function(target)) {
        fn(c);
        drain(c);
        return;
    }
    raise_fault(c, Fault::Unimplemented, pc, target);
}

void tail_indirect(Context& c, uint32_t target, uint32_t pc) {
    if (RecompFunc fn = lookup_function(target)) {
        c.tail_fn = fn;
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

// Only the 32-bit halves the instruction uses are read, as MAME's bus does
// with a partial mask: on I/O (the EPROM's wait states) the count matters.
uint64_t ldl(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc) {
    unsigned k = vaddr & 7;
    uint64_t dword = k <= 3 ? load<uint32_t>(c, vaddr & ~7u, pc) : load<uint64_t>(c, vaddr & ~7u, pc);
    unsigned shift = 8 * (7 - k);
    uint64_t mask = ~0ull << shift;
    return (rt & ~mask) | (dword << shift);
}

uint64_t ldr(Context& c, uint64_t rt, uint32_t vaddr, uint32_t pc) {
    unsigned k = vaddr & 7;
    uint64_t dword = k >= 4 ? static_cast<uint64_t>(load<uint32_t>(c, (vaddr & ~7u) + 4, pc)) << 32
                            : load<uint64_t>(c, vaddr & ~7u, pc);
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

// ---- COP0, timer and interrupts --------------------------------------------
// Count runs at half the CPU clock. The timer interrupt (Cause.IP7) is raised
// when Count reaches Compare and cleared by writing Compare. Board lines
// (Int0 = VBLANK, Int1 = IDE on this board) come from the IoHandler.

static uint32_t count_now(const Context& c) { return static_cast<uint32_t>(c.count_base + c.cycles / 2); }

static uint32_t cause_now(Context& c) {
    uint32_t cause = lo32(c.cp0[kCp0Cause]) & ~0x7c00u;
    if (c.io) cause |= c.io->interrupt_lines(c) & 0x7c00u;
    return cause;
}

void schedule(Context& c) {
    // Cycles until Count == Compare (mod 2^32), at Count's half rate.
    uint32_t delta = lo32(c.cp0[kCp0Compare]) - count_now(c);
    uint64_t timer = c.cycles + (delta == 0 ? (1ull << 33) : 2ull * delta);
    uint64_t board = c.io ? c.io->next_event(c) : ~0ull;
    c.next_event = timer < board ? timer : board;
    if (c.cp0[kCp0Cause] & 0x8000) {
        // Timer already pending: if it can be taken, come back immediately.
        uint32_t sr = lo32(c.cp0[kCp0Status]);
        if ((sr & 1) && !(sr & 6) && (sr & 0x8000)) c.next_event = c.cycles;
    }
}

static void take_interrupt(Context& c, uint32_t pc) {
    ++c.interrupts_taken;
    uint32_t sr = lo32(c.cp0[kCp0Status]);
    c.cp0[kCp0Epc] = sx32(pc);  // approximate: the safe point, not the exact instruction
    c.cp0[kCp0Cause] = (c.cp0[kCp0Cause] & ~0x7cull);  // ExcCode 0 = interrupt
    c.cp0[kCp0Status] = sx32(sr | 2);                 // EXL
    // The SRAM vector stub: save sp in the zero page, enter the handler.
    store<uint64_t>(c, 0x1f0u, c.r[29], pc);
    c.r[29] = sx32(kExceptionHandler);
    RecompFunc saved_tail = c.tail_fn;  // normally null at a safe point
    c.tail_fn = nullptr;
    if (RecompFunc fn = lookup_function(kExceptionHandler)) {
        fn(c);
        drain(c);
    } else {
        raise_fault(c, Fault::Unimplemented, pc, kExceptionHandler);
    }
    c.tail_fn = saved_tail;
}

void service(Context& c, uint32_t pc) {
    c.recent_service_pc[c.recent_service_pos++ & 63] = pc;
    if (c.io && c.cycles >= c.io->next_event(c)) c.io->update(c);
    // Timer: did Count become equal to Compare since the last check?
    uint32_t now = count_now(c), elapsed = now - c.last_count;
    if (elapsed && lo32(c.cp0[kCp0Compare]) - c.last_count - 1 < elapsed) c.cp0[kCp0Cause] |= 0x8000;  // IP7
    c.last_count = now;
    uint32_t sr = lo32(c.cp0[kCp0Status]);
    uint32_t pending = cause_now(c) & sr & 0xff00u;
    if ((sr & 1) && !(sr & 6) && pending) take_interrupt(c, pc);
    schedule(c);
}

void eret(Context& c) {
    c.cp0[kCp0Status] &= ~2ull;  // clear EXL; control returns to the interrupted code
    schedule(c);
}

uint64_t mfc0(Context& c, unsigned reg, uint32_t) {
    switch (reg) {
        case kCp0Count: return count_now(c);
        case kCp0Cause: return cause_now(c);
        case kCp0PRId: return 0x2020;  // R4600 in MAME
        default: return c.cp0[reg & 31];
    }
}

void mtc0(Context& c, unsigned reg, uint64_t value, uint32_t) {
    switch (reg) {
        case kCp0Count:
            c.count_base = static_cast<uint32_t>(lo32(value) - static_cast<uint32_t>(c.cycles / 2));
            c.last_count = count_now(c);
            break;
        case kCp0Compare:
            c.cp0[kCp0Compare] = lo32(value);
            c.cp0[kCp0Cause] &= ~0x8000ull;
            c.last_count = count_now(c);
            break;
        case kCp0Cause: c.cp0[kCp0Cause] = (c.cp0[kCp0Cause] & ~0x300ull) | (value & 0x300); break;  // IP0-1 only
        default: c.cp0[reg & 31] = value; break;
    }
    schedule(c);
}

void cop0_op(Context& c, uint32_t instr, uint32_t pc) {
    switch (instr & 0x3f) {
        case 0x02: {  // TLBWI: only the zero-page mapping is modelled; accept invalid entries
            uint64_t lo0 = c.cp0[kCp0EntryLo0], lo1 = c.cp0[kCp0EntryLo1], hi = c.cp0[kCp0EntryHi];
            bool invalid = !(lo0 & 2) && !(lo1 & 2);
            bool zero_page = (hi & ~0x1fffull) == 0 && ((lo0 >> 6) << 12) == kZeroPagePhys && (lo0 & 2) && !(lo1 & 2) &&
                             (c.cp0[kCp0PageMask] == 0);
            if (!invalid && !zero_page) raise_fault(c, Fault::Tlb, pc, lo32(hi));
            break;
        }
        case 0x01: case 0x06: case 0x08: break;  // TLBR, TLBWR, TLBP: not used by the game in normal play
        default: raise_fault(c, Fault::Unimplemented, pc, instr);
    }
}
void cache_op(Context&, unsigned, uint32_t, uint32_t) {}  // caches are not modelled; RAM is coherent

}  // namespace ki
