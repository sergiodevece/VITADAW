# Validación de VitaDAW 0.2.3 — Track Sends & Auxes

Fecha: 14 de septiembre de 2026.

## Alcance

Este incremento añade múltiples sends Track→Bus con identidad estable, taps
pre-fader/pre-pan y post-fader/post-pan, level suavizado, mute propio y
audibilidad preparada por arista. Un Aux continúa siendo un bus existente. No
añade Bus Sends audibles, plugins, inserts, PDC, feedback, automatización,
meters por send, PFL/AFL, solo-safe, multicanal ni routing durante Play.

## Modelo e identidad

`RoutingState` sigue siendo la única autoridad editable de routing. Cada
`SendRoute` contiene:

- un `SendId` fuerte, monotónico de 64 bits, estable y no reutilizado;
- un `SendSource` tipado como `TrackId | BusId`;
- un `BusId` destino;
- un `SendTapPoint` pre o post;
- un `SendMixState` con level y mute.

La salida principal sigue representada por `OutputDestination`; no se convierte
en un send implícito. Se permiten varios IDs para el mismo origen y destino, y
sus contribuciones se suman deliberadamente.

0.2.3 solo crea y ejecuta Track Sends. Un Bus Send entra en el grafo estructural
para validar referencias y ciclos, incluso muteado o a −100 dB, y después se
rechaza explícitamente como todavía no soportado.

## Flujo de señal y procesamiento preparado

Cada pista se renderiza una vez por subbloque. El motor obtiene de ese render:

```text
render original
    |-> PRE: adaptación estéreo -> gate/level del send -> Bus
    `-> gain/pan/mute de pista -> POST
                                  |-> gate del main -> destino principal
                                  `-> gate/level del send -> Bus
```

Para mono, el pre-tap produce `x/sqrt(2)` en L y R. El post-path vuelve al render
mono original y aplica la pan law una sola vez. Para estéreo, el pre-tap conserva
L/R. Track Gain, Track Pan y Track Mute no afectan al pre-send por política de
producto de este incremento; sí afectan al main y al post-send. Send Mute solo
cierra su propia rama.

`PreparedProcessingPlan` contiene descriptors contiguos ordenados, mapping
`SendId`→índice denso y rangos pre/post por pista. Cada frame mantiene los taps
en valores locales y acumula directamente en el buffer del bus: no hay render
adicional ni buffer de audio por send. `ProcessingPlanRuntime` posee un
`SendMixSmoother` por send y comparte lifetime con descriptors y buffers.

El presupuesto preparado contabiliza buffers, descriptors, mapping, rangos,
runtime/smoothers, pasos y máscaras. Los límites iniciales son 1024 sends
totales y 64 por pista, además de 256 pistas, 64 buses y 16 MiB para este
plan/runtime portable. Overflow, referencias, IDs, tap, estados DSP y memoria
se validan antes del commit.

## Parámetros, Solo y metering

Send Level usa −100..+12 dB, con −100 dB como silencio exacto. NaN, infinito y
valores fuera de rango se rechazan. La conversión a amplitud lineal ocurre fuera
de RT. El gain usa una rampa lineal sample-accurate de 5 ms y avanza exactamente
una vez por frame, aunque el send esté muteado, cerrado por Solo o reciba
silencio. Send Mute es discreto.

`SetSendLevel` y `SetSendMute` resuelven el `SendId` en el productor y publican
índice denso más estado preparado por el ring SPSC existente. Se aceptan durante
Play y no reconstruyen el plan. Una cola llena rechaza el comando y conserva
coherentes el modelo, la especificación persistida y RT.

`PreparedAudibilityState` separa permisos de main, sends, buses y Track Meter:

- Track Solo abre el dry, todos sus sends y los buses downstream necesarios;
- Bus Solo selecciona entradas main upstream, sin abrir sends laterales;
- Aux Solo abre los sends que lo alimentan y produce wet-only;
- varios Solo forman la unión, sin expandir upstream desde nodos abiertos solo
  como transporte;
- Mute actúa localmente y prevalece;
- Track Meter conserva la señal post-fader/post-mute y no suma sends. Un
  pre-send puede sonar con Track Meter a cero.

Después de converger contribuciones en un bus se procesa una mezcla común; no
se intenta recuperar su procedencia individual.

## Tests automatizados

Build core-only y build completo JUCE: **16/16 suites superadas**.

