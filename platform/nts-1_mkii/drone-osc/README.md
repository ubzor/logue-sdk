# DRONE

A dual wavetable drone generator for the **NTS-1 mkII** (Logue SDK).

The unit is a single-voice evolving drone: two detuned wavetable voices at the
base note, two sub-octave voices, an asymmetric saturator + wavefolder, an
internal 12 dB/oct SVF, and four independent random-walk "wandering" streams
that make the tone, timbre, density and voice levels breathe over time. There
is no per-sample branching on audio data and no dynamic allocation -- all state
lives inside the Osc class.

---

## Sound path

The drone is now a **polyphonic 6-voice CLUSTER** (Osc::process, osc_mix.h).
Every MIDI note gets its own lightweight voice in a cluster of `kVoiceCount = 6`;
LATCH is ALWAYS ON (notes are held; re-pressing the SAME note toggles it off; a
7th note steals the OLDEST voice). The
6 voices are **identically configured** (same wavetables, DETU, SATU/ASYM/FOLD,
random walks, SVF, formant, comb, duck); only their pitch (w0), random start
phasors, sub-voice states and A-H-R envelope differ. (The old GRIMM-style
exponential GLIDE/portamento knob was removed: each note now starts at its own
fixed random phase to de-correlate the inter-voice detune beats, which is what
keeps 2-6 held notes from summing into a low-frequency "howl".)

Per sample the path (1) runs each voice's
lightweight source (A/B phasors + wavetables + sub/sub-2 + ring-mod), summing
them into shared accumulators, then (2) applies the SINGLE common post-stage to
the sums (so 6 notes read as one blended drone, not 6 instruments):

```
PER VOICE (x kVoiceCount = 6):
  phasor A  ->  wavetable A (WA1)   [ DETU, FM1, FMAB, JIT, + walk detune ]
  phasor B  ->  wavetable B (WA2)   [ DETU, FM2, FMBB, JIT, + walk detune ]
  phasor sub->  sub-octave sine (SUB1)            [ f/2, LFO-FM ]
  phasor s2 ->  sub-2 voice (SUB2)                [ f/4, saturated/LPF/gated ]
  -> sum_main += (wavA*lvlA + wavB*lvlB)
  -> sum_sub  += sub
  -> sum_sub2 += sub2
  -> sum_ring += wavA*wavB
     (shared per-sample: the random walks, FM index, JIT are ONE drone)

PER-VOICE ENVELOPE + POLYPHONIC LOUDNESS (osc_mix.h, osc_constants.h):
  Each of the 6 voices is gated by its OWN A-H-R envelope (v.eg.env) in the
  accumulation loop: a new note (env 0->1 over 2 s) ADDS to the cluster
  instead of dragging the whole thing through a common MAX. (A common
  output envelope = max over all voices used to dip the entire cluster the
  instant a 2nd note started from env=0 -- the "silence on the 2nd note"
  bug. Per-voice gating removes that shared-dip.)
  env_sum  = sum over the 6 voices of its A-H-R envelope (0..kVoiceCount)
  env_comp = one-pole smoothed (tau ~5 ms, kClusterCompAlpha) of
             1 / sqrt(env_sum)   (floored at 1 -> a single note is never
             attenuated; N=1 -> 1 exactly, N=6 -> ~0.41)
  The SUMMED voice signals (sum_main / sum_sub / sum_sub2 / sum_ring) are
  each multiplied by env_comp BEFORE the shared post-stage, so N notes read
  at ~one note's loudness instead of N times louder (the kMix* budget is
  designed for one note; without this, a full cluster feeds fold/saturate at
  ~x4 and the fastertanh2f soft-clip pegs -> "shouting / clipping").

COMMON POST-STAGE (once, on the compensated, per-voice-enveloped sums):
  mix  = kMixOsc * (sum_main * env_comp)
  -> FLD (wavefolder, FLD)
  -> SAT/ASYM (asymmetric tanh saturator, SAT + ASYM + DWDR walk)   [ x kMixOsc ]
  -> SVF 12 dB/oct LPF (CUT/RES + WFC cutoff walk, dry/wet)
  -> FORMANT (2 parallel IIR-comb resonators F1+F2, FMVT/FLVL, parallel-add)
  -> COMB (IIR resonator, COMB feedback, fixed delay)
  = y_main

out = tanh( kOutSat * kOutDrive * ( y_main*MAIN*DUCK
            + (sum_sub*env_comp)*SUB1 + (sum_sub2*env_comp)*SUB2
            + (sum_ring*env_comp)*RMOD*MAIN*DUCK ) )
  where DUCK = constant sidechain (kDuckConstant = 0.5, floored at 0.7)
        and the per-voice envelopes are already folded into the sums
        (each sum_main/sub/sub2/ring term was multiplied by that voice's
        v.eg.env in the loop). There is NO common output envelope: N notes
        each fade in/out on their own, the cluster reads as one blended
        drone whose level is the sum of the individually-enveloped voices.

Mono-exactness: with 1 note, that voice's env gates its own contribution and
env_comp = 1/sqrt(1) = 1, so the single-note path is numerically identical to
the pre-cluster mono unit. Per-voice envelope + 1/sqrt(N) loudness are what
make the polyphonic cluster both level-independent of note count AND free of
the 1->2 note boundary dip.
```

