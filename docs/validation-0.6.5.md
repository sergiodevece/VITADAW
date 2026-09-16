# VitaDAW 0.6.5 — Arrange Editing II validation

## Alcance

0.6.5 añade únicamente reorder de pistas, multiselección efímera y operaciones
agregadas Move/Delete/Duplicate. No introduce movimiento vertical múltiple,
Shift-range, marquee, drag de headers, snap, Split/Trim múltiples, GC de Sources,
cambios de routing/mixer ni schema nuevo.

El flujo permanece `Command -> ProjectState` candidato -> preparación completa
fuera de RT -> commit conjunto de modelo, plan e historial. El callback no lee
`ProjectState`, no valida estructura y no reconstruye el grafo.

## Reorder y selección

La API pública expresa reorder como `TrackId + anchor TrackId + before/after`.
Solo cambia el orden documental/visual; TrackId, clips, mixer, inserts, routing
y sends permanecen intactos. El orden DSP continúa derivándose de TrackId. La UI
ofrece Move Track Up/Down y los límites son success no-op.

El contrato no relaciona posicionalmente `tracks_` y `trackRoutes_`: el primero
define el orden visual/documental y el segundo es una colección resuelta por
TrackId. Save canonicaliza las rutas por TrackId, mientras conserva el orden de
pistas; Load y preparación DSP vuelven a asociarlas por identidad.

La selección de clips es una colección canónica de ClipIds. Click normal
reemplaza, salvo sobre un miembro seleccionado para poder arrastrar el grupo;
Cmd/Ctrl-click alterna membresía y click vacío limpia. Split y Trim solo se
habilitan con exactamente un clip. Selección y previews son efímeros, no se
persisten ni forman parte del historial, y se reconcilian por ID.
La pista activa `selectedTrack_` es un contexto independiente para acciones de
pista y highlight: una selección multitrack conserva una única pista activa,
pero las operaciones de clips se construyen exclusivamente desde sus ClipIds.

## Atomicidad batch

`MoveClips`, `DeleteClips` y `DuplicateClips` deduplican IDs y resuelven y
validan el conjunto entero antes de mutar. Un ID ausente, una posición fuera del
dominio, capacidad insuficiente o un fallo de preparación rechaza la operación
completa sin cambiar modelo, plan, historial ni contadores.

Move usa un único `deltaFrames`, mantiene pista propietaria y offsets relativos,
y trata delta cero como success no-op. Delete conserva Sources. Duplicate asigna
nuevos IDs monotónicos en el orden ascendente de los ClipIds originales, no los
consume ante fallo y desplaza el gesto UI por el ancho exclusivo completo del
bloque, incluidos huecos. Los IDs creados se devuelven en el campo específico
`createdClips` de `CommandResult` para seleccionar el resultado.

Cada gesto con cambios crea exactamente una preparación, un commit y una entrada
Undo. Los payloads vectoriales restauran valores e IDs exactos y su capacidad
dinámica se suma al presupuesto acotado del UndoManager.

## Cobertura automática

`vitadaw_track_operations_tests` cubre reorder exacto y no-op, preservación de
submodelos/DSP, batches multi-track, deduplicación, IDs deterministas, geometría,
Sources, límites temporales, IDs ausentes, lista vacía, Playing/Paused, rollback,
Undo/Redo, branching, Save/Load v3 y reproducción offline tras reorder/load.
También fija el contrato no posicional con Track→Bus, track send, bus send,
clips, render antes/después, Save→Load y segundo Save canónico por TrackId.

`vitadaw_timeline_ui_foundation_tests` cubre formación y reducción de selección,
click conservando grupo, reconciliación por IDs, drag horizontal con delta común,
Duplicate con huecos, Delete agregado y restricción de Split/Trim a selección
unitaria. Además verifica selección multitrack con pista activa independiente,
selección exacta de los nuevos IDs tras Duplicate y separación entre comandos de
clip y contexto de acciones de pista. `vitadaw_undo_redo_tests` verifica memoria dinámica, rechazo por
presupuesto insuficiente, frontera exacta del presupuesto y límite global.

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
temporales y musicales native, portable y fast-math, además de las regresiones
0.6.0–0.6.4, forman parte de las suites completas.

## Smoke nativo

La aplicación Debug 0.6.5 se abrió con el adaptador y dispositivo reales:

- salida: Altavoces del MacBook Air;
- proyecto/dispositivo: 48.000 Hz;
- buffer: 512 frames;
- canales: 0 entradas / 2 salidas.

Se crearon dos pistas mono y una estéreo; Undo retiró exactamente la última y
Redo la restauró. Se activó el metrónomo, Play avanzó el reloj real, Pause se
aceptó y el doble Stop devolvió la posición a cero. Los controles Move Track
Up/Down estuvieron presentes y la ventana cerró sin diagnóstico visible.

La automatización nativa no pudo establecer selección sobre el canvas JUCE, por
lo que reorder, importación, multiselección y drag no se afirman desde este smoke.
Esas rutas quedan cubiertas por tests de dominio/aplicación, UI portable,
persistencia, integración JUCE y reproducción offline con código de producción.

## Persistencia y RT

El schema continúa siendo **v3**. Reorder utiliza el orden ya existente del array
de pistas; Duplicate persiste IDs, `nextClipId`, ownership y geometría temporal.
No hay migración ni cambio de forma JSON; solo cambia `writerAppVersion` a 0.6.5.
La preparación sigue fuera de RT y el renderer/callback no recibe ninguna nueva
responsabilidad.

## Limitaciones deliberadas

- Move múltiple es solo horizontal y no cambia ownership.
- No hay Shift-range, marquee/lasso, drag de headers, time selection ni snap.
- Split y Trim continúan siendo escalares.
- El smoke nativo complementa, pero no sustituye, una sesión acústica prolongada.
- El benchmark RT profesional del scheduler continúa fuera de este incremento.
