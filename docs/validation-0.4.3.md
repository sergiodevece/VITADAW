# VitaDAW 0.4.3 — Project Persistence

Fecha: 14 de septiembre de 2026. Backend nativo validado: macOS, Apple Silicon.

## Formato, modelo y límites

Archivo `.vitadaw` JSON UTF-8 legible; format `VitaDAWProject`, schemaVersion 1,
writerAppVersion 0.4.3. No ZIP, paquetes ni medios embebidos. ProjectDocument v1
enumera campos explícitos: settings, nextIds, sources, tracks, routing, buses,
sends y master. IDs/contadores uint64 son strings decimales exactos, incluidos
valores superiores a 2^53. La factory validada reconstruye IDs y nextIds, nunca
Add + corrección ni max+1. Los contadores deben superar todos los IDs presentes.

Se conserva nombre/sample rate de proyecto, metadata de fuentes, clips con
posiciones/offsets/duraciones, layouts, mixer, DAG editable, sends e inserts con
tipo estable, ParameterId, valores deseados y bypass. `internal.gain` exige estado
serializado vacío. No se guardan dispositivo, transporte, PCM, índices preparados,
DSP runtime, smoothers, meters, colas, UI ni historial.

Claves ordenadas; arrays no semánticos canónicos por ID; clips por inicio/ClipId;
orden de canales y procesadores preservado. Floats finitos round-trip y locale
independiente; indentación de dos espacios, newline final, sin timestamps.
El registro de migraciones DOM existe vacío; esquemas futuros y campos/tipos
desconocidos se rechazan. No hay apertura parcial/best effort.

Lectura real <=16 MiB, depth <=32, paths/nombres <=4 KiB, keys <=128 B.
SAX comprueba duplicados, cardinalidad, strings y profundidad antes del DOM.
Contenedores JSON limitados a 32 MiB dentro del sobre conservador de documento
128 MiB, con límites auxiliares de 500000 nodos, 8 MiB de strings de valores y
64 keys/objeto. Pueden rechazar antes del máximo nominal. Modelo: 256 pistas,
2048 fuentes, 32768 clips/4096 por pista, 64 buses, 1024 sends/64 por origen,
512 procesadores/16 por cadena. Las dependencias cíclicas incluyen sends mudos.

nlohmann/json 3.12.0 fijado mediante CMake/URL/SHA-256; solo persistence incluye
sus headers. JUCE permanece fuera del dominio y del codec. La limpieza del DOM
es por profundidad acotada y sin allocation; el barrido de fallos detectó y
corrigió el uso indirecto del stack dinámico del destructor general de JSON.

## Sesión y transacciones

ProjectSession reúne ProjectState, UndoManager, ruta y savedStateToken.
UndoManager mantiene currentStateToken y revision. Save conserva historial;
Save As tampoco cambia nombre ni crea operación Undo. Dirty compara tokens.
Move→Undo al estado guardado es clean; Move→Save→Undo es dirty.

Save/Save As/Load se rechazan durante Play, sin auto-stop. Load dirty requiere
autorización expresa de descarte; el diálogo es de UI, no del núcleo.

Save valida y prepara documento/metadata antes de I/O. El backend macOS crea
temporal exclusivo mismo directorio, hace write completo, fsync + F_FULLFSYNC,
rename atómico y fsync de directorio. Solo después publica path/savedToken
noexcept. Fallo pre-rename conserva archivo anterior y sesión. Fallo post-rename
devuelve durabilityUncertain, sin afirmar rollback ni marcar clean. Se limpia
el temporal fallido, no se crean backups visibles. No se afirma soporte equivalente
de durabilidad en otros sistemas ni inmunidad a fallos físicos del almacenamiento.

Load prepara íntegramente modelo exacto, PCM aislado, plan y metadata; retira el
callback, espera quiescencia y publica modelo/plan/recursos/sesión noexcept.
El reemplazado se destruye fuera de RT. Antes de commit un fallo conserva todo A.
Tras commit, una reconexión fallida deja B cargado y dispositivo error; no es
rollback. Load deja historial vacío, token nuevo, revision monotónica de sesión,
savedToken=current, clean y Stopped en cero. No modifica procesamiento RT.

## Identidad de medios

MediaReference localFile contiene ruta relativa/fallback absoluto y SHA-256
completo/tamaño. Se prueba relativa al documento primero, fallback después;
ambas se verifican. Nunca cwd, expansión shell, URLs ni path como identidad.
Los bytes retenidos que alimentan MemoryInputStream/JUCE son los mismos que se
hashean. El digest esperado se comprueba antes del decode. Después se comprueba
el archivo mediante hash streaming e identidad inode/dispositivo/mtime/ctime
del read original: también se detecta un replace con los mismos bytes.

Trabajo por Source, no por Clip. El candidato nace con cache vacía; SourceId=1
en dos proyectos no comparte PCM por accidente. Presupuesto PCM de 512 MiB:
activo + candidatos previos + bytes WAV actuales + decode actual. Scratch/DSP
conservan su presupuesto preparado independiente. Save usa el fingerprint ya
asociado al PCM; nunca sustituye ese hash con el de un archivo modificado.
Save As rebasa rutas hacia medios resueltos y conserva hash/ID/nombre.