The CPU cost scales ~linearly with the number of ACTIVE notes, not with
kVoiceCount: the heavy SVF/formant/comb/duck saturator run only ONCE on the
summed signal (shared post-stage); each active note adds only its lightweight
source (2 wavetable scans + sub/sub-2 + FM-smooth + envelope + accumulators).
Measured (web-sim, `Osc::process` only, no FX, median of 5 runs): the shared
floor (post-stage + 6 idle-voice envelope passes) is ~95 ns/sample, and each
ADDED active note costs ~77 ns/sample. So: 1 note ≈ 172 ns/smp, 4 notes ≈ 410
ns/smp, 6 notes ≈ 558 ns/smp. Against a single 48 kHz core's per-sample budget
(20 833 ns/smp) that is ~0.8% (1 note) / ~2.0% (4) / ~2.7% (6) — i.e. 4→6 notes
is a +36% CPU step on the OSC stage, comfortably within budget. (The hardware
Cortex-M7 @ 48 kHz runs this far faster per sample than the wasm browser build,
so the on-device fraction is lower still; these figures are the relative 1/4/6
comparison, which is what matters for the voice-count trade-off.)







SUB1 voice : sine at f/2 (one octave down, -4 c detune) -> * SUB1 * lvl_sub   <-- new
SUB2 voice : sine at f/4 -> soft tanh -> 200 Hz LPF -> pitch gate -> * SUB2   <-- new
RING voice : (wavA * wavB) * RMOD * MAIN   (A*B ring-mod, ducked by MAIN)

DUCK: CONSTANT sidechain — envelope follower of |SUB1 + SUB2| (one-pole
      abs-peak) ducks y_main + y_ring by a fixed kDuckConstant (0.5), floored
      at 0.7 (~-3 dB), ~75 ms tau: the low end is the constant anchor of the
      drone, the main tone "sinks" out of the way of the low end. Always on.

