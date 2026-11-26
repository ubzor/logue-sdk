# DRONE

A dual wavetable drone generator for the **NTS-1 mkII** (Logue SDK).

The unit is a single-voice evolving drone: two detuned wavetable voices at the
base note (the wavetable carries the low end as subharmonic partials), an
asymmetric saturator + wavefolder, an internal 12 dB/oct SVF, and four
independent random-walk "wandering" streams that make the tone, timbre,
density and voice levels breathe over time. There is no per-sample branching on
audio data and no dynamic allocation -- all state lives inside the Osc class.

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
lightweight source (A/B phasors + wavetables + ring-mod), summing them into
shared accumulators, then (2) computes the ONE shared sub bass (the cluster's
env-weighted ANCHOR note — see below), then (3) applies the SINGLE common
post-stage (so 6 notes read as one blended drone, not 6 instruments):

```
WTE WAVETABLE MORPH (ONE internal wave-LFO; see osc_lfo_sim.h + osc_wte.h):
  The A+B voices read the 16x256 EDITOR wavetable (wte_.frames_ — the 16
  morph frames rendered from the /editor document, or the 16-wave default on
  hardware) as their SOLE wavetable source. The morph position across the 16
  frames is driven by the ONE internal wave-LFO 1 (lfo_sim_, WLFR/WLFD/WLFS/
  WLFG):
    lfo_      = lfo_sim_.process()  (SIN/S/WALK shape * WLFD; phase advances by WLFR Hz)
    wte_morph = 7.5 + lfo_ * 7.5    (CENTERED on the table middle, full 0..15 sweep at 100%)
  The old TWO-stream 2D morph (stream 1 = WAVE bank, stream 2 = WAV2 bank,
  crossfaded by WMIX + the wave-mix LFO) is GONE with the stock banks:
  WAV1/WAV2/WMIX and the stream-2 / wave-mix LFO params (W2LR/W2LD, W2FS/
  W2FG, WMXR/WMXD/WMXS/WMXG) are NO-OPS now (lfo_sim2_ / lfo_mix_ no longer
  process in the audio path).
  NOTE: the platform shape-LFO (the hardware's OSC-button RATE/SHAPE knobs)
        is a NO-OP (setShapeLfo() is a stub); the WTE-table morph is driven
        ENTIRELY by the ONE internal wave-LFO 1 (WLFR / WLFD / WLFS / WLFG).

PER VOICE (x kVoiceCount = 6):
  phasor A  ->  wavA = WteTable::lookup(wte_.frames_, wte_morph, ph_a)
  phasor B  ->  wavB = WteTable::lookup(wte_.frames_, wte_morph, ph_b)
                   (the 16x256 editor wavetable, bilinear crossfade of the two
                    adjacent frames @ wte_morph and the two adjacent phase points)
               [ DETU, FM1, FMAB, JIT, S(static teeth, shape=FM Mod Wave), FM C(vibrato) (both scaled by FMAM), + walk detune ]
  -> sum_main += (wavA*lvlA + wavB*lvlB)   (lvl = per-voice breath level)
  -> sum_ring += wavA*wavB
      (shared per-sample: the random walks, FM index, JIT are ONE drone; the
       A/B wavetables are the SHARED WTE table @ the SHARED morph (wte_morph);
       the low end is carried by the WTE table's subharmonic partials — there
       is NO separate sub-bass branch any more, see the "EDITOR WAVETABLE"
       block below)

EDITOR WAVETABLE (osc_wte.h, osc_wte_doc.h, osc_wte_render.h; the A+B voices' SOLE wavetable source):
  A 16 morph-frame × 256-point wavetable bank (16-bit int16, 8 KB) generated
  from the /editor document: each of the 16 waves is a WaveConfig recipe (4
  sources × 30 shapes + warp stacks + combine + modifiers) — the FULL /editor
  mechanism ported 1:1 from the web editor (docs/editor-model.md,
  src/lib/wavetable/render.ts). The document is sent WHOLE (all 16 waves, all
  parameters, one call) from the web simulator via Osc::wteSetDocument (->
  wte_.set_doc, dirty flag) and the 16×256 frames are re-rendered at K-rate in
  blockPrepass (wteRegen, never in the audio path; sub-millisecond on the
  STM32H725 hard-FPU).
  This table IS the main voice: the A+B voices read it DIRECTLY at their own
  per-voice phasors (WteTable::lookup in the per-voice loop, see the "WTE
  wavetable morph" block above), with the SHARED morph position driven by the
  wave-LFO 1. Its level is the MAIN knob (the A+B voice level) — the old
  separate parallel "editor voice" (its own proc_ph phasor + the PLEV level
  knob) was removed: PMORPH (frame position) and PLEV (voice level) are
  NO-OPS now (superseded: morph = Wave LFO 1, level = MAIN).

  DEFAULT WAVETABLE (wte_default_doc / wte_default_wave in osc_wte_doc.h; the
  on-hardware / pre-document recipe, mirrored 1:1 in the web editor's
  createDefaultWave, store.svelte.ts). The 16 frames are NOT 16 bare sines:
  each frame is a SUM of four sine sources, normalized per frame so the table
  peaks in ~[-1, +1] (level-stable across the morph):
    [0] fundamental  (harmonic 1,        Sine)  — the note itself, gain 1/norm
    [1] organ colour (harmonic 2,        Organ) — top-end openness, gain
        (0.10 + 0.90*t)/norm, t = frame/15 (0.10 at frame 0 -> 1.00 at frame 15)
    [2] breathing    (harmonic 1,        FM Sine) — a slow wobble that
        de-correlates the frames so the morph audibly evolves, gain 0.25/norm
    [3] SUBHARMONIC  (harmonic 0.25 = f/4, Sine) — the LOW END. gain
        (0.18 + 0.37*end)/norm, where end = |frame-7.5|/7.5 (1 at frames 0/15,
        0 at the middle). A full Wave-LFO sweep therefore PUMPS the low end
        (swells at the table ends, dips at the middle) — organic bass movement
        under the static note.
  This is the whole reason the SUB1/SUB2 "sub voices" were REMOVED: the drone's
  low end now lives INSIDE the wavetable as a subharmonic partial of the same
  note (f/4 quantizes exactly in the 8-bit wire format, step 16/255 -> 0.249,
  bit-stable web-sim <-> wasm). The editor's harmonic control allows < 1 (min
  0.25, step 0.25) so any source can be a subharmonic.

PER-VOICE ENVELOPE + POLYPHONIC LOUDNESS (osc_mix.h, osc_constants.h):
  Each of the 6 voices is gated by its OWN A-H-R envelope (v.eg.env) in the
  accumulation loop: a new note (env 0->1 over 2 s) ADDS to the cluster
  instead of dragging the whole thing through a common MAX. (A common
  output envelope = max over all voices used to dip the entire cluster the
  instant a 2nd note started from env=0 -- the "silence on the 2nd note"
  bug. Per-voice gating removes that shared-dip.)
  env_sum  = sum over the 6 voices of its A-H-R envelope (0..kVoiceCount)
  env_comp = one-pole smoothed (tau ~5 ms, kClusterCompAlpha) of
             1 / env_sum   (floored at 1 -> a single note is never
             attenuated; N=1 -> 1 exactly, N=6 -> ~0.167)
  The SUMMED PER-VOICE signals (sum_main / sum_ring) are each multiplied by
  env_comp BEFORE the shared post-stage, so N notes read at ~one note's
  loudness instead of N times louder (the kMix* budget is designed for one
  note; without this, a full cluster feeds fold/saturate at ~x4 and the
  fastertanh2f soft-clip pegs -> "shouting / clipping").
  NOTE: the sub voices are ONE SHARED pair (not N per-voice voices), so they
  are NOT env_comp scaled — a single note -> exactly the old single-note
  sub level; N notes -> one stable fundamental under the cluster (no N-fold
  low-end growth, no "muddy" N incoherent subs).

COMMON POST-STAGE (once, on the compensated, per-voice-enveloped sums):
  mix  = kMixOsc * (sum_main * env_comp)
  (sum_main is the per-voice A+B sum reading the EDITOR wavetable directly — see the
   "WTE wavetable morph" block; there is no separate editor-voice term any more)
  -> FLD (wavefolder, FLD)
  -> SAT/ASYM (asymmetric tanh saturator, SAT + ASYM + DWDR walk)   [ x kMixOsc ]
  -> SVF 12 dB/oct LPF (CUT/RES + WFC cutoff walk, dry/wet)
  -> FORMANT (2 parallel IIR-comb resonators F1+F2, FMVT/FLVL, parallel-add)
  -> COMB (IIR resonator, COMB feedback, fixed delay)
  = y_main

out = tanh( kOutSat * kOutDrive * ( y_main*MAIN
            + (sum_ring*env_comp)*RMOD*MAIN ) )
  where the per-voice envelopes are already folded into the sums (each
        sum_main/ring term was multiplied by that voice's v.eg.env in the
        loop). There is NO separate sub/sub2 low-end branch and NO sidechain
        DUCK any more (both were removed with the SUB1/SUB2 sub voices — the
        low end is now inside the WTE wavetable, see the "DEFAULT WAVETABLE"
        block). There is NO common output envelope: N notes each fade in/out
        on their own; the cluster reads as one blended drone whose level is
        the sum of the individually-enveloped voices.

Mono-exactness: with 1 note, that voice's env gates its own contribution and
env_comp = 1/1 = 1, so the single-note path is numerically identical to the
pre-cluster mono unit EXCEPT the old SUB1/SUB2 sub voices + SUBB breath +
sidechain duck (all removed — the low end now lives inside the WTE wavetable).
Per-voice envelope + 1/N loudness are what make the polyphonic cluster both
level-independent of note count AND free of the 1->2 note boundary dip.
```

