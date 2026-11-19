<!-- Keyboard: нативный Svelte-порт qwerty-hancock (Вариант B).

     Управляет DRONE-осциллятором: ноты (noteOn/noteOff) — через store-хелперы
     (bus -> core.ts -> Module.noteOn/noteOff + setOscPitch). Это единственный
     путь, который заставляет DSP играть — без него симулятор молчит.

     Поведение 1:1 legacy (setup-web-audio.js + qwerty-hancock.js):
       * keyMap: 18 клавиш A2..D4 (11 белых A S D F G H J K L ; ' + 7 чёрных
         W R T U I O [). Чёрные = имя с '#'.
       * Octave shift -2..+2 (старт -1 => A1..D3) применяется к каждой ноте:
         note+shift, MIDI и freq пересчитываются (relabel, 1:1 setKeyOctave).
       * Z/X = hotkey-shift (source 'hotkey'); Octave radio (+page) = 'radio'.
       * noteOn: midi = getKeyNumber(note)+20; freq = 440*2^((key-49)/12).
         setOscPitch(freq) — no-op в polyphonic DSP, но legacy так делал. -->
<script>
	import { onMount, onDestroy } from 'svelte';
	import { audioState, noteOn, noteOff, setOctave } from '$lib/audio/stores';

	// Physical-key -> base note (legacy keyMap), order = musical A2..D4.
	const KEYMAP = {
		65: 'A2', 87: 'A#2', 83: 'B2', 68: 'C3', 82: 'C#3',
		70: 'D3', 84: 'D#3', 71: 'E3', 72: 'F3', 85: 'F#3',
		74: 'G3', 73: 'G#3', 75: 'A3', 79: 'A#3', 76: 'B3',
		59: 'C4', 219: 'C#4', 222: 'D4',
	};
	// e.code (layout-independent) -> base note.
	const CODE_TO_NOTE = {
		KeyA: 'A2', KeyW: 'A#2', KeyS: 'B2', KeyD: 'C3', KeyR: 'C#3',
		KeyF: 'D3', KeyT: 'D#3', KeyG: 'E3', KeyH: 'F3', KeyU: 'F#3',
		KeyJ: 'G3', KeyI: 'G#3', KeyK: 'A3', KeyO: 'A#3', KeyL: 'B3',
		Semicolon: 'C4', Minus: 'C#4', Quote: 'D4',
	};

	// Legacy getKeyNumber: 'A'..'G#' -> 1..12 within an octave (A first),
	// then + (octave-1)*12 with the A/B/C pre-C offset. A2 = 27, A4 = 49.
	const NOTE_ORDER = ['A', 'A#', 'B', 'C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#'];
	function keyNumber(note) {
		const octave = Number(note.slice(-1));
		const name = note.slice(0, -1);
		let n = NOTE_ORDER.indexOf(name);
		n += n < 3 ? 12 + (octave - 1) * 12 + 1 : (octave - 1) * 12 + 1;
		return n;
	}
	const midiOf = (note) => keyNumber(note) + 20;
	const freqOf = (note) => 440 * Math.pow(2, (keyNumber(note) - 49) / 12);
	const shiftNote = (base, shift) => base.slice(0, -1) + (Number(base.slice(-1)) + shift);

	// Musical-order layout (ascending MIDI). whiteIndex = column among whites
	// (blacks sit on the boundary after the previous white).
	let whitePos = 0;
	const KEYS = Object.values(KEYMAP)
		.map((note) => ({ note, black: note.includes('#'), midi: midiOf(note), white: 0 }))
		.sort((a, b) => a.midi - b.midi)
		.map((k) => { k.white = k.black ? whitePos - 1 : whitePos++; return k; });
	const WHITE_COUNT = KEYS.filter((k) => !k.black).length; // 11

	// Octave shift (reactive; -2..+2). Driven by Octave radio (+page) and Z/X.
	let shift = $derived($audioState.octave);

	// Held keys (visual). baseNote -> true.
	let held = $state({});

	function press(noteBase) {
		const note = shiftNote(noteBase, shift);
		held = { ...held, [noteBase]: true };
		noteOn(midiOf(note), freqOf(note));
	}
	function release(noteBase) {
		const note = shiftNote(noteBase, shift);
		const next = { ...held };
		delete next[noteBase];
		held = next;
		noteOff(midiOf(note));
	}

	function onKeyDown(e) {
		if (e.repeat) return; // ignore OS auto-repeat (key already held)
		if (e.code === 'KeyZ') { setOctave(shift - 1, 'hotkey'); return; }
		if (e.code === 'KeyX') { setOctave(shift + 1, 'hotkey'); return; }
		const noteBase = CODE_TO_NOTE[e.code];
		if (noteBase && !e.metaKey && !e.ctrlKey && !e.altKey) press(noteBase);
	}
	function onKeyUp(e) {
		const noteBase = CODE_TO_NOTE[e.code];
		if (noteBase) release(noteBase);
	}

	onMount(() => {
		document.addEventListener('keydown', onKeyDown);
		document.addEventListener('keyup', onKeyUp);
	});
	onDestroy(() => {
		// SSR: onMount не вызывается на сервере, но onDestroy может
		// исполняться при server-side render — guard на document.
		if (typeof document === 'undefined') return;
		document.removeEventListener('keydown', onKeyDown);
		document.removeEventListener('keyup', onKeyUp);
	});
</script>

<!-- 11 white keys (flex row); 7 black keys (absolute, on the boundaries).
     Pointer events drive mouse + touch uniformly (1:1 qwerty-hancock). -->
<!-- Container class is `kbd-box` (NOT `kbd`): the plain `.kbd` utility class
     from flyonui kbd.css (keyboard-shortcut badge: padding .5em, border,
     background, inline-flex) would otherwise hit this container and inject
     padding/box around the keys. -->
<div class="kbd-box" style="height:96px;width:507px" aria-label="Drone keyboard">
	{#each KEYS.filter((k) => !k.black) as k (k.note)}
		<button
			type="button"
			class="kbd-key kbd-key--white {held[k.note] ? 'kbd-key--on' : ''}"
			data-note={k.note}
			aria-label={shiftNote(k.note, shift)}
			onpointerdown={(e) => { e.preventDefault(); press(k.note); }}
			onpointerup={(e) => { e.preventDefault(); release(k.note); }}
			onpointerleave={() => { if (held[k.note]) release(k.note); }}>
		</button>
	{/each}
	{#each KEYS.filter((k) => k.black) as k (k.note)}
		<button
			type="button"
			class="kbd-key kbd-key--black {held[k.note] ? 'kbd-key--on' : ''}"
			style="left:calc((100% / {WHITE_COUNT}) * {k.white} + (100% / {WHITE_COUNT}) * 0.69)"
			data-note={k.note}
			aria-label={shiftNote(k.note, shift)}
			onpointerdown={(e) => { e.preventDefault(); press(k.note); }}
			onpointerup={(e) => { e.preventDefault(); release(k.note); }}
			onpointerleave={() => { if (held[k.note]) release(k.note); }}>
		</button>
	{/each}
</div>

<style>
	.kbd-box {
		position: relative;
		display: flex;
		flex-direction: row;
		user-select: none;
		touch-action: none;
		padding: 0; /* explicit: container must have no padding around keys */
		--kbd-white: oklch(97% 0.01 70);
		--kbd-black: oklch(16% 0.02 65);
	}
	:global([data-theme="luxury"]) .kbd-box { --kbd-white: oklch(88% 0.02 70); --kbd-black: oklch(12% 0.02 65); }
	:global([data-theme="luxury-light"]) .kbd-box { --kbd-white: oklch(99% 0.005 70); --kbd-black: oklch(20% 0.03 65); }
	.kbd-key {
		border: none;
		cursor: pointer;
		padding: 0;
		font-size: 0.62rem;
		display: flex;
		align-items: flex-end;
		justify-content: center;
		padding-bottom: 4px;
		border-radius: 0 0 3px 3px;
		touch-action: none;
	}
	.kbd-key--white {
		position: relative;
		z-index: 1;
		height: 100%;
		width: calc(100% / 11);
		background: var(--kbd-white);
		border: 1px solid rgba(0, 0, 0, 0.12);
	}
	.kbd-key--black {
		position: absolute;
		z-index: 2;
		top: 0;
		width: calc(100% / 11 * 0.62);
		height: 62%;
		background: var(--kbd-black);
		color: #ddd;
		border: 1px solid #000;
	}
	.kbd-key--on { background: var(--color-primary, #4f7cff); }
	.kbd-key--on .kbd-key__lbl { color: #fff; }
	.kbd-key__lbl { opacity: 0.6; font-variant-numeric: tabular-nums; }
</style>
