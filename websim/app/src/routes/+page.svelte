<script>
	import OscPanel from '$lib/components/OscPanel.svelte';
	import FxStack from '$lib/components/FxStack.svelte';
	import AudioHost from '$lib/components/AudioHost.svelte';
	import Keyboard from '$lib/components/Keyboard.svelte';
	import WaveTableView3D from '$lib/components/WaveTableView3D.svelte';
	import SourceEdit from '$lib/components/SourceEdit.svelte';
	import { onMount } from 'svelte';
	import { audioState, audioController, setVolume, setBpm, setOctave } from '$lib/audio/stores';
	import { editor, onDocChange } from '$lib/editor/store.svelte';
	import { renderWave, EDITOR_FRAMES, EDITOR_POINTS } from '$lib/wavetable/render';

	// ---- Wave Table Editor (inlined into the main page, bottom) ------------
	// Clicking a line in the 3D view makes it ACTIVE; editing that wave in
	// SourceEdit re-renders ONLY that line. Every mutation bumps
	// editor.revision, which the $effect below turns into a debounced
	// setWteDocument call into the WASM OSC (the 16x256 bank re-renders at
	// K-rate inside blockPrepass; the audio path is untouched).
	let wteActive = $state(null);
	let wteTable = $derived(editor.waves.map((w, i) => renderWave(w, EDITOR_POINTS, i)));

	$effect(() => {
		// react to any editor mutation (revision increments on every change)
		void editor.revision;
		onDocChange();
	});
	// Drone chain simulator page (SvelteKit port of the legacy
	// websim/index.html shell). Layout: top row = title + all global controls
	// + keyboard + scope (one line); main row = OSC parameters (2/3) +
	// MOD/DEL/REV stack (1/3). Full width, no max-w (FHD target).
	//
	// Вариант B: AudioHost дожидается window.__droneAudioReady (glue из
	// drone-chain/wasm.cc) и вызывает initAudio() из core.ts. Всё UI ходит
	// через stores/bus. Keyboard — нативный Svelte-порт qwerty-hancock:
	// ноты (noteOn/noteOff) + Z/X octave — единственный путь, играющий в DSP.
	//
	// Global controls (Volume/BPM/Octave) — привязаны к stores (bind:value +
	// oninput/onchange -> store-хелперы). Legacy id=-wiring (setup-web-audio.js)
	// более нет: core подписан на volume:set / bpm:set / octave:set.
	//
	// The ThemeSwitch mirrors the effective theme on load (the theme module in
	// +layout.ts set document.documentElement.dataset.theme before first
	// paint); its change handler persists the choice and follows OS preference
	// until the user flips it (lib/theme.ts onThemeSwitchChange, exposed on
	// window by +layout.ts).
	// @ts-ignore - the theme bridge is a plain window property (set by
	// +layout.ts); no TS in this Svelte script block.
	//
	// ВАЖНО: attribute — `onchange={onThemeSwitch}` (Svelte 5 event-handler
	// syntax, как у octave-radio ниже). НЕ `{onThemeSwitch}`: bare function
	// в `{...}`-атрибути Svelte 5 интерпретирует как listener на событие с
	// именем `ThemeSwitch` ($.event('ThemeSwitch', ...)) — browser такого
	// события не генерирует, handler молча не срабатывает, тема не
	// переключается (баг, исправлен 2026-11-19).
	function onThemeSwitch(e) {
		// @ts-ignore
		window.__onThemeSwitchChange?.(e);
	}
	// Octave radio: value = button 1..5 -> shift = value - 3 (-2..+2).
	function onOctaveRadio(e) {
		setOctave(Number(e.currentTarget.value) - 3, 'radio');
	}

	// ---- Main view tabs ------------------------------------------------------
	// FlyonUI tabs (flyonui/components/tab.css, staged via vite.flyonui.js):
	// "Main" = OSC + FX stack, "Wavetable" = the wave table editor (3D view +
	// source/combine/warp panel). State is plain Svelte ($state) — the flyonui
	// JS tab plugin is NOT used; we just toggle .tab-active on the button and
	// .hidden on the other panel (the plugin only adds ARIA/active-tab: sugar,
	// which we set statically below). SSR-safe default: "main".
	let tab = $state('main');

	// Scope: два canvas-осциллографа (time-domain + FFT). Рефы через bind:this;
	// подписываемся на $audioController — когда AudioHost опубликует
	// контроллер (после initAudio), подключаем canvas'ы через
	// controller.attachScope(wave, fft). core.ts сам запускает
	// requestAnimationFrame-цикл drawScope и останавливает его при stop().
	let canvasWave;
	let canvasFFT;
	onMount(() => {
		const unsub = audioController.subscribe((c) => {
			if (c && canvasWave && canvasFFT) {
				c.attachScope(canvasWave, canvasFFT);
			}
		});
		// Синхронизировать switch с effective theme (initTheme в +layout.ts
		// уже применил data-theme до first paint; switch в разметке не имеет
		// начального checked — без этого он всегда показывал OFF, даже при
		// тёмной ОС-теме).
		const t = document.documentElement.dataset.theme;
		const sw = document.querySelector('#ThemeSwitch');
		if (sw) sw.checked = (t === 'luxury');
		return unsub;
	});
