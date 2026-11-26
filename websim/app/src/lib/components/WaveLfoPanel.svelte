<!-- WaveLfoPanel — the Wavetable morph-LFO settings (edit 35/36/40/41):
     Wave LFO Rate/Depth sliders + Shape radio (Sin/S/Walk) + Walk Gap slider
     (visible only in Walk mode, 1:1 legacy "hidden unless Walk" behaviour).
     Rendered in the "Wavetable" tab, to the RIGHT of the 3D surface view.

     EXCLUDED from randomization/reset: Rnd/Rnd-Osc/Rnd-All/Reset (core.ts
     randomizeAllRaw) never re-roll these values, and the preset-apply / rnd
     sync loops in core.ts keep the current state for 35/36/40/41. The panel
     writes through the same setParam/setRadio store helpers as OscPanel, so
     the sliders/radio always reflect $audioState live. -->
<script>
	import { audioState, setParam, setRadio } from '$lib/audio/stores';
	import { LFO_SHAPES, EDIT_PARAM_NAMES, formatParamValue } from '$lib/audio/core';

	// Edit indices of this panel (the WTE-morph LFO 1):
	//   35 = Wave LFO Rate (log 0.001..1 Hz, raw 0 = OFF)
	//   36 = Wave LFO Depth (0..100%)
	//   40 = Wave LFO Shape (radio: Sin/S/Walk, raw band centers 0/640/896)
	//   41 = Wave LFO Gap (log 1..300 s — only used by the Walk shape)
	const RATE = 35, DEPTH = 36, SHAPE = 40, GAP = 41;

	// Walk Gap row: shown only while the Shape radio is on Walk (idx 2).
	// Local $state is the SOLE source of truth for the Gap row visibility.
	// The radio's onchange writes it directly (synchronous, reliable).
	// No store subscription: the store's LfoShape1 value is NOT updated for
	// this group (core.ts skips 35/36/40/41 in all sync loops — the group
	// is user-owned and excluded from randomization/reset), so a subscription
	// would only echo stale values and fight the local state.
	let shape = $state(0);
	function onShapeChange(idx) {
		shape = idx;                     // local: toggles the Gap row
		setRadio('LfoShape1', idx);     // bus -> core -> DSP
	}
</script>

<div class="wte-lfo flex flex-col gap-2">
	<div class="text-xs uppercase tracking-wide opacity-60 mt-1">Wavetable LFO</div>

	<div class="flex items-center gap-3 {$audioState.morphing.has(RATE) ? 'param-row--morphing' : ''}">
		<span class="w-32 text-sm font-medium shrink-0 truncate">{EDIT_PARAM_NAMES[RATE]}</span>
		<input type="range" min="0" max="1023" step="1"
			class="range range-primary range-sm flex-1 min-w-0"
			bind:value={$audioState.params[RATE]}
			oninput={() => setParam(RATE, $audioState.params[RATE])} />
		<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{formatParamValue(RATE, $audioState.params[RATE])}</span>
	</div>

	<div class="flex items-center gap-3 {$audioState.morphing.has(DEPTH) ? 'param-row--morphing' : ''}">
		<span class="w-32 text-sm font-medium shrink-0 truncate">{EDIT_PARAM_NAMES[DEPTH]}</span>
		<input type="range" min="0" max="1023" step="1"
			class="range range-primary range-sm flex-1 min-w-0"
			bind:value={$audioState.params[DEPTH]}
			oninput={() => setParam(DEPTH, $audioState.params[DEPTH])} />
		<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{formatParamValue(DEPTH, $audioState.params[DEPTH])}</span>
	</div>

	<div class="flex items-center gap-3 {$audioState.morphing.has(SHAPE) ? 'param-row--morphing' : ''}">
		<span class="w-32 text-sm font-medium shrink-0 truncate">{EDIT_PARAM_NAMES[SHAPE]}</span>
		<div class="radio-row drop-shadow flex-1 min-w-0">
			{#each LFO_SHAPES as s, idx (idx)}
				<input type="radio"
					class="radio-row-item btn btn-soft btn-xs flex-1 min-w-0"
					name="LfoShape1"
					value={String(idx)}
					checked={shape === idx}
					aria-label={s.label}
					onchange={() => onShapeChange(idx)} />
			{/each}
		</div>
	</div>

	{#if shape === 2}
		<div class="flex items-center gap-3 {$audioState.morphing.has(GAP) ? 'param-row--morphing' : ''}">
			<span class="w-32 text-sm font-medium shrink-0 truncate">{EDIT_PARAM_NAMES[GAP]}</span>
			<input type="range" min="0" max="1023" step="1"
				class="range range-primary range-sm flex-1 min-w-0"
				bind:value={$audioState.params[GAP]}
				oninput={() => setParam(GAP, $audioState.params[GAP])} />
			<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{formatParamValue(GAP, $audioState.params[GAP])}</span>
		</div>
	{/if}
</div>

<style>
	/* Same "join radio with btn" geometry as OscPanel (.radio-row re-asserted
	   locally: Svelte scoped styles can't reach another module's classes). */
	.radio-row { display: flex; gap: 0; flex: 1 1 0; min-width: 0; }
	.radio-row .radio-row-item { &:checked { --btn-color: var(--color-primary); --btn-fg: var(--color-primary-content); } }
	.radio-row .radio-row-item:only-child {
		--join-ss: var(--radius-field);
		--join-se: var(--radius-field);
		--join-es: var(--radius-field);
		--join-ee: var(--radius-field);
	}
	.radio-row .radio-row-item:first-child:not(:last-child) {
		--join-ss: var(--radius-field);
		--join-se: 0;
		--join-es: var(--radius-field);
		--join-ee: 0;
	}
	.radio-row .radio-row-item:last-child:not(:first-child) {
		--join-ss: 0;
		--join-se: var(--radius-field);
		--join-es: 0;
		--join-ee: var(--radius-field);
	}
	.radio-row .radio-row-item:not(:first-child):not(:last-child) {
		--join-ss: 0;
		--join-se: 0;
		--join-es: 0;
		--join-ee: 0;
		margin-inline-start: calc(var(--border, 1px) * -1);
	}
</style>
