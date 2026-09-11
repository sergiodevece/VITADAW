# Validación local de VitaDAW 0.0.8 — Lifecycle and Transaction Hardening

Fecha: 11 de septiembre de 2026  
Plataforma: macOS / CoreAudio / AppleClang 21.0.0

## Builds y tests

- Build completo con JUCE 9.0.2: correcto.
- Build core-only, sin JUCE ni hardware: correcto.
- Tests en build completo y core-only: `8/8` correctos.
- AddressSanitizer + UndefinedBehaviorSanitizer, core-only Debug: `8/8`, sin
  diagnósticos.
- ThreadSanitizer, core-only Debug: `8/8`, sin carreras notificadas.

Los tests de lifecycle usan la misma máquina de estados portable que consume el
adaptador JUCE. Cubren objeto retenido sin consumo, `stopped`, `error`,
re-registro, reinicialización fallida, cancelación por generación y 100 carreras
reales entre el productor de Play y la transición a detenido. Toda secuencia
aceptada queda ejecutada o cubierta por el watermark publicado de cancelación.

Los tests de orden comprueban el estado resultante de `Play -> Stop`,
`Stop -> Play`, secuencias consecutivas, wrap-around del anillo y comandos de una
generación cancelada. No se limitan a comparar el máximo de secuencia.

La integración de carga usa recursos con destructor observable. Verifica carga,
sustitución, error, excepción recuperable, cierre y destrucción fuera de RT. Un
slot inválido fuerza un fallo de preparación del `ProjectState` después de haber
preparado el recurso; el commit del motor no se ejecuta y ambos lados conservan
el recurso anterior.

El mismo `RealtimeAudioEngine::processBlock` usado en producción reproduce
recursos de 1, 2 y 3 frames con ratios 48/44,1 kHz, 44,1/48 kHz y 96/48 kHz.
Cada caso verifica contenido, silencio restante, final natural y posición final
en el límite exclusivo.

## Smoke test real

- Aplicación: VitaDAW 0.0.8, build completo.
- Dispositivo: `Altavoces del MacBook Air`.
- Sample rate de dispositivo y proyecto: `48000 Hz`.
- Buffer: `512 frames`.
- Entradas disponibles: `0`; salidas disponibles: `2`.
- Pista 1: `vitadaw-track1-440hz-44100.wav`, PCM16 mono, `44100 Hz`, `2.000 s`.
- Pista 2: `vitadaw-track2-660hz-48000.wav`, PCM16 mono, `48000 Hz`, `4.000 s`.

Ambos WAV se cargaron y el dispositivo permaneció activo después de cada
retirada/re-registro del callback. Play avanzó el transporte; Stop lo dejó en
`0.000 / 4.000 s`. Una reproducción completa terminó de forma natural en
`Stopped | 4.000 / 4.000 s`. La aplicación y el dispositivo cerraron limpiamente.

## Riesgos deliberadamente pendientes

Se mantienen los límites ya aceptados: cola SPSC con un único productor,
decodificación síncrona y archivo completo en memoria, interpolación lineal,
dos pistas fijas y mezcla `0.5 + 0.5`. No se añaden N pistas, mixer general,
routing, streaming, plugins ni nuevas funciones visibles.
