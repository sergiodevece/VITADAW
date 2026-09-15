# VitaDAW 0.5.6 — Editing & Transport Hardening

## Alcance

Este incremento no añade funciones de producto ni cambia la UI. Congela la
semántica vigente de transporte y edición mediante una suite de integración
hardware-free que utiliza el motor, el compilador de planes y `processBlock`
reales. Los clips de escala conservan duración estrictamente positiva; la
diversidad procede de pistas vacías y huecos entre clips. El loop más corto
probado es exactamente el mínimo ya definido: 1024 ticks y, además, al menos
10 ms, un project frame y un device frame.

## Cobertura añadida

- Play/Pause/Stop/Seek repetidos, comandos acumulados antes del callback,
  rechazo de Seek durante Playing, burst de Seek durante Pause y FIFO lleno.
- Cancelación de comandos aceptados al cerrar la generación de lifecycle.
- Pause silencioso y estable; Stop en dos etapas; historial inalterado por
  navegación.
- Loop mínimo válido, callback único frente a particiones 1–1024+, sample rates
  proyecto/dispositivo 48/44,1 kHz y loops atravesando tempo y métrica.
- Split en 63/64/65, 127/128/129, 255/256/257 y 511/512/513, comparando el
  render bit a bit antes/después.
- Split rechazado en extremos y aceptado en el primer/último project frame
  legal de una fuente de tres frames.
- Rechazo atómico de Move/Duplicate/Split/Trim/Delete/Undo/Redo durante Play.
- Política Paused: edición dentro de pista y Undo/Redo permitidos, Move entre
  pistas rechazado, posición preservada.
- Move entre pistas, Split en playhead, Duplicate/Delete consecutivos, Undo
  completo, Redo completo y ciclos Undo→Redo→Undo→Redo.
- Save→divergencia→Load con estado documental exacto, historial limpio,
  serialización determinista y reproducción posterior real.
- 64 pistas, 1000 clips positivos, al menos ocho pistas vacías, huecos y
  callbacks variables hasta superar la capacidad preparada.
- Instrumentación de cero allocations y cero destrucciones de owners en RT.

## Fallo detectado y corregido

### Prueba roja

Un proyecto válido con una fuente de 1024 frames a 44,1 kHz y proyecto a 48 kHz
era aceptado por `ProjectState`, pero `prepareProcessingPlanFromSources` devolvía
`Prepared clip exceeds project/source bounds`. La suite lo reprodujo antes de
comparar splits alrededor de fronteras de bloque.

### Causa raíz

La duración se almacena como `double` en project frames. Al reconvertirla a
source frames usando `long double`, el resultado era aproximadamente
`1024.0000000000002`. `ProjectState::validateClip` ya admitía un residuo acotado
de round-trip, mientras `PreparedProcessingPlan` comparaba estrictamente contra
1024. Las dos fronteras validaban de forma distinta el mismo clip.

### Corrección mínima

La preparación del plan usa la misma tolerancia relativa que el dominio:
64 veces epsilon de `double`, escalada por la magnitud del límite. Un exceso
real continúa rechazado. El cálculo ocurre exclusivamente durante preparación,
fuera de RT; no modifica el renderer, los índices, la interpolación ni la
semántica temporal.

### Verificación

La prueba de 1024 frames 44,1→48 kHz prepara y conserva render bit a bit tras
Split. Los límites de fuente, clips de 1–3 frames y validaciones previas siguen
cubiertos por las suites existentes y por el nuevo test de extremos.

## Resultados

- Build completo Debug: **31/31 tests superados**.
- Build core-only Debug: **30/30 tests superados**.
- ASan + UBSan core-only: **30/30 tests superados**, sin diagnósticos.
- UBSan core-only: **30/30 tests superados**, sin diagnósticos.
- TSan core-only: **30/30 tests superados**, sin carreras detectadas.
- `git diff --check`: limpio.

## Riesgos residuales

- El harness usa PCM y filesystem deterministas; no sustituye el smoke test de
  dispositivo real, que no es necesario al no cambiar callback ni plataforma.
- Las secuencias generadas son acotadas y deterministas; no constituyen fuzzing
  continuo ni model checking formal.
- La tolerancia numérica está duplicada entre dominio y preparación. Se conserva
  así para mantener mínimo el cambio; una utilidad común solo se justificaría
  si una futura ampliación necesitara la misma política en más fronteras.
