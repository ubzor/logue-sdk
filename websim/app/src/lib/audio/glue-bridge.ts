// Мост glue -> Svelte (Вариант B).
//
// Emscripten glue (static/index.js) после инициализации audio worklet публикует
// window.__droneAudioReady = { context, node, Module } (EM_ASM в
// drone-chain/wasm.cc, AudioWorkletProcessorCreated). Svelte-компонент AudioHost
// (onMount) дожидается этого промиса и вызывает initAudio() из core.ts.
//
// Порядок гарантий: glue-скрипт единственный в <svelte:head> и стартует ДО
// hydration; AudioHost монтируется ПОСЛЕ (hydration). Если ready уже опубликован
// к моменту onMount — резолвим сразу; иначе poll каждые 50 мс. Оба случая
// покрыты.
//
// Browser-only: модуль читает window ТОЛЬКО внутри Promise-конструктора при
// наличии window; на сервере (SSR) `audioReady` — никогда не резолвящийся
// промис, который никогда не ожидается (AudioHost.onMount не вызывается).

export interface AudioBundle {
  context: AudioContext;
  node: AudioWorkletNode;
  /** Emscripten Module: embind-функции (setEditParam, getEditParamValue,
   *  setFxParam, noteOn/noteOff, setOscPitch, setRndMode/rndTrigger, ...)
   *  лежат в корне объекта (см. drone-chain/wasm.cc, drone_ui/my_module). */
  Module: any;
}

declare global {
  interface Window {
    __droneAudioReady?: AudioBundle;
    /** Флаг: AudioHost завершил initAudio (для диагностики/Playwright). */
    __droneWiring?: boolean;
  }
}

let _resolved: AudioBundle | null = null;

export const audioReady: Promise<AudioBundle> = new Promise<AudioBundle>((resolve) => {
  if (typeof window === 'undefined') return; // SSR: не резолвится (не ожидается)
  if (window.__droneAudioReady) {
    _resolved = window.__droneAudioReady;
    resolve(_resolved);
    return;
  }
  const timer = setInterval(() => {
    if (window.__droneAudioReady) {
      clearInterval(timer);
      _resolved = window.__droneAudioReady;
      resolve(_resolved);
    }
  }, 50);
  // Диагностика (не отменяет ожидание): если glue не запустился за 15 с —
  // консольный error (COOP/COEP не выставлены, wasm 404 и т.п.).
  setTimeout(() => {
    if (!_resolved) {
      console.error('[drone] window.__droneAudioReady не опубликован за 15 с — glue не запустился? Проверить консоль и COOP/COEP.');
    }
  }, 15000);
});
