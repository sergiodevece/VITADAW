# VitaDAW 0.6.0 — Transport & Timeline Foundation (candidato corregido)

## Estado

CMake, writerAppVersion y getApplicationVersion permanecen en 0.5.6.
No se ha realizado commit, tag ni push. Este informe no sustituye la nueva
auditoría del candidato.

## Correcciones y contrato

La sección final «Preparación musical exacta» registra la revisión más
reciente y sustituye las conclusiones históricas previas.

1. TransportReducer es puro y lo utilizan productor y reloj RT. Devuelve estado
   y descripciones de efectos. El replay nunca toca reloj real, FIFO, lifecycle,
   recursos ni discontinuidades DSP.
2. RealtimeProjectClock conserva el locator entero y un residuo en [-0.5, 0.5].
   Los hechos de frontera del snapshot no son otro locator ni se persisten.
   Pause/Play conserva una media muestra pendiente aunque el locator ya se haya
   redondeado al final. Stop/Stop conserva sus dos acciones en FIFO.
3. La proyección se reconstruye desde el snapshot confirmado, incluso si su
   watermark resuelve solo parte del historial. Se reaplican los comandos
   aceptados pendientes de la generación vigente. Un final natural no se ignora.
4. FIFO: 7 entradas utilizables; registro del productor: 14 entradas fijas.
   Ambas necesitan espacio antes de reservar un ticket. Saturación del registro
   añade backpressure explícito sin perder comandos ni reservar memoria en RT.
5. El CAS final del gate acepta la acción. AudioCommandSequence es un ticket
   monotónico de reserva/resolución, no una numeración consecutiva de aceptaciones.
   Cola llena/dominio inválido no reservan. Si cierre gana tras reservar, la acción
   no se publica, aunque su número pueda aparecer en el watermark o dejar un hueco.
   Las reservas aceptadas tienen el enlace release/acquire que garantiza inclusión;
   una reserva rechazada puede quedar fuera del watermark sin acción por resolver.
6. Stop no operativo solo devuelve alreadySatisfied con Stopped @ 0 confirmado
   en la generación correspondiente. Snapshot antiguo, Paused conservado o
   Stopped @ X se rechazan. Con consumidor operativo se acepta como scheduled.
7. Seek sigue permitido en Stopped/Paused y rechazado en Playing. No llama reset.
   Pause conserva su notificación hasta DSP. Un rebuild previo la subsume en
   hardDiscontinuity y mantiene los resets legítimos.
8. contentDuration no limita Seek. GoToEnd apunta al contenido. Play en/después
   del final real reinicia provisionalmente; no es política definitiva de
   grabación ni reproducción de proyectos vacíos.

Aceptación y ejecución no son simultáneas. El corte de FIFO decide los comandos
del callback actual; un evento de ese bloque puede preceder a la ejecución de
comandos aceptados después del corte. La proyección es provisional frente a
eventos aún no publicados y se reconcilia al observarlos. La vista de aplicación
separa projectedThroughTicket del watermark RT confirmado.

## Máximo numérico y cambios locales adicionales

Se mantiene maximumSupportedProjectFrame() = 2^53-1, NO porque toda fracción sea
representable al sumarla a ese entero. En binary64, cerca de 2^53 la separación
es de un frame. Por eso:

- El reloj compara diferencias enteras/fases locales; no reconstruye fronteras
  a partir de double(locator) + phase.
- Wrap calcula sobrepaso local antes de crear un intermedio fuera del dominio.
- DspFramePosition copia entero/residuo al scratch preasignado. El render calcula
  coordenadas relativas antes de convertir a una posición fuente flotante.
- Búsquedas de clips y beats usan rangos absolutos conservadores; contribución y
  offset se deciden localmente. La política de loop/metrónomo no cambia.
- El plan guarda la duración local del clip. projectEnd es una cota conservadora
  para selección de candidatos, no el límite exacto para contribuir audio.
- checkedExclusiveProjectEnd comparte start + ceil(duration), con suma entera
  comprobada, entre ProjectState y el plan.
- El presupuesto de memoria incluye sizeof(DspFramePosition). No hay allocations
  nuevas en el callback.
- Preparación rechaza clips/loops que exceden el dominio numérico admitido.

La interpolación lineal y los datos de presentación siguen siendo aproximaciones
flotantes; no se afirma exactitud universal de cada operación DSP. La integral
musical que decide fronteras DSP sí se prepara racionalmente desde los bits
binary64 contractuales, sin pasar antes por esos valores de presentación.

## Pruebas dirigidas y evidencia roja