</script>

<div class="flex flex-col gap-2">

	<!-- Audio: glue -> core.ts (Вариант B). Монтируется первым: дожидается
	     window.__droneAudioReady, инициализирует core, раздаёт состояние
	     по stores. Всё остальное UI ниже — через stores/bus. -->
	<AudioHost />

	<!-- ================= TOP ROW: controls + keyboard/scope ================ -->
	<div class="flex flex-row gap-2 top-row">
		<!-- Left column: title + all global controls + theme switch -->
		<div class="flex flex-col gap-2 shrink-0 w-72">
			<div class="flex items-center gap-2">
				<span
					id="DroneIcon"
					class="icon-[tabler--drone] text-base"
					role="img"
					aria-label="Drone"
				></span>
				<span class="text-sm font-semibold tracking-wide">DRONE</span>
			</div>

			<label class="flex items-center gap-3" title="Master output volume">
				<span class="w-20 text-xs font-medium shrink-0">Volume</span>
				<input
					type="range"
					min="0"
					max="1"
					bind:value={$audioState.volume}
					step="0.01"
					class="range range-primary range-sm flex-1 min-w-0"
					oninput={() => setVolume($audioState.volume)}
				>
				<span
					class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-xs text-right"
				>{Math.round($audioState.volume * 100)}%</span>
			</label>

			<!-- BPM: simulator-only tempo control. Delivered to the DSP
					 through Module.fx_set_bpm (wasm.cc -> Osc::setTempo ->
					 tempo_bpm_). On hardware the platform delivers BPM itself
					 via unit_set_tempo(); the slider here only matters in the
					 web simulator. -->
			<label
					class="flex items-center gap-3"
					title="Simulator tempo (delivered to DSP via fx_set_bpm)"
				>
				<span class="w-20 text-xs font-medium shrink-0">BPM</span>
				<input
					type="range"
					min="56"
					max="240"
					bind:value={$audioState.bpm}
					step="1"
					class="range range-primary range-sm flex-1 min-w-0"
					oninput={() => setBpm($audioState.bpm)}
				>
				<span
					class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-xs text-right"
				>{$audioState.bpm}</span>
			</label>

			<!-- LFO F / LFO S: VESTIGIAL controls. The platform's OSC-button
					 RATE/SHAPE knobs (the shape-LFO) are NO-OPs now - the wavetable
					 intra-bank morph is driven by the INTERNAL wave-LFO (the
					 "Wave LFO Rate" / "Wave LFO Depth" edit parameters in the MAIN
					 group). These sliders send nothing to the DSP (the old
					 setLfoFreq / setLfoDepth bindings were removed from wasm.cc). -->
			<label
					class="flex items-center gap-3"
					title="Vestigial - no-op. The LFO is now the internal Wave LFO (see MAIN group)."
				>
				<span class="w-20 text-xs font-medium shrink-0">LFO F (Hz)</span>
				<input
					type="range"
					min="0"
					max="30"
					value="0"
					step="0.01"
					class="range range-primary range-sm flex-1 min-w-0"
				>
				<span
					class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-xs text-right"
				>0.00</span>
			</label>

			<label
					class="flex items-center gap-3"
					title="Vestigial - no-op. The LFO is now the internal Wave LFO (see MAIN group)."
				>
				<span class="w-20 text-xs font-medium shrink-0">LFO S (%)</span>
				<input
					type="range"
					min="0"
					max="100"
					value="0"
					step="1"
					class="range range-primary range-sm flex-1 min-w-0"
				>
				<span
					class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-xs text-right"
				>0%</span>
			</label>
		</div>

		<!-- Right column: octave + scope -->
		<div class="flex flex-col gap-2 grow min-w-0">
			<!-- Octave block (507px width) + Scope -->
			<div class="flex flex-row items-start gap-2" style="height:128px">
				<!-- Octave: stacked vertically, shared 507px width -->
				<div
					class="flex flex-col shrink-0"
					style="width:507px;height:128px;justify-content: space-between;"
				>
					<!-- Octave (FlyonUI "Join radio with btn style"): the .btn rule
							 in flyonui button.css paints input[type=radio] via
							 `&:after { content: attr(aria-label) }`, and
							 input[type=radio]:checked flips --btn-bg to the primary
							 color. .octave-join (app.css) provides the join geometry.
							 JS (setOctaveShift in core.ts) applies the shift and mirrors the
							 Z/X hotkeys into the radio group state. -->
					<div class="flex items-center gap-2">
						<span class="text-xs font-medium w-10 shrink-0">Octave</span>
						<div
							class="octave-join drop-shadow"
							title="Current octave shift of the whole keyboard range"
						>
							{#each [1, 2, 3, 4, 5] as btn (btn)}
								<input
									class="octave-join-item btn btn-soft btn-primary btn-xs"
									type="radio"
									name="Octave"
									value={btn}
									checked={$audioState.octave === btn - 3}
									aria-label={String(btn)}
									onchange={onOctaveRadio}
								>
							{/each}
						</div>
						<input
							type="checkbox"
							id="ThemeSwitch"
							class="switch switch-primary switch-xs ml-auto"
							title="Toggle dark theme (VSCode) / light theme (Valorant)"
							onchange={onThemeSwitch}
						>
					</div>
					<Keyboard />
				</div>

				<!-- Scope: time-domain oscilloscope + FFT histogram (native buffers
						 512x128, CSS .scope-canvas fixes the render at 512px, see app.css).
						 Side by side in a row; the canvases carry their own
						 --scope-bg background, so no card padding is needed. -->
				<div class="flex flex-row gap-2 scope-row">
					<canvas bind:this={canvasWave} class="scope-canvas" width="512" height="128"></canvas>
					<canvas bind:this={canvasFFT} class="scope-canvas" width="512" height="128"></canvas>
				</div>
			</div>
		</div>
	</div>

	<!-- ================= VIEW TABS: Main / Wavetable ========================== -->
	<!-- FlyonUI "Tabs & Pills" (docs/navigations/tabs-pills): .tabs-bordered
	     container + .tab buttons; the active one carries .tab-active (the
	     primary underline). State is the `tab` rune below; content panels are
	     toggled with .hidden (flyonui's JS tab plugin is not loaded). -->
	<nav
		class="tabs tabs-bordered"
		role="tablist"
		aria-label="Simulator views"
		aria-orientation="horizontal"
	>
		<button
			type="button"
			role="tab"
			id="tab-main"
			aria-controls="tabpanel-main"
			aria-selected={tab === 'main'}
			class="tab {tab === 'main' ? 'tab-active' : ''}"
			onclick={() => (tab = 'main')}
		>
			Main
		</button>
		<button
			type="button"
			role="tab"
			id="tab-wavetable"
			aria-controls="tabpanel-wavetable"
			aria-selected={tab === 'wavetable'}
			class="tab {tab === 'wavetable' ? 'tab-active' : ''}"
			onclick={() => (tab = 'wavetable')}
		>
			Wavetable
		</button>
	</nav>

	<!-- ================= MAIN ROW: OSC 2/3 + FX 1/3 ========================= -->
	<div
		id="tabpanel-main"
		role="tabpanel"
		aria-labelledby="tab-main"
		class={tab === 'main' ? '' : 'hidden'}
	>
		<div class="flex gap-4 items-stretch">
			<OscPanel />
			<FxStack />
		</div>
	</div>

	<!-- ================= WAVE TABLE EDITOR (bottom) ========================== -->
	<!-- The /editor page inlined: 3D preview of the 16-frame wavetable (each
	     frame = one editor wave, live from the store) + the source/combine/
	     warp editing panel for the ACTIVE frame. Every change re-serializes
	     the document into the WASM OSC (debounced), where the 16x256 bank
	     re-renders at K-rate; PMORPH/PLEV (OSC panel, "PROC WAVET" group)
	     select the frame and the level of the editor voice. Lives in the
	     "Wavetable" tab (see the tab strip above). -->
	<div
		id="tabpanel-wavetable"
		role="tabpanel"
		aria-labelledby="tab-wavetable"
		class={tab === 'wavetable' ? '' : 'hidden'}
	>
		<div class="flex flex-col gap-3">
		<div class="flex items-center gap-3">
			<span class="text-sm font-semibold tracking-wide">Wave Table Editor</span>
			<span class="badge badge-soft badge-primary badge-sm font-mono text-xs">
				{EDITOR_FRAMES} × {EDITOR_POINTS}
			</span>
			<span class="text-xs opacity-60">
				{#if wteActive === null}
					Click a line to make it active
				{:else}
					Frame <span class="font-mono">{wteActive + 1}</span> / {EDITOR_FRAMES} active
				{/if}
			</span>
		</div>

		<div class="card p-2 w-fit wte-preview">
			<div class="wte-preview-inner">
			{#if wteTable}
				<WaveTableView3D data={wteTable} bind:active={wteActive} onActiveChange={(i) => wteActive = i} />
			{:else}
				<div class="grid place-items-center text-xs opacity-60" style="width:640px;height:480px">Loading…</div>
			{/if}
			<!-- Vertical frame selector: right of the canvas, full canvas height.
				     Synced with the active frame (click a line or drag this). -->
			<div class="wte-morph">
				<input
					type="range"
					min="1"
					max="16"
					step="1"
					value={wteActive !== null ? wteActive + 1 : 1}
					oninput={(e) => wteActive = parseInt(e.currentTarget.value, 10) - 1}
					class="range range-primary wte-morph-slider"
					title="Frame"
				/>
				<span class="text-[10px] font-mono opacity-60">{wteActive !== null ? wteActive + 1 : '—'}</span>
			</div>
			</div>
		</div>

		<style>
			.wte-preview-inner {
				display: flex;
				gap: 0.5rem;
				align-items: stretch;
			}
			.wte-morph {
				display: flex;
				flex-direction: column;
				align-items: center;
				gap: 0.375rem;
				height: 480px;
				width: 32px;
				flex-shrink: 0;
			}
			.wte-morph-slider {
				writing-mode: vertical-lr;
				direction: rtl;
				flex: 1;
				width: 20px;
				cursor: ns-resize;
			}
		</style>

		<!-- Source/combine/warp editing panel (disabled until a wave is active) -->
		<SourceEdit active={wteActive} />
		</div>
	</div>

</div>
