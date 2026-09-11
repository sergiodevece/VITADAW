# Validación local de VitaDAW 0.1.0 — N-Track Audio Engine

Fecha: 11 de septiembre de 2026  
Plataforma: macOS / CoreAudio / AppleClang 21.0.0

## Builds y pruebas

- Build completo con JUCE 9.0.2: correcto.
- Build core-only, sin JUCE ni hardware: correcto.
- Tests en build completo y core-only: `10/10` correctos.
- AddressSanitizer + UndefinedBehaviorSanitizer, core-only Debug: `10/10`, sin
  diagnósticos.
- ThreadSanitizer, core-only Debug: `10/10`, sin carreras notificadas.
- `git diff --check`: correcto.

Las pruebas cubren proyectos con cero, una, dos, cuatro, ocho y treinta y dos
pistas; entradas vacías entre pistas activas; señales y sample rates distintos;
duraciones desiguales; tamaños de bloque variables; Stop, final natural y Play
posterior al final. Los casos de una, dos, cuatro, ocho y treinta y dos pistas
ejecutan el mismo `RealtimeAudioEngine::processBlock` portable que usa JUCE.

La prueba funcional de 32 pistas utiliza señales sintéticas pequeñas y confirma
la acumulación esperada hasta el final global. No pretende medir capacidad de
producción. Se conservan las pruebas de seis horas sin deriva, recursos de 1–3
frames, lifecycle, FIFO, snapshots, transacción de carga y lifetime frente a una
región RT activa.

## Smoke test real de cuatro pistas

- Aplicación: VitaDAW 0.1.0, build completo Debug.
- Dispositivo: `Altavoces del MacBook Air`.
- Sample rate de dispositivo y proyecto: `48000 Hz`.
- Buffer: `512 frames`.
- Entradas disponibles: `0`; salidas disponibles: `2`.
- Pista 1: `vitadaw-ntrack-330hz-44100-1s.wav`, PCM16 mono, `44100 Hz`, `1 s`.
- Pista 2: `vitadaw-ntrack-440hz-48000-2s.wav`, PCM16 mono, `48000 Hz`, `2 s`.
- Pista 3: `vitadaw-ntrack-550hz-44100-3s.wav`, PCM16 mono, `44100 Hz`, `3 s`.
- Pista 4: `vitadaw-ntrack-660hz-48000-4s.wav`, PCM16 mono, `48000 Hz`, `4 s`.

Las cuatro cargas fueron aceptadas y la duración aumentó sucesivamente hasta
cuatro segundos. Play mantuvo el transporte activo después del final de la pista
de un segundo. Stop dejó `Stopped | 0.000 / 4.000 s`. La reproducción completa
terminó en `Stopped | 4.000 / 4.000 s`; Play desde ese estado reinició en cero y
la UI mostró `0.555 s` al tomar la observación posterior. La
aplicación y el dispositivo cerraron limpiamente. La presencia y suma
de todas las contribuciones se verificó numéricamente offline; no se capturó la
salida acústica.

## Límites conocidos

- Cada pista contiene todavía como máximo un clip y la interfaz provisional
  crea cuatro pistas al abrir la aplicación.
- La ganancia fija por pista es `0.125`. No depende del número de pistas y deja
  margen para ocho señales normalizadas; más contribuciones pueden superar la
  unidad porque todavía no hay gain staging ni limitador.
- El render recorre todas las pistas por cada frame. La prueba de 32 pistas
  demuestra independencia funcional de la topología anterior, no rendimiento
  profesional.
- Los WAV se decodifican de forma síncrona y completa en memoria y continúan
  usando interpolación lineal provisional.

No se añadieron faders, pan, mute, solo, buses, sends, inserts, plugins,
automatización, grabación, edición, waveform ni MIDI.
