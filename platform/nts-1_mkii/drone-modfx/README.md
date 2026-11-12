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
| 0    | TIME  | 0..1  | 512 (~50%)   | %       | LFO tempo. Sweeps the **ENSE chorus LFO rate** and the FLANGER LFO frequency via a shared exp curve (ENSE 0.0167..0.6 Hz = 60 s..1.7 s; FLANGER 0.05..2.0 Hz; the sub voice is detuned ×1.0618 → the two voices drift in/out of phase = chorus pulsing). Also sweeps the NOISE-branch band-pass cutoff (300 Hz..8 kHz, same exp curve), so the “metallic” center of the rustle tracks the modulation tempo. |
| 1    | DEPTH | 0..1  | 511 (~50%)   | %       | Per-stage modulation depth. Chorus LFO **delay-spread** (DEPTH scales the LFO excursion around each voice's center delay; DEPTH=0 → no modulation, DEPTH=1 → full 15–20 / 28–34 ms sweep), flanger center delay (DEPTH²: 1..7.5 ms), tape wow + flutter (DEPTH × 0.60 / × 0.25). |
| 2    | DRIFT | 0..1  | 0 (off)      | %       | Shared self-motion ceiling (like RATE in the OSC). At 0 all level stages stay at their manual values. > 0, a per-sample random-walk (LCG-driven, exponential target gaps 2..12 s, one-pole tracking tau 5..20 s) runs on the channel states: channel 3 drives the NOISE-branch AM (0.5 + 0.5·walk → the rustle “breathes” in level), channels 0/1 are the reserved ENSE/FLANGER level walks (allocated and advancing, wiring into the wet amounts is a follow-up). |
| 3    | CMODE | 0..1  | 512 (~50%)   | I/I+II/II | Chorus mode — **3 equal bands** (Formant-Vowel style): band 0 (CMODE<1/3) = **I** (main voice only), band 1 (1/3..2/3) = **I+II** (both voices, classic Juno), band 2 (≥2/3) = **II** (sub voice only). Selects the Juno voice blend (gains {1,0} / {.707,.707} / {0,1}). |
| 4    | CHORUS  | 0..1  | 1023 (100%)  | %       | Chorus **dry/wet level** (the old "ENSE"/"DWET" knob, renamed to match the CHORUS/FLANGER per-effect level naming). 0 = dry only (pure wire through the chorus), 100% = full chorus (dry + wet summed). |
| 5    | FLANGER  | 0..1  | 0 (off)      | %       | Thru-zero flanger level (**multi-line / bucket-brigade**). 0 = off. FOUR mono REAL delay lines (1..7.5 ms, 48..360 smp, center set by DEPTH²), each with its OWN LFO phase; the LFO sweeps each read tap ±20% × DEPTH² (0.02..2.0 Hz; R half-cycle out per line -> stereo comb). The four de-tuned comb gratings average into a smooth WASH (no resonant peaks, no “locking” at the LFO turnaround). Small feedback (FLANGER × 0.15, re-injected into the line, Q ~1.18, no self-oscillation). |
| 6    | NOISE    | 0..1  | 0 (off)      | %       | Granular noise-sweep level. 0 = off (dry wire, the branch is never computed). > 0 adds an LCG white-noise “metallic rustle / dust-flash” generator on the path input (before the ENSE chorus): white noise (fx_white) gated by a two-phase burst machine (ON flash / GAP lull), AM-modulated by the DRIFT walk, band-passed by a 2-pole SVF (cutoff swept by TIME + per-burst tone wander), summed additively so the rustle passes through the chorus/flanger “in the wash”. Each flash is randomized on three axes for maximum diversity: **duration** (1..25 ms, log), **level** (0.25..1.0 gain) and **tone** (BP center ±½ octave) — so consecutive knocks never sound alike. |
| 7    | NRATE    | 0..1  | 511 (~50%)   | %       | Noise burst cadence: average flashes/second 0.15..8 (exp curve). 0 = rare flashes with long lulls, 1 = dense shimmer. Each lull (GAP) is exponentially drawn (Poisson) from fx_rand, so the cadence “breathes”. Only audible when NOISE > 0. (This knob sets the flash RATE; per-burst duration/level/tone are independent random draws, not driven by NRATE.) |

> **Display (device):** all 8 slots are `frac = 0`, `type_none` — the NTS-1
> display shows the raw 10-bit value 0..1023 (e.g. `TIME 1023`), NOT a
> percentage. The web simulator renders most as a 0..100% label; CMODE shows
> its Juno voice band (`I` / `I+II` / `II`) — see `formatFxParam` in
> `websim/index.html`. The previous build shipped `frac = 1` (fixed-point,
> `frac_mode = 0`), which made the device divide by `1 << frac = 2` and show
> 0..511.5 — fixed in v1.0.3. The Juno-60 BBD-chorus rewrite (2 mono voices
> @ 18/31 ms, CMODE + TIME-rate + DEPTH-spread + CHORUS dry/wet) landed in **v1.0.7**.

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
chorus delay voices, FLANGER) is per-channel.

    in (stereo L/R)
      -> [NOISE branch]  (additive granular noise-sweep, active when NOISE > 0:
                fx_white() white noise, gated by a two-phase burst machine
                (ON flash / GAP lull; lulls Poisson-drawn from NRATE at
                0.15..8 flashes/s), AM-modulated by the DRIFT walk channel 3
                (0.5 + 0.5*walk), band-passed by a 2-pole state-variable
                filter (cutoff swept by TIME 300 Hz..8 kHz + per-burst tone
                wander +/- half-octave, Q 1.5 fixed; same SVF topology as the
                drone-osc FCUT/RESO filter), each flash randomized in
                duration (1..25 ms), level (0.25..1.0) and tone, summed into
                the input so the rustle passes through the chorus/flanger
                "in the wash")
      -> ENSE  (Juno-60 BBD stereo chorus: 2 MONO REAL delay lines @
               main 15..20 ms / sub 28..34 ms, each driven by a TRIANGLE LFO
               set K-rate from TIME (main = shared exp curve, sub detuned
               x1.0618 -> shared heartbeat, voices drift in/out of phase); each
               LFO reads the delay at two fractional taps (t and 1-t) and sums
               them with
               the CMODE-selected voice gains {1,0} / {.707,.707} / {0,1};
               DEPTH scales the LFO delay-spread around each voice center;
               BBD SoftLimit + per-channel 1st-order post-LPF for the stereo
               image; CHORUS dry/wet blend, 0 = wire, 1 = dry+wet)
      -> FLANGER  (thru-zero flanger, multi-line: 4 mono REAL delay lines 1..7.5 ms,
               each own LFO phase, small feedback re-injected; the 4 de-tuned
               comb gratings average into a smooth wash; R half-cycle out per line)
      -> makeup gain (0.9)
      -> out (stereo)

