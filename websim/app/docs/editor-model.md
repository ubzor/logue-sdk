# Wave Table Editor — Data Model

The editor works with a **document** of **16 waves**. Each wave is a recipe that
turns up to four *sources* into a single waveform through a *combine* step and
an ordered *warp stack*. This document is the reference for the TypeScript model
(`src/lib/editor/types.ts`), the reactive store (`src/lib/editor/store.svelte.ts`),
and the (future) renderer.

> **Status:** structure only. The store holds the full model; the actual
> per-sample rendering of each wave into a 256-point polyline is a later step.

## 1. Document

```
EditorDocument
└── waves: WaveConfig[16]
```

16 waves = 16 morph frames = the 16 polylines shown on the `/editor` surface.

## 2. Wave (`WaveConfig`)

```ts
interface WaveConfig {
  name: string;                 // display name, editable
  sources: SourceConfig[4];     // up to four sources (fixed 4 slots)
  combine: CombineMode;         // how the sources are merged
  modifiers: { invert: boolean; mirror: boolean };
  perSourceWarp: WarpList[4];   // warp stack per source (parallel to sources)
  outputWarp: WarpList;         // warp stack on the combined output
}
```

### Signal flow

```
for i in 0..3 (enabled sources):
    s[i](t) = shape(t; harmonic, phase, gain, bias)   // per-source shape
             -> perSourceWarp[i]                       // per-source warp stack

combined(t) = combine( s[0], s[1], s[2], s[3] )       // one of 6 methods
             -> modifiers:  mirror (t -> 1 - t),  invert (v -> -v)
             -> outputWarp                             // output warp stack (ordered)
```

The order is fixed: **source shapes → per-source warps → combine →
invert/mirror → output warps**. Disabled source slots contribute nothing
(treated as zero for Sum/Min/Max, identity for Multiply).

## 3. Sources (`SourceConfig`)

Each wave has up to **4** sources. A source slot is always present; when
`enabled === false` it is "empty". Every source shares the same four settings:

| Setting | Type | Range | Default | Meaning |
|---|---|---|---|---|
| `harmonic` | number | 1–16 (step 1) | 1 | Dominant partial / repetition factor |
| `phase` | number | 0–1 | 0 | Phase offset (fraction of a period) |
| `gain` | number | 0–2 | 1 | Amplitude multiplier |
| `bias` | number | −1…1 | 0 | Vertical DC offset |

Plus the source **type** and an **enabled** flag.

### The 30 source shapes

| # | Source | # | Source | # | Source |
|---|---|---|---|---|---|
| 1 | Sine | 11 | Stairs | 21 | Cubed |
| 2 | Triangle | 12 | Sine² | 22 | Two Bumps |
| 3 | Saw | 13 | FM Sine | 23 | Folded |
| 4 | Square | 14 | Sinc | 24 | Asym Tri |
| 5 | Pulse | 15 | Chirp | 25 | Pinch |
| 6 | Half Sine | 16 | Wave Pkt | 26 | PD Sine |
| 7 | Parabola | 17 | Smooth Sq | 27 | Organ |
| 8 | Bell | 18 | Sharkfin | 28 | Sync Saw |
| 9 | S-Curve | 19 | Spike | 29 | Bit Crush |
| 10 | Trapezoid | 20 | Noise | 30 | Ring Sine |

> Source *rendering* (the actual `shape(t, ...)` math for each of the 30) is
> part of the later renderer step. The model only stores `type` + the 4 settings.

## 4. Combine (`CombineMode`)

How the (up to) four source signals are merged into one. **6 modes:**

| Mode | Definition (conceptually) |
|---|---|
| **Sum** | `s0 + s1 + s2 + s3` |
| **Multiply** | `s0 · s1 · s2 · s3` |
| **Min** | `min(s0, s1, s2, s3)` |
| **Max** | `max(s0, s1, s2, s3)` |
| **AM** | Amplitude-modulate `s0` by `s1` (others unused) |
| **Wrap** | Wrap/fold the summed phase across the period |

### Modifiers (applied to the combined signal, after combine)

| Modifier | Effect |
|---|---|
| `invert` (bool) | Flip vertically: `v -> -v` |
| `mirror` (bool) | Mirror horizontally: `t -> 1 - t` |

## 5. Warp stack (`WarpList`)

A warp stack is an **ordered list** of warp items. Order matters — items are
applied **top to bottom**. The same stack concept is used in two places per
wave:

- **`perSourceWarp[i]`** — applied to each source individually (4 stacks).
- **`outputWarp`** — applied to the combined output (1 stack).

```ts
interface WarpItem {
  id: string;        // stable unique id (UI key / reorder)
  type: WarpType;    // one of 15
  enabled: boolean;  // disabled items are skipped
  params: Record<string, number | string>;
}
```

### The 15 warp types and their parameters

