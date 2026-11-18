// Audio core for the drone-chain simulator (Вариант B: Svelte-страница —
// начальник audio).
//
// Перенос 1:1 логики из setup-web-audio.js (legacy wiring, 1897 строк),
// МИНУС весь DOM:
//   - нет document.* / getElementById / createElement (кроме разрешённого:
//     window.addEventListener autoResume, canvas 2D-контексты, getComputedStyle
//     canvas'ов, MutationObserver на data-theme — см. plan);
//   - UI-состояние живёт в stores.ts (Svelte 5 $state), сюда ходит через bus;
//   - core слушает bus ('...:set' -> wasm) и пушит внешние изменения обратно
//     ('param:init'/'param:ext'/'radio:init'/'radio:ext'/'preset:active'/'morph:highlight').
//
// initAudio({ context, node, Module }) -> AudioController.

import { bus } from './bus';

// ============================ Constants (1:1 legacy) ========================

export const EDIT_PARAM_COUNT = 55; // 55 = RATE..FCUT2 (см. legacy, строка 58)

export const EDIT_PARAM_NAMES = [
  "Rate", "Detune",
  "Tone Rate", "Tone Smooth", "Timbre Rate", "Timbre Smooth", "Density Rate", "Density Smooth",
  "Sub 1", "Sub 2", "Drive", "Asym", "Fold", "Wave", "Wave 2",
  "Main",
  "Morph Wander", "Drive Wander", "Fold Wander",
  "Cutoff", "Resonance", "Cutoff Wander",
  "Breath Depth", "Breath Rate", "Breath Smooth",
  "Jitter", "Ring Mod", "FM A", "FM B", "FM A Bre", "FM B Bre",
  "Formant Vowel", "Formant Level",
  "Comb Res",
  "Sub Bass Bre",
  "Wave LFO Rate", "Wave LFO Depth",
  "Wave 2 LFO Rate", "Wave 2 LFO Depth", "Wave Mix",
  "Wave LFO Shape", "Wave LFO Gap",
  "Wave 2 LFO Shape", "Wave 2 LFO Gap",
  "Wave Mix LFO Rate", "Wave Mix LFO Depth",
  "Wave Mix LFO Shape", "Wave Mix LFO Gap",
  "FM Mod Rate", "FM Mod Wave", "FM Mod Depth", "FM Teeth", "FM Teeth Depth", "FM Amount",
  "Cutoff 24dB",
];
if (EDIT_PARAM_NAMES.length !== EDIT_PARAM_COUNT) {
  throw new Error('EDIT_PARAM_NAMES.length must equal EDIT_PARAM_COUNT (55)');
}

export interface ParamGroup {
  title: string;
  col: 0 | 1 | 2;
  items: number[];
  divider?: boolean;
  className?: string;
}

// Semantic groups (UI-only; DSP enum order in osc.h is untouched). 3-column
// Parameters layout; each group becomes one titled block in its `col`.
// WALK-gap params (41/43/47) deliberately NOT listed: rendered only as the
// gap row next to their Shape radio group (40/42/46), hidden unless Walk.
export const PARAM_GROUPS: ParamGroup[] = [
  // ---- Column 0 ----
  { title: "",           col: 0, items: [13, 35, 36, 40] },
  { title: "",           col: 0, items: [14, 37, 38, 42], divider: true },
  { title: "",           col: 0, items: [39, 44, 45, 46], divider: true },
  { title: "",           col: 0, items: [10, 11, 12, 27, 28, 25, 26, 0], divider: true },
  { title: "FORMANT",    col: 0, items: [31, 32] },
  // ---- Column 1 ----
  { title: "WANDER",     col: 1, items: [1, 2, 3, 4, 5, 6, 7] },
  { title: "WANDER TGT", col: 1, items: [16, 21, 17, 18, 29, 30] },
  // ---- Column 2 ----
  { title: "VOLUME",     col: 2, items: [15, 22, 23, 24, 8, 9, 34] },
  { title: "FILTER",     col: 2, items: [19, 20, 54] },
  { title: "CLUSTER",    col: 2, items: [33] },
  { title: "FM MOD",     col: 2, items: [49, 48, 50, 51, 52, 53] },
];

// ---- WAVE bank (edit 13 WAV1 / 14 WAV2) ------------------------------------
// Bank 0..5 (A..F); the internal LFO morphs WITHIN the selected bank. The DSP
// (osc_param_apply.h, cases WAV1/WAV2) quantizes raw -> bank:
//   bank = int((raw/1023) * 5.999), clamped 0..5
// Reverse: raw = round(bank * 1023 / 5). Exact round-trips for b = 0..5.
export const WAVE_PARAM_IDX = 13;
export const WAVE2_PARAM_IDX = 14;
export const WAVE_BANKS = ["A", "B", "C", "D", "E", "F"];

export function waveBankFromRaw(raw: number): number {
  let b = Math.floor((raw / 1023) * 5.999);
  if (b < 0) b = 0; else if (b > 5) b = 5;
  return b;
}

export function rawFromWaveBank(b: number): number {
  return Math.round(b * 1023 / 5);
}

// ---- LFO shapes / FM mod waves (radio groups, raw = band centers) ----------
export const LFO_SHAPES = [
  { label: "Sin",  raw: 0   },
  { label: "S",    raw: 640 },
  { label: "Walk", raw: 896 },
];

export const FM_MOD_WAVES = [
  { label: "SIN",   raw: 100 },
  { label: "TRI",   raw: 400 },
  { label: "SAW",   raw: 600 },
  { label: "SQR",   raw: 750 },
  { label: "NOISE", raw: 950 },
];

/** Квантизация raw -> index LFO-shape (0..2), mirrors legacy createEditableParam. */
export function lfoShapeIdxFromRaw(raw: number): 0 | 1 | 2 {
  return raw < 512 ? 0 : (raw < 768 ? 1 : 2);
}

