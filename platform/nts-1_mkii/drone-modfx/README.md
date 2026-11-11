# DRONE-MOD (NTS-1 mkII modfx unit)

`drone_modfx.nts1mkiiunit` — the modulation-FX stage of the DRONE audio path.
Sits between the DRONE-OSC (generator) and the DRONE-DELAY / DRONE-REVERB
stages on the NTS-1 mkII; in the web simulator it runs inside the unified
`drone-chain` wasm worklet (see `drone-chain/chain.h`).

## The 8 parameters

The MOD unit is an 8-slot unit. Every slot is a 10-bit knob (0..1023) that
maps to a 0..1 DSP value. Defaults match `header.c` and
`Modfx::Params::reset()`.

| Slot | Name  | Range | Default      | Display | What it does |
|------|-------|-------|--------------|---------|--------------|
| 0    | TIME  | 0..1  | 512 (~50%)   | %       | LFO tempo. Sweeps the **ENSE chorus LFO rate** exponentially (base Juno 0.513/0.863 Hz × `e^(4.5·(TIME−0.5))` → ~0.05..~8 Hz); also drives the TREMO / FLNG LFO frequencies via stage-specific exp curves (TREMO 0.05..0.5 Hz, FLNG 0.30..5.0 Hz). TAPE uses LCG noise, not an LFO, so TIME does not move it. |
| 1    | DEPTH | 0..1  | 511 (~50%)   | %       | Per-stage modulation depth. Chorus LFO **delay-spread** (DEPTH scales the LFO excursion around each voice's center delay; DEPTH=0 → no modulation, DEPTH=1 → full 15–20 / 28–34 ms sweep), tremolo dip (DEPTH × 0.85), flanger delay (DEPTH² × ~2.5 ms) and feedback (DEPTH × 0.60), tape wow + flutter (DEPTH × 0.60 / × 0.25). |
| 2    | DRIFT | 0..1  | 0 (off)      | %       | Shared self-motion ceiling (like RATE in the OSC). At 0 all four level stages stay at their manual values. > 0, a K-rate random-walk (LCG-driven) nudges each of ENSE/TREMO/FLNG/TAPE within [manual·(1−d), manual·(1+d)], with an L1 budget (1.5) so the wandering "percolates" between layers instead of all four moving at once. |
| 3    | CMODE | 0..1  | 512 (~50%)   | I/I+II/II | Chorus mode — **3 equal bands** (Formant-Vowel style): band 0 (CMODE<1/3) = **I** (main voice only), band 1 (1/3..2/3) = **I+II** (both voices, classic Juno), band 2 (≥2/3) = **II** (sub voice only). Selects the Juno voice blend (gains {1,0} / {.707,.707} / {0,1}). |
| 4    | DWET  | 0..1  | 1023 (100%)  | %       | Chorus **dry/wet** (the old "ENSE" knob, renamed). 0 = dry only (pure wire through the chorus), 100% = full chorus (dry + wet summed). |
| 5    | TREMO | 0..1  | 307 (~30%)   | %       | Optical tremolo level. 0 = wire. Pure AM: level dips (TREMO·DEPTH·0.85) toward the LFO trough and returns to unity at the peak (no LPF/HPF split, so it no longer mutes the bass). R runs a quarter-cycle behind for a gentle shimmer. |
| 6    | FLNG  | 0..1  | 0 (off)      | %       | Thru-zero flanger level. 0 = off. A short REAL delay line (DEPTH² × ~2.5 ms) swept by an LFO (× 3 rate; R half-cycle out for a stereo comb) + a feedback tap (DEPTH × 0.60, clamped < 0.85). |
| 7    | TAPE  | 0..1  | 0 (off)      | %       | Tape wow + flutter level. 0 = off. Wow: a per-channel LCG random-walk (one-pole smoothed) modulates the length of a ~3 ms REAL delay line → slow pitch/speed wander. Flutter: LCG white noise → one-pole LPF → level AM (± 0.25 × DEPTH). |

> **Display (device):** all 8 slots are `frac = 0`, `type_none` — the NTS-1
> display shows the raw 10-bit value 0..1023 (e.g. `TIME 1023`), NOT a
> percentage. The web simulator renders most as a 0..100% label; CMODE shows
> its Juno voice band (`I` / `I+II` / `II`) — see `formatFxParam` in
> `websim/index.html`. The previous build shipped `frac = 1` (fixed-point,
> `frac_mode = 0`), which made the device divide by `1 << frac = 2` and show
> 0..511.5 — fixed in v1.0.3. The Juno-60 BBD-chorus rewrite (2 mono voices
> @ 18/31 ms, CMODE + TIME-rate + DEPTH-spread + DWET) landed in **v1.0.7**.

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
      -> ENSE  (Juno-60 BBD stereo chorus: 2 MONO REAL delay lines @
               main 15..20 ms / sub 28..34 ms, each driven by 2 TRIANGLE LFOs
               at the TIME-swept base Juno rates 0.513/0.863 Hz; each LFO reads
               the delay at two fractional taps (t and 1-t) and sums them with
               the CMODE-selected voice gains {1,0} / {.707,.707} / {0,1};
               DEPTH scales the LFO delay-spread around each voice center;
               BBD SoftLimit + per-channel 1st-order post-LPF for the stereo
               image; DWET dry/wet blend, 0 = wire, 1 = dry+wet)
      -> TREMO (optical tremolo: pure AM, level dips at the LFO trough;
               R runs a quarter-cycle behind)
      -> FLNG  (thru-zero flanger: short REAL delay swept by LFO x3 rate,
               R half-cycle out for a stereo comb + feedback tap)
      -> TAPE  (wow: per-channel LCG random-walk on a ~3 ms REAL delay line;
               flutter: LCG noise -> LPF -> level AM)
      -> makeup gain (0.9)
      -> out (stereo)

Note: there is deliberately NO DC-blocker on the output. With REAL delay lines
(the chorus/flanger/tape stages are now true line buffers, not one-pole
lowpasses) the stages no longer accumulate a DC-leaning envelope, so a ~20 Hz
one-pole HPF there (tau ~2 s) would have eaten the low-frequency DRONE signal
for no benefit. The 0.9 makeup (kModMakeup) simply leaves 1 dB of headroom
against the chorus/flanger level peaks.

Every level stage (ENSE / TREMO / FLNG / TAPE) is a dry/wet 0..1 blend: at 0
the stage is a pure wire (regression-safe). The DEL/REV stages downstream see
the MOD output as their input. (The formant + comb resonators are part of the
upstream DRONE-OSC unit — see `drone-osc/README.md`.)

## DRIFT (the shared self-motion ceiling)

Like RATE in the oscillator, DRIFT is a single knob that gates how far the
four "level" stages (ENSE/TREMO/FLNG/TAPE) wander on their own. The design is
a K-rate random-walk:

- Each of the 4 stages gets a target offset within [−d, +d] (d = DRIFT, 0..1)
  that is re-rolled on an exponential schedule (mean gap 2..12 s) and tracked
  with a one-pole (time constant 5..20 s), so the levels drift slowly and
  smoothly.
- The L1 budget (kDriftBudget = 1.5) clamps the SUM of the four per-stage
  drift amplitudes, so at most one or two layers wander at a time — the
  motion "percolates" between stages instead of all four moving in unison.
- The walk is driven by a Park-Miller-Carta LCG (pure 32-bit integer math,
  no libm) — the same seed on the device and in the web simulator.
- At DRIFT = 0 the offsets are zero: every level stays at its manual value
  (the regression-safe default).

> **Status:** the walk state (`drift_target_/cur_/tau_/next_gap_`) is allocated
> and zeroed, but the per-sample path currently drives the four levels from the
> manual knobs — DRIFT is a reserved, currently no-op slot (default 0, so the
> sound is unaffected). Wiring the walk into the wet amounts is a follow-up.

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
  DRIFT walk state, LCG state, real delay-line heads, one-pole filter states,
  `setParameter`, `getParameterStrValue`) AND the full per-sample audio path
  (`Modfx::modfx_process_impl`, defined INLINE in the class so it can be
  compiled into both the ARM unit and the web-simulator chain without a
  separate TU).
