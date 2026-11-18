// Root layout: theme init before first paint + the single global stylesheet.
//
// The emscripten glue (index.js, injected by the drone-chain build) creates
// the AudioContext lazily on the first user gesture (setup-web-audio.js wires
// auto-resume on click/keypress), so no audio runs during SSR/hydration -
// `ssr: true` is fine and faster.
//
// Theme: the legacy websim/scripts/entry.js ran as a deferred module script
// and set <html data-theme> before first paint. Here we do the same from the
// client-side layout module: SvelteKit executes this file's top-level code in
// the browser before the component tree renders (and skips it on the server),
// so the data-theme attribute is applied before any frame.

import { browser } from '$app/environment';
import { initTheme, onThemeSwitchChange } from '../lib/theme.js';
import '../app.css';

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
