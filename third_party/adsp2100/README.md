# ADSP-2105 core (from MAME)

An interpreter for the Analog Devices ADSP-2105, the DSP of the Midway DCS
sound board in Killer Instinct.

**Origin:** MAME `src/devices/cpu/adsp2100/` (`adsp2100.h`, `adsp2100.cpp`,
`2100ops.hxx`) at revision `ee0b4a91e670a47b55c0d17d2fd8c7522ab6f03c`.
License BSD-3-Clause, copyright holder Aaron Giles (see [LICENSE](LICENSE)).
This directory keeps that license; the rest of ki-recomp is MIT.

**Changes:**
- `2100ops.hxx` is copied with the class renamed to `adsp::Adsp2105`. MAME's
  `util::sext`, `logerror` and devcb callbacks are replaced by local helpers
  and the members of the `adsp::Bus` interface.
- `adsp2105.cpp` assembles MAME's constructor tables, reset, boot loader,
  ADSP-2101-family interrupt logic (`generate_irq`, `check_irqs`) and the
  execution loop. Removed: the device framework, address spaces, debugger
  hooks, save states, the HALT input and the ADSP-2100/2181 variants.
- Added: `total_cycles()` that is exact during `run()`, and `abort_slice()`
  (the equivalent of MAME's abort_timeslice) so the board can stop the DSP right
  after it schedules an event.
- As in MAME, `IDLE` and a jump to itself only end the current slice.
