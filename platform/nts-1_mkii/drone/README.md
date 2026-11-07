# DRONE

A dual wavetable drone generator for the **NTS-1 mkII** (Logue SDK).

The unit is a single-voice evolving drone: two detuned wavetable voices at the
base note, a two-voice "chord" at selectable intervals, two sub-octave voices,
an asymmetric saturator + wavefolder, an internal 12 dB/oct SVF, and four
independent random-walk "wandering" streams that make the tone, timbre,
density and voice levels breathe over time. There is no per-sample branching on
audio data and no dynamic allocation -- all state lives inside the Osc class.

---

## Sound path

The audio thread runs one pass per sample (Osc::process, in osc_mix.h).
Five parallel branches are mixed, then lifted by a makeup gain and passed
through a shared tanh soft-clip:

```
out = tanh( kOutSat * kOutDrive * ( y_main*MAIN + y_chord + y_sub1 + y_sub2 + y_ring ) )

MAIN voice (osc A + osc B)
  phasor A  ->  wavetable A (WAV1)   [ DETU, FM_A, RFM_A, JIT, + walk detune ]
  phasor B  ->  wavetable B (WAV2)   [ DETU, FM_B, RFM_B, JIT, + walk detune ]
  y = (wavA + wavB) / 2
    -> FOLD (wavefolder, FOLD)
    -> SATU/ASYM (asymmetric tanh saturator, SATU + ASYM + RDRV walk)
    -> SVF 12 dB/oct LPF (FCUT/RES + RWFC cutoff walk, dry/wet)
    = y_main

CHORD voice (osc C at CINT, osc C2 at CINT2)
  phasor C  ->  wavetable C (WAV3)  at base * 2^(CINT/1200)
  phasor C2 ->  wavetable C (WAV3)  at base * 2^(CINT2/1200)
  each: -> FOLD (CHFLD / CHFLD2) -> SATU (CHSAT / CHSAT2, shared ASYM)
  y_chord = (c1 + c2) * CHRD * lvl_c      (lvl_c = Breath stream)

SUB1 voice : sine at f/2 (one octave down, -4 c detune) -> * SUB1 * lvl_sub
SUB2 voice : sine at f/4 -> soft tanh -> 200 Hz LPF -> pitch gate -> * SUB2
RING voice : (wavA * wavB) * RMOD * MAIN   (A*B ring-mod, ducked by MAIN)

The A-H-R envelope (2 s / 2 s) multiplies the whole sum (note attack/release).
```

Key facts about the path:

* **Detune (DETU)** is applied *per voice* (A gets +c/2 cents, B gets -c/2),
  so the inter-voice beat is ~4x stronger than a post-mix sum detune. The chord
  and sub voices stay on the base note (stable "anchors").
* **Phase-FM** (FM_A / FM_B) modulates the carrier *phasors* (classic Bessel
  sidebands). The modulator is the *other* voice's wavetable output, one sample
  delayed. The static index is **not** gated by RATE; RFM_A / RFM_B add a
  random-walk "breathing" on top (RATE-gated).
* **JIT** adds per-sample white-noise phase to each of A and B (smears the
  carrier into a dense partial; full knob = ~0.5% peak phase deviation).
* The **SUB2** voice bypasses the main soft-clip on purpose (a low-frequency
  sine through tanh would be heavily attenuated) and is gated closed below
  ~120 Hz base pitch (f/4 < 30 Hz).
* **Note glide**: when the base note changes, the base phase increment is
  interpolated linearly over **1 s** and every derived voice (A/B, sub-1,
  sub-2, chord, chord-2, wavetable morph) is recomputed from it, so the whole
  sound slides in pitch together.

### Envelope / key behaviour

The unit runs its own **A-H-R** envelope (the platform EG is set to *open*):
2 s exponential attack, hold at full level while the note is active, 2 s
exponential release. Key behaviour depends on the Legato switch:

* **Legato OFF** (default): key release starts the 2 s release. Pressing a
  *different* key re-targets the pitch (1 s glide, amplitude stays continuous);
  re-pressing the *same* key re-triggers a fresh attack.
