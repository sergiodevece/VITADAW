# VitaDAW 0.1.0 — N-Track Audio Engine

Base arquitectónica para un DAW nativo de escritorio, construida de forma
incremental. La aplicación actual abre una ventana mínima, inicializa y observa
el dispositivo de audio y carga y reproduce una colección variable de pistas WAV
sincronizadas. El incremento 0.1.0 sustituye la topología fija de dos pistas por
un motor N-track portable con un único reloj maestro.

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
rates distintos y un futuro inicio de clip desplazado. Cada contribución se suma
a estéreo con una ganancia provisional fija de `0.125`, independiente del número
de pistas cargadas.

La ventana muestra también, de forma provisional, el estado, posición, duración
y sample rate lógico del proyecto. Al llegar al final natural, el transporte
queda en `Stopped` y conserva la posición en el final; `Stop` explícito continúa
rebobinando a cero.

`RealtimeAudioEngine::processBlock` contiene el reloj, la cola de comandos, el
recorrido del proyecto preparado, el render por pista, la acumulación y la
publicación del transporte. No depende de JUCE. `JuceAudioDeviceAdapter`
conserva la adaptación del dispositivo, la decodificación WAV y el ownership de
los buffers y de la topología preparada.

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

Las validaciones están registradas en [`docs/validation-0.0.2.md`](docs/validation-0.0.2.md),
[`docs/validation-0.0.3.md`](docs/validation-0.0.3.md) y
[`docs/validation-0.0.4.md`](docs/validation-0.0.4.md) y
[`docs/validation-0.0.6.md`](docs/validation-0.0.6.md) y
[`docs/validation-0.0.7.md`](docs/validation-0.0.7.md) y
[`docs/validation-0.0.8.md`](docs/validation-0.0.8.md) y
[`docs/validation-0.0.9.md`](docs/validation-0.0.9.md) y
[`docs/validation-0.1.0.md`](docs/validation-0.1.0.md).
