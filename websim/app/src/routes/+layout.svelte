<!-- Pass-through layout: the page markup lives in +page.svelte. The theme
     <html data-theme> init runs from +layout.ts (client module scope, before
     first paint).

     <head>: the emscripten glue index.js — ЕДИНСТВЕННЫЙ скрипт (Вариант B).
     Он не импортирует UI-wiring: после onRuntimeInitialized публикует
     window.__droneAudioReady (drone-chain/wasm.cc EM_ASM), который
     AudioHost (в +page.svelte) дожидается и передаёт в initAudio().
     Статический файл из app/static/ (копирует make wasm) — НЕ бандлится
     Vite (глук должен сохранить свою байтовую раскладку для worklet boot).

     Глобальный stylesheet (Tailwind + flyonui base/theme/components, включая
     правила слайдеров .range) импортируется ЗДЕСЬ, в <script> layout'а:
     SvelteKit линкует CSS, импортированный в .svelte-компоненте, в <head>
     для всех роутов. Импорт из +layout.ts (.ts-модуль) НЕ линкуется — CSS
     собирается в отдельный ассет, но в HTML не попадает (слайдеры оставались
     без стилей). -->
<script>
	import '../app.css';
</script>

<svelte:head>
	<script id="drone-glue" type="module" src="/index.js"></script>
</svelte:head>

<slot />