* **Legato ON**: key release does **not** stop the sound. Re-pressing the
  *same* key fades it out (toggle-off); pressing a *different* key retargets
  the pitch and keeps playing.

The **Rnd** control (edit-menu / web simulator only) re-rolls every edit
parameter to a fresh audition-friendly random preset -- the same windows the
web simulator's *Random* button uses (see osc_randomize.h).

---

## Parameters

The unit exposes its **41** DSP knobs through a PARAM/VALUE multiplexer on the
two physical knobs:

* **Slot 0 PARAM** (fixed knob A) -- parameter selector, 0..40; the display
  shows the name of the selected parameter.
* **Slot 1 VALUE** (fixed knob B) -- raw 10-bit value (0..1023) of the selected
  parameter. Each parameter maps 0..1023 onto its own range; the raw value is
  cached per parameter, so re-selecting one restores its last value.
* **Slots 2/3 PARAM/VALUE** -- full duplicates of 0/1 (edit-menu visibility).
* **Slot 4 Rnd** (edit-menu) -- randomize trigger (see above).
* **Slot 5 Legato** (edit-menu) -- key-release behaviour switch (on/off).

All 41 editable DSP parameters (select_ index -> name), with range, factory
default and effect:

| # | Name | Display | Range | Default | Effect |
|---|------|---------|-------|---------|--------|
| 0 | DETU | +/-c | -10..+10 c | 0 c | Static symmetric detune: A = +c/2, B = -c/2. |
| 1 | RATE | % | 0..100% (quadratic) | 0% (off) | Shared ceiling for every random-walk stream. 0 = all wandering off. |
| 2 | SPD1 | % | 0..100% -> 0.1 s..10 s (log) | 50% | **Tone** stream period (mean target-update gap). |
| 3 | SMT1 | % | 0..100% -> 10 ms..1.5 s (log) | 50% | **Tone** stream time constant. |
| 4 | SPD2 | % | 0..100% -> 0.1 s..10 s (log) | 50% | **Timbre** stream period. |
| 5 | SMT2 | % | 0..100% -> 10 ms..1.5 s (log) | 50% | **Timbre** stream time constant. |
| 6 | SPD3 | % | 0..100% -> 0.1 s..10 s (log) | 50% | **Density** stream period. |
| 7 | SMT3 | % | 0..100% -> 10 ms..1.5 s (log) | 50% | **Density** stream time constant. |
| 8 | SUB1 | % | 0..100% | 50% | Sub-octave (f/2) sine level. |
| 9 | SUB2 | % | 0..100% | ~20% | Second sub (f/4) saturated level (0 = off). |
| 10 | SATU | 0..300 | 0..3.0 drive | 100 (1.0x) | Main A+B saturator drive. |
| 11 | ASYM | +/-% | -100..+100% | 0% | Saturator asymmetry (even harmonics; >0 = positive half harder). |
| 12 | FOLD | % | 0..100% | 0% (off) | Pre-drive wavefolder depth (metallic edge). |
| 13 | WAV1 | index | 0..90 (banks A-F) | 0 | Osc A wavetable. |
| 14 | WAV2 | index | 0..90 (banks A-F) | ~11 | Osc B wavetable. |
| 15 | WAV3 | index | 0..90 (banks A-F) | 0 | Chord-voice (osc C/C2) wavetable. |
| 16 | CHRD | % | 0..100% | ~20% | Chord-voice level (both intervals CINT + CINT2). |
| 17 | CINT | NNNc | 0..1900 c (continuous) | ~700 c (fifth) | First chord interval above the base note. |
| 18 | MAIN | % | 0..100% | 100% | Main (A+B) voice level (also ducks the ring-mod voice). |
| 19 | RWAV | % | 0..100% | 0% (off) | Walk depth on wavetable morph (+/-10 tables). |
| 20 | RDRV | % | 0..100% | 0% (off) | Walk depth on saturator drive (+/-0.5). |
| 21 | RFLD | % | 0..100% | 0% (off) | Walk depth on wavefolder depth (+/-0.5). |
| 22 | FCUT | Hz / kHz | 80 Hz..12 kHz (log) | ~9 kHz | SVF cutoff **and** dry/wet blend (0% = raw, 100% = full LPF). |
| 23 | RES | X.Y | 0.5..2.5 Q | 0.5 | SVF resonance. |
| 24 | RWFC | % | 0..100% | 0% (off) | Walk depth on SVF cutoff (+/-1 half-octave). |
| 25 | RVLL | % | 0..100% | 0% (off) | **Breath** stream depth on the four voice levels. |
| 26 | RVLS | % | 0..100% -> 0.1 s..10 s (log) | 50% | **Breath** stream period. |
| 27 | RVLSM | % | 0..100% -> 10 ms..1.5 s (log) | 50% | **Breath** stream time constant. |
| 28 | CHSAT | 0..300 | 0..3.0 drive | = SATU | CINT interval's own saturator drive (shared ASYM). |
| 29 | CHFLD | % | 0..100% | = FOLD (0%) | CINT interval's own wavefolder depth. |
| 30 | CINT2 | NNNc | 0..1900 c (continuous) | ~950 c (major sixth) | Second chord interval (shares CHRD level, WAV3, Breath). |
| 31 | JIT | % | 0..100% -> 0..0.5% phase | 0% (off) | Random phase jitter on A/B (smears the carrier). |
| 32 | RMOD | % | 0..100% | 0% (off) | A*B ring-mod parallel voice (ducked by MAIN). |
| 33 | FM1 | Nc | 0..100% (quadratic) -> ~0.5% phase | 0% (off) | Static phase-FM of osc A by B (not RATE-gated). |
| 34 | FM2 | Nc | 0..100% (quadratic) -> ~0.5% phase | 0% (off) | Static phase-FM of osc B by A (not RATE-gated). |
| 35 | RFMA | % | 0..100% | 0% (off) | Walk depth on the FM-A index ("breathing" FM, RATE-gated). |
| 36 | RFMB | % | 0..100% | 0% (off) | Walk depth on the FM-B index ("breathing" FM, RATE-gated). |
| 37 | CHSAT2 | 0..300 | 0..3.0 drive | = CHSAT | CINT2 interval's own saturator drive. |
| 38 | CHFLD2 | % | 0..100% | = CHFLD (0%) | CINT2 interval's own wavefolder depth. |

