<!-- SourceEdit — panel for editing the SOURCES of the active wave.
     The four source slots are laid out in a single HORIZONTAL ROW (4 equal
     columns) to save vertical space. Each slot has:
       - enabled toggle
       - type selector (30 source shapes)
       - 4 sliders: Harmonic / Phase / Gain / Bias
       - a per-source warp stack
     Plus an "Add source" button (enables the next disabled slot) and
     per-source "Remove" (disables the slot).
     The whole panel is disabled (pointer-events none, dimmed) when
     no wave is active (active === null). -->
<script>
	import { editor } from '$lib/editor/store.svelte';
	import { SOURCE_TYPES, SOURCES_PER_WAVE, SOURCE_PARAM_SPECS } from '$lib/editor/types';
	import WarpStack from '$lib/components/WarpStack.svelte';
	import CombineEdit from '$lib/components/CombineEdit.svelte';

	// Reactive prop from the parent (+page.svelte).
	let { active = $bindable(null) } = $props();

	// Slider formatting.
	function fmtVal(key, v) {
		if (key === 'harmonic') return String(Math.round(v));
		return v.toFixed(2);
	}

	// Add: find the first disabled slot and enable it.
	function addSource() {
		if (active === null) return;
		const w = editor.waves[active];
		for (let i = 0; i < SOURCES_PER_WAVE; i++) {
			if (!w.sources[i].enabled) {
				editor.setSourceEnabled(active, i, true);
				return;
			}
		}
	}

	// Remove: disable the slot (if enabled).
	function removeSource(srcIndex) {
		if (active === null) return;
		editor.setSourceEnabled(active, srcIndex, false);
	}
</script>

<div class="flex flex-col gap-3 source-edit" class:source-edit--disabled={active === null}>
	<!-- Header -->
	<div class="flex items-center justify-between">
		<div class="text-xs uppercase tracking-wide opacity-60">
			Sources {#if active !== null}— Wave <span class="font-mono">{active + 1}</span>{/if}
		</div>
		<button type="button"
			class="btn btn-soft btn-xs"
			disabled={active === null || editor.waves[active].sources.every((s) => s.enabled)}
			onclick={addSource}
			title="Add a source (enables the next empty slot)">
			+ Add
		</button>
	</div>

	<!-- Source slots (single horizontal row, one active wave).
	     {#key active} forces the WHOLE panel (source cards + their warp
	     stacks + the CombineEdit column) to be torn down and rebuilt whenever
	     the active wave changes. This guarantees every child re-reads
	     editor.waves[active] from scratch, so no per-wave state (warp stacks,
	     combine type, modifiers) leaks from one frame to another. -->
	{#if active !== null}
		{#key active}
		<div class="src-row">
			{#each editor.waves[active].sources as src, sIdx (sIdx)}
				<div class="src-card flex flex-col gap-1.5"
					class:src-slot--on={src.enabled}
					class:border-transparent={!src.enabled}
					class:opacity-50={!src.enabled}>

					<!-- Source header row -->
					<div class="flex items-center gap-1.5">
						<input type="checkbox" class="switch switch-primary switch-xs"
							checked={src.enabled}
							onchange={(e) => editor.setSourceEnabled(active, sIdx, e.currentTarget.checked)} />
						<span class="text-xs font-semibold flex-1">
							Source {sIdx + 1}
						</span>
						<button type="button"
							class="btn btn-ghost btn-xs src-remove"
							disabled={!src.enabled}
							onclick={() => removeSource(sIdx)}
							title="Remove this source (disables the slot)">
							−
						</button>
					</div>

					<!-- Type selector + sliders (enabled only) -->
					{#if src.enabled}
						<select class="select select-primary select-xs w-full"
							value={src.type}
							onchange={(e) => editor.setSourceType(active, sIdx, e.currentTarget.value)}
							title="Source type">
							{#each SOURCE_TYPES as st (st)}
								<option value={st}>{st}</option>
							{/each}
						</select>

						{#each SOURCE_PARAM_SPECS as spec (spec.key)}
							<div class="src-param">
								<span class="src-param__lbl">{spec.label ?? spec.key}</span>
								<input type="range"
									min={spec.min} max={spec.max} step={spec.step}
									class="range range-primary range-xs flex-1 min-w-0"
									bind:value={src[spec.key]}
									oninput={() => editor.setSourceParam(active, sIdx, spec.key, src[spec.key])} />
								<span class="badge badge-soft badge-primary badge-xs font-mono shrink-0 text-right src-param__val">
									{fmtVal(spec.key, src[spec.key])}
								</span>
							</div>
						{/each}

						<!-- Per-source warp stack -->
						<div class="src-warps">
							<span class="text-[11px] uppercase tracking-wide opacity-60">Warps</span>
							<WarpStack waveIndex={active} target="source" srcIndex={sIdx} />
						</div>
					{/if}
				</div>
			{/each}

		<!-- 5th column: combine settings for the active wave -->
		<CombineEdit waveIndex={active} />
		</div>
		{/key}
	{/if}
</div>

<style>
	/* Four equal columns in one row. On narrow screens the cards wrap to two
	   columns so they stay usable. */
	.src-row {
		display: grid;
		grid-template-columns: repeat(5, minmax(0, 1fr));
		gap: 0.5rem;
		align-items: start;
	}
	@media (max-width: 1279px) {
		.src-row { grid-template-columns: repeat(3, minmax(0, 1fr)); }
	}
	@media (max-width: 859px) {
		.src-row { grid-template-columns: repeat(2, minmax(0, 1fr)); }
	}
	@media (max-width: 559px) {
		.src-row { grid-template-columns: 1fr; }
	}

	.src-card {
		padding: 0.5rem;
		border-radius: 0.375rem;
		border: 1px solid color-mix(in srgb, var(--color-border, #555) 40%, transparent);
		min-width: 0;
	}
	.src-slot--on {
		border-color: color-mix(in srgb, var(--color-primary, #8b5cf6) 45%, transparent);
	}

	/* Compact param row: short label, slider, tiny value badge. */
	.src-param {
		display: flex;
		align-items: center;
		gap: 0.375rem;
	}
	.src-param__lbl {
		width: 2.25rem;
		flex-shrink: 0;
		font-size: 10px;
		opacity: 0.7;
		text-overflow: ellipsis;
		white-space: nowrap;
		overflow: hidden;
	}
	.src-param__val {
		min-width: 2.5rem;
		font-size: 10px;
		padding: 0 0.25rem;
	}
	.src-remove {
		line-height: 1;
		padding: 0 0.4rem;
	}
	.src-warps {
		display: flex;
		flex-direction: column;
		gap: 0.375rem;
		margin-top: 0.25rem;
		border-top: 1px dashed color-mix(in srgb, var(--color-border, #555) 40%, transparent);
		padding-top: 0.375rem;
	}

	.source-edit--disabled {
		pointer-events: none;
		opacity: 0.45;
	}
</style>
