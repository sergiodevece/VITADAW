# Validación de VitaDAW 0.2.4 — Bus Sends

Fecha: 14 de septiembre de 2026.

## Alcance

Este incremento activa sends `BusId -> BusId` reutilizando `SendId`,
`SendSource`, `SendTapPoint`, `SendMixState` y `SendRoute`. Los Track Sends de
0.2.3 permanecen intactos. No se añaden plugins, inserts, PDC, feedback,
automatización, PFL/AFL, solo-safe, multicanal, routing durante Play ni meters
por send.

## Modelo y comandos

`RoutingState` continúa siendo la única autoridad editable. Se conserva
`AddTrackSend` y se añade `AddBusSend` para que la creación mantenga un origen
fuertemente tipado; `RemoveSend`, `SetSendLevel` y `SetSendMute` siguen
resolviendo ambos orígenes por `SendId`. `SetSendRoute` permite retargetear el
destino y cambiar el tap con el transporte detenido.

Alta, eliminación y reubicación siguen la transacción estructural existente:
se construye y valida el proyecto candidato, se prepara un plan completo y solo
entonces se intercambian modelo, plan, runtime y buffers con el callback
quiescente. Un error de referencia, ciclo, capacidad, presupuesto o preparación
conserva el estado anterior en aplicación y RT.

## Flujo DSP del bus

```text
acumulación de todas las entradas
    |-> PRE TAP -> sends pre -> buses destino
    `-> Bus Gain -> Balance -> Mute -> POST TAP
                                      |-> sends post -> buses destino
                                      |-> Bus Meter
                                      `-> Main Output -> Bus/Master
```

Cada bus se ejecuta una sola vez por subbloque. Por frame obtiene una vez su
estado suavizado y calcula una vez `preTap` y `postTap`; las ramas distribuyen
esos valores sin reprocesar gain, balance, mute o meter. El pre-send ignora Bus
Gain, Balance y Mute. El post-send los incluye. Send Mute y audibilidad cierran
solo la rama correspondiente, y Send Level se aplica después del tap.

El Bus Meter continúa midiendo la señal post-fader/post-mute del canal y no suma
sus sends. Por tanto puede marcar cero mientras un pre-send sigue alimentando
un Aux.

## Plan preparado, DAG y Solo

Cada `PreparedSendDescriptor` resuelve fuera de RT:

- tipo e índice denso del origen;
- bus e índice de buffer destino;
- tap pre/post;
- índice de smoother;
- índice de permiso de audibilidad.

Los descriptors se agrupan en rangos pre/post por pista o bus. No hay búsquedas
de `SendId`, resolución de rutas ni allocations en el callback. Cada send tiene
un smoother lineal independiente de 5 ms que avanza una vez por frame, incluso
muteado, inaudible o con una fuente silenciosa.

El DAG incluye todos los Bus Outputs y Bus Sends aunque estén muteados o a
`-100 dB`. Las aristas paralelas se deduplican únicamente al calcular
dependencias; todas las ramas permanecen en el plan DSP y sus contribuciones se
suman. El orden topológico estable garantiza que cada origen se procese antes
que todos sus destinos y que un bus convergente reciba todas sus entradas antes
de ejecutarse.

La resolución de Solo separa `needsFullBusContent` de `busMain`:

- un bus explícitamente en Solo abre su main y sus sends propios;
- un bus abierto solo como transporte downstream no abre sends laterales;
- un Aux en Solo abre todas sus entradas Track/Bus Send, prepara únicamente el
  contenido upstream necesario y mantiene cerrados los dry paths paralelos;
- varios Solo forman la unión sin abrir ramas hermanas.

## Tests automatizados

Build completo JUCE y build core-only: **17/17 suites superadas**.

La nueva suite `BusSendsTests`, junto con las extensiones de comandos, Track
Sends y lifetime, cubre:

