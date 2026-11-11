# DRONE-MOD (NTS-1 mkII modfx unit)

`drone_modfx.nts1mkiiunit` — the modulation-FX stage of the DRONE audio path.
Sits between the DRONE-OSC (generator) and the DRONE-DELAY / DRONE-REVERB
stages on the NTS-1 mkII; in the web simulator it runs inside the unified
`drone-chain` wasm worklet (see `drone-chain/chain.h`).

## The 10 parameters

The MOD unit is a 10-slot unit (the NTS-1 modfx module's maximum). Every slot
is a 10-bit knob (0..1023) that maps to a 0..1 DSP value except where noted.
Defaults match `header.c` and `Modfx::Params::reset()`.

| Slot | Name  | Range | Default     | Display | What it does |
|------|-------|-------|-------------|---------|--------------|
| 0    | TIME  | 0..1  | 256 (~25%)  | %       | LFO tempo ceiling. Drives the ENSE / TREMO / FLNG LFO frequencies via stage-specific exp curves (0.05..1.2 Hz, 0.03..0.8 Hz, 0.30..5.0 Hz respectively). |
| 1    | DEPTH | 0..1  | 511 (~50%)  | %       | Per-stage modulation depth. Flanger delay (DEPTH² × 3 ms), chorus LFO spread (× 32 smp), tape flutter amount, tremolo depth (× 0.85). |
| 2    | DRIFT | 0..1  | 0 (off)     | %       | Shared self-motion ceiling (like RATE in the OSC). At 0 all seven level stages stay at their manual values. > 0, a K-rate random-walk (LCG-driven) nudges each of ENSE/TREMO/FLNG/TAPE/FLVL/COMB within [manual·(1−d), manual·(1+d)], with an L1 budget (1.5) so the wandering "percolates" between layers instead of all seven moving at once. |
| 3    | ENSE  | 0..1  | 1023 (100%) | %       | Stereo chorus level. 0 = wire. Two detuned one-pole "delay" voices (L in-phase, R × 1.0618 + longer base + phase-shifted LFO). |
| 4    | TREMO | 0..1  | 307 (~30%)  | %       | Harmonic/optical tremolo level. 0 = wire. One-pole LPF/HPF split at 800 Hz; the two bands are modulated in antiphase by the TREMO LFO; the LOW band is floored at 35% (the "sub-anchor"). |
| 5    | FLNG  | 0..1  | 0 (off)     | %       | Thru-zero flanger level. 0 = off. First-order HPF + a very short one-pole modulated delay (LFO × 3 rate) + feedback (DEPTH × 0.85, clamped < 0.85). |
| 6    | TAPE  | 0..1  | 0 (off)     | %       | Tape wow + flutter level. 0 = off. Wow: LCG random-walk one-pole delay. Flutter: LCG white noise → one-pole LPF → amplitude mod (± 0.30 × DEPTH). |
| 7    | FORM  | 0..1  | 511 (~50%)  | vowel   | Formant vowel position (morph A-E-I-O-U across 5 stops). Drives the F1/F2 comb frequencies (F1 300..800 Hz, F2 700..2200 Hz, linear between stops). `type_strings` so the 7-segment display shows the nearest vowel. |
| 8    | FLVL  | 0..1  | 0 (dry)     | %       | Formant stage dry/wet. 0 = pure wire (transparent). > 0, the formant color (F1+F2 combs' feedback output) is mixed on top of the intact signal, parallel. |
| 9    | COMB  | 0..1  | 0 (off)     | %       | Comb resonator feedback (0..0.9, clamped < 1). Fixed 250-smp delay (~190 Hz peak). 0 = off. |

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
      -> FORM  (formant F1+F2 one-pole IIR combs, morph by FORM, wet FLVL)
      -> COMB  (IIR comb resonator, fixed delay, feedback COMB)
      -> makeup gain (0.9)
      -> out (stereo)

Note: there is deliberately NO DC-blocker on the output. The ENSE chorus
voices are long one-pole delays whose outputs carry a DC-leaning envelope at
low DRONE frequencies; a ~20 Hz one-pole HPF there (tau ~2 s) let only the
slow level modulation through and ate the entire signal (MOD-on sounded
near-silent). The residual DC offset of the one-pole stages is small and the
0.9 makeup leaves headroom against it.

Every level stage (ENSE / TREMO / FLNG / TAPE / FLVL / COMB) is a dry/wet 0..1
blend: at 0 the stage is a pure wire (regression-safe). FORM is a morph
parameter (not a level); FLVL gates the formant stage's wet. The DEL/REV
stages downstream see the MOD output as their input.

## DRIFT (the shared self-motion ceiling)

Like RATE in the oscillator, DRIFT is a single knob that gates how far the
seven "level" stages wander on their own. The walk is a K-rate random-walk:

- Each of the 7 stages (ENSE/TREMO/FLNG/TAPE/FORM/FLVL/COMB) has a target
  offset within [−d, +d] (d = DRIFT, 0..1) that is re-rolled on an
  exponential schedule (mean gap 2..12 s) and tracked with a one-pole
  (time constant 5..20 s), so the levels drift slowly and smoothly.
- The L1 budget (kDriftBudget = 1.5) clamps the SUM of the seven per-stage
  drift amplitudes, so at most one or two layers wander at a time — the
  motion "percolates" between stages instead of all seven moving in unison.
- The walk is driven by a Park-Miller-Carta LCG (pure 32-bit integer math,
  no libm) — the same seed on the device and in the web simulator.
- At DRIFT = 0 the offsets are zero: every level stays at its manual value
  (the regression-safe default).

## Formant and comb (moved from drone-osc)

The formant (F1+F2 one-pole IIR combs) and comb resonator were formerly the
last three edit-parameters of the DRONE-OSC unit (edit indices 39/40/41,
FMVT / FLVL / COMB). They were relocated to the MOD unit so the oscillator
stays a pure generator and the FX stage owns all the spectral coloring. The
DSP math (delay = sr / (2·f), the 5 vowel stops, the 250-smp comb, the
0..0.9 feedback ceiling) is copied verbatim from `osc_constants.h` into
`modfx_constants.h`; the OSC unit's `kEditParamCount` dropped from 44 to 41
and the tail (FORM/FLVL/COMB) was removed, preserving the first 38 OSC
parameter indices for backward compatibility.

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

The formant comb delays are computed with a float division at K-rate (FPU,
not libm) — fine.

## Files

- `modfx.h` — the `Modfx : Processor` class (Params, LFO phase accumulators,
  DRIFT walk state, formant/comb ring buffers, LCG state, first-order filter
  states, `setParameter`, `getParameterStrValue`, `recompute_formant_comb`)
  AND the full per-sample audio path (`Modfx::modfx_process_impl`, defined
  INLINE in the class so it can be compiled into both the ARM unit and the
  web-simulator chain without a separate TU).
- `modfx_constants.h` — all tuning constants (LFO curves, DRIFT budget,
  formant stops, comb delay, LCG constants) + the libm-free helpers.
- `header.c` — the 10 parameter slots (TIME/DEPTH/DRIFT/ENSE/TREMO/FLNG/TAPE/
  FORM/FLVL/COMB) with their defaults.
- `unit.cc` — the runtime callback trampoline (forwards each header slot 1:1
  to `Modfx::setParameter`; no multiplexer, unlike the OSC's 4-slot
  PARAM/VALUE pair).
- `wasm.cc` — the standalone web-simulator embind interface (legacy; the
  unified UI lives in `drone-chain/` — see below).

## Web simulator

The unified DRONE web simulator is `drone-chain/` (one wasm worklet running
OSC -> MOD -> DEL -> REV). The MOD stage's 10 parameters are exposed in
`websim/index.html` as a 10-slider column below the Parameters card, driven
through `Module.setFxParam(stage=0, which=0..9, raw)`. The FORM slider shows
the nearest vowel (A/E/I/O/U) via `formatFxParam`; the other nine show 0..100%.
The MOD stage's `getBufferSize()` is 16 kfloats (the formant F1+F2 + comb ring
buffers); `Chain::init` slices the chain's single SDRAM allocation across
MOD / DEL / REV in that order.

## Build

    # ARM unit (runs check-und):
    cd platform/nts-1_mkii/drone-modfx && make install

    # Web simulator (unified chain page):
    cd platform/nts-1_mkii/drone-chain && make wasm