# Validación VitaDAW 0.1.1 — Mixer Core

Fecha: 11 de septiembre de 2026.

## Alcance validado

- Gain, pan, mute y solo por pista mediante comandos portables.
- Gain master posterior a la acumulación estéreo.
- Proyecto N-track y reloj maestro existentes sin cambios semánticos.
- Parámetros DSP preparados fuera de RT y publicados por una cola SPSC acotada.
- UI JUCE provisional con controles para cuatro pistas y master.

## Política DSP

- Gain de pista y master: `-100 dB` a `+12 dB`; `0 dB` es unity y
  `-100 dB` se prepara como cero lineal.
- Mono: pan equal-power, con `sqrt(1/2)` por canal al centro.
- Estéreo: balance equal-power; centro a unity y atenuación seno/coseno del
  canal opuesto al mover hacia un extremo.
- Sin solos contribuyen todas las pistas no muteadas. Con solos, solo
  contribuyen las pistas en solo y no muteadas. Mute prevalece sobre solo.
- La suma y el master usan `float`, sin clamp ni limiter. Se admite superar
  `[-1, 1]` internamente.
- No hay smoothing en 0.1.1; un salto de gain o pan puede producir zipper noise.

## Builds y tests automatizados

Build completo JUCE Debug:

```sh
cmake -S . -B build -DVITADAW_BUILD_APP=ON -DVITADAW_BUILD_TESTS=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Resultado: **11/11 tests superados**.

Build core-only:

```sh
cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF -DVITADAW_BUILD_TESTS=ON
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

Resultado: **11/11 tests superados**.

`MixerCoreTests` usa el `RealtimeAudioEngine::processBlock` de producción y
comprueba conversiones de gain, rechazo de NaN/infinito/fuera de rango, pan
mono y estéreo, extremos e intermedios, conservación de potencia mono,
mute/unmute con reloj activo, solo individual y múltiple, mute+solo, gain
master, cola llena y wrap-around. Ejecuta además 1, 2, 4, 8 y 32 pistas con
parámetros distintos, entradas vacías y sample rates 44,1/48 kHz. Los tests
anteriores continúan cubriendo transporte, final global, lifecycle, snapshots,
recursos cortos, carga transaccional y lifetime.

## Sanitizers

- ASan + UBSan combinados, core-only Debug: **11/11**, sin diagnósticos.
- UBSan independiente, core-only Debug: **11/11**, sin diagnósticos.
- TSan, core-only Debug: **11/11**, sin carreras reportadas.

## Smoke test macOS

Aplicación: `build/vitadaw_app_artefacts/Debug/VitaDAW.app`.

Dispositivo observado:

- salida: `Altavoces del MacBook Air`;
- sample rate de dispositivo/proyecto: `48000 Hz`;
- buffer: `512 frames`;
- entradas disponibles: `0`;
- salidas disponibles: `2`.

WAV mono PCM16 usados:

- `vitadaw-ntrack-330hz-44100-1s.wav`: 44,1 kHz, 1 s;
- `vitadaw-ntrack-440hz-48000-2s.wav`: 48 kHz, 2 s;
- `vitadaw-ntrack-550hz-44100-3s.wav`: 44,1 kHz, 3 s;
- `vitadaw-ntrack-660hz-48000-4s.wav`: 48 kHz, 4 s.

La UI confirmó las cuatro cargas y duración global de 4 s. Durante ejecución se
probaron gain de pista a -12 y +6 dB, pan a ambos extremos, mute, solo individual,
solo activado antes de cargar una pista, varios solos, mute sobre una pista en
solo y gain master a -6 y +3 dB. El transporte alcanzó de forma
natural `Stopped | 4.000 / 4.000 s`; Play posterior reinició desde cero y Stop
explícito dejó `Stopped | 0.000 / 4.000 s`. La aplicación cerró limpiamente y el
proceso dejó de estar activo. Los tonos son distinguibles por frecuencia, aunque
esta sesión no dispone de captura acústica para adjuntar evidencia de escucha;
la contribución y los coeficientes se validaron numéricamente offline.

## Límites conscientes

- Sin smoothing, cambios abruptos pueden producir discontinuidades audibles.
- Ring de parámetros SPSC: 64 entradas, 63 pendientes utilizables y un
  productor; el llenado se rechaza de
  forma explícita sin mutar el modelo.
- Capacidad RT preparada actual: 256 pistas; no es una limitación del modelo de
  proyecto, pero una preparación que la exceda se rechaza.
- Sin buses, sends, inserts, plugins, automatización, grabación, edición,
  routing configurable ni medidores gráficos.
