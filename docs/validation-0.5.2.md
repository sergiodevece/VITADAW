# VitaDAW 0.5.2 — Musical Time Foundation

Validación local en macOS / Apple Silicon, 15 de septiembre de 2026.

## Contratos implementados

- ProjectFrame/RealtimeProjectClock sigue siendo la única autoridad que avanza.
  No se añadió reloj musical ni código musical a processBlock.
- PPQ15360, ticks/duraciones int64 fuertes, quarters continuos, bar/beat cero-based,
  display uno-based y BPM double finito 20–400 (negras por minuto).
- TempoEvents step anclados a tick; TimeSignatureEvents anclados a BarIndex.
  Iniciales obligatorios/editables/protegidos de Move/Delete. IDs uint64 exactos,
  monotónicos no reciclados. Numerador1..32 y denominadores1,2,4,8,16,32,64.
- PreparedMusicalTimeMap inmutable, tablas separadas, prefijos compensados,
  consultas binarias y enumeración span acotada. Índice radix auxiliar de
  profundidad máxima41 evita recorrer todos los cambios entre líneas del ruler.
- Nearest/ties-up para Seek; floor normalizado para display; ceil explícito
  disponible para límites exclusivos. El display no se reutiliza como autoridad.
- Ocho comandos undoables. Edición solo Stopped, preparación completa antes
  del commit modelo+mapa+history; no toca frame actual, duración ni plan RT.
- Schema2 determinista, migración real v1→v2 con validación antes/después.
  V1 original intacto; Load migrado limpio, sin historial; Save posteriorv2.
- Ruler Seconds/Frames/BarsBeats, display Bar|Beat|Tick, revisión independiente
  del transporte y tres botones musicales provisionales, sin UI avanzada.

## Pruebas

`MusicalTimeTests` cubre los golden120/60/180 BPM a48k, 4/4→3/4→7/8,
tempo en tick777, empate/ceil/floor, límites antes/exacto/después, BPM123.456,
sample rates44.1/48/96k, 4096 eventos de cada tipo, consultas aleatorias,
frame entero round-trip durante días y tick→frame preciso→tick.
Referencia independiente de prefijos en long double (en Apple Silicon no
necesariamente ofrece más bits que double); tolerancia de comparación1e-5frames.
El oracle no llama a las conversiones que verifica. Las pruebas de round-trip
son exactas tras la cuantización declarada, no una tolerancia de un tick/frame.

Grid: rango vacío, capacidad cero, truncado/continuación, subdivisiones,
fronteras, muchas métricas, y 4096 cambios de tempo antes de la segunda línea.
Validación: NaN/Inf, BPM y métricas inválidos, IDs/anclas duplicadas o ausentes,
curvas no soportadas, sample rate inválido, límites/overflow y capacidad.

`ProjectPersistenceTests` añade las ocho operaciones y Undo/Redo, protección
inicial, contadores no reutilizados, dirty/revisión, rechazo Playing/Paused,
Seek123 seguido de edición sin alterar el frame/plan/duración, fallo en cada
allocation previa al commit, y Load inválido conservando ambas representaciones.
Render real processBlock antes/después al mismo frame123, también con
device44.1k/project48k; muestra idéntica y cero allocations/destrucciones RT.
Los fixtures v1 reales conservan campos/IDs/clips/routing/mixer/inserts; los
campos float se comparan como el valor float histórico, no su spelling JSON.
Una sesión con audio migrada de v1 produce la misma muestra que v2. V1→Load
clean→SetTempo→Undo/Redo→SaveAsv2→Load comprueba el mapa preparado adoptado.
Las pruebas anteriores de audio, routing, inserts, lifecycle y gate se conservan.

## Comandos reproducibles

```sh
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
open build/vitadaw_app_artefacts/Debug/VitaDAW.app
```

Sanitizers core-only, Debug, `-fno-omit-frame-pointer`:

```sh
cmake -S . -B build-asan -DVITADAW_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan -j 4
ctest --test-dir build-asan --output-on-failure
cmake -S . -B build-ubsan -DVITADAW_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer"
cmake --build build-ubsan -j 4
ctest --test-dir build-ubsan --output-on-failure
cmake -S . -B build-tsan -DVITADAW_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
cmake --build build-tsan -j 4
ctest --test-dir build-tsan --output-on-failure
git diff --check
```

Resultados: build completo **27/27**; core-only **26/26**; ASan+UBSan
**26/26**; UBSan independiente **26/26**; TSan **26/26**, sin diagnósticos.
`git diff --check` limpio. Los sanitizers cubren el núcleo y dobles de plataforma,
no el dispositivo CoreAudio físico. El barrido de fallos de Load alcanza el
commit tras comprobar más de 800 posiciones de allocation fallidas.

