# Validación local de VitaDAW 0.0.4 — First Sound

Fecha: 11 de septiembre de 2026  
Plataforma: macOS / CoreAudio

## Archivo WAV

- Archivo: `NO KEBAB_MASTER_DIGITAL.wav`
- Codificación: PCM lineal de 24 bits
- Sample rate: `44100 Hz`
- Canales: `2`
- Duración: `197.000 s`

## Dispositivo

- Dispositivo de salida: `Altavoces del MacBook Air`
- Sample rate: `48000 Hz`
- Tamaño de buffer: `512 frames`
- Canales de salida disponibles: `2`

La diferencia 44,1 → 48 kHz activa el avance fraccional y la interpolación
lineal dentro del callback. La decodificación y la reserva del buffer completo
ocurren antes de publicarlo al callback.

## Comportamiento observado

- Play sin archivo fue rechazado explícitamente.
- El selector cargó el WAV mediante `LoadAudioFile`.
- Play fue aceptado y envió el contenido al dispositivo activo.
- Stop fue aceptado y rebobinó al inicio.
- Un segundo Play fue aceptado desde el inicio.
- La aplicación cerró limpiamente después de detener el transporte.

La ruta de audio perceptual no se captura automáticamente; la validación
automatizada cubre las transiciones, el cursor y el ratio de sample rates.

## Validación de tiempo y fin natural

- Sample rate lógico del proyecto observado: `48000 Hz`, fijado a partir del
  dispositivo activo al abrir la aplicación.
- La ventana mostró avance del transporte durante reproducción, expresado en
  segundos derivados de frames de proyecto.
- `Stop` mantuvo la política de posición cero.
- El fin natural se valida de forma determinista sin hardware: el cursor queda
  en el frame final, publica `playing = false` y la aplicación adopta
  `Stopped` conservando la posición final.
- Las conversiones `44.1 kHz ↔ 48 kHz`, segundos ↔ frames de proyecto y una
  duración de seis horas pasaron sin drift de frames observable.

## Builds y tests

- Build completo con JUCE: correcto.
- Build `VITADAW_BUILD_APP=OFF`: correcto; confirma que el núcleo no depende de
  JUCE ni de un dispositivo físico.
- Tests: `5/5` correctos en ambos builds.
