# VitaDAW 0.6.6 — Arrange Foundation Completion validation

## Alcance

0.6.6 completa la base del Arrange con selección temporal efímera, Snap
determinista y conversión de la selección temporal al loop musical existente.
No añade comandos de dominio, no modifica el schema, no cambia el modelo de
documento y no introduce trabajo nuevo en el callback de audio.

El flujo estructural permanece `Command -> ProjectState` candidato -> preparación
completa fuera de RT -> commit conjunto de modelo, plan e historial. La selección
temporal, el estado de Snap, sus targets y los previews viven exclusivamente en
la capa de interacción/UI.

## Contratos temporales y de Snap

Una selección temporal es el intervalo exclusivo `[startFrame,
exclusiveEndFrame)`, normalizado para drag en ambas direcciones y limitado a
`[0, maximumSupportedProjectFrame()]`. Un gesto de menos de un frame no crea
selección. Click en el ruler hace Seek y limpia la selección; click en fondo de
Arrange la limpia; edición de clips, refresh, zoom, scroll, Undo y Redo la
conservan. La recreación del componente al cargar otro proyecto la elimina.

Snap está desactivado por defecto y usa tolerancia visual de ocho píxeles,
convertida a frames solo en la UI. Los targets se capturan al comienzo del gesto:
bordes de clips, bordes de selección temporal, playhead, frame cero y grid fijo
de beats. Un clip que se está moviendo no puede atraerse a sí mismo; la selección
temporal sí puede atraer sus bordes a cualquier clip. Los empates se resuelven
por distancia, clase explícita de target y frame menor, independientemente del
orden del vector de entrada. El movimiento múltiple aplica un único delta al
grupo y conserva posiciones relativas.

Los bordes de una Time Selection preexistente se capturan al iniciar el gesto y,
al crear otra selección, representan el rango anterior que está siendo
reemplazado: permanecen congelados hasta finalizar y los bordes del preview nuevo
nunca se reinyectan como targets.

Move Clip, MoveClips, Trim Left y Trim Right usan Snap. Split continúa usando el
playhead exacto y no se ha alterado. Todos los resultados quedan dentro del
dominio temporal certificado y los previews nunca se convierten en targets.

## Set Loop From Selection

La acción convierte el inicio con redondeo musical hacia abajo y el final
exclusivo hacia arriba, de modo que el loop resultante contiene siempre la
selección temporal. Reutiliza `SetLoopRangeMusical`: mantiene su política
Stopped-only, validación, historial, dirty state y Undo/Redo. No activa el loop
automáticamente y no consume ni borra la selección temporal. Una selección que
no puede producir un rango musical válido se rechaza antes del dispatch.

## Cobertura automática

`vitadaw_timeline_ui_foundation_tests` cubre normalización inversa, gesto vacío,
clamp al máximo, persistencia efímera ante refresh/zoom/scroll, convivencia con
selección de clips, limpieza explícita y reemplazo de proyecto. También cubre
Snap desactivado, match exacto, frame cero, beat grid, límite superior,
independencia del zoom, permutación de targets, empates, exclusión del clip
movido, Move Clip, MoveClips, ambos Trim y selección temporal contra bordes de
clips. La conversión de loop verifica floor/ceil, contención, preservación de la
selección y rechazo de rangos demasiado cortos.

`vitadaw_command_flow_tests` demuestra que selección temporal y Snap no cambian
modelo, historial, dirty state, revisiones ni preparación. El dispatch del
comando de loop crea una única entrada, soporta Undo/Redo, no activa el loop y
rechaza Paused/Playing con `transportMustBeStopped`.

`vitadaw_track_operations_tests` añade ciclos repetidos de reorder sobre la
cobertura previa de batches, routing, persistencia, rollback y reproducción. El
caso de estrés UI mantiene 1.000 clips seleccionados mientras calcula una
selección temporal con Snap.

## Matriz ejecutada

Validación local en macOS, configuración Debug salvo el benchmark:

- tests directamente afectados: Timeline UI, Command Flow y Track Operations,
  todos correctos;
- core-only (`build-core`): **37/37**, 39,12 s;
- suite completa con JUCE (`build`): **46/46**, 41,99 s;
- ASan+UBSan core (`build-asan`, `detect_leaks=0`): **37/37**, 132,02 s;
- integración JUCE ASan+UBSan (`build-asan-juce`, `temporal_juce_*`): **8/8**,
  1,51 s;
- UBSan core (`build-ubsan`): **37/37**, 99,70 s;
- TSan core (`build-tsan`): **37/37**, 221,86 s.

No hubo diagnósticos de sanitizers. LeakSanitizer permanece desactivado porque
no está disponible en el runtime Apple usado. Las suites completas incluyen las
regresiones acumuladas 0.6.0–0.6.5 y los oráculos temporales/musicales portable,
native y fast-math.

## Benchmark diagnóstico

`vitadaw_arrange_rebuild_benchmarks` es opt-in mediante
`VITADAW_BUILD_BENCHMARKS=ON`, no se registra en CTest y no define umbrales de
aceptación. Se ejecutó en Release, sin I/O dentro de las ventanas medidas, con
escenarios deterministas de 64 pistas/1.000 clips y 128 pistas/10.000 clips.
Informa medianas independientes de mutación del modelo, preparación del plan,
publicación y total para Move Clip, MoveClips, DuplicateClips, DeleteClips, Add,
Delete y Reorder Track, Undo y Redo.

En 64/1.000, las medianas totales observadas estuvieron entre **0,128 ms y
0,321 ms**. En 128/10.000 estuvieron entre **0,893 ms y 1,020 ms**. La fase de
preparación dominó ambos escenarios (aproximadamente 0,108–0,299 ms y
0,895–0,949 ms respectivamente). Son datos diagnósticos de esta máquina, no un
contrato de rendimiento; al ser medianas independientes, sus columnas no deben
sumarse entre sí.

## Smoke nativo

El bundle Debug 0.6.6 recién compilado se abrió con el dispositivo real:

- salida: Altavoces del MacBook Air;
- proyecto/dispositivo: 48.000 Hz;
- buffer: 512 frames;
- canales: 0 entradas / 2 salidas;
- Snap visible y desactivado por defecto;
- `Set Loop From Selection` visible y deshabilitado sin selección;
- Snap conmutó correctamente, Add Track y Undo funcionaron, y Play/Pause/Stop
  terminaron en `Stopped at start`;
- la ventana cerró sin diagnóstico visible.

El inyector de puntero del smoke no produjo de forma fiable un drag sobre el
ruler JUCE, por lo que el gesto visual y la habilitación posterior del botón no
se afirman desde esta automatización. Esas rutas sí quedan cubiertas por tests
portables, de flujo de comandos e integración JUCE con código de producción.

## Persistencia, DSP y límites

El schema continúa siendo **v3** y no hay migración ni nueva forma JSON; solo
cambia `writerAppVersion` a 0.6.6. Selección temporal y Snap no se guardan. El
orden DSP, el renderer, la publicación del plan y el callback no cambian.

Los límites deliberados permanecen: grid fijo de beat, sin grid configurable ni
Snap magnético avanzado; selección temporal sin handles; Set Loop no habilita el
loop; Split no usa Snap; y el benchmark no sustituye profiling RT profesional ni
establece presupuestos de producto.
