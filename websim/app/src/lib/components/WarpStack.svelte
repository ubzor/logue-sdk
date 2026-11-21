<!-- WarpStack — editor for ONE ordered warp stack of the ACTIVE wave.
     Used for both per-source stacks (target="source", srcIndex) and the
     combined-output stack (target="output"). All reads/writes go through the
     editor store so mutations are reactive and shared with the 3D view.
     Each warp row: enabled toggle, type badge, parameters (number -> slider,
     mode/window -> select, math -> text) and up / down / remove controls.
     An "Add Warp" button appends a new warp of the chosen type. -->
<script lang="ts">
	import { editor } from '$lib/editor/store.svelte';
	import {
		WARP_TYPES,
		type WarpItem,
		WARP_META,
		WAVESHAPE_MODES,
		CZ_WINDOWS,
		type WarpType,
		type ParamSpec,
	} from '$lib/editor/types';

	// waveIndex — the ACTIVE wave this stack belongs to (the parent only
	// renders this when a wave is active, so this is always a valid index).
	let { waveIndex, target, srcIndex = 0 } = $props();

	// The ordered list we render. Calling the getter inside the component body
	// makes the read reactive: Svelte tracks the accessed store properties and
	// re-renders when they change.
	const stack: WarpItem[] = editor.warpList(waveIndex, target, srcIndex);

	// Which string-enum a param belongs to (for the <select> options).
	function enumOptions(key: string, p: ParamSpec): readonly string[] {
		if (p.kind === 'mode') return WAVESHAPE_MODES as readonly string[];
		if (p.kind === 'window') return CZ_WINDOWS as readonly string[];
		return [] as readonly string[];
	}

	// Format a numeric param for the value badge.
	function fmt(v: number | string): string {
		if (typeof v !== 'number') return String(v);
		return Math.abs(v) < 1 && v !== 0 ? v.toFixed(2).replace(/0+$/, '').replace(/\.$/, '') : String(v);
	}

	function onAdd(e: Event) {
		const sel = e.currentTarget as HTMLSelectElement;
		const type = sel.value as WarpType;
		editor.addWarp(waveIndex, target, type, srcIndex);
		sel.selectedIndex = -1; // reset the "Add" chooser
	}
</script>

<div class="flex flex-col gap-1.5 warp-stack">
	<!-- Existing warps (top -> bottom = signal flow) -->
	{#if stack.length > 0}
		{#each stack as item, i (item.id)}
			{@const meta = WARP_META[item.type]}
			<div class="rounded-md border p-2 flex flex-col gap-1.5"
				class:warp-row--on={item.enabled}
				class:warp-row--off={!item.enabled}>

				<!-- Row header: toggle + type + reorder/remove -->
				<div class="flex items-center gap-2">
					<input type="checkbox" class="switch switch-primary switch-xs"
						checked={item.enabled}
						onchange={(e) => editor.setWarpEnabled(waveIndex, target, item.id, e.currentTarget.checked, srcIndex)} />
					<select class="select select-primary select-xs flex-1 min-w-0 warp-type"
						value={item.type}
						onchange={(e) => editor.setWarpType(waveIndex, target, item.id, (e.currentTarget as HTMLSelectElement).value as WarpType, srcIndex)}
						title="Warp type">
						{#each WARP_TYPES as t (t)}
							<option value={t}>{t}</option>
						{/each}
					</select>
					<button type="button" class="btn btn-ghost btn-xs" disabled={i === 0}
						onclick={() => editor.moveWarp(waveIndex, target, item.id, -1, srcIndex)} title="Move up">↑</button>
					<button type="button" class="btn btn-ghost btn-xs" disabled={i === stack.length - 1}
						onclick={() => editor.moveWarp(waveIndex, target, item.id, 1, srcIndex)} title="Move down">↓</button>
					<button type="button" class="btn btn-ghost btn-xs text-error"
						onclick={() => editor.removeWarp(waveIndex, target, item.id, srcIndex)} title="Remove warp">✕</button>
				</div>

				<!-- Parameters (only when enabled) -->
				{#if item.enabled}
					{#each meta.params as p (p.key)}
						{#if p.kind === 'math'}
							<div class="flex items-center gap-2">
								<span class="w-14 text-[11px] opacity-70 shrink-0">{p.label ?? p.key}</span>
								<input type="text" class="input input-primary input-xs flex-1 min-w-0 font-mono"
								value={String(item.params[p.key] ?? '')}
								onchange={(e) => editor.setWarpParam(waveIndex, target, item.id, p.key, e.currentTarget.value, srcIndex)} />
							</div>
					{:else if p.kind === 'mode' || p.kind === 'window'}
						<div class="flex items-center gap-2">
							<span class="w-14 text-[11px] opacity-70 shrink-0">{p.label ?? p.key}</span>
							<select class="select select-primary select-xs flex-1 min-w-0"
								value={String(item.params[p.key] ?? '')}
								onchange={(e) => editor.setWarpParam(waveIndex, target, item.id, p.key, e.currentTarget.value, srcIndex)}>
								{#each enumOptions(p.key, p) as opt (opt)}
									<option value={opt}>{opt}</option>
								{/each}
							</select>
						</div>
					{:else}
						<div class="flex items-center gap-2">
							<span class="w-14 text-[11px] opacity-70 shrink-0">{p.label ?? p.key}</span>
							<input type="range"
								min={p.min} max={p.max} step={p.step}
								class="range range-primary range-xs flex-1 min-w-0"
								bind:value={item.params[p.key] as number}
								oninput={() => editor.setWarpParam(waveIndex, target, item.id, p.key, item.params[p.key], srcIndex)} />
							<span class="badge badge-soft badge-primary badge-xs font-mono shrink-0 min-w-10 text-right">{fmt(item.params[p.key])}</span>
						</div>
					{/if}
				{/each}
				{/if}
			</div>
		{/each}
	{:else}
		<div class="text-[11px] opacity-50 px-1">No warps in this stack</div>
	{/if}

	<!-- Add warp -->
	<div class="flex items-center gap-2">
		<span class="w-14 text-[11px] opacity-70 shrink-0">Add</span>
		<select class="select select-primary select-xs flex-1 min-w-0" value=""
			onchange={onAdd}>
			<option value="" disabled>Add a warp…</option>
			{#each WARP_TYPES as t (t)}
				<option value={t}>{t}</option>
			{/each}
		</select>
	</div>
</div>

<style>
	.warp-row--on {
		border-color: color-mix(in srgb, var(--color-primary, #8b5cf6) 35%, transparent);
	}
	.warp-row--off {
		border-color: transparent;
		opacity: 0.5;
	}
</style>