/** Квантизация raw -> index FM-mod-wave (0..4), mirrors legacy (idx 49). */
export function fmModWaveIdxFromRaw(raw: number): 0 | 1 | 2 | 3 | 4 {
  return raw < 342 ? 0 : (raw < 512 ? 1 : (raw < 683 ? 2 : (raw < 854 ? 3 : 4)));
}

// ============================ Radio groups (UI -> raw) ========================
// UI: radio-группы (WaveBank/WaveBank2/LfoShape1..3/FmModWave). idx -> raw.
export function rawFromRadioGroup(
  group: "WaveBank" | "WaveBank2" | "LfoShape1" | "LfoShape2" | "LfoShape3" | "FmModWave",
  idx: number,
): [number, string] {
  if (group === "WaveBank" || group === "WaveBank2") {
    return [rawFromWaveBank(idx), WAVE_BANKS[idx]];
  }
  if (group === "LfoShape1" || group === "LfoShape2" || group === "LfoShape3") {
    return [LFO_SHAPES[idx].raw, LFO_SHAPES[idx].label];
  }
  if (group === "FmModWave") {
    return [FM_MOD_WAVES[idx].raw, FM_MOD_WAVES[idx].label];
  }
  return [0, "?"];
}

// ---- MOD / TAPE edit names ---------------------------------------------------
export const MOD_EDIT_NAMES = ["DRIFT", "CMODE", "CHOR", "FLNGR", "ENSMBL"];
export const TAPE_EDIT_NAMES = ["NOISE", "NRATE", "TAPWOB", "HISS", "WARP"];

// ============================ Formatters (1:1 legacy) ========================

/**
 * Format the raw 10-bit value (0..1023) of one of the 55 edit parameters for
 * display. Mirrors Osc::getParameterStrValue in osc.h (single source of truth)
 * — 1:1 port of legacy formatParamValue (setup-web-audio.js ~196..368).
 */
export function formatParamValue(idx: number, raw: number): string {
  if (idx === 13 || idx === 14) {
    // WAVE / WAVE 2: bank letter A..F (raw -> bank 0..5).
    const bank = Math.round((raw / 1023) * 5.999);
    return "ABCDEF".charAt(Math.max(0, Math.min(5, bank)));
  }
  if (idx === 11 || idx === 39) {
    // ASYM / WAVE MIX: symmetric -100..+100% (511 = 0).
    const pct = Math.round((raw * 200) / 1023) - 100;
    return (pct >= 0 ? "+" : "") + pct + "%";
  }
  // DETU (edit 1): +/-10 cents, centered on the raw MIDPOINT (511), NOT 1023.
  if (idx === 1) {
    const cents = Math.round(((raw - 511) * 10) / 511);
    return (cents >= 0 ? "+" : "") + cents + " c";
  }
  if (idx === 10)
    return String(Math.round((raw / 1023) * 300)); // 300 = kSatuMax*100 (SATU)
  if (idx === 27 || idx === 28) {
    // FM1 / FM2: phase-FM index, LOG scale 1e-5..0.2 cycles.
    // 10^(x*5 - 5) * 0.2 cycles, x = raw/1023 (raw 0 = OFF).
    if (raw === 0) return "0";
    const cycles = Math.pow(10, 5 * (raw / 1023) - 5) * 0.2;
    if (cycles < 0.0001) {
      const d = Math.max(1, Math.min(10, Math.round(cycles * 100000)));
      return "0.000" + d;
    } else if (cycles < 0.001) {
      const d = Math.max(10, Math.min(100, Math.round(cycles * 100000)));
      return "0.000" + (d < 100 ? d : "0" + (d % 10));
    } else if (cycles < 0.01) {
      const d = Math.max(10, Math.min(100, Math.round(cycles * 1000)));
      return "0.00" + (d < 100 ? d : "0" + (d % 10));
    } else if (cycles < 0.1) {
      const d = Math.max(10, Math.min(100, Math.round(cycles * 100)));
      return "0.0" + (d < 100 ? d : "0" + (d % 10));
    } else {
      return cycles.toFixed(1);
    }
  }
  if (idx === 19) {
    // FCUT: SVF cutoff (log-mapped 80 Hz .. 12 kHz). Hz or kHz.
    const knob = raw / 1023;
    const fc = 80 * Math.pow(12000 / 80, knob);
    if (fc >= 1000) return (fc / 1000).toFixed(1) + "k";
    return Math.round(fc) + "H";
  }
  if (idx === 20) {
    // RES: SVF resonance Q (0.5 .. 2.5)
    const q = 0.5 + (raw / 1023) * 2.0;
    return q.toFixed(1);
  }
  if (idx === 31) {
    // FORM: formant vowel position -> nearest vowel letter (A/E/I/O/U).
    const vi = Math.min(4, Math.floor((raw / 1023) * 4.999));
    return "AEIOU".charAt(vi);
  }
  if (idx === 35 || idx === 37 || idx === 44) {
    // WAVE LFO RATE / WAVE 2 LFO RATE / WAVE MIX LFO RATE: LOG 0.001..1 Hz
    // (raw 0 = OFF). freq = 10^(3*raw/1023 - 3).
    if (raw === 0) return "0 H";
    const hz = Math.pow(10, 3 * (raw / 1023) - 3); // 0.001..1 (log)
    if (hz >= 0.1) {
      const d = Math.round(hz * 10); // 1..10
      return d >= 10 ? "1 H" : "0." + d + " H";
    }
    const mhz = Math.min(999, Math.max(1, Math.round(hz * 1000)));
    return "0." + String(mhz).padStart(3, "0") + " H";
  }
  if (idx === 40 || idx === 42 || idx === 46) {
    // LFO SHAPE: 0..511 = SIN, 512..767 = S, 768..1023 = WALK.
    if (raw < 512) return "Sin";
    if (raw < 768) return "S";
    return "Walk";
  }
  if (idx === 41 || idx === 43 || idx === 47) {
    // LFO GAP: LOG scale 1..300 s.
    const sec = Math.round(Math.pow(300, raw / 1023));
    return Math.max(1, Math.min(300, sec)) + " s";
  }
  if (idx === 48) {
    // FM Mod Rate: LOG 0..1 Hz (raw 0 = OFF): 10^(3*raw/1023 - 3) Hz.
    if (raw === 0) return "0 H";
    const hz = Math.pow(10, 3 * (raw / 1023) - 3);
    if (hz >= 0.1) return (hz >= 1 ? "1" : "0." + Math.round(hz * 10)) + " H";
    return "0." + String(Math.max(1, Math.min(999, Math.round(hz * 1000)))).padStart(3, "0") + " H";
  }
  if (idx === 49) {
    // FM Mod Wave: SIN / TRI / SAW / SQR / NOISE (raw band, mirrors DSP).
    if (raw < 342) return "SIN";
    if (raw < 512) return "TRI";
    if (raw < 683) return "SAW";
    if (raw < 854) return "SQR";
    return "NOISE";
  }
  if (idx === 51) {
    // FM Teeth: LOG 0.05..16 teeth per carrier cycle (raw 0 = OFF).
    if (raw === 0) return "OFF";
    const teeth = 0.05 * Math.pow(16 / 0.05, raw / 1023);
    if (teeth >= 9.5) return String(Math.round(teeth)); // 10..16 whole
    if (teeth >= 1) return (Math.floor(teeth) + "." + Math.floor(teeth * 10) % 10); // N.F
    return (Math.floor(teeth * 10) / 10).toFixed(2); // 0.NN
  }
  // Every other index formats as 0..100%:
  // 0..7 (RATE, SPD1..SMT3), 8..9 (SUB1/SUB2), 12 (FOLD), 15 (MAIN),
  // 16..18 (RWAV/RDRV/RFLD), 21 (RWFC), 22..24 (Breath), 25 (JIT), 26 (RMOD),
  // 29..30 (RFMA/RFMB), 32 (FVOL), 33 (COMB), 34 (SUBB), 36/38 (LFO DEPTHS),
  // 50 (FM Mod Depth), 52 (FM Teeth Depth), 53 (FM Amount), 54 (Cutoff 24dB).
  return Math.round((raw * 100) / 1023) + "%";
}

