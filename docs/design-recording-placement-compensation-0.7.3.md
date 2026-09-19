# VitaDAW 0.7.3B — Recording Placement Compensation

> Estado: implementación completada en el árbol de trabajo. Este documento fija
> el contrato de 0.7.3B: no altera PCM/WAV ni rediseña 0.7.3A.

## Objetivo y contrato

0.7.3B compensa exclusivamente la **posición documental** de una toma al
publicarla como `clips::AudioClip`. El WAV publicado conserva, en el mismo
orden y sin desplazamiento destructivo, los samples raw aceptados por
`RealtimeCapture`.

```text
interpretación física
  -> latencia de entrada del dispositivo
  -> callback / RealtimeCapture
  -> ring SPSC / writer / WAV raw e inmutable
  -> finalización y verificación de media
  -> cálculo no-RT de la posición documental
  -> commit atómico de AudioSource + AudioClip en ProjectState
```

No es compensación de Monitoring. En particular, ni `Monitor Gain`, ni el staging de 0.7.2, ni `Estimated Monitoring Latency`, ni la latencia de salida intervienen en la posición de una toma.

## Recording timeline actual auditado

| Etapa | Propietario actual | Hecho relevante para placement |
| --- | --- | --- |
| Orden de usuario | `commands::Record` entra por `DawApplication::handle()` en `src/vitadaw/application/DawApplication.cpp`. | La orden no fija hoy un start frame persistente: puede requerir preflight no-RT antes de que el callback la acepte. |
| Preflight | `JuceAudioDeviceAdapter::prepareRecording()` en `src/vitadaw/platform/juce/JuceAudioDeviceAdapter.cpp`. | Abre/valida input, reprepara el motor, reserva ring, crea temporal exclusivo y writer WAV, y devuelve un `RecordingRequest` preparado. |
| `prepared` | `RealtimeAudioEngine::tryRequestRecord()` llama a `RealtimeCapture::prepareRequest()` y encola `beginRecord`. | Capture ya tiene sesión/track/layout; aún no tiene posición ni rate. |
| Aceptación RT | `RealtimeAudioEngine::consumeCommands()`. | Llama a `RealtimeCapture::begin(request, clock_.publicPosition(), processingFormat_.sampleRate)` y después a `clock_.record()`. |
| Inicio de Capture | `RealtimeAudioEngine::processBlock()`. | Tras consumir commands, `capture_.capture(input)` copia el input raw antes de meter, staging, limpiar output o Playback. |
| Servicio | `JuceAudioDeviceAdapter::serviceRecording()`. | Drena el ring y escribe WAV; no conoce placement. |
| Finalize/publicación | `DawApplication::synchroniseRecording()` → `finalizeRecording()` → `commitRecordedAudio()`. | La media se cierra, sincroniza, publica y verifica antes de tocar `ProjectState`. |
| Creación del clip | `DawApplication::commitRecordedAudio()`. | Calcula placement después de validar media y llama a `ProjectState::importAudioToTrack(..., compensatedStart)`: es el único punto que fija `AudioClip::projectStart`. |

### Start actual y ownership

El start actual de una toma es `RecordingSnapshot::projectStart`. No es el instante en que la UI pulsó Record: es `clock_.publicPosition()` en el callback que acepta `beginRecord`. Es un `timeline::ProjectFramePosition`, publicado por `RealtimeCapture::begin()` junto con `deviceSampleRate` y usado por `commitRecordedAudio()`.

Durante preflight, prepared y capture, los propietarios actuales son:

- `JuceAudioDeviceAdapter`: directorio, temporal exclusivo, identidad de fichero, marker auxiliar, `RecordingFileHandle`, stream, writer WAV, rutas temporal/publicada y publicación segura.
- `RealtimeCapture`: sesión, track, layout, `projectStart`, `deviceSampleRate`, contador de frames aceptados y ring SPSC. El callback es el único productor; `serviceRecording()` es el consumidor.
- `DawApplication`: `activeRecording_`, lifecycle de aplicación, creación de candidato `ProjectState`, history y commit de plan.

Tras finalizar, `RecordingFinalizationResult` transporta el `RecordingSnapshot`, `PreparedAudioFilePtr`, ruta publicada y diagnósticos. `ProjectState::importAudioToTrack()` crea `media::AudioSource` y `clips::AudioClip`; `history::RecordAudio` conserva copias owning de ambos para Undo/Redo.

El end es exclusivo. `ProjectState::importAudioToTrack()` deriva duración desde `sourceFrameCount * projectRate / sourceRate`; `ProjectState::validateClip()` y `timeline::checkedExclusiveProjectEnd()` comprueban el rango.

## Terminología y dominios

