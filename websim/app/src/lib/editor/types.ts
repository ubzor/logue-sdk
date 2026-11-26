// ============================================================================
// Wave Table Editor — data model (the single source of truth for the editor).
//
// A "document" is an array of 16 waves. Each wave is built from up to FOUR
// sources that are COMBINED by one of six methods (with two boolean modifiers
// Invert / Mirror), and the result can be run through an ORDERED list of warps
// (the "warp stack"). The same warp stack can be applied to each source
// individually AND to the combined output — so a wave is:
//
//     for each source:  shape(t; harmonic,phase,gain,bias) -> perSourceWarp[i]
//     combined = combine(4 sources)  [Sum | Multiply | Min | Max | AM | Wrap]
//     combined = mirror ? mirror(combined) : combined
//     combined = invert ? -combined   : combined
//     output   = outputWarp.reduce(apply, combined)
//
// This file is TYPES + METADATA + constants only. No Svelte reactivity here —
// the reactive store lives in store.svelte.ts. The metadata (source/warp
// catalogs, param specs, defaults) is what the docs generator and the future
// renderer consume, so keep it in sync with docs/editor-model.md.
// ============================================================================

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/** Number of waves (morph frames / polylines) in the editor document. */
export const WAVE_COUNT = 16;

/** Number of source slots per wave (up to four are usable; unused -> disabled). */
export const SOURCES_PER_WAVE = 4;

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

/** The 30 usable source shapes. */
export type SourceType =
	| 'Sine'
	| 'Triangle'
	| 'Saw'
	| 'Square'
	| 'Pulse'
	| 'Half Sine'
	| 'Parabola'
	| 'Bell'
	| 'S-Curve'
	| 'Trapezoid'
	| 'Stairs'
	| 'Sine²'
	| 'FM Sine'
	| 'Sinc'
	| 'Chirp'
	| 'Wave Pkt'
	| 'Smooth Sq'
	| 'Sharkfin'
	| 'Spike'
	| 'Noise'
	| 'Cubed'
	| 'Two Bumps'
	| 'Folded'
	| 'Asym Tri'
	| 'Pinch'
	| 'PD Sine'
	| 'Organ'
	| 'Sync Saw'
	| 'Bit Crush'
	| 'Ring Sine';

export const SOURCE_TYPES: readonly SourceType[] = [
	'Sine', 'Triangle', 'Saw', 'Square', 'Pulse', 'Half Sine', 'Parabola',
	'Bell', 'S-Curve', 'Trapezoid', 'Stairs', 'Sine²', 'FM Sine', 'Sinc',
	'Chirp', 'Wave Pkt', 'Smooth Sq', 'Sharkfin', 'Spike', 'Noise', 'Cubed',
	'Two Bumps', 'Folded', 'Asym Tri', 'Pinch', 'PD Sine', 'Organ',
	'Sync Saw', 'Bit Crush', 'Ring Sine',
] as const;

/**
 * One source within a wave. Always present (4 slots per wave); `enabled=false`
 * means "this slot is empty" (the renderer skips it / treats it as zero).
 * Every source has the same four numeric settings.
 */
export interface SourceConfig {
	enabled: boolean;
	type: SourceType;
	/** Dominant partial / repetition factor (1-based). */
	harmonic: number;
	/** Phase offset in [0, 1). */
	phase: number;
	/** Amplitude (multiplier). */
	gain: number;
	/** Vertical DC offset (adds to the wave before the combine step). */
	bias: number;
}

// ---------------------------------------------------------------------------
// Combine
// ---------------------------------------------------------------------------

/** How the (up to) four source signals are merged into one. */
export type CombineMode = 'Sum' | 'Multiply' | 'Min' | 'Max' | 'AM' | 'Wrap';

export const COMBINE_MODES: readonly CombineMode[] = [
	'Sum', 'Multiply', 'Min', 'Max', 'AM', 'Wrap',
] as const;

/** The two boolean modifiers applied to the combined signal. */
export interface CombineModifiers {
	/** Flip the combined signal vertically (multiply by -1). */
	invert: boolean;
	/** Mirror the combined signal horizontally (t -> 1 - t). */
	mirror: boolean;
}

// ---------------------------------------------------------------------------
// Warps
// ---------------------------------------------------------------------------

/** The 15 warp types available in a warp stack. */
export type WarpType =
	| 'Wavefold'
	| 'Hard Sync'
	| 'FM'
	| 'Waveshape'
	| 'Bitcrush'
	| 'Exponent'
	| 'CZ Reso'
	| 'Bend'
	| 'PWM'
	| 'Asym'
	| 'Formant'
	| 'Spectral Tilt'
	| 'Ring Mod'
	| 'Additive Shape'
	| 'Math';

export const WARP_TYPES: readonly WarpType[] = [
	'Wavefold', 'Hard Sync', 'FM', 'Waveshape', 'Bitcrush', 'Exponent',
	'CZ Reso', 'Bend', 'PWM', 'Asym', 'Formant', 'Spectral Tilt', 'Ring Mod',
	'Additive Shape', 'Math',
] as const;

