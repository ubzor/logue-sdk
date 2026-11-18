# websim

Web-sимулятор DRONE (NTS-1 mkII): полный звуковой тракт OSC → MOD → DEL → REV
в одном wasm audio worklet. UI — SvelteKit-приложение в [`app/`](app/README.md);
запуск: `platform/nts-1_mkii/serve.sh` (http://localhost:8000/).

Папка содержит только:

| Путь        | Роль |
|---|---|
| `app/`      | SvelteKit-приложение (UI + audio wiring + глук-мост) — единственный источник правок |
| `dsp/*.c`   | LUT/stub-исходники, которые wasm-сборка `drone-chain/` линкует (см. `drone-chain/config.mk`) |

Всё остальное (legacy-шелл `index.html`, `scripts/`, `styles/`, `sim/`,
`images/`, `samples/`, Vite-конфиги верхнего уровня) удалено: UI живёт в
`app/src/`, а артефакты emscripten (`index.js`/`index.wasm`) `make wasm` из
`platform/nts-1_mkii/drone-chain/` сам синхронизирует в `app/static/`, а
`platform/nts-1_mkii/serve.sh` собирает и запускает приложение.
