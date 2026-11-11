# DRONE-MOD (NTS-1 mkII modfx unit)

`drone_modfx.nts1mkiiunit` — the modulation-FX stage of the DRONE audio path.
Sits between the DRONE-OSC (generator) and the DRONE-DELAY / DRONE-REVERB
stages on the NTS-1 mkII; in the web simulator it runs inside the unified
`drone-chain` wasm worklet (see `drone-chain/chain.h`).

## The 7 parameters

The MOD unit is a 7-slot unit. Every slot is a 10-bit knob (0..1023) that
maps to a 0..1 DSP value. Defaults match `header.c` and
`Modfx::Params::reset()`.

| Slot | Name  | Range | Default     | Display | What it does |
|------|-------|-------|-------------|---------|--------------|
| 0    | TIME  | 0..1  | 256 (~25%)  | %       | LFO tempo ceiling. Drives the ENSE / TREMO / FLNG LFO frequencies via stage-specific exp curves (0.05..1.2 Hz, 0.03..0.8 Hz, 0.30..5.0 Hz respectively). |
| 1    | DEPTH | 0..1  | 511 (~50%)  | %       | Per-stage modulation depth. Flanger delay (DEPTH² × 3 ms), chorus LFO spread (× 32 smp), tape flutter amount, tremolo depth (× 0.85). |
| 2    | DRIFT | 0..1  | 0 (off)     | %       | Shared self-motion ceiling (like RATE in the OSC). At 0 all four level stages stay at their manual values. > 0, a K-rate random-walk (LCG-driven) nudges each of ENSE/TREMO/FLNG/TAPE within [manual·(1−d), manual·(1+d)], with an L1 budget (1.5) so the wandering "percolates" between layers instead of all four moving at once. |
| 3    | ENSE  | 0..1  | 1023 (100%) | %       | Stereo chorus level. 0 = wire. Two detuned one-pole "delay" voices (L in-phase, R × 1.0618 + longer base + phase-shifted LFO). |
| 4    | TREMO | 0..1  | 307 (~30%)  | %       | Harmonic/optical tremolo level. 0 = wire. One-pole LPF/HPF split at 800 Hz; the two bands are modulated in antiphase by the TREMO LFO; the LOW band is floored at 35% (the "sub-anchor"). |
| 5    | FLNG  | 0..1  | 0 (off)     | %       | Thru-zero flanger level. 0 = off. First-order HPF + a very short one-pole modulated delay (LFO × 3 rate) + feedback (DEPTH × 0.85, clamped < 0.85). |
| 6    | TAPE  | 0..1  | 0 (off)     | %       | Tape wow + flutter level. 0 = off. Wow: LCG random-walk one-pole delay. Flutter: LCG white noise → one-pole LPF → amplitude mod (± 0.30 × DEPTH). |

## Signal path

**Stereo in / stereo out, interleaved** (`in[2n]=L, in[2n+1]=R`, `out[2n]=L,
out[2n+1]=R`). MOD does NOT upmix: the NTS-1 platform feeds modfx units true
stereo (see `drone-modfx/unit.cc`, which rejects non-2-channel geometry), and
in the web simulator `Chain::process` (drone-chain/chain.h) upmixes the mono
OSC to L/R *before* the first FX stage. The OLD code read `in[n]` (a mono
layout) from the interleaved buffer -- i.e. it consumed the L half only and
walked the R half as if it were the next L frames (a half-rate garbled
signal; clean in the web chain only because L==R there). The per-sample path
now reads both channels and every stateful stage (ENSE pre/post LPF, the
chorus delay voices, TREMO/FLNG/TAPE) is per-channel.

    in (stereo L/R)
      -> ENSE  (stereo chorus: 2 detuned voices, modulated one-pole delays,
               per-channel pre/post LPF + allpass)
      -> TREMO (harmonic/optical tremolo: LPF/HPF split, antiphase LFO)
      -> FLNG  (thru-zero flanger: HPF, modulated short delay + feedback)
      -> TAPE  (wow: LCG random-walk delay; flutter: LCG noise level mod)
      -> makeup gain (0.9)
      -> out (stereo)