The A-H-R envelope (2 s / 2 s) multiplies the whole sum (note attack/release).
```

Key facts about the path:

* **Detune (DET)** is applied *per voice* **asymmetrically (GRIMM-style)**:
  A stays exactly at the note (factor 1.0), B is shifted UP by the full DET
  cents (B = f * 2^(DET/1200)). The beat is |f_B - f_A| ~ f * DET * ln2/1200
  per voice, and it is applied BEFORE the A/B sum so every note keeps its own
  beating pair. The sub voices stay on the base note (stable "anchors").
* **Phase-FM** (FM_A / FM_B) modulates the carrier *phasors* (classic Bessel
  sidebands). The modulator is the *other* voice's wavetable output, one sample
  delayed. The static index is **not** gated by RATE; RFM_A / RFM_B add a
  random-walk "breathing" on top (RATE-gated).
* **JIT** adds per-sample white-noise phase to each of A and B (smears the
  carrier into a dense partial; full knob = ~0.5% peak phase deviation).
* The **SUB2** voice bypasses the main soft-clip on purpose (a low-frequency
  sine through tanh would be heavily attenuated) and is gated closed below
  ~120 Hz base pitch (f/4 < 30 Hz).
* **No pitch glide (removed)**: a new note starts at its exact pitch with a
  random per-voice start phase (the GRIMM-style de-correlation that kills the
  polyphonic "howl"); there is no portamento between notes. (The old linear
  1 s glide, and later the GRIMM exponential `GLIDE` knob, were both removed.)
  (The chord-voice fade/crossfade notes were removed — the chord/interval
   concept is gone.)

### Rnd re-roll (BPM-synced smooth morph)

The Rnd re-rolls do **not** jump: when a re-roll fires (from the **Rnd**
header slot, or the web sim's **All / Part / Orbit** buttons) the affected
parameters **morph** from their old values to the new ones over a BPM-synced
phrase, each with its own random start delay (stagger) and a **smoothstep**
(`3t² − 2t³`) curve, so the parameters "roll in" one after another instead of
all at once.

* **Duration**: `kRndMorphBeats` = **1 bar of 4/4 = 4 beats** (constants in
  `osc_constants.h`). At the default 120 BPM that is **2 s**; the actual length
  is computed at fire time from the current tempo (`setTempo()`, default 120,
  clamped to 40–240 BPM), so at 60 BPM the same re-roll takes 4 s. A tempo
  change mid-morph does **not** re-aim an in-flight morph — it only affects the
  next fire.
* **Stagger**: each active parameter gets a random start delay of
  `0..(kRndMorphStaggerDivs − 1)` subdivisions of the phrase
  (`kRndMorphStaggerDivs` = 16), so a 1-bar re-roll "unrolls" across the whole
  window. **Anchor**: at least one parameter (the one with the largest raw
  delta, i.e. the most audible change) always starts immediately
  (`morph_start_ = 0`), so the user sees/hears a change on the very first
  audio block after the fire -- without this, the random stagger could put
  every active parameter's start delay at 0..2 s and the re-roll would feel
  "unresponsive". The anchor is only applied on a FRESH morph (not on a
  re-aim mid-morph), so an in-flight morph is never disrupted.
* **What morphs**: the parameters selected by the re-roll (All → all 36,
  Part → the 2..6 re-drawn ones, Orbit → the 2..4 nudged ones), each through the same
  `apply_edit_param()` path a manual PARAM_VAL edit uses, so the derived-voice
  caches (wavetables, SVF coefficients, formant combs) stay consistent
  with the moving raw values. Discrete parameters morph in their native space
  for free: a fractional wavetable index crossfades via `resolve_wave()`.
  // (The CINT/CINT2 morph exception + minimum-separation rules were removed —
  //  the chord/interval concept is gone.)
* **Re-fire mid-morph (C1-continuous re-aim)**: a new fire while a morph is in
  flight does NOT restart the morph. It re-aims **every** parameter (not just
  the currently-active ones), because a new fire (especially Orbit/Part) often
  re-rolls a *different* subset of parameters than the one in flight. For each
  parameter:
  - **already active AND re-targeted, safely mid-flight (0<t<1)**: the segment
    start (`morph_from_`) is **re-solved** so the smoothstep passes through the
    current value at the current progress AND reaches the new target at
    progress 1 — both **value** and **velocity** stay continuous (no click, no
    phase jump).
  - **already active but in its stagger (t~0) or already at/over its target
    (t~1)**: the C1 solve is degenerate there (division by `1-st`→∞ near
    `st=1`; a stagger-delayed param's `morph_cur_` is not a live point), so it
    does a **plain re-start** from its current value (`from = cur`, `start =
    now`, `t = 0`) — click-free, no division, no garbage read.
  - **NOT yet active but re-targeted by the new fire**: it is **added to the
    morph now** (`from = edit_value_` [still the old value — the caller
    overwrites `edit_value_` *after* `rndMorphStart`], `to = new target`,
    `cur = from`, `t = 0`, `start = now`). Without this, such a parameter's
    slider would jump instantly to the new target (and the DSP would snap)
    because `edit_value_` is committed immediately by the caller. Starting it
    now (no stagger on a re-aim — the user already clicked) makes it morph.
  - **unchanged** (`new_to == current value`): skipped — nothing to morph.
  The anchor is NOT applied on a re-aim (parameters already have their start
  delays; newly-added ones start immediately).
* **Where it runs**: audio thread, K-rate — `rndMorphTick()` is called once per
  `process()` block (in `osc_mix.h`), right after the re-roll `rndTick()`.
  When no morph is active it is a single branch check (zero cost).
* **Web sim UI**: `getEditParamValue()` (wasm.cc) returns the CURRENT
  interpolated value while a morph is in flight, so the sim's sliders travel
  with the morph in real time. The post-fire sync poll window in
  `websim/index.html` is `kRndMorphBeats` @ 120 BPM + max stagger + margin
  (≈ 5 s). The poll stops early when **both** (a) no value changed on this
  tick **and** (b) `getMorphActive()` (wasm.cc, exposed from
  `Osc::isMorphActive()`) reports the morph is done. The morph-active check is
  essential: the stagger window (up to `kRndMorphStaggerDivs-1` = 15
  subdivisions of the phrase, ~2 s at 120 BPM) can be longer than a single
  33 ms poll tick, so a "no value changed this frame" tick during the stagger
  would otherwise stop the poll prematurely and the sliders would never move
  even though the worklet's morph was still running.
* **Web sim buttons**: in the web simulator the main-thread and the
  audio-worklet processors are **separate copies** (wasm.cc), so a plain
  `rndRequest()` on the main-thread copy would never reach the worklet's
  `rndTick`. To bridge the gap, `Osc::simRndRequest(mode)` (a 32-bit packed
  slot: bit 31 = valid, bits 0..2 = mode, consumed in `process()` by
  `osc_mix.h`) carries the request from the UI thread to the worklet. The
  **All Random / Partial Random / Orbit Random** buttons all go through
  `fireRandom(mode)` (websim/index.html) → `Module.setRndMode(mode)` +
  `Module.rndTrigger()` → worklet `rndTick` → `randomizeAll/Partial/Orbi` →
  `rndMorphStart` — i.e. the SAME code path as the hardware, so all three
  morph identically. **Reset** is the only button that bypasses the worklet
  path: it is a "snap to factory defaults" action (no morph) and uses the
  UI-thread `applyPresetRaw(DEFAULT_RAW)` path (one `setEditParam` per 8 ms,
  followed by `Module.stableSync()` so the C++ Orbit reference matches the
  freshly-reset sound).

### Envelope / key behaviour (LATCH + polyphonic cluster)

Each cluster voice runs its own **A-H-R** envelope (the platform EG is set to
*open*): 2 s exponential attack, hold at full level while the note is active,
2 s exponential release. **LATCH is ALWAYS ON** (there is no Legato switch):
key release does **not** stop the note — it is held. Behaviour:

* **New note** -> starts its own voice in the cluster (up to 6 at once), with a
  fresh 2 s A-H-R attack at its exact pitch (random per-voice start phase, no
  glide); the very first note behaves the same.
* **Same note re-pressed** -> TOGGLE-OFF that one voice (2 s release, fades out
  while the others keep sounding).
* **> 6 notes held** -> the OLDEST active voice is **stolen** (released) and the
  new note takes it, so the cluster never exceeds `kVoiceCount`.
* The whole cluster is stopped only by All-Notes-Off (instant, no release tail).

`setPitch()` is now a **no-op** in this build (the cluster's pitch is driven
entirely by `noteOn`); it is kept for API compatibility with `unit.cc` /
`wasm.cc`.

The randomization is a **two-slot multiplexer** (edit-menu, device-only):

* **Slot 4 RndM** -- the randomization **TYPE** (a persistent setting):
  * `All`  -- re-roll **every** edit parameter to a fresh audition-friendly
    random preset (the original Rnd behaviour). The result becomes the new
    **stable state** (the ORBI reference).
  * `Part` -- re-roll a **random subset (2..6)** of parameters to fresh values
    from the same "safe windows", leaving the rest untouched -- a *development
    around the current sound*, not a reset. Each changed parameter is
    guaranteed a **significant step** (new raw >= `kRndMinStepRaw` = 102, ~10%
    of the 10-bit range, from the current raw; re-drawn up to 5 times) so the
    change is actually audible -- no "FOLD 53 -> 56" no-ops. The result becomes
    the new **stable state**.
  * `ORBI` -- a **tight, short "development"** of the last **stable state**
    (`Osc::stable_value_`, `osc_randomize.h`), deliberately gentler than Part so
    the result is recognizably the *same* sound, developed. Per fire it:
    * nudges **2..4** parameters (vs Part's 2..6);
    * picks each from **ONE weighted family** -- *core* (harmonic core: detune,
      subs, A/B wavetables), *timbre* (color: saturation,
      fold, filter, ring-mod, comb), *motion* (movement: RATE, the wander
      streams, walk depths, breath, FM) or *extra* (enrichment layers, default
      OFF -- a low-weight "surprise") -- so a single fire develops one "sphere"
      of the sound rather than reaching across all 36 parameters (a small
      ~4% "surprise" chance still lets a fire touch any parameter);
    * moves each by a step: at least `kRndOrbiMinStepRaw` (64, ~6%, still
      audible) but at most `kRndOrbiMaxStepRaw` (307, ~30%) of the 10-bit range
      -- a clear nudge, **not** a fresh re-draw of up to half the range;
    * morphs the whole fire over a **short phrase** (`kRndOrbiBeats`/
      `kRndOrbiBeatDiv` = a quarter bar, ~0.5 s at 120 BPM), not Part's full bar
      -- so the shift lands quickly and is clearly audible.
    The nudge is anchored to the **stable** values (not the current sound) and
    is applied to the live DSP **without** creating a new stable state. Factory
    defaults are the stable state at startup; every All/Part re-roll rewrites
    it. Firing ORBI again always develops from the SAME stable sound.
* **Slot 5 Rnd** -- the **trigger** (0..15, enum): like the old Rnd, changing it
  to any value 1..15 fires the re-roll **per the current RndM type**.

**The hardware Rnd trigger (slot 5) is debounced (0.5 s).** Turning the knob in
the edit menu sends a stream of value changes (one per detent); left raw, each
would fire a re-roll, audible as a continuous jump. The debounce (hardware-only,
running on the audio clock in `process()`) turns that into:
* the **first** change re-rolls **immediately** (next `process()` block, <3 ms);
* every further change within the **0.5 s** window only **resets** the timer;
* after **0.5 s of silence** following a change inside the window, **one final**
  re-roll fires (so the last-set value is reflected), then silence.

Re-rolls therefore happen at most once per 0.5 s. The window is
`Osc::kRndDebounceSamples = 24000` samples (0.5 s at the fixed 48 kHz rate).

**The web simulator** has a *Random* block mirroring this: a **Type** selector
(All/Part/ORBI, `Module.setRndMode`), a **Trigger** slider (0..15,
`Module.setRndRun`) and a **Fire** button (`Module.rndTrigger()`), which
re-rolls on the audio worklet thread per the selected type (NOT debounced -- a
single click fires immediately). The preset row has an **Orbit Random** button
(`Module.setRndMode(2)` + `Module.rndTrigger()`, audio-worklet path like the
device) next to **All Random** / **Partial Random** / **Reset** (UI-thread
conveniences via `applyPresetRaw`, which call `Module.stableSync()` after the
apply so the C++ Orbit reference matches what the sim is showing).

**Manual slider edits update the Orbit stable state** (web simulator only):
each slider's `apply()` (the `oninput` handler in `createEditableParam`,
`websim/index.html`) calls `Module.stableSync()` after writing the value -- a
manual edit is by definition the user's new "stable" sound, so the next
Orbit Random must develop from the just-set values, not from the previous
All/Part/Reset snapshot. The call is guarded by `getMorphActive() === 0`
(benignly racing read of the worklet's `morph_active_`, same as the
poll's dirty-check): while an All/Part/Orbit morph is in flight -- including
the `fireRandom` slider-sync poll, which itself routes through the same
`apply()` to write the interpolated morph values -- a sync would drag
`stable_value_` along the glide and clobber the C++-side stabilize
(`randomizeAll`/`randomizePartial` rewrite it at the end of their re-roll).
On hardware, manual PARAM/VALUE edits do NOT touch the stable state (the
Orbit reference only moves on All/Part re-rolls or reset), keeping the
device's "develop from the last confirmed preset" behaviour intact.

---

## Parameters

The unit exposes its **44** DSP knobs through a PARAM/VALUE multiplexer on the
two physical knobs:

* **Slot 0 PARAM** (fixed knob A) -- parameter selector, 0..43; the display
  shows the name of the selected parameter.
* **Slot 1 VALUE** (fixed knob B) -- raw 10-bit value (0..1023) of the selected
  parameter. Each parameter maps 0..1023 onto its own range; the raw value is
  cached per parameter, so re-selecting one restores its last value.
* **Slots 2/3 PARAM/VALUE** -- full duplicates of 0/1 (edit-menu visibility).
* **Slot 4 RndM** (edit-menu) -- randomization TYPE (All / Part / ORBI), see above.
* **Slot 5 Rnd** (edit-menu) -- randomization TRIGGER (0..15, debounced), see above.

All 36 editable DSP parameters (select_ index -> name), with range, factory
default and effect:

| # | Name | Display | Range | Default | Effect |
|---|------|---------|-------|---------|--------|
| 0 | RATE | % | 0..100% (quadratic) | 0% (off) | Shared ceiling for every random-walk stream. 0 = all wandering off. (First in the multiplexer -- the most-edited chaos lever.) |
| 1 | DET | +/-c | -10..+10 c | 0 c | Static ASYMMETRIC detune (GRIMM-style): A stays at f, B = f * 2^(DET/1200) (shifted UP). |
| 2 | SPD1 | % | 0..100% -> 0.1 s..10 s (log) | 50% | **Tone** stream period (mean target-update gap). |
| 3 | SM1 | % | 0..100% -> 10 ms..1.5 s (log) | 50% | **Tone** stream time constant. |
| 4 | SPD2 | % | 0..100% -> 0.1 s..10 s (log) | 50% | **Timbre** stream period. |
| 5 | SM2 | % | 0..100% -> 10 ms..1.5 s (log) | 50% | **Timbre** stream time constant. |
| 6 | SPD3 | % | 0..100% -> 0.1 s..10 s (log) | 50% | **Density** stream period. |
| 7 | SM3 | % | 0..100% -> 10 ms..1.5 s (log) | 50% | **Density** stream time constant. |
| 8 | SUB1 | % | 0..100% | 50% | Sub-octave (f/2) sine level. |
| 9 | SUB2 | % | 0..100% | ~40% | Second sub (f/4) saturated level (0 = off). The bass is the drone's anchor, so the randomizer keeps it always audible (window 25..70%). |
| 10 | SAT | 0..300 | 0..3.0 drive | 100 (1.0x) | Main A+B saturator drive. |
| 11 | ASYM | +/-% | -100..+100% | 0% | Saturator asymmetry (even harmonics; >0 = positive half harder). |
| 12 | FLD | % | 0..100% | 0% (off) | Pre-drive wavefolder depth (metallic edge). |
| 13 | WA1 | index | 0..90 (banks A-F) | 0 | Osc A wavetable. |
| 14 | WA2 | index | 0..90 (banks A-F) | ~11 | Osc B wavetable. |
| 15 | MAIN | % | 0..100% | 100% | Main (A+B) voice level (also ducks the ring-mod voice). |
| 16 | TWDR | % | 0..100% | 0% (off) | Walk depth on wavetable morph (+/-10 tables). |
| 17 | DWDR | % | 0..100% | 0% (off) | Walk depth on saturator drive (+/-0.5). |
| 18 | FWDR | % | 0..100% | 0% (off) | Walk depth on wavefolder depth (+/-0.5). |
| 19 | CUT | Hz / kHz | 80 Hz..12 kHz (log) | ~9 kHz | SVF cutoff **and** dry/wet blend (0% = raw, 100% = full LPF). |
| 20 | RES | X.Y | 0.5..2.5 Q | 0.5 | SVF resonance. |
| 21 | WFC | % | 0..100% | 0% (off) | Walk depth on SVF cutoff (+/-1 half-octave). |
| 22 | BRT | % | 0..100% | 0% (off) | **Breath** stream depth on the voice levels. |
| 23 | BRR | % | 0..100% -> 0.1 s..10 s (log) | 50% | **Breath** stream period. |
| 24 | SMT | % | 0..100% -> 10 ms..1.5 s (log) | 50% | **Breath** stream time constant. |
| 25 | JIT | % | 0..100% -> 0..0.5% phase | 0% (off) | Random phase jitter on A/B (smears the carrier). |
| 26 | RMOD | % | 0..100% | 0% (off) | A*B ring-mod parallel voice (ducked by MAIN). |
| 27 | FM1 | Nc | 0..100% (quadratic) -> 0..0.05 c | 0% (off) | Static phase-FM of osc A by B (not RATE-gated). |
| 28 | FM2 | Nc | 0..100% (quadratic) -> 0..0.05 c | 0% (off) | Static phase-FM of osc B by A (not RATE-gated). |
| 29 | FMAB | % | 0..100% | 0% (off) | Walk depth on the FM-A index ("breathing" FM, RATE-gated). |
| 30 | FMBB | % | 0..100% | 0% (off) | Walk depth on the FM-B index ("breathing" FM, RATE-gated). |
| 31 | FMVT | A/E/I/O/U | 0..100% -> 5 vowels (A-E-I-O-U morph) | 50% (mid) | Formant vowel position: morphs BOTH comb delays (the vowel's F1+F2 pair). The stage is OFF until FLVL>0, so this dials the vowel silently. |
| 32 | FLVL | % | 0..100% | 0% (off) | Formant stage level: shared feedback (0..0.9) + parallel-add gain of the two comb resonators (F1 peak 135..365 Hz, F2 peak 435..1145 Hz, gain 1.0). 0 = transparent. |
| 33 | COMB | % | 0..100% -> 0..0.9 fb (fixed ~190 Hz delay) | 0% (off) | Comb resonator: IIR feedback peak + harmonics (wooden/cavernous tint). |

(FOLD2 — second cascaded wavefolder — and DCK — sidechain-duck amount — were
removed. DCK is now a CONSTANT: see "Non-editable (fixed) behaviour" /
"DUCK" above.)

### The four random-walk streams

RATE is the single ceiling that enables all wandering (0 = everything static).
Each stream is independent (own period SPDx, own time constant SMx) and its
targets are **not** phase-locked:

| Stream | Period / Smooth | Target knob(s) | Drives |
|--------|-----------------|----------------|--------|
| **Tone** | SPD1 / SM1 | (depth from RATE) | A/B detune (+/-10 c) + FM index breathing |
| **Timbre** | SPD2 / SM2 | TWDR, WFC | wavetable morph (+/-10 tables) + SVF cutoff (+/-1 half-oct) |
| **Density** | SPD3 / SM3 | DWDR, FWDR | saturator drive (+/-0.5) + wavefolder depth (+/-0.5) |
| **Breath** | BRR / SMT | BRT | the voice levels (A / B / sub) |

Each stream's targets are drawn from an **exponential distribution** (Poisson
process) with the SPDx value as the mean, so the cadence "breathes" instead of
ticking at a fixed period. The tracking time constant (SMx / SMT) is
independent of the period, so the walk depth does not depend on speed.

### Non-editable (fixed) behaviour

| Behaviour | Value | Notes |
|-----------|-------|-------|
| Attack / release | 2 s / 2 s | Exponential A-H-R, own envelope (platform EG open). |
| Sub-1 detune | -4 c | One octave below the base, fixed. |
| Sub-2 pitch gate | base < ~120 Hz (f/4 < 30 Hz) | Gate closes; first-order smoothed. |
| Wavetable pitch morph | up to +10 tables at top of range | Higher notes drift toward the denser F bank. |
| Shape-LFO | +/-10 tables (morph), +/-5 c (sub FM) | On hardware: platform OSC-B shape-LFO; in the web sim: synthesized. |
| Output makeup | kOutDrive (~+8 dB) then tanh (kOutSat) | Saturates to 1.0; tune kOutDrive, not the mix budget. |
| Formant stops | A 730/1090, E 530/1840, I 270/2290, O 450/1020, U 300/870 Hz | F1/F2 per vowel; two one-pole IIR combs (delay = sr/(2*f) -> peaks at 135..365 / 435..1145 Hz), shared feedback by FLVL, parallel-add gain 1.0; morphed by FMVT. |
| Comb delay | 250 smp (~190 Hz peak) | Fixed IIR-comb delay length; feedback (0..0.9) by the COMB knob. |
| Duck (sidechain) | constant 0.5, floor 0.7, 75 ms tau | One-pole abs-peak follower of \|SUB1+SUB2\|; constantly ducks the MAIN voice (A+B + RMOD) by kDuckConstant (0.5), floored at 0.7. The low end is the drone's anchor; the main tone sinks. (Formerly the DCK knob — now a constant.) |

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

DRONE's own internal 12 dB/oct SVF (CUT/RES) is **in addition to** (not a
replacement for) the platform's FILTER block: the unit's SVF shapes the main
A+B voice before the subs are mixed in, while the platform filter and the
downstream MOD / DELAY / REVERB FX apply to the whole DRONE output.

---

## File layout

| File | Role |
|------|------|
| header.c | Unit header: name, the 6 parameter slots (4 multiplexer + RndM type + Rnd trigger). |
| unit.cc | Runtime entry points; maps the 4 multiplexer slots onto PARAM_SEL/PARAM_VAL (slots 2->0, 3->1); slot 4 -> setRndMode, slot 5 -> rndArm (trigger). |
| osc.h | Thin Osc class skeleton: parameter enum, multiplexer state, note callbacks, and the K-rate caches; method bodies live in the out-of-line headers below. |
| osc_params.h | OscParamsData -- the live DSP parameter struct (raw -> float) + reset() defaults. |
| osc_param_apply.h | apply_edit_param() -- the single raw-10-bit -> DSP-float converter (source of truth for ranges). |
| osc_param_display.h | getParameterStrValue() -- raw -> display string. |
| osc_rnd.h | Rnd re-roll + morph engine: rndArm/rndTick (0.5 s debounce), rndMorphStart/rndMorphTick (BPM-synced morph), RndM mode state, web-sim request channels. |
| osc_lifecycle.h | init/reset/noteOn/noteOff/allNoteOff/setPitch -- unit lifecycle; polyphonic cluster voice allocation (steal oldest), LATCH toggle-off; setPitch is a no-op. |
| osc_mix.h | process() -- the per-sample audio path; blockPrepass() -- the K-rate per-block prepass (edit/Rnd request consumption, rndTick, rndMorphTick). |
| osc_waves.h | Wavetable bank scanning + pitch-shifted morph + LFO-morph crossfade. |
| osc_saturator.h | Stateless wavefolder (fold) + asymmetric tanh saturator (saturate). |
| osc_svf.h | 12 dB/oct state-variable low-pass (cutoff/resonance/dry-wet + stability clamps). |
| osc_random_walk.h | RandomWalk -- multi-channel wandering (exponential target cadence). |
| osc_sub2.h | Sub2Voice -- f/4 saturated, LPF'd, pitch-gated sub. |
| osc_detune.h | Detune::factor() -- 2^(cents/1200) Taylor expansion (no libm). |
| osc_envelope.h | AhEnvelope -- single-voice A-H-R state machine. |
| osc_lfo_sim.h | Web-simulator-only shape-LFO synthesizer. |
| osc_constants.h | All static tuning constants (max depths, scales, envelope lengths). |
| osc_randomize.h | randomizeAll() / randomizePartial() + the shared safe-window list -- the Rnd re-roll (All / Part), fired by the Rnd trigger debounce (device) or Module.rndTrigger (sim). |
| wasm.cc | Web-simulator glue (embind); now only consumed by the unified drone-chain simulator. |
| Makefile / config.mk | Build: the ARM unit (`make` / `make install`). |

## Build

    # ARM unit (via Docker):
    docker run --rm -v $(pwd):/src -w /src korg/logue-sdk-sdk:latest make

    # Web simulator (the unified chain page, OSC -> MOD -> DEL -> REV in one
    # wasm worklet -- there is no standalone OSC page anymore):
    cd platform/nts-1_mkii/drone-chain && make wasm

    # Serve the simulator:
    ./serve.sh        # http://localhost:8000/ (drone-chain/sim/index.html)

> The web simulator's UI is generated from websim/index.html (the build shell
> of drone-chain, referenced via SANDBOXDIR in drone-chain/Makefile).
> drone-chain/sim/index.html is the **output** of `make wasm` -- edit
> websim/index.html, never the generated page.

> **Keep this README in sync with the code.** See the project rule in the root
> .clinerules: any parameter add/remove/rename/re-range (or DSP-topology change)
> must be reflected in this file in the same change.
