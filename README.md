# VitaDAW 0.2.4 — Bus Sends

Base arquitectónica para un DAW nativo de escritorio, construida de forma
incremental. La aplicación actual abre una ventana mínima, inicializa y observa
el dispositivo de audio y carga y reproduce una colección variable de pistas WAV
sincronizadas. El incremento 0.2.0 sustituye la suma directa al master por un
plan portable preparado con buses estéreo, destinos de pista y metering de bus.
El incremento 0.2.1 convierte esos buses en canales funcionales con gain,
balance, mute, solo y smoothing sample-accurate. VitaDAW 0.2.2 permite que la
salida principal de un bus alimente otro bus mediante un DAG validado y ordenado
completamente fuera del hilo de audio. VitaDAW 0.2.3 añade múltiples sends por
pista hacia buses existentes, con taps pre/post, nivel suavizado, mute propio y
Solo resuelto por rama. VitaDAW 0.2.4 activa además sends cuyo origen es un bus,
los integra en el DAG y conserva un único procesamiento de cada bus aunque tenga
varias ramas.

El proyecto mantiene ahora una escala temporal explícita. Su sample rate se fija
al crear el proyecto: usa el del dispositivo activo y, si la apertura falla,
usa `48000 Hz` como valor de reserva. No cambia automáticamente si después se
reconfigura el dispositivo.

## Tecnología propuesta

- **C++20** para el núcleo y el callback de audio.
- **CMake** para builds reproducibles y separación por targets.
- **JUCE 9.0.2** como adaptador para ventana y dispositivo de audio. CMake lo
  descarga de su repositorio oficial si no encuentra una instalación local.

JUCE no aparece en el modelo del proyecto ni en el sistema de comandos. Así se
puede probar el comportamiento sin hardware y sustituir un adaptador sin
reescribir el dominio.

## Licencia de JUCE 9

JUCE 9 usa un modelo de licencia dual. Sus módulos pueden utilizarse bajo:

- **AGPLv3**, cumpliendo todas sus condiciones; o
- la **JUCE 9 EULA**, con uno de sus niveles Starter, Indie, Pro o Educational.

