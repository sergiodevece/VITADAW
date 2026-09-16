# VitaDAW 0.6.3 — Metronome validation

## Contrato validado

La versión mantiene Musical Time, `PreparedBeatSegment`, reloj, loop y
scheduler exactos de 0.6.0–0.6.2. Para N/D, D define la unidad de beat, existen N
beats por compás y solo el primero recibe accent. No hay grouping, subdivisions,
swing, count-in, muestras configurables ni patrones arbitrarios.

Pause congela la posición, cancela voces ya iniciadas y conserva únicamente una
obligación musical legítima todavía no emitida. Resume no continúa una cola PCM
antigua ni crea un click por sí mismo. Stop, Seek y metronome-off limpian voces y
pending. Los tests producen pending normal y accent mediante scheduling real de
fronteras fraccionales; no escriben flags privados.

Enabled y level permanecen como sesión no persistente, con defaults disabled y
-12 dB. `MetronomeReadModel{enabled,level,temporalRevision}` publica los tres
campos coherentemente y no contiene scheduler ni autoridad temporal. Los
rollbacks conservan semánticamente documento, contexto preparado anterior,
revisión, read model, playback y runUntilStop sin exigir identidad de puntero
como contrato.

La cadena comprobada es Master Inserts → Metronome → Master Gain → Master
Meter/Output. Los inserts no alteran el click, Master Gain sí lo escala y el
meter mide el resultado escalado.

## Cobertura dirigida

- 4/4, 3/4 y 7/8 a 120/123 BPM, incluido un único accent en 7/8;
- vecinos `nextafter` de 123 BPM y cambios de tempo on-beat/off-beat;
- cambios 4/4→3/4→7/8 con accent en cada nuevo BarIndex;
- 44.1, 48 y 96 kHz;
- callbacks monolíticos, 1, 127, 256, 1024 e irregulares;
- comparación de PCM real y tablas diagnósticas 0/1/2;
- Pause a mitad de voz, pending legítimo y Resume sin pending;
- Stop, doble Stop, Seek exacto/anterior/posterior y rechazo durante Playing;
- enabled/disabled en Stopped, Paused y Playing, incluido runUntilStop;
- loopStart downbeat/no downbeat, loopEnd, wrap fraccional y particiones;
- checkpoints, rebuild pending normal/accent/wrap, rate reprepare y rollback;
- mapas densos máximos, posiciones grandes, niveles e inputs inválidos;
- oracle de allocations en callback y oráculos temporales native/portable/fast-math.

Las tablas diagnósticas se preparan por la misma ruta real y reemplazan solo el
PCM del click por impulsos de una muestra: 0 sin evento, 1 normal y 2 accent. No
añaden instrumentación ni API al engine.

## Matriz ejecutada

Validación local en macOS, configuración Debug:

- suite completa con JUCE (`build`): 46/46;
- core-only (`build-core`): 37/37;
- ASan+UBSan core (`build-asan`): 37/37;
- integración JUCE ASan+UBSan (`build-asan-juce`, `temporal_juce_*`): 8/8;
- UBSan (`build-ubsan`): 37/37;
- TSan (`build-tsan`): 37/37;
- oráculos/regresiones temporales, musicales y de metrónomo: 16/16;
- `git diff --check`: limpio.

Los sanitizers se ejecutaron con parada inmediata ante diagnóstico. En ASan se
usó `detect_leaks=0` porque el runtime Apple empleado no ofrece LeakSanitizer;
la detección de accesos inválidos y UBSan permaneció activa. Ningún carril
emitió diagnósticos de AddressSanitizer, UndefinedBehaviorSanitizer o
ThreadSanitizer.

## Riesgos residuales

- el recorrido global acotado de hasta 8191 segmentos por subbloque no dispone
  todavía de benchmark RT profesional;
- LeakSanitizer depende del soporte del runtime de la plataforma;
- la integración automatizada JUCE no sustituye una sesión acústica prolongada
  con hardware real.
