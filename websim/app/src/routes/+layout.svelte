<!-- Pass-through layout: the page markup (and its audio wiring) lives in
     +page.svelte. The theme <html data-theme> init runs from +layout.ts
     (client module scope, before first paint).

     <head> extras: qwerty-hancock.js (classic UMD -> window.QwertyHancock)
     and the emscripten glue index.js (module; it loads the audio wiring from
     src/lib/audio/ and the wasm). These are STATIC files in app/static/
     (copied there by the drone-chain build, stage 3) - NOT processed by Vite,
     because the glue must keep its byte layout for the worklet boot. The
     id on the glue tag makes the build step able to relocate the <script>
     into <body> after the page markup (Svelte hoists svelte:head children
     into <head>, but the glue must run after the DOM exists). -->
<svelte:head>
	<script src="/qwerty-hancock.js"></script>
	<script id="drone-glue" type="module" src="/index.js"></script>
</svelte:head>

<slot />
