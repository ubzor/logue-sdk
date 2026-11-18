// Typed event bus for the drone-chain simulator (Вариант B: Svelte-страница —
// начальник audio).
//
// Односторонний поток:
//   UI -> core : bus.emit('...:set', ...)          (намерение пользователя)
//   core -> UI : bus.emit('param:init' | 'param:ext' | 'radio:init' |
//                        'radio:ext' | 'preset:active' | 'morph:highlight')
//                (внешние изменения: старт-синк, preset apply, rnd-morph poll)
//
// `param:set` / `radio:checked` — UI-события, core слушает и пишет в wasm.
// `param:init` / `radio:init` — core пушит ОДИН РАЗ на старт (реальные wasm-
// значения), UI заполняет stores.
// `param:ext` / `radio:ext` — core пушит при внешних изменениях (preset /
// morph). ВАЖНО: это НЕ те же события, что `param:set` / `radio:checked` —
// если core emit'нул бы `param:set`, его же listener записал бы значение в
// wasm повторно (двойная запись, drop-окно direct-канала). `:ext`-события
// читает только UI.
//
// Read-only снимок состояния живёт в stores.ts (Svelte 5 $state), НЕ здесь:
// bus — транспорт, не хранилище (в плане было bus.state, отклонено:
// двойное хранение = рассинхрон; единственный источник = stores.audioState).

export type RadioGroupKey =
  | 'WaveBank'        // edit 13, options A..F (6)
  | 'WaveBank2'       // edit 14, options A..F (6)
  | 'LfoShape1'       // edit 40, options Sin/S/Walk (3)
  | 'LfoShape2'       // edit 42, options Sin/S/Walk (3)
  | 'LfoShape3'       // edit 46 (Wave Mix LFO), options Sin/S/Walk (3)
  | 'FmModWave'       // edit 49, options SIN/TRI/SAW/SQR/NOISE (5)
  | 'HwFltType'       // не edit-param: 0..6 (Off/HP4/HP2/BP4/BP2/LP4/LP2)
  | 'Octave';         // не edit-param: кнопки 1..5 (shift = btn - 3)

export interface ParamSet { i: number; v: number; }               // 0..54, raw 0..1023
export interface RadioChecked { group: RadioGroupKey; idx: number; }
export interface VolumeSet { v: number; }                         // 0..1
export interface BpmSet { v: number; }                            // 56..240 int
export interface OctaveSet { shift: number; source: 'radio' | 'hotkey'; } // -2..+2
export interface HwfltType { t: number; }                         // 0..6
export interface HwfltParam { slot: 0 | 1 | 2 | 3; raw: number; } // cutoff/res/sweepF/sweepAmt
export interface FxmodTime { v: number; }                         // raw 0..1023
export interface FxmodDepth { v: number; }                        // raw 0..1023
export interface FxmodEdit { i: 0 | 1 | 2 | 3 | 4; v: number; }   // DRIFT..ENSMBL
export interface FxmodTape { i: 0 | 1 | 2 | 3 | 4; v: number; }   // NOISE..WARP
export interface FxSet { which: 0 | 1 | 2; raw: number; }         // time/depth/mix
export interface StageBypass { stage: 0 | 1 | 2; bypass: 0 | 1; } // 0 = active
export interface NoteOn { name: string; freq: number; }
export interface NoteOff { name: string; freq: number; }
export interface PresetRandom { mode: 0 | 1 | 2; }                // All/Part/Orbit
export interface PresetApply { name: string; }                    // 'Reset' или сохранённый (stage 4)
export interface PresetActive { name: string | null; }
export interface PresetSave { name: string; params: number[]; }   // сохранение из UI (stage 4)
export interface PresetDelete { name: string; }                    // удаление из UI (stage 4)
export interface PresetList { names: string[]; }                   // core -> UI: обновить список
export interface PresetListRequest { }                             // UI -> core: запросить список
export interface PresetSaveResult { name: string; ok: boolean; error?: string; }
export interface PresetDeleteResult { name: string; ok: boolean; error?: string; }
export interface MorphHighlight { i: number; on: boolean; }

export interface BusEvents {
  'param:set': ParamSet;
  'param:init': ParamSet;
  'param:ext': ParamSet;
  'radio:checked': RadioChecked;
  'radio:init': RadioChecked;
  'radio:ext': RadioChecked;
  'volume:set': VolumeSet;
  'bpm:set': BpmSet;
  'octave:set': OctaveSet;
  'hwflt:type': HwfltType;
  'hwflt:param': HwfltParam;
  'fxmod:time': FxmodTime;
  'fxmod:depth': FxmodDepth;
  'fxmod:edit': FxmodEdit;
  'fxmod:tape': FxmodTape;
  'fxdel:set': FxSet;
  'fxrev:set': FxSet;
  'stage:bypass': StageBypass;
  'note:on': NoteOn;
  'note:off': NoteOff;
  'preset:random': PresetRandom;
  'preset:apply': PresetApply;
  'preset:active': PresetActive;
  'preset:save': PresetSave;
  'preset:delete': PresetDelete;
  'preset:list': PresetList;
  'preset:list:request': PresetListRequest;
  'preset:saveResult': PresetSaveResult;
  'preset:deleteResult': PresetDeleteResult;
  'morph:highlight': MorphHighlight;
}

export class Bus extends EventTarget {
  /** Отправить событие (синхронно, по порядку подписки). */
  emit<K extends keyof BusEvents>(type: K, detail: BusEvents[K]): void {
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }

  /** Подписаться. Возвращает функцию отмены (для onCleanup / onMount). */
  on<K extends keyof BusEvents>(type: K, fn: (detail: BusEvents[K]) => void): () => void {
    const handler = (e: Event) => fn((e as CustomEvent<BusEvents[K]>).detail);
    this.addEventListener(type, handler);
    return () => this.removeEventListener(type, handler);
  }
}

export const bus = new Bus();
