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
out = tanh( kOutSat * kOutDrive * ( y_main*MAIN*DUCK + y_chord + y_sub1 + y_sub2 + y_ring*DUCK ) )
  where DUCK = constant sidechain (kDuckConstant = 0.5, floored at 0.7)

MAIN voice (osc A + osc B)
  phasor A  ->  wavetable A (WA1)   [ DET, FM1, FMAB, JIT, + walk detune ]
  phasor B  ->  wavetable B (WA2)   [ DET, FM2, FMBB, JIT, + walk detune ]
  y = (wavA + wavB) / 2
    -> FLD (wavefolder, FLD)
    -> SAT/ASYM (asymmetric tanh saturator, SAT + ASYM + DWDR walk)
    -> SVF 12 dB/oct LPF (CUT/RES + WFC cutoff walk, dry/wet)
    -> FORMANT (2 parallel IIR-comb resonators F1+F2, FMVT/FLVL, parallel-add)  <-- new
    -> COMB (IIR resonator, COMB feedback, fixed delay)             <-- new
    = y_main

CHORD voice (osc C at INT, osc C2 at INT2)
  phasor C  ->  wavetable C (WA3)  at base * 2^(INT/1200)
  phasor C2 ->  wavetable C (WA3)  at base * 2^(INT2/1200)
  [pitch crossfade: when INT/INT2 changes, old + new phasors are AM-blended
   over 2 s (same one-pole as the level fade) before the folder/saturator]
  each: -> FLD (CFLD / CH2F) -> SAT (CH2D / CH2D, shared ASYM)
  y_chord = (c1 + c2) * CHRD * lvl_c * fade    (lvl_c = Breath stream,
                                               fade = 2 s one-pole on INT / INT2)

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

* **Detune (DET)** is applied *per voice* (A gets +c/2 cents, B gets -c/2),
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
 * **Chord interval fade**: switching INT / INT2 on (> 0 c) or off (0 c,
   unison) does not toggle the chord level instantly -- each interval's level
   is relaxed toward (active ? 1 : 0) with a **2 s one-pole exponential**
   (`chord1_fade_` / `chord2_fade_`, the same curve as the note attack/release
   envelope), so an interval **fades in over 2 s when turned on** and **fades
   out over 2 s when turned off** (no click, no instant "start screaming").
   The chord phasor **always advances** (even at cents == 0), so the signal
   shape is continuous (no "sine -> constant" jump) when the interval crosses
   0; only the LEVEL fades. Re-targeting mid-fade is continuous, so scrubbing
   the knob through 0 is click-free. The whole chord is still gated by the
   note A-H-R envelope, so a note-off kills it instantly.
 * **Chord pitch crossfade**: when the INT or INT2 cents value **changes**
   the phasor's frequency ratio (w0) jumps and would produce an audible click.
   To smooth the transition, TWO wavetable lookups are run per chord voice —
   the "old" phasor (running at the previous offset) and the "new" phasor
   (reset to the fresh offset) — and their outputs are **AM-blended** with a
   one-pole coefficient that relaxes from 0 (all old) to 1 (all new) over
   **2 s** (same `kChordCrossSamples` / `kChordFadeSamples` curve as the on/off
   level fade). The blend happens in the wavetable-output domain, **before**
   the folder/saturator, so the saturation is shared. When the cents value is
   stable the old phasor is synced to the new one and only one lookup runs
   (regression-safe, zero extra cost in steady state). The crossfade is
   triggered by `apply_edit_param(CINT/CINT2)` (osc_param_apply.h) resetting
   `chord_new_` / `chord2_new_` and starting `chord1_cross_` / `chord2_cross_`
   from 0, and records the pre-change cents in `chord_old_cents_` /
   `chord2_old_cents_` so the "old" phasors keep their pre-change frequency for
   the whole fade (a base-pitch change mid-fade does not destroy the crossfade;
   on completion the old cents sync back to `params_`). Because CINT / CINT2 are
   excluded from the Rnd morph (see the *Rnd re-roll* section), a re-roll
   applies the new interval **immediately** and the crossfade is the ONLY
   smoothing — a clean 2 s voice swap, not a cents glide.

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
* **What morphs**: the parameters selected by the re-roll (All → all 44,
  Part → the 2..6 re-drawn ones, Orbit → the 2..4 nudged ones), each through the same
  `apply_edit_param()` path a manual PARAM_VAL edit uses, so the derived-voice
  caches (wavetables, chords, SVF coefficients, formant combs) stay consistent
  with the moving raw values. Discrete parameters morph in their native space
  for free: a fractional wavetable index crossfades via `resolve_wave()`.
  **Exception — CINT / CINT2 do NOT morph**: the two chord intervals are
  excluded from the morph engine (`rndMorphStart` skips idx 17 / 30) and are
  applied **immediately** inside `rndMorphStart` instead (an
  `apply_edit_param()` call for each, on a real change), so the chord
  **pitch-crossfades** (old interval fades out, new interval fades in over
  2 s — see the *Chord pitch crossfade* note in the Sound-path section) rather
  than the interval gliding in cents. This is audible as a clean "voice swap"
  instead of a slide. (The sim's CINT / CINT2 sliders therefore snap to the
  new value on a re-roll — the DSP, not the slider, carries the 2 s crossfade.)
   **CINT / CINT2 minimum separation**: when a re-roll draws the two chord
   intervals, `rnd_separate_cint()` (osc_randomize.h) guarantees that if BOTH are
   non-zero they end up at least `kRndCintMinSepCents` (175 c) apart — closer
   they sound like a single beating note rather than two distinct voices. If the
   fresh draw is too close, the second interval is re-drawn (up to 3×) and, if
   still too close, snapped to the nearest consonant-set element far enough away.
   A 0 (unison) on either interval is always allowed (it silences that voice).
   Enforced in all three re-roll paths (All / Part / Orbit) and mirrored in the
   web simulator's `randomizeAllRaw()` / `partialRandomRaw()`.
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
    now (no stagger on a re-aim — the user already clicked) makes it glide.
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

