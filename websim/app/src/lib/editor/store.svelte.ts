// ============================================================================
// Wave Table Editor — reactive store (Svelte 5 runes).
//
// A single EditorStore holds the whole document: 16 waves, each with up to four
// sources, a combine mode, two boolean modifiers, a per-source warp stack and
// an output warp stack. The store exposes the raw `waves` array (deeply
// reactive via $state) plus a set of helper functions that mutate it in a
// predictable, UI-friendly way.
//
// This module is STRUCTURE ONLY — it does not render any wave. The future
// renderer will read editor.waves and turn each WaveConfig into a polyline.
// ============================================================================

import {
	WAVE_COUNT,
	SOURCES_PER_WAVE,
	newSource,
	newWarp,
	defaultWarpParams,
	newWarpId,
	type WaveConfig,
	type SourceConfig,
	type SourceType,
	type CombineMode,
	type CombineModifiers,
	type WarpList,
	type WarpItem,
	type WarpType,
	type WarpParams,
} from './types';

// ---------------------------------------------------------------------------
// Factories (defaults)
// ---------------------------------------------------------------------------

/**
 * A wave with a single enabled sine source (harmonic = index+1 so the 16
 * default waves read as a closed->open harmonic morph), Sum combine, no
 * modifiers, and empty warp stacks.
 */
export function createDefaultWave(index: number): WaveConfig {
	const srcs: SourceConfig[] = [];
	for (let i = 0; i < SOURCES_PER_WAVE; i++) {
		if (i === 0) {
			srcs.push({ ...newSource('Sine', true), harmonic: index + 1 });
		} else {
			srcs.push(newSource('Sine', false));
		}
	}
	return {
		name: `Wave ${index + 1}`,
		sources: srcs,
		combine: 'Sum',
		modifiers: { invert: false, mirror: false },
		perSourceWarp: Array.from({ length: SOURCES_PER_WAVE }, () => [] as WarpList),
		outputWarp: [] as WarpList,
	};
}

/** All 16 default waves. */
export function createDefaultWaves(): WaveConfig[] {
	return Array.from({ length: WAVE_COUNT }, (_, i) => createDefaultWave(i));
}

// ---------------------------------------------------------------------------
// Warp-list helpers (operate on a WarpList in place; safe under $state)
// ---------------------------------------------------------------------------

function warpIndex(list: WarpList, id: string): number {
	return list.findIndex((w) => w.id === id);
}

function addToWarpList(list: WarpList, item: WarpItem): void {
	list.push(item);
}

function removeWarpFromList(list: WarpList, id: string): void {
	const i = warpIndex(list, id);
	if (i !== -1) list.splice(i, 1);
}

function moveWarpInList(list: WarpList, id: string, dir: -1 | 1): void {
	const i = warpIndex(list, id);
	const j = i + dir;
	if (i === -1 || j < 0 || j >= list.length) return;
	const [item] = list.splice(i, 1);
	list.splice(j, 0, item);
}

// ---------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------

export class EditorStore {
	/** The full document: 16 waves. Deeply reactive. */
	waves: WaveConfig[] = $state(createDefaultWaves());

	// ---- document -----------------------------------------------------------

	/** Replace the whole document with a freshly generated default one. */
	reset(): void {
		this.waves = createDefaultWaves();
	}

	/** Randomize every wave (sources, combine, modifiers, warps). */
	randomize(): void {
		this.waves = createRandomWaves();
	}

	// ---- wave-level ---------------------------------------------------------

	setWaveName(waveIndex: number, name: string): void {
		const w = this.waves[waveIndex];
		if (w) w.name = name;
	}

	// ---- source-level -------------------------------------------------------

	setSourceEnabled(waveIndex: number, srcIndex: number, enabled: boolean): void {
		const s = this.waves[waveIndex]?.sources[srcIndex];
		if (s) s.enabled = enabled;
	}

	setSourceType(waveIndex: number, srcIndex: number, type: SourceType): void {
		const s = this.waves[waveIndex]?.sources[srcIndex];
		if (s) s.type = type;
	}

	setSourceParam(
		waveIndex: number,
		srcIndex: number,
		key: 'harmonic' | 'phase' | 'gain' | 'bias',
		value: number
	): void {
		const s = this.waves[waveIndex]?.sources[srcIndex];
		if (s) s[key] = value;
	}

	// ---- combine / modifiers ------------------------------------------------

	setCombine(waveIndex: number, mode: CombineMode): void {
		const w = this.waves[waveIndex];
		if (w) w.combine = mode;
	}

