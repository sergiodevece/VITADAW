# VitaDAW 0.6.4 — Arrange Editing I validation

## Alcance consolidado

0.6.4 no reimplementa la edición introducida en 0.5.4. Consolida Add/Delete
Audio Track y Move Clip horizontal/cross-track sobre los contratos temporales
de 0.6.x. No añade comandos, subsistemas, concurrencia, garbage collection de
Sources ni otra política de solapamiento.

El flujo continúa siendo `Command -> ProjectState` candidato -> preparación
completa fuera de RT -> commit conjunto de modelo, plan e historial. El callback
no lee `ProjectState`.

## Cierre del dominio temporal

`ProjectState::validateClip` valida ahora dos fronteras:

- `projectStart` debe satisfacer `isSupportedProjectFramePosition`;
- el final exclusivo calculado por `checkedExclusiveProjectEnd` debe existir y
  satisfacer la misma condición.

La utilidad de final comprobado evita sumar duración y posición mediante entero
con overflow. La frontera exacta `exclusiveEnd == 2^53-1` permanece válida. Un
inicio negativo, un inicio posterior a `2^53-1` o un final exclusivo posterior
se rechazan como `invalidPosition` antes de preparar el processing plan.

La prueba de aplicación registra el número de preparaciones y conserva puntero
de plan, token de historial y valor de clip. Los tres rechazos mantienen todos
esos valores. Un Move a la misma pista y posición continúa siendo success no-op,
sin preparación ni entrada de historial.

## Semántica preservada

- Add/Delete Track son Stopped-only y undoables.
- Move horizontal se admite en Stopped o Paused; Move cross-track exige Stopped.
- Move conserva ClipId, SourceId, sourceOffset y duration.
- Mono solo se mueve a mono y estéreo solo a estéreo.
- Una posición válida posterior al contenido actual amplía contentDuration.
- Los clips adyacentes, solapes parciales y solapes completos son válidos.
- Los solapes se suman antes de los inserts de la pista destino.
- Delete Track conserva Sources.
- IDs y contadores permanecen monotónicos y no reutilizables.
- La selección de timeline continúa reconciliándose por TrackId/ClipId.

## Cobertura automática

La cobertura nueva de `vitadaw_track_operations_tests` comprueba:

- clip cuyo final exclusivo coincide exactamente con la frontera certificada;
- final exclusivo un frame fuera del dominio;
- inicio un frame fuera del dominio;
- inicio negativo;
- error `invalidPosition` antes de preparación;
- rollback de modelo, plan, historial y token en cada rechazo;
- Move exacto no-op sin historial ni rebuild.

Las suites existentes revalidan Add/Delete, múltiples pistas, layouts, clips
compartidos, solapamientos, reproducción, Undo/Redo, fallos de preparación y
commit, selección UI por IDs, Save/Load determinista, schema v3 y las matrices
de Transport, Musical Time, Loop y Metronome.

## Matriz ejecutada

Validación local en macOS, configuración Debug:

- suite completa con JUCE (`build`): **46/46**;
- core-only (`build-core`): **37/37**;
- ASan+UBSan core (`build-asan`): **37/37**;
- integración JUCE ASan+UBSan (`build-asan-juce`, `temporal_juce_*`): **8/8**;
- UBSan (`build-ubsan`): **37/37**;
- TSan (`build-tsan`): **37/37**.

ASan se ejecutó con `detect_leaks=0` porque el runtime Apple empleado no ofrece
LeakSanitizer. ASan, UBSan y TSan no emitieron diagnósticos. Los oráculos
temporales y musicales native, portable y fast-math forman parte de todas las
suites core ejecutadas.

## Persistencia

El schema continúa siendo **v3**. Pistas, clips, ownership, posiciones y
contadores ya eran documentales. Solo cambia `writerAppVersion` a 0.6.4; no hay
migración ni modificación de la forma JSON. Las pruebas de persistencia y el
harness de hardening validan Save/Load, serialización determinista y reproducción
real posterior a edición y Load.

## Smoke nativo

La aplicación Debug 0.6.4 se abrió con el adaptador y dispositivo reales:

- salida: Altavoces del MacBook Air;
- proyecto/dispositivo: 48.000 Hz;
- buffer: 512 frames;
- canales: 0 entradas / 2 salidas.

Se verificaron alta de pista mono y estéreo, Undo/Redo estructural, metrónomo
enabled, Play sobre proyecto vacío con metrónomo, Pause, primer Stop conservando
posición y segundo Stop a cero. Además, se importó mediante el selector nativo un
WAV temporal determinista, mono, 48 kHz, Int16 y 1 s: el proyecto mostró una
duración de 48 000 frames, la reproducción alcanzó el final y los medidores Master
y T1 registraron señal. La ventana cerró sin diagnóstico visible.

El smoke no afirma validación auditiva, drag manual de clip ni Save/Load mediante
los selectores nativos. Esas rutas quedan cubiertas por las suites JUCE,
persistencia, operaciones de pista y hardening con el código de producción.

## Riesgos residuales

- El smoke no sustituye una sesión acústica prolongada con un WAV conocido.
- Un clip completamente cubierto por otro conserva la prioridad visual
  determinista existente; no se añade selección de capas.
- No hay upmix/downmix, auto-scroll de drag, reorder ni multi-selection.
- El límite `2^53-1` sigue siendo el dominio numérico certificado provisional,
  no la definición conceptual del final del timeline.
- El benchmark RT profesional del scheduler temporal sigue fuera de este
  incremento.
