<!-- OSC: parameters card (2/3 width). Svelte-driven (Вариант B).
     Состояние — из audioState (stores.ts), намерения — через store-хелперы
     (bus -> core.ts -> wasm). Пресет-банк + 3 колонки edit-параметров из
     PARAM_GROUPS (core.ts). Radio-группы — кнопки (idx -> raw через
     rawFromRadioGroup); остальные — слайдеры 0..1023.
     Рэндом/Reset + сохранённые пресеты (stage 4: /api/presets). Morph —
     подсветка движущихся слайдеров ($audioState.morphing). -->
<script>
	import {
		audioState, presetList, setParam, randomize, applyPreset,
		savePreset, deletePreset,
	} from '$lib/audio/stores';
	import { PARAM_GROUPS, formatParamValue, rawFromRadioGroup } from '$lib/audio/core';

	// Radio-индексы edit-параметров (1:1 legacy).
	const RADIO_IDX = new Set([13, 14, 40, 42, 46, 49]);
	// edit-idx -> radio group name.
	const IDX_TO_GROUP = {
		13: 'WaveBank', 14: 'WaveBank2',
		40: 'LfoShape1', 42: 'LfoShape2', 46: 'LfoShape3',
		49: 'FmModWave',
	};
	// radio group -> edit-idx (для setParam).
	const GROUP_TO_IDX = {
		WaveBank: 13, WaveBank2: 14,
		LfoShape1: 40, LfoShape2: 42, LfoShape3: 46,
		FmModWave: 49,
	};

	const fmtSlider = (i, raw) => formatParamValue(i, raw);

	const RADIO_OPTS = {
		WaveBank: ['Saw', 'Saw', 'Saw', 'Saw', 'Saw', 'Saw'],
		WaveBank2: ['Saw', 'Saw', 'Saw', 'Saw', 'Saw', 'Saw'],
		LfoShape1: ['Sin', 'S', 'Walk'],
		LfoShape2: ['Sin', 'S', 'Walk'],
		LfoShape3: ['Sin', 'S', 'Walk'],
		FmModWave: ['Saw', 'Saw', 'Sin', 'Tri', 'Noise'],
	};
	const radioOpts = (group) => RADIO_OPTS[group] ?? [];

	// Radio-группа -> edit-индекс (для setParam).
	const RADIO_TO_IDX = {
		WaveBank: 9, WaveBank2: 10, LfoShape1: 40,
		LfoShape2: 42, LfoShape3: 46, FmModWave: 49,
	};

	// Список сохранённых пресетов (stage 4) — store presetList, обновляется
	// core.ts через bus 'preset:list' (GET /api/presets на старт + save/delete).
	// $presetList — reactive snapshot (auto-subscribe).

	// Save-current UI: prompt for a name, then save the live 55-raw state.
	let saveBusy = $state(false);
	let presetMsg = $state('');
	function doSave() {
		const name = (window.prompt('Save current sound as preset:', '') || '').trim();
		if (!name) return;
		saveBusy = true;
		presetMsg = 'Saving ' + name + '…';
		savePreset(name, $audioState.params);
	}

	function doDelete(name) {
		presetMsg = 'Deleting ' + name + '…';
		deletePreset(name);
	}

	// Clear the transient status line after a short delay (fire-and-forget).
	$effect(() => {
		if (!presetMsg) return;
		const t = setTimeout(() => { presetMsg = ''; }, 1500);
		return () => clearTimeout(t);
	});
</script>

