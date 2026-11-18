// /api/presets — server-side storage for saved drone-chain presets (stage 4).
//
// A preset is { name, params } where `params` is the full 55-element raw
// array (index 0..54, values 0..1023) read straight from the live UI state
// (stores.audioState.params). The audio controller applies it via
// applyPresetRaw (paced setEditParam), so saving/round-tripping is lossless
// for the 55 edit-params that the UI exposes.
//
// Storage: a plain JSON file at <repo>/data/presets.json (NOT under build/,
// NOT under static/ — those are wiped/regenerated). The file holds
// [{ name, params, savedAt }]. The repo root is two levels up from src/
// (src -> app -> websim). The file is created lazily on first write; GET
// returns [] when it is missing or empty.
//
// Routes:
//   GET    /api/presets            -> [{ name, params, savedAt }]
//   POST   /api/presets            body { name, params } -> upsert by name
//   DELETE /api/presets?name=...   -> remove by name
//
// The adapter-node server runs with cwd = app/ (serve.sh `cd "$APP_DIR"`), so
// process.cwd() is the app dir and path.resolve('..') is the websim root.

import { json } from '@sveltejs/kit';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const DATA_DIR = resolve(process.cwd(), '..', 'data');
const PRESETS_FILE = join(DATA_DIR, 'presets.json');
const MAX_NAME_LEN = 48;
const MAX_PRESETS = 64;

interface Preset {
	name: string;
	params: number[];
	savedAt: number;
}

function loadPresets(): Preset[] {
	if (!existsSync(PRESETS_FILE)) return [];
	try {
		const raw = readFileSync(PRESETS_FILE, 'utf8');
		const parsed = JSON.parse(raw);
		if (!Array.isArray(parsed)) return [];
		return parsed.filter(
			(p): p is Preset =>
				p && typeof p.name === 'string' && Array.isArray(p.params) && p.params.every((n) => typeof n === 'number')
		);
	} catch {
		return [];
	}
}

function savePresets(list: Preset[]): void {
	if (!existsSync(DATA_DIR)) mkdirSync(DATA_DIR, { recursive: true });
	writeFileSync(PRESETS_FILE, JSON.stringify(list, null, 2), 'utf8');
}

export function GET({ request }: { request: Request }) {
	// Only accept GET (the browser fetches the list on load).
	if (request.method !== 'GET') {
		return json({ error: 'method not allowed' }, { status: 405 });
	}
	return json(loadPresets());
}

export async function POST({ request }: { request: Request }) {
	let body: { name?: unknown; params?: unknown } | null = null;
	try {
		body = (await request.json()) as { name?: unknown; params?: unknown };
	} catch {
		body = null;
	}
	if (!body || typeof body.name !== 'string' || !Array.isArray(body.params)) {
		return json({ error: 'expected { name: string, params: number[] }' }, { status: 400 });
	}
	const name = body.name.trim().slice(0, MAX_NAME_LEN);
	if (!name) {
		return json({ error: 'name required' }, { status: 400 });
	}
	const params = body.params.filter((n) => typeof n === 'number');
	if (params.length === 0) {
		return json({ error: 'params must be a non-empty number array' }, { status: 400 });
	}

	const list = loadPresets();
	const existing = list.findIndex((p) => p.name === name);
	const entry: Preset = { name, params, savedAt: Date.now() };
	if (existing >= 0) list[existing] = entry;
	else list.push(entry);
	// Keep the bank bounded (oldest first when over the cap).
	while (list.length > MAX_PRESETS) list.shift();
	savePresets(list);
	return json(list);
}

export async function DELETE({ request, url }: { request: Request; url: URL }) {
	const name = url.searchParams.get('name');
	if (!name) {
		return json({ error: 'name required' }, { status: 400 });
	}
	const list = loadPresets();
	const next = list.filter((p) => p.name !== name);
	if (next.length === list.length) {
		return json({ error: 'not found' }, { status: 404 });
	}
	savePresets(next);
	return json(next);
}