	setModifier(waveIndex: number, key: keyof CombineModifiers, value: boolean): void {
		const w = this.waves[waveIndex];
		if (w) w.modifiers[key] = value;
	}

	// ---- warp stacks --------------------------------------------------------
	// target: 'output' -> outputWarp;  'source' + srcIndex -> perSourceWarp[srcIndex]

	warpList(waveIndex: number, target: 'output' | 'source', srcIndex = 0): WarpList {
		const w = this.waves[waveIndex];
		if (!w) return [] as WarpList;
		return target === 'output' ? w.outputWarp : w.perSourceWarp[srcIndex];
	}

	addWarp(waveIndex: number, target: 'output' | 'source', type: WarpType, srcIndex = 0): WarpItem {
		const list = this.warpList(waveIndex, target, srcIndex);
		const item = newWarp(type);
		addToWarpList(list, item);
		return item;
	}

	removeWarp(waveIndex: number, target: 'output' | 'source', id: string, srcIndex = 0): void {
		const list = this.warpList(waveIndex, target, srcIndex);
		removeWarpFromList(list, id);
	}

	/**
	 * Change a warp's type in place. The item keeps its `id`, `enabled` flag and
	 * stack position; its `params` are reset to the defaults of the new type
	 * (parameters differ between warp types, so old values are dropped).
	 */
	setWarpType(waveIndex: number, target: 'output' | 'source', id: string, type: WarpType, srcIndex = 0): void {
		const list = this.warpList(waveIndex, target, srcIndex);
		const w = list.find((x) => x.id === id);
		if (w) {
			w.type = type;
			w.params = defaultWarpParams(type);
		}
	}

	setWarpEnabled(waveIndex: number, target: 'output' | 'source', id: string, enabled: boolean, srcIndex = 0): void {
		const list = this.warpList(waveIndex, target, srcIndex);
		const w = list.find((x) => x.id === id);
		if (w) w.enabled = enabled;
	}

	setWarpParam(
		waveIndex: number,
		target: 'output' | 'source',
		id: string,
		key: string,
		value: number | string,
		srcIndex = 0
	): void {
		const list = this.warpList(waveIndex, target, srcIndex);
		const w = list.find((x) => x.id === id);
		if (w) w.params[key] = value;
	}

	/** Replace a warp item's params (shallow-merged). */
	setWarpParams(
		waveIndex: number,
		target: 'output' | 'source',
		id: string,
		params: WarpParams,
		srcIndex = 0
	): void {
		const list = this.warpList(waveIndex, target, srcIndex);
		const w = list.find((x) => x.id === id);
		if (w) w.params = { ...w.params, ...params };
	}

	/** Reorder a warp within its stack (dir = -1 up, +1 down). */
	moveWarp(
		waveIndex: number,
		target: 'output' | 'source',
		id: string,
		dir: -1 | 1,
		srcIndex = 0
	): void {
		const list = this.warpList(waveIndex, target, srcIndex);
		moveWarpInList(list, id, dir);
	}
}

/** The shared singleton the /editor route (and UI) consumes. */
export const editor = new EditorStore();

// ---------------------------------------------------------------------------
// Randomization (for `randomize()`)
// ---------------------------------------------------------------------------

function pick<T>(arr: readonly T[]): T {
	return arr[Math.floor(Math.random() * arr.length)];
}

function createRandomWaves(): WaveConfig[] {
	return Array.from({ length: WAVE_COUNT }, (_, i) => {
		const nSrc = 1 + Math.floor(Math.random() * SOURCES_PER_WAVE); // 1..4
		const sources = Array.from({ length: SOURCES_PER_WAVE }, (_, k) => {
			if (k < nSrc) {
				const s = newSource(
					pick(['Sine', 'Saw', 'Triangle', 'Square', 'Sine²', 'Parabola']),
					true
				);
				s.harmonic = 1 + Math.floor(Math.random() * 8);
				return s;
			}
			return newSource('Sine', false);
		});
		const outputWarp =
			Math.random() < 0.7
				? [newWarp(pick(['Wavefold', 'Waveshape', 'Bitcrush', 'Ring Mod']))]
				: ([] as WarpItem[]);
		return {
			name: `Wave ${i + 1}`,
			sources,
			combine: pick(['Sum', 'Sum', 'Multiply', 'AM', 'Max', 'Min', 'Wrap']),
			modifiers: { invert: Math.random() < 0.25, mirror: Math.random() < 0.25 },
			perSourceWarp: sources.map(() => [] as WarpList),
			outputWarp,
		};
	});
}

// Re-exported for the UI to key new items if it builds them locally.
export { newWarpId };