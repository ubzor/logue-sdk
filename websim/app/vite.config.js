// Vite config for the SvelteKit drone chain simulator app.
//
// Replaces the legacy websim/ Vite CSS-only build: SvelteKit (Vite) now owns
// the whole page (UI components in src/routes, audio wiring in src/lib/audio),
// so the Tailwind/flyonui asset pipeline moves into this config as a plugin.
//
// The flyonuiRepair() plugin (vite.flyonui.js, copied from the legacy
// websim/ project) must run FIRST: it stages repaired copies of flyonui 2.4.1
// component CSS into ./flyonui-patched/ before @tailwindcss/vite reads the
// @imports inside src/app.css. Do not reorder the plugins.
//
// COOP/COEP: the wasm build uses -s SHARED_MEMORY=1, so every response must be
// cross-origin isolated. serve.sh set these headers on the legacy static
// server; here they are set in src/hooks.server.ts (dev: vite server.headers,
// prod: the adapter-node response).

import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig } from 'vite';
import tailwindcss from '@tailwindcss/vite';
// Repair flyonui 2.4.1 minified component CSS before Lightning CSS runs.
// See vite.flyonui.js for the details (flyonui's minifier fuses value/
// declaration boundaries, which makes Lightning CSS crash or drop the
// &-nested blocks - e.g. the switch knob).
import flyonuiRepair from './vite.flyonui.js';

export default defineConfig({
	plugins: [flyonuiRepair(), tailwindcss(), sveltekit()],
	server: {
		headers: {
			'Cross-Origin-Opener-Policy': 'same-origin',
			'Cross-Origin-Embedder-Policy': 'require-corp',
			'Cross-Origin-Resource-Policy': 'same-origin'
		}
	},
	logLevel: 'warn'
});
