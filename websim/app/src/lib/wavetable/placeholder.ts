// Placeholder wavetable surface for the /editor 3D preview (WaveTableView3D).
//
// A real procedural generator (ProcTable / seed, see drone-osc/osc_procedural.h)
// is not wired in yet. This module produces the SAME shape the editor will
// consume: N frames (lines) x P points, values in [-1, +1]. Each frame is a
// POLYLINE (P independent points), so it can be straight OR curved — the
// renderer does not assume straightness, it just plots the P points.
//
// The default fill uses a simple harmonic family (per-frame harmonic + phase +
// slight amplitude taper) so the surface reads as a recognisable morphing
// wavetable rather than flat noise. Replace `makePlaceholderFrames` with the
// real generator output (same `number[][]` contract) when the DSP side lands.

export const EDITOR_FRAMES = 16; // morph frames (Z rows) -> 16 polylines
export const EDITOR_POINTS = 256; // points per frame (X axis) -> wavetable width

export function makePlaceholderFrames(
  frames = EDITOR_FRAMES,
  points = EDITOR_POINTS
): number[][] {
  const data: number[][] = [];
  for (let f = 0; f < frames; f++) {
    // Frame 0 -> low harmonic (closed/warm), last frame -> high (open/dense),
    // mirroring the DRONE proc-bank "closed -> open" morph direction.
    const harmonic = 1 + Math.round((f / Math.max(1, frames - 1)) * 7); // 1..8
    const phase = (f / frames) * 0.5; // slow phase drift across the morph
    const amp = 0.75 - 0.25 * (f / Math.max(1, frames - 1)); // gentle taper
    const row = new Array(points);
    for (let i = 0; i < points; i++) {
      const x = i / points; // 0..1 phase
      // Main harmonic + a faint 2nd partial + tiny per-point jitter so the
      // polyline is visibly curved (not a single straight chord).
      const v =
        amp * Math.sin(2 * Math.PI * (harmonic * x + phase)) +
        0.15 * amp * Math.sin(2 * Math.PI * (2 * harmonic * x + phase * 2)) +
        0.01 * Math.sin(2 * Math.PI * 13 * x + f);
      row[i] = Math.max(-1, Math.min(1, v));
    }
    data.push(row);
  }
  return data;
}

export function defaultWavetable(): number[][] {
  return makePlaceholderFrames();
}