Antes de las primeras correcciones se añadieron y ejecutaron individualmente,
con fallo determinista, las pruebas:
- resume: 48/96 kHz proyectaba un reinicio prematuro;
- stop: Paused no operativo se declaraba satisfecho;
- maximum: la suma absoluta anticipaba el final;
- loop: la proyección y RT discrepaban en 1000.5 frente a loopEnd 1000.75.

Durante la integración se añadieron los casos de reconciliación y capacidad
doble. No se atribuye retrospectivamente una ejecución roja previa a todos ellos.

El análisis numérico añadió dos rojas más, corregidas antes de validar:
- maximum-audio: se perdía la media muestra al alimentar el render absoluto;
- duración documental/plan: start grande + 2.5 perdía el ceil exclusivo.
El cambio de cota conservadora también hizo fallar la regresión existente de
Split; se corrigió su ruta de render para usar duración local. No se cambiaron
expectativas para ocultar esa regresión.

Cobertura final:
- A: loop fraccionario, hechos de frontera y equivalencia reductor/reloj.
- B: processBlock real con un procesador de prueba que encola Pause después del
  corte FIFO y antes de publicar el snapshot; natural end, replay y Seek dejan
  Stopped @ C. Hook únicamente de test, sin espera ni allocation.
- C: 48/96 kHz, 999 -> 999.5 -> Pause -> Play; señal distingue último sample de cero.
- D: intercalaciones concurrentes de claim/reserva/aceptación/cierre en la suite
  del gate; integración de watermark rechazado aislada de los efectos legítimos
  de lifecycle. El ticket puede cambiar, la acción rechazada no mueve el reloj.
- E: cierre antes de publicación, generación antigua, Paused y Stopped @ X
  preservados, y reconocimiento explícito de alreadySatisfied.
- F: ratios racionales 1/2, 147/160 y 320/147 próximos al máximo, residuos de ambos
  signos, frontera/wrap, audio real legacy y plan preparado, duración del modelo.
  El oráculo usa numeradores/denominadores enteros y distancias locales.
- G: Pause -> Seek -> rebuild -> Play: reset legítimo, hardDiscontinuity, continuous.
- H: FIFO libre/registro lleno y registro libre/FIFO llena; no entrada ni cambio
  de proyección y siguiente ticket sin hueco por esos rechazos.
- Snapshot concurrente: generación y hechos de frontera coherentes con el payload.
- Regresiones de 0.5.6, loop/metrónomo, edición, persistencia, inserts y zero-allocation.

El acceso friend de tests permite detenerse entre etapas existentes para verificar
capacidad y cierre; no añade hooks ejecutables ni esperas a producción. La prueba
B ordena la intercalación en un único hilo de test; las carreras C++ reales del
gate continúan probándose mediante threads y semáforos en su suite existente.

## Validación de esta ejecución

- Build completo Debug y tests: 32/32.
- ASan + UBSan core-only: 31/31, sin diagnósticos.
- Core-only Debug: 31/31.
- UBSan core-only: 31/31, sin diagnósticos.
- TSan core-only: 31/31, sin diagnósticos.
- git diff --check: limpio.

Comandos ejecutados para cada directorio build, build-core, build-asan,
build-ubsan y build-tsan: cmake --build <directorio> -j <2 o 3>, seguido de
ctest --test-dir <directorio> --output-on-failure. Las configuraciones sanitizer
usan respectivamente -fsanitize=address,undefined; -fsanitize=undefined; y
-fsanitize=thread, con -fno-omit-frame-pointer.

Dictamen de implementación: candidato apto para una nueva auditoría, sin declarar
cerrado su versionado hasta esa revisión. No hay hallazgos Critical/High conocidos
pendientes dentro de los contraejemplos tratados; los límites siguientes siguen
siendo condiciones explícitas, no garantías eliminadas por pasar sanitizers.

## Riesgos residuales

- Low: un único productor y un único lector de snapshots siguen siendo
  precondiciones; introducir otro productor requiere un diseño distinto.
- Low: la proyección no tiene frescura instantánea; projectedThroughTicket no es
  confirmación de ejecución RT.
- Low: interpolación lineal y posiciones derivadas flotantes conservan precisión
  finita. El locator y las decisiones de frontera no dependen de esa presentación.
- Low: el registro puede rechazar por backpressure aunque la FIFO ya tenga hueco.
- No se realizó smoke acústico ni se cambiaron UI/versión o políticas de producto.

## Inventario completo del candidato frente a HEAD 0.5.6

Incluye cambios anteriores del candidato, no solo esta corrección.