/**
 * Format a DRONE FX parameter. stage: 0=MOD, 1=DEL, 2=REV; which: 0=TIME,
 * 1=DEPTH, 2=MIX (DEL/REV only). DEL/REV MIX are drywet knobs (raw -1000..1000
 * -> -100%..+100%); everything else 0..100%.
 */
export function formatFxParam(stage: number, which: number, raw: number): string {
  if ((stage === 1 || stage === 2) && which === 2) {
    const pct = Math.round(raw / 10.0);
    return (pct > 0 ? "+" : "") + pct + "%";
  }
  return Math.round((raw * 100) / 1023) + "%";
}

/** MOD edit: CMODE (idx 1) shows the Juno voice band (I / I+II / II). */
export function formatModEditParam(idx: number, raw: number): string {
  if (idx === 1) { // CMODE
    if (raw < 342) return "I";
    if (raw < 684) return "I+II";
    return "II";
  }
  return Math.round((raw * 100) / 1023) + "%";
}

/** TAPE edit: every value shows 0..100%. */
export function formatTapeEditParam(_idx: number, raw: number): string {
  return Math.round((raw * 100) / 1023) + "%";
}

// ---- HW filter formatters (web-sim only; mirrors hwfilter.h) ----------------

/** Cutoff: fc = 1 * (20000/1)^(raw/1023) Hz. */
export function formatHwCutoff(raw: number): string {
  const fc = 1.0 * Math.pow(20000.0 / 1.0, raw / 1023);
  if (fc < 1000) return Math.round(fc) + " Hz";
  return (fc / 1000).toFixed(2) + " kHz";
}

/** Resonance: q = 0.5 + (raw/1023) * 2.0. */
export function formatHwRes(raw: number): string {
  const q = 0.5 + (raw / 1023) * 2.0;
  return q.toFixed(2);
}

/** Sweep F: 0..30 Hz. */
export function formatHwSweepF(freq: number): string {
  return freq.toFixed(2) + " Hz";
}

/** Sweep Amt: -100..+100. */
export function formatHwAmt(amt: number): string {
  return (amt > 0 ? "+" : "") + String(amt);
}

// ---- Factory defaults (1:1 legacy DEFAULT_RAW, 55 values) -------------------
// The edit_value_ initializers in osc.h (RATE-first order).
export const DEFAULT_RAW = [
  0, 511, 511, 511, 511, 511, 511, 511, 511, 410,
  341, 511, 0, 0, 128, 1023, 0, 0, 0, 921,
  0, 0, 0, 511, 511, 0, 0, 0, 0, 0,
  0, 511, 0, 0, 307, 0, 1023, 0, 1023, 511,
  0, 511, 0, 511, 0, 1023, 0, 511, 0, 0,
  0, 0, 1023, 1023, 0,
];
if (DEFAULT_RAW.length !== EDIT_PARAM_COUNT) {
  throw new Error('DEFAULT_RAW.length must equal EDIT_PARAM_COUNT (55)');
}

// ============================ Randomize (1:1 legacy) =========================

/**
 * Full randomization of all 55 parameters. "Safe" (walk) parameters get the
 * full 0..1023 range; "critical" parameters are constrained to
 * audition-friendly windows so Random always yields a playable sound
 * (1:1 port of legacy randomizeAllRaw, ~1116..1298).
 */
