// Root layout: theme init before first paint + the single global stylesheet.
//
// Вариант B: glue (index.js, injected by the drone-chain build) публикует
// window.__droneAudioReady после инициализации audio worklet. AudioHost
// (src/lib/components/AudioHost.svelte) дожидается этого сигнала и вызывает
// initAudio() из core.ts. AudioContext создаётся glue lazily на первом
// user gesture (auto-resume on click/keypress), так что `ssr: true` — ок.
//
// Theme: the legacy websim/scripts/entry.js ran as a deferred module script
// and set <html data-theme> before first paint. Here we do the same from the
// client-side layout module: SvelteKit executes this file's top-level code in
// the browser before the component tree renders (and skips it on the server),
// so the data-theme attribute is applied before any frame.

import { browser } from '$app/environment';
import { initTheme, onThemeSwitchChange } from '../lib/theme.js';
// NB: the global stylesheet is imported in +layout.svelte (NOT here) — SvelteKit
// only links CSS imported in a .svelte component into <head>; a .ts import
// builds the asset but never links it (range sliders stayed unstyled).

if (browser) {
	// The layout module runs in the browser before the component tree renders,
	// so initTheme() sets <html data-theme> before first paint (the legacy
	// entry.js equivalent). On the server `browser` is false, so the DOM
	// calls below are never executed.
	const init = initTheme();
	// Expose the switch change handler on window: the bridge to the plain-JS
	// ThemeSwitch handler in +page.svelte (the OS-preference follow flag is
	// captured in the closure).
	// @ts-ignore
	window.__onThemeSwitchChange = (e: Event) => onThemeSwitchChange(e, init.fromPrefers);
}

export const ssr = true;
