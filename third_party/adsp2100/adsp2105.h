// license:BSD-3-Clause
// copyright-holders:Aaron Giles
//
// ADSP-2105 interpreter for the Killer Instinct DCS sound board.
//
// Derived from MAME src/devices/cpu/adsp2100 (adsp2100.h/.cpp, 2100ops.hxx) at
// revision ee0b4a91e670a47b55c0d17d2fd8c7522ab6f03c. The MAME device framework
// (address spaces, devcb callbacks, debugger, save states) is replaced by the
// small Bus interface below; the instruction semantics are MAME's, unchanged.
// Only the ADSP-2101 family behaviour (ADSP-2105 configuration) is kept.
#pragma once

#include <cstdint>

#define LSB_FIRST 1

namespace adsp {

// Interrupt inputs (ADSP-2105 numbering, as MAME's ADSP2105_*).
constexpr int kIrq0 = 0;   // also SPORT1 receive
constexpr int kIrq1 = 1;   // also SPORT1 transmit
constexpr int kIrq2 = 2;
constexpr int kTimer = 5;
constexpr int kInputReset = 10;  // not an interrupt: see Adsp2105::set_reset

// What the DSP sees outside its core: memory, serial port transmit and the
// timer enable bit. Implemented by the sound board model.
class Bus {
public:
    virtual ~Bus() = default;
    virtual uint16_t data_read(uint32_t addr) = 0;
    virtual void data_write(uint32_t addr, uint16_t data) = 0;
    virtual uint32_t program_read(uint32_t addr) = 0;
    virtual void program_write(uint32_t addr, uint32_t data) = 0;
    virtual void sport_tx(int port, uint32_t data) = 0;
    virtual uint32_t sport_rx(int port) { (void)port; return 0; }
    virtual void timer_fired(bool enabled) = 0;
};

class Adsp2105 {
public:
    explicit Adsp2105(Bus& bus);

    void reset();
    // Runs at least `cycles` instructions (one cycle each), returns the number run.
    int run(int cycles);
    // Interrupt input level (MAME execute_set_input semantics: a rising edge latches).
    void set_input(int line, bool asserted);
    // Holds the core in reset while asserted; releasing it resets the core.
    void set_reset(bool asserted);
    bool in_reset() const { return m_in_reset; }
    // Ends the current run() after the instruction being executed (MAME abort_timeslice),
    // so that an event the bus has just scheduled is not served late.
    void abort_slice() {
        if (m_running && m_icount > 0) {
            m_slice -= m_icount;  // the instruction being executed is the last one counted
            m_icount = 0;
        }
    }
    bool idle() const { return m_idle != 0; }

    // Boot loader: copies the page described by an 8-bit boot ROM image.
    static void load_boot_data(const uint8_t* srcdata, uint32_t* dstdata);

    // Index/modify/length registers, as MAME state_int(ADSP2100_I0 + n) etc.
    uint32_t ireg(int n) const { return m_i[n]; }
    int32_t mreg(int n) const { return m_m[n]; }
    uint32_t lreg(int n) const { return m_l[n]; }
    void set_ireg(int n, uint32_t v);
    uint32_t get_ibase(int n) const { return m_base[n]; }
    uint32_t pc() const { return m_pc; }
    // Cycles run so far, also correct from a Bus callback in the middle of run().
    uint64_t total_cycles() const { return m_running ? m_total_cycles + (m_slice - m_icount) : m_total_cycles; }

private:
    enum { CHIP_TYPE_ADSP2100, CHIP_TYPE_ADSP2101, CHIP_TYPE_ADSP2104, CHIP_TYPE_ADSP2105,
           CHIP_TYPE_ADSP2115, CHIP_TYPE_ADSP2181 };
    // Interrupt numbers referenced by the shared MAME code paths.
    enum { ADSP2101_IRQ0 = 0, ADSP2101_IRQ1 = 1, ADSP2101_IRQ2 = 2, ADSP2101_SPORT0_RX = 3,
           ADSP2101_SPORT0_TX = 4, ADSP2101_TIMER = 5 };
    enum { ADSP2181_IRQ0 = 0, ADSP2181_IRQ1 = 1, ADSP2181_IRQ2 = 2, ADSP2181_SPORT0_RX = 3,
           ADSP2181_SPORT0_TX = 4, ADSP2181_TIMER = 5, ADSP2181_IRQE = 6, ADSP2181_BDMA = 7 };

    Bus& m_bus;
    bool m_in_reset = false;
    bool m_running = false;
    int m_slice = 0;
    uint64_t m_total_cycles = 0;

    // MAME glue
    uint32_t sport_rx(int port) { return m_bus.sport_rx(port); }
    void sport_tx(int port, uint32_t data, uint32_t mask) { (void)mask; m_bus.sport_tx(port, data); }
    void timer_fired(bool state) { m_bus.timer_fired(state); }

    void create_tables();
    inline void update_mstat();
    inline uint32_t pc_stack_top();
    inline void set_pc_stack_top(uint32_t top);
    inline void pc_stack_push();
    inline void pc_stack_push_val(uint32_t val);
    inline void pc_stack_pop();
    inline uint32_t pc_stack_pop_val();
    inline uint32_t cntr_stack_top();
    inline void cntr_stack_push();
    inline void cntr_stack_pop();
    inline uint32_t loop_stack_top();
    inline void loop_stack_push(uint32_t value);
    inline void loop_stack_pop();
    inline void stat_stack_push();
    inline void stat_stack_pop();
    int slow_condition();
    inline void modify_address(uint32_t ireg, uint32_t mreg);
    inline void data_write_dag1(uint32_t op, int32_t val);
    inline uint32_t data_read_dag1(uint32_t op);
    inline void data_write_dag2(uint32_t op, int32_t val);
    inline uint32_t data_read_dag2(uint32_t op);
    inline void pgm_write_dag2(uint32_t op, int32_t val);
    inline uint32_t pgm_read_dag2(uint32_t op);
    void alu_op_ar(uint32_t op);
    void alu_op_ar_const(uint32_t op);
    void alu_op_af(uint32_t op);
    void alu_op_af_const(uint32_t op);
    void alu_op_none(uint32_t op);
    void mac_op_mr(uint32_t op);
    void mac_op_mr_xop(uint32_t op);
    void mac_op_mf(uint32_t op);
    void mac_op_mf_xop(uint32_t op);
    void shift_op(uint32_t op);
    void shift_op_imm(uint32_t op);

