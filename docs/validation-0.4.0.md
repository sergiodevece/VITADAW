# Validación de VitaDAW 0.4.0 — Source / Clip / Track Foundation

Fecha: 14 de septiembre de 2026.

## Alcance y modelo

Este incremento introduce `SourceId`, `AudioSource`, `MediaReference`, un
`ClipId` fuerte, clips múltiples y layout estable mono/estéreo por pista.
`ProjectSettings` agrupa nombre y sample rate lógico. Cada importación crea una
fuente nueva aunque la ruta coincida; `AddClip` reutiliza explícitamente una
fuente existente. Source y Clip IDs son monotónicos, cero es inválido y no se
reutilizan después de eliminar objetos.

`AudioSource` contiene solo referencia de medio, frames, sample rate y layout.
`AudioClip` contiene solo IDs, `ProjectFramePosition`,
`ProjectFrameDuration` precisa y `SourceFramePosition`. El proyecto conserva
fuentes sin clips y rechaza eliminar una fuente referenciada. No hay PCM ni
estado runtime en el modelo editable.

## Preparación, render y ownership

JUCE decodifica el WAV fuera de RT. Su caché/owner se identifica por `SourceId`,
no por pista. El plan preparado posee arrays densos de vistas de fuentes y
clips; todos los clips que comparten `SourceId` apuntan al mismo PCM. El ratio
`sourceRate/projectRate`, los límites precisos y los índices fuente quedan
resueltos antes del callback.

Cada pista tiene un rango contiguo de clips ordenados por `(projectStart,
ClipId)` y `prefixMaximumEnd`. Una consulta por subbloque usa búsquedas binarias
y filtra únicamente intersecciones reales. El renderer es random-access: parte
siempre de la posición global del reloj maestro. Los clips solapados se suman
en el scratch y la cadena de inserts se procesa una sola vez por pista y
subbloque. La interpolación lineal continúa siendo provisional.

El final de contenido preciso es el máximo final derivado de los clips. El
transporte usa el techo entero como límite exclusivo; el renderer compara con
el final preciso y no emite audio fuera de él. Buses y latencia DSP no alteran
esta duración.

Import, AddClip, RemoveClip y el movimiento estructural mínimo se aceptan solo
detenidos. El flujo candidato -> validación -> preparación/reutilización -> plan
-> quiescencia -> commit intercambia modelo, cache y plan conjuntamente. Un
fallo conserva el estado y bundle anteriores. La destrucción de owners retirados
ocurre después de retirar el callback.

## Tests automatizados

La suite nueva `SourceClipTrackFoundationTests` cubre:

- settings, IDs fuertes/monotónicos y eliminación sin reutilización;
- importaciones independientes de la misma ruta y reutilización explícita;
- uno y diez clips por fuente, múltiples fuentes y pistas vacías;
- orden canónico, offsets, duración precisa, límites y layout incompatible;
- conservación de la fuente al eliminar clips y rechazo si está referenciada;
- gap, tres clips solapados, suma antes de inserts y una llamada por subbloque;
- índice temporal, sample rates distintos y render random-access no secuencial;
- final natural, ausencia de doble atenuación mono y cero allocations RT;
- capacidades 64/128/256/512/1024 y callbacks mayores;
- sesión funcional de 64 pistas, 100 fuentes y 1.000 clips.

Las suites históricas mantienen routing DAG, sends, Solo/Mute, smoothing,
metering, lifecycle, transacciones, processors, latencia y ownership.

Build completo JUCE y build core-only: **20/20 suites superadas** en ambos.

Comandos ejecutados:

```sh
cmake -S . -B build -DVITADAW_BUILD_APP=ON -DVITADAW_BUILD_TESTS=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure

cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF -DVITADAW_BUILD_TESTS=ON
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

## Sanitizers

- ASan + UBSan: **20/20**, sin diagnósticos.
- UBSan independiente: **20/20**, sin diagnósticos.
- TSan: **20/20**, sin carreras detectadas.
- `git diff --check`: sin errores.

## Smoke test nativo

Dispositivo observado:

- Altavoces del MacBook Air;
- 48.000 Hz;
- buffer de 512 frames;
- 0 entradas / 2 salidas.

Se importaron en Snare, a tiempo cero, dos WAV mono distinguibles:

- `vitadaw-ntrack-550hz-44100-3s.wav`: 44,1 kHz, mono, 3 s;
- `vitadaw-ntrack-440hz-48000-2s.wav`: 48 kHz, mono, 2 s.

La ventana conservó duración de proyecto de 3 s. Durante los dos primeros
segundos los meters mostraron simultáneamente señal en Snare, Drum/Music,
Plate/Parallel/Room y Master, confirmando overlap, inserts, routing y sends. El
proyecto alcanzó `Stopped | 3.000 / 3.000 s`; Play posterior reinició desde
cero y Stop explícito dejó `Stopped | 0.000 / 3.000 s`. Mute se aceptó sin
alterar el estado temporal y el cierre dejó la aplicación fuera de ejecución.

La UI provisional no expone todavía `AddClip`, posiciones ni offsets. Por ello
el sharing de un `SourceId`, gaps, offsets, tres overlaps, Solo/Mute y
equivalencia de subbloques se validaron en la ruta real portable de comandos y
`processBlock`, no mediante controles de timeline inexistentes.

## Persistencia y Undo readiness

Una futura serialización guardará settings, contadores, fuentes, pistas, clips,
mixer, routing, sends, inserts y parámetros deseados. El schema version se
añadirá al formato persistido, no como campo runtime sin consumidor. PCM,
índices densos, ratios, scratch, smoothers, generaciones y playhead RT quedan
fuera. Las operaciones trabajan con IDs/valores y candidates, de modo que Undo,
split, trim y duplicate no necesitan cambiar el ownership de la fuente.

## Límites conocidos

- No hay timeline visual, waveform, split/trim, fades ni clip gain.
- No hay persistencia, relink ni missing-media operativo.
- No hay edición durante Play.
- El índice es deliberadamente sencillo y el render sigue siendo escalar.
- PCM completo permanece en memoria con presupuesto de 512 MiB.
- No hay upmix/downmix; mono y estéreo deben coincidir con la pista.
- No hay resampling de calidad final, recording, automation, MIDI ni hosting
  externo de plugins.
