# VitaDAW 0.7.1 — Recording Recovery Hardening validation

## Resultado final

VitaDAW 0.7.1 — `VitaDAW 0.7.1 - Recording Recovery Hardening` — fue publicado
en el commit `9f1cf29dbc10ba6c2f28941e24cc0a211a30a386`, con el tag anotado
`v0.7.1-recording-recovery-hardening`. El schema de proyecto permanece en v3 y
los documentos recién guardados identifican `writerAppVersion` como `0.7.1`.

La versión endurece la infraestructura de grabación/recovery sin convertir el
marker de recovery en una condición necesaria para Record ni en una autorización
de ownership de filesystem.

## Contratos de grabación y marker

- El recovery marker es auxiliar y best-effort. Un fallo exclusivo de create,
  write, `fsync` o close conserva operación, ruta, código y mensaje del sistema,
  pero no bloquea el preflight, el writer ni la aceptación de Record.
- La media real permanece crítica. Errores de creación/escritura/finalización
  WAV, `fsync` de media, publicación sin reemplazo, verificación de identidad o
  `fsync` del directorio impiden el commit de `ProjectState`.
- Si el marker inicial no está disponible, la sesión continúa de manera
  explícitamente degradada: puede finalizar, limpiar o apagarse sin usarlo, y
  una segunda grabación parte de estado limpio.
- El cleanup terminaliza tanto `prepared` como `capturing` antes del reset. No
  publica modelo ni historial durante shutdown; si no puede limpiarse media con
  ownership demostrable de forma atómica, la retiene como artefacto seguro.
- La integración hardware-free usa `AudioDeviceManager` y
  `JuceAudioDeviceAdapter::prepareRecording()` productivos con un `AudioIODevice`
  virtual de test. Cubre marker fallido, writer/captura preparados, Record
  aceptado, cleanup y una segunda preparación.
- En Windows, el marker usa creación exclusiva `CreateFileW(..., CREATE_NEW,
  ...)`, seguida de `WriteFile`, `FlushFileBuffers` y `CloseHandle`; las fases
  conservan diagnósticos diferenciados.

Recovery marker, filesystem I/O y finalización continúan fuera del callback RT.
La ruta de audio sigue limitada a `processBlock` y la instrumentación de cierre
no detectó allocations en esa función.

## Validación final

- build completo: **OK**;
- suite completa: **51/51**;
- Recording + integración JUCE: **5/5**;
- ASan + UBSan: **4/4**;
- ASan + UBSan incluyendo JUCE: **5/5**;
- TSan: **4/4**;
- instrumentación RT: sin allocations detectadas en `processBlock`;
- `git diff --check`: limpio durante el cierre;
- auditoría técnica final: sin findings bloqueantes;
- auditoría independiente desde VS Code: sin findings relevantes.

ASan en este macOS no disponía de LeakSanitizer; esta limitación no afecta los
resultados ASan+UBSan indicados.

## Smoke físico

El smoke físico de cierre fue correcto:

- primera grabación: OK;
- reproducción y finalización: OK;
- segunda grabación: OK;
- seis operaciones de Undo/Redo durante el smoke: OK.

## Limitaciones aprobadas de 0.7.1

- La rama Windows del marker fue revisada por inspección, pero no compilada ni
  ejecutada con SDK/toolchain Windows en el entorno macOS de cierre. No se
  considera un fallo bloqueante.
- 0.7.1 no implementa recuperación automática después de crash, escaneo de
  temporales al arrancar, adopción de huérfanos ni garbage collection de media.
- No añade input monitoring, compensación de latencia, punch recording, loop
  recording ni grabación multipista.
