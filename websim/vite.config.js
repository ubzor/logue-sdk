// Vite config for the logue-sdk web simulators.
//
// The simulators are HTML shells processed by emscripten (see the per-unit
// Makefile: `--shell-file .../websim/osc.html`). Emscripten only injects the
// {{{ SCRIPT }}} placeholder, so everything else in the shell is passed
// through verbatim. This Vite project therefore builds the *assets only*:
//
//   sim/flyon.css  - Tailwind CSS v4 + the flyonui plugin (semantic classes)
//   sim/flyon.js   - the entry module (theme init, flyonui JS plugins if any)
//
// The shells reference them with relative paths (./flyon.css, ./flyon.js),
// and serve.sh serves sim/ as the document root.
//
// CSS-only build: the only Vite *input* is app.css. We must not set
// appType: 'custom' here - with no JS entry Vite's build hook skips the whole
// build. The default appType ('spa') is a no-op for our purposes (there is no
// index.html to transform and no /src/main.js to inject).
import { defineConfig } from 'vite';
import tailwindcss from '@tailwindcss/vite';
// Repair flyonui 2.4.1 minified component CSS before Lightning CSS runs.
// See vite.flyonui.js for the details (flyonui's minifier fuses value/
// declaration boundaries, which makes Lightning CSS crash or drop the
// &-nested blocks - e.g. the switch knob).
import flyonuiRepair from './vite.flyonui.js';

export default defineConfig({
  plugins: [flyonuiRepair(), tailwindcss()],
  // Relative asset URLs so the page works no matter which path it is served
  // from (e.g. http://host:8000/drone.html).
  base: './',
  // sim/ holds emscripten's artifacts (drone.html/.js/.wasm) - never clean it.
  build: {
    outDir: 'sim',
    emptyOutDir: false,
    rollupOptions: {
      // Both the entry module (theme init + flyonui JS plugins, if any) and
      // the stylesheet. Rollup bundles entry.js into flyon.js; the CSS it
      // imports (@import "./app.css") is extracted into the asset flyon.css.
      input: ['entry.js', 'app.css'],
      output: {
        entryFileNames: 'flyon.js',
        assetFileNames: 'flyon.css',
      },
    },
  },
  // In dev mode (vite serve) we don't want the watcher to re-trigger on the
  // build output.
  server: {
    watch: { ignored: ['**/sim/**', '**/build/**'] },
  },
  logLevel: 'warn',
});