Note: there is deliberately NO DC-blocker on the output. With REAL delay lines
(the chorus/flanger/tape stages are now true line buffers, not one-pole
lowpasses) the stages no longer accumulate a DC-leaning envelope, so a ~20 Hz
one-pole HPF there (tau ~2 s) would have eaten the low-frequency DRONE signal
for no benefit. The 0.9 makeup (kModMakeup) simply leaves 1 dB of headroom
against the chorus/flanger level peaks.

Every level stage (ENSE / FLANGER / TAPE) is a dry/wet 0..1 blend: at 0
the stage is a pure wire (regression-safe). The DEL/REV stages downstream see
the MOD output as their input. (The formant + comb resonators are part of the
upstream DRONE-OSC unit — see `drone-osc/README.md`.)

## DRIFT (the shared self-motion ceiling)

Like RATE in the oscillator, DRIFT is a single knob that gates how far the
walk channels wander. It is a per-sample random-walk (the same architecture
as the OSC's four random-walk streams: exponentially-drawn target gaps,
one-pole tracking, LCG-driven):

- Each of the 4 channels gets a target offset within [−d, +d] (d = DRIFT,
  0..1) that is re-rolled on an exponential schedule (mean gap 2..12 s,
  Poisson-drawn via fx_rand) and tracked with a one-pole (time constant
  5..20 s, drawn per re-roll), so the values drift slowly and smoothly.
