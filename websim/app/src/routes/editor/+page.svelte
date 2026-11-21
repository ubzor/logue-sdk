<!-- Wave Table Editor — 3D preview of the DRONE procedural wavetable.
     The surface is LIVE: it renders the editor store's waves (16 polylines,
     each from a WaveConfig via renderWave). Clicking a line makes it ACTIVE;
     editing that wave in the panel below re-renders ONLY that line. -->
<script>
	import WaveTableView3D from '$lib/components/WaveTableView3D.svelte';
	import SourceEdit from '$lib/components/SourceEdit.svelte';
	import { editor } from '$lib/editor/store.svelte';
	import { renderWave, EDITOR_FRAMES, EDITOR_POINTS } from '$lib/wavetable/render';

	let active = $state(null);

	/**
	 * The full 16-row surface, derived from the store.
	 *
	 * `$derived` re-evaluates whenever ANY reactive field of any wave changes
	 * (deep `$state` tracking). It is PURE — it never writes to reactive state —
	 * so it cannot feed back into itself and cannot trip
	 * `effect_update_depth_exceeded`. We re-render the WHOLE table here:
	 * renderWave is O(256×16) ≈ trivial, and a fresh array every time means
	 * the 3D view's `data` prop reference actually changes (which is what its
	 * internal `$effect` watches to rebuild the lines).
	 */
	let table = $derived(editor.waves.map((w, i) => renderWave(w, EDITOR_POINTS, i)));
	// (No $effect needed: `table` is a fresh array on every re-evaluation, so
	//  WaveTableView3D's own $effect keyed on its `data` prop rebuilds the 16
	//  lines automatically. A $effect here that wrote to reactive state is what
	//  previously tripped effect_update_depth_exceeded — avoided by design.)
</script>

<div class="flex flex-col gap-3">
	<div class="flex items-center gap-3">
		<span class="text-sm font-semibold tracking-wide">Wave Table Editor</span>
		<span class="badge badge-soft badge-primary badge-sm font-mono text-xs">
			{EDITOR_FRAMES} × {EDITOR_POINTS}
		</span>
		<span class="text-xs opacity-60">
			{#if active === null}
				Click a line to make it active
			{:else}
				Frame <span class="font-mono">{active + 1}</span> / {EDITOR_FRAMES} active
			{/if}
		</span>
	</div>

	<div class="card p-2 w-fit">
		{#if table}
			<WaveTableView3D data={table} bind:active={active} onActiveChange={(i) => active = i} />
		{:else}
			<div class="grid place-items-center text-xs opacity-60" style="width:640px;height:480px">Loading…</div>
		{/if}
	</div>

	<!-- Source editing panel (disabled until a wave is active) -->
	<SourceEdit active={active} />
</div>