A fecha de 11 de septiembre de 2026, JUCE publica Starter gratuito con límite
de ingresos o financiación de 20.000 USD, Indie con límite de 300.000 USD, Pro
sin límite y Educational gratuito sujeto a requisitos y sin uso comercial. Los
niveles, límites, precios, requisitos de distribución y duración deben
comprobarse en la [página oficial de licencias](https://juce.com/get-juce/) y en
la [EULA de JUCE 9](https://juce.com/legal/juce-9-licence/) antes de distribuir.

Este proyecto no adopta todavía una opción de licencia para su distribución.
La integración técnica de JUCE no debe interpretarse como esa decisión.

## Build actual

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

La primera configuración descarga JUCE 9.0.2 si no está instalado. Para
compilar únicamente el núcleo y sus tests, sin JUCE:

```sh
cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

## Ejecución

En macOS:

```sh
open build/vitadaw_app_artefacts/VitaDAW.app
```

En Windows y Linux, ejecuta `VitaDAW` desde el directorio de artefactos que
muestre CMake al terminar el build.

La ventana muestra provisionalmente el dispositivo de salida activo, sample
rate, tamaño de buffer y canales de entrada/salida disponibles. Si el dispositivo
no puede abrirse, conserva un estado de error coherente y muestra el diagnóstico
fuera del callback de audio.

El motor distingue explícitamente `unavailable`, `initializing`, `operational`,
`stopped` y `error`. `audioDeviceAboutToStart()` solo conduce a
`initializing`: VitaDAW exige además callback registrado y `isPlaying()` del
dispositivo, o la entrada efectiva en `processBlock`, antes de permitir Play.
Retirar y volver a registrar el callback no basta por sí solo para declarar el
dispositivo operativo.

La aceptación usa un gate atómico único que contiene estado y generación. El
productor reclama temporalmente el gate, reserva y escribe el comando todavía
invisible, y lo acepta mediante un CAS final. Ese CAS es el punto de
linearización; el índice de cola se publica después. El cierre de lifecycle
compite sobre el mismo gate: si queda después de la aceptación, su watermark
incluye la secuencia; si queda antes, el CAS de aceptación falla.

Los cuatro botones provisionales de carga aceptan únicamente archivos `.wav`
que `juce::WavAudioFormat` pueda decodificar. La interfaz crea cuatro pistas al
arrancar para el smoke test; el dominio y el motor admiten una colección
variable. `Play` reproduce simultáneamente todas las pistas disponibles y
`Stop` detiene y vuelve al inicio. Si ninguna pista
tiene un WAV válido preparado, `Play` se rechaza explícitamente.

Las pistas tienen un `TrackId` monotónico independiente de su posición en el
vector y no tienen relojes propios. Un único reloj de frames de proyecto
determina en cada muestra la posición fuente de cada WAV, incluyendo sample
rates distintos y un futuro inicio de clip desplazado.

Cada pista conserva gain de `-100 dB` (silencio) a `+12 dB`, pan normalizado de
`-1` a `+1`, mute y solo. La UI solo envía `SetTrackGain`, `SetTrackPan`,
`SetTrackMute`, `SetTrackSolo` y `SetMasterGain`; no toca el estado RT. El pan
mono usa ley equal-power con `-3 dB` al centro. Para fuentes estéreo, el control
actúa como balance equal-power: el centro conserva ambos canales a unity y cada
extremo atenúa el canal opuesto.

Cada pista puede conservar varios `SendRoute` además de su salida principal.
`SendId` es monotónico y estable; el origen portable admite `TrackId` o `BusId`,
y ambos tipos se ejecutan en 0.2.4. Cada send termina en un `BusId`: el mismo bus
puede recibir outputs principales, Track Sends y Bus Sends, por lo que no existe
una clase Aux distinta.

El tap `PreFaderPrePan` se extrae tras render y antes de gain, pan y mute. Mono
se centra una sola vez a `x/sqrt(2)` por canal y estéreo conserva L/R. El tap
`PostFaderPostPan` incluye gain, pan y mute. Después se aplican el permiso Solo
de la rama, el mute propio y el nivel del send. El pre-send sobrevive a Track
Mute y al fader en silencio por política explícita de esta versión.

En un bus, el tap pre se extrae una sola vez tras acumular todas sus entradas y
antes de Bus Gain, Balance y Mute. El tap post se obtiene una sola vez después
de esos controles. Los sends pre/post y la salida principal distribuyen esos
dos valores ya calculados; añadir fan-out no reprocesa el bus ni vuelve a
avanzar sus smoothers o meter. Bus Mute y Bus Gain a −100 dB silencian main y
post-send, pero el pre-send sigue disponible. El Bus Meter mide únicamente la
señal post-fader/post-mute del canal principal y nunca suma sends.

Si no hay solos, todas las rutas quedan abiertas y cada mute actúa localmente.
Si existe al menos un solo, la aplicación prepara los permisos de las ramas
seleccionadas y de sus caminos necesarios; mute siempre tiene precedencia
local. El gain master usa el mismo rango y se aplica después de la suma.
No existe ya atenuación fija por pista, clamp ni limiter: `float` puede superar
`[-1, 1]` internamente y una salida física puede recortar si se excede su rango.

Los valores dB y coeficientes trigonométricos de pan se convierten fuera del
callback. Un ring SPSC acotado de 64 entradas (63 pendientes utilizables)
publica estados DSP completos y el motor los aplica al inicio de bloque sobre
almacenamiento preasignado. El estado global de solo incluye también pistas
vacías. Una cola llena rechaza el comando explícitamente y el modelo no cambia.

Gain y coeficientes de pan por pista, y gain master, usan rampas lineales de
5 ms avanzadas por frame de dispositivo. Su duración no depende del tamaño del
bloque y un nuevo target parte del valor instantáneo. Mute y solo continúan
siendo discretos. El smoothing de gain ocurre en amplitud lineal: es económico,
alcanza silencio exactamente y no representa una velocidad perceptual constante
en dB. El paneo interpola los coeficientes equal-power ya preparados, evitando
trigonometría por muestra; los endpoints cumplen exactamente la pan law, aunque
el trayecto transitorio no conserva potencia exacta.

Cada Send Level usa el mismo rango de `-100 dB` a `+12 dB` y una rampa lineal
sample-accurate de 5 ms. El smoother avanza una vez por frame aunque el send
esté muteado, excluido por Solo o reciba silencio. `SetSendLevel` y
`SetSendMute` se publican por el ring SPSC durante Play; alta y eliminación son
cambios estructurales y exigen transporte detenido.

Los Bus Sends reutilizan exactamente ese estado y cada uno mantiene su propio
smoother. `AddBusSend` conserva tipado explícito sin alterar `AddTrackSend`;
`SetSendRoute` permite cambiar destino/tap únicamente con el transporte parado.
`RemoveSend`, `SetSendLevel` y `SetSendMute` siguen operando por `SendId` para
ambos orígenes.

El motor calcula peak absoluto por bloque para cada pista después de gain, pan y
mute/solo, y para master después del gain master. No hace clamp, por lo que el
meter puede indicar valores superiores a 1. Los snapshots RT se publican en
atomics lock-free y la UI realiza lecturas acotadas sin bloquear el audio. Son
telemetría latest-value: no se conserva cada bloque y una lectura concurrente
fallida devuelve un snapshot vacío coherente. RMS, decay y peak hold quedan para
una versión futura.

`RoutingState` es la única fuente editable de las conexiones. `OutputDestination`
representa Master o un `BusId` estable y monotónico. Cada pista y cada bus tienen
exactamente un destino principal. Los buses son nodos estéreo de acumulación
independientes de WAV, clips y sample rates fuente; Master es el terminal único.

Los cambios estructurales pasan por comandos y solo se aceptan con el transporte
parado. `ProjectState` y `RoutingState` producen una especificación candidata;
el compilador portable valida identidades, destinos, límites y memoria, resuelve
índices densos y crea `PreparedProcessingPlan` junto a `ProcessingPlanRuntime`.
El commit retira el callback, intercambia modelo, plan, buffers y recursos como
una transacción, y vuelve a registrar el consumidor. Un fallo conserva el
proyecto anterior completo.

El plan fija el orden Tracks → buses en orden topológico → Master. Cada pista se
renderiza una sola vez por subbloque. Cada bus se procesa una sola vez después
de recibir todas sus entradas, se mide y entrega su salida a otro bus o Master. Una
capacidad interna preparada de 512 frames divide callbacks mayores sin perder
continuidad de reloj, smoothing ni máximos de metering. Toda reserva, resolución
de IDs y construcción topológica ocurre fuera de RT.

El DAG incluye outputs principales y Bus Sends, incluso si una rama está muteada
o a −100 dB. Para topología, las aristas paralelas origen/destino se deduplican;
el plan DSP conserva todos sus descriptors y suma todas las contribuciones. Cada
descriptor resuelve fuera de RT tipo/índice de origen, bus y buffer de destino,
tap, smoother y permiso de audibilidad, agrupado en rangos pre/post por pista o
bus.

La ventana muestra también, de forma provisional, el estado, posición, duración
y sample rate lógico del proyecto. Al llegar al final natural, el transporte
queda en `Stopped` y conserva la posición en el final; `Stop` explícito continúa
rebobinando a cero.

`RealtimeAudioEngine::processBlock` contiene el reloj, la cola de comandos, el
recorrido del proyecto preparado, el render por pista, la acumulación y la
publicación del transporte. No depende de JUCE. `JuceAudioDeviceAdapter`
conserva la adaptación del dispositivo, la decodificación WAV y el ownership de
los buffers y de la topología preparada.

Cada bus conserva ahora un `BusMixState` portable con gain de −100 a +12 dB,
balance estéreo de −1 a +1, mute y solo. Sus coeficientes DSP se preparan fuera
de RT. Gain y balance usan las mismas rampas de 5 ms que las pistas; mute y solo
son discretos. El flujo es acumulación → gain/balance → mute/solo → bus meter →
Master, de modo que un bus muteado o excluido por Solo marca cero.

Solo se resuelve separando selección y camino audible. Sin solos quedan abiertas
todas las pistas y buses. Un Track Solo abre esa pista y el bus que necesita;
un Bus Solo selecciona todo el contenido upstream que alcanza ese bus. En ambos
casos se abre después el camino downstream necesario hasta Master sin seleccionar
ramas hermanas. Varios solos forman la unión de las selecciones. Mute prevalece
localmente. La aplicación publica las máscaras densas junto al parámetro, sin
búsquedas de IDs ni interpretación del grafo en el callback.

Con sends, la resolución prepara permisos independientes para el main output de
cada pista y para cada send. Track Solo conserva su rama dry y sus auxiliares;
Bus Solo abre únicamente las rutas que lo alimentan, sin abrir sends laterales;
Aux Solo produce una escucha wet-only. Las selecciones múltiples forman una
unión. Después de converger en un bus se procesa una mezcla común: no se intenta
preservar la procedencia individual de las contribuciones.

Con Bus Sends, «contenido completo necesario» y «salida Main audible» son
permisos diferentes. Un bus en Solo abre su main y sus sends propios; un bus
abierto solo como transporte downstream no hereda sus sends laterales. Un Aux
en Solo abre todas sus aristas de entrada —Track o Bus Sends—, prepara el
contenido upstream imprescindible y conserva cerrados los dry paths paralelos.

La carga tiene dos fases. `prepareWav` realiza y captura fuera de RT cualquier
operación que puede fallar; `ProjectState` prepara también el nuevo clip y el
mensaje. El commit detiene el callback, intercambia el recurso, ejecuta el
commit portable del modelo —solo swaps y asignaciones `noexcept`— y después
reconecta el callback. El estado final contiene el recurso nuevo en ambos lados
o conserva el anterior en ambos.

La topología RT es un `span` inmutable sobre vistas construidas fuera del
callback. Cargar o sustituir un WAV prepara primero una copia completa del
proyecto RT; el commit intercambia su owner sin reservar memoria mientras el
render está activo. La duración global es el máximo final de todos los clips.

La preparación admite WAV mono o estéreo y rechaza sample rates o muestras no
finitos, tamaños con overflow y preparaciones que superarían un presupuesto
provisional total de 512 MiB. Las duraciones representan límites exclusivos y
se redondean hacia arriba; incluso un recurso de un frame conserva duración.

La arquitectura y las reglas de tiempo real se describen en
[`docs/architecture.md`](docs/architecture.md).
## Historial de incrementos

- **0.0.2:** aplicación JUCE y dispositivo básico.
- **0.0.3:** dispositivo observable y robusto.
- **0.0.4 — First Sound:** primera reproducción WAV.
- **0.0.5:** tiempo de proyecto y transporte sincronizado con el hilo RT.
- **0.0.6 — Two-Track Playback:** dos pistas desde un único reloj maestro.
- **0.0.7 — Realtime Foundation Hardening:** snapshot coherente, lifecycle
  determinista, duración exclusiva y `processBlock` portable.
- **0.0.8 — Lifecycle and Transaction Hardening:** disponibilidad real del
  consumidor, cancelación concurrente y carga WAV transaccional.
- **0.0.9 — Concurrency Closure:** admisión linealizable de comandos y lifetime
  de recursos verificado frente a regiones RT activas.
- **0.1.0 — N-Track Audio Engine:** colección variable con identidad estable,
  proyecto preparado RT, render por pista y acumulación general.
- **0.1.1 — Mixer Core:** gain, pan equal-power, mute/solo global y gain master
  con actualizaciones ligeras de parámetros hacia RT.
- **0.1.2 — Smooth Mixer & Metering:** rampas sample-accurate de gain/pan y
  peak metering portable por pista y master.
- **0.2.0 — Routing Foundation:** `RoutingState`, buses estéreo, destinos
  Track→Bus/Master y ejecución mediante un plan RT preparado.
- **0.2.1 — Bus Mixer Controls:** gain, balance, mute y solo de bus con
  smoothing y resolución portable de caminos audibles.
- **0.2.2 — Bus-to-Bus Routing DAG:** salida principal Bus→Bus/Master,
  validación de ciclos, orden topológico y Solo resuelto a través del grafo.
- **0.2.3 — Track Sends & Auxes:** sends múltiples Track→Bus con taps
  pre/post, level, mute, smoothing y audibilidad preparada por aristas.
- **0.2.4 — Bus Sends:** sends Bus→Bus audibles, taps pre/post de bus,
  integración completa en el DAG y Solo wet-only con permisos por arista.

Las validaciones están registradas en [`docs/validation-0.0.2.md`](docs/validation-0.0.2.md),
[`docs/validation-0.0.3.md`](docs/validation-0.0.3.md) y
[`docs/validation-0.0.4.md`](docs/validation-0.0.4.md) y
[`docs/validation-0.0.6.md`](docs/validation-0.0.6.md) y
[`docs/validation-0.0.7.md`](docs/validation-0.0.7.md) y
[`docs/validation-0.0.8.md`](docs/validation-0.0.8.md) y
[`docs/validation-0.0.9.md`](docs/validation-0.0.9.md) y
[`docs/validation-0.1.0.md`](docs/validation-0.1.0.md) y
[`docs/validation-0.1.1.md`](docs/validation-0.1.1.md) y
[`docs/validation-0.1.2.md`](docs/validation-0.1.2.md) y
[`docs/validation-0.2.0.md`](docs/validation-0.2.0.md) y
[`docs/validation-0.2.1.md`](docs/validation-0.2.1.md) y
[`docs/validation-0.2.2.md`](docs/validation-0.2.2.md) y
[`docs/validation-0.2.3.md`](docs/validation-0.2.3.md) y
[`docs/validation-0.2.4.md`](docs/validation-0.2.4.md).