export type WaveshapeMode = 'Tahn' | 'Atan' | 'Cubic' | 'Sin';
export const WAVESHAPE_MODES: readonly WaveshapeMode[] = ['Tahn', 'Atan', 'Cubic', 'Sin'] as const;

export type CzWindow = 'Saw' | 'Tri' | 'Trap';
export const CZ_WINDOWS: readonly CzWindow[] = ['Saw', 'Tri', 'Trap'] as const;

/**
 * A warp instance. `params` is a flat map keyed by the param names from
 * WARP_META (e.g. `{ Gain: 1, Bias: 0 }`, `{ Mode: 'Tahn', Drive: 0.5 }`).
 * Numeric params are plain numbers; the few enumerated params (Waveshape Mode,
 * CZ Reso Window) hold the string from the matching MODES/WINDOWS list.
 */
export type WarpParams = Record<string, number | string>;

export interface WarpItem {
	/** Stable unique id (used by the UI as a DOM key / for reorder). */
	id: string;
	type: WarpType;
	enabled: boolean;
	params: WarpParams;
}

/** An ordered list of warps — order matters (applied top-to-bottom). */
export type WarpList = WarpItem[];

// ---------------------------------------------------------------------------
// Metadata catalogs (param specs, labels, defaults)
// ---------------------------------------------------------------------------

/**
 * Declares one warp parameter: its name, kind and (for numbers) its range and
 * default. `kind` is 'number' or a reference to a string enum (mode/window).
 * This table drives UI generation and docs, and gives the store its defaults.
 */
export interface ParamSpec {
	key: string;
	kind: 'number' | 'mode' | 'window' | 'math';
	min?: number;
	max?: number;
	step?: number;
	def: number | string;
	/** Human label (defaults to `key`). */
	label?: string;
}

export interface WarpMeta {
	type: WarpType;
	label: string;
	params: ParamSpec[];
	/** Short one-line description (docs). */
	desc?: string;
}

/**
 * Full catalog of the 15 warps with their parameter specs + defaults.
 * Keep in sync with docs/editor-model.md.
 */
export const WARP_META: Record<WarpType, WarpMeta> = {
	Wavefold: {
		type: 'Wavefold', label: 'Wavefold',
		desc: 'Hard-folds the signal so it stays within ±1 (wavefolding).',
		params: [
			{ key: 'Gain', kind: 'number', min: 0, max: 4, step: 0.01, def: 1 },
			{ key: 'Bias', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 },
		],
	},
	'Hard Sync': {
		type: 'Hard Sync', label: 'Hard Sync',
		desc: 'Phase-locks the signal to a harmonic multiple (synchronization).',
		params: [{ key: 'Ratio', kind: 'number', min: 0, max: 4, step: 0.01, def: 1 }],
	},
	FM: {
		type: 'FM', label: 'FM',
		desc: 'Frequency-modulates the carrier with itself (or a phase-shifted copy).',
		params: [
			{ key: 'Ratio', kind: 'number', min: 0, max: 8, step: 0.01, def: 1 },
			{ key: 'Index', kind: 'number', min: 0, max: 4, step: 0.01, def: 0.5 },
		],
	},
	Waveshape: {
		type: 'Waveshape', label: 'Waveshape',
		desc: 'Shapes the waveform with a parametric curve (tanh / atan / cubic / sin).',
		params: [
			{ key: 'Mode', kind: 'mode', def: 'Tahn' },
			{ key: 'Drive', kind: 'number', min: 0, max: 4, step: 0.01, def: 1 },
		],
	},
	Bitcrush: {
		type: 'Bitcrush', label: 'Bitcrush',
		desc: 'Quantizes amplitude to a fixed number of bits and/or rate.',
		params: [
			{ key: 'Bits', kind: 'number', min: 1, max: 16, step: 1, def: 8 },
			{ key: 'Rate', kind: 'number', min: 0, max: 1, step: 0.01, def: 1 },
		],
	},
	Exponent: {
		type: 'Exponent', label: 'Exponent',
		desc: 'Raises the (rectified) signal to a power (soft clipping / harmonic tilt).',
		params: [{ key: 'Power', kind: 'number', min: 0.1, max: 8, step: 0.01, def: 1 }],
	},
	'CZ Reso': {
		type: 'CZ Reso', label: 'CZ Reso',
		desc: 'One-pole resonant filter (Zolnerk / CZ style) with a selectable window.',
		params: [
			{ key: 'Reso', kind: 'number', min: 0, max: 1, step: 0.01, def: 0.5 },
			{ key: 'Window', kind: 'window', def: 'Saw' },
		],
	},
	Bend: {
		type: 'Bend', label: 'Bend',
		desc: 'Bends the phase (time-warp) of the signal by a fixed amount.',
		params: [{ key: 'Amount', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 }],
	},
	PWM: {
		type: 'PWM', label: 'PWM',
		desc: 'Pulse-width modulates the signal across the period.',
		params: [{ key: 'Width', kind: 'number', min: 0, max: 1, step: 0.01, def: 0.5 }],
	},
	Asym: {
		type: 'Asym', label: 'Asym',
		desc: 'Skews the waveform asymmetrically around the zero line.',
		params: [{ key: 'Amount', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 }],
	},
	Formant: {
		type: 'Formant', label: 'Formant',
		desc: 'Re-emphasizes a band of partials (vowel-like formant filter).',
		params: [{ key: 'Shift', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 }],
	},
	'Spectral Tilt': {
		type: 'Spectral Tilt', label: 'Spectral Tilt',
		desc: 'Tilts the spectrum (darken/brighten), blended with the original.',
		params: [
			{ key: 'Tilt', kind: 'number', min: 0, max: 2, step: 0.01, def: 0.5 },
			{ key: 'Mix', kind: 'number', min: 0, max: 1, step: 0.01, def: 1 },
		],
	},
	'Ring Mod': {
		type: 'Ring Mod', label: 'Ring Mod',
		desc: 'Ring-modulates the signal with a sine of a given frequency.',
		params: [
			{ key: 'Freq', kind: 'number', min: 0, max: 8, step: 0.01, def: 1 },
			{ key: 'Depth', kind: 'number', min: 0, max: 1, step: 0.01, def: 1 },
		],
	},
	'Additive Shape': {
		type: 'Additive Shape', label: 'Additive Shape',
		desc: 'Adds a stack of up to 8 harmonics (per-partial levels) to the signal.',
		params: [
			{ key: '1', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 },
			{ key: '2', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 },
			{ key: '3', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 },
			{ key: '4', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 },
			{ key: '5', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 },
			{ key: '6', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 },
			{ key: '7', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 },
			{ key: '8', kind: 'number', min: -1, max: 1, step: 0.01, def: 0 },
			{ key: 'Mix', kind: 'number', min: 0, max: 1, step: 0.01, def: 0.5 },
		],
	},
	Math: {
		type: 'Math', label: 'Math',
		desc: 'A user function of the normalized phase T and the sample value V.',
		params: [{ key: 'F', kind: 'math', def: 'V' }],
	},
};