Modificados:
- CMakeLists.txt
- README.md
- docs/architecture.md
- src/vitadaw/application/DawApplication.cpp
- src/vitadaw/audio/CommandLifecycleGate.h
- src/vitadaw/audio/IAudioEngineControl.h
- src/vitadaw/audio/PreparedProcessingPlan.cpp
- src/vitadaw/audio/PreparedProcessingPlan.h
- src/vitadaw/audio/PreparedProject.h
- src/vitadaw/audio/PreparedTemporalContext.cpp
- src/vitadaw/audio/RealtimeAudioEngine.cpp
- src/vitadaw/audio/RealtimeAudioEngine.h
- src/vitadaw/audio/RealtimeProjectClock.h
- src/vitadaw/audio/RealtimeTransportExchange.h
- src/vitadaw/audio/TrackRenderer.h
- src/vitadaw/platform/juce/JuceAudioDeviceAdapter.cpp
- src/vitadaw/platform/juce/JuceAudioDeviceAdapter.h
- src/vitadaw/processors/IAudioProcessor.h
- src/vitadaw/project/ProjectState.cpp
- src/vitadaw/timeline/Time.cpp
- src/vitadaw/timeline/Time.h
- src/vitadaw/transport/TransportState.h
- tests/CommandFlowTests.cpp
- tests/EditingTransportHardeningTests.cpp
- tests/ProcessorCommandTests.cpp
- tests/ProcessorInsertCoreTests.cpp
- tests/RealtimeAudioEngineTests.cpp
- tests/TrackOperationsTests.cpp
- tests/TransportStateTests.cpp

Añadidos:
- docs/validation-0.6.0.md
- src/vitadaw/audio/DspFramePosition.h
- src/vitadaw/transport/TransportReducer.h
- tests/TransportTimelineFoundationTests.cpp

## Histórico: corrección de la segunda auditoría (sustituida)

Esta sección registra un candidato anterior. Sus afirmaciones de compensación
FMA y su dictamen no describen la migración exacta actual; véase la última sección.

Alcance exclusivo: disponibilidad de Play/lifecycle, coordenadas locales grandes
y final natural n-epsilon. Sin cambios de versión, commits, tags ni push.

### Evidencia roja previa a modificar producción

Se ampliaron primero los tests C++ y se ejecutaron individualmente en build-core:

- `availability`: fallo en admisión de Play tras cancelar SetMetronomeEnabled y
  completar su store auxiliar tardío.
- `generation`: fallo al aceptar un enqueue validado antes de cierre/reapertura.
- `large-local`: fallo de señal usando preparación real y processBlock con
  L=2^53-2, inicio cero, una muestra, sourceRate=48000/L, proyecto 48k/device 96k.
- `n-epsilon`: fallo al declarar Stopped antes del final con deviceRate=48000.0024.

### Correcciones

1. Se eliminan los dos flags requested*. Play usa snapshot confirmado más replay.
   Enqueue compara la generación del claim con la de la proyección antes de
   reservar. Cierre posterior al claim invalida el CAS; cierre posterior a la
   aceptación cancela normalmente. No se añade otra copia mutable de disponibilidad.
2. Render compara pertenencia con entero/fase sin fusionarlos en un double local.
   La posición fuente se calcula con producto/división/suma compensados y se
   conserva high/low hasta bounds e interpolación. También se prueba sourceOffset=3
   y la ruta legacy. Se mantiene maximumSupportedProjectFrame()=2^53-1, apoyado en
   estas comparaciones por componentes, no solo en representabilidad del locator.
3. Se elimina la tolerancia 1e-7 del final natural. La regresión existente
   `RealtimeAudioEngineTests` reveló entonces una muestra extra al acumular 4/3.
   No se cambió su expectativa: el avance real retiene el resto de la división
   de rates como numerador/escala, normalizados por potencia de dos, con FMA.
   La fase derivada permanece en [-0.5,0.5]. Pause/checkpoint conservan el resto;
   Seek lo reinicia. No se introduce otra epsilon ni se cambia la política de loop.

### Tests dirigidos finales

Todos están en `TransportTimelineFoundationTests.cpp`:

- `playAvailabilityLifecycle` (`availability`): productor y lifecycle en hilos
  distintos, ordenados por semáforos. El acceso friend divide las etapas existentes
  del setter en enqueue/tail; reproduce el store antiguo si el miembro existe.
  Tras eliminarlo no hay tail de estado. Se ejecuta el enqueue real y posteriormente
  processBlock real, incluida cancelación y confirmación de consumidor. No hay
  semáforos ni hooks de espera en producción ni dentro del callback.
