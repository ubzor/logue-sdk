// Wave Table Editor — renderer (WaveConfig -> polyline points).
// Turns ONE wave into a 1D polyline of `points` samples in [-1, +1].
// Pure module: no Svelte, no three.js, no DOM. Deterministic.

import type { WaveConfig, SourceType, CombineMode, WarpItem } from '../editor/types';

export const EDITOR_FRAMES = 16;
export const EDITOR_POINTS = 256;
const TAU = Math.PI * 2;

function frac(x: number): number { return x - Math.floor(x); }
function clip(x: number, lo: number, hi: number): number { return x < lo ? lo : x > hi ? hi : x; }
function tri(t: number): number { return 1 - 4 * Math.abs(frac(t) - 0.5); }
function mulberry32(seed: number): () => number {
	let a = seed >>> 0;
	return () => {
		a = (a + 0x6d2b79f5) | 0;
		let t = Math.imul(a ^ (a >>> 15), 1 | a);
		t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
		return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
	};
}

type SourceFn = (t: number, harmonic: number, phase: number) => number;
const SOURCE_FNS: Record<SourceType, SourceFn> = {
	Sine: (t, h, p) => Math.sin(TAU * (h * t + p)),
	Triangle: (t, h, p) => tri(h * t + p),
	Saw: (t, h, p) => 2 * frac(h * t + p) - 1,
	Square: (t, h, p) => (Math.sin(TAU * (h * t + p)) >= 0 ? 1 : -1),
	Pulse: (t, h, p) => (frac(h * t + p) < 0.5 ? 1 : -1),
	'Half Sine': (t, h, p) => Math.max(0, Math.sin(TAU * (h * t + p))) - 0.5,
	Parabola: (t, h, p) => 1 - 8 * Math.pow(frac(h * t + p) - 0.5, 2),
	Bell: (t, h, p) => { const x = Math.abs(2 * frac(h * t + p) - 1); return 1 - x * x; },
	'S-Curve': (t, h, p) => {
		const u = frac(h * t + p);
		return (2 / Math.PI) * Math.atan(2 * Math.tan(Math.PI * (u - 0.5)));
	},
	Trapezoid: (t, h, p) => {
		const u = frac(h * t + p);
		if (u < 0.25) return u * 4 - 1;
		if (u < 0.75) return 1;
		return 2 - u * 4;
	},
	Stairs: (t, h, p) => Math.round(4 * frac(h * t + p) - 0.5) * 0.5,
	'Sine²': (t, h, p) => Math.pow(Math.sin(TAU * (h * t + p)), 2) - 0.5,
	'FM Sine': (t, h, p) => {
		const x = h * t + p;
		return Math.sin(TAU * x + 0.5 * Math.sin(2 * TAU * x));
	},
	Sinc: (t, h, p) => {
		const x = 2 * (h * t + p) - 1;
		if (Math.abs(x) < 1e-6) return 1;
		return Math.sin(Math.PI * x) / (Math.PI * x);
	},
	Chirp: (t, h, p) => { const x = h * t + p; return Math.sin(TAU * (x + 0.5 * x * x)); },
	'Wave Pkt': (t, h, p) => Math.exp(-8 * Math.pow(t - 0.5, 2)) * Math.sin(TAU * (h * t + p)),
	'Smooth Sq': (t, h, p) => {
		const u = frac(h * t + p);
		return (2 / Math.PI) * Math.atan(8 * Math.tan(Math.PI * (u - 0.5)));
	},
	Sharkfin: (t, h, p) => {
		const u = frac(h * t + p);
		return u < 0.5 ? Math.pow(u * 2, 0.3) * 0.5 - 0.5 : Math.pow((1 - u) * 2, 3) * 0.5 - 0.5;
	},
	Spike: (t, h, p) => { const u = frac(h * t + p); return 1 - Math.pow(Math.abs(2 * u - 1), 8); },
	// Noise is overridden in renderWave() with a seeded version.
	Noise: () => 0,
	Cubed: (t, h, p) => { const v = Math.sin(TAU * (h * t + p)); return Math.sign(v) * Math.pow(Math.abs(v), 3); },
	'Two Bumps': (t, h, p) => {
		const u = frac(h * t + p);
		return Math.exp(-32 * Math.pow(u - 0.25, 2)) + Math.exp(-32 * Math.pow(u - 0.75, 2)) - 0.5;
	},
	Folded: (t, h, p) => { const x = h * t + p; return 1 - Math.abs(2 * frac(x) - 1); },
	'Asym Tri': (t, h, p) => {
		const u = frac(h * t + p);
		return u < 0.3 ? (u / 0.3) * 2 - 1 : ((0.7 - u) / 0.7) * 2 - 1;
	},
	Pinch: (t, h, p) => { const v = Math.sin(TAU * (h * t + p)); return Math.sign(v) * Math.pow(Math.abs(v), 0.5); },
	'PD Sine': (t, h, p) => { const x = h * t + p; const v = Math.sin(TAU * x); return Math.sin(TAU * x + 0.3 * v); },
	Organ: (t, h, p) => {
		const x = h * t + p;
		return (Math.sin(TAU * x) + 0.5 * Math.sin(2 * TAU * x) + 0.33 * Math.sin(3 * TAU * x)) / 1.83;
	},
	'Sync Saw': (t, h, p) => { const x = h * t + p; return (2 * frac(2 * x) - 1) - (2 * frac(x) - 1); },
	'Bit Crush': (t, h, p) => { const v = Math.sin(TAU * (h * t + p)); return Math.round(v * 8) / 8; },
	'Ring Sine': (t, h, p) => { const x = h * t + p; return Math.sin(TAU * x) * Math.sin(3 * TAU * x); },
};