| Warp | Parameters (kind / range / default) |
|---|---|
| **Wavefold** | Gain (0–4, 1) · Bias (−1…1, 0) |
| **Hard Sync** | Ratio (0–4, 1) |
| **FM** | Ratio (0–8, 1) · Index (0–4, 0.5) |
| **Waveshape** | Mode (Tahn / Atan / Cubic / Sin; `Tahn`) · Drive (0–4, 1) |
| **Bitcrush** | Bits (1–16, 8) · Rate (0–1, 1) |
| **Exponent** | Power (0.1–8, 1) |
| **CZ Reso** | Reso (0–1, 0.5) · Window (Saw / Tri / Trap; `Saw`) |
| **Bend** | Amount (−1…1, 0) |
| **PWM** | Width (0–1, 0.5) |
| **Asym** | Amount (−1…1, 0) |
| **Formant** | Shift (−1…1, 0) |
| **Spectral Tilt** | Tilt (0–2, 0.5) · Mix (0–1, 1) |
| **Ring Mod** | Freq (0–8, 1) · Depth (0–1, 1) |
| **Additive Shape** | 1…8 (−1…1, 0 each) · Mix (0–1, 0.5) |
| **Math** | F — a function of `(T, V)` (string expression, default `V`) |

Enumerated parameters:
- `Waveshape.Mode` ∈ **Tahn, Atan, Cubic, Sin**
- `CZ Reso.Window` ∈ **Saw, Tri, Trap**
- `Math.F` — an expression over `T` (normalized phase) and `V` (current value),
  e.g. `V`, `1 - V`, `V*T`, `sin(T)`.

> The **algorithm** for each warp (how its parameters reshape a sample) is part
> of the later renderer step. The model stores only the type, params, and order.

## 6. Example document (one wave, JSON)

```json
{
  "name": "Warm Fuzz",
  "sources": [
    { "enabled": true,  "type": "Saw",     "harmonic": 1, "phase": 0,    "gain": 0.8, "bias": 0 },
    { "enabled": true,  "type": "Sine",    "harmonic": 3, "phase": 0.25, "gain": 0.3, "bias": 0 },
    { "enabled": false, "type": "Square",  "harmonic": 1, "phase": 0,    "gain": 1,   "bias": 0 },
    { "enabled": false, "type": "Sine",    "harmonic": 1, "phase": 0,    "gain": 1,   "bias": 0 }
  ],
  "combine": "Sum",
  "modifiers": { "invert": false, "mirror": true },
  "perSourceWarp": [
    [ { "id": "w_a1", "type": "Asym",      "enabled": true,  "params": { "Amount": 0.25 } } ],
    [ { "id": "w_b1", "type": "Bitcrush",  "enabled": false, "params": { "Bits": 8, "Rate": 1 } } ],
    [],
    []
  ],
  "outputWarp": [
    { "id": "w_o1", "type": "Waveshape",    "enabled": true, "params": { "Mode": "Tahn", "Drive": 1.5 } },
    { "id": "w_o2", "type": "Wavefold",     "enabled": true, "params": { "Gain": 1.2, "Bias": 0 } },
    { "id": "w_o3", "type": "Ring Mod",     "enabled": false, "params": { "Freq": 2, "Depth": 0.5 } }
  ]
}
```

A full document is `{ "waves": [ <16 of the above> ] }`.

## 7. Store API (`store.svelte.ts`)

`editor` is a `Svelte 5` runes singleton (`waves = $state(...)`). Helpers:

| Method | Purpose |
|---|---|
| `reset()` | Replace the document with a fresh default (16 waves). |
| `randomize()` | Fill all 16 waves with random sources/combine/modifiers/warps. |
| `setWaveName(i, name)` | Rename a wave. |
| `setSourceEnabled(w, s, on)` | Toggle a source slot. |
| `setSourceType(w, s, type)` | Change a source's shape. |
| `setSourceParam(w, s, 'harmonic'\|'phase'\|'gain'\|'bias', v)` | Set a source setting. |
| `setCombine(w, mode)` | Set the combine mode. |
| `setModifier(w, 'invert'\|'mirror', on)` | Toggle a modifier. |
| `warpList(w, target, s?)` | Read the target warp list (getter). |
| `addWarp(w, target, type, s?)` | Append a warp; returns the item (with its `id`). |
| `removeWarp(w, target, id, s?)` | Remove a warp by id. |
| `setWarpEnabled(w, target, id, on, s?)` | Toggle a warp. |
| `setWarpParam(w, target, id, key, v, s?)` | Set one warp parameter. |
| `setWarpParams(w, target, id, params, s?)` | Merge a set of warp parameters. |
| `moveWarp(w, target, id, -1\|+1, s?)` | Reorder a warp in its stack. |

`target` is `'output'` (→ `outputWarp`) or `'source'` (→ `perSourceWarp[s]`).

## 8. Where the model lives

| Concern | File |
|---|---|
| Types, catalogs, constants, defaults | `src/lib/editor/types.ts` |
| Reactive document + mutation helpers | `src/lib/editor/store.svelte.ts` |
| 3D surface preview (renderer, later step) | `src/lib/components/WaveTableView3D.svelte` |
| `/editor` route (UI, later step) | `src/routes/editor/+page.svelte` |
| This document | `docs/editor-model.md` |