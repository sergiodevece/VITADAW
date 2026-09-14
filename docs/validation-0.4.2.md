# Validación de VitaDAW 0.4.2 — Undo / Redo Foundation

Fecha: 14 de septiembre de 2026.

## Implementación

UndoManager portable en DawApplication; CommandDispatcher permanece simple.
Seis payloads tipados de valores de AudioClip/TrackId, labels estables, historial
vector/cursor de 512 entradas y 8 MiB aproximados. Move y trims usan before/after
completos, Duplicate/Delete guardan el clip y Split original/izquierdo/derecho.
No se guardan planes, PCM ni direcciones runtime.

La restauración interna valida ID consumido y ausente, track, source, layout y
bounds. Nunca retrocede nextClipId. Antes de publicar una nueva edición se
prepara el vector de historial que sobrevivirá al commit, incluyendo expulsión
de entradas antiguas e invalidación de Redo. Bajo quiescencia se publican
modelo/plan/historial; las operaciones finales son noexcept. El historial
sustituido se libera después fuera de RT. Undo/Redo solo mueven su cursor tras
commit y usan un plan recién preparado que comparte las fuentes PCM activas.

Los cambios persistentes no undoables son barreras. Los parámetros preparan su
mensaje de éxito antes de publicar a RT; así no existe una allocation posterior
que impida registrar la barrera. Play/Stop y los no-ops de edición no limpian
historial. Tokens identifican estados lógicos; revision cuenta todos los commits.
No se implementa guardado ni dirty UI.

## Pruebas automatizadas

`UndoRedoTests` usa un adaptador doble sin hardware con el compilador de planes
y RealtimeAudioEngine reales. Comprueba:

- Move, Duplicate, Split, Trim Left/Right y Delete; identidad, orden, duración y
  offsets fraccionales de una fuente de 44,1 kHz en proyecto de 48 kHz.
- Cadena Move/Duplicate/Trim/Delete, Undo x4 y Redo x4 con igualdad exacta del
  modelo semántico y del render preparado.
- Mismos IDs después de Redo; IDs abandonados nunca reutilizados.
- Fallos de prepare y commit en Undo/Redo, reintento y conservación de Redo
  ante una nueva operación fallida.
- Inyección de divergencia en la aplicación: historyInvalid conserva modelo,
  plan y cursor; validación directa de restauraciones inválidas.
- Barrido de todos los puntos de allocation de una edición hasta alcanzar un
  commit exitoso; los fallos conservan modelo, plan, token, cursor y Redo.
- Una fuente decodificada una vez; dirección PCM y único PreparedSource
  conservados al rehacer repetidamente.
- Barreras estructurales y ligeras, barrera fallida, Play/Stop sin limpiar
  historial, rechazo durante Play y no-op sin nueva entrada.
- 600 append con expulsión hasta 512, Undo/Redo completos del tramo retenido,
  presupuesto de bytes reducido y rechazo cuando ni una entrada cabe.
- Tokens de retorno al estado guardado simulado y nueva rama; revision
  monotónica en append/Undo/Redo.
- Routing, sends e inserts en pista/bus/master tras reconstrucción. Un insert
  por nodo/subbloque activo, solapes antes del insert, gaps y final natural.
- processBlock real a capacidades 64/128/256/512/1024 y callback de 1100 frames;
  instrumentación de allocations/deallocations y destrucción de owners en RT.

## Builds y sanitizers

- Build completo: 22/22 suites.
- Core-only: 22/22 suites.
- ASan + UBSan: 22/22, sin diagnósticos.
- UBSan independiente: 22/22, sin diagnósticos.
- TSan: 22/22, sin carreras detectadas.
- git diff --check: limpio.

Comandos de reproducción desde la raíz del repositorio:

```sh
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

Los directorios build-asan/build-ubsan/build-tsan usan respectivamente
`-fsanitize=address,undefined`, `-fsanitize=undefined` y `-fsanitize=thread`,
con `-fno-omit-frame-pointer`, app desactivada y sus respectivas suites CTest.

## Smoke test nativo

Aplicación JUCE real con `vitadaw-ntrack-660hz-48000-4s.wav`: PCM16 mono,
48.000 Hz, 4,000 s. Dispositivo Altavoces del MacBook Air, proyecto/dispositivo
a 48.000 Hz, buffer 512 frames, 0 entradas / 2 salidas.

Se ejecutaron desde los botones provisionales:

1. Duplicate #1 @2s: duración 6 s.
2. Move #1 @0.5s: duración 6 s.
3. Split #1 @1.5s: duración 6 s.
4. Trim Left #3 @2s: duración 6 s.
5. Trim Right #3 @3s: duración 6 s.
6. Delete #2: duración 3 s.

Seis Undo aceptados devolvieron el proyecto de 4 s. Un séptimo devolvió
`No history entry`. Play produjo señal en Snare, Drum, Music, Plate, Parallel,
Room y Master; Undo durante Play fue rechazado. Stop conservó Redo.

Seis Redo aceptados restauraron la duración de 3 s; un séptimo fue rechazado por
historial vacío. Se observó Playing a 0,821/3,000 s, final natural
Stopped 3,000/3,000 s, replay desde inicio y Stop 0,000/3,000 s.

Meters observados durante señal: Snare/Drum/Music 0,177 por canal,
Plate/Parallel/Room 0,089 y Master 0,443. Snare Mute dejó la rama pre hacia
Plate activa (Master 0,089). Snare Solo conservó su main y send propio, cerrando
los sends laterales downstream de Parallel/Room (Master 0,265). Tras esas
mutaciones Undo devolvió `No history entry`, confirmando la barrera.

El cierre mediante el botón nativo terminó el proceso; se verificó su ausencia.
No se capturó audio acústico: la comprobación nativa utiliza transporte/meters,
complementada por render numérico offline.

## Archivos de este incremento

Añadidos:

- src/vitadaw/history/UndoManager.h
- src/vitadaw/history/UndoManager.cpp
- tests/UndoRedoTests.cpp
- docs/validation-0.4.2.md

Modificados:

- CMakeLists.txt: versión, módulo y nueva suite.
- README.md y docs/architecture.md: alcance, historial y contratos.
- src/vitadaw/application/DawApplication.h/.cpp: ownership, comandos, staging,
  tokens, barreras y commit coordinado.
- src/vitadaw/commands/Command.h: Undo/Redo y errores.
- src/vitadaw/project/ProjectState.h/.cpp: restauración/reemplazo internos.
- src/vitadaw/platform/juce/JuceApplication.cpp: versión.
- src/vitadaw/platform/juce/MainWindow.cpp: botones provisionales Undo/Redo.

Se conservaron los cambios de 0.4.0/0.4.1 que ya estaban en el working tree.

## Límites

Solo las seis operaciones de clip entran en historial. El comando legado
RemoveClip sigue siendo barrera; DeleteClip es la vía undoable. No hay gestures,
Undo de mixer/routing/processors, composites genéricos ni Undo durante Play.
Historial y tokens pertenecen al hilo de aplicación; no admiten productores
concurrentes. El staging copia como máximo 512 entradas pequeñas y mantiene
temporalmente dos vectores; no es un historial infinito. Los botones de edición
conservan los IDs/posiciones fijos del smoke test. Persistencia queda fuera.
