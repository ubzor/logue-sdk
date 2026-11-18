<!-- OSC: parameters card (2/3 width). Port of the legacy OSC panel in
     websim/index.html.

     The OSC edit-parameter groups (PARAM_GROUPS in src/lib/audio/
     setup-web-audio.js) render into three equal columns; groups are assigned
     column-by-column (MAIN/BREATH/FORMANT -> col 0; WANDER/WANDER TGT ->
     col 1; CHORD/SUBS/FILTER -> col 2) so the heights balance. The preset
     bank (Random/Reset buttons, plus the user presets from stage 4) sits
     above the grid, in one row.

     Border-less, padding-less container (flat panel): saves the card's
     chrome (border + card-body padding) around the parameter grid.

     #Presets / #Parameters / #ParamCol0..2 are created here but FILLED by
     setup-web-audio.js (createEditableParam + the preset bank builder) -
     the ids must exist before the glue runs, which they do (static markup). -->
<div class="min-w-0 flex-[2]">
	<div class="flex flex-col items-stretch gap-3">
		<!-- ---- Preset bank (Random/Reset + saved presets) ---------------- -->
		<div class="flex flex-wrap gap-2" id="Presets"></div>
		<div id="Parameters" class="grid grid-cols-3 gap-x-8 gap-y-4 items-stretch">
			<div id="ParamCol0" class="flex flex-col justify-between gap-2"></div>
			<div id="ParamCol1" class="flex flex-col gap-2"></div>
			<div id="ParamCol2" class="flex flex-col justify-between gap-2"></div>
		</div>
	</div>
</div>