The CPU cost scales ~linearly with the number of ACTIVE notes, not with
kVoiceCount: the heavy SVF/formant/comb saturator run only ONCE on the
summed signal (shared post-stage); each active note adds only its lightweight
source (2 wavetable scans + FM-smooth + envelope + accumulators). (The old
shared sub bass — SUB1/SUB2 pair + anchor + SUBB gain — is gone; the low end
now lives inside the WTE wavetable, so there is no separate per-sample bass
pass.)
Measured (web-sim, `Osc::process` only, no FX, median of 5 runs): the shared
floor (post-stage + 6 idle-voice envelope passes) is ~95 ns/sample, and each
ADDED active note costs ~77 ns/sample. So: 1 note ≈ 172 ns/smp, 4 notes ≈ 410
ns/smp, 6 notes ≈ 558 ns/smp. Against a single 48 kHz core's per-sample budget
(20 833 ns/smp) that is ~0.8% (1 note) / ~2.0% (4) / ~2.7% (6) — i.e. 4→6 notes
is a +36% CPU step on the OSC stage, comfortably within budget. (The hardware
Cortex-M7 @ 48 kHz runs this far faster per sample than the wasm browser build,
so the on-device fraction is lower still; these figures are the relative 1/4/6
comparison, which is what matters for the voice-count trade-off.)







