# Validación local de VitaDAW 0.0.9 — Concurrency Closure

Fecha: 11 de septiembre de 2026  
Plataforma: macOS / CoreAudio / AppleClang 21.0.0

## Builds y tests

- Build completo con JUCE 9.0.2: correcto.
- Build core-only, sin JUCE ni hardware: correcto.
- Tests en build completo y core-only: `10/10` correctos.
- AddressSanitizer + UndefinedBehaviorSanitizer, core-only Debug: `10/10`, sin
  diagnósticos.
- ThreadSanitizer, core-only Debug: `10/10`, sin carreras notificadas.
- `git diff --check`: correcto.

`CommandLifecycleGateTests` fuerza con semáforos los interleavings situados antes
y después de la reserva de secuencia y del punto de aceptación. Incluye el
contraejemplo original: el productor reclama la generación, lifecycle la cierra
y solo después el productor intenta reservar y aceptar. El resultado es rechazo,
nunca una solicitud aceptada fuera del watermark. También verifica cierre tras
la reserva, cierre tras la aceptación, aceptación en una generación posterior y
rechazo por cola llena sin consumir una secuencia.

Se conservan además las carreras repetidas Play/lifecycle, el orden FIFO, el
wrap-around del anillo, generaciones y cancelaciones. La generación y la
secuencia no envuelven sus dominios lógicos: al agotarse se rechaza operación
nueva de manera determinista.

`RealtimeResourceLifetimeTests` utiliza el `RealtimeAudioEngine` real, vistas y
recursos vectoriales reales, un destructor instrumentado y una región RT
controlada. Sustitución y cierre esperan a que esa región quede quiescente; el
último owner se libera después y el test falla si el destructor coincide con un
usuario RT. También comprueba que un candidato fallido no sustituye el recurso
publicado, que este sigue renderizando y que ambos recursos se destruyen en el
momento correcto. La instrumentación existe solo en el test.

## Smoke test real

- Aplicación: VitaDAW 0.0.9, build completo.
- Dispositivo: `Altavoces del MacBook Air`.
- Sample rate de dispositivo y proyecto: `48000 Hz`.
- Buffer: `512 frames`.
- Entradas disponibles: `0`; salidas disponibles: `2`.
- Pista 1: `vitadaw-track1-440hz-44100.wav`, PCM16 mono, `44100 Hz`, `2.000 s`.
- Pista 2: `vitadaw-track2-660hz-48000.wav`, PCM16 mono, `48000 Hz`, `4.000 s`.

Ambos archivos se cargaron correctamente y Play inició la mezcla. Al terminar la
pista de dos segundos, el proyecto continuó con la pista larga. El final natural
quedó en `Stopped | 4.000 / 4.000 s`; Play desde ese estado reinició desde cero y
Stop dejó `Stopped | 0.000 / 4.000 s`. La aplicación y el dispositivo cerraron
limpiamente.

## Riesgos residuales directamente relacionados

- La cola continúa siendo SPSC: solo un productor puede reclamar el gate. Una
  futura pluralidad de productores deberá serializarse antes de esta frontera o
  requerirá otro protocolo.
- El contador de secuencia rechaza nuevas solicitudes al alcanzar su máximo, en
  lugar de envolver y crear una ambigüedad ABA. El gate entra en error terminal
  si se agotan sus 61 bits de generación. Ambos límites son inalcanzables en un
  uso razonable, pero su política es explícita.
- La garantía de lifetime exige conservar la frontera de quiescencia: producción
  debe retirar el callback mediante la serialización de `AudioDeviceManager`
  antes de cambiar vistas o liberar el último owner. El test prueba ese contrato,
  no una sustitución concurrente arbitraria de punteros.

No se introdujeron N-track, mixer general ni funciones visibles.
