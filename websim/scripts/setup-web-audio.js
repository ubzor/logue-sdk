// UI setup for the drone chain simulator (extracted from the inline
// <script> block in websim/index.html). Plain (non-module) script: declares
// window.setupWebAudioAndUI, which emscripten glue code (drone.js, linked
// via the {{{ SCRIPT }}} placeholder) invokes through EM_ASM once the wasm
// worklet module is ready. It must therefore be a classic script (not
// type="module") - the classic global assignment is all the glue needs.
// Loaded in index.html before the {{{ SCRIPT }}} placeholder.

    // Expose function globally so EM_ASM can call it
    window.setupWebAudioAndUI = function (context, wasmProcessor) {

      function approximatelyEqual(a, b, epsilon = 1e-4) {
        return Math.abs(a - b) < epsilon;
      }

      // The unit exposes its DSP knobs through a PARAM/VALUE multiplexer on
      // the hardware (knob A = PARAM_SEL, knob B = PARAM_VAL; see header.c and
      // osc.h). The simulator renders each of the 17 selectable parameters as
      // its own slider instead of the 4 header slots, which is equivalent but
      // more convenient.
      //
      // Each slider talks to the DSP through the direct edit-parameter
      // channel (Module.setEditParam, see wasm.cc), NOT through WebAudioParam
      // slots 0/1. The direct channel packs (index, raw) into a single 32-bit
      // slot that the audio worklet thread consumes once per block -- the
      // (select_, PARAM_VAL) pair is applied atomically, exactly like the
      // hardware's unit_set_param_value() callback. The old WebAudioParam
      // path suffered K-rate smoothing: moving a slider made select_ sweep
      // through intermediate values (0 -> 1 -> 2 -> ... -> target), each of
      // which re-applied a different edit parameter to the DSP, producing
      // audible glitches that looked like a phantom LFO.
      // 35 editable parameters total (edit indices 0..34); see EDIT_PARAM_NAMES
      // below for the full index->name map. This value only sizes the scratch
      // array in randomRaw() — the actual sliders are created from
      // PARAM_GROUPS. Kept in sync with EDIT_PARAM_NAMES.length (asserted).
      // (FOLD2 and DCK were removed — FOLD2 was judged unnecessary, DCK is now
      //  a constant (kDuckConstant in osc_constants.h).)
      const EDIT_PARAM_COUNT = 35; // 35 = RATE..SUBB
      // Names in select_ order (kEditParamFirst = RATE_AMT, see osc.h). The
      // polyphonic 6-voice cluster: each new note gets its own voice (no
      // glide: notes start at their exact pitch with random per-voice phases).
      // 0 RATE (shared depth ceiling), 1 DETU,
      // 2 SPD1, 3 SMT1 (Tone stream period / smooth),
      // 4 SPD2, 5 SMT2 (Timbre stream period / smooth),
      // 6 SPD3, 7 SMT3 (Density stream period / smooth),
      // 8 SUB1, 9 SUB2, 10 SATU, 11 ASYM, 12 FOLD, 13 WAVE (bank A..F, LFO morphs within bank),
      // 15 MAIN (main A+B mixed voice level, 0..100%, default 100%),
      // 16 RWAV, 17 RDRV, 18 RFLD,
      // 19 FCUT, 20 RES, 21 RWFC,
      // 22 RVLL (RW_LVL, breath depth), 23 RVLS (RW_LSP, breath period),
      // 24 RVLSM (RW_LSM, breath smooth),
      // 25 JIT (random phase jitter on A/B, 0..100%),
      // 26 RMOD (A*B ring-mod parallel voice, 0..100%),
      // 27 FM1 (STATIC phase-FM on A by B, 0..100%),
      // 28 FM2 (STATIC phase-FM on B by A, 0..100%),
      // 29 RFMA (random-walk depth on FM A, 0..100%, RATE-gated),
      // 30 RFMB (random-walk depth on FM B, 0..100%, RATE-gated),
      // 31 FORM (formant vowel position), 32 FVOL (formant dry/wet),
      // 33 COMB (comb resonator feedback),
      // 34 SUBB (SHARED sub-bass "breath" depth: the one shared SUB1/SUB2 pair
      //    follows the LOUDEST (breath-focus) voice's envelope. 0% = static
      //    low end (old behaviour), default ~30% -> gentle +/-18% swing. The
      //    sub bass itself is ONE shared pair for the whole polyphonic cluster
      //    (env-weighted ANCHOR note) — see the "Shared sub bass" block in
      //    osc_mix.h / the SUBB comment in osc_params.h.)
      const EDIT_PARAM_NAMES = [
        "Rate", "Detune",
        "Tone Rate", "Tone Smooth", "Timbre Rate", "Timbre Smooth", "Density Rate", "Density Smooth",
        "Sub 1", "Sub 2", "Drive", "Asym", "Fold", "Wave", "Wave B (deprecated)",
        "Main",
        "Morph Wander", "Drive Wander", "Fold Wander",
        "Cutoff", "Resonance", "Cutoff Wander",
        "Breath Depth", "Breath Rate", "Breath Smooth",
        "Jitter", "Ring Mod", "FM A", "FM B", "FM A Bre", "FM B Bre",
        "Formant Vowel", "Formant Level",
        "Comb Res",
        "Sub Bass Bre",
      ];

      // Semantic groups of the 37 edit parameters (UI-only; the DSP enum order
      // in osc.h is untouched). 3-column Parameters layout; each group becomes
      // one titled block. Items are edit_ indices (select_ order).
      // Each group has an explicit `col` (0/1/2) so the layout loop places it
      // as a true column BLOCK (not a round-robin):
      //   col 0: MAIN + BREATH + FORMANT — A+B voice (static params) + wander
      //   col 1: WANDER (global) + WANDER TGT (targets)
      //   col 2: SUBS + FILTER + CLUSTER (polyphonic 6-voice: COMB)
      //   MAIN        MAIN WAVE DETU SATU ASYM FOLD FM1 FM2 JIT RMOD
      //   BREATH      RVLL RVLS RVLSM (voice-level wander, 4th stream)
      //   FORMANT     FMVT FLVL
      //   WANDER      RATE SPD1 SMT1 SPD2 SMT2 SPD3 SMT3
      //   WANDER TGT  RWAV RWFC RDRV RFLD RFMA RFMB
      //   SUBS        SUB1 SUB2 SUBB (sub-bass breath depth)
      //   FILTER      FCUT RES
      //   CLUSTER     COMB (comb resonator)
      const PARAM_GROUPS = [
        // ---- Column 0 ----
        { title: "MAIN",       col: 0, items: [15, 13, 10, 11, 12, 27, 28, 25, 26, 0] },
        { title: "BREATH",   col: 0, items: [22, 23, 24] },
        { title: "FORMANT",  col: 0, items: [31, 32] },
        // ---- Column 1 ----
        { title: "WANDER",     col: 1, items: [1, 2, 3, 4, 5, 6, 7] },
        { title: "WANDER TGT", col: 1, items: [16, 21, 17, 18, 29, 30] },
        // ---- Column 2 ----
        { title: "SUBS",       col: 2, items: [8, 9, 34] },
        { title: "FILTER",     col: 2, items: [19, 20] },
        { title: "CLUSTER",    col: 2, items: [33] },
      ];

      // Format the raw 10-bit value (0..1023) of one of the 37 editable
      // parameters for display. NOTE: idx is the edit_ index, and because
      // PARAM_SEL=0 / PARAM_VAL=1 are explicit, DETUNE=2 — so the edit indices
      // are OFFSET BY 2 from the enum's sequential order. Mapping:
      //   0: DETU  - static detune of the A+B+C sum (+/-10 cents)
      //   1: RATE  - shared random-walk depth ceiling for all 3 streams (0..100%)
      //   2..7: SPD1..SMT3 - per-stream period / time constant (0..100%, log)
      //   8: SUB1  - sub-octave sine level (0..100%)
      //   9: SUB2  - second sub level (0..100%)
      //  10: SATU  - saturator drive (0..300)
      //  11: ASYM  - asymmetric saturator (-100%..+100%)
      //  12: FOLD  - pre-drive wavefolder depth (0..100%)
      //  13: WAV1  - osc A wavetable (0..90)
      //  14: WAV2  - osc B wavetable (0..90)
      //  15: WAV3  - osc C (chord) wavetable (0..90)
      //  16: CHRD  - chord-voice level (0..100%)
      //  17: CINT  - chord interval (0..1900 cents, continuous)
      //  18: MAIN  - main (A+B mixed) voice level (0..100%, default 100%)
      //  19: RWAV  - random-walk depth on wavetable morph (0..100%)
      //  20: RDRV  - random-walk depth on saturator drive (0..100%)
      //  21: RFLD  - random-walk depth on wavefolder depth (0..100%)
      //  22: FCUT  - SVF cutoff + dry/wet (80 Hz .. 12 kHz, log)
      //  23: RES   - SVF resonance Q (0.5 .. 2.5)
      //  24: RWFC  - random-walk depth on SVF cutoff (0..100%)
      //  25: RVLL  - breath depth on voice levels (0..100%)
      //  26: RVLS  - breath stream period (0..100%)
      //  27: RVLSM - breath stream time constant (0..100%)
      //  28: CHSAT - chord's own saturator drive (0..300)
      //  29: CHFLD - chord's own wavefolder depth (0..100%)
      //  30: CINT2 - second chord interval (0..1900 cents, continuous)
      //  31: JIT   - random phase jitter on A/B (0..100%)
      //  32: RMOD  - A*B ring-mod parallel voice (0..100%)
      //  33: FM1   - STATIC phase-FM on A by B (0..100%)
      //  34: FM2   - STATIC phase-FM on B by A (0..100%)
      //  35: RFMA  - random-walk depth on FM A (0..100%, RATE-gated)
      //  36: RFMB  - random-walk depth on FM B (0..100%, RATE-gated)
      function formatParamValue(idx, raw) {
        // Mirrors Osc::getParameterStrValue in osc.h (single source of truth):
        //   WAV1 (WAVE) -> bank letter A..F (raw 0..1023 -> bank 0..5; the LFO
        //                  morphs within the selected bank, see osc_waves.h)
        //   ASYM      -> -100..+100% (raw*200/1023 - 100)
        //   DETU      -> +/-10 cents, centered on the raw MIDPOINT (511),
        //                 NOT 1023: (raw-511)*10/511. Full knob (1023) reads +10.
        //   SATU      -> 0..300 (drive 0..3.0)
        //   SPDx/SMTx -> 0..100% (log-mapped period / time constant)
        //   CINT      -> interval cents (0..1900)
        //   FCUT      -> Hz / kHz (log-mapped 80 Hz .. 12 kHz)
        //   RES       -> Q (0.5 .. 2.5)
        //   FM1/FM2   -> 0..0.1c (quadratic curve)
        //   others    -> 0..100%
        if (idx === 13 || idx === 14) {
          // WAVE selects the bank A..F (raw 0..1023 -> bank 0..5)
          const bank = Math.round((raw / 1023) * 5.999);
          return "ABCDEF".charAt(Math.max(0, Math.min(5, bank)));
        }
        if (idx === 11) {
          const pct = Math.round((raw * 200) / 1023) - 100;
          return (pct >= 0 ? "+" : "") + pct + "%";
        }
        // DETU (edit idx 1, RATE-first order): +/-10 cents, centered on 511.
        if (idx === 1) {
          const cents = Math.round(((raw - 511) * 10) / 511); // /511, not /1023
          return (cents >= 0 ? "+" : "") + cents + " c";
        }
        if (idx === 10)
          return String(Math.round((raw / 1023) * 300)); // 300 = kSatuMax*100 (SATU)
        // (CHSAT/CHFLD/CINT/CINT2 were removed — the chord/interval concept is gone.)
        if (idx === 27 || idx === 28) {
          // FM1 / FM2 (edit 27/28): phase-FM index, 0..kFmMaxCents/2 cycles
          // (0..0.05 c at kFmMaxCents=0.1, mirrors the constant in osc.h).
          // Display the ACTUAL depth after the quadratic kFmCurveExp (2.0)
          // curve that the audio path applies: powf(raw/1023, 2) * kFmMaxCents.
          // Low knob -> ~0c (near-silent "breathing"), full knob -> 0.1c.
          return (Math.pow(raw / 1023, 2) * 0.1).toFixed(2) + "c";
        }
        if (idx === 19) {
          // FCUT (edit 19): SVF cutoff (log-mapped 80 Hz .. 12 kHz). Hz or kHz.
          // Mirrors Svf::cutoffHz: fc = 80 * (12000/80)^(raw/1023).
          const knob = raw / 1023;
          const fc = 80 * Math.pow(12000 / 80, knob);
          if (fc >= 1000) return (fc / 1000).toFixed(1) + "k";
          return Math.round(fc) + "H";
        }
        if (idx === 20) {
          // RES (edit 20): SVF resonance Q (0.5 .. 2.5)
          const q = 0.5 + (raw / 1023) * 2.0;
          return q.toFixed(1);
        }
        if (idx === 31) {
          // FORM (edit 31): formant vowel position. Show the nearest vowel
          // (A/E/I/O/U) as a single letter.
          const vi = Math.min(4, Math.floor((raw / 1023) * 4.999));
          return "AEIOU".charAt(vi);
        }
        // Every other index formats as 0..100%: 0..7 (RATE, SPD1..SMT3),
        // 8..9 (SUB1/SUB2), 12 (FOLD), 15 (MAIN, default 100%),
        // 16..18 (RWAV/RDRV/RFLD), 21 (RWFC), 22..24 (Breath),
        // 25 (JIT), 26 (RMOD), 29..30 (RFMA/RFMB), 32 (FVOL), 33 (COMB),
        // 34 (SUBB — sub-bass breath depth, 0..100% -> 0..0.6 swing).
        return Math.round((raw * 100) / 1023) + "%";
      }

      // ---- WAVE bank (edit index 13, WAV1) -----------------------------------
      // The WAVE knob selects a BANK (0..5, A..F); the LFO morphs WITHIN the
      // selected bank (intra-bank position centered on the bank, see
      // osc_waves.h). The DSP (osc_param_apply.h, case WAV1) quantizes the
      // raw to a bank: bank = int((raw/1023) * 5.999), clamped 0..5, and
      // stores wave_pos = bank. The reverse mapping for the UI:
      // raw = round(bank * 1023 / 5). Both directions are exact round-trips:
      //   b -> round(b*204.6) -> floor(raw*5.999/1023) = b  (b = 0..5)
      // The simulator renders WAVE as a radio group (A..F) instead of the
      // usual range slider — the same FlyonUI "join radio with btn" pattern
      // as the HW Filter type / Octave groups. No hidden slider is created,
      // so applyPresetRaw / fireRandom special-case index 13 (they walk
      // #editparam_<i> by index).
      const WAVE_PARAM_IDX = 13;
      const WAVE_BANKS = ["A", "B", "C", "D", "E", "F"];
      function waveBankFromRaw(raw) {
        let b = Math.floor((raw / 1023) * 5.999);
        if (b < 0) b = 0; else if (b > 5) b = 5;
        return b;
      }
      function rawFromWaveBank(b) {
        return Math.round(b * 1023 / 5);
      }
      // Checked state of the WAVE radio group. Set by the row builder in
      // createEditableParam once the group exists; the Rnd-morph poll calls
      // it to re-check the radio whose bank the morphing raw quantizes to
      // (no setEditParam — the morph owns the write stream).
      let setWaveBankChecked = null;

      // Format the raw 10-bit value (0..1023) of a DRONE-MOD FX parameter.
      // `stage` is the FX stage (0=MOD, 1=DEL, 2=REV); `which` is the param
      // index within that stage (0=TIME, 1=DEPTH; DEL adds 2=MIX as a drywet
      // knob; the MOD edit params are formatted by formatModEditParam instead).
      // DEL MIX (stage 1, which 2) shows a drywet percentage: raw -1000 ->
      // -100% (full dry), raw +1000 -> +100% (full wet); raw 0 = BAL (50/50).
      function formatFxParam(stage, which, raw) {
        // DEL MIX (stage 1, which 2) and REV MIX (stage 2, which 2) show a
        // drywet percentage: the knob is a drywet param (header.c
        // -1000..1000, frac=1 base-10), so raw/10 is the percentage:
        // raw -1000 -> -100% (full dry, pure wire), raw 0 -> 0% (50/50),
        // raw +1000 -> +100% (full wet); everything else is proportional.
        if ((stage === 1 || stage === 2) && which === 2) {
          const pct = Math.round(raw / 10.0);
          return (pct > 0 ? "+" : "") + pct + "%";
        }
        return Math.round((raw * 100) / 1023) + "%";
      }

      // Format a DRONE-MOD EDIT parameter. Two independent multiplexers now:
      //   MOD group (idx 0..4) = DRIFT / CMODE / CHOR / FLNGR / ENSMBL
      //   TAPE group (idx 0..4) = NOISE / NRATE / TAPWOB / HISS / WARP
      // CMODE (MOD idx 1) shows the Juno voice band (I / I+II / II, equal
      // thirds); everything else shows 0..100%.
      const MOD_EDIT_NAMES = ["DRIFT", "CMODE", "CHOR", "FLNGR", "ENSMBL"];
      const TAPE_EDIT_NAMES = ["NOISE", "NRATE", "TAPWOB", "HISS", "WARP"];
      function formatModEditParam(idx, raw) {
        if (idx === 1) { // CMODE
          if (raw < 342) return "I";
          if (raw < 684) return "I+II";
          return "II";
        }
        return Math.round((raw * 100) / 1023) + "%";
      }
      // The TAPE group has no special case: every value shows 0..100%.
      function formatTapeEditParam(idx, raw) {
        return Math.round((raw * 100) / 1023) + "%";
      }

      // One slider per selectable parameter (instead of the hardware's
      // PARAM/VALUE knob pair). Moving slider i writes (i, raw) to the DSP
      // through the direct edit-parameter channel (Module.setEditParam) —
      // the (select_, PARAM_VAL) pair is applied atomically by the worklet
      // thread, the same write the hardware's unit_set_param_value() does.
      // `targetEl` is the column block the row is appended to (the 3-column
      // Parameters layout); the slider keeps its global id="editparam_<i>" so
      // the preset bank (applyPresetRaw) can still find it by index.
      function createEditableParam(wasmProcessor, index, targetEl) {
        let container = document.createElement("div");
        container.className = "flex items-center gap-3";

        let label = document.createElement("label");
        label.textContent = EDIT_PARAM_NAMES[index];
        label.className = "w-32 text-sm font-medium shrink-0 truncate";
        container.appendChild(label);

        let value_string = document.createElement("span");
        value_string.className = "badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right";
        // NOTE: value_string is appended to `container` at the END (after the
        // control) by each branch below -- the order is label, control, value
        // so the badge sits to the RIGHT of the slider / radio group. Do NOT
        // append it here: an earlier revision did, which moved every value
        // readout to the left of its control.

        const apply = (raw) => {
          value_string.textContent = formatParamValue(index, raw);
          // Direct channel: bypasses WebAudioParam smoothing. The worklet
          // thread applies (select_ = index, PARAM_VAL = raw) atomically on
          // its next block, so the DSP never sees an intermediate select_
          // value (the old path's phantom-LFO bug).
          if (Module.setEditParam) Module.setEditParam(index, raw);
          // Keep the Orbit "stable state" (C++ stable_value_) anchored to the
          // live values the user is actually hearing. A manual slider edit
          // IS a new stable state, so the next Orbit Random must develop from
          // the just-set values, not from the previous All/Part/Reset
          // snapshot. Guarded by getMorphActive(): while an All/Part/Orbit
          // morph is in flight (and this very apply() call is the
          // fireRandom poll writing the interpolated morph values), a sync
          // would drag stable_value_ along the glide and break the
          // C++-side stabilize at morph end (randomizeAll/Partial rewrite it
          // there anyway -- the authoritative write).
          if (Module.stableSync && Module.getMorphActive && Module.getMorphActive() === 0)
            Module.stableSync();
        };

        // ---- WAVE bank (index 13): radio group A..F, not a range slider ------
        // Same FlyonUI "join radio with btn" pattern as the HW Filter type
        // group: <div class="octave-join drop-shadow"> with one
        // <input type="radio" class="octave-join-item btn btn-soft
        // btn-primary btn-xs"> per bank; the visible text is the aria-label
        // (the flyonui .btn rule paints input[type=radio]:after). Clicking a
        // bank writes the raw through the SAME direct edit channel as the
        // sliders (apply -> setEditParam(13, raw)) — the DSP quantizes raw ->
        // bank. No #editparam_13 slider is created, so applyPresetRaw /
        // fireRandom special-case index 13 (they walk #editparam_<i> by
        // index). The radios are created AFTER apply (above) so the change
        // handlers close over it safely.
        if (index === WAVE_PARAM_IDX) {
          const group = document.createElement("div");
          group.className = "octave-join drop-shadow flex-1 min-w-0";
          const radios = [];
          WAVE_BANKS.forEach((bank, b) => {
            const radio = document.createElement("input");
            radio.type = "radio";
            radio.name = "WaveBank";
            radio.value = String(b);
            radio.className = "octave-join-item btn btn-soft btn-primary btn-xs flex-1 min-w-0";
            radio.setAttribute("aria-label", bank);
            radio.onchange = () => apply(rawFromWaveBank(b));
            group.appendChild(radio);
            radios.push(radio);
          });
          container.appendChild(group);
          // Remember the checked-state setter so the Rnd-morph poll and the
          // paced preset apply can re-check the radio whose bank the raw
          // quantizes to (they must NOT call apply — that would re-write the
          // value the morph/preset is already driving).
          setWaveBankChecked = (bank) => {
            const b = Math.max(0, Math.min(5, bank | 0));
            for (let i = 0; i < radios.length; i++) radios[i].checked = (i === b);
          };
          // No value_string for WAVE: the radio group IS the readout (the
          // checked button shows the selected bank letter). value_string is
          // created above but deliberately NOT appended here.
          const initRaw = (Module.getEditParamValue) ? Module.getEditParamValue(index) : 0;
          setWaveBankChecked(waveBankFromRaw(initRaw));
          (targetEl || document.getElementById("Parameters")).appendChild(container);
          return;
        }

        // ---- Default: one range slider per edit parameter --------------------
        const input = document.createElement("input");
        input.type = "range";
        input.min = 0;
        input.max = 1023;
        input.step = 1;
        input.id = "editparam_" + index;
        input.className = "range range-primary range-sm flex-1 min-w-0";
        container.appendChild(input);
        // value_string goes AFTER the slider so the badge sits to the RIGHT
        // of the range control (the original order: label, slider, value).
        container.appendChild(value_string);

        input.oninput = () => apply(parseInt(input.value, 10));

        // Initialize from the main-thread processor state (wasm.cc
        // getEditParamValue) — the value the DSP is currently using for this
        // parameter.
        const initRaw = (Module.getEditParamValue) ? Module.getEditParamValue(index) : 0;
        apply(initRaw);

        (targetEl || document.getElementById("Parameters")).appendChild(container);
      }

      // audio graph
      // let oscillator = new OscillatorNode(context, { frequency: 440.0 });
      let envelope = new GainNode(context, { gain: 1.0 }); // unity gain: the A-H-R envelope lives in the DSP (processor.noteOn/noteOff)
      // Master sim volume. Starts at 1.0 (= the VolumeSlider default, full
      // scale) so the simulator is as loud as the unit's DSP output; the
      // slider (value below) is what the user controls. The previous 0.1 /
      // 0.5 defaults made the sim read ~2x quieter than the device.
      let volume = new GainNode(context, { gain: 1.0 })
      let analyser = new AnalyserNode(context, { fftSize: 4096 });
      let data = new Float32Array(analyser.frequencyBinCount);

      // let noise = new AudioBufferSourceNode(context, { loop: true });
      // noise.buffer = context.createBuffer(1, context.sampleRate, context.sampleRate);
      // for (i = 0; i < context.sampleRate; i++) {
      //   noise.buffer.getChannelData(0)[i] = 2 * Math.random() - 1;
      // }

      wasmProcessor.connect(envelope).connect(volume).connect(context.destination);
      envelope.connect(analyser);

      // let samplesDiv = document.getElementById("Samples");
      // let audioElements = samplesDiv.getElementsByTagName("audio");
      // for (let audio of audioElements) {
      //   let source = context.createMediaElementSource(audio);
      //   source.connect(wasmProcessor);
      // }

      // Browsers start AudioContext in "suspended" state until a user
      // gesture, so playback is off by default. Resume automatically on
      // the first click/keypress so the user does not have to do anything
      // special to start playback.
      const autoResume = () => {
        if (context.state !== "running") {
          context.resume();
        }
        // keep the listener for further pauses (e.g. tab-switch)
      };
      document.addEventListener("click", autoResume, true);
      document.addEventListener("keydown", autoResume, true);

      // main volume
      let volumeSlider = document.getElementById("VolumeSlider");
      let volumeLabel = document.getElementById("VolumeLabel");
      volume.gain.value = volumeSlider.value; // expontial ramp do not work here, need proper smoothing
      volumeLabel.innerText = volumeSlider.value;
      volumeSlider.oninput = function () {
        volume.gain.value = this.value;
        volumeLabel.innerText = this.value;
      };

      // BPM (simulator-only tempo control). Delivered to the DSP via
      // Module.fx_set_bpm (wasm.cc -> Osc::setTempo -> tempo_bpm_). On
      // hardware the platform delivers BPM itself via unit_set_tempo(); this
      // slider only drives the web simulator. Default 120, range 56..240,
      // integer BPM (step 1).
      let bpmSlider = document.getElementById("BpmSlider");
      let bpmLabel = document.getElementById("BpmLabel");
      function applyBpm() {
        const v = parseInt(bpmSlider.value, 10);
        bpmLabel.innerText = v;
        if (Module.fx_set_bpm) Module.fx_set_bpm(v);
      }
      applyBpm();
      bpmSlider.oninput = function () {
        applyBpm();
      };

      // keyboard osc — explicit keyMap: 18 keys covering A2..D4
      // (11 white: A S D F G H J K L ; '  +  7 black: W R T U I O [).
      // Notes containing '#' are rendered as black keys. setKeyOctave()
      // (Z/X below) works as an octave shift (-2..+2) applied to the whole
      // range, relabeling the keys (A2..D4 -> A3..D5 etc.).
      //
      // keyCode values are the legacy physical QWERTY-US codes (independent
      // of the OS layout): 65..76 are the letter rows, 59=';', 222="'".
      // 219 ('-') is used for C#4 (black key) — kept as-is.
      // Do NOT use 186 ('+') here — it is NOT part of the piano row.
      let octaveShift = -1;
      // The keyboard starts one octave below the base A2..D4 range (A1..D3).
      // setOctaveShift() must be called explicitly after construction: the
      // radios' change listeners fire only on user interaction, and the
      // library itself applies no shift until told to.
      let keyboard = new QwertyHancock({
        id: "keyboard",
        // No width: the container is w-full (see #keyboard in the markup,
        // inside the Global card) and the library reads the computed width
        // once at init. The keyboard therefore resizes with the Global card.
        // 128 px: compact, so the whole top row (controls + keyboard + scope)
        // fits on one screen line at FHD. Black keys auto-scale to
        // height/1.5 (~85 px) in qwerty-hancock.
        height: 96,
        keyMap: {
          65: "A2", 87: "A#2", 83: "B2", 68: "C3", 82: "C#3",
          70: "D3", 84: "D#3", 71: "E3", 72: "F3", 85: "F#3",
          74: "G3", 73: "G#3", 75: "A3", 79: "A#3", 76: "B3",
          59: "C4", 219: "C#4", 222: "D4"
        },
        whiteKeyColour: "var(--kbd-white)",
        blackKeyColour: "var(--kbd-black)",
        // Theme-aware: --color-primary is set by the active flyonui theme
        // (gourmet/luxury etc.). var() works in inline styles and inherits
        // from <html>, so the highlight color updates automatically on theme
        // toggle with no JS intervention.
        activeColour: "var(--color-primary)"
      });

      let osc_frequency = 440.0;
      // Notes currently "held" (for the touch keyboard's virtual-hold quirk,
      // see the keyDown handler). Used to synthesize a note-off before a
      // re-press so the DSP's legato toggle-off (same key while sounding ->
      // fade out) can fire: the touch lib keeps a key "mouse-down" after a
      // touchstart, so a later touch of the same key fires no keyup/keydown
      // pair of its own.
      let pressedNotes = new Set();

      keyboard.keyDown = function (note, frequency) {
        // Touch keyboard quirk: pressing an already-"held" key (e.g. the
        // mouse is still down from the first touch) never produced a keyup
        // for it, so the DSP never saw noteOff and its legato toggle-off
        // (same note while sounding -> fade out) could not trigger. Detect
        // the re-press and synthesize the missing noteOff just before the
        // noteOn, restoring the press/release event pair.
        if (pressedNotes.has(note)) {
          Module.noteOff(note);
        }
        pressedNotes.add(note);

        if (!window.frequencyStack) {
          window.frequencyStack = [];
        }
        window.frequencyStack.push(frequency);
        osc_frequency = frequency;
        Module.setOscPitch(osc_frequency);
        Module.noteOn(note, 100); // TODO: add a slider for velocity
        // noteOn starts (or restarts) the A-H-R sequence: 2 s exp attack,
        // then hold at full level while the key is held.
      };

      keyboard.keyUp = function (note, frequency) {
        pressedNotes.delete(note);
        if (!window.frequencyStack) {
          window.frequencyStack = [];
        }
        // Remove the frequency from the stack (last matching entry)
        for (let i = window.frequencyStack.length - 1; i >= 0; --i) {
          if (approximatelyEqual(window.frequencyStack[i], frequency)) {
            window.frequencyStack.splice(i, 1);
            break;
          }
        }

        // latest note priority
        if (window.frequencyStack.length > 0) {
          osc_frequency = window.frequencyStack[window.frequencyStack.length - 1];
          Module.setOscPitch(osc_frequency);
        }
        // Note: no host-side mute on key-up. noteOff triggers the 2 s
        // exponential release; the sound fades out on its own.

        Module.noteOff(note);
      };

      // Octave switching: radio group (the "Octave" buttons below the
      // keyboard, FlyonUI "Join radio with btn style") + Z/X hotkeys. Both go
      // through setOctaveShift(), so whichever one fires, the other is
      // updated. The buttons are labeled 1..5 (button N = base A2..D4 shifted
      // by N-3, i.e. 1=A0..D2 .. 5=A4..D6); internally octaveShift stays on
      // the -2..+2 scale (keys are relabeled, count and layout stay the same).
      const OCTAVE_BTN_MIN = 1;
      const OCTAVE_BTN_MAX = 5;
      const octaveRadios = Array.from(document.querySelectorAll('input[name="Octave"]'));
      function setOctaveShift(value, source) {
        value = Math.max(-2, Math.min(2, Math.round(value)));
        if (source !== "hotkey") {
          // Radio click: keep the checked state (the browser already did it)
          // and apply the shift.
        } else {
          // Z/X hotkey: check the matching radio so the button group reflects
          // the current octave (radio values are the 1..5 button numbers).
          const r = octaveRadios.find((x) => x.value === String(value + 3));
          if (r) r.checked = true;
        }
        octaveShift = value;
        keyboard.setKeyOctave(octaveShift);
      }
      octaveRadios.forEach((r) => {
        r.addEventListener("change", () => {
          // Radio values are the 1..5 button numbers; shift = value - 3.
          const btn = Math.max(OCTAVE_BTN_MIN, Math.min(OCTAVE_BTN_MAX, parseInt(r.value, 10)));
          setOctaveShift(btn - 3, "radio");
        });
      });
      // Apply the initial shift (keyboard starts at -1, i.e. A1..D3 = button
      // 2) to the just-built keyboard: relabels the keys and syncs with the
      // checked radio in the markup (see the value="2" checked attribute above).
      setOctaveShift(octaveShift, "radio");

      // Z/X shift the whole range by one octave.
      // Uses keyCode (physical key position) so it works on any keyboard
      // layout (same approach as the note hotkeys). 90 = Z, 88 = X.
      document.addEventListener("keydown", (e) => {
        if (e.keyCode === 90) {
          setOctaveShift(octaveShift - 1, "hotkey");
        } else if (e.keyCode === 88) {
          setOctaveShift(octaveShift + 1, "hotkey");
        }
      });

      // scope (guarded: canvases may be absent in headless / minimal DOM;
      // the rest of setupWebAudioAndUI must still run so sliders/presets
      // are created even if the scope cannot start).
      let canvasWave = document.getElementById("TimeDomain");
      let canvasFFT = document.getElementById("FrequencyDomain");
      let canvasWaveContext = canvasWave ? canvasWave.getContext("2d") : null;
      let canvasFFTContext = canvasFFT ? canvasFFT.getContext("2d") : null;

      // Theme-aware drawing colors, cached outside the per-frame loop. app.css
      // exposes --scope-bg/--scope-fg on .scope-canvas (mapped to the active
      // flyonui theme's base-100/base-content); we resolve them once and
      // refresh on <html> data-theme changes (MutationObserver), so the trace
      // and histogram stay readable on every theme at zero per-frame cost.
      let scopeBg = "";
      let scopeFg = "";
      function refreshScopeColors() {
        if (!canvasWave) return;
        const s = getComputedStyle(canvasWave);
        scopeBg = s.getPropertyValue("--scope-bg").trim() || "#fff";
        scopeFg = s.getPropertyValue("--scope-fg").trim() || "#000";
      }
      refreshScopeColors();
      if (canvasWave) {
        new MutationObserver(refreshScopeColors).observe(document.documentElement, {
          attributes: true,
          attributeFilter: ["data-theme"],
        });
      }

      // Vertical gain for the time-domain trace. The analyser is tapped at the
      // unity-gain `envelope` node, i.e. it sees the RAW DSP output (before the
      // user-controlled `volume` master), so getFloatTimeDomainData values are
      // on the same ~0..1 scale as the unit's audio path (a full-scale tone
      // reads v ≈ ±1.0, a typical drone ≈ ±0.3..±0.9). WAVE_GAIN scales that
      // to the canvas height (128 px); the trace is hard-clamped to [0, H] so
      // it can never leave the canvas even when the DSP peaks near 1.0.
      const WAVE_GAIN = 1.0;

      if (canvasWaveContext && canvasFFTContext) {
        draw();
      }
      function draw() {
        if (!canvasWaveContext || !canvasFFTContext) return;
        analyser.getFloatTimeDomainData(data);

        canvasWaveContext.fillStyle = scopeBg;
        canvasWaveContext.fillRect(0, 0, canvasWave.width, canvasWave.height);

        // Skip the trace entirely when the signal is inaudible (-60 dBFS
        // threshold): otherwise the flat zero-line shows up as a stray
        // horizontal stripe across the middle of the canvas.
        let peak = 0;
        for (let i = 0; i < data.length; i++) {
          const a = Math.abs(data[i]);
          if (a > peak) peak = a;
        }
        if (peak < 1e-3) {
          canvasFFTContext.clearRect(0, 0, canvasFFT.width, canvasFFT.height);
          requestAnimationFrame(draw);
          return;
        }

        canvasWaveContext.lineWidth = 1.5;
        canvasWaveContext.strokeStyle = scopeFg;
        canvasWaveContext.beginPath();

        // find trigger point
        let start_index = 0;
        let score_max = 0.0;

        let dx = Math.floor(400 / osc_frequency) + 1;

        for (let i = dx; i < data.length / 2; ++i) {
          if ((data[i - 1] > 0) && (data[i + 1] < 0)) { // zero-cross point
            let score = (data[i - dx] - data[i + dx]) - data[i + 5 * dx];
            if (score > score_max) {
              score_max = score;
              start_index = i;
            }
          }
        }

        let num_of_half_cycles = Math.floor(analyser.fftSize * osc_frequency / context.sampleRate) - 1;
        let end_index = data.length - 1;
        if (num_of_half_cycles > 0) {
          end_index = Math.round(start_index + num_of_half_cycles * context.sampleRate / osc_frequency / 2);
        }

        let delta_x = canvasWave.width / (end_index - start_index);
        let x = 0;

        for (let i = start_index; i <= end_index; i++) {
          let v = data[i];
          // Center on the mid-line, invert Y (positive v -> up, like a real
          // scope), apply WAVE_GAIN, and hard-clamp so the trace can never
          // leave the canvas.
          let y = canvasWave.height / 2 - v * WAVE_GAIN * (canvasWave.height / 2);
          if (y < 0) y = 0;
          if (y > canvasWave.height) y = canvasWave.height;

          if (i === start_index) {
            canvasWaveContext.moveTo(x, y);
          } else {
            canvasWaveContext.lineTo(x, y);
          }

          x += delta_x;
        }
        canvasWaveContext.stroke();

        analyser.getFloatFrequencyData(data);

        canvasFFTContext.clearRect(0, 0, canvasFFT.width, canvasFFT.height);

        let deltaX = canvasFFT.width / data.length;
        let posX = 0;
        for (let i = 0; i < data.length; i++) {
          let barHeight = data[i] + canvasFFT.height;
          canvasFFTContext.fillStyle = scopeFg;
          canvasFFTContext.fillRect(
            posX,
            canvasFFT.height - barHeight,
            deltaX,
            barHeight,
          );
          posX += deltaX;
        }
        requestAnimationFrame(draw);
      }

      // Build the 3-column Parameters layout: each PARAM_GROUPS entry becomes
      // a titled block inside its assigned column (col set per group, see
      // PARAM_GROUPS). All 30 sliders are created exactly once, in their
      // select_ (edit_) index order; the per-slider global ids
      // ("editparam_<i>") let the preset bank address them by index.
      {
        const paramColumns = [
          document.getElementById("ParamCol0"),
          document.getElementById("ParamCol1"),
          document.getElementById("ParamCol2"),
        ];
        for (let g = 0; g < PARAM_GROUPS.length; ++g) {
          // Each group carries an explicit `col` (0/1/2) so the layout is a
          // true column BLOCK, not a round-robin:
          //   col 0: TIMBRE, TIMBRE FILTER, BREATH
          //   col 1: PITCH, WANDER
          //   col 2: ANCHORS, TEXTURE
          const col = paramColumns[PARAM_GROUPS[g].col];
          // Wrap each group's heading + sliders in one flex column so that the
          // column-level justify-between (on the shorter col 0 / col 2) treats
          // the whole group as a single item: heading stays glued to its
          // sliders, and the group as a block is pushed to its column edge.
          const group = document.createElement("div");
          // WANDER is the bottom block of col 1. col 1 is top-aligned (plain
          // flex-col), so without a hook it would sit just under PITCH, leaving
          // a gap at the card's bottom edge. `mt-auto` (margin-top: auto) is
          // the idiomatic way to push a single flex item to the bottom of a
          // full-height flex column (the column is stretched to the grid's full
          // height by items-stretch). If col 1 ends up being the tallest column
          // the auto margin collapses to 0, so it degrades gracefully.
          group.className = (PARAM_GROUPS[g].className
            ? PARAM_GROUPS[g].className + " "
            : "") + "flex flex-col gap-2";
          const heading = document.createElement("div");
          heading.className = "text-xs uppercase tracking-wide opacity-60 mt-1";
          heading.textContent = PARAM_GROUPS[g].title;
          group.appendChild(heading);
          for (const idx of PARAM_GROUPS[g].items) {
            createEditableParam(wasmProcessor, idx, group);
          }
          col.appendChild(group);
        }
      }

      // ---- Preset bank -------------------------------------------------------
      // One-click parameter banks (simulator-only, like the BPM slider). A
      // preset is a full 30-parameter raw array in EDIT_PARAM_NAMES order.
      // Applying one MUST be paced: the DSP's direct edit channel
      // (simEditRequest) has a SINGLE pending slot, and the audio worklet
      // consumes at most one request per block (~128 samples @ 44.1 kHz ≈ 2.9
      // ms). A synchronous burst of 30 setEditParam calls would let each write
      // overwrite the previous one before the worklet reads it, dropping ~29
      // parameters. So we spread the writes ~8 ms apart (3 audio blocks of
      // headroom each) and let each one be consumed. Total apply time ≈ 240 ms
      // (a short, smooth morph from the previous sound).
      // Two banks only: Random (randomize the wandering streams) and Reset
      // (always restore the factory defaults, osc.h edit_value_ initializers).
      const PRESET_RANDOM = "All Random";
      const PRESET_RESET = "Reset";
      // Factory defaults = the edit_value_ initializers in osc.h (kEditParamFirst
      // order, RATE-first): RATE 0, DETU 511, SPD1 511, SMT1 511, SPD2 511, SMT2 511,
      // SPD3 511, SMT3 511, SUB1 511, SUB2 410 (~40%), SATU 341, ASYM 511, FOLD 0,
      // WAV1 0, WAV2 128, WAV3 0, CHRD 205, CINT 377, MAIN 1023 (full),
      // RWAV 0, RDRV 0, RFLD 0, FCUT 921, RES 0, RWFC 0,
      // RVLL 0, RVLS 511, RVLSM 511, CHSAT 341 (chord drive = SATU default),
      // CHFLD 0 (chord fold off, = FOLD default), ..., FMVT 511, FLVL 0, COMB 0.
      // (FOLD2 / DCK removed — 44 params total.)
      const DEFAULT_RAW = [0, 511, 511, 511, 511, 511, 511, 511, 511, 410, 341, 511, 0, 0, 128, 1023, 0, 0, 0, 921, 0, 0, 0, 511, 511, 0, 0, 0, 0, 0, 0, 511, 0, 0, 0];

      // Full randomization of all 30 parameters. "Safe" (walk) parameters get
      // the full 0..1023 range; "critical" parameters are constrained to
      // audition-friendly windows so Random always yields a playable sound
      // (no clipping, no mud, no dissonance, no resonance spikes). The raw
      // windows below are the exact inverse of formatParamValue (websim) /
      // Osc::getParameterStrValue (osc.h), so they line up with the slider
      // display. See the per-parameter comments for the DSP rationale.
      function randomizeAllRaw() {
        const raws = new Array(EDIT_PARAM_COUNT).fill(0);
        const I = (name) => EDIT_PARAM_NAMES.indexOf(name);
        // Guarded setter: `I(name)` returns -1 if the name is missing from
        // EDIT_PARAM_NAMES (a silent no-op / out-of-bounds write before).
        // Failing loudly in the console means a name/index mismatch can never
        // again leave a slider silently stuck at 0 (the "Random doesn't move
        // X" bug). Behavior is identical for every correctly-named param.
        const set = (name, raw) => {
          const i = I(name);
          if (i < 0 || i >= raws.length) { console.warn("randomRaw: unknown param", name); return; }
          raws[i] = raw;
        };
        const r1023 = () => Math.floor(Math.random() * 1024); // 0..1023
        const rRange = (lo, hi) => lo + Math.floor(Math.random() * (hi - lo + 1)); // inclusive

        // --- Full-range (walk / timbre) parameters ---------------------------
        // SPD1..3 / SMT1..3: stream period / time constant (0.1 s..10 s log,
        // 10 ms..1.5 s log). Any value is musically safe.
        for (const n of ["Tone Rate", "Tone Smooth", "Timbre Rate", "Timbre Smooth", "Density Rate", "Density Smooth"])
          set(n, r1023());
        // RWAV / RDRV / RFLD / RWFC: per-stream random-walk depths (0..100%).
        // Full range — they only scale the wander, never the base sound.
        for (const n of ["Morph Wander", "Drive Wander", "Fold Wander", "Cutoff Wander"])
          set(n, r1023());
        // WAVE (WAV1): the selected WAVETABLE BANK (A..F, 0..5). The LFO then
        // morphs within the bank (intra-bank position centered on the bank,
        // see osc_waves.h), so each of the 6 banks is a valid, clearly
        // different timbre family — randomize uniformly over them. (WAV2 is
        // deprecated: both A/B read the same wave.)
        {
          const bank = Math.floor(Math.random() * 6); // 0..5
          set("Wave", rawFromWaveBank(bank));
        }

        // --- Constrained (critical) parameters -------------------------------
        // DETU: static A/B detune, +/-kDetuneMaxCents (10 c) centered on 511.
        // Window +/-5 c (raw 256..767) — a clear beat without going out of tune.
        set("Detune", rRange(256, 767));
        // RATE: shared walk depth ceiling (0..100%, quadratic). It scales
        // EVERY wandering stream (detune, morph, drive, fold, cutoff, XFM),
        // so it is the single loudest "chaos" lever in the unit. Random over
        // 20..50% (raw 204..511) — a "safe" barrier: below ~20% the wander is
        // too subtle to hear, above ~50% the combined (quadratic-mapped)
        // depth of all three streams plus the full-range RWAV/RDRV/RFLD/RWFC
        // depths tends to clip / howl (the old 20..100% window was the source
        // of the "overwhelming" random presets). 50% keeps the drone
        // breathing without going nuclear.
        set("Rate", rRange(204, 511));
        // SUB1: sub-octave (f/2) sine. 20..80% — the bass is ALWAYS audible
        // (the low end is the drone's anchor); only the depth varies.
        set("Sub 1", rRange(204, 818));
        // SUB2: second sub (f/4), LPF'd + gated. 25..70% — the bass is ALWAYS
        // audible; a light-to-present low-end, never silent, never the f/4 "mud".
        set("Sub 2", rRange(256, 716));
        // SATU: saturator drive, 0..kSatuMax (3.0) => display 0..300. Window
        // 40..240 (drive 0.4..2.4) — audible drive, no near-clip.
        set("Drive", rRange(136, 819));
        // ASYM: asymmetric saturator, -100..+100% centered on 511. Window
        // +/-50% (raw 256..767) — moderate asymmetry.
        set("Asym", rRange(256, 767));
        // FOLD: pre-drive wavefolder depth (0..1, multiplicative). 0..60%
        // (raw 0..613) — metallic character possible, no "metal mush".
        set("Fold", rRange(0, 613));
        // (CHRD/CINT/CINT2/CHSAT/CHFLD/CHSAT2/CHFLD2 were removed — the
        //  chord/interval concept is gone.)
        // MAIN: main (A+B) voice level (0..100%). Window 55..100%
        // (raw 563..1023) — the main voice stays clearly present (never fully
        // ducked, which would leave only subs/chord and sound "hollow"), but
        // can be dialed down a few dB so the subs/chord have more room.
        set("Main", rRange(563, 1023));
        // FCUT: SVF cutoff, log 80 Hz..12 kHz. Window 1 kHz..11 kHz
        // (raw 740..1023) — keeps the filter in its working band, never
        // "in a box" (low) or fully open (high, where the walk does nothing).
        set("Cutoff", rRange(740, 1023));
        // RES: SVF resonance Q, 0.5..2.5. Window 0.5..1.8 (raw 0..669) —
        // gentle peak, no ringy spike.
        set("Resonance", rRange(0, 669));
        // RVLL: breath depth on the four voice levels (0..100%). Window
        // 20..80% (raw 204..818) — audible voice-trading without any voice
        // fully vanishing (a full 0% would silence a voice, which reads as a
        // glitch, not a "breath"). The depth is still gated by RATE (the
        // shared walk ceiling), so the effective breathing is gentler than the
        // raw window suggests.
        set("Breath Depth", rRange(204, 818));
        // RVLS: breath stream period (0.1 s..10 s, log). Full range — a slow
        // breath (10 s) or a lively one (0.1 s) are both musically usable.
        set("Breath Rate", r1023());
        // RVLSM: breath stream time constant (10 ms..1.5 s, log). Full range.
        set("Breath Smooth", r1023());
        // (Chord Drive / Chord Fold / Chord 2 Drive / Chord 2 Fold were removed —
        //  the chord/interval concept is gone.)
        // ---- Enrichment params (formerly left at 0 — now randomized safely) ----
        // JIT: random phase jitter on A/B (0..100% -> 0..0.5% cycle peak).
        // Window 0..15% (raw 0..153) — a light "smear" of the carriers into a
        // dense partial, never the harsh "static noise" that full JIT produces.
        set("Jitter", rRange(0, 153));
        // RMOD: A*B ring-mod parallel voice (0..100%). Window 0..25%
        // (raw 0..256) — a gentle metallic "ring"/beating on top of the A+B
        // tone, not the harsh full-level ring-mod that masks the drone.
        set("Ring Mod", rRange(0, 256));
        // FM A / FM B: static phase-FM (quadratic curve -> 0..kFmMaxCents/2
        // cycles at full). After the FM fix, 100% = 0.05 cycle peak index.
        // Window 0..30% (raw 0..307): peak index ~0.009 c — a clearly-audible
        // "color" without the old "screech/whistle" that full FM produced.
        set("FM A", rRange(0, 307));
        set("FM B", rRange(0, 307));
        // RFM A / B: random-walk DEPTH on the FM index (0..100%, RATE-gated).
        // Window 0..50% (raw 0..511) — the FM "breathes" with the walk on top
        // of the static sidebands. Dead when FM=0 (the walk modulates a zero
        // index), but since FM A/B are randomized 0..307 above, RFM will
        // usually have something to breathe on. Still gentle thanks to the
        // RATE ceiling (20..50%) and the quadratic FM curve.
        set("FM A Bre", rRange(0, 511));
        set("FM B Bre", rRange(0, 511));
        // ---- Enrichment-2 (formant / comb) ----
        // (FOLD2 and DCK were removed — FOLD2 judged unnecessary, DCK is now a
        //  constant (kDuckConstant).)
        // FORM: any vowel (full range) -- the formant stage is OFF until FVOL>0,
        // so this is musically free (no clipping).
        set("Formant Vowel", r1023());
        // FVOL: 0..50% (raw 0..512) -- a clear formant color, never the
        // full-level resonant biquad sum that would mask the drone.
        set("Formant Level", rRange(0, 512));
        // COMB: 0..30% (raw 0..307) -- a gentle resonance, never the ringy
        // fb>0.7 zone that "rings" harshly.
        set("Comb Res", rRange(0, 307));
        return raws;
      }

      // Backward-compat alias (All Random button / any existing callers).
      function randomRaw() { return randomizeAllRaw(); }

      // Partial randomization (the "Partial Random" button): a "development"
      // around the CURRENT sound. Mirrors the device's RndM=Part behaviour
      // (Osc::randomizePartial) but runs on the UI thread so the sliders
      // visibly move (the same applyPresetRaw path All Random uses, so the
      // worklet receives every write through the paced direct channel).
      //
      // Mechanics:
      //   1. Read the current slider values (document.getElementById("editparam_<i>"))
      //      as the BASE -- this is what the user is currently hearing.
      //   2. Generate a fresh randomizeAllRaw() array (the same safe windows
      //      All Random uses, so the draw distribution is identical).
      //   3. Pick a RANDOM SUBSET of 2..6 edit indices (no duplicates).
      //   4. For each chosen index, copy the fresh value from the array, BUT
      //      re-draw up to 5 times if the step |fresh - current| < 102 raw
      //      (~10% of the 10-bit range) -- a "significant" audible change,
      //      matching Osc::randomizePartial's kRndMinStepRaw on the device.
      //   5. Return the modified base array (unchanged params keep their
      //      current value; only the 2..6 picked ones get fresh draws).
      //
      // The result is applied through applyPresetRaw() below: it writes each
      // value to the slider DOM (so the UI moves) AND to the DSP via the
      // paced setEditParam direct channel (one per 8 ms, the same morph
      // All Random uses). Sliders and sound stay in lockstep.
      function partialRandomRaw() {
        // 1. Read current slider values as the base.
        const base = new Array(EDIT_PARAM_COUNT).fill(0);
        for (let i = 0; i < EDIT_PARAM_COUNT; ++i) {
          const el = document.getElementById("editparam_" + i);
          base[i] = el ? parseInt(el.value, 10) : 0;
        }
        // 2. Fresh random values for EVERY parameter (same safe windows as
        //    All Random, so the draw distribution is identical).
        const fresh = randomizeAllRaw();
        // 3. Pick a random SUBSET of 2..6 indices (no duplicates).
        const count = 2 + Math.floor(Math.random() * 5); // 2..6
        const picked = new Set();
        while (picked.size < count) {
          picked.add(Math.floor(Math.random() * EDIT_PARAM_COUNT));
        }
        // 4. For each picked index, take the fresh value; if the step from the
        //    current value is too small (< 102 raw, ~10%), re-draw up to 5
        //    times. After 5 tries accept the last draw (a small step is better
        //    than no step -- the same fallback as Osc::randomizePartial).
        const MIN_STEP = 102; // ~10% of 1024, matches kRndMinStepRaw in osc_randomize.h
        for (const idx of picked) {
          let v = fresh[idx];
          let step = Math.abs(v - base[idx]);
          for (let t = 0; t < 5 && step < MIN_STEP; ++t) {
            // Re-draw just this one parameter by regenerating the full array
            // and taking the new draw for idx (the other draws are discarded,
            // which is cheap -- randomizeAllRaw is pure Math.random, no DSP).
            v = randomizeAllRaw()[idx];
            step = Math.abs(v - base[idx]);
          }
          base[idx] = v;
        }
        // (CINT/CINT2 separation was removed — the chord/interval concept is gone.)
        return base;
      }

      let presetApplyTimer = null;
      function applyPresetRaw(raws, presetLabel) {
        if (context.state !== "running") context.resume();
        if (presetApplyTimer) clearTimeout(presetApplyTimer);
        // Deactivate all preset buttons (FlyonUI btn-active class).
        document.querySelectorAll("#Presets button").forEach((b) => b.classList.remove("btn-active"));
        const me = document.querySelector('#Presets button[data-name="' + presetLabel + '"]');
        if (me) me.classList.add("btn-active");
        // Paced application: one setEditParam every 8 ms (see the block above).
        let i = 0;
        (function step() {
          if (i < raws.length) {
            const id = "editparam_" + i;
            const el = document.getElementById(id);
            if (i === WAVE_PARAM_IDX) {
              // WAVE bank: no #editparam_13 slider exists (it is the A..F
              // radio group) — re-check the radio for the bank this raw
              // quantizes to; the setEditParam below still applies the raw.
              if (setWaveBankChecked) setWaveBankChecked(waveBankFromRaw(raws[i]));
            } else if (el) {
              el.value = raws[i];
              const span = el.parentElement.querySelector("span");
              if (span) span.textContent = formatParamValue(i, raws[i]);
            }
            if (Module.setEditParam) Module.setEditParam(i, raws[i]);
            ++i;
            presetApplyTimer = setTimeout(step, 8);
          } else {
            presetApplyTimer = null;
            // All/Partial/Reset applied through this UI-thread path bypass the
            // audio re-roll that would stabilize on its own -- sync the C++
            // Orbit "stable state" snapshot with what the sim is showing now.
            if (Module.stableSync) Module.stableSync();
          }
        })();
      }

      // Fire an audio-worklet re-roll in the given RndM type (0 = All,
      // 1 = Part, 2 = Orbit) and poll the main-thread processor mirror until
      // the sliders settle on the new values.
      //
      // Why the poll: in the web sim the main-thread and the audio-worklet
      // processor are SEPARATE COPIES (see wasm.cc). rndTrigger() fires on the
      // worklet copy; the UI reads the main-thread copy, which the worklet
      // mirrors into shared state on each apply. So after a fire we poll
      // getEditParamValue() every frame (rAF) and move the slider + span when
      // its value changed.
      //
      // The re-roll is now a BPM-synced MORPH (Osc::rndMorphStart /
      // rndMorphTick): instead of a single jump, each affected parameter
      // glides from its old value to the new one over a 4-bar phrase (8 s at
      // the default 120 BPM) with a random per-parameter start delay (stagger)
      // and a smoothstep curve. getEditParamValue() returns the CURRENT
      // interpolated value while the morph is in flight (see wasm.cc), so the
      // poll below makes the sliders travel with the morph in real time. The
      // sync window is therefore the morph length + the max stagger + a margin:
      // 4 bars @ 120 BPM = 8 s, plus ~8 s of stagger (64 subdivisions), plus a
      // 2 s settle margin = ~18 s (kRndMorphBeats/kRndMorphStaggerDivs in
      // osc_constants.h). The poll stops early (the "dirty" check) as soon as
      // no value changes between two frames, which usually happens well before
      // the 18 s hard timeout.
      let fireSyncTimer = null;
      // Sliders currently highlighted as "in motion" during a morph (their
      // thumb/track is recolored via the plain "range" class -- which uses
      // --range-color: var(--color-base-content), a lighter neutral -- so the
      // user can see which parameters are still traveling toward their re-roll
      // target. The default class is "range range-primary" (accent color);
      // during the morph the accent is stripped and the lighter neutral shows.
      // Cleared when the morph finishes (or on the hard timeout).
      const morphingSliders = new Set();
      function clearMorphHighlights() {
        for (const el of morphingSliders) {
          el.classList.add("range-primary");
        }
        morphingSliders.clear();
      }
      function highlightSlider(i) {
        // The WAVE radio group has no slider to recolor (index 13 is rendered
        // as A..F radios, not a range input) — skip the highlight.
        if (i === WAVE_PARAM_IDX) return;
        const el = document.getElementById("editparam_" + i);
        if (el && !morphingSliders.has(el)) {
          el.classList.remove("range-primary");
          morphingSliders.add(el);
        }
      }
      function fireRandom(mode) {
        if (fireSyncTimer) clearInterval(fireSyncTimer);
        Module.setRndMode(mode);
        Module.rndTrigger();
        const t0 = performance.now();
        const MORPH_SYNC_MS = 5000; // 1-bar morph (2 s @120) + max stagger (~2 s) + 1 s margin
        fireSyncTimer = setInterval(() => {
          let dirty = false;
          let lastWaveBank = -1; // WAVE group: bank last seen by the poll
          for (let i = 0; i < EDIT_PARAM_COUNT; ++i) {
            const v = Module.getEditParamValue(i);
            const el = document.getElementById("editparam_" + i);
            if (i === WAVE_PARAM_IDX) {
              // WAVE bank: no #editparam_13 slider exists (it is the A..F
              // radio group). Re-check the radio whose bank the morphing raw
              // currently quantizes to, so the group follows the morph the
              // same way the sliders do (checked flips as the raw crosses
              // the bank boundaries). No setEditParam — the morph owns the
              // write stream. dirty=true only on an actual bank change, so
              // the poll's "nothing changed this tick" stop condition still
              // works.
              if (setWaveBankChecked) {
                const bank = waveBankFromRaw(v);
                if (lastWaveBank !== bank) {
                  lastWaveBank = bank;
                  setWaveBankChecked(bank);
                  dirty = true;
                }
              }
              continue;
            }
            if (el && parseInt(el.value, 10) !== v) {
              el.value = v;
              const span = el.parentElement.querySelector("span");
              if (span) span.textContent = formatParamValue(i, v);
              dirty = true;
              // Recolor the slider so the user can see which parameters are
              // still traveling toward their re-roll target (the morph is
              // BPM-synced with per-parameter stagger, so different sliders
              // light up at different times across the 4-bar phrase).
              highlightSlider(i);
            }
          }
          // Stop only when the morph is DONE (getMorphActive() === 0) AND no
          // value changed this tick, OR on the hard timeout. The stagger
          // window (up to kRndMorphStaggerDivs-1 = 63 subdivisions of the
          // phrase, ~8 s at 120 BPM) can be longer than a single 33 ms tick,
          // so a "no value changed this frame" tick during the stagger would
          // otherwise stop the poll prematurely and the sliders would never
          // move even though the worklet's morph was still running. The
          // morph-active check keeps the poll alive until rndMorphTick
          // disarms morph_active_ (all parameters have reached their targets).
          const morphDone = Module.getMorphActive === undefined
            ? true
            : (Module.getMorphActive() === 0);
          if ((dirty === false && morphDone) || performance.now() - t0 > MORPH_SYNC_MS) {
            clearInterval(fireSyncTimer);
            fireSyncTimer = null;
            // The morph is finished (or timed out): restore the default
            // slider colors for every parameter that was highlighted.
            clearMorphHighlights();
          }
        }, 33);
      }

      // Build the preset buttons (after the sliders exist, so apply can touch
      // them). All styled like Reset (btn btn-outline btn-primary) and slightly
      // smaller (btn-sm):
      //   * "All Random"     -- re-roll EVERY parameter to a fresh audition-
      //                         friendly preset. Runs on the AUDIO worklet
      //                         thread via fireRandom(0) -> Module.setRndMode(0)
      //                         + Module.rndTrigger() -- the same Osc::rndArm /
      //                         rndTick / randomizeAll path the device uses.
      //                         The re-roll is a BPM-synced MORPH (4 bars @
      //                         current tempo, per-param stagger + smoothstep),
      //                         and the fireSyncTimer poll makes the sliders
      //                         travel with the morph in real time.
      //   * "Partial Random" -- re-roll a RANDOM SUBSET (2..6) of parameters to
      //                         fresh safe-window values, leaving the rest (a
      //                         "development" around the current sound). Each
      //                         changed param gets a significant step (>=102
      //                         raw). Runs on the AUDIO worklet thread via
      //                         fireRandom(1) -> Osc::randomizePartial -> the
      //                         same morph engine.
      //   * "Orbit Random"   -- a TIGHT, SHORT development of the LAST STABLE
      //                         state (defaults at startup, or the result of
      //                         the last All/Partial re-roll): nudge 2..4
      //                         parameters, each from ONE sphere (harmony /
      //                         timbre / motion) and by a clear step (~6..30%
      //                         of range), morphed over a short quarter-bar
      //                         phrase so the shift lands quickly. Stays
      //                         recognizably the same sound, developed. Does
      //                         not create a new stable state. Runs on the
      //                         AUDIO worklet thread via fireRandom(2) ->
      //                         Osc::randomizeOrbi.
      //   * "Reset"          -- always restore the factory defaults. Instant
      //                         (no morph): this is a "snap to the known good"
      //                         action, so it uses the UI-thread
      //                         applyPresetRaw(DEFAULT_RAW) path (one
      //                         setEditParam per 8 ms) and finishes with
      //                         Module.stableSync() so the C++ Orbit reference
      //                         (stable_value_) matches the freshly-reset sound.
      const presetGrid = document.getElementById("Presets");
      {
        const b = document.createElement("button");
        b.textContent = PRESET_RANDOM; // "All Random"
        b.className = "btn btn-outline btn-primary btn-sm";
        b.title = "Re-roll EVERY parameter to a fresh audition-friendly preset (BPM-synced morph).";
        b.dataset.name = PRESET_RANDOM;
        b.onclick = () => fireRandom(0);
        presetGrid.appendChild(b);
      }
      {
        const b = document.createElement("button");
        b.textContent = "Partial Random";
        b.className = "btn btn-outline btn-primary btn-sm";
        b.title = "Re-roll a random SUBSET (2..6) of parameters to fresh safe values (a development around the current sound). BPM-synced morph.";
        b.dataset.name = "Partial Random";
        b.onclick = () => fireRandom(1);
        presetGrid.appendChild(b);
      }
      {
        const b = document.createElement("button");
        b.textContent = "Orbit Random";
        b.className = "btn btn-outline btn-primary btn-sm";
        b.title = "Nudge 2..4 parameters from ONE sphere (harmony / timbre / motion) of the LAST STABLE state by a clear step (~6..30% of range), morphed over a short quarter-bar phrase -- a development that stays recognizably the same sound but lands quickly. Does not create a new stable state.";
        b.dataset.name = "Orbit Random";
        b.onclick = () => fireRandom(2);
        presetGrid.appendChild(b);
      }
      {
        const b = document.createElement("button");
        b.textContent = PRESET_RESET;
        b.className = "btn btn-outline btn-primary btn-sm";
        b.title = "Always restore the factory default parameters (instant, no morph).";
        b.dataset.name = PRESET_RESET;
        b.onclick = () => applyPresetRaw(DEFAULT_RAW.slice(), PRESET_RESET);
        presetGrid.appendChild(b);
      }

      // LFO (Shape) controls — simulator-only, mirrors the hardware OSC knobs
      const lfoFreq = document.getElementById("LfoFreq");
      const lfoDepth = document.getElementById("LfoDepth");
      const lfoFreqLabel = document.getElementById("LfoFreqLabel");
      const lfoDepthLabel = document.getElementById("LfoDepthLabel");

      function updateLfoFreq() {
        const v = parseFloat(lfoFreq.value);
        lfoFreqLabel.textContent = v.toFixed(2);
        Module.setLfoFreq(v);
      }
      function updateLfoDepth() {
        const v = parseInt(lfoDepth.value, 10);
        lfoDepthLabel.textContent = v + "%";
        Module.setLfoDepth(v / 100.0);
      }

      lfoFreq.addEventListener("input", updateLfoFreq);
      lfoDepth.addEventListener("input", updateLfoDepth);

      // initialize (defaults: F=1 Hz, S=0% = LFO off)
      updateLfoFreq();
      updateLfoDepth();

      // ---- HW FILTER (web-sim only, between OSC and MOD) -------------------
      // Emulates the physical NTS-1 filter between the OSC module and the MOD
      // module (the SDK only exposes unused cutoff / resonance placeholder
      // fields in the OSC runtime context, so the simulator builds its own
      // block: see drone-chain/hwfilter.h).
      //
      // Controls:
      //   - TYPE : radio join (Off / HP 4 / HP 2 / BP 4 / BP 2 / LP 4 / LP 2),
      //            default Off (the block is out of the path).
      //   - CUTOFF : raw 0..1023, log-mapped 1 Hz .. 20 kHz (hwfilter.h).
      //   - RES    : raw 0..1023, linearly mapped Q 0.5 .. 2.5.
      //   - SWEEP F : 0..30 Hz (step 0.01), default 0 = sweep off.
      //   - SWEEP AMT : -100..+100 (UI), default 0. Raw = (amt+100)*5.115.
      //
      // All writes go through Module.setHwFilterType / setHwFilterParam
      // (wasm.cc -> Chain::hwflt_). The worklet thread applies them on its
      // next block (hwflt_.apply() at the top of hwflt_.process).
      {
        // Format the raw 10-bit cutoff value (0..1023) as Hz / kHz.
        // Mirrors HwFilter::cutoffHz: fc = 1 * (20000/1)^(raw/1023).
        function formatHwCutoff(raw) {
          const fc = 1.0 * Math.pow(20000.0 / 1.0, raw / 1023);
          if (fc < 1000) return Math.round(fc) + " Hz";
          return (fc / 1000).toFixed(2) + " kHz";
        }

        // Format the raw 10-bit resonance value (0..1023) as Q.
        // Mirrors HwFilter::quality: q = 0.5 + (raw/1023) * 2.0.
        function formatHwRes(raw) {
          const q = 0.5 + (raw / 1023) * 2.0;
          return q.toFixed(2);
        }

        // Type selector: FlyonUI "Join radio with btn style" (same pattern as
        // Octave). The radio inputs are static in index.html (name="HwFltType");
        // the CSS paints :checked via .btn rule, so JS only mirrors the wasm
        // state on init and writes the type on change.
        //
        // Listen on BOTH 'input' and 'change': some browsers (and flyonui's
        // event handling) fire 'input' on radios, others only 'change'. Both
        // are idempotent (applyHwType reads the current checked state), so
        // double-firing is harmless. Use `r.value` at event time (not a
        // closure capture) so the correct type is written even if flyonui
        // re-dispatches or mutates the event.
        const typeRadios = Array.from(document.querySelectorAll('input[name="HwFltType"]'));
        if (typeRadios.length > 0) {
          let currentType = (Module.getHwFilterType) ? Module.getHwFilterType() : 0;
          function applyHwType(typeId) {
            currentType = typeId;
            if (Module.setHwFilterType) Module.setHwFilterType(typeId);
            // Mirror the checked state (the browser already did it on click;
            // this keeps the radio in sync when the type is set from wasm).
            for (const r of typeRadios) {
              r.checked = parseInt(r.value, 10) === typeId;
            }
          }
          for (const r of typeRadios) {
            const handler = () => applyHwType(parseInt(r.value, 10));
            r.addEventListener("input", handler);
            r.addEventListener("change", handler);
          }
          applyHwType(currentType);
        }

        // Cutoff slider (raw 0..1023).
        const cutoffInput = document.getElementById("hwflt_cutoff");
        const cutoffLabel = document.getElementById("hwflt_cutoff_val");
        if (cutoffInput && cutoffLabel) {
          const apply = (raw) => {
            cutoffInput.value = raw;
            cutoffLabel.textContent = formatHwCutoff(raw);
            if (Module.setHwFilterParam) Module.setHwFilterParam(0, raw); // PARAM_CUTOFF
          };
          cutoffInput.addEventListener("input", () => apply(parseInt(cutoffInput.value, 10)));
          const initRaw = (Module.getHwFilterParam) ? Module.getHwFilterParam(0) : 0;
          apply(initRaw);
        }

        // Resonance slider (raw 0..1023).
        const resInput = document.getElementById("hwflt_res");
        const resLabel = document.getElementById("hwflt_res_val");
        if (resInput && resLabel) {
          const apply = (raw) => {
            resInput.value = raw;
            resLabel.textContent = formatHwRes(raw);
            if (Module.setHwFilterParam) Module.setHwFilterParam(1, raw); // PARAM_RES
          };
          resInput.addEventListener("input", () => apply(parseInt(resInput.value, 10)));
          const initRaw = (Module.getHwFilterParam) ? Module.getHwFilterParam(1) : 0;
          apply(initRaw);
        }

        // Sweep F slider (0..30 Hz, step 0.01). Raw = round(freq*1000).
        const sweepFInput = document.getElementById("hwflt_sweepf");
        const sweepFLabel = document.getElementById("hwflt_sweepf_val");
        if (sweepFInput && sweepFLabel) {
          const apply = (freq) => {
            sweepFInput.value = freq;
            sweepFLabel.textContent = freq.toFixed(2) + " Hz";
            const raw = Math.round(freq * 1000);
            if (Module.setHwFilterParam) Module.setHwFilterParam(2, raw); // PARAM_SWEEP_F
          };
          sweepFInput.addEventListener("input", () => apply(parseFloat(sweepFInput.value)));
          const initRaw = (Module.getHwFilterParam) ? Module.getHwFilterParam(2) : 0;
          apply(initRaw / 1000.0);
        }

        // Sweep Amt slider (-100..+100, step 1). Raw = (amt+100)*5.115.
        const amtInput = document.getElementById("hwflt_sweepamt");
        const amtLabel = document.getElementById("hwflt_sweepamt_val");
        if (amtInput && amtLabel) {
          const apply = (amt) => {
            amtInput.value = amt;
            amtLabel.textContent = (amt > 0 ? "+" : "") + amt;
            const raw = Math.round((amt + 100) * 5.115);
            if (Module.setHwFilterParam) Module.setHwFilterParam(3, raw); // PARAM_SWEEP_AMT
          };
          amtInput.addEventListener("input", () => apply(parseInt(amtInput.value, 10)));
          const initRaw = (Module.getHwFilterParam) ? Module.getHwFilterParam(3) : 511;
          const initAmt = Math.round(initRaw / 5.115 - 100);
          apply(initAmt);
        }
      }

      // ---- FX chain knobs (MOD 9-params / DEL 3 / REV 3) -------------------
      // The full audio path OSC -> MOD -> DEL -> REV -> out runs inside the
      // single wasm worklet (drone-chain/chain.h).
      //
      // MOD now uses a PARAM/VALUE multiplexer (like the OSC): only TIME /
      // DEPTH are fixed header slots (driven through setFxParam(0, 0/1, raw)).
      // The 8 edit params (DRIFT / CMODE / CHORUS / FLANGER / NOISE / NRATE /
      // ENSMBL / TAPWOB) are driven through Module.modSetEditParam(idx, raw),
      // which writes the (PARAM_SEL, PARAM_VAL) pair atomically (the worklet
      // applies both on its next block, so the DSP never sees a stale select_).
      //
      // DEL / REV keep their own TIME / DEPTH / MIX via setFxParam(stage, 0/1/2, raw).
      //
      // Fixed knobs (setFxParam): MOD 0..1 (TIME/DEPTH), DEL 0..2 (TIME/DEPTH/MIX),
      // REV 0..2 (TIME/DEPTH/MIX). DEL MIX and REV MIX are drywet knobs
      // (setFxParam raw 0..1023, which the DSP maps to -1..+1; see delay.h /
      // reverb.h MIX case + formatFxParam above).
      const FX_STAGES = [
        { stage: 0, prefix: "fxmod", params: ["time", "depth"] },
        { stage: 1, prefix: "fxdel", params: ["time", "depth", "mix"] },
        { stage: 2, prefix: "fxrev", params: ["time", "depth", "mix"] },
      ];
      for (const { stage, prefix, params } of FX_STAGES) {
        params.forEach((param, which) => {
          const input = document.getElementById(prefix + "_" + param);
          const label = document.getElementById(prefix + "_" + param + "_val");
          if (!input || !label) return;
          const apply = (raw) => {
            input.value = raw;
            label.textContent = formatFxParam(stage, which, raw);
            if (Module.setFxParam) Module.setFxParam(stage, which, raw);
          };
          input.addEventListener("input", () => apply(parseInt(input.value, 10)));
          // Initialize from the wasm side (defaults in Modfx::Params::reset /
          // the header.c TIME/DEPTH defaults).
          const initRaw = (Module.getFxParam) ? Module.getFxParam(stage, which) : 0;
          apply(initRaw);
        });
      }

      // ---- MOD edit params (the MOD-group multiplexer, slots 2/3) --------------
      // Each slider carries data-modedit=<idx> (0..4 = DRIFT / CMODE / CHOR /
      // FLNGR / ENSMBL). Moving it calls Module.modSetEditParam(idx, raw)
      // (atomic PARAM_SEL + PARAM_VAL). The readout is formatted by
      // formatModEditParam (CMODE -> I / I+II / II, everything else -> %).
      // Initialized from Module.modGetEditParamValue(idx).
      const MOD_EDIT_SLIDERS = [
        { idx: 0, id: "fxmod_drift" },
        { idx: 1, id: "fxmod_cmode" },
        { idx: 2, id: "fxmod_chorus" },
        { idx: 3, id: "fxmod_flanger" },
        { idx: 4, id: "fxmod_ensemble" },
      ];
      for (const { idx, id } of MOD_EDIT_SLIDERS) {
        const input = document.getElementById(id);
        const label = document.getElementById(id + "_val");
        if (!input || !label) continue;
        const apply = (raw) => {
          input.value = raw;
          label.textContent = formatModEditParam(idx, raw);
          if (Module.modSetEditParam) Module.modSetEditParam(idx, raw);
        };
        input.addEventListener("input", () => apply(parseInt(input.value, 10)));
        // Initialize from the wasm side (Modfx::edit_value_ defaults).
        const initRaw = (Module.modGetEditParamValue) ? Module.modGetEditParamValue(idx) : 0;
        apply(initRaw);
      }

      // ---- TAPE edit params (the TAPE-group multiplexer, slots 4/5) -----------
      // Each slider carries data-modtape=<idx> (0..4 = NOISE / NRATE / TAPWOB /
      // HISS / WARP, the KoS-derived tape stages). Moving it calls
      // Module.modSetTapeParam(idx, raw) (atomic TPARAM + TVALUE). The readout
      // is formatted by formatTapeEditParam (0..100%). Initialized from
      // Module.modGetTapeEditParamValue(idx).
      const TAPE_EDIT_SLIDERS = [
        { idx: 0, id: "fxmod_noise" },
        { idx: 1, id: "fxmod_nrate" },
        { idx: 2, id: "fxmod_tapwob" },
        { idx: 3, id: "fxmod_hiss" },
        { idx: 4, id: "fxmod_warp" },
      ];
      for (const { idx, id } of TAPE_EDIT_SLIDERS) {
        const input = document.getElementById(id);
        const label = document.getElementById(id + "_val");
        if (!input || !label) continue;
        const apply = (raw) => {
          input.value = raw;
          label.textContent = formatTapeEditParam(idx, raw);
          if (Module.modSetTapeParam) Module.modSetTapeParam(idx, raw);
        };
        input.addEventListener("input", () => apply(parseInt(input.value, 10)));
        // Initialize from the wasm side (Modfx::tedit_value_ defaults).
        const initRaw = (Module.modGetTapeEditParamValue) ? Module.modGetTapeEditParamValue(idx) : 0;
        apply(initRaw);
      }

      // ---- Per-stage ON/OFF toggles (diagnostic A/B) --------------------------
      // A checkbox in each FX card header controls whether that stage is in the
      // C++ chain (OSC -> [MOD] -> [DEL] -> [REV]). The toggle is UNLABELED:
      //   checked   -> the stage IS in the path (active, contributes to sound).
      //   unchecked -> the stage is BYPASSED (dry wire through).
      // Default = checked = every stage active (the unit runs the full path).
      // stage: 0=MOD, 1=DEL, 2=REV.
      const BYPASS_STAGES = [
        { stage: 0, id: "fxmod_bypass" },
        { stage: 1, id: "fxdel_bypass" },
        { stage: 2, id: "fxrev_bypass" },
      ];
      for (const { stage, id } of BYPASS_STAGES) {
        const box = document.getElementById(id);
        if (!box) continue;
        const refresh = () => {
          // checked = active (in path); unchecked = bypass.
          // Only the audio chain changes (setStageBypass); the block UI is
          // NOT dimmed/de-emphasized when the unit is switched off.
          const bypass = box.checked ? 0 : 1;
          if (Module.setStageBypass) Module.setStageBypass(stage, bypass);
        };
        // Listen on both: some automation/headless contexts emit `input`
        // rather than `change` for checkboxes. Both are idempotent (refresh
        // reads box.checked), so double-firing is harmless.
        box.addEventListener("change", refresh);
        box.addEventListener("input", refresh);
        // Init from wasm state: active (bypass==0) means the box is checked.
        const active = !(Module.getStageBypass && Module.getStageBypass(stage));
        box.checked = active;
        refresh();
      }
    };
    