### The four random-walk streams

RATE is the single ceiling that enables all wandering (0 = everything static).
Each stream is independent (own period SPDx / RW_LSP, own time constant
SMTx / RW_LSM) and its targets are **not** phase-locked:

| Stream | Period / Smooth | Target knob(s) | Drives |
|--------|-----------------|----------------|--------|
| **Tone** | SPD1 / SMT1 | (depth from RATE) | A/B detune (+/-10 c) + FM index breathing |
| **Timbre** | SPD2 / SMT2 | RWAV, RWFC | wavetable morph (+/-10 tables) + SVF cutoff (+/-1 half-oct) |
| **Density** | SPD3 / SMT3 | RDRV, RFLD | saturator drive (+/-0.5) + wavefolder depth (+/-0.5) |
| **Breath** | RVLS / RVLSM | RVLL | the four voice levels (A / B / chord / sub) |

Each stream's targets are drawn from an **exponential distribution** (Poisson
process) with the SPDx / RW_LSP value as the mean, so the cadence "breathes"
instead of ticking at a fixed period. The tracking time constant (SMTx /
RW_LSM) is independent of the period, so the walk depth does not depend on
speed.

### Non-editable (fixed) behaviour

| Behaviour | Value | Notes |
|-----------|-------|-------|
| Attack / release | 2 s / 2 s | Exponential A-H-R, own envelope (platform EG open). |
| Note glide | 1 s | Linear interpolation; all derived voices follow. |
| Sub-1 detune | -4 c | One octave below the base, fixed. |
| Sub-2 pitch gate | base < ~120 Hz (f/4 < 30 Hz) | Gate closes; first-order smoothed. |
| Wavetable pitch morph | up to +10 tables at top of range | Higher notes drift toward the denser F bank. |
| Shape-LFO | +/-10 tables (morph), +/-5 c (sub FM) | On hardware: platform OSC-B shape-LFO; in the web sim: synthesized. |
| Output makeup | kOutDrive (~+8 dB) then tanh (kOutSat) | Saturates to 1.0; tune kOutDrive, not the mix budget. |