export function randomizeAllRaw(): number[] {
  const raws = new Array(EDIT_PARAM_COUNT).fill(0);
  const I = (name: string) => EDIT_PARAM_NAMES.indexOf(name);
  // Guarded setter: -1 (missing name) -> loud console.warn, no silent no-op.
  const set = (name: string, raw: number) => {
    const i = I(name);
    if (i < 0 || i >= raws.length) { console.warn("randomRaw: unknown param", name); return; }
    raws[i] = raw;
  };
  const r1023 = () => Math.floor(Math.random() * 1024); // 0..1023
  const rRange = (lo: number, hi: number) => lo + Math.floor(Math.random() * (hi - lo + 1)); // inclusive

  // --- Full-range (walk / timbre) parameters -------------------------------
  for (const n of ["Tone Rate", "Tone Smooth", "Timbre Rate", "Timbre Smooth", "Density Rate", "Density Smooth"])
    set(n, r1023());
  for (const n of ["Morph Wander", "Drive Wander", "Fold Wander", "Cutoff Wander"])
    set(n, r1023());
  {
    const bank = Math.floor(Math.random() * 6); // 0..5
    const bank2 = Math.floor(Math.random() * 6); // 0..5 (independent)
    set("Wave", rawFromWaveBank(bank));
    set("Wave 2", rawFromWaveBank(bank2));
  }
  for (const n of ["Wave LFO Rate", "Wave LFO Depth", "Wave 2 LFO Rate", "Wave 2 LFO Depth", "Wave Mix"])
    set(n, r1023());
  for (const n of ["Wave Mix LFO Rate", "Wave Mix LFO Depth"])
    set(n, r1023());
  // LFO SHAPES: SIN or WALK (S too niche for random).
  set("Wave LFO Shape", Math.random() < 0.5 ? 0 : 896);
  set("Wave LFO Gap", rRange(171, 962));
  set("Wave 2 LFO Shape", Math.random() < 0.5 ? 0 : 896);
  set("Wave 2 LFO Gap", rRange(171, 962));
  set("Wave Mix LFO Shape", Math.random() < 0.5 ? 0 : 896); // 0 = Sin, 896 = Walk
  set("Wave Mix LFO Gap", rRange(171, 962)); // ~10..200 s
  // --- Constrained (critical) parameters -----------------------------------
  set("Detune", rRange(256, 767));     // +/-5 c
  set("Rate", rRange(204, 511));       // 20..50%
  set("Sub 1", rRange(204, 818));      // 20..80%
  set("Sub 2", rRange(256, 716));      // 25..70%
  set("Drive", rRange(136, 819));      // 40..240
  set("Asym", rRange(256, 767));       // +/-50%
  set("Fold", rRange(0, 613));         // 0..60%
  set("Main", rRange(563, 1023));      // 55..100%
  set("Cutoff", rRange(740, 1023));    // 1..11 kHz
  set("Resonance", rRange(0, 669));    // 0.5..1.8
  set("Breath Depth", rRange(204, 818)); // 20..80%
  set("Breath Rate", r1023());
  set("Breath Smooth", r1023());
  // ---- Enrichment params ----------------------------------------------------
  set("Jitter", rRange(0, 153));        // 0..15%
  set("Ring Mod", rRange(0, 256));      // 0..25%
  set("FM A", rRange(0, 307));          // 0..30%
  set("FM B", rRange(0, 307));
  set("FM A Bre", rRange(0, 511));      // 0..50%
  set("FM B Bre", rRange(0, 511));
  set("Formant Vowel", r1023());        // any vowel
  set("Formant Level", rRange(0, 512)); // 0..50%
  set("Comb Res", rRange(0, 307));      // 0..30%
  // ---- FM modulator C (48/49/50) + static teeth (51/52) ---------------------
  set("FM Mod Rate", rRange(0, 307));
  set("FM Mod Wave", rRange(0, 1023));
  set("FM Mod Depth", rRange(0, 511));
  set("FM Teeth", rRange(0, 614));
  set("FM Teeth Depth", rRange(256, 1023));
  set("Cutoff 24dB", rRange(0, 716));   // 0..70%
  return raws;
}

/**
 * Partial randomization: a "development" around the CURRENT sound.
 * 1:1 port of legacy partialRandomRaw, EXCEPT the base is read from
 * `base` (the live UI state) instead of DOM sliders. Mirrors
 * Osc::randomizePartial on the device (kRndMinStepRaw = 102).
 */
export function partialRandomRaw(base: ArrayLike<number>): number[] {
  const fresh = randomizeAllRaw();
  const count = 2 + Math.floor(Math.random() * 5); // 2..6
  const picked = new Set<number>();
  while (picked.size < count) {
    picked.add(Math.floor(Math.random() * EDIT_PARAM_COUNT));
  }
  const out = Array.from(base) as number[];
  const MIN_STEP = 102; // ~10% of 1024, matches kRndMinStepRaw in osc_randomize.h
  for (const idx of picked) {
    let v = fresh[idx];
    let step = Math.abs(v - out[idx]);
    for (let t = 0; t < 5 && step < MIN_STEP; ++t) {
      v = randomizeAllRaw()[idx];
      step = Math.abs(v - out[idx]);
    }
    out[idx] = v;
  }
  return out;
}

// ============================ Core ===========================================

export interface AudioController {
  /** Размонтировать core: остановить таймеры/rAF, отключить listeners. */
  stop(): void;
  /** Связать scope-canvas'ы и запустить rAF-цикл (UI передаёт элементы). */
  attachScope(wave: HTMLCanvasElement, fft: HTMLCanvasElement): void;
  // Прямые API (не обязательны для UI — UI ходит через bus; для тестов):
  getParam(i: number): number;
  setParam(i: number, v: number): void;
  randomize(mode: 0 | 1 | 2): void;
  applyPreset(name: string, raws?: number[]): void;
}

function approximatelyEqual(a: number, b: number, epsilon = 1e-4): boolean {
  return Math.abs(a - b) < epsilon;
}