- `playGenerationValidation` (`generation`): cierre/reapertura después de construir
  disponibilidad y antes del claim; rechazo y reconciliación posterior.
- `largeLocalAudio` (`large-local`): audio en L-1 y L-0.5, silencio al final,
  offsets fuente cero/tres, plan preparado y compatibilidad legacy.
- `naturalEndBelowBoundary` (`n-epsilon`): audio/Playing antes de frontera, última
  muestra válida, Stopped al cruzarla y silencio posterior.
- `rateBoundariesAndPartitioning` (`rate-boundaries`): audio y final exactos para
  4/3, 44.1/48, 48/44.1, 48/96 y 96/48; igualdad de señal con distinto particionado.

Las cuatro primeras pruebas fallaron antes de la corrección; la quinta amplía
la regresión racional detectada durante ella. No se atribuye ejecución roja a
las variantes añadidas posteriormente (offset fuente/legacy).

### Validación final y riesgos

Validación final de este estado, reconstruyendo todas las configuraciones:

- Tests dirigidos `availability`, `generation`, `large-local`, `n-epsilon` y
  `rate-boundaries`: 5/5 ejecuciones individuales correctas.
- Build completo Debug (incluida aplicación JUCE): 32/32 tests, 16.60 s.
- Core-only Debug: 31/31 tests, 15.81 s.
- ASan + UBSan: 31/31 tests, 16.75 s, sin diagnósticos.
- UBSan independiente: 31/31 tests, 17.56 s, sin diagnósticos.
- TSan: 31/31 tests, 33.08 s, sin diagnósticos.
- `git diff --check`: limpio.

Comandos: `cmake --build <directorio> -j 2` y
`ctest --test-dir <directorio> --output-on-failure`, usando build, build-core,
build-asan, build-ubsan y build-tsan. Las opciones sanitizer son las registradas
arriba. Los casos individuales se ejecutan con
`build-core/vitadaw_transport_timeline_foundation_tests <caso>`.

La compensación usa binary64/FMA y requiere no habilitar fast-math. Conserva
precisión finita; no se promete exactitud matemática universal para todos los
ratios representables. Los tests no sustituyen una nueva auditoría. Se conservan
productor/lector únicos, lifecycle quiescente y publicación preasignada. No hay
allocations, locks, I/O, ownership nuevo ni bucles de reintento nuevos en RT;
la conversión añade trabajo aritmético constante por muestra.

Dictamen de implementación: apto para una nueva auditoría de los tres hallazgos,
no una autorización de versionado. CMake/writerAppVersion/getApplicationVersion
siguen en 0.5.6 y HEAD continúa en 4df6f98e2b6b4575447cb11a2b1ece19fe3e14f6.

Archivos tocados en esta corrección: RealtimeAudioEngine.cpp/.h,
RealtimeProjectClock.h, DspFramePosition.h, TrackRenderer.h,
TransportTimelineFoundationTests.cpp, README.md, architecture.md y este documento.
Los demás cambios del working tree pertenecen al candidato anterior.

## Estado actual: migración numérica 128/256 (sin cierre ni versionado)

Esta sección sustituye los dictámenes históricos anteriores. La migración está
implementada en reloj/render, pero **no está completa respecto al contrato de
independencia floating point de extremo a extremo**.

### Representación y ejecución

- `TemporalInteger.h`: UInt128 de dos palabras y UInt256 de cuatro; comparación,
  carry/borrow, shifts, producto 128x128 y división/resto acotada. Backend portable
  con mitades de 32 bits; aceleración opcional con `__int128`. Sin memoria dinámica.
- `ExactTemporal.h`: descomposición exacta de binary64, certificados de ratios y
  límites, posición por componentes y mapping factorizado fuente/proyecto.
- `ExactProjectPhase.h`: locator entero más residuo firmado racional. Avance por
  cociente/resto preparado; comparación exacta, wrap y corte de subbloques sin
  epsilon ni acumulación floating point. Checkpoint incompatible rechazado sin
  modificar el estado anterior.
- Render legacy/prepared comparten `sourceIndex`: pertenencia e índice enteros,
  después conversión de resto/denominador para interpolación. No se construye
  primero una coordenada fuente absoluta double.
- El callback no prepara ratios. La preparación legacy de device rate y la
  certificación previa al reemplazo de plan ocurren fuera de RT. Los nuevos
  segmentos analíticos del metrónomo son inmutables y se preparan fuera de RT.

