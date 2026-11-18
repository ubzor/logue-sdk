# logue-websim-app (SvelteKit)

SvelteKit-приложение — web-симулятор drone chain (OSC → MOD → DEL → REV).
Единственный источник правок UI (legacy статический шелл удалён).

## Запуск

Полный цикл (wasm + app + сервер) — один скрипт:

```bash
platform/nts-1_mkii/serve.sh [PORT] [BIND]   # default: 8000 0.0.0.0
```

Вручную, по частям:

```bash
cd app
npm install
# артефакты emscripten (index.js/index.wasm) синхронизирует сам `make wasm`
# (drone-chain/Makefile копирует их в app/static/); вручную:
cp ../../../platform/nts-1_mkii/drone-chain/sim/index.{js,wasm} static/
npm run dev                    # http://localhost:5173 (dev, COOP/COEP из vite headers)
# production:
npm run build
node scripts/relink-glue.mjs   # после КАЖДОГО build (см. ниже почему)
node build                     # http://localhost:3000
```

## Структура

| Путь | Роль |
|---|---|
| `src/routes/+page.svelte` | страница симулятора (top row + main row) |
| `src/lib/components/OscPanel.svelte` | OSC-панель: `#Presets` + `#Parameters` (3 колонки) |
| `src/lib/components/FxStack.svelte` | Filter/MOD/DEL/REV-стек (правая треть) |
| `src/lib/audio/setup-web-audio.js` | порт legacy-скрипта (1:1) как ES-модуль; экспортирует `setupWebAudioAndUI` |
| `src/lib/theme.ts` | модель темы (luxury / luxury-light, localStorage + OS-preference) |
| `src/app.css` | Tailwind v4 + flyonui + кастом (range-репайр, octave-join, scope) |
| `static/index.js` / `index.wasm` | emscripten-глук из drone-chain (git-ignored; копится `make wasm`) |
| `static/qwerty-hancock.js` | классический UMD-скрипт клавиатуры |
| `scripts/relink-glue.mjs` | патчит глук: EM_ASM-завод → dynamic import модуля wiring + `Module` |

## Как работает интеграция с wasm (критично)

1. **Глук (`static/index.js`)** — байт-в-байт из chain-сборки, НО с одной
   переписанной точкой: EM_ASM `72320` (завод processAudio) теперь async-IIFE:
   динамически импортирует `/setup-web-audio.js`, кладёт
   `window.setupWebAudioAndUI = m.setupWebAudioAndUI` (для legacy-ожиданий) и
   вызывает `m.setupWebAudioAndUI(ctx, node, Module)` — передавая `Module`
   явно (глук — ES-модуль, его `var Module` не глобален).
   Делает это `scripts/relink-glue.mjs` (идемпотентен; сам лечит двойные патчи).
2. **Wiring (`/setup-web-audio.js`)** — КОПИЯ `src/lib/audio/setup-web-audio.js`
   (не Vite-чанк): стабильный URL, `const Module = wasmModule ||
   globalThis.Module` (мост к legacy-глобалке), весь DOM-код 1:1 от legacy
   (id'ы: `VolumeSlider`, `BpmSlider`, `editparam_*`, `hwflt_*`, `fxmod_*`,
   `fxdel_*`, `fxrev_*`, `#Presets`, `#ParamCol0..2`, `#keyboard`,
   `#TimeDomain`/`#FrequencyDomain`, радио-группы `Octave`/`HwFltType`).
3. **Почему relink после build**: `vite build` затирает `build/client/` и
   пересобирает `.br`/`.gz`-пресжатие. `relink-glue.mjs` (а) перепатчивает
   глук из свежего `static/`, (б) копирует `static/*` в `build/client/`,
   (в) удаляет устаревшие `.br`/`.gz` для этих файлов — иначе SvelteKit отдаёт
   СТАРУЮ пресжатую копию поверх свежего (браузер с `Accept-Encoding: br`
   получает не то, что на диске).
4. **COOP/COEP**: wasm-сборка требует SharedArrayBuffer → заголовки
   `Cross-Origin-Opener-Policy: same-origin` + `Cross-Origin-Embedder-Policy:
   require-corp` выставляются в `src/hooks.server.ts` (prod) и
   `vite.config.js` `server.headers` (dev).
5. **Тема**: `+layout.ts` (client, до первого render) применяет
   `data-theme` на `<html>`; `<html data-theme="light">` в `app.html` —
   no-JS fallback. Switch `#ThemeSwitch` пишет `localStorage['flyon-theme']`.

## Пресеты (задел под бэкенд)

UI-точка — `#Presets` (кнопки All Random / Partial Random / Orbit Random /
Reset создаются wiring'ом; слоты под сохранённые пресеты добавляются туда же).
Бэкенд — SvelteKit server routes `src/routes/api/presets/` (+ `+server.ts`):
пока — JSON-файл в `app/data/presets.json`; дальше — SQLite (better-sqlite3)
без изменения формы API (`GET /api/presets`, `POST /api/presets`,
`DELETE /api/presets/:name`). Wiring будет ходить в API через fetch (UI-поток,
не аудио-поток).

## Чего НЕ делать

- Не править `static/index.js` руками — только через `relink-glue.mjs`
  (пересобирается из chain-глuka + патч).
- Не превращать wiring в Vite-чанк — глук должен знать стабильный URL.
- Не менять DOM id'ы в разметке — wiring находит элементы по ним.
- Не коммитить `build/`, `node_modules/`, `.svelte-kit/` (см. .gitignore).
