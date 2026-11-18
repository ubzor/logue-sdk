// Svelte 5 reactive state for the drone-chain simulator (Вариант B).
//
// Svelte stores (writable) — единственный источник UI-состояния. Компоненты
// читают через $audioState (auto-subscribe) и пишут НАМЕРЕНИЯ через store-
// хелперы (bus.emit -> core.ts -> wasm). Внешние изменения (старт-синк,
// preset apply, rnd-morph) core пушит событиями 'param:init'/'param:ext'/
// 'radio:init'/'radio:ext'/'preset:active'/'morph:highlight', которые этот
// модуль превращает в store-обновления.
//
// Подписки на bus ставятся при import (client-side) — ДО initAudio, поэтому
// все :init события core ловятся.

import { writable } from 'svelte/store';
import { browser } from '$app/environment';
import { bus } from './bus';
import type { RadioGroupKey } from './bus';
import { DEFAULT_RAW } from './core';
import type { AudioController } from './core';

// ============================ initial state =================================

const initialState = () => ({
  params: Array.from(DEFAULT_RAW),
  radios: {
    WaveBank: 0, WaveBank2: 0,
    LfoShape1: 0, LfoShape2: 0, LfoShape3: 0,
    FmModWave: 0, HwFltType: 0, Octave: 2,
  } as Record<RadioGroupKey, number>,
  volume: 1,
  bpm: 120,
  octave: -1,
  hwflt: { type: 0, cutoff: 0, res: 0, sweepF: 0, sweepAmt: 0 },
  fxmod: { time: 512, depth: 511, edit: [0, 0, 0, 0, 0], tape: [0, 0, 0, 0, 0] },
  fxdel: { time: 0, depth: 512, mix: -1000 },
  fxrev: { time: 512, depth: 512, mix: -1000 },
  bypass: [0, 0, 0],
  activePreset: null,
  morphing: new Set(),
  ready: false,
});

// ============================ writable stores ================================

export const audioState = writable(initialState());

/** Список сохранённых пресетов (stage 4): имена. Обновляется core.ts через
 *  bus 'preset:list' (GET /api/presets на старт + после save/delete). */
export const presetList = writable<string[]>([]);

/** Запросить свежий список пресетов у core (bus -> GET /api/presets). */
export function refreshPresetList(): void {
  // core.ts подписан на 'preset:list:request' и отвечает 'preset:list'.
  bus.emit('preset:list:request', { });
}

/** Update a slice of the state (shallow-merge top-level keys). */
export function patchState(patch: Partial<ReturnType<typeof initialState>>): void {
  audioState.update((s) => ({ ...s, ...patch }));
}

// ============================ controller ref ==================================

let _controller: AudioController | null = null;

/** Reactive ref на AudioController (core.ts). AudioHost.svelte вызывает
 *  setAudioController() после initAudio(); +page.svelte подписывается на
 *  $audioController и при появлении контроллера подключает canvas'ы
 *  осциллографа через controller.attachScope(wave, fft). */
export const audioController = writable<AudioController | null>(null);

export function setAudioController(c: AudioController | null): void {
  _controller = c;
  audioController.set(c);
  patchState({ ready: c !== null });
  if (typeof window !== 'undefined') {
    window.__droneWiring = c !== null;
  }
}

export function getAudioController(): AudioController | null {
  return _controller;
}

// ============================ bus -> state ====================================

if (browser) {
  bus.on('param:init', ({ i, v }) => {
    audioState.update((s) => {
      const params = s.params.slice();
      params[i] = v;
      return { ...s, params };
    });
  });
  bus.on('param:ext', ({ i, v }) => {
    audioState.update((s) => {
      const params = s.params.slice();
      params[i] = v;
      return { ...s, params };
    });
  });
  bus.on('radio:init', ({ group, idx }) => {
    audioState.update((s) => ({ ...s, radios: { ...s.radios, [group]: idx } }));
  });
  bus.on('radio:ext', ({ group, idx }) => {
    audioState.update((s) => ({ ...s, radios: { ...s.radios, [group]: idx } }));
  });
  // octave:set — core обновил state.octave (setOctaveShift) + radio:ext (Octave
  // group). Store sync'ит $audioState.octave из события, чтобы Keyboard
  // (derived $audioState.octave) и +page radio (checked=$audioState.octave===btn-3)
  // реагировали на смену октавы (Z/X hotkey + radio).
  bus.on('octave:set', ({ shift }) => {
    audioState.update((s) => ({ ...s, octave: shift }));
  });
  bus.on('preset:active', ({ name }) => {
    patchState({ activePreset: name });
  });
  bus.on('preset:list', ({ names }) => {
    presetList.set(names);
  });
  bus.on('morph:highlight', ({ i, on }) => {
    audioState.update((s) => {
      const morphing = new Set(s.morphing);
      if (on) morphing.add(i);
      else morphing.delete(i);
      return { ...s, morphing };
    });
  });
}

// ============================ write helpers (UI -> bus) =======================

export function setParam(i: number, v: number): void {
  bus.emit('param:set', { i, v });
}

export function setRadio(group: RadioGroupKey, idx: number): void {
  bus.emit('radio:checked', { group, idx });
}

export function setVolume(v: number): void {
  bus.emit('volume:set', { v });
}

export function setBpm(v: number): void {
  bus.emit('bpm:set', { v });
}

export function setOctave(shift: number, source: 'radio' | 'hotkey' = 'radio'): void {
  bus.emit('octave:set', { shift, source });
}

export function setHwfltType(t: number): void {
  bus.emit('hwflt:type', { t });
}

export function setHwfltParam(slot: 0 | 1 | 2 | 3, raw: number): void {
  bus.emit('hwflt:param', { slot, raw });
}

export function setFxmodTime(v: number): void {
  bus.emit('fxmod:time', { v });
}

export function setFxmodDepth(v: number): void {
  bus.emit('fxmod:depth', { v });
}

export function setFxmodEdit(i: number, v: number): void {
  bus.emit('fxmod:edit', { i: i as 0 | 1 | 2 | 3 | 4, v });
}

export function setFxmodTape(i: number, v: number): void {
  bus.emit('fxmod:tape', { i: i as 0 | 1 | 2 | 3 | 4, v });
}

export function setFxdel(which: number, raw: number): void {
  bus.emit('fxdel:set', { which: which as 0 | 1 | 2, raw });
}

export function setFxrev(which: number, raw: number): void {
  bus.emit('fxrev:set', { which: which as 0 | 1 | 2, raw });
}

export function setStageBypass(stage: number, bypass: number): void {
  bus.emit('stage:bypass', { stage: stage as 0 | 1 | 2, bypass: bypass as 0 | 1 });
}

export function noteOn(name: string, freq: number): void {
  bus.emit('note:on', { name, freq });
}

export function noteOff(name: string, freq: number): void {
  bus.emit('note:off', { name, freq });
}

export function randomize(mode: 0 | 1 | 2): void {
  bus.emit('preset:random', { mode });
}

export function applyPreset(name: string): void {
  bus.emit('preset:apply', { name });
}

/** Сохранить текущий звук как пресет (stage 4): UI -> bus -> POST /api/presets. */
export function savePreset(name: string, params: number[]): void {
  bus.emit('preset:save', { name, params: params.slice() });
}

/** Удалить сохранённый пресет (stage 4): UI -> bus -> DELETE /api/presets. */
export function deletePreset(name: string): void {
  bus.emit('preset:delete', { name });
}

export const EDIT_PARAM_COUNT = DEFAULT_RAW.length;
