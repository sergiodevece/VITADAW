# VitaDAW 0.5.3 — Loop & Metronome

## Contratos

El rango documental es `[startTick,endTick)` y se compila con el mapa musical
como una única revisión. Bars 2–4 a 120 BPM, 4/4 y 48 kHz producen exactamente
`[96000,288000)` project frames. Los mínimos son 1024 ticks, 10 ms, un project
frame y un device frame; un fallo rechaza la edición sin clamp ni publicación.

El callback subdivide por scratch capacity y por cada loop end. La posición tras
wrap conserva el avance residual fraccional. `LoopWrap` preserva processors y
voces; Seek, Stop y lifecycle limpian scheduler y aplican la discontinuidad dura
existente. El Master Meter incluye el metrónomo porque el click se suma después
de Master Inserts y antes de Master Gain/Meter.

El metrónomo prepara dos bursts deterministas de unos 4 ms fuera de RT y usa
cuatro voces fijas. Beats se enumeran desde el mapa, se cuantizan causalmente y
los límites pertenecen a intervalos half-open. No hay allocations, locks,
filesystem, logging, trigonometría ni destrucción de owners en `processBlock`.

## Validación automática

`vitadaw_loop_metronome_tests` cubre golden loop, validación sin clamp,
persistencia determinista, round-trip de ticks, recompilación de tempo/métrica,
mapa de 4096 eventos, fase fraccional prolongada, más de cuatro wraps dentro de
un callback, invariancia ante particiones, límites de callback, playback vacío,
clicks normal/accent, Master Meter, Pause/Stop, re-registro de callback y cero
allocations durante el render medido. Las suites de comandos/persistencia cubren
clasificación documento/sesión, Undo y migraciones v1/v2→v3. Las suites
anteriores permanecen como regresión de clips, routing, sends, inserts,
lifecycle y RT.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure

cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

Resultados finales:

- build completa Debug: 28/28 tests;
- build core-only Debug: 27/27 tests;
- ASan + UBSan: 27/27, sin diagnósticos;
- UBSan independiente: 27/27, sin diagnósticos;
- TSan: 27/27, sin carreras detectadas;
- `git diff --check`: limpio.

## Smoke test nativo

Aplicación Debug ejecutada con `vitadaw-track1-440hz-44100.wav`: WAV mono,
44100 Hz y 2.000 s. Dispositivo observado: Altavoces del MacBook Air, 48000 Hz,
buffer de 512 frames, 0 entradas y 2 salidas.

Se comprobó rango Bars 2–4, enable de loop, metrónomo, cambio de nivel a
-24 dB, Play, wrap manteniendo `Playing`, Pause/Resume, primer Stop conservando
posición, segundo Stop a cero, y cierre limpio. El cambio de tempo inicial de
120 a 100 BPM recompiló el contexto y conservó el playhead; el WAV de 44.1 kHz
se reprodujo contra el proyecto/dispositivo a 48 kHz y produjo señal observable
en Track 1 y Master. La región de loop se extendió más allá de los 2 s de
contenido y continuó en silencio/metrónomo según la política de playback.

La sesión permite observar señal y estado en los meters, pero no dispone de una
captura acústica independiente; la colocación exacta de clicks y el contenido
sample-a-sample se verificaron además mediante el `processBlock` portable.

## Limitaciones deliberadas

No hay crossfade: un loop no continuo puede clicar. El click no es configurable
y no tiene subdivisions, routing ni meter propios. Si se agotan cuatro voces,
se sustituye la más antigua. Sin PDC, la latencia de processors no se compensa
frente al metrónomo.

## Release blocker: primera importación manual

La causa raíz estaba en `AudioStatusComponent::chooseWav`: tras recibir el
resultado correcto del chooser, la UI ejecutaba `juce::File::existsAsFile()`.
Cuando esa comprobación devolvía false para una selección ya confirmada, el
callback terminaba sin despachar comando, sin tocar el proyecto y sin mostrar
resultado. Era el único corte silencioso del recorrido; los rechazos posteriores
de `DawApplication` ya pasaban por `showResult`.

El fix elimina esa prevalidación de UI. Toda ruta no vacía se envía al backend,
que distingue `fileNotFound`, `permissionDenied`, `unsupportedFormat`,
`decodeFailed`, `preparationFailed` y `commitFailed`; Cancel devuelve
`userCancelled`. El callback captura `SafePointer`, y tanto el chooser WAV como
el de proyecto liberan su owner en cancelación y quedan seguros si se destruye
la ventana.

`New Project` deja de inyectar la plantilla provisional de smoke test y conserva
el modelo canónico vacío. Se eligió la política B: la primera importación añade
una pista mínima mono/estéreo dentro del mismo candidato que Source y Clip. El
ID usado es el devuelto por el modelo. Preparación y commit fallidos conservan
proyecto, PreparedProject y StateToken; el éxito mantiene la barrera de historial
no undoable y marca el documento dirty.

Las pruebas portables cubren estado inicial exacto, cancelación, acceso denegado,
archivo ausente, formato, decode, ausencia de destino explícito, rollback de
preparación/commit, primera pista/Source/Clip, conversión 44.1→48 kHz, duración,
TimelineSnapshot, playback, dirty/barrera, pista vacía existente y preservación
de mapas musicales, loop y metrónomo. La integración JUCE usa WAV real, prueba
errores reales de archivo/formato/decode, Save/Load y ejecuta `processBlock` con
instrumentación de allocations tras publicar el primer recurso.

La revalidación completa tras el fix obtuvo 28/28 tests en la build Debug con
aplicación y 27/27 en core-only. ASan+UBSan, UBSan independiente y TSan obtuvieron
27/27 sin diagnósticos; `git diff --check` quedó limpio.

El smoke test nativo comenzó en el `New Project` canónico sin pistas. Se canceló
el selector WAV y apareció `Import cancelled` sin mutación. Después se importó
`vitadaw-track1-440hz-44100.wav`, PCM16 mono, 44100 Hz y 2.000 s: se crearon una
pista, un Source y un Clip visibles en timeline. Con el dispositivo Altavoces del
MacBook Air a 48000 Hz, buffer de 512 frames, 0 entradas y 2 salidas, Play mostró
señal 0.259 en Track 1 y Master. Stop volvió a cero.

Se activaron loop Bars 2–4 y metrónomo. Tras unos 13 s de Play, el transporte
seguía en `Playing` a 5.312 s, confirmando varios wraps del intervalo 2–6 s. El
proyecto se guardó como `vitadaw-0.5.3-import-fix-smoke.vitadaw`, se cerró la
aplicación, se reabrió, se cargó mediante el selector nativo y volvió a reproducir
el PCM con señal. Los locators persistieron; loop enabled y metrónomo volvieron a
off por ser estado de sesión. Stop y cierre final fueron limpios.

Durante la primera pasada, destruir el `FileChooser` dentro de su propio callback
dejó bloqueada la hoja nativa posterior de Save As. La liberación se difirió al
siguiente turno del message thread mediante `MessageManager::callAsync`, manteniendo
el `SafePointer`; la repetición completa de importación, guardado y carga confirmó
que los selectores se cierran y pueden reutilizarse sin callbacks colgantes.
