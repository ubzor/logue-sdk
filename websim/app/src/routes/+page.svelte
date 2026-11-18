<script>
	import OscPanel from '$lib/components/OscPanel.svelte';
	import FxStack from '$lib/components/FxStack.svelte';
	// Re-export the audio wiring so the built page node carries the
	// setupWebAudioAndUI export for the emscripten glue (see scripts/
	// relink-glue.mjs). The page itself never calls it in JS - the glue does,
	// via EM_ASM.
	import { setupWebAudioAndUI } from '$lib/audio/index.js';
	export { setupWebAudioAndUI };

	// Drone chain simulator page (SvelteKit port of the legacy
	// websim/index.html shell). Layout: top row = title + all global controls
	// + keyboard + scope (one line); main row = OSC parameters (2/3) +
	// MOD/DEL/REV stack (1/3). Full width, no max-w (FHD target).
	//
	// The audio wiring (setupWebAudioAndUI, src/lib/audio/) is invoked by the
	// emscripten glue (index.js) once the wasm worklet module is ready. It
	// only talks to the DOM through the ids this template provides, so the
	// markup here mirrors the legacy shell 1:1 (ids, data-* attributes, radio
	// group names "Octave" / "HwFltType").
	//
	// qwerty-hancock (window.QwertyHancock) is a classic UMD script injected
	// by the root layout (app/static/qwerty-hancock.js) before this page
	// renders - setup-web-audio.js constructs the keyboard in #keyboard.
	//
	// The ThemeSwitch mirrors the effective theme on load (the theme module in
	// +layout.ts set document.documentElement.dataset.theme before first
	// paint); its change handler persists the choice and follows OS preference
	// until the user flips it (lib/theme.ts onThemeSwitchChange, exposed on
	// window by +layout.ts).
	// @ts-ignore - the theme bridge is a plain window property (set by
	// +layout.ts); no TS in this Svelte script block.
	function onThemeSwitch(e) {
		// @ts-ignore
		window.__onThemeSwitchChange?.(e);
	}
</script>

<div class="flex flex-col gap-2">

	<!-- ================= TOP ROW: controls + keyboard/scope ================ -->
	<div class="flex flex-row gap-2">
		<!-- Left column: title + all global controls + theme switch -->
		<div class="flex flex-col gap-2 grow">
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
					value="1"
					step="0.01"
					id="VolumeSlider"
					class="range range-primary range-sm flex-1"
				>
				<span
					id="VolumeLabel"
					class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-xs text-right"
				></span>
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
					value="120"
					step="1"
					id="BpmSlider"
					class="range range-primary range-sm flex-1"
				>
				<span
					id="BpmLabel"
					class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-xs text-right"
				></span>
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
					id="LfoFreq"
					class="range range-primary range-sm flex-1"
				>
				<span
					id="LfoFreqLabel"
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
					id="LfoDepth"
					class="range range-primary range-sm flex-1"
				>
				<span
					id="LfoDepthLabel"
					class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-xs text-right"
				>0%</span>
			</label>
		</div>

		<!-- Right column: octave + keyboard + scope -->
		<div class="flex flex-col gap-2 shrink-0">
			<!-- Keyboard + Octave block (shared 507px width) + Scope -->
			<div class="flex flex-row items-start gap-2" style="height:128px">
				<!-- Octave + Keyboard: stacked vertically, shared 507px width -->
				<div
					class="flex flex-col shrink-0"
					style="width:507px;height:128px;justify-content: space-between;"
				>
					<!-- Octave (FlyonUI "Join radio with btn style"): the .btn rule
							 in flyonui button.css paints input[type=radio] via
							 `&:after { content: attr(aria-label) }`, and
							 input[type=radio]:checked flips --btn-bg to the primary
							 color. .octave-join (app.css) provides the join geometry.
							 JS (setOctaveShift in setup-web-audio.js) only applies the
							 shift and mirrors the Z/X hotkeys into .checked. -->
					<div class="flex items-center gap-2">
						<span class="text-xs font-medium w-10 shrink-0">Octave</span>
						<div
							class="octave-join drop-shadow"
							title="Current octave shift of the whole keyboard range"
						>
							<input
								class="octave-join-item btn btn-soft btn-primary btn-xs"
								type="radio"
								name="Octave"
								value="1"
								aria-label="1"
								title="Down two octaves (A0..D2)"
							>
							<input
								class="octave-join-item btn btn-soft btn-primary btn-xs"
								type="radio"
								name="Octave"
								value="2"
								aria-label="2"
								title="Down one octave (A1..D3)"
								checked
							>
							<input
								class="octave-join-item btn btn-soft btn-primary btn-xs"
								type="radio"
								name="Octave"
								value="3"
								aria-label="3"
								title="Base range (A2..D4)"
							>
							<input
								class="octave-join-item btn btn-soft btn-primary btn-xs"
								type="radio"
								name="Octave"
								value="4"
								aria-label="4"
								title="Up one octave (A3..D5)"
							>
							<input
								class="octave-join-item btn btn-soft btn-primary btn-xs"
								type="radio"
								name="Octave"
								value="5"
								aria-label="5"
								title="Up two octaves (A4..D6)"
							>
						</div>
						<input
							type="checkbox"
							id="ThemeSwitch"
							class="switch switch-primary switch-xs ml-auto"
							title="Toggle dark theme (VSCode) / light theme (Valorant)"
							{onThemeSwitch}
						>
					</div>
					<!-- Keyboard (qwerty-hancock). Fixed 507px width, 96px height.
							 window.QwertyHancock is injected by the root layout before
							 the page renders (classic UMD script, app/static/). -->
					<div id="keyboard" class="shrink-0" style="width:507px"></div>
				</div>

				<!-- Scope: time-domain oscilloscope + FFT histogram (native buffers
						 512x128, CSS .scope-canvas fixes the render at 512px, see app.css).
						 Side by side in a row; the canvases carry their own
						 --scope-bg background, so no card padding is needed. -->
				<div class="flex flex-row gap-2 shrink-0">
					<canvas id="TimeDomain" class="scope-canvas" width="512" height="128"></canvas>
					<canvas id="FrequencyDomain" class="scope-canvas" width="512" height="128"></canvas>
				</div>
			</div>
		</div>
	</div>

	<!-- ================= MAIN ROW: OSC 2/3 + FX 1/3 ========================= -->
	<div class="flex gap-4 items-stretch">
		<OscPanel />
		<FxStack />
	</div>

</div>
