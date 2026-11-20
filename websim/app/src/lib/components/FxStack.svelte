<!-- FX: MOD / DEL / REV stack (1/3 width). Port of the legacy FX stack in
     websim/index.html.

     The audio path OSC -> MOD -> DEL -> REV runs inside the wasm worklet.
     MOD uses TWO PARAM/VALUE multiplexers (like the OSC, but doubled):
     TIME / DEPTH are fixed knobs (setFxParam), the 5 MOD-group edit params
     (DRIFT / CMODE / CHOR / FLNGR / ENSMBL) are driven through
     Module.modSetEditParam(idx, raw), and the 5 TAPE-group edit params
     (NOISE / NRATE / TAPWOB / HISS / WARP, KoS-derived) through
     Module.modSetTapeParam(idx, raw). DEL / REV keep their own TIME /
     DEPTH via setFxParam. The four flat blocks (Filter / MOD / DEL / REV)
     stack vertically in the right third of the page - NO .card chrome
     (like the OSC panel), which saves border + 24px padding per block.
     .fx-params restores the full --color-base-content so the FX labels
     match the MAIN labels exactly. -->
<script>
	import {
		audioState,
		setHwfltType, setHwfltParam,
		setFxmodTime, setFxmodDepth, setFxmodEdit, setFxmodTape,
		setFxdel, setFxrev,
		setStageBypass,
	} from '$lib/audio/stores';
