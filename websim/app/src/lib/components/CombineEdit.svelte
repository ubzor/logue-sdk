<!-- CombineEdit — settings for how the active wave's up-to-four sources are
     merged into one signal. Rendered as the 5th column of the source row.
     Contains:
       - a combine-type select (6 methods)
       - two boolean modifiers: Invert / Mirror
       - the OUTPUT warp stack (applied after combine + modifiers)
     Minimalist styling, matching the source cards (.combine-card). -->
<script lang="ts">
	import { editor } from '$lib/editor/store.svelte';
	import { COMBINE_MODES, type CombineMode } from '$lib/editor/types';
	import WarpStack from '$lib/components/WarpStack.svelte';

	// The active wave index (the parent only renders this when a wave is active).
	let { waveIndex } = $props();

	// Live view of the active wave. $derived keeps this reactive to `waveIndex`
	// so switching frames re-reads the NEW wave's combine/modifiers (a plain
	// const would stick to the list captured at first render).
	const wave = $derived.by(() => editor.waves[waveIndex]);

	function onCombine(e: Event) {
		editor.setCombine(waveIndex, (e.currentTarget as HTMLSelectElement).value as CombineMode);
	}
</script>

<div class="combine-card flex flex-col gap-1.5">
	<!-- Header -->
	<div class="text-xs font-semibold">Combine</div>

	<!-- Combine type -->
	<div class="flex items-center gap-2">
		<span class="combine-lbl">Type</span>
		<select class="select select-primary select-xs flex-1 min-w-0"
			value={wave.combine}
			onchange={onCombine}
			title="Combine method">
			{#each COMBINE_MODES as m (m)}
				<option value={m}>{m}</option>
			{/each}
		</select>
	</div>

	<!-- Modifiers -->
	<div class="combine-mods">
		<label class="combine-mod">
			<input type="checkbox" class="switch switch-primary switch-xs"
				checked={wave.modifiers.invert}
				onchange={(e) => editor.setModifier(waveIndex, 'invert', e.currentTarget.checked)} />
			<span>Invert</span>
		</label>
		<label class="combine-mod">
			<input type="checkbox" class="switch switch-primary switch-xs"
				checked={wave.modifiers.mirror}
				onchange={(e) => editor.setModifier(waveIndex, 'mirror', e.currentTarget.checked)} />
			<span>Mirror</span>
		</label>
	</div>

	<!-- Output warp stack (after combine + modifiers) -->
	<div class="combine-warps">
		<span class="text-[11px] uppercase tracking-wide opacity-60">Warps</span>
		<WarpStack waveIndex={waveIndex} target="output" />
	</div>
</div>

<style>
	.combine-card {
		padding: 0.5rem;
		border-radius: 0.375rem;
		border: 1px solid color-mix(in srgb, var(--color-border, #555) 40%, transparent);
		min-width: 0;
	}
	.combine-lbl {
		width: 2.25rem;
		flex-shrink: 0;
		font-size: 10px;
		opacity: 0.7;
	}
	.combine-mods {
		display: flex;
		gap: 0.75rem;
	}
	.combine-mod {
		display: flex;
		align-items: center;
		gap: 0.375rem;
		font-size: 11px;
	}
	.combine-warps {
		display: flex;
		flex-direction: column;
		gap: 0.375rem;
		margin-top: 0.25rem;
		border-top: 1px dashed color-mix(in srgb, var(--color-border, #555) 40%, transparent);
		padding-top: 0.375rem;
	}
</style>