## Smoke nativo

Se abrió `build/timeline-0.5.0-smoke.vitadaw`, schema1 existente. Contiene dos
clips de `vitadaw-ntrack-550hz-44100-3s.wav` (mono,44100Hz,3s) en Snare:
inicio0/duración144000frames e inicio218550/duración101400frames. Duración
del proyecto319950frames=6.665625s, sin cambios al introducir el mapa musical.
Dispositivo: Altavoces del MacBook Air,48000Hz,512frames,0entradas/2salidas.
Proyecto48000Hz.

Se verificaron Seconds→Frames→BarsBeats, Seek real en ruler a109920frames
(2.290s), tempo inicial100BPM, cambio60BPM en tick245760, métrica7/8 en barIndex8,
Undo/Redo y zoom para ver compases futuros de anchuras distintas. El frame
permaneció109920; el display pasó de2|1|8908 a1|4|12544. Las dos capturas de la
ventana, al mismo zoom, muestran los clips sin desplazamiento ni cambio de ancho.

Play→Pause observado en130400frames (2.717s), botones musicales deshabilitados
en Paused; Stop/dobleStop y replay; fin natural Stopped en319950frames y
display3|4|1680. SaveAs creó `/private/tmp/vitadaw-0.5.2-smoke.vitadaw` con
schema2, tempos100/60 y métricas4/4,7/8. El v1 original se conserva. Cierre desde
el botón nativo de ventana y reapertura realizados. Load v2 restauró clips y
mapa musical, sin historial previo. Replay observado en Playing/257536frames,
display3|1|14472, master0.442/0.442 y pista1=0.177/0.177. Cierre nativo final realizado.
La build final se ejecutó además directamente: startup completed, Load v2,
Play/Pause/Stop, normal shutdown completed y código de salida **0**.
No se dispone de captura acústica: no se afirma una escucha independiente.
El render numérico y el flujo de dispositivo se verifican por separado.

## Archivos

Añadidos:

- `src/vitadaw/musical/MusicalTime.h`
- `src/vitadaw/musical/MusicalTime.cpp`
- `src/vitadaw/application/MusicalTimeCommands.cpp`
- `tests/MusicalTimeTests.cpp`
- `docs/validation-0.5.2.md`

Modificados:

- `CMakeLists.txt`: versión, fuentes, suite nueva y JSON en tests de migración.
- `README.md`, `docs/architecture.md`: contratos, historial, instrucciones/límites.
- `src/vitadaw/project/ProjectState.h`, `.cpp`, `ProjectDocumentData.cpp`: submodelo, swap y validación documental.
- `src/vitadaw/application/DawApplication.h`, `.cpp`: owner preparado, revisión, despacho e historial musical.
- `src/vitadaw/application/ProjectPersistenceCommands.cpp`: preparar/adoptar mapa junto al documento/audio.
- `src/vitadaw/commands/Command.h`: ocho comandos tipados.
- `src/vitadaw/history/UndoManager.h`, `.cpp`: payloads pequeños de eventos y restauración exacta.
- `src/vitadaw/persistence/ProjectPersistence.h`, `.cpp`: schema2, codec y registro v1→v2.
- `src/vitadaw/ui/timeline/TimelineModel.h`, `.cpp`: revisión y posición musicales del read model.
- `src/vitadaw/platform/juce/JuceApplication.cpp`: versión de la app.
- `src/vitadaw/platform/juce/MainWindow.cpp`: display y botones provisionales.
- `src/vitadaw/platform/juce/TimelineComponent.h`, `.cpp`: modos/densidad del ruler.
- `tests/ProjectPersistenceTests.cpp`: regresión/migración/transacción musical.
- `tests/fixtures/persistence/future-version.vitadaw`: future=3; los fixtures v1 no se modifican.

## Límites y riesgos

Dominio numérico explícito0..2^40 ticks/frames; no se promete precisión para
todo int64. Capacidad4096+4096, prepared total≤1MiB, grid UI≤512líneas.
Las referencias al mapa son solo de aplicación y no deben retenerse tras un
commit; un consumidor RT futuro necesitará publicación/reclamación explícita.
El índice de grid aprovecha el dominio fijo; ampliar ese dominio exige revisar
su profundidad, las conversiones y los contratos numéricos.
Controles de eventos fijos y etiqueta de historial son provisionales.
No loop, metronome, rampas, swing, snapping, MIDI, automation, recording,
waveform, time-stretch, musical audio lock ni external sync.