- Channel 3 drives the NOISE-branch AM: AM = 0.5 + 0.5 · walk, so the
  rustle "breathes" in level (0.5 = flat at DRIFT = 0; wanders 0..1 at
  DRIFT > 0).
- Channels 0/1 are the reserved ENSE/FLANGER level walks: allocated,
  advancing, and zeroed at DRIFT = 0, but their output is not yet mixed
  into the chorus/flanger wet amounts (see the status note below).
- The walk is driven by the platform Park-Miller-Carta LCG (`fx_rand()`,
  pure 32-bit integer math, no libm) — the same seed on the device and in
  the web simulator.
- At DRIFT = 0 the walk is static (no fx_rand() consumption at all): every
  channel stays at its manual value (the regression-safe default).

> **Status:** the walk is LIVE on channel 3 (the NOISE-branch AM). Channels
> 0/1 (the ENSE/FLANGER level walks) are allocated and advancing but not yet
> mixed into the wet amounts — wiring them in is a follow-up.

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
  - the DRIFT walk, the noise burst cadence, and the white-noise source are
    driven by the platform Park-Miller-Carta LCG (`fx_rand()` /
    `fx_white()` from common/fx_api.h — the FX-module twin of the OSC unit's
    `osc_rand()` / `osc_white()`) — no hardware random, no libm.

## Files

- `modfx.h` — the `Modfx : Processor` class (Params, LFO phase accumulators,
  DRIFT walk state, NOISE-branch state (burst envelope / SVF states), real
  delay-line heads, one-pole filter states, `setParameter`,
  `getParameterStrValue`) AND the full per-sample audio path
  (`Modfx::modfx_process_impl`, defined INLINE in the class so it can be
  compiled into both the ARM unit and the web-simulator chain without a
  separate TU). Also declares the platform noise API (`fx_rand` /
  `fx_white`, extern "C") that the NOISE branch and the DRIFT walk consume.
- `modfx_constants.h` — all tuning constants (LFO curves, DRIFT budget, LCG
  constants, delay-line lengths) + the libm-free helpers.
- `header.c` — the 7 parameter slots (TIME/DEPTH/DRIFT/CMODE/CHORUS/FLANGER)
  with their defaults.
- `unit.cc` — the runtime callback trampoline (forwards each header slot 1:1
  to `Modfx::setParameter`; no multiplexer, unlike the OSC's 4-slot
  PARAM/VALUE pair).
- `wasm.cc` — the web-simulator embind interface (now only used by the unified
  chain simulator in `drone-chain/` — see below).

## SDRAM (delay lines)

The ENSE / FLANGER / TAPE stages use REAL delay lines (true circular line
buffers), not one-pole lowpasses — a one-pole is a lowpass and reads as a
mute, which is exactly the bug the old "modulated one-pole delays" had.
`Modfx::getBufferSize()` reports `kModSramBytes` (two MONO ENSE delay lines at
2048 smp each for the Juno main/sub voices, plus the FLANGER L/R pair at 192 smp
and the TAPE L/R pair at 384 smp per channel ≈ 20.5 KB), well inside the NTS-1
modfx slot's 256 KB of allocatable external memory. `unit.cc` allocates it from
`sdram_alloc` and `init()` slices it into the ENSE (2 mono) + FLANGER + TAPE lines;
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
The web chain also provides the `fx_rand()` / `fx_white()` stubs
(Park-Miller-Carta, sharing the OSC stubs' LCG state) in `drone-chain/wasm.cc`
so the NOISE branch and the DRIFT walk run identically to the device.
The MOD stage's `getBufferSize()` is `kModSramBytes` (the ENSE + FLANGER + TAPE
delay lines, see *SDRAM* above); `Chain::init` slices the chain's single SDRAM
allocation across MOD / DEL / REV in that order.

## Build

    # ARM unit (runs check-und):
    cd platform/nts-1_mkii/drone-modfx && make install

    # Web simulator (unified chain page):
    cd platform/nts-1_mkii/drone-chain && make wasm