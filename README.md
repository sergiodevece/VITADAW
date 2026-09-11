# VitaDAW 0.0.4 — First Sound

Base arquitectónica para un DAW nativo de escritorio, construida de forma
incremental. La aplicación actual abre una ventana mínima, inicializa y observa
el dispositivo de audio y carga y reproduce un único archivo WAV en una única
pista.

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

`Load WAV` acepta únicamente archivos `.wav` que `juce::WavAudioFormat` pueda
decodificar. `Play` inicia la reproducción, `Stop` detiene y vuelve al inicio, y
un nuevo `Play` reproduce otra vez desde el primer frame. Si no hay un WAV válido
preparado, `Play` se rechaza explícitamente.

La ventana muestra también, de forma provisional, el estado, posición, duración
y sample rate lógico del proyecto. Al llegar al final natural, el transporte
queda en `Stopped` y conserva la posición en el final; `Stop` explícito continúa
rebobinando a cero.

La arquitectura y las reglas de tiempo real se describen en
[`docs/architecture.md`](docs/architecture.md).
## Historial de incrementos

- **0.0.2:** aplicación JUCE y dispositivo básico.
- **0.0.3:** dispositivo observable y robusto.
- **0.0.4 — First Sound:** primera reproducción WAV.

Las validaciones están registradas en [`docs/validation-0.0.2.md`](docs/validation-0.0.2.md),
[`docs/validation-0.0.3.md`](docs/validation-0.0.3.md) y
[`docs/validation-0.0.4.md`](docs/validation-0.0.4.md).