    uint16_t data_read(uint32_t addr) { return m_bus.data_read(addr); }
    void data_write(uint32_t addr, uint16_t data) { m_bus.data_write(addr, data); }
    uint16_t io_read(uint32_t) { return 0xffff; }
    void io_write(uint32_t, uint16_t) {}
    uint32_t program_read(uint32_t addr) { return m_bus.program_read(addr); }
    void program_write(uint32_t addr, uint32_t data) { m_bus.program_write(addr, data & 0xffffff); }
    uint32_t opcode_read() { return m_bus.program_read(m_pc); }

    inline void update_i(int which);
    inline void update_l(int which);
    inline void update_dmovlay();
    void write_reg0(int regnum, int32_t val);
    void write_reg1(int regnum, int32_t val);
    void write_reg2(int regnum, int32_t val);
    void write_reg3(int regnum, int32_t val);
    int32_t read_reg0(int regnum);
    int32_t read_reg1(int regnum);
    int32_t read_reg2(int regnum);
    int32_t read_reg3(int regnum);

    bool generate_irq(int which, int indx);
    void check_irqs();

    static const int PC_STACK_DEPTH = 16;
    static const int CNTR_STACK_DEPTH = 4;
    static const int STAT_STACK_DEPTH = 4;
    static const int LOOP_STACK_DEPTH = 4;

    union adsp_reg16 {
        uint16_t u;
        int16_t s;
    };
    union adsp_shift {
        struct { adsp_reg16 sr0, sr1; } srx;
        uint32_t sr;
    };
    union adsp_mac {
        struct { adsp_reg16 mr0, mr1, mr2, mrzero; } mrx;
        struct { uint32_t mr0, mr1; } mry;
        uint64_t mr;
    };
    struct adsp_core {
        adsp_reg16 ax0, ax1;
        adsp_reg16 ay0, ay1;
        adsp_reg16 ar;
        adsp_reg16 af;
        adsp_reg16 mx0, mx1;
        adsp_reg16 my0, my1;
        adsp_mac mr;
        adsp_reg16 mf;
        adsp_reg16 si;
        adsp_reg16 se;
        adsp_reg16 sb;
        adsp_shift sr;
        adsp_reg16 zero;
    };

    uint32_t m_chip_type = CHIP_TYPE_ADSP2105;
    uint32_t m_pc = 0;
    uint32_t m_ppc = 0;
    uint32_t m_loop = 0;
    uint32_t m_loop_condition = 0;
    uint32_t m_cntr = 0;
    uint32_t m_astat = 0;
    uint32_t m_sstat = 0;
    uint32_t m_mstat = 0;
    uint32_t m_mstat_prev = 0;
    uint32_t m_astat_clear = 0;
    uint32_t m_idle = 0;
    adsp_core m_core{};
    uint32_t m_i[8]{};
    int32_t m_m[8]{};
    uint32_t m_l[8]{};
    uint32_t m_lmask[8]{};
    uint32_t m_base[8]{};
    uint8_t m_px = 0;
    uint32_t m_pmovlay = 0;
    uint32_t m_dmovlay = 0;
    uint32_t m_loop_stack[LOOP_STACK_DEPTH]{};
    uint32_t m_cntr_stack[CNTR_STACK_DEPTH]{};
    uint32_t m_pc_stack[PC_STACK_DEPTH]{};
    uint16_t m_stat_stack[STAT_STACK_DEPTH][3]{};
    int32_t m_pc_sp = 0;
    int32_t m_cntr_sp = 0;
    int32_t m_stat_sp = 0;
    int32_t m_loop_sp = 0;
    uint8_t m_flagout = 0;
    uint8_t m_flagin = 0;
    uint8_t m_fl0 = 0;
    uint8_t m_fl1 = 0;
    uint8_t m_fl2 = 0;
    uint16_t m_imask = 0;
    uint8_t m_icntl = 0;
    uint16_t m_ifc = 0;
    uint8_t m_irq_state[10]{};
    uint8_t m_irq_latch[10]{};
    int m_icount = 0;
    int m_mstat_mask = 0x7f;
    int m_imask_mask = 0x3f;
    int16_t* m_read0_ptr[16]{};
    uint32_t* m_read1_ptr[16]{};
    uint32_t* m_read2_ptr[16]{};
    void* m_alu_xregs[8]{};
    void* m_alu_yregs[4]{};
    void* m_mac_xregs[8]{};
    void* m_mac_yregs[4]{};
    void* m_shift_xregs[8]{};
    adsp_core m_alt{};
    uint8_t m_condition_table[0x1000]{};
    uint16_t m_mask_table[0x4000]{};
    uint16_t m_reverse_table[0x4000]{};

    static const int SSFLAG = 0x80;
    static const int MVFLAG = 0x40;
    static const int QFLAG = 0x20;
    static const int SFLAG = 0x10;
    static const int CFLAG = 0x08;
    static const int VFLAG = 0x04;
    static const int NFLAG = 0x02;
    static const int ZFLAG = 0x01;
};

}  // namespace adsp
