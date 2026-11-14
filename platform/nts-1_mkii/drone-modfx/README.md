# DRONE-MOD (NTS-1 mkII modfx unit)

`drone_modfx.nts1mkiiunit` — the modulation-FX stage of the DRONE audio path.
Sits between the DRONE-OSC (generator) and the DRONE-DELAY / DRONE-REVERB
stages on the NTS-1 mkII; in the web simulator it runs inside the unified
`drone-chain` wasm worklet (see `drone-chain/chain.h`).

## The parameter model (6 header slots + 10 edit parameters)

The MOD unit uses **the same split as drone-osc, but with TWO independent
PARAM/VALUE multiplexers**. Two **fixed** knobs (TIME / DEPTH, always on the
A/B encoders) plus 10 edit parameters split into two groups:

- **MOD group** — `PARAM`/`VALUE` (header slots 2/3), selectors 0..4:
  DRIFT / CMODE / CHORUS / FLANGER / ENSMBL (the modulation stages).
- **TAPE group** — `TPARAM`/`TVALUE` (header slots 4/5), selectors 0..4:
  NOISE / NRATE / TAPWOB / HISS / WARP (the KoS-derived film/tape stage).

The unit has 6 header slots (`header.c`). `unit.cc` maps header slot 2 →
`PARAM_SEL`, slot 3 → `PARAM_VAL`, slot 4 → `PARAM_TSEL`, slot 5 → `PARAM_TVAL`
before forwarding, so `Modfx::setParameter` only ever sees
`PARAM_TIME / PARAM_DEPTH / PARAM_SEL / PARAM_VAL / PARAM_TSEL / PARAM_TVAL`.

### The 6 header slots

| Slot | Name  | Range    | Default | Type     | What it does |
|------|-------|----------|---------|----------|--------------|
| 0    | TIME  | 0..1023  | 512     | none (%) | **Fixed A knob.** LFO tempo (0..1). Sweeps the **chorus / flanger / ensemble / tape-wob LFO rate** via a shared exp curve (0.0167..0.6 Hz = 60 s..1.7 s). |
| 1    | DEPTH | 0..1023  | 511     | none (%) | **Fixed B knob.** Per-stage modulation depth (0..1). Chorus LFO **delay-spread**, flanger center delay (DEPTH²), ensemble LFO spread (DEPTH²), TAPWOB wow depth (DEPTH³) and flutter depth (DEPTH²). |
| 2    | PARAM | 0..4     | 0       | strings  | **MOD-group multiplexer selector.** Which of the 5 MOD edit parameters is being edited. Display shows the selected parameter's NAME (DRIFT / CMODE / CHOR / FLNGR / ENSMBL) via `getParameterStrValue`. |
| 3    | VALUE | 0..1023  | 0       | strings  | **Raw value of the selected MOD edit parameter.** Display shows the FORMATTED value (CMODE → I / I+II / II, everything else → 0..100%). |
| 4    | TPARAM| 0..4     | 0       | strings  | **TAPE-group multiplexer selector.** Which of the 5 TAPE edit parameters is being edited. Display shows the selected parameter's NAME (NOISE / NRATE / TAPWOB / HISS / WARP). |
| 5    | TVALUE| 0..1023  | 0       | strings  | **Raw value of the selected TAPE edit parameter.** Display shows the FORMATTED value (0..100%). |

### The 10 edit parameters (two multiplexers, 5 each)

**MOD group** — `select_` (0..4) indexes `edit_value_[5]`:

| Edit | Name   | Default      | Display   | What it does |
|------|--------|--------------|-----------|--------------|
| 0    | DRIFT  | 0 (off)      | %         | Shared self-motion ceiling (like RATE in the OSC). At 0 all level stages stay at their manual values. > 0, a per-sample random-walk (LCG-driven, exponential target gaps 2..12 s, one-pole tracking tau 5..20 s) runs on the channel states: channel 3 drives the NOISE-branch AM (0.5 + 0.5*walk -> the rustle "breathes" in level), channels 0/1 are the reserved CHORUS/FLANGER level walks. |
| 1    | CMODE  | 0 (band 0)   | I/I+II/II | Chorus mode -- **3 equal bands**: band 0 (CMODE<1/3) = **I** (main voice only), band 1 (1/3..2/3) = **I+II** (both voices, classic Juno), band 2 (>=2/3) = **II** (sub voice only). Selects the Juno voice blend (gains {1,0} / {.707,.707} / {0,1}). |
| 2    | CHORUS | 0 (dry)      | %         | Chorus **dry/wet level**. 0 = dry only (pure wire through the chorus), 100% = full chorus (dry + wet summed). |
| 3    | FLNGR  | 0 (off)      | %         | Thru-zero flanger level (**multi-line / bucket-brigade**). 0 = off. FOUR mono REAL delay lines (1..7.5 ms, center set by DEPTH^2), each with its OWN LFO phase; the LFO sweeps each read tap +/-20% x DEPTH^2 (R half-cycle out per line -> stereo comb). The four de-tuned comb gratings average into a smooth WASH. Small feedback (FLANGER x 0.15, re-injected, Q ~1.18, no self-oscillation). |
| 4    | ENSMBL | 0 (off)      | %         | **String-synth ensemble** level (RS-202 style). 0 = off (dry wire, regression-safe). > 0 adds a "strings" generator on the path input (before the Juno CHORUS): one **shared mono** REAL delay line (1024 smp = 21 ms), **three read taps at LFO phases 120 deg apart**, summed and averaged (x 1/3). The LFO is a **binary-sine** (full-amplitude base + 1/16 x 10x harmonic) whose base rate uses the **SAME exp curve as CHORUS / FLANGER** (0.0167..0.6 Hz), so the ensemble moves in **lockstep** with the other stages. The delay geometry is a **fixed 15 ms center (720 smp)** with a **DEPTH^2-driven spread** (460..980 smp at DEPTH=1). The wet norm is **x 1.0** (matched to CHORUS). |

**TAPE group** — `tsel_` (0..4) indexes `tedit_value_[5]` (the KoS-derived
film/tape stages):