| Magnitud | Dominio propuesto | Significado |
| --- | --- | --- |
| `recordRequestProjectFrame` | `ProjectFramePosition`, diagnóstico opcional | Posición proyectada al solicitar Record. No es autoridad porque preflight y la cola pueden retrasar la aceptación RT. |
| `capturedStartProjectFrame` | `ProjectFramePosition` | Actual `capture.projectStart`; posición asociada al primer bloque aceptado. |
| `reportedInputLatencyDeviceFrames` | `optional<DeviceFrameCount>` | `AudioIODevice::getInputLatencyInSamples()` no negativo, leído fuera de RT al rate de dispositivo. |
| `reportedInputLatencyProjectFrames` | `optional<ProjectFrameCount>` | Conversión determinista de la cifra anterior al dominio de proyecto. |
| `manualRecordingOffsetProjectFrames` | `ProjectFrameCount` signed | Corrección efímera de usuario: negativa antes, positiva después. |
| `effectivePlacementCompensationProjectFrames` | `ProjectFrameCount` signed | Cantidad que se resta a captured start: `reportedInputLatencyProjectFrames - manualOffset`. |
| `compensatedStartProjectFrame` | `ProjectFramePosition` | Start final, limitado al rango válido del clip. |

`acceptedDeviceFrames` sigue describiendo frames de WAV/Capture y no se reinterpretará como duración de proyecto ni como compensation.

## Fuente de latency y fórmula

La base de 0.7.3B es solamente la **input latency reportada**. JUCE expone
input y output separadamente; el adaptador la consulta fuera de RT desde el
`AudioIODevice` certificado. La implementación CoreAudio de JUCE, por ejemplo,
compone input latency, safety offset, buffer y stream latency de entrada. Otros
backends pueden tener otra composición, por lo que VitaDAW no añade el buffer
por separado.

No se suma output latency: describe callback → salida física, puede afectar a lo que se oye durante Monitoring, pero no a cuándo llegaron al callback los samples que el WAV conserva. Sumarla movería la toma por una ruta ajena a Capture. Tampoco se suman `currentBufferSizeSamples`, staging, smoother o cola propia: hacerlo puede duplicar valores ya informados por backend.

Con input latency disponible y convertida a project frames:

```text
effectivePlacementCompensation = reportedInputLatencyProjectFrames
                                 - manualRecordingOffsetProjectFrames

rawCompensatedStart = capturedStartProjectFrame
                      - effectivePlacementCompensation
```

Es decir: `capturedStart - reportedInputLatency + manualOffset`.

Ejemplo:

```text
captured start       = 100000
reported input       =    256
manual offset        =    -16  (antes)
effective comp.      =    272
raw compensated      =  99728
```

Con `manualOffset = +16`, el clip queda 16 frames más tarde que la compensación automática. Con latency reportada 0, sólo opera el offset manual. Con latency desconocida, la base automática es 0 y un offset manual válido sigue aplicándose.

## Snapshot por toma

Se congela un `RecordingPlacementSnapshot` en el preflight exitoso de
`JuceAudioDeviceAdapter::prepareRecording()`, después de certificar input,
device rate, buffer y contexto temporal, y antes de hacer visible `beginRecord`
al callback. El snapshot viaja en el `RecordingRequest` preparado, se copia por
`RealtimeCapture` en `prepared` y vuelve mediante
`RecordingFinalizationResult.capture`.

El mínimo snapshot es:

- project rate y device rate certificados;
- identidad/contexto de dispositivo y buffer efectivo para diagnóstico;
- input latency de device opcional y conversión opcional a project frames;
- manual offset, effective compensation y estado de diagnóstico (`reported`, `unavailable` o `invalid` degradado).

El `capturedStartProjectFrame` no puede congelarse en preflight: sólo existe al aceptar `beginRecord` en RT. Finalize/publication lo combinará con el snapshot inmutable.

Cambio posterior de buffer, rate, device o manual offset no altera esa toma ni una ya publicada. Un cambio antes de la siguiente toma toma otro snapshot. Durante Recording, buffer control ya se rechaza; pérdida/reinicio involuntario terminaliza la toma según lifecycle actual, sin recálculo a mitad de ella.

## Conversión device rate → project rate

`getInputLatencyInSamples()` está en frames del `AudioIODevice::getCurrentSampleRate()`. El proyecto posee su propio `ProjectState::sampleRate()` y el clock actual soporta project y device rates distintos; por tanto la cifra no se puede copiar directamente a `ProjectFrame` salvo a igual rate.

```text
reportedInputLatencyProjectFrames = round-nearest(
  reportedInputLatencyDeviceFrames * projectSampleRate / deviceSampleRate)
```

