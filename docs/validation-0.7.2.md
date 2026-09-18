# VitaDAW 0.7.2 — Input Monitoring Foundation validation

## Estado

La validación automatizada de Input Monitoring Foundation A/B/C está preparada
para auditoría. La validación física sigue pendiente y no se declara completa
en este documento.

## Validado automáticamente

- build completo y suite completa;
- Commands de Enable, Disable, Toggle y Set Monitor Gain sin `ProjectState`,
  historial ni dirty state;
- staging alias-safe, Capture raw y reproducción más Monitoring;
- meter de pico pre-gain, sin allocations en `processBlock`;
- integración hardware-free con `AudioDeviceManager` y
  `JuceAudioDeviceAdapter`, incluyendo preflight, rollback, buffer lifecycle,
  loss/error, Toggle posterior y shutdown;
- Recording con Monitoring ON/OFF, stop y cleanup, incluido el contrato
  `Monitoring ON → Record → Stop → Monitoring remains active`: la ruta
  productiva virtual verifica la señal de entrada/salida después de finalizar,
  una segunda toma, Disable durante Recording y cancelación sin device loss;
- ASan+UBSan y TSan focalizados;
- `git diff --check` limpio al finalizar la validación automatizada.

## Pendiente de smoke físico

- Monitoring solo, Playback + Monitoring y Recording + Monitoring;
- cambio de Monitor Gain durante una toma y verificación de WAV intacto;
- activación/desactivación de Monitoring durante Recording;
- segunda grabación, Undo/Redo documental y cierre con Monitoring activo.

## Limitaciones

- No existe direct monitor hardware, detección de feedback, compensación de
  latencia, routing avanzado, monitor por pista ni grabación multipista.
- El input meter no abre hardware por sí mismo; sólo observa input ya activo.