| Edit | Name   | Default      | Display   | What it does |
|------|--------|--------------|-----------|--------------|
| 0    | NOISE  | 0 (off)      | %         | **Granular rustle** level (KoS HurricaneSandy "age"). 0 = off (dry wire, never computed). > 0 adds a **procedural PINK-noise source** (3 one-pole LPF, Voss) feeding a shared mono RING (8192 smp); a small **granulator** (8 linear-env grains with random pointer/offset) reads the ring, band-shaped by an HPF (50 Hz) + per-channel LPF (2 kHz); an **AM "dips" curve** (16 random points over 1 s, re-seeded by fx_rand, **dips 1.0 → 0.15** at kDipsExtremity = 0.85) makes the rustle **breathe** (deep enough to be audible as a level swing); periodic white-noise **bursts** (cadence from NRATE, **128-smp scrape window**, **dBurstPeak = 0.55**, **dBurstDuckDepth = 0.5**) **DUCK the dry signal** (the KoS sigEnv scrape); a low-freq **room tone** (pink -> LPF 80 Hz) fills the low end. The whole source is scaled by `kNoiseWetNorm = 0.08` before adding to the dry. The grain gain is **kGrainGain = 0.45** (raised from 0.15) so the rustle is clearly audible at ~20% of the knob and grows smoothly to 100%. Summed additively so the rustle passes through the chorus/flanger "in the wash". The DRIFT walk channel 3 AM-modulates the rustle. |
| 1    | NRATE  | 0 (rarest)   | %         | Noise **burst cadence**: average flashes/second 0.15..8 (exp curve). 0 = rare flashes with long lulls, 1 = dense shimmer. Each lull is exponentially drawn (Poisson) from fx_rand. Only audible when NOISE > 0. The scrape window is **128 smp (~2.7 ms)** (raised from 16 smp) so the NRATE cadence is clearly audible across the 0..100% range — at 16 smp the bursts were sub-millisecond ticks that did not change perceived loudness. |
| 2    | TAPWOB | 0 (off)      | %         | **Tape wow/flutter pitch-mod** -- the ONLY stage that modulates PITCH. 0 = off (dry wire). > 0 adds a tape deck on the master bus (AFTER CHORUS/FLANGER, before the makeup): one **shared mono** REAL delay line (512 smp), L/R read taps offset by a **fixed 5.3 ms center (256 smp)** plus TWO independent LFOs -- **WOW** (slow, 0.0167..0.6 Hz at TIME 0..1, up to 40 cents x DEPTH^3) and **FLUTTER** (fast, 0.5..15 Hz at TIME 0..1, up to 12 cents x DEPTH^2; R's flutter is 90 deg out of L). A 1st-order LPF per channel (cutoff 2..8 kHz, TAPWOB-swept) dulls the highs as the deck "wears". **Dry/wet replace** (NOT additive). |
| 3    | HISS   | 0 (off)      | %         | **Tape hiss** level (KoS Hiss, procedural -- no WAV, the SDK has no pink/hiss sample). 0 = off. > 0 adds `fx_white()` -> 1st-order HPF (1500 Hz) -> a quiet level (max **~ −12 dB, kHissMaxGain = 0.25**, raised from 0.08), summed to L/R (the HPF turns white into "hiss" color). **Also attenuates the dry signal** by `dry_scale = 1 − hiss·0.4` (kHissDryDuck = 0.4) so the effect reads as "tape on the master bus" (the KoS Hiss class does the same: signalLevel = 1 − hissLevel). At HISS = 1 the dry is ducked to 0.6 (−4.4 dB) so the drone stays present. Additive, on the master bus after TAPWOB. |
| 4    | WARP   | 0 (off)      | %         | **Magnetic instability** level (KoS "Shame"). 0 = off (dry wire). > 0 re-tapes the signal: one **shared mono** REAL delay line (4096 smp = 85 ms), the mono input written once per sample, the read tap = (center 2048 smp + cosine-LFO x depth **0..30 smp** (kWarpMaxDepthSmp = 30, **halved from 60** so full WARP reads as a gentle "magnetic sag", not a pitch wobble)). The cosine LFO rate is set from WARP (7..77 Hz, exp) and gets a **random rate kick each cycle** (fx_rand x periodicity 0.5..0.25) so the wobble never repeats -- the "tape is alive" feel. L/R read the same tap (mono instability); stereo image is kept by the other stages. **Dry/wet replace** (T = WARP). |

> **Display (device):** the 4 string slots (PARAM / VALUE / TPARAM / TVALUE)
> show the selected parameter's NAME (PARAM/TPARAM) or FORMATTED value
> (VALUE/TVALUE: CMODE → `I`/`I+II`/`II`, everything else → 0..100%) via
> `getParameterStrValue`. The web simulator renders each edit parameter as a
> 0..100% slider (CMODE shows its Juno voice band).

## Signal path

**Stereo in / stereo out, interleaved** (`in[2n]=L, in[2n+1]=R`, `out[2n]=L,
out[2n+1]=R`). MOD does NOT upmix: the NTS-1 platform feeds modfx units true
stereo (see `drone-modfx/unit.cc`, which rejects non-2-channel geometry), and
in the web simulator `Chain::process` (drone-chain/chain.h) upmixes the mono
OSC to L/R *before* the first FX stage. The OLD code read `in[n]` (a mono
layout) from the interleaved buffer -- i.e. it consumed the L half only and
walked the R half as if it were the next L frames (a half-rate garbled
signal; clean in the web chain only because L==R there). The per-sample path
now reads both channels and every stateful stage (CHORUS pre/post LPF, the
chorus delay voices, FLANGER) is per-channel.

    in (stereo L/R)
      -> [NOISE branch]  (additive granular rustle, active when NOISE > 0:
                a PINK-noise source (3 one-pole LPF, Voss) feeds a shared
                mono RING (8192 smp); a small granulator (8 linear-env grains
                with random pointer/offset) reads the ring, band-shaped by an
                HPF (50 Hz) + per-channel LPF (2 kHz); an AM "dips" curve
                (16 random points over 1 s, re-seeded by fx_rand) makes the
                rustle breathe; periodic white-noise bursts (cadence from
                NRATE at 0.15..8 flashes/s) DUCK the dry signal (the KoS
                sigEnv scrape); a low-freq room tone (pink -> LPF 80 Hz)
                fills the low end; AM-modulated by the DRIFT walk channel 3
                (0.5 + 0.5*walk); the whole source is scaled by kNoiseWetNorm
                (0.08) before being summed into the input so the rustle
                passes through the chorus/flanger "in the wash".
                Source: KoS HurricaneSandy "age" stage, rewritten
                procedurally -- no WAV (the SDK has no pink/hiss sample).)
      -> [ENSEMBLE branch] (additive string-synth ensemble, active when ENSMBL > 0:
                1 shared MONO REAL delay line @ 1024 smp (21 ms), 3 read taps
                at LFO phases 120 deg apart (0 / 1/3 / 2/3 of a cycle),
                summed and averaged (x1/3); the LFO is a binary-sine
                1.0*sin(2*pi*f*t) + (1/16)*sin(20*pi*f*t) -- full-amplitude
                base rate f TIME-swept 0.0167..0.6 Hz (SAME exp curve as
                CHORUS / FLANGER: 60 s..1.7 s, lockstep) + a gentle 10x
                "breathing" harmonic (instantaneous rate wobbles ~+/-12.5%);
                delay = fixed 15 ms center (720 smp) + DEPTH^2-driven spread
                (0 at DEPTH=0 -> 460..980 smp / 9.6..20.4 ms at DEPTH=1); the R
                channel reads the SAME taps shifted 367 smp (~90 deg of the
                line) for the stereo spread; 1st-order LPF (alpha 0.3) per
                channel; summed into the input so the strings pass through
                the chorus/flanger "in the wash". Architecture:
                quarterturn/teensy3-ensemble-chorus, adapted to float.)
      -> CHORUS  (Juno-60 BBD stereo chorus: 2 MONO REAL delay lines @
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
      -> [TAPWOB branch] (tape wow/flutter pitch-mod, active when TAPWOB > 0:
                the ONLY stage that modulates PITCH. 1 shared MONO REAL delay
                line @ 512 smp (10.7 ms, deliberately long so the read-tap
                velocity stays < 1 sample/sample and never clicks); the mono
                input is written once per sample, L/R read taps offset by a fixed
                5.3 ms center (256 smp) plus TWO independent LFOs -- WOW (slow,
                0.0167..0.6 Hz from TIME, up to 40 cents x DEPTH^3) and FLUTTER
                (fast, 0.5..15 Hz from TIME, up to 12 cents x DEPTH^2; R's
                flutter 90 deg out of L for a stereo "tape" image); the LFO rates
                follow the SAME shared TIME exp curve as CHORUS / FLANGER /
                ENSMBL (lockstep); WOW cents depth is scaled by DEPTH^3 (cubic,
                much shallower than the FLANGER's DEPTH^2) so low DEPTH stays a
                subtle pitch shimmer, while flutter stays on DEPTH^2 (12 cents is
                already tame); cents -> sample offset (center/1200) is a constant
                so the pitch shift is exactly (wow+flutter) cents; 1st-order LPF
                per channel (cutoff 2..8 kHz, TAPWOB-swept) dulls the highs as
                the deck "wears"; DRY/WET REPLACE (not additive): out =
                dry*(1-T) + wet*T, T = TAPWOB (full wet at TAPWOB=1). Sits AFTER
                CHORUS/FLANGER (a tape deck worn over the whole FX chain), before
                the makeup. TAPWOB is DRY/WET LEVEL only; TIME and DEPTH drive
                the tempo and depth, like every other stage.)
      -> [HISS branch]  (tape hiss, active when HISS > 0: fx_white() -> 1st-order
                HPF (1500 Hz) -> a quiet level (max ~ -22 dB, kHissMaxGain
                0.08), summed to L/R.
                The HPF turns white noise into "hiss" color. Additive, on the
                master bus after TAPWOB. Source: KoS "Hiss" stage, rewritten
                procedurally -- no WAV (the SDK has no pink/hiss sample).)
      -> [WARP branch]  (magnetic instability, active when WARP > 0: 1 shared
                MONO REAL delay line @ 4096 smp (85 ms); the L channel of the
                interleaved stereo input is written once per sample (L==R in
                the web chain; the NTS-1 platform upmixes the OSC mono too),
                the read tap = center (2048 smp) + cosine-LFO x depth
                (0..60 smp). The line is zeroed in init() so the first
                reads (before the line is filled) return silence, not SDRAM
                garbage. The cosine LFO rate is set from WARP (7..77 Hz,
                exp) and gets a RANDOM rate kick each cycle (fx_rand x
                periodicity 0.5..0.25) so the wobble never repeats -- the
                "tape is alive" feel. L/R read the same tap (mono
                instability); the stereo image is kept by the other stages.
                DRY/WET REPLACE (not additive): out = dry*(1-T) + wet*T,
                T = WARP (full wet at WARP=1). Source: KoS "Shame" stage,
                rewritten with a cosine LFO (no STK/KoS code, MIT-clean).)
      -> makeup gain (1.0 = unity, bypass-transparent)
      -> out (stereo)

Note: there is deliberately NO DC-blocker on the output. With REAL delay lines
(the chorus/flanger/tape stages are now true line buffers, not one-pole
lowpasses) the stages no longer accumulate a DC-leaning envelope, so a ~20 Hz
one-pole HPF there (tau ~2 s) would have eaten the low-frequency DRONE signal
for no benefit. The makeup is UNITY (kModMakeup = 1.0): with all stage levels
at 0 the unit is a pure wire, so the output gain must be unity or enabling the
MOD slot would be an audible ~0.9 dB step that no modulation stage caused
(stage peaks are bounded by their own wet ceilings: kChorusWetMax = 0.5,
flanger wet = 0.5 x knob, ...).

Every level stage (CHORUS / FLANGER / TAPE) is a dry/wet 0..1 blend: at 0
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
- Channels 0/1 are the reserved CHORUS/FLANGER level walks: allocated,
  advancing, and zeroed at DRIFT = 0, but their output is not yet mixed
  into the chorus/flanger wet amounts (see the status note below).
- The walk is driven by the platform Park-Miller-Carta LCG (`fx_rand()`,
  pure 32-bit integer math, no libm) — the same seed on the device and in
  the web simulator.
- At DRIFT = 0 the walk is static (no fx_rand() consumption at all): every
  channel stays at its manual value (the regression-safe default).

> **Status:** the walk is LIVE on channel 3 (the NOISE-branch AM). Channels
> 0/1 (the CHORUS/FLANGER level walks) are allocated and advancing but not yet
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
  DRIFT walk state, NOISE-branch state (pink ring / granulator / dips / bursts),
  delay-line heads, one-pole filter states, `setParameter`,
  `getParameterStrValue`) AND the full per-sample audio path
  (`Modfx::modfx_process_impl`, defined INLINE in the class so it can be
  compiled into both the ARM unit and the web-simulator chain without a
  separate TU). Also declares the platform noise API (`fx_rand` /
  `fx_white`, extern "C") that the NOISE branch and the DRIFT walk consume.
- `modfx_constants.h` — all tuning constants (LFO curves, DRIFT budget, LCG
  constants, delay-line lengths) + the libm-free helpers.
- `header.c` — the 6 header slots: TIME / DEPTH (fixed, type_none) + PARAM /
  VALUE (the MOD-group multiplexer) + TPARAM / TVALUE (the TAPE-group
  multiplexer, type_strings). The 10 edit parameters (5 MOD-group: DRIFT /
  CMODE / CHORUS / FLANGER / ENSMBL; 5 TAPE-group: NOISE / NRATE / TAPWOB /
  HISS / WARP) live behind the two multiplexers (see *The parameter model*
  above).
- `unit.cc` — the runtime callback trampoline. Maps header slot 2 → PARAM_SEL,
  slot 3 → PARAM_VAL, slot 4 → PARAM_TSEL, slot 5 → PARAM_TVAL (the
  6-slot / two-multiplexer model, like drone-osc's PARAM/VALUE but doubled);
  the VALUE slot (3) reports `Modfx::getValue()` (the selected MOD edit
  param's raw value) and the TVALUE slot (5) reports `Modfx::getTapeValue()`
  (the selected TAPE edit param's raw value); the rest report their cached
  values.
- `wasm.cc` — the web-simulator embind interface (now only used by the unified
  chain simulator in `drone-chain/` — see below).

## SDRAM (delay lines)

The CHORUS / FLANGER / ENSEMBLE / TAPWOB / WARP / NOISE stages use REAL delay
lines / ring buffers (true circular line buffers), not one-pole lowpasses — a
one-pole is a lowpass and reads as a mute, which is exactly the bug the old
"modulated one-pole delays" had. `Modfx::getBufferSize()` reports
`kModSramBytes`: two MONO CHORUS delay lines at 2048 smp each (the Juno
main/sub voices), the four FLANGER MONO bucket-brigade lines at 1024 smp each,
the ENSEMBLE shared MONO line at 1024 smp, the TAPWOB shared MONO line at 512
smp, the WARP shared MONO line at 4096 smp, and the NOISE pink ring at 8192
smp: 2·2048·4 + 4·1024·4 + 1024·4 + 512·4 + 4096·4 + 8192·4 ≈ 90 KB, well inside
the NTS-1 modfx slot's 256 KB of allocatable external memory. `unit.cc`
allocates it from `sdram_alloc` and `init()` slices it into the CHORUS (2
mono) + FLANGER (4 mono) + ENSEMBLE (1 mono) + TAPWOB (1 mono) + WARP (1 mono)
+ NOISE (1 mono ring) lines in that order; the web chain (`Chain::init`)
slices the same size automatically. The line heads are plain `uint32_t` (exact
wrap, no float drift) and the fractional (LFO-modulated) read offset is
interpolated between the two nearest samples.

## Web simulator

The unified DRONE web simulator is `drone-chain/` (one wasm worklet running
OSC -> MOD -> DEL -> REV). The MOD stage uses the same TWO-multiplexer model as
the device: TIME / DEPTH are fixed sliders (driven through
`Module.setFxParam(stage=0, which=0/1, raw)`), the 5 **MOD-group** edit
parameters (DRIFT / CMODE / CHORUS / FLANGER / ENSMBL) are 5 sliders driven
through `Module.modSetEditParam(idx=0..4, raw)`, and the 5 **TAPE-group** edit
parameters (NOISE / NRATE / TAPWOB / HISS / WARP) are 5 more sliders driven
through `Module.modSetTapeParam(idx=0..4, raw)` — each writes its
(select, value) pair atomically so the DSP never sees a stale selector.
Every slider shows 0..100% except CMODE, which shows its Juno voice band
(`I` / `I+II` / `II`). The web chain also provides the `fx_rand()` /
`fx_white()` stubs (Park-Miller-Carta, sharing the OSC stubs' LCG state) in
`drone-chain/wasm.cc` so the NOISE granulator, the DRIFT walk, and the TAPWOB /
WARP LFOs run identically to the device. The MOD stage's `getBufferSize()` is
`kModSramBytes` (the CHORUS + FLANGER + ENSEMBLE + TAPWOB + WARP + NOISE delay
lines / ring, see *SDRAM* above); `Chain::init` slices the chain's single
SDRAM allocation across MOD / DEL / REV in that order.

## Build

    # ARM unit (runs check-und):
    cd platform/nts-1_mkii/drone-modfx && make install

    # Web simulator (unified chain page):
    cd platform/nts-1_mkii/drone-chain && make wasm