// Server hooks: cross-origin isolation headers for the wasm audio worklet.
//
// The drone-chain wasm is built with -s WASM_WORKERS=1 -s SHARED_MEMORY=1,
// which needs SharedArrayBuffer, which the browser only enables on
// cross-origin isolated pages (COOP: same-origin + COEP: require-corp).
//
// The legacy static server (platform/nts-1_mkii/serve.sh) set these headers
// on every response in its Python handler; the Vercel deploy had them in
// vercel.json. This hook does the same for the SvelteKit server (prod) -
// dev mode gets the identical headers from vite.config.js (server.headers).
//
// NOTE: COEP require-corp blocks cross-origin subresources without a CORP /
// CORS header. The only external resource is the Google Fonts stylesheet,
// which serves CORS (*) and is a style (not a script), so it stays isolated.
// All local assets are same-origin.
import { sequence } from '@sveltejs/kit/hooks';

const COI_HEADERS = {
	'Cross-Origin-Opener-Policy': 'same-origin',
	'Cross-Origin-Embedder-Policy': 'require-corp',
	'Cross-Origin-Resource-Policy': 'same-origin'
};

/** @type {import('@sveltejs/kit').Handle} */
export const handle = sequence(async ({ event, resolve }) => {
	const response = await resolve(event);
	for (const [key, value] of Object.entries(COI_HEADERS)) {
		response.headers.set(key, value);
	}
	return response;
});
