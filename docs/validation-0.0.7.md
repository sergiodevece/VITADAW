# Validación local de VitaDAW 0.0.7 — Realtime Foundation Hardening

Fecha: 11 de septiembre de 2026  
Plataforma: macOS / CoreAudio / AppleClang 21.0.0

## Builds y tests

- Build completo JUCE: correcto.
- Build core-only sin JUCE ni hardware: correcto.
- Tests en ambos builds: `8/8` correctos.
- AddressSanitizer + UndefinedBehaviorSanitizer, core-only Debug: `8/8`, sin
  diagnósticos.
- ThreadSanitizer, core-only Debug: `8/8`, sin carreras notificadas.

Los tests añadidos ejercitan el snapshot con escritor y lector concurrentes, el
anillo SPSC lleno y tras wrap-around, orden y cancelación por lifecycle, el mismo
`processBlock` que usa producción, bloques variables, una y dos pistas, final a
mitad de bloque, sustitución quiescente y validación de tamaños, memoria y
muestras no finitas. Las conversiones de duración incluyen recursos de uno,
dos y tres frames y ratios 44,1/48/96 kHz.

## Smoke test real

- Dispositivo: `Altavoces del MacBook Air`.
- Sample rate de dispositivo y proyecto: `48000 Hz`.
- Buffer: `512 frames`.
- Salidas disponibles: `2`.
- Pista 1: `vitadaw-track1-440hz-44100.wav`, PCM16 mono, `44100 Hz`, `2.000 s`.
- Pista 2: `vitadaw-track2-660hz-48000.wav`, PCM16 mono, `48000 Hz`, `4.000 s`.

La aplicación arrancó limpia con el dispositivo activo. Ambos archivos se
cargaron en sus slots; Play avanzó el transporte desde cero y el final natural
quedó en `Stopped | 4.000 / 4.000 s`. Play posterior al final reinició desde
cero. Stop dejó `Stopped | 0.000 / 4.000 s` y el cierre fue limpio.

La automatización de UI no captura la señal acústica ni se muestreó exactamente
en el instante de dos segundos. La presencia simultánea de las dos señales, el
silencio de la pista corta mientras continúa la larga y las conversiones de
sample rate se verifican numéricamente ejecutando offline el mismo
`RealtimeAudioEngine::processBlock` usado por producción.

## Garantías y límites

El render propio no reserva, bloquea, hace I/O ni destruye recursos. El snapshot
usa únicamente atomics declarados siempre lock-free. Esta garantía no se extiende
a todo `JUCE AudioDeviceManager`, que internamente usa sincronización y puede
ajustar almacenamiento temporal en su ruta de callback.

Continúan deliberadamente fuera de alcance: N pistas, mixer general, streaming,
resampling de calidad final, routing, plugins, edición y nuevas funciones de UI.