Los certificados de audio cubren tasas binary64 de [1, 1048576] Hz; el certificado
genérico admite además algunas tasas fuera de ese intervalo, incluido el fixture
de una sola muestra de la tercera auditoría. El scheduler musical requiere
project/device en ese intervalo. No cambia SampleRate::isValid ni el máximo
público 2^53-1. Fracciones documentales de duración/offset/loop se limitan a
denominadores diádicos 2^84/2^116/2^72 respectivamente, además de los límites
existentes de posición, memoria y documento. D base admite hasta 125 bits;
su extensión musical certificada admite hasta 128 bits. Cada
componente persistente hasta 128; los productos/sumas se certifican antes de RT.

Se rechazan ratios/componentes fuera de capacidad y conversiones de checkpoint
con resto no nulo. No se aplica redondeo ni fallback aproximado en esos kernels.
La preservación transaccional se comprueba directamente en cambios del formato
del reloj; los tests estructurales/lifecycle existentes siguen ejecutándose.

### Evidencia matemática y de integración

`TemporalIntegerOracleTests.py` utiliza enteros ilimitados y Fraction de Python,
fuera de producción y sin repetir el algoritmo de palabras de C++. Ejecuta 8688
comprobaciones por binario, contra los targets normal, portable forzado y
fast-math/fp-contract. Comprueba operaciones en fronteras de bits, casos aleatorios
reproducibles, índices/restos exactos, ratios enteros/no enteros, fases de ambos
signos, offsets grandes/fraccionarios, límites y secuencias de 100003 avances.

El fixture verifica explícitamente:
`1-sourcePosition = 69/5070602400912917605986812821504000`.
`TransportTimelineFoundationTests::exactAuditReproducers` lo verifica también
mediante plan preparado y processBlock real, comprobando audio/audio/silencio.
Incluye los dos pares de tasas binary64 de la auditoría para final justo anterior
y exacto. Siguen ejecutándose los tests anteriores de legacy/prepared,
particionado, loops, discontinuidades, transporte, lifecycle, FIFO, editing,
procesadores y persistencia. Los oráculos no sustituyen una prueba exhaustiva.

### Coste y alcance de las garantías

La división portable tiene un máximo fijo de 256 pasos; los tipos no crecen con
el tiempo. El render añade dos divmod por muestra/clip activo y operaciones
fijas sobre factores. No hay GCD/LCM/descomposición binary64 en esas rutas RT.
Los scratch/planes se dimensionan antes del callback; no se añade ownership
dinámico en RT. Las regresiones de zero-allocation siguen activas.
No se ha medido cumplimiento de deadlines en una sesión profesional: los tiempos
de tests Debug/sanitizados concurrentes no son un benchmark de capacidad RT.

### Preparación musical exacta

`TempoBpm::value` y la persistencia permanecen en double. Para DSP, sus bits
binary64 y los del project sample rate se descomponen directamente durante la
preparación. Cada segmento conserva un mapping racional exacto con:

`framesPerTick = projectRate / (256 * BPM)`

El primer anchor es cero y cada anchor posterior se obtiene evaluando exactamente
el segmento previo en el tick del cambio. No se calculan primero `60/BPM`,
segundos acumulados ni frames double. Los segmentos retienen denominadores
independientes; no existe un denominador global del tempo map.

`PreparedTemporalContext` obtiene de esa tabla los beats y los dos extremos del
loop. Los reduce a `RationalBoundary`, certifica su integración con el
`ClockFormat` y publica una sola estructura inmutable. RealtimeAudioEngine,
TransportReducer y la proyección usan esos mismos límites; el metrónomo programa
el click desde los mismos mappings exactos. Los doubles de loop conservados son
solo presentación y no vuelven a autoridad DSP.

Una combinación cuyo anchor, integral, LCM con el reloj o checkpoint no cabe en
los componentes UInt128/temporales UInt256 se rechaza antes del commit. La prueba
de comando comprueba que modelo, revisión temporal y contexto RT anterior se
conservan. No hay cuantización ni fallback aproximado.

El caso obligatorio 48000 Hz/123 BPM produce exactamente 960000/41 frames por
negra. `MusicalPreparationOracleTests.py` usa `Fraction` y comprueba 216 fronteras
contra tres binarios: normal, backend portable forzado y compilación
fast-math/fp-contract. Incluye los dos binary64 vecinos, tasas binary64 aleatorias
y mapas multisegmento. Las pruebas C++ añaden frontera anterior/exacta/posterior,
loop fraccionario mediante processBlock, independencia del particionado,
discontinuidad, coincidencia RT/proyección, scheduling tras cambio de tempo y
rechazo transaccional por capacidad.

