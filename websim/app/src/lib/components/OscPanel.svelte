<!-- OSC: parameters card (2/3 width). Svelte-driven (Вариант B).
     Состояние — из audioState (stores.ts), намерения — через store-хелперы
     (bus -> core.ts -> wasm). Пресет-банк + 3 колонки edit-параметров из
     PARAM_GROUPS (core.ts). Radio-группы — "join radio" (1:1 legacy: FlyonUI
     octave-join, <input type="radio"> + aria-label) через setRadio;
     остальные — слайдеры 0..1023. Метки — из EDIT_PARAM_NAMES (1:1 legacy).
     Рэндом/Reset + сохранённые пресеты (stage 4: /api/presets). Morph —
     подсветка движущихся слайдеров ($audioState.morphing). -->
<script>
	import {
		audioState, presetList, setParam, setRadio, randomize, applyPreset,
		savePreset, deletePreset,
	} from '$lib/audio/stores';
	import {
		PARAM_GROUPS, EDIT_PARAM_NAMES, WAVE_BANKS, LFO_SHAPES, FM_MOD_WAVES,
		formatParamValue,
	} from '$lib/audio/core';

	// Radio-индексы edit-параметров (1:1 legacy).
	const RADIO_IDX = new Set([13, 14, 40, 42, 46, 49]);
	// edit-idx -> radio group name.
	const IDX_TO_GROUP = {
		13: 'WaveBank', 14: 'WaveBank2',
		40: 'LfoShape1', 42: 'LfoShape2', 46: 'LfoShape3',
		49: 'FmModWave',
	};
	// Radio-группа -> edit-индекс (для setParam).
	const GROUP_TO_IDX = {
		WaveBank: 13, WaveBank2: 14,
		LfoShape1: 40, LfoShape2: 42, LfoShape3: 46,
		FmModWave: 49,
	};

	// Radio-группа -> список подписей кнопок (1:1 legacy createEditableParam):
	// WAVE / WAVE 2 — банки A..F (WAVE_BANKS), Wave LFO Shape 1/2/3 — Sin/S/Walk
	// (LFO_SHAPES), FM Mod Wave — SIN/TRI/SAW/SQR/NOISE (FM_MOD_WAVES).
	// Ключи — те же, что в IDX_TO_GROUP.
	const RADIO_OPTS = {
		WaveBank: WAVE_BANKS,
		WaveBank2: WAVE_BANKS,
		LfoShape1: LFO_SHAPES.map((s) => s.label),
		LfoShape2: LFO_SHAPES.map((s) => s.label),
		LfoShape3: LFO_SHAPES.map((s) => s.label),
		FmModWave: FM_MOD_WAVES.map((s) => s.label),
	};
	const radioOpts = (group) => RADIO_OPTS[group] ?? [];

	const fmtSlider = (i, raw) => formatParamValue(i, raw);

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
			<!-- Preset bank (1:1 legacy: btn btn-outline btn-primary btn-sm; the active
			     preset keeps the btn-active fill via $audioState.activePreset) -->
			<button class="btn btn-outline btn-primary btn-sm" title="Re-roll EVERY parameter to a fresh audition-friendly preset (BPM-synced morph)."
			disabled={$audioState.activePreset !== null} onclick={() => randomize(0)}>Rnd</button>
			<button class="btn btn-outline btn-primary btn-sm" title="Re-roll a random SUBSET (2-6) of parameters to fresh safe values (a development around the current sound). BPM-synced morph."
			disabled={$audioState.activePreset !== null} onclick={() => randomize(1)}>Rnd-Osc</button>
			<button class="btn btn-outline btn-primary btn-sm" title="Nudge 2-4 parameters from ONE sphere (harmony / timbre / motion) of the LAST STABLE state -- a development that stays recognizably the same sound."
			disabled={$audioState.activePreset !== null} onclick={() => randomize(2)}>Rnd-All</button>
			<button class="btn btn-outline btn-primary btn-sm '$audioState.activePreset === 'Reset' ? 'btn-active' : ''}" title="Always restore the factory default parameters (instant, no morph)."
			disabled={$audioState.activePreset !== null} onclick={() => applyPreset('Reset')}>Reset</button>
			<button class="btn btn-outline btn-primary btn-sm" title="Save the live 55-parameter state as a named preset."
			onclick={doSave} disabled={saveBusy}>Save</button>
			{#each $presetList as name (name)}
				<button class="btn btn-outline btn-primary btn-sm '$audioState.activePreset === name ? 'btn-active' : ''}"
				disabled={$audioState.activePreset !== null} onclick={() => applyPreset(name)}>{name}</button>
				<button class="btn btn-outline btn-sm" title="Delete preset"
				onclick={() => doDelete(name)}>✕</button>
			{/each}
			{#if presetMsg}<span class="preset-status">{presetMsg}</span>{/if}
			</div>

		<div class="grid grid-cols-3 gap-x-8 gap-y-4 items-stretch">
			{#each [0, 1, 2] as col (col)}
				<div class="flex flex-col justify-between gap-2">
					{#each PARAM_GROUPS as g (g.title + JSON.stringify(g.items))}
						{#if g.col === col}
							{#if g.divider}
							<div class="border-t my-1" style="border-color: var(--btn-color, var(--color-primary))"></div>
							{/if}
							<div class="flex flex-col gap-2">
								{#if g.title}<div class="text-xs uppercase tracking-wide opacity-60 mt-1">{g.title}</div>{/if}
								{#each g.items as i (i)}
									{#if RADIO_IDX.has(i)}
										{@const group = IDX_TO_GROUP[i]}
										<div class="flex items-center gap-3 '{$audioState.morphing.has(i) ? 'param-row--morphing' : ''}">
											<span class="w-32 text-sm font-medium shrink-0 truncate">{EDIT_PARAM_NAMES[i]}</span>
											<div class="radio-row drop-shadow flex-1 min-w-0">
												{#each radioOpts(group) as label, idx (idx)}
													<input type="radio"
														class="radio-row-item btn btn-soft btn-xs flex-1 min-w-0"
														name={group}
														value={String(idx)}
														checked={$audioState.radios[group] === idx}
														aria-label={label}
														onchange={() => setRadio(group, idx)} />
												{/each}
											</div>
										</div>
									{:else}
										<div class="flex items-center gap-3 '{$audioState.morphing.has(i) ? 'param-row--morphing' : ''}">
											<span class="w-32 text-sm font-medium shrink-0 truncate">{EDIT_PARAM_NAMES[i]}</span>
											<input type="range" min="0" max="1023" step="1"
												class="range range-primary range-sm flex-1 min-w-0"
												bind:value={$audioState.params[i]}
												oninput={() => setParam(i, $audioState.params[i])} />
											<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{fmtSlider(i, $audioState.params[i])}</span>
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
	/* Preset status line (save/delete feedback). Buttons themselves are
	   flyonui btn btn-outline btn-primary btn-sm (1:1 legacy). */
	.preset-status { font-size: 0.7rem; opacity: 0.6; margin-left: 0.25rem; }

	/* Radio groups (WAVE/WAVE 2 A..F, LFO Shape Sin/S/Walk, FM Mod Wave
		 SIN/TRI/SAW/SQR/NOISE): FlyonUI "join radio with btn" pattern — the
		 same geometry as .octave-join (app.css), re-asserted locally so the
		 component stays self-contained (Svelte scoped styles can't reach
		 app.css custom properties from another module). */
	.radio-row { display: flex; gap: 0; flex: 1 1 0; min-width: 0; }
	/* flyonui 2.4.1 has no .btn-ghost utility. Unchecked items = .btn-soft (neutral
	   10% bg); the active radio is filled PRIMARY by the built-in
	   .btn input[type=radio]:checked rule (1:1 legacy look). Svelte keeps
	   the DOM :checked in sync with $audioState.radios (core radio:init/radio:ext
	   -> stores), so no per-item class toggle is needed. */
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