Falta/cambio de cualquier medio impide Load completo. No hay offline/relink.
PersistenceResult expone code, phase y contexto opcional (path, JSON Pointer
cuando se conoce exactamente, entidad/ID y error del sistema). UI muestra códigos
estables y gestiona consentimiento; errores recuperables no son excepciones UX.

## Pruebas nuevas

ProjectPersistenceTests, sin JUCE/hardware:

- Round-trip completo de fuentes, diez clips con valores fraccionales, pistas
  mono/estéreo, mezcla, buses, track/bus sends e inserts Track/Bus/Master.
- Igualdad canónica de todos los campos y contadores; determinismo, golden
  mínimo y fixture completo; IDs >2^53 y contadores deliberadamente superiores
  a max+1.
- Truncado, firma/esquema, duplicate key, campo desconocido, IDs inválidos/
  overflow, duplicate ClipId, contador bajo, referencia rota, bounds/layout/
  enums/parámetros/rates inválidos, processor desconocido, ciclos,
  límites de strings/arrays/depth/file.
- Save/Save As/Load, política Stopped, descarte explícito, saved/current tokens,
  Undo/Redo después de Load y Save sin pérdida de historial.
- Fallos de cada fase de Save. También se inyectan en el backend macOS real:
  tempCreate, write, flush, replace y durability. Archivo anterior intacto antes
  del replace, nuevo visible en durabilityUncertain, sin temporales abandonados.
- Relative/fallback, proyecto movido, Save As rebase, missing/changed media,
  fingerprint incorrecto y Save que conserva el hash del PCM antiguo.
- A/SourceId1 positivo → B/SourceId1 negativo: comprobación numérica mediante
  RealtimeAudioEngine real. Una fuente/diez clips/un decode/un PCM.
- Fallos de parse, último medio, processor, prepare y commit conservan modelo,
  plan, PCM, historial, path y tokens; A sigue reproduciéndose.
- Fallo de reconnect después de commit conserva B limpio con dispositivo error.
- Barrido de 776 posiciones de fallo de allocation durante Load antes del
  primer éxito, sin publicación parcial. Lifetime instrumentado: último owner
  previo liberado después de quiescencia; cero allocations/destrucciones RT en
  processBlock real, con solapes/routing/sends/inserts y callback de 1024 frames
  sobre capacidad de subbloque 64.
- SHA-256 conocido de `abc`, verificación streaming y sustitución con los
  mismos bytes pero nueva identidad de archivo.

ProjectMediaIntegrationTests, con JUCE pero sin dispositivo físico:

- WAV PCM16 reales de 480 frames, mono, 44100/48000 Hz y muestras de signo
  distinto; decoder real, persistencia y commit del adaptador real.
- A y B con SourceId=1: render B negativo tras Load, sin contaminación de A.
- Cambio de WAV rechazado antes del decode verificado; rollback mantiene PCM
  anterior reproducible. Diez clips restaurados usan 480 floats de un solo PCM.
- Shutdown y destrucción fuera de RT. El test no inicializa AudioDeviceManager;
  un acceso friend confinado al harness ejecuta el consumidor offline exclusivo.

Las 22 suites anteriores se conservan íntegramente.

## Builds y sanitizers

- Full: 24/24 suites, incluida integración JUCE sin hardware.
- Core-only: 23/23 suites.
- ASan + UBSan: 23/23 suites core-only, sin diagnósticos.
- UBSan independiente: 23/23 suites core-only, sin diagnósticos.
- TSan: 23/23 suites core-only, sin carreras detectadas.
- `git diff --check`: limpio.