export function initAudio(bundle: { context: AudioContext; node: AudioWorkletNode; Module: any }): AudioController {
  const context = bundle.context;
  const node = bundle.node;
  const Module = bundle.Module;

  // ---- current state (UI owns the reactive copy in stores.ts) ---------------
  let octaveShift = -1; // legacy: starts one octave below base A2..D4 (A1..D3)
  let osc_frequency = 440.0;
  const frequencyStack: number[] = []; // was window.frequencyStack
  const pressedNotes = new Set<string>(); // touch-quirk re-press detection

  const state = {
    params: Array.from(DEFAULT_RAW) as number[],
    volume: 1,
    bpm: 120,
    octave: -1,
    hwflt: { type: 0, cutoff: 0, res: 0, sweepF: 0, sweepAmt: 0 },
    fxmod: { time: 512, depth: 511, edit: [0, 0, 0, 0, 0], tape: [0, 0, 0, 0, 0] },
    fxdel: { time: 0, depth: 512, mix: -1000 },
    fxrev: { time: 512, depth: 512, mix: -1000 },
    bypass: [0, 0, 0],
    activePreset: null as string | null,
  };

  // ---- audio graph (1:1 legacy ~700..718) -----------------------------------
  // Unity-gain envelope: the A-H-R envelope lives in the DSP (noteOn/noteOff).
  let envelope = new GainNode(context, { gain: 1.0 });
  let volume = new GainNode(context, { gain: 1.0 });
  let analyser = new AnalyserNode(context, { fftSize: 4096 });
  let data = new Float32Array(analyser.frequencyBinCount);
  let dataByte = new Uint8Array(analyser.frequencyBinCount);

  node.connect(envelope).connect(volume).connect(context.destination);
  envelope.connect(analyser);

  // ---- auto-resume (allowed global listeners, not DOM traversal) ------------
  const autoResume = () => {
    if (context.state !== "running") context.resume();
  };
  window.addEventListener("click", autoResume, true);
  window.addEventListener("keydown", autoResume, true);

  // ============================ preset / morph ===============================

  let presetApplyTimer: ReturnType<typeof setTimeout> | null = null;

  /**
   * Paced application of a full raw array: one setEditParam every 8 ms (the
   * DSP direct edit channel has a SINGLE pending slot; a burst would drop
   * ~29 of 55 writes). Also pushes each value to the UI via param:ext /
   * radio:ext so the controls travel with the apply (legacy applied to DOM).
   */
  function applyPresetRaw(raws: number[], presetLabel: string) {
    if (context.state !== "running") context.resume();
    if (presetApplyTimer) clearTimeout(presetApplyTimer);
    state.activePreset = presetLabel;
    bus.emit('preset:active', { name: presetLabel });
    let i = 0;
    (function step() {
      if (i < raws.length) {
        const raw = raws[i];
        if (i === WAVE_PARAM_IDX) {
          bus.emit('radio:ext', { group: 'WaveBank', idx: waveBankFromRaw(raw) });
        } else if (i === WAVE2_PARAM_IDX) {
          bus.emit('radio:ext', { group: 'WaveBank2', idx: waveBankFromRaw(raw) });
        } else if (i === 40 || i === 42 || i === 46) {
          bus.emit('radio:ext', {
            group: i === 40 ? 'LfoShape1' : (i === 46 ? 'LfoShape3' : 'LfoShape2'),
            idx: lfoShapeIdxFromRaw(raw),
          });
        } else if (i === 49) {
          bus.emit('radio:ext', { group: 'FmModWave', idx: fmModWaveIdxFromRaw(raw) });
        } else {
          bus.emit('param:ext', { i, v: raw });
        }
        if (Module.setEditParam) Module.setEditParam(i, raw);
        state.params[i] = raw;
        ++i;
        presetApplyTimer = setTimeout(step, 8);
      } else {
        presetApplyTimer = null;
        // UI-thread apply bypasses the audio re-roll stabilization — sync the
        // C++ Orbit "stable state" snapshot with the freshly applied sound.
        if (Module.stableSync) Module.stableSync();
      }
    })();
  }

  let fireSyncTimer: ReturnType<typeof setInterval> | null = null;
  const morphingParams = new Set<number>(); // params highlighted during a morph

  /**
   * Fire an audio-worklet re-roll (RndM: 0=All, 1=Part, 2=Orbit) and poll the
   * main-thread processor mirror until the values settle (1:1 legacy
   * fireRandom). The re-roll is a BPM-synced MORPH (Osc::rndMorphStart/Tick);
   * getEditParamValue() returns the CURRENT interpolated value in flight, so
   * the poll makes the UI travel with the morph in real time.
   */
  function fireRandom(mode: 0 | 1 | 2) {
    if (fireSyncTimer) clearInterval(fireSyncTimer);
    Module.setRndMode(mode);
    Module.rndTrigger();
    const t0 = performance.now();
    const MORPH_SYNC_MS = 5000; // 1-bar morph + max stagger + margin (legacy)
    let lastWaveBank = -1;
    let lastWave2Bank = -1;
    fireSyncTimer = setInterval(() => {
      let dirty = false;
      for (let i = 0; i < EDIT_PARAM_COUNT; ++i) {
        const v = Module.getEditParamValue(i);
        if (i === WAVE_PARAM_IDX || i === WAVE2_PARAM_IDX) {
          const isWave1 = (i === WAVE_PARAM_IDX);
          const lastBank = isWave1 ? lastWaveBank : lastWave2Bank;
          const bank = waveBankFromRaw(v);
          if (lastBank !== bank) {
            if (isWave1) lastWaveBank = bank; else lastWave2Bank = bank;
            bus.emit('radio:ext', { group: isWave1 ? 'WaveBank' : 'WaveBank2', idx: bank });
            dirty = true;
          }
          continue;
        }
        if (i === 40 || i === 42 || i === 46) {
          bus.emit('radio:ext', {
            group: i === 40 ? 'LfoShape1' : (i === 46 ? 'LfoShape3' : 'LfoShape2'),
            idx: lfoShapeIdxFromRaw(v),
          });
          dirty = true;
          continue;
        }
        if (i === 49) {
          bus.emit('radio:ext', { group: 'FmModWave', idx: fmModWaveIdxFromRaw(v) });
          dirty = true;
          continue;
        }
        if (v !== state.params[i]) {
          state.params[i] = v;
          bus.emit('param:ext', { i, v });
          if (!morphingParams.has(i)) {
            morphingParams.add(i);
            bus.emit('morph:highlight', { i, on: true });
          }
          dirty = true;
        }
      }
      const morphDone = Module.getMorphActive === undefined
        ? true
        : (Module.getMorphActive() === 0);
      if ((dirty === false && morphDone) || performance.now() - t0 > MORPH_SYNC_MS) {
        if (fireSyncTimer) { clearInterval(fireSyncTimer); fireSyncTimer = null; }
        for (const i of morphingParams) {
          bus.emit('morph:highlight', { i, on: false });
        }
        morphingParams.clear();
      }
    }, 33);
  }

  // ============================ octave shift =================================
  // setOctaveShift: radio (UI emits octave:set source='radio') or Z/X hotkey
  // (UI emits source='hotkey'); core mirrors the radio state via
  // radio:ext so the button group follows the hotkey.
  function setOctaveShift(value: number, source: 'radio' | 'hotkey') {
    value = Math.max(-2, Math.min(2, Math.round(value)));
    octaveShift = value;
    state.octave = value;
    if (source === 'hotkey') {
      // button = shift + 3 (1..5)
      bus.emit('radio:ext', { group: 'Octave', idx: value + 3 });
    }
  }

  // ============================ scope (canvas'ы через attachScope) ===========

  let canvasWave: HTMLCanvasElement | null = null;
  let canvasFFT: HTMLCanvasElement | null = null;
  let canvasWaveContext: CanvasRenderingContext2D | null = null;
  let canvasFFTContext: CanvasRenderingContext2D | null = null;
  let scopeBg = "";
  let scopeFg = "";
  let scopeObserver: MutationObserver | null = null;
  let scopeRafId: number | null = null;

  // Theme-aware drawing colors, cached outside the per-frame loop. app.css
  // exposes --scope-bg/--scope-fg on .scope-canvas; refresh on <html>
  // data-theme changes (MutationObserver — observation, not traversal).
  function refreshScopeColors() {
    if (!canvasWave) return;
    const s = getComputedStyle(canvasWave);
    scopeBg = s.getPropertyValue("--scope-bg").trim() || "#fff";
    scopeFg = s.getPropertyValue("--scope-fg").trim() || "#000";
  }

  // Vertical gain for the time-domain trace (1:1 legacy).
  const WAVE_GAIN = 1.0;

  function drawScope() {
    if (!canvasWaveContext || !canvasFFTContext || !canvasWave || !canvasFFT) return;
    analyser.getFloatTimeDomainData(data);
    canvasWaveContext.fillStyle = scopeBg;
    canvasWaveContext.fillRect(0, 0, canvasWave.width, canvasWave.height);
    // Skip the trace when the signal is inaudible (-60 dBFS threshold).
    let peak = 0;
    for (let i = 0; i < data.length; i++) {
      const a = Math.abs(data[i]);
      if (a > peak) peak = a;
    }
    if (peak < 1e-3) {
      canvasFFTContext.clearRect(0, 0, canvasFFT.width, canvasFFT.height);
      scopeRafId = requestAnimationFrame(drawScope);
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
      // Center on the mid-line, invert Y, apply WAVE_GAIN, hard-clamp.
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
    analyser.getByteFrequencyData(dataByte);
    canvasFFTContext.clearRect(0, 0, canvasFFT.width, canvasFFT.height);
    // Downsample: 2048 bins -> 512 bars (every 4th bin) so each bar is 1px wide.
    // getByteFrequencyData returns 0..255; map to pixel height directly.
    const step = Math.max(1, Math.floor(dataByte.length / canvasFFT.width));
    let posX = 0;
    for (let i = 0; i < dataByte.length; i += step) {
      let barHeight = dataByte[i]; // 0..255 -> 0..255px (clamp to canvas height)
      if (barHeight > canvasFFT.height) barHeight = canvasFFT.height;
      canvasFFTContext.fillStyle = scopeFg;
      canvasFFTContext.fillRect(posX, canvasFFT.height - barHeight, 1, barHeight);
      posX += 1;
    }
    scopeRafId = requestAnimationFrame(drawScope);
  }

  function attachScope(wave: HTMLCanvasElement, fft: HTMLCanvasElement) {
    canvasWave = wave;
    canvasFFT = fft;
    canvasWaveContext = wave.getContext("2d");
    canvasFFTContext = fft.getContext("2d");
    if (canvasWaveContext && canvasFFTContext) {
      refreshScopeColors();
      scopeObserver = new MutationObserver(refreshScopeColors);
      scopeObserver.observe(document.documentElement, {
        attributes: true,
        attributeFilter: ["data-theme"],
      });
      drawScope();
    }
  }

  // ============================ bus listeners (UI -> core) ===================

  const offParamSet = bus.on('param:set', ({ i, v }) => {
    state.params[i] = v;
    if (Module.setEditParam) Module.setEditParam(i, v);
    // Keep the Orbit "stable state" anchored to the live values: a manual
    // slider edit IS a new stable state (skip while a morph is in flight —
    // that apply is the fireRandom poll; the C++ side stabilizes at morph end).
    if (Module.stableSync && Module.getMorphActive && Module.getMorphActive() === 0)
      Module.stableSync();
  });

  const offVolumeSet = bus.on('volume:set', ({ v }) => {
    volume.gain.value = v;
    state.volume = v;
  });

  const offBpmSet = bus.on('bpm:set', ({ v }) => {
    if (Module.fx_set_bpm) Module.fx_set_bpm(v);
    state.bpm = v;
  });

  const offOctaveSet = bus.on('octave:set', ({ shift, source }) => {
    setOctaveShift(shift, source);
  });

  const offHwfltType = bus.on('hwflt:type', ({ t }) => {
    state.hwflt.type = t;
    if (Module.setHwFilterType) Module.setHwFilterType(t);
  });

  const offHwfltParam = bus.on('hwflt:param', ({ slot, raw }) => {
    if (Module.setHwFilterParam) Module.setHwFilterParam(slot, raw);
    if (slot === 0) state.hwflt.cutoff = raw;
    else if (slot === 1) state.hwflt.res = raw;
    else if (slot === 2) state.hwflt.sweepF = raw / 1000.0;
    else state.hwflt.sweepAmt = Math.round(raw / 5.115 - 100);
  });

  const offFxmodTime = bus.on('fxmod:time', ({ v }) => {
    state.fxmod.time = v;
    if (Module.setFxParam) Module.setFxParam(0, 0, v);
  });

  const offFxmodDepth = bus.on('fxmod:depth', ({ v }) => {
    state.fxmod.depth = v;
    if (Module.setFxParam) Module.setFxParam(0, 1, v);
  });

  const offFxmodEdit = bus.on('fxmod:edit', ({ i, v }) => {
    state.fxmod.edit[i] = v;
    if (Module.modSetEditParam) Module.modSetEditParam(i, v);
  });

  const offFxmodTape = bus.on('fxmod:tape', ({ i, v }) => {
    state.fxmod.tape[i] = v;
    if (Module.modSetTapeParam) Module.modSetTapeParam(i, v);
  });

  const offFxdelSet = bus.on('fxdel:set', ({ which, raw }) => {
    if (which === 0) state.fxdel.time = raw;
    else if (which === 1) state.fxdel.depth = raw;
    else state.fxdel.mix = raw;
    if (Module.setFxParam) Module.setFxParam(1, which, raw);
  });

  const offFxrevSet = bus.on('fxrev:set', ({ which, raw }) => {
    if (which === 0) state.fxrev.time = raw;
    else if (which === 1) state.fxrev.depth = raw;
    else state.fxrev.mix = raw;
    if (Module.setFxParam) Module.setFxParam(2, which, raw);
  });

  const offStageBypass = bus.on('stage:bypass', ({ stage, bypass }) => {
    state.bypass[stage] = bypass;
    if (Module.setStageBypass) Module.setStageBypass(stage, bypass);
  });

  const offNoteOn = bus.on('note:on', ({ name, freq }) => {
    // Touch keyboard quirk (1:1 legacy keyDown): pressing an already-"held"
    // key (mouse still down from the first touch) never produced a keyup, so
    // the DSP never saw noteOff and its legato toggle-off could not fire.
    // Detect the re-press and synthesize the missing noteOff before noteOn.
    if (pressedNotes.has(name)) {
      Module.noteOff(name);
    }
    pressedNotes.add(name);
    frequencyStack.push(freq);
    osc_frequency = freq;
    Module.setOscPitch(osc_frequency);
    Module.noteOn(name, 100); // velocity: TODO slider (legacy)
  });

  const offNoteOff = bus.on('note:off', ({ name, freq }) => {
    pressedNotes.delete(name);
    // Remove the frequency from the stack (last matching entry).
    for (let i = frequencyStack.length - 1; i >= 0; --i) {
      if (approximatelyEqual(frequencyStack[i], freq)) {
        frequencyStack.splice(i, 1);
        break;
      }
    }
    // latest note priority
    if (frequencyStack.length > 0) {
      osc_frequency = frequencyStack[frequencyStack.length - 1];
      Module.setOscPitch(osc_frequency);
    }
    // No host-side mute on key-up: noteOff triggers the 2 s exp release.
    Module.noteOff(name);
  });

  const offPresetRandom = bus.on('preset:random', ({ mode }) => {
    fireRandom(mode);
  });

  const offPresetApply = bus.on('preset:apply', ({ name }) => {
    if (name === 'Reset') {
      applyPresetRaw(DEFAULT_RAW.slice(), 'Reset');
    } else {
      // Сохранённые пресеты (stage 4: /api/presets): apply the live state.
      applyPresetRaw(state.params.slice(), name);
    }
  });

  // ============================ presets: /api/presets (stage 4) ===============
  // The UI (OscPanel) holds the list in the `presetList` store (stores.ts) and
  // sends save/delete/refresh intents over the bus. core owns the HTTP calls
  // (single writer for the data file) and pushes 'preset:list' back so every
  // connected UI updates in one place.

  async function fetchPresetNames(): Promise<string[]> {
    try {
      const r = await fetch('/api/presets', { method: 'GET' });
      if (!r.ok) return [];
      const list = (await r.json()) as Array<{ name: string }>;
      return Array.isArray(list) ? list.map((p) => p.name).filter(Boolean) : [];
    } catch {
      return [];
    }
  }

  async function pushPresetList(): Promise<void> {
    bus.emit('preset:list', { names: await fetchPresetNames() });
  }

  const offPresetListRequest = bus.on('preset:list:request', () => {
    void pushPresetList();
  });

  const offPresetSave = bus.on('preset:save', async ({ name, params }) => {
    let ok = false;
    let error: string | undefined;
    try {
      const r = await fetch('/api/presets', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name, params }),
      });
      if (!r.ok) {
        error = `HTTP ${r.status}`;
      } else {
        ok = true;
      }
    } catch (e) {
      error = e instanceof Error ? e.message : 'network error';
    }
    bus.emit('preset:saveResult', { name, ok, error });
    if (ok) void pushPresetList();
  });

  const offPresetDelete = bus.on('preset:delete', async ({ name }) => {
    let ok = false;
    let error: string | undefined;
    try {
      const r = await fetch('/api/presets?name=' + encodeURIComponent(name), { method: 'DELETE' });
      if (!r.ok) {
        error = r.status === 404 ? 'not found' : `HTTP ${r.status}`;
      } else {
        ok = true;
      }
    } catch (e) {
      error = e instanceof Error ? e.message : 'network error';
    }
    bus.emit('preset:deleteResult', { name, ok, error });
    if (ok) void pushPresetList();
  });

  // Kick off the initial list load (fire-and-forget; the UI renders on
  // 'preset:list').
  void pushPresetList();

  // ============================ start-sync (core -> UI, one-shot) ============
  // Read the wasm state and publish it to the UI via :init events. stores.ts
  // subscribes at import time (before initAudio runs), so all :init events
  // are caught.
  for (let i = 0; i < EDIT_PARAM_COUNT; ++i) {
    const v = (Module.getEditParamValue) ? Module.getEditParamValue(i) : DEFAULT_RAW[i];
    state.params[i] = v;
    if (i === WAVE_PARAM_IDX) {
      bus.emit('radio:init', { group: 'WaveBank', idx: waveBankFromRaw(v) });
    } else if (i === WAVE2_PARAM_IDX) {
      bus.emit('radio:init', { group: 'WaveBank2', idx: waveBankFromRaw(v) });
    } else if (i === 40 || i === 42 || i === 46) {
      bus.emit('radio:init', {
        group: i === 40 ? 'LfoShape1' : (i === 46 ? 'LfoShape3' : 'LfoShape2'),
        idx: lfoShapeIdxFromRaw(v),
      });
    } else if (i === 49) {
      bus.emit('radio:init', { group: 'FmModWave', idx: fmModWaveIdxFromRaw(v) });
    } else {
      bus.emit('param:init', { i, v });
    }
  }
  // HW filter.
  state.hwflt.type = (Module.getHwFilterType) ? Module.getHwFilterType() : 0;
  bus.emit('radio:init', { group: 'HwFltType', idx: state.hwflt.type });
  state.hwflt.cutoff = (Module.getHwFilterParam) ? Module.getHwFilterParam(0) : 0;
  state.hwflt.res = (Module.getHwFilterParam) ? Module.getHwFilterParam(1) : 0;
  state.hwflt.sweepF = ((Module.getHwFilterParam) ? Module.getHwFilterParam(2) : 0) / 1000.0;
  state.hwflt.sweepAmt = Math.round(((Module.getHwFilterParam) ? Module.getHwFilterParam(3) : 511) / 5.115 - 100);
  // FX stages (MOD 0..1, DEL/REV 0..2).
  for (let which = 0; which <= 1; ++which) {
    const v = (Module.getFxParam) ? Module.getFxParam(0, which) : 0;
    if (which === 0) state.fxmod.time = v; else state.fxmod.depth = v;
  }
  for (let which = 0; which <= 2; ++which) {
    const vd = (Module.getFxParam) ? Module.getFxParam(1, which) : 0;
    if (which === 0) state.fxdel.time = vd;
    else if (which === 1) state.fxdel.depth = vd;
    else state.fxdel.mix = vd;
    const vr = (Module.getFxParam) ? Module.getFxParam(2, which) : 0;
    if (which === 0) state.fxrev.time = vr;
    else if (which === 1) state.fxrev.depth = vr;
    else state.fxrev.mix = vr;
  }
  for (let i = 0; i < 5; ++i) {
    state.fxmod.edit[i] = (Module.modGetEditParamValue) ? Module.modGetEditParamValue(i) : 0;
    state.fxmod.tape[i] = (Module.modGetTapeEditParamValue) ? Module.modGetTapeEditParamValue(i) : 0;
  }
  for (let stage = 0; stage <= 2; ++stage) {
    // active (in path) = bypass 0 = checked.
    state.bypass[stage] = (Module.getStageBypass && Module.getStageBypass(stage)) ? 1 : 0;
  }
  // BPM: fx_get_bpmf is NOT in the embind module (wasm.cc) -> default 120
  // (legacy started the slider at its markup value 120 too).
  state.bpm = 120;
  // Volume: legacy default 1.0 (full scale, matches the slider markup).
  volume.gain.value = state.volume;
  // Octave: legacy starts at -1 (A1..D3 = button 2).
  setOctaveShift(octaveShift, "radio");
  bus.emit('radio:init', { group: 'Octave', idx: octaveShift + 3 });

  // ============================ teardown =====================================

  const allOff: Array<() => void> = [
    offParamSet, offVolumeSet, offBpmSet, offOctaveSet,
    offHwfltType, offHwfltParam,
    offFxmodTime, offFxmodDepth, offFxmodEdit, offFxmodTape,
    offFxdelSet, offFxrevSet, offStageBypass,
    offNoteOn, offNoteOff, offPresetRandom, offPresetApply,
    offPresetListRequest, offPresetSave, offPresetDelete,
  ];

  function stop() {
    if (presetApplyTimer) { clearTimeout(presetApplyTimer); presetApplyTimer = null; }
    if (fireSyncTimer) { clearInterval(fireSyncTimer); fireSyncTimer = null; }
    if (scopeRafId) { cancelAnimationFrame(scopeRafId); scopeRafId = null; }
    if (scopeObserver) { scopeObserver.disconnect(); scopeObserver = null; }
    window.removeEventListener("click", autoResume, true);
    window.removeEventListener("keydown", autoResume, true);
    for (const off of allOff) off();
  }

  return {
    stop,
    attachScope,
    getParam: (i) => state.params[i],
    setParam: (i, v) => { bus.emit('param:set', { i, v }); },
    randomize: (mode) => { bus.emit('preset:random', { mode }); },
    applyPreset: (name, raws) => {
      if (name === 'Reset') applyPresetRaw(DEFAULT_RAW.slice(), 'Reset');
      else if (raws) applyPresetRaw(raws, name);
      else applyPresetRaw(state.params.slice(), name);
    },
  };
}
