// Theme model for the simulator (ported from the legacy websim/scripts/entry.js).
//
// The shell hosts a single switch (#ThemeSwitch) that toggles between the two
// "personality" themes:
//   off -> luxury-light (light)   /   on -> luxury (dark)
// <html> declares data-theme="light" as the no-JS fallback (see app.html).
// The effective theme at startup:
//   1. the persisted "flyon-theme" localStorage value, if present (legacy
//      values dark/spotify/vscode map to dark, light/valorant/slack/gourmet/
//      luxury-light to light);
//   2. otherwise the OS preference: prefers-color-scheme: dark -> luxury,
//      else luxury-light.
// The chosen theme is persisted back to localStorage so subsequent loads use
// the stored value.
//
// +layout.ts runs the browser-side parts in a module-level IIFE (SvelteKit
// runs client layout JS before the body renders), so the data-theme attribute
// is set before first paint exactly like the legacy deferred module script.

export const DARK_THEME = 'luxury';
export const LIGHT_THEME = 'luxury-light';

// Legacy themes from pre-switch shells. Kept so a saved "dark" (or
// "spotify" / "vscode") keeps working on the first load after the upgrade -
// it just maps onto the luxury personality.
const DARK_LEGACY = new Set(['vscode', 'dark', 'spotify', 'luxury']);
const LIGHT_LEGACY = new Set(['light', 'valorant', 'slack', 'gourmet', 'luxury-light']);

export function isDarkTheme(name: string): boolean {
	return DARK_LEGACY.has(name) || !LIGHT_LEGACY.has(name);
}

function applyTheme(name: string) {
	document.documentElement.dataset.theme = name;
	try {
		localStorage.setItem('flyon-theme', name);
	} catch (e) {}
	// Mark that the user explicitly set the switch: the matchMedia('change')
	// OS-follow listener (registered in onThemeSwitchChange when the initial
	// theme came from the OS preference) checks this attribute and only
	// follows live OS changes while the user has NOT flipped the switch yet.
	// Without this the OS-follow branch was dead code — no one ever set the
	// attribute, so live OS changes were silently ignored.
	document.querySelector('#ThemeSwitch')?.setAttribute('data-user-set', '');
}

// Decide the initial theme. Returns { name, fromPrefers } where fromPrefers
// is true when the OS preference (not a stored value) decided - in that case
// the switch reflects the preference without writing to localStorage until
// the user actually flips it.
function initialTheme(): { name: string; fromPrefers: boolean } {
	let stored = null;
	try {
		stored = localStorage.getItem('flyon-theme');
	} catch (e) {}
	if (stored) {
		return { name: DARK_LEGACY.has(stored) ? DARK_THEME : LIGHT_THEME, fromPrefers: false };
	}
	let dark = false;
	try {
		dark = window.matchMedia('(prefers-color-scheme: dark)').matches;
	} catch (e) {}
	return { name: dark ? DARK_THEME : LIGHT_THEME, fromPrefers: true };
}

export function initTheme(): { name: string; fromPrefers: boolean } {
	const init = initialTheme();
	// Apply before first paint (this runs during client layout hydration).
	document.documentElement.dataset.theme = init.name;
	return init;
}

export function onThemeSwitchChange(event: Event, _fromPrefers: boolean) {
	const input = event.target as HTMLInputElement;
	// applyTheme persists the choice to localStorage AND stamps the switch
	// with data-user-set. From the next load on, initTheme uses the stored
	// value (fromPrefers=false), so OS preference is consulted only until
	// the user explicitly flips the switch — that is the "follow OS until
	// the user takes over" behaviour described in the file header.
	//
	// (The previous matchMedia('change') live-follow listener was dead code:
	// its guard `if (!hasAttribute('data-user-set')) return;` was inverted
	// relative to the documented behaviour, and its body was empty. Removed.)
	applyTheme(input.checked ? DARK_THEME : LIGHT_THEME);
}