type WarpFn = (t: number, v: number, item: WarpItem) => number;
const WARP_FNS: Record<string, WarpFn> = {
	Wavefold: (_t, v, item) => {
		const gain = (item.params['Gain'] as number) ?? 1;
		const bias = (item.params['Bias'] as number) ?? 0;
		return 1 - Math.abs(2 * frac((v * gain + bias + 1) / 2) - 1);
	},
	'Hard Sync': (t, _v, item) => {
		const ratio = (item.params['Ratio'] as number) ?? 1;
		return Math.sin(TAU * ratio * t);
	},
	FM: (t, _v, item) => {
		const ratio = (item.params['Ratio'] as number) ?? 1;
		const index = (item.params['Index'] as number) ?? 0.5;
		return Math.sin(TAU * (t + index * Math.sin(TAU * ratio * t)));
	},
	Waveshape: (_t, v, item) => {
		const mode = (item.params['Mode'] as string) ?? 'Tahn';
		const drive = (item.params['Drive'] as number) ?? 1;
		switch (mode) {
			case 'Tahn': return Math.tanh(drive * v);
			case 'Atan': return (2 / Math.PI) * Math.atan(drive * v);
			case 'Cubic': return v * (1 - (v * v) / 3);
			case 'Sin': return Math.sin((Math.PI / 2) * v);
			default: return v;
		}
	},
	Bitcrush: (_t, v, item) => {
		const bits = Math.round((item.params['Bits'] as number) ?? 8);
		const levels = 1 << (bits - 1);
		return Math.round(v * levels) / levels;
	},
	Exponent: (_t, v, item) => {
		const power = (item.params['Power'] as number) ?? 1;
		return Math.sign(v) * Math.pow(Math.abs(v), power);
	},
	'CZ Reso': (_t, v, item) => {
		const reso = (item.params['Reso'] as number) ?? 0.5;
		return v * (1 - reso * 0.5);
	},
	Bend: (t, _v, item) => {
		const amount = (item.params['Amount'] as number) ?? 0;
		return Math.sin(TAU * (t + amount * Math.sin(TAU * t) * 0.25));
	},
	PWM: (t, v, item) => {
		const width = (item.params['Width'] as number) ?? 0.5;
		return frac(t) < width ? v : -v;
	},
	Asym: (_t, v, item) => {
		const amount = (item.params['Amount'] as number) ?? 0;
		const scaled = v > 0 ? v * (1 + amount) : v * (1 - amount);
		return scaled / Math.max(1 + amount, 1 - amount);
	},
	Formant: (_t, v, item) => {
		const shift = (item.params['Shift'] as number) ?? 0;
		return v * (1 + shift * 0.5);
	},
	'Spectral Tilt': (_t, v, item) => {
		const tilt = (item.params['Tilt'] as number) ?? 0.5;
		const mix = (item.params['Mix'] as number) ?? 1;
		return v * (1 - mix) + v * (1 - tilt * 0.5) * mix;
	},
	'Ring Mod': (t, v, item) => {
		const freq = (item.params['Freq'] as number) ?? 1;
		const depth = (item.params['Depth'] as number) ?? 1;
		return v * (1 - depth + depth * Math.sin(TAU * freq * t));
	},
	'Additive Shape': (t, v, item) => {
		const mix = (item.params['Mix'] as number) ?? 0.5;
		let additive = 0;
		for (let k = 1; k <= 8; k++) {
			const a = (item.params[String(k)] as number) ?? 0;
			additive += a * Math.sin(TAU * k * t);
		}
		return v * (1 - mix) + (v + additive) * mix;
	},
	// TODO: Math warp — implement a tokenizer + parser (no eval) in a future
	// pass. For now, pass through unchanged.
	Math: (_t, v) => v,
};

