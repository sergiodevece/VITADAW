# Validación local de VitaDAW 0.0.6 — Two-Track Playback

Fecha: 11 de septiembre de 2026  
Plataforma: macOS / CoreAudio

## Archivos WAV

- Pista 1: `vitadaw-track1-440hz-44100.wav`; PCM16 mono, `44100 Hz`,
  `2.000 s`, tono de `440 Hz`.
- Pista 2: `vitadaw-track2-660hz-48000.wav`; PCM16 mono, `48000 Hz`,
  `4.000 s`, tono de `660 Hz`.

Los tonos se generaron como artefactos locales de smoke test y son
auditivamente distinguibles. No forman parte del producto ni del repositorio.

## Dispositivo y proyecto

- Dispositivo: `Altavoces del MacBook Air`.
- Sample rate del dispositivo: `48000 Hz`.
- Sample rate del proyecto: `48000 Hz`.
- Buffer: `512 frames`.
- Canales de salida disponibles: `2`.

## Comportamiento observado

- Ambos selectores cargaron su WAV en el slot correcto.
- La duración pasó de `2.000 s` tras cargar la pista 1 a `4.000 s` tras cargar
  la pista 2.
- Play inició el reloj compartido y la mezcla estéreo.
- A `3.477 s`, después del final de la pista 1, el transporte seguía en
  `Playing`: la pista 1 ya aportaba silencio y la pista 2 continuaba.
- Al terminar la pista 2, el estado fue `Stopped | 4.000 / 4.000 s`.
- Play posterior al final reinició el reloj desde cero.
- Stop durante el segundo recorrido produjo `Stopped | 0.000 / 4.000 s`.
- La aplicación cerró limpiamente tras la prueba.

La presencia numérica de ambas señales y la suma con ganancia fija se verifican
además en el test offline. La sesión automatizada de UI no captura la salida
acústica del dispositivo.

## Builds y tests

- Build completo con JUCE: correcto.
- Build core-only, sin JUCE ni hardware: correcto.
- Tests: `6/6` correctos en ambos builds.
- La prueba prolongada simula seis horas con proyecto a `48000 Hz` y dispositivo
  a `44100 Hz`; termina en el frame de proyecto exacto y las dos posiciones
  fuente representan el mismo instante sin deriva relativa observable.