### Envelope / key behaviour

The unit runs its own **A-H-R** envelope (the platform EG is set to *open*):
2 s exponential attack, hold at full level while the note is active, 2 s
exponential release. **Legato is always on** (there is no Legato switch):
key release does **not** stop the sound. Re-pressing the *same* key fades it
out (toggle-off); pressing a *different* key retargets the pitch (1 s glide,
amplitude stays continuous) and keeps playing. The drone is stopped by the
toggle-off re-press or by All-Notes-Off.

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
      subs, A/B/C wavetables, chord, intervals), *timbre* (color: saturation,
      fold, filter, ring-mod, comb), *motion* (movement: RATE, the wander
      streams, walk depths, breath, FM) or *extra* (enrichment layers, default
      OFF -- a low-weight "surprise") -- so a single fire develops one "sphere"
      of the sound rather than reaching across all 44 parameters (a small
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

All 44 editable DSP parameters (select_ index -> name), with range, factory
default and effect:

| # | Name | Display | Range | Default | Effect |
|---|------|---------|-------|---------|--------|
| 0 | RATE | % | 0..100% (quadratic) | 0% (off) | Shared ceiling for every random-walk stream. 0 = all wandering off. (First in the multiplexer -- the most-edited "chaos" lever.) |
| 1 | DET | +/-c | -10..+10 c | 0 c | Static symmetric detune: A = +c/2, B = -c/2. |
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
| 15 | WA3 | index | 0..90 (banks A-F) | 0 | Chord-voice (osc C/C2) wavetable. |
| 16 | CHRD | % | 0..100% | ~20% | Chord-voice level (both intervals INT + INT2). |
| 17 | INT | NNNc | 0..1900 c (continuous) | ~700 c (fifth) | First chord interval above the base note. 0 = off (unison); on/off fades over 2 s. |
| 18 | MAIN | % | 0..100% | 100% | Main (A+B) voice level (also ducks the ring-mod voice). |
| 19 | TWDR | % | 0..100% | 0% (off) | Walk depth on wavetable morph (+/-10 tables). |
| 20 | DWDR | % | 0..100% | 0% (off) | Walk depth on saturator drive (+/-0.5). |
| 21 | FWDR | % | 0..100% | 0% (off) | Walk depth on wavefolder depth (+/-0.5). |
| 22 | CUT | Hz / kHz | 80 Hz..12 kHz (log) | ~9 kHz | SVF cutoff **and** dry/wet blend (0% = raw, 100% = full LPF). |
| 23 | RES | X.Y | 0.5..2.5 Q | 0.5 | SVF resonance. |
| 24 | WFC | % | 0..100% | 0% (off) | Walk depth on SVF cutoff (+/-1 half-octave). |
| 25 | BRT | % | 0..100% | 0% (off) | **Breath** stream depth on the four voice levels. |
| 26 | BRR | % | 0..100% -> 0.1 s..10 s (log) | 50% | **Breath** stream period. |
| 27 | SMT | % | 0..100% -> 10 ms..1.5 s (log) | 50% | **Breath** stream time constant. |
| 28 | CH2D | 0..300 | 0..3.0 drive | = SAT | INT interval's own saturator drive (shared ASYM). |
| 29 | CFLD | % | 0..100% | = FLD (0%) | INT interval's own wavefolder depth. |
| 30 | INT2 | NNNc | 0..1900 c (continuous) | ~950 c (major sixth) | Second chord interval (shares CHRD level, WA3, Breath). 0 = off (unison); on/off fades over 2 s. |
| 31 | JIT | % | 0..100% -> 0..0.5% phase | 0% (off) | Random phase jitter on A/B (smears the carrier). |
| 32 | RMOD | % | 0..100% | 0% (off) | A*B ring-mod parallel voice (ducked by MAIN). |
| 33 | FM1 | Nc | 0..100% (quadratic) -> ~0.5% phase | 0% (off) | Static phase-FM of osc A by B (not RATE-gated). |
| 34 | FM2 | Nc | 0..100% (quadratic) -> ~0.5% phase | 0% (off) | Static phase-FM of osc B by A (not RATE-gated). |
| 35 | FMAB | % | 0..100% | 0% (off) | Walk depth on the FM-A index ("breathing" FM, RATE-gated). |
| 36 | FMBB | % | 0..100% | 0% (off) | Walk depth on the FM-B index ("breathing" FM, RATE-gated). |
| 37 | CH2D | 0..300 | 0..3.0 drive | = CH2D(28) | INT2 interval's own saturator drive. |
| 38 | CH2F | % | 0..100% | = CFLD (0%) | INT2 interval's own wavefolder depth. |
| 39 | FMVT | A/E/I/O/U | 0..100% -> 5 vowels (A-E-I-O-U morph) | 50% (mid) | Formant vowel position: morphs BOTH comb delays (the vowel's F1+F2 pair). The stage is OFF until FLVL>0, so this dials the vowel silently. |
| 40 | FLVL | % | 0..100% | 0% (off) | Formant stage level: shared feedback (0..0.9) + parallel-add gain of the two comb resonators (F1 peak 135..365 Hz, F2 peak 435..1145 Hz, gain 1.0). 0 = transparent. |
| 41 | COMB | % | 0..100% -> 0..0.9 fb (fixed ~190 Hz delay) | 0% (off) | Comb resonator: IIR feedback peak + harmonics (wooden/cavernous tint). |

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
| **Breath** | BRR / SMT | BRT | the four voice levels (A / B / chord / sub) |

Each stream's targets are drawn from an **exponential distribution** (Poisson
process) with the SPDx value as the mean, so the cadence "breathes" instead of
ticking at a fixed period. The tracking time constant (SMx / SMT) is
independent of the period, so the walk depth does not depend on speed.

### Non-editable (fixed) behaviour

| Behaviour | Value | Notes |
|-----------|-------|-------|
| Attack / release | 2 s / 2 s | Exponential A-H-R, own envelope (platform EG open). |
| Chord interval fade | 2 s / 2 s | INT / INT2 on-off: per-interval one-pole level fade (same curve as the note envelope); 0 c = off. |
| Note glide | 1 s | Linear interpolation; all derived voices follow. |
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
A+B voice before the chord/subs are mixed in, while the platform filter and the
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
| osc_lifecycle.h | init/reset/noteOn/noteOff/allNoteOff/setPitch -- unit lifecycle, legato note handling, 2 s pitch glide. |
| osc_mix.h | process() -- the per-sample audio path; blockPrepass() -- the K-rate per-block prepass (edit/Rnd request consumption, rndTick, rndMorphTick, glide). |
| osc_waves.h | Wavetable bank scanning + pitch-shifted morph + LFO-morph crossfade. |
| osc_saturator.h | Stateless wavefolder (fold) + asymmetric tanh saturator (saturate). |
| osc_svf.h | 12 dB/oct state-variable low-pass (cutoff/resonance/dry-wet + stability clamps). |
| osc_random_walk.h | RandomWalk -- multi-channel wandering (exponential target cadence). |
| osc_chord.h | ChordVoice -- the interval oscillator (INT / INT2 phasor). |
| osc_sub2.h | Sub2Voice -- f/4 saturated, LPF'd, pitch-gated sub. |
| osc_detune.h | Detune::factor() -- 2^(cents/1200) Taylor expansion (no libm). |
| osc_envelope.h | AhEnvelope -- single-voice A-H-R state machine. |
| osc_lfo_sim.h | Web-simulator-only shape-LFO synthesizer. |
| osc_constants.h | All static tuning constants (max depths, scales, glide/envelope lengths). |
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
