// Vite plugin: stage repaired copies of flyonui 2.4.1 component CSS for the
// Tailwind v4 compiler to read.
//
// Why this exists
// ===============
// flyonui 2.4.1 ships its component stylesheets (components/*.css) already
// minified by its own lightningcss pass, and that minifier dropped a handful of
// declaration separators. The result is syntactically invalid CSS such as:
//
//   transition:... .2s&:before{...}              value glued to a nested '&' selector
//   ...cubic-bezier(0,0,.96,1.03)@media (hover:hover){   value glued to an at-rule
//   ...outline-offset:-1rem--tw-content:"";      '}' of a nested block glued to a decl
//
// @tailwindcss/vite compiles app.css and resolves every @import *itself*
// (via @tailwindcss/node's readFile), then runs each imported file through
// Lightning CSS. Lightning CSS either crashes with
// `SyntaxError: Unexpected token CurlyBracketBlock` on these files or silently
// drops the affected nested blocks - so the switch knob (&:before) and other
// pseudo-elements vanished when the components were @import'ed directly.
//
// Why a transform() hook does NOT work
// ====================================
// A normal Vite plugin's `transform` only runs on modules Vite loads. The
// flyonui components are @import'ed *inside* app.css and read directly from
// disk by Tailwind's compiler, so they never reach Vite's transform pipeline.
// The only way to hand Tailwind repaired CSS is to put repaired files on disk
// and @import *those*.
//
// What this plugin does
// =====================
// On `buildStart` (before @tailwindcss/vite's transform runs) it copies each
// whitelisted component from node_modules/flyonui/components/ into
// ./flyonui-patched/, applying the conservative repair below to each file.
// app.css then @imports the patched copies, and Tailwind reads them cleanly.
//
//   - node_modules is never modified (a plain `npm install` stays the source
//     of truth; the patched dir is derived build output).
//   - the copy is idempotent: if a patched file is already up to date (same
//     source mtime + same repair result) it is not rewritten, so the Vite
//     watcher does not loop.
//
// The repair is conservative - it only inserts a `; ` (or ` `) separator
// between two tokens that are obviously a value/declaration boundary the
// minifier fused together:
//
//   - a value char ')' digit '%' '.' 's' '-' immediately before an '@' at-rule
//   - a value char ')' digit '%' '.' 's'  immediately before a '&' nested selector
//   - a '}' immediately followed by a '--' custom property (fused @media close + next decl)
//   - the known flyonui 2.4.1 '@media print' typo (missing '}' after 'solid')
//
// The patterns only match a *value* char on the left, so legitimate CSS (e.g.
// `cubic-bezier(0,0,.96,1.03)` has no '@' after it, `oklch(...)inset` is one
// value) is left untouched. After the repair all eight component files parse
// cleanly through Lightning CSS (verified) and their &-nested blocks survive.

import { readFileSync, writeFileSync, mkdirSync, statSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

// Which flyonui component files the shells actually use. Everything else in
// node_modules/flyonui/components is not imported, so it is not staged. This
// list must stay in sync with the @import block in app.css.
const COMPONENTS = [
  'button', 'card', 'range', 'switches',
  'badge', 'kbd', 'label', 'divider',
];

const PATCHED_DIR = path.join(__dirname, 'flyonui-patched');

function repair(css) {
  // 1) flyonui 2.4.1 typo: an unterminated @media print block fuses 'solid'
  //    with 'outline-offset'. Close the @media and keep outline-offset as a
  //    sibling declaration.
  css = css.replace(
    /@media print\{outline:\.25rem solidoutline-offset:-1rem\}/g,
    '@media print{outline:.25rem solid}outline-offset:-1rem',
  );
  // 2) a value char fused with the start of an '@' at-rule (e.g. '1.03)@media',
  //    'translate:0@media'). Insert a ';' to terminate the previous declaration.
  css = css.replace(/([)\d%.s-])(?=@)/g, '$1; ');
  // 3) a value char fused with a '&' nested selector (e.g. '.2s&:before',
  //    '1.03)&:hover'). Insert a ';' before the '&'.
  css = css.replace(/([)\d%.s])(?=&:)/g, '$1; ');
  // 4) a value fused with the next '--tw-content' custom property. After
  //    patch #1 closes the @media print block, the '}' that used to precede
  //    '--tw-content' moves inside the @media, leaving a value char ('m' from
  //    '-1rem') directly before '--tw-content' (e.g. ':-1rem--tw-content').
  //    A value char (letter/digit/%) before '--tw-content' (and not a '(',
  //    which would be a valid var() reference) is always a fused boundary.
  //    '--tw-content' is a unique name, so this cannot collide with a longer
  //    property like '--text-base--line-height'.
  css = css.replace(/([a-zA-Z0-9%)-])--tw-content/g, '$1; --tw-content');
  return css;
}

// Copy node_modules/flyonui/components/<name>.css -> flyonui-patched/<name>.css
// applying repair(). Skips the write when the staged file already matches the
// current source (by content), so repeated builds / the dev watcher are no-ops.
function stagePatchedComponents() {
  const flyonDir = path.join(__dirname, 'node_modules', 'flyonui', 'components');
  mkdirSync(PATCHED_DIR, { recursive: true });
  for (const name of COMPONENTS) {
    const src = path.join(flyonDir, `${name}.css`);
    if (!existsSync(src)) {
      throw new Error(`flyonui component missing: ${src}`);
    }
    const repaired = repair(readFileSync(src, 'utf8'));
    const out = path.join(PATCHED_DIR, `${name}.css`);
    const current = existsSync(out) ? readFileSync(out, 'utf8') : null;
    if (current !== repaired) {
      writeFileSync(out, repaired, 'utf8');
    }
  }
}

export default function flyonuiRepair() {
  return {
    name: 'flyonui-repair',
    enforce: 'pre',
    buildStart() {
      stagePatchedComponents();
    },
    // In dev (vite serve) buildStart runs once at startup; the watcher will
    // re-run the transform of app.css when a staged file changes, which is
    // fine because the staged files only change when node_modules changes.
  };
}

export { repair, stagePatchedComponents, COMPONENTS, PATCHED_DIR };
