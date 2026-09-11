# Validación de VitaDAW 0.1.2 — Smooth Mixer & Metering

Fecha: 11 de septiembre de 2026.

## Alcance validado

Este incremento añade únicamente smoothing portable para gain y pan de pista y
gain master, además de peak metering por pista y master. No añade buses, sends,
inserts, plugins, automatización, grabación, edición, waveform, routing
configurable, MIDI ni limitador.

## Política de smoothing

- Rampa lineal de 5 ms, avanzada una vez por frame de dispositivo.
- Gain de pista y master se interpolan en amplitud lineal.
- Pan interpola los coeficientes mono y estéreo ya calculados fuera de RT.
- Un target nuevo parte del valor instantáneo alcanzado, no del target anterior.
- Mute y solo continúan siendo discretos y se aplican al límite de bloque.
- La duración usa `round(0.005 * deviceSampleRate)` frames; no depende del
  tamaño del bloque.

Los tests cubren bloques de 64, 128, 256, 512 y 1024 frames y sample rates de
44,1, 48 y 96 kHz. También cubren cambios de target durante una rampa.

## Metering

Cada pista se mide después de render, gain, pan y decisión mute/solo, y antes de
sumarse al master. El master se mide después de la suma y del gain master, antes
de copiar la señal a output.

La magnitud publicada es el peak absoluto por canal del último bloque, sin clamp.
Por ello una señal interna superior a 1,0 también produce un valor superior a
1,0. Cada lectura por pista conserva su `TrackId`.

`RealtimeMeterExchange` publica un snapshot latest-value mediante atomics
lock-free. El escritor RT usa una revisión impar/par y el lector hace como máximo
tres intentos; si coincide continuamente con una escritura, devuelve un snapshot
vacío coherente. No existe cola ni obligación de conservar cada bloque de
telemetría. La UI lee mediante `DawApplication` a 30 Hz y nunca accede directamente
al motor.

## Builds y tests

Configuración core-only:

```sh
cmake -S . -B build-core \
  -DVITADAW_BUILD_APP=OFF \
  -DVITADAW_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

Resultado: **12/12 tests superados**.

Configuración completa con JUCE:

```sh
cmake -S . -B build \
  -DVITADAW_BUILD_APP=ON \
  -DVITADAW_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Resultado: **12/12 tests superados** y aplicación nativa compilada.

La suite nueva verifica mediante el `processBlock` real:

- gain estable, rampas ascendentes y descendentes y retargeting;
- pan izquierda/derecha y retargeting durante la transición;
- gain master;
- equivalencia temporal entre tamaños de bloque;
- adaptación a 44,1, 48 y 96 kHz;
- silencio, señal positiva, negativa, seno conocido y estéreo desigual;
- mute, solo y varias pistas;
- master unity y atenuado;
- headroom interno por encima de 1,0;
- 1, 4, 8 y 32 pistas con identidad `TrackId`;
- lectura/escritura concurrente del exchange sin snapshots híbridos.

## Sanitizers

- ASan + UBSan: **12/12**, sin diagnósticos.
- UBSan independiente: **12/12**, sin diagnósticos.
- TSan: **12/12**, sin carreras detectadas.
- `git diff --check`: correcto.

## Smoke test

Dispositivo observado:

- salida: Altavoces del MacBook Air;
- sample rate de dispositivo/proyecto: 48.000 Hz;
- buffer: 512 frames;
- canales: 0 entradas / 2 salidas.

WAV utilizados:

- `vitadaw-ntrack-330hz-44100-1s.wav`: mono, 44,1 kHz, 1 s;
- `vitadaw-ntrack-440hz-48000-2s.wav`: mono, 48 kHz, 2 s;
- `vitadaw-ntrack-550hz-44100-3s.wav`: mono, 44,1 kHz, 3 s;
- `vitadaw-ntrack-660hz-48000-4s.wav`: mono, 48 kHz, 4 s.

Se verificó que los cuatro meters de pista y el master muestran actividad durante
Play. Se enviaron rápidamente varios targets de gain, pan y master; se comprobó
mute con meter de pista a cero y solo excluyendo las pistas no elegibles. El final
natural quedó en `Stopped | 4.000 / 4.000 s`; Play posterior reinició desde cero.
Stop explícito dejó `Stopped | 0.000 / 4.000 s` y todos los meters a cero. El
cierre fue limpio.

La sesión de validación no dispone de captura ni monitorización acústica, por lo
que no es posible afirmar de forma auditiva verificable si se percibieron clicks
o zipper noise. La continuidad se verificó numéricamente con rampas sample-accurate,
retargeting desde el valor instantáneo y equivalencia entre tamaños de bloque.

## Limitaciones conocidas

- La rampa de amplitud lineal no tiene velocidad perceptual constante en dB.
- La interpolación de coeficientes de pan es continua, pero durante la transición
  puede desviarse ligeramente de potencia constante; los endpoints sí respetan
  exactamente la pan law.
- El peak es instantáneo por bloque; no hay RMS, decay ni peak hold.
- La UI puede omitir bloques transitorios porque consume telemetría latest-value.
- El exchange realiza escrituras atómicas O(N) por bloque; es apropiado para este
  prototipo, pero deberá medirse antes de escalar a recuentos muy altos de pistas.
- No hay limiter ni clamp: la salida física puede recortar señales fuera de rango.