<div class="min-w-0 flex-[2]">
	<div class="flex flex-col items-stretch gap-3">
		<div class="flex flex-wrap gap-2 items-center">
			<button class="preset-btn" disabled={$audioState.activePreset !== null} onclick={() => randomize(0)}>Rnd</button>
			<button class="preset-btn" disabled={$audioState.activePreset !== null} onclick={() => randomize(1)}>Rnd-Osc</button>
			<button class="preset-btn" disabled={$audioState.activePreset !== null} onclick={() => randomize(2)}>Rnd-All</button>
			<button class="preset-btn" disabled={$audioState.activePreset !== null} onclick={() => applyPreset('Reset')}>Reset</button>
			<button class="preset-btn" onclick={doSave} disabled={saveBusy}>Save</button>
			{#each $presetList as name (name)}
				<button class="preset-btn preset-btn--saved" disabled={$audioState.activePreset !== null}
					onclick={() => applyPreset(name)}>{name}</button>
				<button class="preset-btn preset-btn--del" title="Delete preset"
					onclick={() => doDelete(name)}>✕</button>
			{/each}
			{#if presetMsg}<span class="preset-status">{presetMsg}</span>{/if}
		</div>

		<div class="grid grid-cols-3 gap-x-8 gap-y-4 items-stretch">
			{#each [0, 1, 2] as col (col)}
				<div class="flex flex-col gap-2">
					{#each PARAM_GROUPS as g (g.title + JSON.stringify(g.items))}
						{#if g.col === col}
							<div class="param-group">
								{#if g.title}<div class="param-group-title">{g.title}</div>{/if}
								{#each g.items as i (i)}
									{#if RADIO_IDX.has(i)}
										{@const group = IDX_TO_GROUP[i]}
										<div class="param-row">
											<span class="param-row-label">{i}</span>
											<div class="radio-row">
												{#each radioOpts(group) as label, idx (idx)}
													<button type="button"
														class="radio-btn {$audioState.radios[group] === idx ? 'radio-btn--on' : ''}"
														onclick={() => {
															const [raw] = rawFromRadioGroup(group, idx);
															setParam(GROUP_TO_IDX[group], raw);
														}}>
														{label}
													</button>
												{/each}
											</div>
										</div>
									{:else}
										<div class="param-row {$audioState.morphing.has(i) ? 'param-row--morphing' : ''}">
											<span class="param-row-label">{i}</span>
											<input type="range" min="0" max="1023" step="1"
												class="range range-primary range-sm"
								bind:value={$audioState.params[i]}
												oninput={() => setParam(i, $audioState.params[i])} />
											<span class="param-row-value">{fmtSlider(i, $audioState.params[i])}</span>
										</div>
									{/if}
								{/each}
							</div>
						{/if}
					{/each}
				</div>
			{/each}
		</div>
	</div>
</div>

<style>
	.preset-btn { font-size: 0.72rem; padding: 0.3rem 0.6rem; cursor: pointer; }
	.preset-btn:disabled { opacity: 0.4; cursor: default; }
	.preset-btn--saved { font-weight: 600; }
	.preset-btn--del { padding: 0.3rem 0.45rem; opacity: 0.6; }
	.preset-btn--del:hover { opacity: 1; }
	.preset-status { font-size: 0.7rem; opacity: 0.6; margin-left: 0.25rem; }
	.param-group-title { font-size: 0.65rem; letter-spacing: 0.12em; text-transform: uppercase; opacity: 0.55; margin-bottom: 0.25rem; }
	.param-row { display: grid; grid-template-columns: 3.5rem 1fr 3rem; align-items: center; gap: 0.5rem; font-size: 0.72rem; }
	.param-row--morphing { outline: 1px solid rgba(255, 170, 60, 0.7); outline-offset: 1px; border-radius: 2px; }
	.param-row-label { text-transform: uppercase; opacity: 0.8; }
	.param-row-value { text-align: right; opacity: 0.7; font-variant-numeric: tabular-nums; }
	.radio-row { display: flex; gap: 0.25rem; flex-wrap: wrap; }
	.radio-btn { font-size: 0.65rem; padding: 0.15rem 0.4rem; cursor: pointer; opacity: 0.6; }
	.radio-btn--on { opacity: 1; outline: 1px solid currentColor; }
</style>
