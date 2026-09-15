# VitaDAW 0.5.4 — Track Operations & Cross-Track Editing

## Alcance validado

El incremento añade Add/Delete Audio Track, selección explícita de pista,
importación dirigida y movimiento 2D de clips. No cambia el schema documental
v3 ni incorpora reorder, waveform, grabación, MIDI, automatización,
multi-selection o drag externo.

## Contratos

Una pista nueva se añade al final, recibe un `TrackId` monotónico, nombre
`Audio N` cuando la UI no proporciona uno, layout mono/estéreo fijo, mixer por
defecto, inserts y sends vacíos y output directo a Master. Add/Delete son
Stopped-only y dirty.

Delete retira la pista, clips, inserts, mix, route y sends cuyo origen sea esa
pista. Conserva Sources, buses y estado no relacionado. Su payload Undo posee
únicamente ese submodelo; no incluye PCM ni runtime. Undo/Redo restaura los
mismos IDs y posiciones, mientras los contadores nunca retroceden.

Move 2D cambia owner y `projectStart` en una operación. Mantiene ClipId,
SourceId, sourceOffset y duration. Se permiten mono→mono y stereo→stereo; no hay
conversión implícita. Los solapes se suman antes de los inserts destino.

La timeline resuelve la lane visible a TrackId usando snapshot, geometría y
scroll. Preview no muta el modelo. La selección desaparece si su pista o clip
deja de existir. Con pistas existentes, Import requiere una selección explícita;
con cero pistas se mantiene la creación automática de 0.5.3.

## Validación automática

`vitadaw_track_operations_tests` cubre:

- defaults, salida Master, nombres e IDs monotónicos;
- Undo/Redo de Add y Delete con clips, mixer, insert, route y sends;
- Sources compartidas y Delete de la última pista;
- importación posterior a cero pistas;
- Move horizontal/vertical atómico y preservación de identidades;
- mono/stereo compatible y rechazo de layout o TrackId inválido;
- cambio de duración, overlap y procesamiento por inserts destino;
- rollback de preparación y commit;
- preservación de posición, mapas musicales, loop y metrónomo;
- round-trip schema v3 con pistas vacías y orden exacto;
- 64 pistas/1000 clips, límite de 256 pistas y render RT sin allocations.

`vitadaw_timeline_ui_foundation_tests` añade resolución vertical portable con
scroll, selección Track/Clip, preview compatible/incompatible, comando único en
mouseUp e invalidación tras Delete. Todas las suites anteriores permanecen como
regresión de routing, sends, processors, persistencia, lifecycle, transporte,
loop, metrónomo y tiempo real.

## Resultados

- Build Debug completo: 29/29 tests superados.
- Build core-only: 28/28 tests superados.
- ASan + UBSan: 28/28 tests superados, sin diagnósticos.
- UBSan independiente: 28/28 tests superados, sin diagnósticos.
- TSan: 28/28 tests superados, sin carreras detectadas.
- `git diff --check`: sin errores.

## Smoke test nativo

Ejecutado con la aplicación Debug real y el dispositivo:

- salida: Altavoces del MacBook Air;
- proyecto/dispositivo: 48.000 Hz;
- buffer: 512 frames;
- canales: 0 entradas / 2 salidas;
- WAV 1: `vitadaw-track1-440hz-44100.wav`, mono, 44.100 Hz, 2 s;
- WAV 2: `vitadaw-track2-660hz-48000.wav`, mono, 48.000 Hz, 4 s.

Se validó el recorrido completo de 25 pasos: proyecto nuevo, importación que
crea Track 1, alta y selección de Track 2, importación dirigida, render de ambas
fuentes con meters activos, Play, Stop a cero, movimiento horizontal y vertical
del mismo clip, Undo/Redo, alta de pista estéreo, rechazo mono→estéreo sin
mutación, Delete/Undo de una pista con clips, loop, metrónomo, cambio de tempo,
Save As, cierre, reapertura y Load.

El round-trip restauró tres pistas en el orden exacto, dos clips superpuestos en
Track 2, sus IDs, layouts, rutas directas a Master, posiciones 0 y 48.000, rango
de loop y mapa de tempo. Después se eliminaron todas las pistas y una nueva
importación creó `T4`, demostrando que el contador no retrocede. El cierre de
esa sesión y el de una segunda instancia limpia fueron correctos.

Como los clips quedaron completamente superpuestos tras el primer movimiento
vertical, se hizo Undo para volver a exponer el clip corto, se movió
horizontalmente y se repitió el movimiento vertical antes de comprobar
Undo/Redo. El modelo y el historial validaron el mismo `ClipId`; la UI mantiene
por ahora la prioridad determinista del clip dibujado en último lugar para hit
testing de solapes completos.

## Limitaciones deliberadas

- Delete con contenido es directo y muestra resultado provisional, sin modal.
- No hay auto-scroll durante cross-track drag.
- No hay upmix/downmix: el layout debe coincidir.
- Import sigue siendo una barrera no undoable.
- No hay reorder, resize de lane, multi-selection ni drag externo.
- Un clip totalmente cubierto por otro no puede elegirse directamente con el
  ratón; la selección de capas/solapes queda fuera de 0.5.4.