La nueva suite `TrackSendsAuxesTests`, junto con las ampliaciones de comandos y
lifetime, cubre:

- identidad monotónica, no reutilización, duplicados deliberados y mapping
  denso independiente del orden editable;
- un send, varios sends, un origen a varios buses, varios orígenes a un Aux y
  dos aristas idénticas con IDs distintos;
- pre/post, mono sin doble atenuación, Track Gain/Pan/Mute, Send Mute, Aux Mute
  y mute en un bus downstream;
- level, silencio, boost válido y rechazo de NaN/infinito/fuera de rango;
- smoothing, retargeting, rama inaudible y un único avance de smoothers;
- Track Solo dry+wet, Bus Solo sin sends laterales, Aux Solo wet-only, unión de
  solos y convergencia en buses;
- callbacks de 64, 128, 256, 512 y 1024 frames con capacidad interna 128,
  conservando señal, reloj y meters;
- 32 pistas, 8 buses y 128 sends sin allocations en `processBlock`;
- IDs/destinos/taps inválidos, límites total/por pista, presupuesto y overflow;
- futuros Bus Sends cíclicos, incluso silenciosos, y rechazo explícito del caso
  acíclico todavía no ejecutable;
- add/remove estructural transaccional, rechazo durante Play, persistencia de
  parámetros aceptados y cola llena;
- sustitución de un plan con send por otro sin él y cierre, demostrando que el
  owner anterior se destruye solo después de la última región RT.

Comandos de validación:

```sh
cmake -S . -B build -DVITADAW_BUILD_APP=ON -DVITADAW_BUILD_TESTS=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure

cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF -DVITADAW_BUILD_TESTS=ON
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

## Sanitizers

- ASan + UBSan: **16/16**, sin diagnósticos.
- UBSan independiente: **16/16**, sin diagnósticos.
- TSan: **16/16**, sin carreras detectadas.
- `git diff --check`: correcto.

## Smoke test nativo

Dispositivo observado:

- Altavoces del MacBook Air;
- 48.000 Hz;
- buffer de 512 frames;
- 0 entradas / 2 salidas.

Sesión:

- Snare: `vitadaw-ntrack-440hz-48000-2s.wav`, mono, 48 kHz, 2 s,
  main→Drum→Music→Master y pre-send→Plate→Master;
- Kick: `vitadaw-ntrack-330hz-44100-1s.wav`, mono, 44,1 kHz, 1 s,
  main→Drum;
- Guitar: `vitadaw-ntrack-550hz-44100-3s.wav`, mono, 44,1 kHz, 3 s,
  main→Music y post-send→Delay→Master.

Con ambos sends a −6 dB se observaron aproximadamente Drum 0,345, Music 0,504,
Plate 0,089, Delay 0,089 y Master 0,678. Cambiar el pre-send de Snare a −18 dB
durante Play redujo Plate a 0,022 y Master a 0,612; Send Mute dejó Plate a cero
sin afectar Delay.

Track Mute sobre Snare dejó su Track Meter a cero y retiró su main de Drum,
pero el pre-send continuó en Plate a 0,022. El control Track Gain aceptó cambios
durante Play; su separación respecto al pre-send se verifica además de forma
numérica en la suite portable. Solo Snare produjo dry+wet (Master≈0,199); Solo
Drum produjo solo Snare+Kick por la ruta main (Master≈0,345) y cerró Plate; Solo
Plate produjo wet-only (Master≈0,022, Track Meter de Snare a cero). Solo Snare +
Solo Plate formó la unión esperada. Mute de Plate anuló únicamente la rama wet.

Stop dejó `Stopped | 0.000 / 3.000 s`; replay comenzó desde cero y el final
natural quedó en `Stopped | 3.000 / 3.000 s`. La aplicación y el dispositivo se
cerraron sin error. La señal se verificó mediante meters y pruebas numéricas
offline; la sesión automatizada no captura la salida acústica.

## Riesgos y límites pendientes

- Los Bus Sends se representan y validan, pero 0.2.3 no los ejecuta.
- El ring de parámetros sigue siendo SPSC y admite un único productor.
- El smoothing de amplitud es lineal; Send Mute y Solo son discretos.
- Se mantiene un buffer estéreo completo por bus y procesamiento escalar.
- No hay meters por send, solo-safe, PFL/AFL ni multicanal.
- No existe limiter ni clamp; la suma puede superar `[-1, 1]`.
