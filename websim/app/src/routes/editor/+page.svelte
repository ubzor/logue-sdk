<!-- Wave Table Editor — 3D preview of the DRONE procedural wavetable.
     Standalone route (no audio): it only renders the surface and lets you
     select a frame (line). The data is a placeholder (see lib/wavetable/
     placeholder.ts) until the real ProcTable/seed generator is wired in.
     Layout mirrors the simulator: a card wrapping the 3D view + a status
     line showing the active frame. -->
<script>
	import { onMount } from 'svelte';
	import WaveTableView3D from '$lib/components/WaveTableView3D.svelte';
	import { defaultWavetable, EDITOR_FRAMES, EDITOR_POINTS } from '$lib/wavetable/placeholder';

	let active = $state(null);
	let table = $state(null);

	onMount(() => {
		table = defaultWavetable();
	});

	function onActiveChange(i) {
		active = i;
	}
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
			<WaveTableView3D data={table} active={active} onActiveChange={onActiveChange} />
		{:else}
			<div class="grid place-items-center text-xs opacity-60" style="width:640px;height:480px">Loading…</div>
		{/if}
	</div>
</div>