</script>
<div class="flex flex-col gap-4 min-w-0 flex-[1]">

	<!-- ---- Filter block (web-sim only, emulates the NTS-1 physical filter
		 between OSC and MOD) ---------------------------------------------- -->
	<!-- The block is controlled by its TYPE: Off = out of the path (dry wire),
		 any other type = in the path. CUTOFF / RES / SWEEP F / SWEEP AMT are
		 driven through Module.setHwFilterParam (wasm.cc -> Chain::hwflt_).
		 The block sits in the right column ABOVE MOD (the filter is between
		 OSC and MOD in the audio path). Like the OSC parameter panel (left of
		 the FX stack), it is a FLAT block: no .card chrome (border + 24px
		 card-body padding) - that saves a lot of vertical/horizontal space.
		 Layout: each row is a flex container (NOT a <label> wrapping the
		 input), mirroring createEditableParam in setup-web-audio.js.
		 Type: FlyonUI "Join radio with btn style" (same pattern as Octave). -->
	<div class="w-full flex flex-col gap-3">
		<div class="flex items-center justify-between">
			<div class="text-xs uppercase tracking-wide opacity-60">Filter</div>
		</div>
		<div class="flex flex-col gap-2 fx-params">
			<!-- Type: radio join (FlyonUI "Join radio with btn style", same as Octave) -->
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Type</span>
				<div class="octave-join drop-shadow" title="Filter type (thru = out of the path)">
					{#each ['Off','HP 4','HP 2','BP 4','BP 2','LP 4','LP 2'] as label, idx (idx)}
						<button type="button"
							class="octave-join-item btn btn-soft btn-xs {$audioState.radios.HwFltType === idx ? 'btn-primary' : 'btn-ghost'}"
							onclick={() => setHwfltType(idx)}>
							{label}
						</button>
					{/each}
				</div>
			</div>
			<!-- Cutoff: raw 0..1023 (log 1 Hz .. 20 kHz), default 0 -->
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Cutoff</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.hwflt.cutoff} oninput={() => setHwfltParam(0, $audioState.hwflt.cutoff)}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.hwflt.cutoff / 1023) * 100)}%</span>
			</div>
			<!-- Resonance: raw 0..1023 (linear Q 0.5 .. 2.5), default 0 -->
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Resonance</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.hwflt.res} oninput={() => setHwfltParam(1, $audioState.hwflt.res)}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.hwflt.res / 1023) * 100)}%</span>
			</div>
			<!-- Sweep F: 0..30 Hz (step 0.01), default 0 = sweep off -->
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Sweep F</span>
				<input type="range" min="0" max="30" step="0.01" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.hwflt.sweepF} oninput={() => setHwfltParam(2, Math.round($audioState.hwflt.sweepF * 1000))}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{$audioState.hwflt.sweepF.toFixed(2)} Hz</span>
			</div>
			<!-- Sweep Amt: -100..+100 (UI), default 0. Raw = (amt+100)*5.115. -->
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Sweep Amt</span>
				<input type="range" min="-100" max="100" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.hwflt.sweepAmt} oninput={() => setHwfltParam(3, Math.round(($audioState.hwflt.sweepAmt + 100) * 5.115))}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{($audioState.hwflt.sweepAmt > 0 ? '+' : '') + $audioState.hwflt.sweepAmt + '%'}</span>
			</div>
		</div><!-- /hwflt_body -->
	</div>

	<!-- ---- MOD block (TIME / DEPTH + two multiplexer groups) ---------------- -->
	<div class="w-full flex flex-col gap-3">
		<div class="flex items-center justify-between">
			<div class="text-xs uppercase tracking-wide opacity-60">Mod</div>
			<label class="flex items-center gap-1.5 cursor-pointer select-none">
				<input type="checkbox" class="switch switch-primary switch-xs" checked={$audioState.bypass[0] === 0} onchange={(e) => setStageBypass(0, e.currentTarget.checked ? 0 : 1)}>
			</label>
		</div>
		<div class="flex flex-col gap-2 fx-params">
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Time</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.fxmod.time} oninput={() => setFxmodTime($audioState.fxmod.time)}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.time / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Depth</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.fxmod.depth} oninput={() => setFxmodDepth($audioState.fxmod.depth)}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.depth / 1023) * 100)}%</span>
			</div>
			<!-- ===== MODULATION group (first PARAM/VALUE multiplexer, slots 2/3) -->
			<!-- idx 0..4 = DRIFT / CMODE / CHOR / FLNGR / ENSMBL, driven through
				 Module.modSetEditParam(idx, raw) (atomic PARAM_SEL+PARAM_VAL). -->
			<div class="text-xs uppercase tracking-wide opacity-60 mt-1">Modulation</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Drift</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" data-i="0" data-kind="edit" bind:value={$audioState.fxmod.edit[0]} oninput={() => setFxmodEdit(0, $audioState.fxmod.edit[0])}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.edit[0] / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Chorus Mode</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" data-i="1" data-kind="edit" bind:value={$audioState.fxmod.edit[1]} oninput={() => setFxmodEdit(1, $audioState.fxmod.edit[1])}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.edit[1] / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Chorus</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" data-i="2" data-kind="edit" bind:value={$audioState.fxmod.edit[2]} oninput={() => setFxmodEdit(2, $audioState.fxmod.edit[2])}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.edit[2] / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Flanger</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" data-i="3" data-kind="edit" bind:value={$audioState.fxmod.edit[3]} oninput={() => setFxmodEdit(3, $audioState.fxmod.edit[3])}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.edit[3] / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Ensemble</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" data-i="4" data-kind="edit" bind:value={$audioState.fxmod.edit[4]} oninput={() => setFxmodEdit(4, $audioState.fxmod.edit[4])}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.edit[4] / 1023) * 100)}%</span>
			</div>

			<!-- ===== TAPE group (second PARAM/VALUE multiplexer, slots 4/5) -->
			<!-- idx 0..4 = NOISE / NRATE / TAPWOB / HISS / WARP (KoS-derived), driven
				 through Module.modSetTapeParam(idx, raw) (atomic TPARAM+TVALUE). -->
			<div class="text-xs uppercase tracking-wide opacity-60 mt-1">Tape</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Noise</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" data-i="0" data-kind="tape" bind:value={$audioState.fxmod.tape[0]} oninput={() => setFxmodTape(0, $audioState.fxmod.tape[0])}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.tape[0] / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">N Rate</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" data-i="1" data-kind="tape" bind:value={$audioState.fxmod.tape[1]} oninput={() => setFxmodTape(1, $audioState.fxmod.tape[1])}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.tape[1] / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Tape Wob</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" data-i="2" data-kind="tape" bind:value={$audioState.fxmod.tape[2]} oninput={() => setFxmodTape(2, $audioState.fxmod.tape[2])}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.tape[2] / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Hiss</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" data-i="3" data-kind="tape" bind:value={$audioState.fxmod.tape[3]} oninput={() => setFxmodTape(3, $audioState.fxmod.tape[3])}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.tape[3] / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Warp</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" data-i="4" data-kind="tape" bind:value={$audioState.fxmod.tape[4]} oninput={() => setFxmodTape(4, $audioState.fxmod.tape[4])}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxmod.tape[4] / 1023) * 100)}%</span>
			</div>
		</div><!-- /fxmod_body -->
	</div>

	<!-- ---- DEL + REV row (one line: DEL left, REV right) ------------------ -->
	<!-- The audio path runs OSC -> MOD -> DEL -> REV, so DEL sits LEFT of
		 REV (matches the signal direction). Each block is w-full inside the
		 row -> two equal halves. Time/Depth/Mix labels are w-16 (64px)
		 instead of w-32 (128px) - the names are short enough, and the
		 freed space goes to the sliders. -->
	<div class="w-full flex flex-row gap-4">

		<!-- ---- DEL block (left half) -------------------------------------- -->
		<div class="w-full min-w-0 flex flex-col gap-3">
			<div class="flex items-center justify-between">
				<div class="text-xs uppercase tracking-wide opacity-60">Del</div>
				<label class="flex items-center gap-1.5 cursor-pointer select-none">
					<input type="checkbox" class="switch switch-primary switch-xs" checked={$audioState.bypass[1] === 0} onchange={(e) => setStageBypass(1, e.currentTarget.checked ? 0 : 1)}>
				</label>
			</div>
			<div class="flex flex-col gap-2 fx-params">
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Time</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.fxdel.time} oninput={() => setFxdel(0, $audioState.fxdel.time)}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxdel.time / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Depth</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.fxdel.depth} oninput={() => setFxdel(1, $audioState.fxdel.depth)}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxdel.depth / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Mix</span>
				<input type="range" min="-1000" max="1000" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.fxdel.mix} oninput={() => setFxdel(2, $audioState.fxdel.mix)}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxdel.mix + 1000) / 20)}%</span>
			</div>
			</div><!-- /fxdel_body -->
		</div>

		<!-- ---- REV block (right half) -------------------------------------- -->
		<div class="w-full min-w-0 flex flex-col gap-3">
			<div class="flex items-center justify-between">
				<div class="text-xs uppercase tracking-wide opacity-60">Rev</div>
				<label class="flex items-center gap-1.5 cursor-pointer select-none">
					<input type="checkbox" class="switch switch-primary switch-xs" checked={$audioState.bypass[2] === 0} onchange={(e) => setStageBypass(2, e.currentTarget.checked ? 0 : 1)}>
				</label>
			</div>
			<div class="flex flex-col gap-2 fx-params">
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Time</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.fxrev.time} oninput={() => setFxrev(0, $audioState.fxrev.time)}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxrev.time / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Depth</span>
				<input type="range" min="0" max="1023" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.fxrev.depth} oninput={() => setFxrev(1, $audioState.fxrev.depth)}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxrev.depth / 1023) * 100)}%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Mix</span>
				<input type="range" min="-1000" max="1000" step="1" class="range range-primary range-sm flex-1 min-w-0" bind:value={$audioState.fxrev.mix} oninput={() => setFxrev(2, $audioState.fxrev.mix)}>
				<span class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">{Math.round(($audioState.fxrev.mix + 1000) / 20)}%</span>
			</div>
			</div><!-- /fxrev_body -->
		</div>

	</div><!-- /DEL+REV row -->

</div><!-- /FX stack -->
