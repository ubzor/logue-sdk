// SvelteKit config for the drone chain web simulator app.
//
// adapter-node: the app is a single Node server that (a) renders the simulator
// UI and (b) will host the /api/presets backend (stage 4). One server, one
// process, COOP/COEP headers set in hooks.server.js (needed by the wasm
// SharedArrayBuffer build).

import adapter from '@sveltejs/adapter-node';
import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

/** @type {import('@sveltejs/kit').Config} */
const config = {
	// CSS is handled by the @tailwindcss/vite plugin (see vite.config.js);
	// Svelte <style> blocks need no extra preprocessing for now.
	preprocess: vitePreprocess(),

	kit: {
		adapter: adapter({ out: 'build' })
		// Static files (index.js/.wasm glue, favicon) live in app/static/ and
		// are served/copied by SvelteKit by default.
	}
};

export default config;