La inicialización hardware-free del adaptador JUCE descubrió una regresión: antes
del primer plan no existe un reloj contra el que prevalidar el contexto. El
adaptador establece una vez el motor portable vacío al sample rate lógico del
proyecto y certifica sobre él el contexto inicial. Los commits temporales
posteriores conservan la ruta normal de checkpoint. La prueba de integración JUCE
que fallaba pasa de nuevo; no cambia semántica musical ni de transporte.

Dictamen de implementación: **APTO PARA AUDITORÍA FINAL**. No se cambian
versiones, commits, tags ni se realiza push.

### Archivos de esta migración

Añadidos: `audio/TemporalInteger.h`, `audio/ExactTemporal.h`,
`audio/ExactProjectPhase.h`, `tests/TemporalIntegerOracleDriver.cpp`,
`tests/TemporalIntegerOracleTests.py`, `tests/MusicalPreparationOracleDriver.cpp`
y `tests/MusicalPreparationOracleTests.py`.

Actualizados (algunos ya eran nuevos/modificados en el candidato anterior):
`CMakeLists.txt`, `README.md`, `docs/architecture.md`, este documento;
`audio/DspFramePosition.h`, `audio/RealtimeProjectClock.h`, `audio/TrackRenderer.h`,
`audio/PreparedProject.h`, `audio/PreparedProcessingPlan.h/.cpp`,
`audio/PreparedTemporalContext.h/.cpp`, `audio/RealtimeAudioEngine.h/.cpp`,
`transport/TransportReducer.h`, `timeline/Time.h/.cpp`,
`platform/juce/JuceAudioDeviceAdapter.cpp`,
`musical/MusicalTime.h/.cpp`,
`tests/TransportTimelineFoundationTests.cpp`, `tests/RealtimeAudioEngineTests.cpp`
y `tests/LoopMetronomeTests.cpp`, `tests/MusicalTimeTests.cpp` y
`tests/CommandFlowTests.cpp`. Rutas de producción relativas a src/vitadaw.
Los demás cambios visibles frente a HEAD pertenecen al candidato preexistente.

### Ejecución final de este estado

Tras la preparación musical exacta y la corrección de inicialización del adaptador,
se volvieron a construir y ejecutar todas las configuraciones:

| Configuración | Resultado | Tiempo total de CTest |
| --- | --- | --- |
| Build completo Debug, JUCE incluido | 38/38 | 18.83 s |
| Core-only Debug | 37/37 | 34.32 s |
| ASan + UBSan | 37/37, sin diagnósticos | 120.03 s |
| UBSan | 37/37, sin diagnósticos | 97.59 s |
| TSan | 37/37, sin diagnósticos | 178.60 s |

Comandos: `cmake --build <build-dir> -j 2` seguido de
`ctest --test-dir <build-dir> --output-on-failure -j 2`, con directorios build,
build-core, build-asan, build-ubsan y build-tsan. Se conservaron las configuraciones
sanitizer existentes. Las builds/tests de distintos directorios se ejecutaron
concurrentemente; estos tiempos no deben interpretarse como rendimiento DSP.

Ejecuciones dirigidas adicionales correctas: los tres oráculos temporales
(8688 comprobaciones cada uno), los tres oráculos musicales (216 fronteras cada
uno), MusicalTimeTests, LoopMetronomeTests, ProcessorInsertCoreTests y
CommandFlowTests. `vitadaw_project_media_integration_tests` reprodujo primero el
fallo de inicialización descrito y pasa tras la corrección localizada.

`git diff --check` limpio. HEAD permanece en
4df6f98e2b6b4575447cb11a2b1ece19fe3e14f6, tag
v0.5.6-editing-transport-hardening. CMake, writerAppVersion y
getApplicationVersion permanecen en 0.5.6. No se hizo commit, tag ni push.
No se realizó smoke acústico; este trabajo valida preparación y decisiones
discretas con render offline/hardware-free. Los tiempos sanitizados no son un
benchmark RT profesional.

## Corrección localizada posterior a la auditoría final (sin versionar)

### Reproducciones antes de corregir

Se añadieron primero `TemporalJuceIntegrationTests.cpp` y el caso
`crossedLoopStartMetronome` de `LoopMetronomeTests.cpp`. Fallaron bootstrap,
checkpoint activo, rollback y audio de las vueltas del loop. El caso lifetime
no fallaba sin instrumentación; ejecutado con ASan+UBSan reprodujo un
`heap-use-after-free` en `RealtimeAudioEngine::prepareLegacyDeviceRate`, al
consultar el contexto destruido por `reprepareTemporalForCurrentDevice`.
No se interpreta que un test no sanitizado que pasa descarte un lifetime inválido.

