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
	/**
	 * Revision counter: incremented on every mutation so that external
	 * consumers (e.g. the 3D view's $effect in +page.svelte) can react to
	 * changes without having to read every nested field manually.
	 */
	revision = $state(0);

	/** Bump the revision (call at the end of every mutating method). */
	private bump(): void {
		this.revision += 1;
	}

	// ---- document -----------------------------------------------------------

	/** Replace the whole document with a freshly generated default one. */
	reset(): void {
		this.waves = createDefaultWaves();
		this.bump();
	}

	/** Randomize every wave (sources, combine, modifiers, warps). */
	randomize(): void {
		this.waves = createRandomWaves();
		this.bump();
	}

	// ---- wave-level ---------------------------------------------------------

	setWaveName(waveIndex: number, name: string): void {
		const w = this.waves[waveIndex];
		if (w) {
			w.name = name;
			this.bump();
		}
	}

	// ---- source-level -------------------------------------------------------

	setSourceEnabled(waveIndex: number, srcIndex: number, enabled: boolean): void {
		const s = this.waves[waveIndex]?.sources[srcIndex];
		if (s) {
			s.enabled = enabled;
			this.bump();
		}
	}

	setSourceType(waveIndex: number, srcIndex: number, type: SourceType): void {
		const s = this.waves[waveIndex]?.sources[srcIndex];
		if (s) {
			s.type = type;
			this.bump();
		}
	}

	setSourceParam(
		waveIndex: number,
		srcIndex: number,
		key: 'harmonic' | 'phase' | 'gain' | 'bias',
		value: number
	): void {
		const s = this.waves[waveIndex]?.sources[srcIndex];
		if (s) {
			s[key] = value;
			this.bump();
		}
	}

	// ---- combine / modifiers ------------------------------------------------

	setCombine(waveIndex: number, mode: CombineMode): void {
		const w = this.waves[waveIndex];
		if (w) {
			w.combine = mode;
			this.bump();
		}
	}

	setModifier(waveIndex: number, key: keyof CombineModifiers, value: boolean): void {
		const w = this.waves[waveIndex];
		if (w) {
			w.modifiers[key] = value;
			this.bump();
		}
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
		this.bump();
		return item;
	}

	removeWarp(waveIndex: number, target: 'output' | 'source', id: string, srcIndex = 0): void {
		const list = this.warpList(waveIndex, target, srcIndex);
		removeWarpFromList(list, id);
		this.bump();
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
			this.bump();
		}
	}

	setWarpEnabled(waveIndex: number, target: 'output' | 'source', id: string, enabled: boolean, srcIndex = 0): void {
		const list = this.warpList(waveIndex, target, srcIndex);
		const w = list.find((x) => x.id === id);
		if (w) {
			w.enabled = enabled;
			this.bump();
		}
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
		if (w) {
			w.params[key] = value;
			this.bump();
		}
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
		if (w) {
			w.params = { ...w.params, ...params };
			this.bump();
		}
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
		this.bump();
	}
}

/** The shared singleton the /editor route (and UI) consumes. */
export const editor = new EditorStore();

// ---------------------------------------------------------------------------
// Send the document to the WASM OSC (debounced)
// ---------------------------------------------------------------------------

import { serializeWteDoc } from './serialize';
import { audioReady } from '../audio/glue-bridge';

let _sendTimer: ReturnType<typeof setTimeout> | null = null;
let _sendPending = false;

/**
 * Schedule a send of the current editor document to the WASM OSC.
 * Debounced: at most one setWteDocument call per 100 ms. The actual
 * 16x256 table regeneration happens at K-rate in blockPrepass (audio
 * thread), so the main thread only does the (cheap) serialization +
 * one embind call.
 */
export function scheduleSendToUnit(): void {
  if (_sendPending) return;
  _sendPending = true;
  if (_sendTimer) clearTimeout(_sendTimer);
  _sendTimer = setTimeout(async () => {
    _sendPending = false;
    _sendTimer = null;
    try {
      const { Module } = await audioReady;
      const buf = serializeWteDoc(editor.waves);
      // embind does NOT auto-convert JS arrays to std::vector<float> —
      // we must construct an embind FloatArray instance (registered via
      // register_vector<float>("FloatArray") in wasm.cc) and fill it.
      const FA = (Module as any).FloatArray;
      if (typeof FA !== 'function') throw new Error('FloatArray embind class not found');
      const fa = new FA();
      fa.resize(buf.length);
      for (let i = 0; i < buf.length; i++) fa.set(i, buf[i]);
      Module.setWteDocument(fa);
    } catch (e) {
      console.warn('[wte] setWteDocument failed:', e);
    }
  }, 100);
}

/**
 * Call this from the UI whenever the editor document changes (e.g. in a
 * $effect watching editor.revision). It debounces and sends the whole
 * document in one shot.
 */
export function onDocChange(): void {
  scheduleSendToUnit();
}

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