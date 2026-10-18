// Synthetic checks of runtime helpers. These do not involve game code and do
// not validate anything about Killer Instinct; they guard helper semantics.
#include <cstdio>
#include <limits>

#include "ki/runtime.h"

static int failures = 0;
#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        unsigned long long _a = (a), _b = (b);                                                      \
        if (_a != _b) {                                                                             \
            std::printf("%s:%d: %s = %016llx, expected %016llx\n", __FILE__, __LINE__, #a, _a, _b); \
            ++failures;                                                                             \
        }                                                                                           \
    } while (0)

int main() {
    using namespace ki;
    CHECK_EQ(sx32(0x80000000u), 0xffffffff80000000ull);
    CHECK_EQ(sra32(0x0000000080000000ull, 4), 0xfffffffff8000000ull);
    CHECK_EQ(dsra(0x8000000000000000ull, 63), ~0ull);
    CHECK_EQ(add32_overflows(0x7fffffffu, 1), 1);
    CHECK_EQ(add32_overflows(0x88500000u, 1), 0);
    CHECK_EQ(sub32_overflows(0x80000000u, 1), 1);

    Context c;
    mult(c, sx32(0x80000000u), sx32(0xffffffffu));  // INT_MIN * -1
    CHECK_EQ(c.lo, 0xffffffff80000000ull);
    CHECK_EQ(c.hi, 0);
    div32(c, sx32(0x80000000u), sx32(0xffffffffu));
    CHECK_EQ(c.lo, 0xffffffff80000000ull);
    CHECK_EQ(c.hi, 0);
    c.lo = 0x1234;
    c.hi = 0x5678;
    divu32(c, 5, 0);  // unchanged on divide by zero (MAME behaviour)
    CHECK_EQ(c.lo, 0x1234);
    CHECK_EQ(c.hi, 0x5678);
    dmultu(c, ~0ull, ~0ull);
    CHECK_EQ(c.hi, 0xfffffffffffffffeull);
    CHECK_EQ(c.lo, 1);
    dmult(c, ~0ull, 2);  // -1 * 2
    CHECK_EQ(c.hi, ~0ull);
    CHECK_EQ(c.lo, 0xfffffffffffffffeull);
    dmult(c, 0x8000000000000000ull, 0x8000000000000000ull);  // (-2^63)^2 = 2^126
    CHECK_EQ(c.hi, 0x4000000000000000ull);
    CHECK_EQ(c.lo, 0);

    // Little-endian guest memory: bytes 11 22 33 44 55 66 77 88 at 0x80001000.
    for (int i = 0; i < 8; ++i) c.sram[0x1000 + i] = static_cast<uint8_t>(0x11 * (i + 1));
    CHECK_EQ(load<uint32_t>(c, 0x80001000u, 0), 0x44332211u);
    CHECK_EQ(load<uint32_t>(c, 0xa0001004u, 0), 0x88776655u);  // KSEG1 alias
    // Unaligned word at 0x80001001 = 0x55443322 via LWR(addr) + LWL(addr+3).
    uint64_t r = lwr(c, 0, 0x80001001u, 0);
    r = lwl(c, r, 0x80001004u, 0);
    CHECK_EQ(r, 0x55443322u);
    swr(c, 0xaabbccddu, 0x80001001u, 0);
    swl(c, 0xaabbccddu, 0x80001004u, 0);
    CHECK_EQ(load<uint32_t>(c, 0x80001000u, 0), 0xbbccdd11u);
    CHECK_EQ(load<uint32_t>(c, 0x80001004u, 0), 0x887766aau);
    bool faulted = false;
    try { (void)load<uint32_t>(c, 0x80001002u, 0); } catch (const GuestFault& f) { faulted = f.fault == Fault::AddressErrorLoad; }
    CHECK_EQ(faulted, 1);
    CHECK_EQ(load<uint32_t>(c, 0xb0000080u, 0), 0xffffffffu);  // no I/O handler: unmapped reads all ones

    // COP1: CVT.W.S follows FCSR.RM; ties go to even in round-to-nearest.
    c.fcsr = 0x01000000;  // value the game writes at boot: FS=1, RM=0
    CHECK_EQ(static_cast<uint32_t>(cvt_w_s(c, 2.5f)), 2u);
    CHECK_EQ(static_cast<uint32_t>(cvt_w_s(c, 3.5f)), 4u);
    CHECK_EQ(static_cast<uint32_t>(cvt_w_s(c, -2.5f)), static_cast<uint32_t>(-2));
    CHECK_EQ(static_cast<uint32_t>(cvt_w_s(c, -2.6f)), static_cast<uint32_t>(-3));
    c.fcsr = 1;  // RZ
    CHECK_EQ(static_cast<uint32_t>(cvt_w_s(c, -2.6f)), static_cast<uint32_t>(-2));
    c.fcsr = 2;  // RP
    CHECK_EQ(static_cast<uint32_t>(cvt_w_s(c, 2.1f)), 3u);
    c.fcsr = 3;  // RM
    CHECK_EQ(static_cast<uint32_t>(cvt_w_s(c, -2.1f)), static_cast<uint32_t>(-3));
    c.fcsr = 0;
    CHECK_EQ(static_cast<uint32_t>(cvt_w_s(c, 3e9f)), 0x7fffffffu);
    float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_EQ(fcompare(4, 1.0f, 2.0f), 1);    // c.olt
    CHECK_EQ(fcompare(4, 1.0f, nan), 0);
    CHECK_EQ(fcompare(5, 1.0f, nan), 1);     // c.ult: unordered or less
    CHECK_EQ(fcompare(2, -0.0f, 0.0f), 1);   // c.eq: -0 == +0
    CHECK_EQ(fcompare(12, 1.0f, 2.0f), 1);   // c.lt (signalling) compares like c.olt
    // FPR writes keep the high half (DRC behaviour), reads use the low half.
    c.f[3] = 0xdeadbeef00000000ull;
    set_fs(c, 3, 1.0f);
    CHECK_EQ(c.f[3], 0xdeadbeef3f800000ull);
    set_fcc(c, true);
    CHECK_EQ(c.fcsr >> 23 & 1, 1);

    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