(SUB1 / SUB2 / ANCHOR / SUBB-breath / DUCK: REMOVED — the separate sub-octave
sub voices and the constant sidechain duck are gone. The low end now lives
INSIDE the WTE wavetable as a subharmonic partial (f/4), and the main + RMOD
mix is un-ducked. See the "DEFAULT WAVETABLE" block above.)
RING voice : (wavA * wavB) * RMOD * MAIN   (A*B ring-mod, ducked by MAIN)

The A-H-R envelope (2 s / 2 s) gates each voice in the accumulation loop
(note attack/release). There is no separate low-end branch to gate any more.
```

Key facts about the path:

* **Detune (DET)** is applied *per voice* **asymmetrically (GRIMM-style)**:
  A stays exactly at the note (factor 1.0), B is shifted UP by the full DET
  cents (B = f * 2^(DET/1200)). The beat is |f_B - f_A| ~ f * DET * ln2/1200
  per voice, and it is applied BEFORE the A/B sum so every note keeps its own
  beating pair. The sub bass stays on the cluster's env-weighted ANCHOR
  (one stable fundamental under the whole drone — see the "SHARED SUB BASS"
  block in the Sound path above).
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

**UI initialization** (web simulator only): at startup `createEditableParam`
(websim/scripts/setup-web-audio.js) positions each generated slider at
`Module.getEditParamValue(i)` (the main-thread processor state, the factory
default at first boot) BEFORE calling `apply(initRaw)` — without the
`input.value = initRaw` step the range inputs render at their HTML default
(no `value` attribute), so every readout would show 0% while the DSP sounds at
factory defaults (the sound is unaffected: `init()` on the worklet thread
already applied `edit_value_`; only the UI was out of sync). The WAVE bank
(index 13, rendered as an A..F radio group, not a slider) sets its checked
radio the same way (`setWaveBankChecked(waveBankFromRaw(initRaw))`) and,
deliberately, does NOT call `apply` — the radio group is the readout and a
write during startup would race the paced direct channel.

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

The unit exposes its **35** editable DSP parameters through a PARAM/VALUE
multiplexer on the two physical knobs:

* **Slot 0 PARAM** (fixed knob A) -- parameter selector, 0..34; the display
  shows the name of the selected parameter.
* **Slot 1 VALUE** (fixed knob B) -- raw 10-bit value (0..1023) of the selected
  parameter. Each parameter maps 0..1023 onto its own range; the raw value is
  cached per parameter, so re-selecting one restores its last value.
* **Slots 2/3 PARAM/VALUE** -- full duplicates of 0/1 (edit-menu visibility).
* **Slot 4 RndM** (edit-menu) -- randomization TYPE (All / Part / ORBI), see above.
* **Slot 5 Rnd** (edit-menu) -- randomization TRIGGER (0..15, debounced), see above.

All 57 editable DSP parameters (select_ index -> name), with range, factory
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
| 8 | SUB1 | (no-op) | — | — | **REMOVED**: the SUB1 f/2 sub-octave sine voice is gone. The low end now lives INSIDE the WTE wavetable as a subharmonic partial (the f/4 source in the default recipe — see the "DEFAULT WAVETABLE" block). The raw index is kept so the parameter layout is stable; the knob no longer drives any DSP and is not rendered in the web UI. |
| 9 | SUB2 | (no-op) | — | — | **REMOVED**: the SUB2 f/4 sub-2 voice is gone (low end is in the WTE wavetable). Raw index kept for a stable parameter layout; no DSP, not rendered. |
| 10 | SAT | 0..300 | 0..3.0 drive | 100 (1.0x) | Main A+B saturator drive. |
| 11 | ASYM | +/-% | -100..+100% | 0% | Saturator asymmetry (even harmonics; >0 = positive half harder). |
| 12 | FLD | % | 0..100% | 0% (off) | Pre-drive wavefolder depth (metallic edge). |
| 13 | WAVE | (legacy) A | (legacy) | *No-op* (superseded by the WTE editor wavetable — the A+B voices read the 16x256 editor table directly, see rows 35/36 and the "WTE wavetable morph" block). The stock A..F banks are gone; the param keeps its layout slot, the display still shows a bank letter. |
| 14 | WA2 | (legacy) A | (legacy) | *No-op* (superseded by the WTE editor wavetable): the old stream-2 wave bank / 2D-morph y-axis is gone with the stock banks. Keeps its layout slot. |
| 15 | MAIN | % | 0..100% | 100% | Main (A+B) voice level (also ducks the ring-mod voice). The A+B voices ARE the editor wavetable (WTE) now, so MAIN is the level of the editor texture as well (the old separate PLEV editor-voice level is a no-op — see row 55). MAIN = 0 mutes the A+B + RMOD cluster. |
| 16 | TWDR | % | 0..100% | 0% (off) | *No-op* (was: random-walk depth on the wavetable morph). The WTE-table morph is now driven by the wave-LFO 1 (WLFR / WLFD / WLFS / WLFG, rows 35/36/40/41), not by the random-walk Timbre stream (which now drives only the SVF cutoff, row 21). |
| 17 | DWDR | % | 0..100% | 0% (off) | Walk depth on saturator drive (+/-0.5). |
| 18 | FWDR | % | 0..100% | 0% (off) | Walk depth on wavefolder depth (+/-0.5). |
| 19 | CUT | Hz / kHz | 80 Hz..12 kHz (log) | ~9 kHz | SVF cutoff **and** dry/wet blend of the **1st** (12 dB/oct) stage (0% = raw, 100% = full LPF). The 2nd (cascaded, 12->24 dB/oct) stage is transparent by default (svf_wet2 = 0) — see the SVF cascade note below. |
| 20 | RES | X.Y | 0.5..2.5 Q | 0.5 | SVF resonance (applied to BOTH cascaded stages). |
| 21 | WFC | % | 0..100% | 0% (off) | Walk depth on SVF cutoff (+/-1 half-octave). The SAME (wandered) cutoff feeds both stages in unison. |

**SVF cascade (24 dB/oct)**: a second `Svf::State` (`svf2_`) is cascaded after the 1st, with its own dry/wet (`svf_wet2`, default 0 = transparent) exposed as the **FCUT2** edit parameter (the "Cutoff 24dB" slider in the web sim). At the factory preset the cascade is off (12 dB/oct, bit-identical); raising FCUT2 morphs the response 12 -> 24 dB/oct, giving a steeper top-end roll-off for taming FM upper sidebands (the "FM complex but not screechy" recipe). Both stages share the SAME (random-walk-wandered) cutoff and resonance (CUT / RES), so they track in unison. The 2nd stage is always full-wet as an LPF of the 1st stage's output (no separate dry/wet inside the cascade) — FCUT2 alone controls the 12 <-> 24 dB/oct morph.
| 22 | BRT | % | 0..100% | 0% (off) | **Breath** depth: per-voice level swing on the MAIN (A+B) voice. INDEPENDENT of RATE — turns on with BRT > 0 even at RATE = 0. Only the main voice breathes; SUB1/SUB2 stay static. |
| 23 | BRR | % | 0..100% -> 0.1 s..10 s (log) | 50% | **Breath** stream period: how often each note's breath target is redrawn. |
| 24 | SMT | % | 0..100% -> 10 ms..1.5 s (log) | 50% | **Breath** stream time constant: how quickly each note's level tracks its target. |
| 25 | JIT | % | 0..100% -> 0..0.5% phase | 0% (off) | Random phase jitter on A/B (smears the carrier). |
| 26 | RMOD | % | 0..100% | 0% (off) | A*B ring-mod parallel voice (ducked by MAIN). |
| 27 | FM1 | 0.XXX | LOG 0..0.2 cycles (raw 0 = OFF, raw 1023 = 0.2) | 0% (off) | Static phase-FM of osc A by B (not RATE-gated). LOG scale: 1e-5..0.2 cycles. |
| 28 | FM2 | 0.XXX | LOG 0..0.2 cycles (raw 0 = OFF, raw 1023 = 0.2) | 0% (off) | Static phase-FM of osc B by A (not RATE-gated). LOG scale: 1e-5..0.2 cycles. |
| 29 | FMAB | % | 0..100% | 0% (off) | Walk depth on the FM-A index ("breathing" FM, RATE-gated). |
| 30 | FMBB | % | 0..100% | 0% (off) | Walk depth on the FM-B index ("breathing" FM, RATE-gated). |
| 31 | FMVT | A/E/I/O/U | 0..100% -> 5 vowels (A-E-I-O-U morph) | 50% (mid) | Formant vowel position: morphs BOTH comb delays (the vowel's F1+F2 pair). The stage is OFF until FLVL>0, so this dials the vowel silently. |
| 32 | FLVL | % | 0..100% | 0% (off) | Formant stage level: shared feedback (0..0.9) + parallel-add gain of the two comb resonators (F1 peak 135..365 Hz, F2 peak 435..1145 Hz, gain 1.0). 0 = transparent. |
| 33 | COMB | % | 0..100% -> 0..0.9 fb (fixed ~190 Hz delay) | 0% (off) | Comb resonator: IIR feedback peak + harmonics (wooden/cavernous tint). |
| 34 | SUBB | (no-op) | — | — | **REMOVED**: the shared sub bass (SUB1/SUB2) is gone, so there is no "sub-bass breath" to breathe. The low end now lives INSIDE the WTE wavetable as a subharmonic partial (and a full Wave-LFO sweep pumps it — see the "DEFAULT WAVETABLE" block). Raw index kept for a stable parameter layout; no DSP, not rendered. |
| 35 | WLFR | 0 H / 0.NNN H / 0.N H / 1 H | 0..1 Hz (LOG: linear knob -> exponential freq; raw 0 = 0 Hz / OFF) | 0 H (LFO OFF) | **Internal wave-LFO 1 frequency**: the LFO that drives the EDITOR wavetable (WTE) morph — the morph position across the 16 frames is 7.5 + lfo * 7.5 (see the "WTE wavetable morph" block in the Sound path). LOG scale (raw 0 = 0 Hz / LFO OFF; else 10^(3*raw/1023 - 3) = 0.001..1 Hz across 3 decades, the same convention as the SPDx/SMTx knobs), so each equal knob step is a constant multiplier of the previous frequency — fine control near 0. 0 = static table (the factory default). This REPLACES the hardware's OSC-button RATE knob (which is now a NO-OP, `setShapeLfo()` is a stub). |
| 36 | WLFD | % | 0..100% (linear) | 100% | **Internal wave-LFO 1 morph depth**: 0 = the WTE table stays static at its middle frame (7.5), 100% = the morph sweeps the FULL table (frame 0 to frame 15, and back). The factory default is 100%, so any non-zero WLFR drives the morph across all 16 editor waves. This REPLACES the hardware's OSC-button SHAPE knob (now a NO-OP). |
| 37 | W2LR | (legacy) 0 H | (legacy) | *No-op* (superseded by the WTE editor wavetable): the stream-2 wave-LFO is gone with the 2D morph. Keeps its layout slot. |
| 38 | W2LD | (legacy) 100% | (legacy) | *No-op* (superseded by the WTE editor wavetable): the stream-2 wave-LFO depth is gone with the 2D morph. Keeps its layout slot. |
| 39 | WMIX | (legacy) 0% | (legacy) | *No-op* (superseded by the WTE editor wavetable): the stream 1 <-> stream 2 crossfade is gone with the 2D morph. Keeps its layout slot. |
| 40 | WLFS | SIN / S / WALK | SIN / S / WALK (raw 0..511 / 512..767 / 768..1023) | SIN | **Wave-LFO 1 shape**: the WTE-morph LFO trajectory (see `osc_lfo_sim.h`): SIN = classic sine, S = S-curve with a local reversal, WALK = a random-walk scene regenerated every LFO cycle (Catmull-Rom smoothed; the mean control-point gap is WLFG). |
| 41 | WLFG | N s | 1..300 s (LOG) | ~30 s | **Wave-LFO 1 WALK gap**: mean seconds between random control points of the WALK scene (only used when WLFS = WALK; no effect in SIN / S). |
| 42 | W2FS | (legacy) SIN | (legacy) | *No-op* (superseded by the WTE editor wavetable): the stream-2 LFO shape is gone with the 2D morph. Keeps its layout slot. |
| 43 | W2FG | (legacy) N s | (legacy) | *No-op* (superseded by the WTE editor wavetable): the stream-2 LFO WALK gap is gone with the 2D morph. Keeps its layout slot. |
| 44 | WMXR | (legacy) 0 H | (legacy) | *No-op* (superseded by the WTE editor wavetable): the wave-mix LFO is gone with the 2D morph (stream 1 <-> stream 2 crossfade). Keeps its layout slot. |
| 45 | WMXD | (legacy) 100% | (legacy) | *No-op* (superseded by the WTE editor wavetable): the wave-mix LFO swing depth is gone with the 2D morph. Keeps its layout slot. |
| 46 | WMXS | (legacy) SIN | (legacy) | *No-op* (superseded by the WTE editor wavetable): the wave-mix LFO shape is gone with the 2D morph. Keeps its layout slot. |
| 47 | WMXG | (legacy) N s | (legacy) | *No-op* (superseded by the WTE editor wavetable): the wave-mix LFO WALK gap is gone with the 2D morph. Keeps its layout slot. |
| 48 | FMCR | 0 H / 0.NNN H / 0.N H / 1 H | LOG 0..1 Hz (raw 0 = OFF) | 0 H (OFF) | **FM modulator C frequency**: an INDEPENDENT low-frequency oscillator (the modulator) that phase-FMs the (A+B) sum (the carrier). C is NOT heard by itself -- it only bends the sum's phase for vibrato / motion colour on top of the (untouched) cross-FM A+B timbre. raw 0 = OFF (the sum is unmodulated, regression-safe). The log scale (3 decades) gives fine control near 0; 1 Hz is the ceiling (above it a SAW/SQR modulator would be heard as its own tone on a low note). |
| 49 | FMCW | SIN / TRI / SAW / SQR / NOISE | raw band (0..341 / 342..511 / 512..682 / 683..853 / 854..1023) | SIN | **FM modulator C wave shape**: the C oscillator's waveform (SIN = smooth vibrato, **TRI = triangle — the "soft" shape, harmonics fall off as 1/n² (vs 1/n for SIN/SAW) so it draws the fewest upper sidebands → the go-to "FM complex but not screechy" choice**, SAW = metallic, SQR = hard/robotic, NOISE = chaotic tremolo). The same wave drives BOTH the C (vibrato) and S (static teeth) modulators. |
| 50 | FMCD | % | 0..100% (LINEAR) -> 0..0.05 cycles phase peak | 0% (OFF) | **FM modulator C depth**: how strongly C bends the (A+B) sum's phase (0.05 cycles = 5% of the carrier at 100%). 0 = OFF. |
| 51 | FMST | 0.05..16 (teeth) / OFF | LOG 0.05..16 teeth per carrier cycle (raw 0 = OFF) | OFF | **FM static teeth (rate)**: the NUMBER of teeth of the S modulator per (A+B) carrier cycle. S is a carrier-LOCKED FM oscillator (the static counterpart of C's free-running vibrato): its phasor advances at FMST × the A rate, so it draws FMST teeth of phase deviation per note cycle — a STATIC harmonic comb that stays in tune with the note. LOG scale (4 decades; raw 0 = OFF, raw 1 = 0.05 teeth, 1023 = 16 teeth). **Teeth < 1 = sub-octave** (fm/fc < 1, the modulator runs slower than the carrier — a deep organ-like comb with sidebands that dip below the note); **1 tooth = in-tune** (fm/fc = 1); 0 = OFF (bit-identical). |\
| 52 | FMSA | % | 0..100% (LINEAR) -> 0..0.05 cycles peak | 100% | **FM static depth (tooth size)**: the AMPLITUDE (peak swing) of each S tooth — how strongly each tooth bends the (A+B) sum's phase. The effective per-tooth offset is FMSA × 0.05 cycles (5% of the carrier at full scale). The tooth SHAPE is shared with the C modulator (FM Mod Wave / FMCW: SIN/TRI/SAW/SQR/NOISE). 0 = no static FM (bit-identical); 100% (default) = full tooth size. |
| 53 | FMAM | % | 0..100% (LINEAR) | 100% | **FM MOD amount (global master)**: how strongly the WHOLE FM MOD block is applied — a single multiplier on BOTH the C (vibrato) and S (static teeth) phase offsets before they are added to the A/B phasors. 100% (default) = full C+S exactly as set by FMCD/FMSA (bit-identical regression-safe); 0% = the whole FM MOD block is OFF (both offsets are exactly 0, the (A+B) sum is unmodulated) regardless of the individual C/S settings. It scales the DEVIATION only, never the C/S rates (FMCR/FMST) or shapes (FMCW) — turning FMAM down fades the modulation out smoothly while the modulator keeps running in phase. |
| 54 | FCUT2 | % | 0..100% (LINEAR) | 0% (12 dB/oct) | **Cascaded 2nd-stage SVF wet** (the "Cutoff 24dB" slider in the web sim): the dry/wet of the SECOND (cascaded) `Svf::State` (`svf2_`). It shares the SAME cutoff (CUT) and resonance (RES) as the 1st stage — only the wet amount is settable. 0% (default) = the 2nd stage is transparent → a single 12 dB/oct SVF (bit-identical to the factory preset); 100% = a full 2nd-stage LPF → 24 dB/oct total. Raising FCUT2 gives a steeper top-end roll-off for taming FM upper sidebands (the "FM complex but not screechy" recipe). |
| 55 | PLEV | (legacy) 0% | (legacy) | *No-op* (superseded): the separate editor-wavetable VOICE (its own phasor + level) was removed — the WTE table IS the main A+B voice now, so its level is MAIN (row 15). Keeps its layout slot. |
| 56 | PMORPH | (legacy) 7.5 | (legacy) | *No-op* (superseded): the editor-wavetable MORPH position is now driven by the internal wave-LFO 1 (WLFR / WLFD, rows 35/36), centered on frame 7.5 (this old default). Keeps its layout slot. |

(FOLD2 — second cascaded wavefolder — and DCK — sidechain-duck amount — were
removed. DCK is now a CONSTANT: see "Non-editable (fixed) behaviour" /
"DUCK" above. The former FM bus — FMCB / FMSV, phase-FM on the comb resonator
delay and the SVF cutoff, driven by the shared FM-B index — was also removed:
it proved inaudible in practice. It was replaced by the FM modulator C above
(FMCR/FMCW/FMCD) — a dedicated, always-running modulator that phase-FMs the
(A+B) sum, which is the "post-oscillator FM" the FM bus was removed for.)

### The four random-walk streams

RATE is the ceiling that enables the first three wandering streams
(0 = Tone / Timbre / Density static). The **Breath** stream is INDEPENDENT of
RATE: it is gated only by BRT (Breath Depth) and breathes even at RATE = 0.
Each stream is independent (own period, own time constant) and its targets are
**not** phase-locked:

| Stream | Period / Smooth | Target knob(s) | Drives | RATE-gated |
|--------|-----------------|----------------|--------|------------|
| **Tone** | SPD1 / SM1 | (depth from RATE) | A/B detune (+/-10 c) + FM index breathing | yes |
| **Timbre** | SPD2 / SM2 | WFC | SVF cutoff (+/-1 half-oct) | yes |
| **Density** | SPD3 / SM3 | DWDR, FWDR | saturator drive (+/-0.5) + wavefolder depth (+/-0.5) | yes |
| **Breath** | BRR / SMT | BRT | per-note MAIN (A+B) voice level (each note breathes independently) | **no** |

Each stream's targets are drawn from an **exponential distribution** (Poisson
process) with the period value as the mean, so the cadence "breathes" instead
of ticking at a fixed period. The tracking time constant is independent of the
period, so the walk depth does not depend on speed.

The **Breath** stream drives the MAIN voice level of EACH cluster note using a
**"Rotating Focus"** algorithm: on each tick (period = BRR), the "focus"
(highlighted note) advances to the next active voice in round-robin order. The
focus voice's level increases by BRT (Breath Depth); the other voices' levels
decrease by BRT / (n_active - 1), so the SUM of all levels stays constant (the
overall loudness does not pump). The transition is SMOOTH: each voice's level
tracks its target with the SMT time constant (exponential, non-linear). With
only 1 note, breathing is off (level stays at 1.0). The per-voice BREATH
(BRT) breathes only the MAIN (A+B) voice level. (The old SHARED sub bass
(SUB1/SUB2) + its independent **SUBB** "sub-bass breath" are REMOVED — the
low end now lives INSIDE the WTE wavetable as a subharmonic partial, and a
full Wave-LFO sweep pumps it; see the "DEFAULT WAVETABLE" block.)

### Non-editable (fixed) behaviour

| Behaviour | Value | Notes |
|-----------|-------|-------|
| Attack / release | 2 s / 2 s | Exponential A-H-R, own envelope (platform EG open). |
| Low end | inside the WTE wavetable (f/4 subharmonic) | The old SUB1 (f/2) / SUB2 (f/4) sub voices, the env-weighted ANCHOR, the SUBB breath, and the constant sidechain duck are all REMOVED. The drone's low end is now the f/4 subharmonic partial of the note carried by the default wavetable recipe (see the "DEFAULT WAVETABLE" block); it is read out of the table by the A+B voices, so it is gated per-voice like the rest of the table. |
| WTE table morph | centered on frame 7.5, full 0..15 sweep at WLFD = 100% | The editor-wavetable morph position is the wave-LFO 1 (WLFR/WLFD/WLFS/WLFG): `wte_morph = 7.5 + lfo * 7.5` (at 100% depth the LFO sweeps all 16 frames; at 0 depth the table is static at the middle). |
| Platform shape-LFO | NO-OP | The platform's OSC-button RATE/SHAPE knobs (delivered via `setShapeLfo()`) are IGNORED — the callback is a stub. The WTE editor-wavetable morph is driven ENTIRELY by the ONE INTERNAL wave-LFO 1 (edit params WLFR / WLFD / WLFS / WLFG; see the Edit parameters table above and `osc_lfo_sim.h`). The old two-stream 2D morph (W2LR / W2LD / WMIX / the wave-mix LFO) is gone with the stock banks (those params are no-ops now). (The old +/-5 c sub-octave FM on the SUB1 phasor is GONE with the sub voices.) |
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
| osc_waves.h | Legacy stock-bank wavetable scanning + pitch-shifted morph (vestigial — the A+B voices now read the WTE editor wavetable directly, see `osc_wte.h`). |
| osc_saturator.h | Stateless wavefolder (fold) + asymmetric tanh saturator (saturate). |
| osc_svf.h | 12 dB/oct state-variable low-pass (cutoff/resonance/dry-wet + stability clamps). |
| osc_random_walk.h | RandomWalk -- multi-channel wandering (exponential target cadence). |
| (osc_sub2.h removed) | The f/4 Sub2Voice is gone — the low end now lives inside the WTE wavetable as a subharmonic partial (see the "DEFAULT WAVETABLE" block). |
| osc_detune.h | Detune::factor() -- 2^(cents/1200) Taylor expansion (no libm). |
| osc_envelope.h | AhEnvelope -- single-voice A-H-R state machine. |
| osc_lfo_sim.h | INTERNAL wave-LFO synthesizer (`ShapeLfoSim`; the LIVE instance is `lfo_sim_`, driving the WTE editor-wavetable morph via the WLFR / WLFD / WLFS / WLFG edit parameters — the morph position is 7.5 + lfo * 7.5. The old `lfo_sim2_` (stream-2 bank morph) and `lfo_mix_` (wave-mix crossfade) instances no longer process in the audio path — stream 2 / wave-mix are gone with the stock banks). |
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