```sh
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
open build/vitadaw_app_artefacts/Debug/VitaDAW.app

cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

Sanitizers: configurar app OFF, `CMAKE_CXX_FLAGS` con
`-fsanitize=address,undefined -fno-omit-frame-pointer`,
`-fsanitize=undefined -fno-omit-frame-pointer` o
`-fsanitize=thread -fno-omit-frame-pointer`, respectivamente, y mismo
`-fsanitize=...` en CMAKE_EXE_LINKER_FLAGS. Build + CTest en cada directorio.
Puede reutilizarse nlohmann descargado mediante FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON.

## Smoke nativo

Documento: `build/persistence-0.4.3-smoke.vitadaw` (artefacto de validación, no
fixture portable). Fuente: `vitadaw-ntrack-660hz-48000-4s.wav`, PCM16 mono,
48000 Hz, 192000 frames, 4 s, 384044 bytes. SHA-256 de la fuente:
`9306c5ce1697f2c36a5541952eb4f02b76424e98072fafee9d17ac853f1cf97b`.

Dispositivo Altavoces del MacBook Air, proyecto/dispositivo 48000 Hz, buffer
512 frames, 0 entradas / 2 salidas.

1. Import de fuente a Snare; Duplicate #1 @2s y Move #1 @0.5s.
2. Save As desde diálogo nativo: success. Documento con 1 Source, 2 Clips,
   4 Tracks, 5 Buses, 3 Sends y 3 GainProcessor; duración 6 s.
3. Cierre nativo y comprobación de ausencia del proceso; reapertura nueva.
4. Load del archivo y diálogo de cambios sin guardar de la sesión de ejemplo
   inicial. Tras resolverse la confirmación, proyecto de 6 s cargado y UI
   reconstruida a partir del modelo. El clic de comprobación de Undo vacío
   fue bloqueado por la revisión de seguridad: ese caso se verifica en tests.
5. Play observado a 0.992/6.000 s. Meters por canal: Snare/Drum/Music 0.177,
   Plate/Parallel/Room 0.089, Master 0.443. Routing, sends e inserts presentes.
6. Final natural Stopped 6.000/6.000 s. Snare Mute: Plate 0.089, Master 0.089,
   ramas principales silenciosas. Restaurar Mute y Solo Snare: main+Plate,
   Parallel/Room cero, Master 0.265. Restaurar Solo.
7. Stop vuelve a 0.000/6.000 s. Split #1 @1.5s → Undo committed, restaura
   los dos clips. Save success, conservando nextClipId=4 (ID consumido por Split).
8. Dos Save sin cambios producen SHA-256 de documento idéntico:
   `1fec2c204a5f06ea9f1fd375ff23645c394b6a5142c9ddd24ac7102055973ef5`.
9. Cierre nativo; no queda proceso VitaDAW.

No hay captura acústica: validación nativa por transporte/meters y validación
numérica offline del audio, además del uso del decoder JUCE real.

## Archivos de este incremento

Añadidos:

- src/vitadaw/application/ProjectSession.h
- src/vitadaw/application/ProjectPersistenceCommands.cpp
- src/vitadaw/project/ProjectDocumentData.cpp
- src/vitadaw/persistence/PersistenceResult.h
- src/vitadaw/persistence/ProjectPersistence.h
- src/vitadaw/persistence/ProjectPersistence.cpp
- src/vitadaw/platform/files/ProjectFileIO.h
- src/vitadaw/platform/files/ProjectFileIO.cpp
- tests/ProjectPersistenceTests.cpp
- tests/ProjectMediaIntegrationTests.cpp
- tests/fixtures/persistence/minimal-project-v1.vitadaw
- tests/fixtures/persistence/full-project-v1.vitadaw
- tests/fixtures/persistence/future-version.vitadaw
- tests/fixtures/persistence/invalid-duplicate-key.vitadaw
- tests/fixtures/persistence/invalid-truncated.vitadaw
- docs/validation-0.4.3.md

Modificados:

- CMakeLists.txt: versión, dependencia fijada, fuentes y suites.
- README.md y docs/architecture.md: contratos, alcance, historial y validación.
- src/vitadaw/application/DawApplication.h/.cpp: ProjectSession y comandos.
- src/vitadaw/audio/IAudioEngineControl.h: preparación verificada y reemplazo
  documental aislado, metadata/fingerprint y resultado de preparación.
- src/vitadaw/media/AudioSource.h: MediaFingerprint asociado a referencia.
- src/vitadaw/project/ProjectState.h y src/vitadaw/routing/RoutingState.h:
  exportación DTO/factory exacta, sin exponer mutación privada al parser.
- src/vitadaw/commands/Command.h: Save/Save As/Load y resultado portable.
- src/vitadaw/platform/juce/JuceAudioDeviceAdapter.h/.cpp: snapshot WAV,
  verificación, presupuesto agregado, colección aislada y harness offline.
- src/vitadaw/platform/juce/JuceApplication.cpp: versión de aplicación.
- src/vitadaw/platform/juce/MainWindow.h/.cpp: selectores, confirmación, códigos
  y reconstrucción de controles/valores después de Load.

Se preservaron los cambios anteriores de 0.4.0/0.4.1/0.4.2 que ya estaban en el
working tree. La lista de git incluye esos cambios, no solo este incremento.

## Riesgos y limitaciones

I/O/hash/decode son síncronos y pueden bloquear la UI, nunca RT; cada Source se
lee de nuevo para verificar que no cambió durante preparación. Documentos muy
grandes pueden rechazarse por cotas auxiliares antes del máximo de entidades.
La política es estricta: falta/cambio de un medio requerido impide abrir todo el
proyecto. No se garantiza guardado durable en backends no implementados ni frente
a dispositivos que incumplan fsync. No hay garantías ante agotamiento absoluto
de memoria del proceso/OS; se prueban los fallos recuperables antes de commit.

El UI sigue siendo provisional y conserva botones de edición con IDs fijos.
Las operaciones son serializadas en el hilo de aplicación, no multi-productor.
Solo seis operaciones de clip son undoables; mixer/routing/import siguen siendo
barreras. No se añade relink/offline, autosave, backups, packages, embedded media,
async I/O, timeline visual, MIDI, automation ni hosting externo.
