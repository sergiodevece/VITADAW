# VitaDAW 0.7.2 — Input Monitoring Foundation validation

## Estado

Input Monitoring Foundation está validado de forma automatizada y mediante
smoke físico en macOS. Esta evidencia no convierte la rama Windows, revisada
por inspección, en una validación ejecutada sobre Windows.

## Validado automáticamente

- build completo y suite completa: 53/53;
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
- ASan+UBSan y TSan focalizados, sin diagnósticos;
- `git diff --check` limpio al finalizar la validación automatizada.

## Smoke físico

- Monitoring parado, durante Playback y durante Recording: OK;
- `Monitoring ON → Record → Stop → Monitoring remains active`: OK;
- segunda grabación: OK;
- Disable Monitoring durante Recording no interrumpe Recording: OK;
- `Monitoring OFF → Record → Stop` permanece OFF: OK;
- WAV, finalización y reproducción: OK.

## Limitaciones

- No existe direct monitor hardware, detección de feedback, compensación de
  latencia, routing avanzado, monitor por pista ni grabación multipista.
- El input meter no abre hardware por sí mismo; sólo observa input ya activo.
- Windows fue revisado por inspección, pero no compilado ni ejecutado con SDK o
  toolchain Windows en este entorno macOS.