### Causas y correcciones

1. **Bootstrap:** `initialise` ocurre antes de DawApplication. La preparación de
   dispositivo certifica ahora el motor vacío en ese orden, adoptando la tasa
   del dispositivo solo cuando aún no existe tasa lógica de proyecto. No se
   relajan `canConfigureTemporalContext` ni los certificados. El harness usa
   `beginDeviceReinitialisation` y `reprepareForCurrentDevice`, las mismas entradas
   privadas de la aplicación nativa, antes de construir DawApplication.
2. **Lifetime:** cierre y sustitución mantenían vistas crudas del contexto. La
   retirada explícita `releasePreparedReferences` ocurre bajo quiescencia y con
   los owners antiguos todavía vivos. Solo después se sustituyen/destruyen.
   Incluye referencias de plan/runtime/processors, no únicamente el puntero
   temporal. Los callbacks controlados de retirada conservan fase, y los
   checkpoints se leen después de retirar el consumidor, no concurrentemente
   con `processBlock`.
3. **Metrónomo:** mirar solo la posición después del wrap descartaba el beat
   cruzado. `pendingMetronomeWrap_` conserva esa discontinuidad hasta el próximo
   scheduling real, que incluye el intervalo desde loopStart. No cambia el
   overshoot ni ninguna frontera; eventos en una misma muestra se coalescen
   mediante la política existente. El oráculo de audio compara todas las muestras
   de 100000 frames contra clicks en `ceil(k * 960000 / 41)`, incluidas cinco
   entradas de vuelta, silencios y ausencia de duplicados, con particiones
   100000, 127 y 1024. Comprueba también cero allocations RT.
4. **Checkpoint:** el certificado utiliza el ClockFormat extendido del contexto
   vigente y valida su compatibilidad con el plan candidato. La secuencia loop
   fraccionario -> wrap -> Stop -> sustitución estructural conserva `15/41`
   exactamente. Una tasa de plan distinta de la del contexto no puede instalarse
   por esa ruta de commit de plan aislado.
5. **Reprepare:** plan, contexto y conversión del checkpoint se preparan y
   certifican antes de tocar owners o políticas. El caso BPM
   `0x1.ec00000000001p+6`, loop de una negra, 48000 -> 44100 Hz requiere pasar
   de D extendido de 125 a 133 bits y se rechaza. El test verifica identidad de
   ambos owners, revisión, posición/fase exacta y políticas anteriores. Un error
   físico cierra/cancela la generación y deja el dispositivo no operativo; no se
   promete restaurar físicamente el dispositivo antiguo. El checkpoint lógico
   se conserva en esta ruta de fallo de preparación.

### Orden de publicación y destrucción

Preparar candidatos completos -> certificar formato efectivo, fuentes y
checkpoint -> retirar referencias del motor con callback quiescente -> swap
de owners -> configurar motor/contexto y restaurar checkpoint. Desde ese punto
los propietarios retirados pueden destruirse fuera de RT; la reanudación del
consumidor siempre sucede después de instalar el conjunto válido. Si la preparación o
certificación falla, no se alcanza la retirada/sustitución. Los commits
documentales siguen ejecutándose únicamente tras instalar su recurso válido.

### Harness JUCE y alcance de la validación

Casos CTest nuevos: `vitadaw_temporal_juce_bootstrap`,
`vitadaw_temporal_juce_lifetime`, `vitadaw_temporal_juce_checkpoint` y
`vitadaw_temporal_juce_rollback`. Usan el adaptador JUCE real y `processBlock`,
sin abrir un dispositivo físico. Lifetime cubre cambio de tasa sin plan,
device-unavailable, vuelta de tasa, instalación de plan, cierre efectivo y
cierre repetido. Checkpoint cubre además la notificación de retirada controlada.
El harness no pretende validar el driver CoreAudio ni audibilidad física.

Se creó `build-asan-juce` con `VITADAW_BUILD_APP=ON`, flags C++
`-fsanitize=address,undefined -fno-omit-frame-pointer` y linker
`-fsanitize=address,undefined`, reutilizando las fuentes JUCE locales. Se
compilaron tanto el harness nuevo como `vitadaw_project_media_integration_tests`.
Esto instrumenta el adaptador y las unidades C++/Objective-C++ JUCE enlazadas,
además del núcleo; no se presenta como evidencia core-only. Las suites core
ASan+UBSan, UBSan y TSan se mantienen por separado. El entorno sandbox puede
emitir avisos JUCE/CoreMIDI al construir AudioDeviceManager; no se abre audio
físico ni se han añadido funcionalidades MIDI.