function combineSources(values: number[], mode: CombineMode): number {
	if (values.length === 0) return 0;
	if (values.length === 1) return values[0];
	switch (mode) {
		case 'Sum': return values.reduce((a, b) => a + b, 0);
		case 'Multiply': return values.reduce((a, b) => a * b, 1);
		case 'Min': return Math.min(...values);
		case 'Max': return Math.max(...values);
		case 'AM': return values[0] * (0.5 + 0.5 * values[1]);
		case 'Wrap': return 2 * frac(values.reduce((a, b) => a + b, 0)) - 1;
		default: return values.reduce((a, b) => a + b, 0);
	}
}

export function renderWave(config: WaveConfig, points: number, frameIndex = 0): number[] {
	const row = new Array(points);
	for (let i = 0; i < points; i++) {
		const t = i / Math.max(1, points - 1);
		const srcVals: number[] = [];
		for (let s = 0; s < config.sources.length; s++) {
			const src = config.sources[s];
			if (!src.enabled) continue;
			let v: number;
			if (src.type === 'Noise') {
				const seed = frameIndex * 7919 + src.harmonic * 131;
				const rng = mulberry32(seed + Math.floor(t * 256));
				v = rng() * 2 - 1;
			} else {
				v = SOURCE_FNS[src.type](t, src.harmonic, src.phase);
			}
			v = v * src.gain + src.bias;
			const warpList = config.perSourceWarp[s] ?? [];
			for (const w of warpList) {
				if (!w.enabled) continue;
				const fn = WARP_FNS[w.type];
				if (fn) v = fn(t, v, w);
			}
			srcVals.push(v);
		}
		let out = combineSources(srcVals, config.combine);
		if (config.modifiers.invert) out = -out;
		for (const w of config.outputWarp) {
			if (!w.enabled) continue;
			const fn = WARP_FNS[w.type];
			if (fn) out = fn(t, out, w);
		}
		row[i] = clip(out, -1, 1);
	}
	if (config.modifiers.mirror) {
		for (let i = 0; i < points / 2; i++) {
			const j = points - 1 - i;
			const tmp = row[i];
			row[i] = row[j];
			row[j] = tmp;
		}
	}
	return row;
}

export function makeWavetable(waves: WaveConfig[], points: number): number[][] {
	return waves.map((w, i) => renderWave(w, points, i));
}