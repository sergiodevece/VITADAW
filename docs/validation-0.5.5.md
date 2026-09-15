# VitaDAW 0.5.5 — Waveform Foundation

## Alcance validado

El incremento añade formas de onda mono/estéreo a la timeline existente sin
modificar reproducción, edición documental ni callback. No incorpora grabación,
fades, clip gain, edición destructiva, análisis espectral, transients, stretch,
automatización, MIDI ni generación de waveform en vivo. El schema sigue en v3.

## Arquitectura y preparación

`WaveformCache` pertenece a `ProjectSession` y almacena una entrada inmutable por
`SourceId`. `ProjectState`, `TimelineSnapshot`, Undo y el plan RT no contienen
arrays de picos. Tras decodificar y validar el WAV, JUCE presta una vista del PCM
ya residente al builder portable; no hay segunda lectura ni copia masiva.

El nivel base usa 128 source frames. Guarda min/max float por canal y agrega
pares para crear niveles superiores. Mono mantiene una serie; estéreo mantiene
L/R independientes. Se rechazan shapes inválidos, punteros ausentes, NaN,
infinito y aritmética desbordable con diagnóstico explícito.

Cada entrada publica `approximateBytes`. La caché de sesión tiene un presupuesto
de 64 MiB. Superarlo omite el waveform pero no rechaza audio, proyecto ni plan;
la UI usa un placeholder. No existe LRU en este incremento.

Import y Load construyen una caché candidata antes del commit. El mismo callback
de commit `noexcept` que publica el modelo intercambia la caché; cualquier fallo
previo conserva el proyecto, PCM y waveforms activos. Load comienza con caché
vacía, evitando colisiones de SourceId entre sesiones, y reconstruye desde los
medios verificados. Delete y Undo reutilizan el handle conservado.

## Mapping y render

La conversión es:

```
sourceFrame = sourceOffset
            + clipLocalProjectFrame * sourceSampleRate / projectSampleRate
```

El device sample rate no participa. La selección de nivel deriva de source
frames por pixel; a zoom normal/bajo visita aproximadamente uno o dos buckets
por columna. El detalle máximo queda limitado al bucket base de 128 frames.
Solo se visitan clips y columnas visibles. `paint()` consume handles const y no
lee archivos, decodifica, calcula fingerprints, genera niveles ni espera.

## Validación automática

`vitadaw_waveform_foundation_tests` cubre:

- extrema mono y L/R estéreo con señales conocidas;
- agregación exacta de niveles multirresolución;
- NaN/infinito, fallo inyectado por presupuesto cero y presupuesto total;
- fuentes sintéticas cercanas a `uint64_t::max` sin overflow ni indexación;
- una Source compartida por diez clips y conservación en Duplicate, Split,
  Trim, Move entre pistas y Delete;
- impulso 44,1 kHz en proyecto 48 kHz y cálculo de sourceOffset;
- selección de nivel, límite base y trabajo proporcional al rango visible;
- 1000 clips con culling horizontal y 1000 handles de fuente;
- aislamiento de dos cachés con el mismo SourceId.

`vitadaw_project_media_integration_tests` usa el decoder JUCE real y verifica
una sola identidad para diez clips, sustitución de sesión A/B con SourceId=1,
conservación tras Load fallido, reconstrucción al reabrir y cero allocations en
el `processBlock` real. Las suites anteriores mantienen lifecycle, lifetime,
Undo/Redo, timeline, persistencia, routing y RT como regresión.

## Resultados

- Build Debug completo: 30/30 tests superados.
- Build core-only: 29/29 tests superados.
- ASan + UBSan: 29/29 tests superados, sin diagnósticos.
- UBSan independiente: 29/29 tests superados, sin diagnósticos.
- TSan: 29/29 tests superados, sin carreras detectadas.
- `git diff --check`: sin errores.

Durante la primera ejecución del test largo se detectó un overflow en la forma
ingenua `(count + 127) / 128`; quedó sustituida por
`1 + (count - 1) / 128` y cubierta con un frameCount cercano al máximo de 64 bits.

## Smoke test nativo

Ejecutado con:

- dispositivo: Altavoces del MacBook Air;
- proyecto/dispositivo: 48.000 Hz;
- buffer: 512 frames;
- canales: 0 entradas / 2 salidas;
- `vitadaw-track1-440hz-44100.wav`: mono, 44.100 Hz, 2 s;
- `vitadaw-track2-660hz-48000.wav`: mono, 48.000 Hz, 4 s.

Se recorrieron los 24 pasos prescritos: proyecto vacío; importación y aparición
del primer waveform; segunda pista/importación; ambos waveforms; zoom in/out y
scroll; Split a 1 s; Trim; Move entre pistas; Duplicate; Undo/Redo; tempo a
100 BPM sin cambio de geometría absoluta; loop y metrónomo; Play/Stop con señal
y meters; Save As a `/private/tmp/vitadaw-0.5.5-waveform-smoke.vitadaw`; cierre;
reapertura y Load; reconstrucción visual de los waveforms; Delete Track/Undo; y
cierre limpio confirmado por el sistema.

Tras Load reaparecieron el clip derecho del split en Track 1 y, en Track 2, la
fuente de 48 kHz junto al clip movido y su duplicado, conservando sus ventanas
de fuente. El cambio de tempo alteró únicamente las etiquetas musicales. Loop
enabled y metrónomo se reiniciaron al cargar, como corresponde a estado de
sesión no persistente.

## Riesgos y límites pendientes

- La preparación es síncrona fuera de RT; archivos grandes pueden bloquear la
  UI hasta que exista un worker cancelable.
- El bucket base de 128 frames no ofrece detalle sample-level a zoom extremo.
- No hay LRU ni GC de Sources sin clips; el presupuesto fijo limita el crecimiento.
- La estimación no incluye con precisión absoluta overhead de allocator/map.
- El waveform representa PCM fuente pre-inserts; no refleja gain, processors ni
  la suma audible final, deliberadamente.