### Archivos de esta corrección

Nuevo: `tests/TemporalJuceIntegrationTests.cpp`.
Modificados: `CMakeLists.txt`, `README.md`, `docs/architecture.md`, este documento,
`src/vitadaw/audio/RealtimeAudioEngine.h/.cpp`,
`src/vitadaw/platform/juce/JuceAudioDeviceAdapter.h/.cpp` y
`tests/LoopMetronomeTests.cpp`.
Los otros cambios del working tree pertenecen al candidato anterior. No se han
modificado UInt128/UInt256, aritmética de reloj/render, BPM persistente ni versión.

El scheduler sigue acotado a 8191 segmentos por subbloque global, no por pista.
El indicador de wrap no añade un segundo recorrido. Sigue pendiente un benchmark
profesional y un smoke con dispositivo físico; no se infieren de los sanitizers.

### Resultados finales de esta corrección

| Configuración | Resultado |
| --- | --- |
| Build completo Debug, aplicación y JUCE | 43/43 (21,94 s) |
| Build core-only Debug | 37/37 |
| ASan + UBSan core-only | 37/37 (120,83 s), sin diagnósticos sanitizer |
| UBSan core-only | 37/37 (98,10 s), sin diagnósticos sanitizer |
| TSan core-only | 37/37 (181,18 s), sin diagnósticos sanitizer |
| ASan + UBSan integración JUCE | 6/6, sin diagnósticos sanitizer |
| Oráculos temporales normal / portable / fast-math | 8688 por binario |
| Oráculos musicales normal / portable / fast-math | 216 por binario |
| git diff --check | limpio |

Las suites se ejecutaron con `cmake --build <dir> -j 3` y
`ctest --test-dir <dir> --output-on-failure -j 2` (o `-j 3` para Debug).
En build-asan-juce se construyeron los targets
`vitadaw_temporal_juce_integration_tests` y
`vitadaw_project_media_integration_tests`, y se ejecutó CTest con
`-R 'temporal_juce_|project_media_integration'`. Los seis oráculos también se
repitieron con `ctest --test-dir build-core -R oracle -V`.
No se extrapola TSan core-only a los callbacks del driver nativo.

Dictamen de implementación: **APTO PARA VERSIONADO**, no congelado ni
versionado. HEAD sigue en 4df6f98e2b6b4575447cb11a2b1ece19fe3e14f6 y las tres
identificaciones de versión permanecen en 0.5.6. No se ha hecho commit/tag/push.

## Cierre de la política runUntilStop

La auditoría de cierre detectó que `TemporalCheckpoint` guardaba posición, fase,
playback, loop y metrónomo, pero reconstruía `runUntilStop` como
`metronomeEnabled && !loop`. Esa inferencia pierde la semántica existente cuando
el metrónomo se desactiva durante Playing: el reloj debe continuar abierto hasta
Stop aunque el flag visible del metrónomo ya sea falso.

`TemporalCheckpoint` contiene ahora `runUntilStop`. `temporalCheckpoint()` lo
captura mediante `RealtimeProjectClock::isRunUntilStop()` y
`restoreTemporalCheckpoint()` lo entrega explícitamente a `setPlaybackPolicy`.
No cambia ninguna transición normal que active o desactive la política.

El nuevo caso `vitadaw_temporal_juce_run_until_stop` usa el adaptador JUCE real:
proyecto vacío a 48 kHz, BPM `nextafter(123)`, loop de una negra preparado pero
deshabilitado, metronome on -> Play -> metronome off. Comprueba explícitamente
`Playing`, flags loop/metronome falsos y `runUntilStop=true`; fuerza el rechazo
48 -> 44,1 kHz, verifica checkpoint exacto, revisión y owners; recupera 48 kHz,
procesa audio real y confirma que sigue Playing hasta un Stop explícito.

Para que esta última comprobación ejercite el callback y no se limite a observar
un reloj inmóvil, el helper de integración reproduce también el paso real de
`attachAudioCallback(true)`: tras un reprepare correcto notifica al motor la
entrada en `initializing` antes de confirmar el consumidor. Es una corrección
del harness; no altera el lifecycle de producción.

Archivos tocados para este cierre: `CMakeLists.txt`, `docs/architecture.md`, este
documento, `src/vitadaw/audio/RealtimeAudioEngine.h/.cpp` y
`tests/TemporalJuceIntegrationTests.cpp`.
