<script>
	// WaveTableView3D — three.js preview of a wavetable surface.
	//
	// The surface is N frames (default 16), each rendered as a POLYLINE of P
	// points (default 256) along X, laid out along Z, with Y = amplitude.
	// A polyline is P independent vertices, so a frame may be straight (all
	// points collinear) OR curved (arbitrary points) — the renderer never
	// assumes straightness; it just plots the P points the data gives it.
	// This is the hook for future per-point editing: only the data changes.
	//
	// Interaction:
	//   - OrbitControls (drag to orbit, wheel to zoom).
	//   - CLICK a line -> it becomes ACTIVE (highlighted). Pick uses a
	//     Raycaster against an invisible thick hit-box per line (a Line's pick
	//     radius is tiny; the box follows the CURVED polyline, not endpoints).
	//     Click is distinguished from an orbit-drag by a move/time threshold.
	//   - Active line re-tinted to --color-primary; the rest dim to neutral.
	//     Colours read from CSS vars so the scene follows the active theme.
	//
	// Props: data (number[][]), points (number), active (number|null),
	//        onActiveChange(i|null). Editing hooks: framePositions(i),
	//        markFrameDirty(i) — mutate data then refresh just that line.

	import { onMount } from 'svelte';
	import * as THREE from 'three';
	import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
	import { EDITOR_FRAMES, EDITOR_POINTS } from '$lib/wavetable/placeholder';

	// Props via $props() — `data` and `points` are reactive prop bindings.
	let {
		data = $bindable(null),
		points = $bindable(EDITOR_POINTS),
		active = $bindable(null),
		onActiveChange = (_i) => {}
	} = $props();

	let containerEl;

	let renderer, scene, camera, controls, raf = 0;
	let lines = []; // { line, positions:Float32Array, frame }
	let lineGroup;
	let pickBox = null;   // single invisible box covering the whole surface (100% catch)
	let themeObs;
	let needsBuild = false; // set by $effect when data changes; consumed by init/loop

	const X_SPAN = 4.0;
	const Z_SPAN = 2.4;
	const Y_SPAN = 1.2;

	function cssColor(varName, fallback) {
		if (typeof window === 'undefined') return fallback;
		const v = getComputedStyle(document.documentElement).getPropertyValue(varName).trim();
		return v || fallback;
	}
	// three's Color (r187) can't parse oklch()/lab()/light-dark() — the active
	// theme (luxury / luxury-light) defines its vars in oklch. Resolve ANY CSS
	// color to #rrggbb by painting it onto a 2D canvas: fillStyle normalises
	// oklch/lab/rgb/hex/keyword to a concrete sRGB value, which we read back.
	let _colorCanvas = null;
	function resolveColor(css, fallback = '#808080') {
		if (!css) return fallback;
		try {
			if (!_colorCanvas) {
				_colorCanvas = document.createElement('canvas');
				_colorCanvas.width = _colorCanvas.height = 1;
			}
			const ctx = _colorCanvas.getContext('2d', { willReadFrequently: true });
			ctx.clearRect(0, 0, 1, 1);
			ctx.fillStyle = css;
			ctx.fillRect(0, 0, 1, 1);
			const d = ctx.getImageData(0, 0, 1, 1).data;
			const h = (n) => n.toString(16).padStart(2, '0');
			return '#' + h(d[0]) + h(d[1]) + h(d[2]);
		} catch (e) {
			return /^#([0-9a-f]{6})$/i.test(css) ? css : fallback;
		}
	}
	function themeColors() {
		return {
			base: resolveColor(cssColor('--color-base-100', '#0f1115')),
			primary: resolveColor(cssColor('--color-primary', '#8b5cf6')),
			neutral: resolveColor(cssColor('--color-neutral', '#3a3f4b'))
		};
	}

	// Build/replace the N polyline line-objects from the live data/points props.
	function buildLinesFrom() {
		const cols = themeColors();
		if (!scene) return;
		if (lineGroup) {
			scene.remove(lineGroup);
			lineGroup.traverse((o) => {
				if (o.geometry) o.geometry.dispose();
				if (o.material) o.material.dispose();
			});
		}
		lineGroup = new THREE.Group();
		lines = [];
		scene.add(lineGroup);
		if (!data) return;
		const F = data.length;
		// Per-frame Z level (matches the vertex layout below).
		const zOf = (f) => (F > 1 ? f / (F - 1) - 0.5 : 0) * Z_SPAN;
		for (let f = 0; f < F; f++) {
			const row = data[f];
			const N = row.length;
			const pos = new Float32Array(N * 3);
			for (let i = 0; i < N; i++) {
				pos[i * 3 + 0] = (i / Math.max(1, N - 1) - 0.5) * X_SPAN;
				pos[i * 3 + 1] = row[i] * Y_SPAN;
				pos[i * 3 + 2] = zOf(f);
			}
			const geo = new THREE.BufferGeometry();
			geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
			const isActive = active === f;
			const mat = new THREE.LineBasicMaterial({
				color: new THREE.Color(isActive ? cols.primary : cols.neutral),
				transparent: true,
				opacity: isActive ? 1.0 : 0.55
			});
			const line = new THREE.Line(geo, mat);
			line.userData.frame = f;

			lineGroup.add(line);
			lines.push({ line, positions: pos, frame: f });
		}
		// ONE invisible box spanning the whole surface volume: it guarantees a
		// click is registered anywhere over the projected surface (per-line
		// hit-boxes left Z-gaps -> most clicks missed). The frame is then chosen
		// by nearest-line-in-screen-space (see pickLine), which correctly handles
		// the overlapping lines caused by the angled camera.
		const pad = 0.3;
		const boxGeo = new THREE.BoxGeometry(X_SPAN + pad, Y_SPAN * 2 + pad, Z_SPAN + pad);
		pickBox = new THREE.Mesh(boxGeo, new THREE.MeshBasicMaterial({ visible: false, side: THREE.DoubleSide }));
		lineGroup.add(pickBox);
	}

	// Resolve a screen click to the nearest line. Step 1: raycast the pick box
	// to confirm the click is over the surface (and get a 3D anchor point).
	// Step 2: project every line's vertices to screen space and pick the line
	// with the smallest distance from the click. This is robust to the angled
	// camera (lines overlap in projection) and works on CURVED lines (every
	// vertex is tested, not just endpoints). Returns a frame index or null.
	function pickLine(clientX, clientY) {
		const dom = renderer?.domElement;
		if (!dom || !camera || !lines.length || !pickBox) return null;
		const rect = dom.getBoundingClientRect();
		const ndc = new THREE.Vector2(
			((clientX - rect.left) / rect.width) * 2 - 1,
			-((clientY - rect.top) / rect.height) * 2 + 1
		);
		const ray = new THREE.Raycaster();
		ray.setFromCamera(ndc, camera);
		const boxHits = ray.intersectObject(pickBox, false);
		if (!boxHits.length) return null;

		// Screen-space coordinates of the click (in device px of the canvas).
		const sx = clientX - rect.left;
		const sy = clientY - rect.top;
		// Project line vertices to NDC -> pixel; track the min distance to the
		// click. Downsample vertices (every 4th) — 256/4 = 64 per line is
		// plenty for a distance test, and keeps this O(16*64) per click.
		const v = new THREE.Vector3();
		let best = -1;
		let bestD = Infinity;
		for (const L of lines) {
			const attr = L.line.geometry.attributes.position;
			for (let i = 0; i < attr.count; i += 4) {
				v.fromBufferAttribute(attr, i).project(camera);
				if (v.z > 1 || v.z < -1) continue; // behind / outside frustum
				const px = (v.x * 0.5 + 0.5) * rect.width;
				const py = (-v.y * 0.5 + 0.5) * rect.height;
				const dx = px - sx, dy = py - sy;
				const d = dx * dx + dy * dy;
				if (d < bestD) { bestD = d; best = L.frame; }
			}
		}
		// Require the nearest line to be reasonably close to the click,
		// otherwise treat as a miss (clicked empty space inside the box).
		// 28px is forgiving: lines are ~1px wide on screen, but the user may
		// overshoot; 28px still keeps adjacent (spaced ~40px+ apart in Y at
		// this camera) lines from cross-picking.
		const hitRadiusPx = 28;
		return bestD <= hitRadiusPx * hitRadiusPx ? best : null;
	}

	function setActive(i, emit = true) {
		if (active === i) return;
		active = i;
		const cols = themeColors();
		for (const L of lines) {
			const on = active === L.frame;
			L.line.material.color.set(on ? cols.primary : cols.neutral);
			L.line.material.opacity = on ? 1.0 : 0.55;
		}
		if (emit) onActiveChange(i);
	}

	// Fixed canvas size (editor preview): 640 wide x 480 tall. The container
	// (CSS) matches this, so the canvas is not stretched/squashed.
	const CANVAS_W = 640;
	const CANVAS_H = 480;

	function init() {
		const box = containerEl;
		const w = CANVAS_W;
		const h = CANVAS_H;
		const cols = themeColors();

		renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true, preserveDrawingBuffer: true });
		renderer.setPixelRatio(Math.min(2, window.devicePixelRatio || 1));
		renderer.setSize(w, h);
		renderer.setClearColor(new THREE.Color(cols.base), 1);
		box.appendChild(renderer.domElement);

		scene = new THREE.Scene();
		camera = new THREE.PerspectiveCamera(45, w / h, 0.1, 100);
		camera.position.set(2.6, 2.2, 4.2);
		camera.lookAt(0, 0, 0);

		controls = new OrbitControls(camera, renderer.domElement);
		controls.enableDamping = true;
		controls.dampingFactor = 0.08;
		controls.target.set(0, 0, 0);

		// (The volume reference frame / bounding box was removed: it read as a
		// limiting "frame" around the surface. The lines stand alone now.)
		buildLinesFrom();
		needsBuild = false;

		let downX = 0, downY = 0, downT = 0;
		const dom = renderer.domElement;
		dom.addEventListener('pointerdown', (e) => {
			downX = e.clientX; downY = e.clientY; downT = performance.now();
		});
		dom.addEventListener('pointerup', (e) => {
			const dx = e.clientX - downX, dy = e.clientY - downY;
			if (dx * dx + dy * dy > 25 || performance.now() - downT > 400) return;
			setActive(pickLine(e.clientX, e.clientY));
		});

		// Canvas is a fixed size (CANVAS_W x CANVAS_H); no resize handling needed.

		const loop = () => {
			raf = requestAnimationFrame(loop);
			if (needsBuild) {
				needsBuild = false;
				buildLinesFrom();
			}
			controls.update();
			renderer.render(scene, camera);
		};
		loop();
	}

	onMount(() => {
		init();
		// Debug/inspect hook: expose scene graph + pick() for in-page testing.
		// pick(clientX, clientY) -> frame index or null (same raycast as click).
		window.__wvt3d = {
			get scene() { return scene; },
			get camera() { return camera; },
			get lines() { return lines; },
			pick(clientX, clientY) {
				return pickLine(clientX, clientY);
			}
		};
		return () => {
			cancelAnimationFrame(raf);
			themeObs?.disconnect();
			controls?.dispose();
			scene?.traverse((o) => {
				if (o.geometry) o.geometry.dispose();
				if (o.material) o.material.dispose();
			});
			renderer?.dispose();
			containerEl?.replaceChildren();
			delete window.__wvt3d;
		};
	});

	$effect(() => {
		// Track the (reactive) data/points props; flag a rebuild. The actual
		// build happens in init() (first scene creation) or the render loop
		// (later data changes) — NOT here, because $effect runs BEFORE
		// onMount/init, when `scene` is still null.
		const _d = data;
		const _p = points;
		needsBuild = true;
	});

	// Re-tint on live theme switches (data-theme is set by +layout.ts / the
	// ThemeSwitch). A MutationObserver catches the attribute change; a plain
	// $effect read of dataset.theme would not be reactive.
	$effect(() => {
		if (typeof MutationObserver === 'undefined') return;
		const retheme = () => {
			if (!scene || !lines.length) return;
			const cols = themeColors();
			for (const L of lines) {
				L.line.material.color.set(active === L.frame ? cols.primary : cols.neutral);
			}
			renderer?.setClearColor(new THREE.Color(cols.base), 1);
		};
		themeObs = new MutationObserver(retheme);
		themeObs.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
		retheme();
		return () => themeObs?.disconnect();
	});

	function framePositions(frame) {
		return lines[frame]?.positions ?? null;
	}
	function markFrameDirty(frame) {
		const L = lines[frame];
		if (!L || !data) return;
		const row = data[frame];
		const pos = L.positions;
		const N = row.length;
		for (let i = 0; i < N; i++) {
			pos[i * 3 + 0] = (i / Math.max(1, N - 1) - 0.5) * X_SPAN;
			pos[i * 3 + 1] = row[i] * Y_SPAN;
		}
		L.line.geometry.attributes.position.needsUpdate = true;
	}
</script>

<div bind:this={containerEl} class="wvt3d">
	<!-- three.js appends its <canvas> here in onMount; no static children. -->
</div>

<style>
	.wvt3d {
		position: relative;
		width: 640px;
		height: 480px;
		overflow: hidden;
		border-radius: var(--radius-field, 0.5rem);
	}
</style>