---

## NTS-1 mkII platform signal path

The DRONE unit is **only the OSC block** inside the NTS-1 mkII. It is a
**monophonic** source (it writes to `out[0]` only). The platform wires it into
the full synth chain as follows (this is the device's external signal path, not
part of the DRONE code):

```
OSC (mono)  ->  FILTER (low-pass / high-pass / band-pass, with Cutoff and Resonance)
            ->  MODULATION FX
            ->  DELAY FX
            ->  REVERB FX
            ->  Output
```

DRONE's own internal 12 dB/oct SVF (FCUT/RES) is **in addition to** (not a
replacement for) the platform's FILTER block: the unit's SVF shapes the main
A+B voice before the chord/subs are mixed in, while the platform filter and the
downstream MOD / DELAY / REVERB FX apply to the whole DRONE output.

---

## File layout

| File | Role |
|------|------|
| header.c | Unit header: name, the 6 parameter slots (4 multiplexer + Rnd + Legato). |
| unit.cc | Runtime entry points; maps the 4 multiplexer slots onto PARAM_SEL/PARAM_VAL (slots 2->0, 3->1). |
| osc.h | Thin Osc class skeleton: parameter enum, multiplexer state, pitch/glide/envelope/legato handling, note callbacks. |
| osc_params.h | OscParamsData -- the live DSP parameter struct (raw -> float) + reset() defaults. |
| osc_param_apply.h | apply_edit_param() -- the single raw-10-bit -> DSP-float converter (source of truth for ranges). |
| osc_param_display.h | getParameterStrValue() -- raw -> display string. |
| osc_mix.h | process() -- the per-sample audio path (see Sound path above). |
| osc_waves.h | Wavetable bank scanning + pitch-shifted morph + LFO-morph crossfade. |
| osc_saturator.h | Stateless wavefolder (fold) + asymmetric tanh saturator (saturate). |
| osc_svf.h | 12 dB/oct state-variable low-pass (cutoff/resonance/dry-wet + stability clamps). |
| osc_random_walk.h | RandomWalk -- multi-channel wandering (exponential target cadence). |
| osc_chord.h | ChordVoice -- the interval oscillator (CINT / CINT2 phasor). |
| osc_sub2.h | Sub2Voice -- f/4 saturated, LPF'd, pitch-gated sub. |
| osc_detune.h | Detune::factor() -- 2^(cents/1200) Taylor expansion (no libm). |
| osc_envelope.h | AhEnvelope -- single-voice A-H-R state machine. |
| osc_lfo_sim.h | Web-simulator-only shape-LFO synthesizer. |
| osc_constants.h | All static tuning constants (max depths, scales, glide/envelope lengths). |
| osc_randomize.h | randomizeAll() -- the Rnd re-roll (shared by device + web sim). |
| wasm.cc | Web-simulator glue (embind) for the sim/ build. |
| Makefile / config.mk | Build: ARM unit (make) and web simulator (make wasm). |

## Build

    # ARM unit (via Docker):
    docker run --rm -v $(pwd):/src -w /src korg/logue-sdk-sdk:latest make

    # Web simulator (two-stage C/C++ compile, linked with em++):
    make wasm

    # Serve the simulator:
    ./serve.sh        # or: emrun --serve 0.0.0.0:8000 sim/drone.html

> The web simulator's UI is generated from websim/osc.html (the build shell,
> referenced via SANDBOXDIR in the Makefile). sim/drone.html is the **output**
> of make wasm -- edit websim/osc.html, never sim/drone.html.

> **Keep this README in sync with the code.** See the project rule in the root
> .clinerules: any parameter add/remove/rename/re-range (or DSP-topology change)
> must be reflected in this file in the same change.
