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
	import { EDITOR_FRAMES, EDITOR_POINTS } from '$lib/wavetable/render';

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
	let surfaceMesh = null; // semi-transparent mesh spanning the 16 frame-lines
	let pickBox = null;   // single invisible box covering the whole surface (100% catch)
	let themeObs;
	let needsBuild = false; // set by $effect when data changes; consumed by init/loop

	const X_SPAN = 4.0;
	// The 16 frame-lines are laid out along Z (the surface's "height"); each
	// line spans X_SPAN along X (the surface's "width" = one period). We want
	// the surface square: Z_SPAN == X_SPAN, so a full 16-line stack occupies
	// the same extent as one period in EITHER axis — orientation-independent.
	// (Previously 2.4, which made the surface wider than it was tall.)
	const Z_SPAN = 4.0;
	// Y_SPAN sets the vertical scale: at gain = 1 (row[i] in [-1, +1]) the
	// peak-to-peak height of ONE period = 2 * Y_SPAN. X_SPAN is the period's
	// width. The target proportion at gain = 1 is 1100 (wide) : 240 (tall)
	// ~= 4.583, so 2 * Y_SPAN = X_SPAN / 4.583 = 0.873 -> Y_SPAN = 0.4365.
	// (Previously 1.2, which made the wave ~2.8x too tall / "вытянута".)
	const Y_SPAN = 0.4365;

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
			// Active line: skip depth test so it's visible over the surface.
			if (isActive) {
				mat.depthTest = false;
				line.renderOrder = 10;
			}

			lineGroup.add(line);
			lines.push({ line, positions: pos, frame: f });
		}
		// ONE invisible box spanning the whole surface volume: it guarantees a
		// click is registered anywhere over the projected surface (per-line
		// hit-boxes left Z-gaps -> most clicks missed). The frame is then chosen
		// by nearest-line-in-screen-space (see pickLine), which correctly handles
		// the overlapping lines caused by the angled camera.
		const pad = 0.3;
		// The box must also cover waves whose gain pushes them ABOVE the base
		// Y_SPAN scale (the data is NOT clipped to [-1, +1] in the view — only
		// the source render clips, and a high gain still overflows). Measure the
		// actual peak amplitude across all frames and grow the box height so a
		// click on an over-unity crest still hits the surface.
		let maxAbs = 1.0; // at least the base [-1, +1] span
		for (const L of lines) {
			const attr = L.line.geometry.attributes.position;
			for (let i = 0; i < attr.count; i++) {
				const a = Math.abs(attr.getY(i));
				if (a > maxAbs) maxAbs = a;
			}
		}
		const boxH = Math.max(Y_SPAN * 2, maxAbs * 2) + pad;
		const boxGeo = new THREE.BoxGeometry(X_SPAN + pad, boxH, Z_SPAN + pad);
		pickBox = new THREE.Mesh(boxGeo, new THREE.MeshBasicMaterial({ visible: false, side: THREE.DoubleSide }));
		lineGroup.add(pickBox);

		// Semi-transparent surface mesh: an F x N grid of vertices (the same
		// data as the 16 frame-lines), triangulated with indexed quads. This
		// gives the smooth interpolation between adjacent lines that matches
		// WteTable::lookup (bilinear in Z and X). Rendered UNDER the lines.
		if (F > 1) {
			const N = data[0].length;
			const gridPos = new Float32Array(F * N * 3);
			for (let f = 0; f < F; f++) {
				const row = data[f];
				for (let i = 0; i < N; i++) {
					const idx = (f * N + i) * 3;
					gridPos[idx]     = (i / Math.max(1, N - 1) - 0.5) * X_SPAN;
					gridPos[idx + 1] = row[i] * Y_SPAN;
					gridPos[idx + 2] = zOf(f);
				}
			}
			const gridIdx = new Uint32Array((F - 1) * (N - 1) * 6);
			let k = 0;
			for (let f = 0; f < F - 1; f++) {
				for (let i = 0; i < N - 1; i++) {
					const a = f * N + i;
					const b = f * N + (i + 1);
					const c = (f + 1) * N + i;
					const d = (f + 1) * N + (i + 1);
					gridIdx[k++] = a; gridIdx[k++] = b; gridIdx[k++] = c;
					gridIdx[k++] = b; gridIdx[k++] = d; gridIdx[k++] = c;
				}
			}
			const surfGeo = new THREE.BufferGeometry();
			surfGeo.setAttribute('position', new THREE.BufferAttribute(gridPos, 3));
			surfGeo.setIndex(new THREE.BufferAttribute(gridIdx, 1));
			surfGeo.computeVertexNormals();
			// MeshStandardMaterial for proper lighting: the directional + ambient
			// lights give the surface a 3D relief (peaks brighter, valleys darker).
			// Use the primary (ochre) color at reduced brightness so the surface
			// has a warm tint and the lines (neutral/primary) still read on top.
			const surfColor = new THREE.Color(cols.primary);
			surfColor.multiplyScalar(0.45);
			const surfMat = new THREE.MeshStandardMaterial({
				color: surfColor,
				side: THREE.DoubleSide,
				metalness: 0.0,
				roughness: 0.85
			});
			surfaceMesh = new THREE.Mesh(surfGeo, surfMat);
			surfaceMesh.renderOrder = -1; // draw before lines
			lineGroup.add(surfaceMesh);
		}
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
		const changed = active !== i;
		active = i;
		// Always re-tint the lines (even if `active` didn't change, e.g. when
		// $effect re-fires after a prop sync). The tint loop is cheap (16 lines).
		const cols = themeColors();
		for (const L of lines) {
			const on = active === L.frame;
			// The active line is drawn ON TOP of the surface mesh (depthTest off)
			// so it is never occluded by the opaque surface between it and the camera.
			L.line.material.depthTest = !on;
			L.line.renderOrder = on ? 10 : 0; // active line renders last
			L.line.material.color.set(on ? cols.primary : cols.neutral);
			L.line.material.opacity = on ? 1.0 : 0.55;
		}
		// Only emit back to the parent if the value actually changed (avoids
		// infinite loops when $effect -> setActive -> onActiveChange -> prop
		// -> $effect ...).
		if (emit && changed) onActiveChange(i);
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
		// Lights for the surface mesh (MeshStandardMaterial needs them).
		// A soft ambient + a directional from above-front gives a 3D feel
		// without being too dramatic.
		const ambient = new THREE.AmbientLight(0xffffff, 0.4);
		scene.add(ambient);
		const dirLight = new THREE.DirectionalLight(0xffffff, 0.8);
		dirLight.position.set(3, 0, 2);
		scene.add(dirLight);
		const dirLight2 = new THREE.DirectionalLight(0xffffff, 0.2);
		dirLight2.position.set(-3, 0, 2);
		scene.add(dirLight2);
		// Orthographic (parallel-projection) camera: no perspective foreshortening,
		// so the 1100:1100 square surface and 1100:240 period proportion are preserved
		// exactly on screen regardless of viewing angle. The 'frustumSize' controls how
		// much of the world is visible: we want the surface (X_SPAN=4 wide) to fill
		// most of the 640x480 canvas. frustumSize = 6 gives ~2.5:1 aspect headroom.
		const frustumSize = 6;
		const aspect = w / h;
		camera = new THREE.OrthographicCamera(
			-frustumSize * aspect / 2, frustumSize * aspect / 2,
			frustumSize / 2, -frustumSize / 2,
			0.1, 100
		);
		camera.position.set(0, 3, -4.5);
		camera.zoom = 1.9;
		camera.updateProjectionMatrix();
		camera.lookAt(0, 0, 0);

		// OrbitControls: left-drag orbits, wheel zooms, right-drag pans. It
		// calls setPointerCapture on pointerdown, which is fine in real
		// browsers. Picking a line on a STATIONARY click still works: we track
		// the pointerdown point/time ourselves and only call pickLine on
		// pointerup if the pointer barely moved (< ~5px) and was held < 400ms.
		// A drag (orbit) therefore never triggers a pick.
		controls = new OrbitControls(camera, renderer.domElement);
		controls.enabled = false; // temporarily disabled
		controls.enableDamping = true;
		controls.dampingFactor = 0.08;
		controls.target.set(0, 0, 0);
		// Disable pan so right-drag / two-finger doesn't fight the layout:
		controls.enablePan = false;
		// Log camera position to console on every change (orbit/zoom).
		controls.addEventListener('change', () => {
			const p = camera.position;
			console.log(`[WVT3D] camera: x=${p.x.toFixed(3)} y=${p.y.toFixed(3)} z=${p.z.toFixed(3)} zoom=${camera.zoom.toFixed(3)}`);
		});

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
			if (controls) controls.update();
			renderer.render(scene, camera);
		};
		loop();
	}

	onMount(() => {
		init();
		// Debug/inspect hook: expose scene graph + pick() for in-page testing.
		// pick(clientX, clientY) -> frame index or null (same raycast as click).
		// activate(frame) -> manually set the active line (for testing).
		window.__wvt3d = {
			get scene() { return scene; },
			get camera() { return camera; },
			get lines() { return lines; },
			pick(clientX, clientY) {
				return pickLine(clientX, clientY);
			},
			activate(frame) {
				setActive(frame);
			},
			// Re-read data[frame] into the existing line's vertices without
			// rebuilding the whole surface. The /editor route calls this after
			// mutating the active wave, because the `data` prop reference does
			// not change (only table[i] is replaced), so the internal
			// needsBuild $effect does not fire.
			refreshFrame(frame) {
				markFrameDirty(frame);
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

	// Sync the active prop (from parent, e.g. the morph slider) with the
	// internal line highlighting. setActive() is called from pointerup
	// (clicks on lines), but when the parent changes `active` directly
	// (slider drag), we need to re-tint the lines here.
	$effect(() => {
		const a = active;
		if (!scene || !lines.length) return;
		setActive(a, false); // don't emit back to parent
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
			if (surfaceMesh) {
				surfaceMesh.material.color.set(new THREE.Color(cols.primary).multiplyScalar(0.45));
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
		// Also update the corresponding row in the surface mesh
		if (surfaceMesh) {
			const F = data.length;
			const surfAttr = surfaceMesh.geometry.attributes.position;
			for (let i = 0; i < N; i++) {
				const idx = (frame * N + i) * 3;
				surfAttr.setX(idx, (i / Math.max(1, N - 1) - 0.5) * X_SPAN);
				surfAttr.setY(idx, row[i] * Y_SPAN);
				// Z stays the same (zOf(frame))
			}
			surfAttr.needsUpdate = true;
			surfaceMesh.geometry.computeVertexNormals();
		}
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
