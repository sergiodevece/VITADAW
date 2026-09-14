# VitaDAW 0.5.0 — Timeline UI Foundation

## Alcance validado

Este incremento añade únicamente la primera timeline visual sobre el modelo
`Source -> Clip -> Track`. La UI muestra pistas —incluidas las vacías—, clips,
regla en segundos y playhead. Permite seleccionar un clip, Move, Trim Left/Right,
Split en playhead, Duplicate contiguo, Delete, Undo/Redo, zoom y scroll. Todas
las mutaciones pasan por `ICommandDispatcher` y conservan la preparación/commit
estructural existente.

No se añadieron waveforms, Seek, fades, clip gain, snapping musical, tempo map,
automatización, MIDI, plugins, PDC, multiselección ni edición durante Play.

## Arquitectura y actualización

`TimelineModel` es portable y contiene `TimelineSnapshot`,
`CoordinateTransform` y `TimelineInteraction`. El snapshot se genera desde el
estado confirmado de `DawApplication` y contiene solo datos de pintura e IDs;
no contiene PCM, punteros RT ni referencias mutables. La revisión monotónica de
`UndoManager` cambia también en barreras estructurales, Undo, Redo y Load.

`TimelineComponent` es el adaptador JUCE. Dibuja lanes y clips como primitivas,
hace hit-testing central y culling contra el viewport. La UI conserva solamente
selección, preview, zoom y scroll. Un drag produce un único comando al mouseUp;
el preview se descarta también ante no-op o error y la geometría vuelve a derivar
del snapshot. El timer de aplicación entrega `TransportState` a 30 Hz para el
playhead, sin repaints desde RT.

Tras Load se reconstruye la vista desde el proyecto adoptado, se limpia selección
y el viewport vuelve al inicio. Save y Save As no cambian el timeline musical.

## Coordenadas

La conversión usa exclusivamente el sample rate lógico:

```
x = (projectFrame / projectSampleRate - visibleStartSeconds) * pixelsPerSecond
```

El retorno a modelo redondea al project frame más cercano. Zoom está limitado a
20–600 px/s y conserva el instante bajo el cursor; scroll horizontal se limita a
tiempo no negativo. La duración visible deja diez segundos de margen tras el
contenido y al menos treinta segundos de espacio de trabajo.

## Pruebas automatizadas

La suite `vitadaw_timeline_ui_foundation_tests` cubre:

- frame↔pixel, viewport desplazado, zoom anclado y límites;
- snapshot, nombre derivado de media, transporte y pistas vacías;
- selección por `ClipId` y limpieza al desaparecer;
- preview separado del modelo y un solo comando por gesture;
- Move de dos segundos idéntico a 100 y 200 px/s y clamp en cero;
- Trim Left idéntico entre zooms y Trim Right por límite exclusivo;
- Split solo en interior, Duplicate contiguo y Delete seleccionado;
- snapshot nuevo tras Split y tras restauración equivalente a Undo;
- Save/deserialize/snapshot con posiciones y duraciones idénticas;
- 64 pistas y 1000 clips sin crear componentes JUCE.

Todas las suites anteriores se mantienen. Los tests del timeline no requieren
JUCE, ventana, dispositivo ni hardware.

## Comandos de validación

Build completo:

```sh
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Core-only:

```sh
cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

Sanitizers:

```sh
cmake -S . -B build-asan -DVITADAW_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j 4
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-ubsan -DVITADAW_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined"
cmake --build build-ubsan -j 4
ctest --test-dir build-ubsan --output-on-failure

cmake -S . -B build-tsan -DVITADAW_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j 4
ctest --test-dir build-tsan --output-on-failure
```

Resultados en esta máquina:

- build completo: 26/26 tests;
- build core-only: 25/25 tests;
- ASan + UBSan: 25/25, sin diagnósticos;
- UBSan independiente: 25/25, sin diagnósticos;
- TSan: 25/25, sin carreras detectadas;
- `git diff --check`: limpio.

## Corrección release blocker de lifecycle

