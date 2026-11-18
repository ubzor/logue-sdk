<!-- AudioHost: единственная точка, где Svelte встречает glue (Вариант B).

     onMount: дожидается window.__droneAudioReady (glue публикует его после
     инициализации wasm audio worklet — drone-chain/wasm.cc EM_ASM), затем
     вызывает initAudio() из core.ts и регистрирует контроллер в stores.ts.
     Всё остальное UI ходит через bus/stores — не через этот компонент.

     SSR: onMount не вызывается на сервере, audioReady не трогаем. -->
<script>
	import { onMount } from 'svelte';
	import { audioReady } from '$lib/audio/glue-bridge';
	import { initAudio } from '$lib/audio/core';
	import { setAudioController } from '$lib/audio/stores';

	// onMount: дожидаемся window.__droneAudioReady (glue, drone-chain/wasm.cc
	// EM_ASM), затем initAudio() из core.ts; при unmount — stop() (timers,
	// scope, bus-listeners) + снятие контроллера. SSR: onMount не вызывается.
	onMount(() => {
		let c = null;
		let cancelled = false;
		audioReady
			.then((bundle) => {
				if (cancelled) return;
				c = initAudio(bundle);
				setAudioController(c);
			})
			.catch((err) => console.error('[drone] audio init failed', err));
		return () => {
			cancelled = true;
			if (c) {
				c.stop();
				setAudioController(null);
			}
		};
	});
</script>
