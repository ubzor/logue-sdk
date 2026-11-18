#!/usr/bin/env node
// Relink the emscripten glue (static/index.js) to the SvelteKit audio wiring.
//
// Why: the drone-chain Makefile builds index.js from a plain HTML shell where
// setup-web-audio.js was a CLASSIC script (window.setupWebAudioAndUI). In the
// SvelteKit app the wiring lives in a hashed Vite chunk. This script copies
// src/lib/audio/setup-web-audio.js to static/setup-web-audio.js (plain file,
// stable URL, served as-is) and rewrites the glue's EM_ASM boot call into an
// async IIFE that dynamic-imports it. The module keeps a legacy
// window.setupWebAudioAndUI assignment, so anything else expecting the bare
// global keeps working. Idempotent.
//
// Run after `npm run build` (the copy is source-derived, not build-derived):
//   node scripts/relink-glue.mjs

import { readFileSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const src = join(root, 'src', 'lib', 'audio', 'setup-web-audio.js');
const out = join(root, 'static', 'setup-web-audio.js');
const glue = join(root, 'static', 'index.js');

// 1) Copy the wiring module to a stable static URL.
writeFileSync(out, readFileSync(src, 'utf8'));

// 2) Patch the glue's EM_ASM boot call (idempotent).
let g = readFileSync(glue, 'utf8');
// Replace the WHOLE 72320 lambda body with a single clean wrapper. This is
// idempotent (re-running replaces the wrapper with itself) and self-heals
// double-patched glue (an outer wrapper wrapping an inner one).
const wrapper =
	'void (async function () {' +
	'  const m = await import("/setup-web-audio.js");' +
	'  window.setupWebAudioAndUI = m.setupWebAudioAndUI;' +
	'  window.__droneWiring = true;' +
	'  m.setupWebAudioAndUI(emscriptenGetAudioObject($0), emscriptenGetAudioObject($1), Module);' +
	' })();';
// The 72320 entry in the FRESH glue is a one-liner:
//   72320: ($0, $1) => { setupWebAudioAndUI(...); }
// Replace the whole entry (up to its closing '};' of ASM_CONSTS or the next
// entry) with a single clean wrapper. Matching the full line keeps the shape
// simple and idempotent (a previously patched line is replaced by the same
// one).
const re = /72320: \(\$0, \$1\) => \{[\s\S]*?(?=\}[ \t]*\n)/;
if (!re.test(g)) {
	console.error('relink-glue: EM_ASM 72320 entry not found - glue layout changed?');
	process.exit(1);
}
g = g.replace(re, '72320: ($0, $1) => { ' + wrapper);
writeFileSync(glue, g);

// 3) Re-copy static/ into build/client/ (the vite build wiped it). The server
//    serves build/client as the static root; without this step the patched
//    glue and the wiring module 404 after a rebuild.
import { copyFileSync } from 'node:fs';
import { existsSync } from 'node:fs';
const clientDir = join(root, 'build', 'client');
if (!existsSync(clientDir)) {
	console.error('relink-glue: build/client missing - run `npm run build` first.');
	process.exit(1);
}
for (const f of ['index.js', 'index.wasm', 'setup-web-audio.js', 'qwerty-hancock.js', 'favicon.ico']) {
	const from = join(root, 'static', f);
	if (existsSync(from)) copyFileSync(from, join(clientDir, f));
	// Remove stale precompressed copies: SvelteKit serves .br/.gz INSTEAD of
	// the source file when Accept-Encoding matches, and the vite build only
	// regenerates them for files that existed at build time - so a file we
	// (re)copy after the build keeps serving its OLD compressed version.
	for (const ext of ['.br', '.gz']) {
		const stale = join(clientDir, f + ext);
		if (existsSync(stale)) {
			import('node:fs').then(({ unlinkSync }) => unlinkSync(stale));
		}
	}
}
console.log('relink-glue: patched static/index.js (wiring: /setup-web-audio.js) + copied static -> build/client');