La política es **round to nearest, ties away from zero**, implementada una vez
en un helper checked de compensation; no se repiten casts `double` en
UI/adaptador. Valida rates finitos y positivos, entrada no negativa, resultado
finito, rango `int64_t` y límite de `ProjectFrame`; el placement posterior usa
aritmética signed comprobada.

| Device → project | 256 input frames | Resultado |
| --- | ---: | ---: |
| 48 kHz → 48 kHz | `256 * 48000 / 48000` | 256 |
| 44.1 kHz → 48 kHz | `278.639…` | 279 |
| 48 kHz → 44.1 kHz | `235.2` | 235 |

La conversión ocurre una vez al snapshot. Milisegundos son sólo presentación y nunca autoridad ni conversión de ida y vuelta.

## Convención manual, frame 0 y arithmetic safety

La convención única es negativa = antes, cero = sin ajuste y positiva = después. Internamente se almacena sólo `ProjectFrameCount` signed. El límite de producto/control es **±2.0 segundos** convertidos con el project rate efectivo (por ejemplo, ±96000 frames a 48 kHz y ±192000 a 96 kHz). No es un límite matemático del motor temporal. Una UI futura que acepte ms convierte una vez fuera de RT; `NaN`, infinito, valores no convertibles o fuera de ese rango se rechazan explícitamente sin alterar el último offset válido.

El helper `computeRecordingPlacement(capturedStart, snapshot)`:

1. validar snapshot y rango `0 .. 2^53 - 1` de `ProjectFrame`;
2. calcular `reported - manual` y `captured - compensation` con enteros signed checked o la utilidad wide existente;
3. limitar el start y publicar diagnóstico de clipping, sin overflow/underflow;
4. deja la validación del end exclusivo a `ProjectState::importAudioToTrack()`,
   que aplica `checkedExclusiveProjectEnd()` antes de aceptar el clip.

Quedan prohibidos casts de un `int` JUCE negativo a `uint32_t`, y casts de offset negativo a `size_t` o `uint64_t`.

La posición final se satura a la ventana legal del clip:

```text
compensatedStart = clamp(rawCompensatedStart,
                         0,
                         máximo start cuyo end exclusivo es válido)
```

Así, `capturedStart = 100`, compensation `= 256` publica el clip en `0` y
registra que 156 frames de avance no pudieron aplicarse. Ese valor se conserva
como diagnóstico efímero de la última toma comprometida y se muestra en el read
model/UI; no forma parte de `ProjectState`. El WAV, duración y end no se
modifican. El helper rechaza un start fuera del límite superior; si el end
exclusivo no cabe, `importAudioToTrack()` rechaza el candidato. En ambos casos
no hay commit y la media válida se retiene según 0.7.1.

## Punto de aplicación y política de fallos

El único cambio propuesto está en `DawApplication::commitRecordedAudio()`:

```text
finalize media válida
  -> verificar metadata, WAV e identidad como hoy
  -> computeRecordingPlacement(capture + snapshot)
  -> candidate.importAudioToTrack(..., compensatedStart)
  -> preparar waveform/processing plan
  -> commit atómico ProjectState + history + plan
```

No se aplica en callback, `RealtimeCapture`, ring, `drainRecording`, writer, fsync, publicación de media ni marker recovery. Si latency metadata está unavailable/invalid, base automática 0, offset manual válido y diagnóstico explícito: la media sigue publicable. Si un placement positivo o su end exclusivo excede el límite temporal, se aborta únicamente el commit documental y se retiene la media válida como orphan según 0.7.1; nunca se hace wrap ni se desplaza silenciosamente el WAV. Los fallos de ring, writer, close, fsync, publicación, identidad, directory fsync o WAV siguen siendo críticos y bloqueantes.

## Undo/Redo, Save/Load

`history::RecordAudio` ya posee el `AudioSource` y `AudioClip` completos. Al ser el `compensatedStartProjectFrame` el `AudioClip::projectStart` almacenado:

- Undo elimina la toma.
- Redo restaura el mismo source y el mismo clip.
- Redo no consulta device, buffer, rate, manual offset ni recalcula compensation.

La persistencia actual guarda `projectStartFrames`; basta para reproducir correctamente tras Save/Load. No se añadirá a `ProjectState` captured start, latency snapshot, manual offset ni effective compensation: no son necesarios para reproducción, son diagnóstico efímero y no habilitan recalibración retroactiva.

## Monitoring, buffer, lifecycle y UI

Placement es independiente de Monitoring ON/OFF, gain, staging, input meter, Playback, Play/Stop/Seek/Loop y output/estimated monitoring latency. Dos tomas con mismo device/context, latency snapshot y offset deben tener el mismo start con Monitoring ON u OFF y WAV bit-identical.