Los tres crash reports originales, generados antes de recompilar la corrección,
mostraban procesos de unos 140 ms, acceso a `0x420` y el recorrido
`shutdown -> setStateChangedCallback -> std::function::operator=`. No había un
adapter destruido: el receptor de la llamada era nulo. Con una instancia ya
activa, JUCE rechazaba la segunda antes de invocar nuestro `initialise()`, pero
llamaba después a `shutdown()`. El código anterior desreferenciaba
incondicionalmente `audioDevice_` tras resetear los demás owners.

La corrección mantiene VitaDAW en 0.5.0. El cierre portable se basa en owners
presentes y cubre inicialización completa, fallo antes/después del adapter,
adapter sin ventana, ventana con adapter activo, retirada de callback antes de
destruir el receptor, doble shutdown y fallo de startup. La secuencia validada
es: timer fuera, callback retirado, dispositivo/RT aquietado, ventana,
dispatcher, aplicación y adapter. Excepciones de construcción se diagnostican
con la fase alcanzada, fijan retorno de error y convergen en el mismo cierre.
Un fallo normal al abrir el dispositivo no aborta la UI: queda diagnosticado y
se muestra el aviso existente fuera de RT.

Se reprodujo además el camino original ejecutando una segunda copia mientras la
primera permanecía abierta. La salida fue:

```text
Another instance is running - quitting...
[VitaDAW lifecycle] shutdown entered from phase: not started
[VitaDAW lifecycle] partial/idempotent shutdown completed
```

El proceso terminó con código 0 y no apareció un crash report nuevo.

## Smoke test nativo

Se abrió la aplicación real y se observó `Altavoces del MacBook Air`, 48.000 Hz,
buffer de 512 frames, 0 entradas y 2 salidas. Se importó
`vitadaw-ntrack-550hz-44100-3s.wav` (mono, 44,1 kHz, 3 s): aparecieron sus label,
posición y duración en la lane Snare y se reprodujo con conversión al reloj de
proyecto/dispositivo de 48 kHz.

Se validaron selección, Duplicate contiguo (duración de proyecto 6 s), Move por
drag (7,109 s), Trim Left y Trim Right (6,666 s), Delete, Undo, Redo, zoom,
scroll horizontal, Play, avance visual del playhead y deshabilitación de edición
durante Play. Stop devolvió la posición a 0. Save As creó
`build/timeline-0.5.0-smoke.vitadaw` con writerAppVersion 0.5.0. Tras cierre limpio,
reapertura y Load, la vista volvió al origen, sin selección ni historial, y
reconstruyó los dos clips en 0–3 s y 4,553–6,666 s. Replay, Stop y segundo cierre
fueron correctos.

Tras la corrección de lifecycle se abrió exactamente
`build/vitadaw_app_artefacts/Debug/VitaDAW.app` tres veces, con cierre normal
entre ciclos. En las tres inspecciones visuales posteriores a recompilar se
observó la UI 0.5.0 con Timeline, `Altavoces del MacBook Air`, 48.000 Hz, buffer
de 512 frames, 0 entradas y 2 salidas. Las tres aperturas permanecieron activas,
los cierres finalizaron sin crash y la reapertura fue correcta. No se generó
ningún informe `VitaDAW` posterior al binario recompilado.

Split se comprobó extremo a extremo en la suite portable usando la misma acción
`selected Clip + playhead` y el Command System. No pudo ejecutarse manualmente en
una posición interior porque 0.5.0 no introduce Seek ni Pause y Stop conserva su
semántica de rewind; el botón se rechaza en los límites y se deshabilita durante
Play. Esta limitación es deliberada y no se sorteó con acceso directo al motor.

## Limitaciones deliberadas

- Split depende del playhead observable; sin un comando Seek todavía no puede
  posicionarse desde la regla.
- Culling e hit-test son lineales sobre clips ordenados.
- Los bindings de teclado son provisionales y no configurables.
- La UI no incorpora aún mixer detallado ni medidores específicos; el motor y
  sus comandos permanecen disponibles sin cambios.