- `modfx_constants.h` — all tuning constants (LFO curves, DRIFT budget, LCG
  constants, delay-line lengths) + the libm-free helpers.
- `header.c` — the 8 parameter slots (TIME/DEPTH/DRIFT/CMODE/DWET/TREMO/FLNG/TAPE)
  with their defaults.
- `unit.cc` — the runtime callback trampoline (forwards each header slot 1:1
  to `Modfx::setParameter`; no multiplexer, unlike the OSC's 4-slot
  PARAM/VALUE pair).
- `wasm.cc` — the web-simulator embind interface (now only used by the unified
  chain simulator in `drone-chain/` — see below).

## SDRAM (delay lines)

The ENSE / FLNG / TAPE stages use REAL delay lines (true circular line
buffers), not one-pole lowpasses — a one-pole is a lowpass and reads as a
mute, which is exactly the bug the old "modulated one-pole delays" had.
`Modfx::getBufferSize()` reports `kModSramBytes` (two MONO ENSE delay lines at
2048 smp each for the Juno main/sub voices, plus the FLNG L/R pair at 192 smp
and the TAPE L/R pair at 384 smp per channel ≈ 20.5 KB), well inside the NTS-1
modfx slot's 256 KB of allocatable external memory. `unit.cc` allocates it from
`sdram_alloc` and `init()` slices it into the ENSE (2 mono) + FLNG + TAPE lines;
the web chain (`Chain::init`) slices the same size automatically. The
line heads are plain `uint32_t` (exact wrap, no float drift) and the
fractional (LFO-modulated) read offset is interpolated between the two
nearest samples.

## Web simulator

The unified DRONE web simulator is `drone-chain/` (one wasm worklet running
OSC -> MOD -> DEL -> REV). The MOD stage's 8 parameters are exposed in
`websim/index.html` as an 8-slider column below the Parameters card, driven
through `Module.setFxParam(stage=0, which=0..7, raw)`; every slider shows
0..100% except CMODE, which shows its Juno voice band (`I` / `I+II` / `II`).
The MOD stage's `getBufferSize()` is `kModSramBytes` (the ENSE + FLNG + TAPE
delay lines, see *SDRAM* above); `Chain::init` slices the chain's single SDRAM
allocation across MOD / DEL / REV in that order.

## Build

    # ARM unit (runs check-und):
    cd platform/nts-1_mkii/drone-modfx && make install

    # Web simulator (unified chain page):
    cd platform/nts-1_mkii/drone-chain && make wasm