El buffer de 0.7.3A puede cambiar latency para la toma siguiente; durante la toma sigue rechazado y una toma publicada nunca se mueve. Device/input/output loss o rate change inesperado preservan la política existente: Capture terminaliza y no hay recalculado intermedio. Fuera de Recording, el read model se refresca/invalida para el próximo preflight.

El offset es una preferencia efímera aplicación/device, default 0, fuera de
`ProjectState`, dirty state, Undo/Redo y archivo de proyecto. No existe
infraestructura general de preferencias y 0.7.3B no crea una. La UI mínima, por
Command System → `DawApplication`, muestra el control en frames y su read model.
Cuando input latency está unavailable/invalid, su parte automática es cero pero
la Effective Recording Compensation sigue siendo `-manualOffset`; no se muestra
falsamente como cero. También muestra el clamp documental de la última toma si
existió:

```text
Reported Input Latency:              128 device frames / 2.67 ms (Reported)
Recording Offset:                      0 project frames / 0.00 ms (Manual)
Effective Recording Compensation:    128 project frames / 2.67 ms
```

`Unknown`/`Unavailable` nunca se sustituye por output latency o `Estimated Monitoring Latency`.

## Relación futura con loopback 0.7.3C

Una calibración posterior podrá generar un impulso, medir round trip físico y derivar error residual. Su única salida hacia 0.7.3B será sugerir/alimentar el mismo `manualRecordingOffsetProjectFrames`; no cambia WAV, snapshots ni clips existentes. No se implementan aquí impulso, medición, detección, auto-calibración o perfiles persistidos.

## Cobertura implementada

1. latency 0, positiva, unknown y negativa/invalid;
2. manual 0, positivo, negativo y combinado con latency;
3. frame 0 exacto, cruce de 0, clipping diagnosticado observable, límite
   `2^53 - 1` y end exclusivo con rollback documental/retención de media;
4. igual rate, 44.1 → 48, 48 → 44.1 y rounding determinista;
5. snapshot fijo durante Recording; take A/B mediante adaptador JUCE productivo
   después de cambio de buffer/latency, antes de la toma siguiente;
6. Monitoring ON/OFF igual placement y WAV bit-identical; Playback independiente;
7. media failure bloqueante; latency unavailable no destruye media válida;
8. Undo/Redo conserva posición tras cambiar device/buffer/offset; Save/Load conserva start;
9. límites exactos ±2 s a 44.1 y 96 kHz, con rechazo ephemera fuera de rango;
9. regresiones 0.7.3A buffer/read model, 0.7.2 Monitoring/alias/RT y 0.7.1 Recording/Recovery;
10. instrumentación: el callback no consulta latency, no calcula placement, no
    toca `ProjectState`, no reserva, registra ni usa filesystem/locks/waits.

Los fixtures JUCE hardware-free deben fijar rate y input latency, pero placement debe recorrer `prepareRecording` → `processBlock` → finalize → commit; ningún mock puede saltar Capture o la publicación documental.

## Riesgos y mitigaciones

| Riesgo | Mitigación |
| --- | --- |
| Confundir Monitoring con placement | Sólo input latency reportada entra en fórmula. |
| Doble conteo de buffer | No sumar buffer/callback/staging a cifra del backend. |
| Cambio mueve una toma | Snapshot inmutable; redo reutiliza clip almacenado. |
| Underflow/overflow | Helper signed checked, clamp y end exclusivo. |
| Falta latency impide grabar | Base automática 0, offset válido y diagnóstico. |
| PCM mutado | Único punto: argumento `projectStart` del import/commit. |
| Trabajo RT accidental | Snapshot/preflight/finalize no-RT; callback sólo Capture raw. |
| Preferencia ambiental en proyecto | Offset efímero fuera de ProjectState/document/history. |

## Implementación realizada

1. Snapshot/diagnóstico de placement en la frontera Recording, sin formato de
   proyecto nuevo.
2. Offset efímero, read model y control mínimo por Command System.
3. Captura de input latency certificada en preflight y propagación por
   `RecordingRequest` → `RealtimeCapture` → finalization.
4. Conversión y placement checked con clamp/diagnóstico.
5. Sustitución exclusiva del start pasado a `importAudioToTrack()`.
6. Tests de regresión, instrumentación RT existente y sanitizers focalizados.

## Decisiones que requieren aprobación

No quedan decisiones arquitectónicas pendientes. El límite de producto/control aprobado es **±2 segundos** convertidos a project frames; no limita la aritmética interna de `ProjectFrame` ni la futura calibración loopback.