/** Param spec for the four per-source settings (shared by all sources). */
export const SOURCE_PARAM_SPECS: ParamSpec[] = [
	// harmonic: min 0.25 (= f/4) so sources can be SUBHARMONICS of the note
	// (a fractional harmonic < 1 reads as a sub-octave partial of the SAME
	// note — the drone's low end now lives inside the wavetable). step 0.25
	// snaps to the 8-bit wire quant (16/255 ≈ 0.0625; 0.25/0.5/0.75/1/2/...
	// all round-trip stably). 1.0 = the note itself, >1 = upper harmonics.
	{ key: 'harmonic', kind: 'number', min: 0.25, max: 16, step: 0.25, def: 1, label: 'Harmonic' },
	{ key: 'phase', kind: 'number', min: 0, max: 1, step: 0.01, def: 0, label: 'Phase' },
	{ key: 'gain', kind: 'number', min: 0, max: 2, step: 0.01, def: 1, label: 'Gain' },
	{ key: 'bias', kind: 'number', min: -1, max: 1, step: 0.01, def: 0, label: 'Bias' },
];

// ---------------------------------------------------------------------------
// Wave / document
// ---------------------------------------------------------------------------

export interface WaveConfig {
	/** Display name of the wave (editable). */
	name: string;
	/** Up to four source slots (fixed length SOURCES_PER_WAVE). */
	sources: SourceConfig[];
	/** How the sources are merged. */
	combine: CombineMode;
	/** Boolean modifiers on the combined signal. */
	modifiers: CombineModifiers;
	/** Warp stack applied to EACH source individually (parallel to `sources`). */
	perSourceWarp: WarpList[];
	/** Warp stack applied to the combined output (after combine + modifiers). */
	outputWarp: WarpList;
}

export interface EditorDocument {
	waves: WaveConfig[];
}

// ---------------------------------------------------------------------------
// Small helpers (pure, no Svelte)
// ---------------------------------------------------------------------------

let __id = 0;

/** Generate a stable unique id for a warp item. */
export function newWarpId(): string {
	__id += 1;
	return `w${Date.now().toString(36)}_${__id}`;
}

/** Build the default `params` object for a warp type (from WARP_META). */
export function defaultWarpParams(type: WarpType): WarpParams {
	const out: WarpParams = {};
	for (const p of WARP_META[type].params) out[p.key] = p.def;
	return out;
}

/** Build a fresh enabled WarpItem with default params for the given type. */
export function newWarp(type: WarpType): WarpItem {
	return { id: newWarpId(), type, enabled: true, params: defaultWarpParams(type) };
}

/** Build a fresh source slot (defaults from SOURCE_PARAM_SPECS). */
export function newSource(type: SourceType = 'Sine', enabled = true): SourceConfig {
	const spec = Object.fromEntries(SOURCE_PARAM_SPECS.map((s) => [s.key, s.def])) as Record<
		string,
		number
	>;
	return {
		enabled,
		type,
		harmonic: spec.harmonic,
		phase: spec.phase,
		gain: spec.gain,
		bias: spec.bias,
	};
}
