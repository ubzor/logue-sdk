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
		<div id="hwflt_body" class="flex flex-col gap-2 fx-params">
			<!-- Type: radio join (FlyonUI "Join radio with btn style", same as Octave) -->
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Type</span>
				<div class="octave-join drop-shadow" id="hwflt_type" title="Filter type (thru = out of the path)">
					<input class="octave-join-item btn btn-soft btn-primary btn-xs" type="radio" name="HwFltType" value="0" aria-label="Off" title="Filter off (dry wire)" checked>
					<input class="octave-join-item btn btn-soft btn-primary btn-xs" type="radio" name="HwFltType" value="1" aria-label="HP 4" title="High-pass, 24 dB/oct">
					<input class="octave-join-item btn btn-soft btn-primary btn-xs" type="radio" name="HwFltType" value="2" aria-label="HP 2" title="High-pass, 12 dB/oct">
					<input class="octave-join-item btn btn-soft btn-primary btn-xs" type="radio" name="HwFltType" value="3" aria-label="BP 4" title="Band-pass, 24 dB/oct">
					<input class="octave-join-item btn btn-soft btn-primary btn-xs" type="radio" name="HwFltType" value="4" aria-label="BP 2" title="Band-pass, 12 dB/oct">
					<input class="octave-join-item btn btn-soft btn-primary btn-xs" type="radio" name="HwFltType" value="5" aria-label="LP 4" title="Low-pass, 24 dB/oct">
					<input class="octave-join-item btn btn-soft btn-primary btn-xs" type="radio" name="HwFltType" value="6" aria-label="LP 2" title="Low-pass, 12 dB/oct">
				</div>
			</div>
			<!-- Cutoff: raw 0..1023 (log 1 Hz .. 20 kHz), default 0 -->
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Cutoff</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="hwflt_cutoff" class="range range-primary range-sm flex-1 min-w-0">
				<span id="hwflt_cutoff_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right"></span>
			</div>
			<!-- Resonance: raw 0..1023 (linear Q 0.5 .. 2.5), default 0 -->
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Resonance</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="hwflt_res" class="range range-primary range-sm flex-1 min-w-0">
				<span id="hwflt_res_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right"></span>
			</div>
			<!-- Sweep F: 0..30 Hz (step 0.01), default 0 = sweep off -->
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Sweep F</span>
				<input type="range" min="0" max="30" value="0" step="0.01" id="hwflt_sweepf" class="range range-primary range-sm flex-1 min-w-0">
				<span id="hwflt_sweepf_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right"></span>
			</div>
			<!-- Sweep Amt: -100..+100 (UI), default 0. Raw = (amt+100)*5.115. -->
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Sweep Amt</span>
				<input type="range" min="-100" max="100" value="0" step="1" id="hwflt_sweepamt" class="range range-primary range-sm flex-1 min-w-0">
				<span id="hwflt_sweepamt_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right"></span>
			</div>
		</div><!-- /hwflt_body -->
	</div>

	<!-- ---- MOD block (TIME / DEPTH + two multiplexer groups) ---------------- -->
	<div class="w-full flex flex-col gap-3">
		<div class="flex items-center justify-between">
			<div class="text-xs uppercase tracking-wide opacity-60">Mod</div>
			<label class="flex items-center gap-1.5 cursor-pointer select-none">
				<input type="checkbox" id="fxmod_bypass" class="switch switch-primary switch-xs" checked>
			</label>
		</div>
		<div id="fxmod_body" class="flex flex-col gap-2 fx-params">
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Time</span>
				<input type="range" min="0" max="1023" value="512" step="1" id="fxmod_time" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_time_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">25%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Depth</span>
				<input type="range" min="0" max="1023" value="511" step="1" id="fxmod_depth" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_depth_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">50%</span>
			</div>
			<!-- ===== MODULATION group (first PARAM/VALUE multiplexer, slots 2/3) -->
			<!-- idx 0..4 = DRIFT / CMODE / CHOR / FLNGR / ENSMBL, driven through
				 Module.modSetEditParam(idx, raw) (atomic PARAM_SEL+PARAM_VAL). -->
			<div class="text-xs uppercase tracking-wide opacity-60 mt-1">Modulation</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Drift</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxmod_drift" data-modedit="0" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_drift_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Chorus Mode</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxmod_cmode" data-modedit="1" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_cmode_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">I</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Chorus</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxmod_chorus" data-modedit="2" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_chorus_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Flanger</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxmod_flanger" data-modedit="3" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_flanger_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Ensemble</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxmod_ensemble" data-modedit="4" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_ensemble_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>

			<!-- ===== TAPE group (second PARAM/VALUE multiplexer, slots 4/5) -->
			<!-- idx 0..4 = NOISE / NRATE / TAPWOB / HISS / WARP (KoS-derived), driven
				 through Module.modSetTapeParam(idx, raw) (atomic TPARAM+TVALUE). -->
			<div class="text-xs uppercase tracking-wide opacity-60 mt-1">Tape</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Noise</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxmod_noise" data-modtape="0" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_noise_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">N Rate</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxmod_nrate" data-modtape="1" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_nrate_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Tape Wob</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxmod_tapwob" data-modtape="2" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_tapwob_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Hiss</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxmod_hiss" data-modtape="3" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_hiss_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-32 text-sm font-medium shrink-0">Warp</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxmod_warp" data-modtape="4" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxmod_warp_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
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
					<input type="checkbox" id="fxdel_bypass" class="switch switch-primary switch-xs" checked>
				</label>
			</div>
			<div id="fxdel_body" class="flex flex-col gap-2 fx-params">
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Time</span>
				<input type="range" min="0" max="1023" value="0" step="1" id="fxdel_time" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxdel_time_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Depth</span>
				<input type="range" min="0" max="1023" value="512" step="1" id="fxdel_depth" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxdel_depth_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">50%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Mix</span>
				<input type="range" min="-1000" max="1000" value="-1000" step="1" id="fxdel_mix" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxdel_mix_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>
			</div><!-- /fxdel_body -->
		</div>

		<!-- ---- REV block (right half) -------------------------------------- -->
		<div class="w-full min-w-0 flex flex-col gap-3">
			<div class="flex items-center justify-between">
				<div class="text-xs uppercase tracking-wide opacity-60">Rev</div>
				<label class="flex items-center gap-1.5 cursor-pointer select-none">
					<input type="checkbox" id="fxrev_bypass" class="switch switch-primary switch-xs" checked>
				</label>
			</div>
			<div id="fxrev_body" class="flex flex-col gap-2 fx-params">
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Time</span>
				<input type="range" min="0" max="1023" value="512" step="1" id="fxrev_time" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxrev_time_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">50%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Depth</span>
				<input type="range" min="0" max="1023" value="512" step="1" id="fxrev_depth" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxrev_depth_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">50%</span>
			</div>
			<div class="flex items-center gap-3">
				<span class="w-16 text-sm font-medium shrink-0">Mix</span>
				<input type="range" min="-1000" max="1000" value="-1000" step="1" id="fxrev_mix" class="range range-primary range-sm flex-1 min-w-0">
				<span id="fxrev_mix_val" class="badge badge-soft badge-primary badge-sm font-mono shrink-0 min-w-14 text-right">0%</span>
			</div>
			</div><!-- /fxrev_body -->
		</div>

	</div><!-- /DEL+REV row -->

</div><!-- /FX stack -->
