#!/usr/bin/env node
// Sync the emscripten glue + static assets into build/client/ (Вариант B).
//
// Вариант B: glue (static/index.js) НЕ патчится — он сам публикует
// window.__droneAudioReady = { context, node, Module } после инициализации
// audio worklet (EM_ASM в drone-chain/wasm.cc, AudioWorkletProcessorCreated).
// Svelte-компонент AudioHost (onMount) дожидается этого сигнала и вызывает
// initAudio() из core.ts. Никакого legacy-импорта /setup-web-audio.js нет.
//
// Эта скрипта делает только две вещи:
//   1) Убирает stale legacy-артёфакты (setup-web-audio.js, qwerty-hancock.js)
//      из static/ и build/client/ — они больше не нужны.
//   2) Re-копирует static/ -> build/client/ (vite build стирает build/client),
//      удаляя stale .br/.gz (иначе SvelteKit отдаст СТАРУЮ сжатую версию).
//
// Запускать ПОСЛЕ `npm run build`:
//   node scripts/relink-glue.mjs

import { copyFileSync, existsSync, unlinkSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const staticDir = join(root, 'static');
const clientDir = join(root, 'build', 'client');

if (!existsSync(clientDir)) {
	console.error('relink-glue: build/client missing - run `npm run build` first.');
	process.exit(1);
}

// 1) Удалить legacy-артефакты (setup-web-audio.js, qwerty-hancock.js).
//    Вариант B не использует ни UI-wiring-модуль, ни qwerty-hancock
//    (клавиатура в +page.svelte рендерится Svelte'ом, не через QwertyHancock).
const legacyFiles = ['setup-web-audio.js', 'qwerty-hancock.js'];
for (const f of legacyFiles) {
	for (const dir of [staticDir, clientDir]) {
		const p = join(dir, f);
		if (existsSync(p)) unlinkSync(p);
		for (const ext of ['.br', '.gz']) {
			const stale = p + ext;
			if (existsSync(stale)) unlinkSync(stale);
		}
	}
}

// 2) Re-копировать static/ -> build/client/ (только glue + wasm).
//    Vite build стирает build/client/ и не знает о static/index.js|wasm,
//    потому что glue не бандлится (должен сохранить байтовую раскладку).
const glueFiles = ['index.js', 'index.wasm'];
for (const f of glueFiles) {
	const from = join(staticDir, f);
	if (!existsSync(from)) {
		console.error(`relink-glue: static/${f} missing - run \`make wasm\` first.`);
		process.exit(1);
	}
	copyFileSync(from, join(clientDir, f));
	// Remove stale precompressed copies: SvelteKit serves .br/.gz INSTEAD of
	// the source file when Accept-Encoding matches, and the vite build only
	// regenerates them for files that existed at build time - so a file we
	// (re)copy after the build keeps serving its OLD compressed version.
	for (const ext of ['.br', '.gz']) {
		const stale = join(clientDir, f + ext);
		if (existsSync(stale)) unlinkSync(stale);
	}
}

console.log('relink-glue: static -> build/client (glue: index.js + index.wasm), legacy artifacts removed');