Note: there is deliberately NO DC-blocker on the output. The ENSE chorus
voices are long one-pole delays whose outputs carry a DC-leaning envelope at
low DRONE frequencies; a ~20 Hz one-pole HPF there (tau ~2 s) let only the
slow level modulation through and ate the entire signal (MOD-on sounded
near-silent). The residual DC offset of the one-pole stages is small and the
0.9 makeup leaves headroom against it.

Every level stage (ENSE / TREMO / FLNG / TAPE) is a dry/wet 0..1 blend: at 0
the stage is a pure wire (regression-safe). The DEL/REV stages downstream see
the MOD output as their input. (The formant + comb resonators are part of the
upstream DRONE-OSC unit — see `drone-osc/README.md`.)

## DRIFT (the shared self-motion ceiling)

Like RATE in the oscillator, DRIFT is a single knob that gates how far the
four "level" stages wander on their own. The walk is a K-rate random-walk:

- Each of the 4 stages (ENSE/TREMO/FLNG/TAPE) has a target
  offset within [−d, +d] (d = DRIFT, 0..1) that is re-rolled on an
  exponential schedule (mean gap 2..12 s) and tracked with a one-pole
  (time constant 5..20 s), so the levels drift slowly and smoothly.
- The L1 budget (kDriftBudget = 1.5) clamps the SUM of the four per-stage
  drift amplitudes, so at most one or two layers wander at a time — the
  motion "percolates" between stages instead of all four moving in unison.
- The walk is driven by a Park-Miller-Carta LCG (pure 32-bit integer math,
  no libm) — the same seed on the device and in the web simulator.
- At DRIFT = 0 the offsets are zero: every level stays at its manual value
  (the regression-safe default).

## libm discipline (check-und)

The NTS-1 mkII FX runtime does not link `libm` into a unit, and the
`check-und` target in the Makefile fails the build if any undefined (UND)
libm symbol appears in the unit's dynamic symbol table. Every per-sample and
K-rate computation in this unit uses only:

- plain float / integer arithmetic, and
- the hand-rolled helpers in `modfx_constants.h`:
  - `modfx_one_minus_expf(x)` — 3rd-order Taylor of 1 − eˣ (for one-pole
    alphas: 1 − exp(−1/τ)).
  - `modfx_expf(x)` — 4th-order Taylor of eˣ (for the LFO frequency curves,
    valid over |x| ≤ ~3.3).
  - the LFO sine is a 4th-order Taylor of sin(phase) evaluated on a phase
    accumulator (no `sinf`).
  - the DRIFT walk and tape noise are driven by the integer LCG (no
    hardware random, no libm).

## Files

- `modfx.h` — the `Modfx : Processor` class (Params, LFO phase accumulators,
  DRIFT walk state, LCG state, first-order filter states, `setParameter`,
  `getParameterStrValue`) AND the full per-sample audio path
  (`Modfx::modfx_process_impl`, defined INLINE in the class so it can be
  compiled into both the ARM unit and the web-simulator chain without a
  separate TU).
- `modfx_constants.h` — all tuning constants (LFO curves, DRIFT budget, LCG
  constants) + the libm-free helpers.
- `header.c` — the 7 parameter slots (TIME/DEPTH/DRIFT/ENSE/TREMO/FLNG/TAPE)
  with their defaults.
- `unit.cc` — the runtime callback trampoline (forwards each header slot 1:1
  to `Modfx::setParameter`; no multiplexer, unlike the OSC's 4-slot
  PARAM/VALUE pair).
- `wasm.cc` — the web-simulator embind interface (now only used by the unified
  chain simulator in `drone-chain/` — see below).

## Web simulator

The unified DRONE web simulator is `drone-chain/` (one wasm worklet running
OSC -> MOD -> DEL -> REV). The MOD stage's 7 parameters are exposed in
`websim/index.html` as a 7-slider column below the Parameters card, driven
through `Module.setFxParam(stage=0, which=0..6, raw)`; every slider shows
0..100%. The MOD stage's `getBufferSize()` is 0 (no SDRAM state);
`Chain::init` slices the chain's single SDRAM allocation across
MOD / DEL / REV in that order (MOD takes none).

## Build

    # ARM unit (runs check-und):
    cd platform/nts-1_mkii/drone-modfx && make install

    # Web simulator (unified chain page):
    cd platform/nts-1_mkii/drone-chain && make wasm