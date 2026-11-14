// Entry module for the FlyonUI-powered simulator shells.
//
// Imported by the generated sim/flyon.js bundle (see vite.config.js). Runs in
// the main thread before emscripten's glue code, so it only touches the DOM
// theme - never the audio graph.
import './app.css';

// Theme model: the shell's header row hosts a single switch
// (#ThemeSwitch) that toggles between the two "personality" themes:
//   off  -> luxury-light (light)   /   on -> luxury (dark)
// The shell declares data-theme="light" on <html> as the no-JS fallback.
// At startup the effective theme is:
//   1. the persisted "flyon-theme" localStorage value, if present
//      (legacy values dark/spotify/vscode are treated as dark, light/valorant/
//       slack/gourmet/luxury-light as light - see isDarkTheme below),
//   2. otherwise the OS preference: prefers-color-scheme: dark -> luxury,
//      else -> luxury-light.
// The chosen theme is persisted back to localStorage, so subsequent loads
// always use the stored value.

const DARK_THEME = 'luxury';
const LIGHT_THEME = 'luxury-light';

// Legacy themes from pre-switch shells. Kept so a saved "dark" (or "spotify"/
// "vscode") keeps working on the first load after the upgrade - it just maps
// onto the luxury personality.
const DARK_LEGACY = new Set(['vscode', 'dark', 'spotify', 'luxury']);
const LIGHT_LEGACY = new Set(['light', 'valorant', 'slack', 'gourmet', 'luxury-light']);

function isDarkTheme(name) {
  return DARK_LEGACY.has(name) || !LIGHT_LEGACY.has(name);
}

function applyTheme(name) {
  document.documentElement.dataset.theme = name;
  try { localStorage.setItem('flyon-theme', name); } catch (e) {}
}

// Decide the initial theme. Returns { name, fromPrefers } where fromPrefers
// is true when the OS preference (not a stored value) decided - in that case
// the switch reflects the preference without writing to localStorage until
// the user actually flips it.
function initialTheme() {
  let stored = null;
  try { stored = localStorage.getItem('flyon-theme'); } catch (e) {}
  if (stored) {
    return { name: DARK_LEGACY.has(stored) ? DARK_THEME : LIGHT_THEME, fromPrefers: false };
  }
  let dark = false;
  try { dark = window.matchMedia('(prefers-color-scheme: dark)').matches; } catch (e) {}
  return { name: dark ? DARK_THEME : LIGHT_THEME, fromPrefers: true };
}

// Apply before first paint (this module runs deferred, after DOM parse).
const init = initialTheme();
document.documentElement.dataset.theme = init.name;

// Theme switcher: the shells render a #ThemeSwitcher card with one
// <button data-theme-btn="..."> per enabled theme. Wire them up here so
// every shell (osc, fx, xypad) gets the switcher for free.
function initThemeSwitcher() {
  const container = document.getElementById('ThemeSwitcher');
  if (!container) return;

  const buttons = Array.from(container.querySelectorAll('button[data-theme-btn]'));

  function markActive(name) {
    for (const b of buttons) {
      const on = b.dataset.themeBtn === name;
      b.classList.toggle('btn-active', on);
      b.classList.toggle('btn-soft', on);
      b.classList.toggle('btn-primary', on);
      b.classList.toggle('btn-outline', !on);
    }
  }

  for (const b of buttons) {
    b.addEventListener('click', () => {
      const name = b.dataset.themeBtn;
      document.documentElement.dataset.theme = name;
      try { localStorage.setItem('flyon-theme', name); } catch (e) {}
      markActive(name);
    });
  }

  // Reflect the current theme on load.
  const current = document.documentElement.dataset.theme || 'light';
  markActive(current);
}

// Theme switch: the shells render a #ThemeSwitch <label> with a
// flyonui switch input. Checked = dark (luxury), unchecked = light (luxury-light).
function initThemeSwitch() {
  const input = document.getElementById('ThemeSwitch');
  if (!input) return;

  // Reflect the effective theme (stored or OS preference) on the switch.
  input.checked = isDarkTheme(document.documentElement.dataset.theme);

  // Follow live OS preference changes, but only until the user explicitly
  // flips the switch (which persists a value and takes over).
  input.addEventListener('change', () => {
    applyTheme(input.checked ? DARK_THEME : LIGHT_THEME);
    osPreferenceFollow = false;
  });
  let osPreferenceFollow = init.fromPrefers;
  try {
    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
      if (!osPreferenceFollow) return;
      input.checked = e.matches;
      applyTheme(e.matches ? DARK_THEME : LIGHT_THEME);
    });
  } catch (e) {
    // matchMedia without addEventListener (older Safari) - no live follow.
  }
}

// Run after the DOM is ready (flyon.js is type="module", so it is already
// deferred - the DOM is parsed by the time this executes).
initThemeSwitcher();
initThemeSwitch();