- un send, fan-out, convergencia, varios buses hacia un Aux y ramas paralelas;
- taps pre/post, gain, balance, Bus Mute, Send Mute, Aux/downstream mute y
  `-100 dB`;
- level, rampa de 5 ms, retargeting y avance de smoothers con ramas cerradas;
- autoruta, ciclos cortos/largos y ciclos combinando output/send, incluso con
  aristas muteadas o a `-100 dB`;
- orden topológico estable y procesamiento único de bus, smoother y meter;
- Solo explícito de bus, bus de transporte sin laterales, Aux wet-only con
  Track/Bus Sends, convergencias y varios Solo;
- callbacks de 64, 128, 256, 512 y 1024 frames sobre subbloques internos;
- 32 pistas, 8 buses y 256 sends totales —128 Track Sends y 128 Bus Sends— sin
  allocations en `processBlock`;
- add/remove/retarget transaccional, rechazo durante Play y conservación del
  plan ante fallo;
- sustitución, remove y cierre de planes con Bus Sends, destruidos solo tras la
  quiescencia RT.

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

- ASan + UBSan: **17/17**, sin diagnósticos.
- UBSan independiente: **17/17**, sin diagnósticos.
- TSan: **17/17**, sin carreras detectadas.
- `git diff --check`: correcto.

## Smoke test nativo

Dispositivo observado:

- Altavoces del MacBook Air;
- 48.000 Hz;
- buffer de 512 frames;
- 0 entradas / 2 salidas.

Sesión ejecutada:

- Snare y Kick: `vitadaw-ntrack-440hz-48000-2s.wav`, mono, 48 kHz, 2 s,
  ambos main a Drum;
- Guitar: `vitadaw-ntrack-550hz-44100-3s.wav`, mono, 44,1 kHz, 3 s,
  main a Music;
- Audio 4: `vitadaw-ntrack-660hz-48000-4s.wav`, mono, 48 kHz, 4 s,
  main a Master;
- Snare pre-send a Plate, Drum pre-send a Parallel y Music post-send a Room;
- Drum main a Music; Music, Plate, Parallel y Room a Master.

La reproducción mostró actividad simultánea en pistas, buses, Auxes y Master.
Con Drum muteado, su meter/main quedó a cero mientras Parallel siguió recibiendo
el pre-send. Al mutear Music, Room quedó a cero porque su send es post. Send Mute
cerró Parallel sin cerrar otras ramas, y el nivel del Bus Send aceptó durante
Play el cambio de `-6 dB` a `-18 dB`.

Solo Snare conservó su dry y el Track Send a Plate, pero no abrió el send lateral
de Drum. Solo Drum abrió su main y su Bus Send propio a Parallel. Solo Plate,
Solo Parallel y Solo Room produjeron escuchas wet-only; Plate + Room formó la
unión esperada sin abrir Parallel. Estos resultados se observaron en los meters
y se respaldan con comparaciones numéricas mediante el `processBlock` portable;
la sesión automatizada no captura la salida acústica.

El final natural quedó en `Stopped | 4.000 / 4.000 s`; Play posterior reinició
desde cero y Stop dejó `Stopped | 0.000 / 4.000 s`. La aplicación y el
dispositivo cerraron limpiamente.

## Riesgos y límites pendientes

- El ring de parámetros sigue siendo SPSC y admite un único productor.
- Se conserva un buffer estéreo completo por bus y procesamiento escalar.
- El smoothing de amplitud es lineal; mute y Solo son discretos.
- No hay limiter ni clamp: ramas paralelas pueden superar `[-1, 1]`.
- Los límites preparados actuales son 256 pistas, 64 buses, 1024 sends totales,
  64 sends por pista, 64 por bus y 16 MiB para plan/runtime portable.
- No hay plugins, inserts, PDC, feedback, automatización, PFL/AFL, solo-safe,
  multicanal, routing estructural durante Play ni meters por send.
