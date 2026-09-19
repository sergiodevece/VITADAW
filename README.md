# VitaDAW 0.7.3 — Device Latency, Recording Placement & Physical Loopback Validation

Base arquitectónica para un DAW nativo de escritorio, construida de forma
incremental. La aplicación actual abre una ventana mínima, inicializa y observa
el dispositivo de audio y carga y reproduce una colección variable de pistas WAV
sincronizadas. El incremento 0.2.0 sustituye la suma directa al master por un
plan portable preparado con buses estéreo, destinos de pista y metering de bus.
El incremento 0.2.1 convierte esos buses en canales funcionales con gain,
balance, mute, solo y smoothing sample-accurate. VitaDAW 0.2.2 permite que la
salida principal de un bus alimente otro bus mediante un DAG validado y ordenado
completamente fuera del hilo de audio. VitaDAW 0.2.3 añade múltiples sends por
pista hacia buses existentes, con taps pre/post, nivel suavizado, mute propio y
Solo resuelto por rama. VitaDAW 0.2.4 activa además sends cuyo origen es un bus,
los integra en el DAG y conserva un único procesamiento de cada bus aunque tenga
varias ramas.
VitaDAW 0.3.0 incorpora el primer contrato DSP portable y cadenas de inserts en
pistas, buses y Master. El procesador interno `internal.gain` se ejecuta por
subbloques, admite parámetros suavizados y bypass con latencia preservada. No se
aloja ningún plugin externo ni se compensa todavía la latencia entre rutas.

VitaDAW 0.4.0 sustituye el recurso implícito por pista por el modelo explícito
`AudioSource -> AudioClip -> AudioTrack`. Una fuente PCM inmutable se prepara
una sola vez por `SourceId`; cualquier número de clips puede reutilizarla con
inicio, offset y duración propios. Los solapes se suman de forma determinista
antes de ejecutar una sola vez los inserts y el mixer de la pista.

VitaDAW 0.4.1 añade las primeras operaciones portables de edición no
destructiva: Move, Duplicate, Split, Trim Left/Right y Delete. Todas localizan
clips por `ClipId`, se rechazan durante Play, reutilizan el PCM por `SourceId` y
publican un plan nuevo mediante la transacción estructural existente. La fila
de botones de edición de la ventana es únicamente un soporte provisional de
validación; todavía no existe timeline visual.

VitaDAW 0.4.2 añade Undo/Redo portable para esas seis operaciones de clips.
El historial vive en `ProjectSession`, coordinado por `DawApplication`, y guarda valores de modelo e IDs estables
y participa en el commit transaccional. Cada Redo restaura los mismos IDs sin
retroceder sus contadores ni decodificar PCM. Solo funciona con transporte
detenido. Los botones Undo/Redo son provisionales.

El historial tiene un máximo de 512 entradas y 8 MiB aproximados. Una edición
nueva confirmada descarta Redo; un fallo conserva la rama. En esta versión,
una mutación persistente todavía no undoable (import, mixer, routing, send o
processor) limpia ambos lados del historial. Play/Stop lo conservan.
`StateToken` identifica estados lógicos y `revision` cuenta commits, incluidos
Undo/Redo. Desde 0.4.3 el token guardado permite calcular dirty sin comparar PCM.

VitaDAW 0.4.3 añade Save, Save As y Load, únicamente en Stopped. Los botones
provisionales guardan archivos `.vitadaw`: JSON UTF-8 determinista, esquema v1,
modelo editable completo e IDs/contadores exactos como strings decimales.
`ProjectSession` reúne modelo, historial, ruta del documento y savedStateToken.
Save conserva Undo; Load adopta un documento limpio con historial vacío y un
token nuevo. Load requiere confirmar el descarte si la sesión tiene cambios.

Cada Source conserva SHA-256 y tamaño de los bytes realmente decodificados.
Load prueba ruta relativa al documento y luego fallback absoluto; medios ausentes
o modificados hacen fallar toda la carga. El PCM candidato queda aislado del
proyecto activo incluso cuando ambos usan SourceId=1. Save As recalcula rutas
sin cambiar el nombre del proyecto ni recalcular hashes de medios modificados.

El backend macOS guarda en un temporal exclusivo del mismo directorio,
sincroniza, hace rename atómico y sincroniza el directorio. Un fallo anterior al
rename conserva el archivo previo; un fallo posterior devuelve
`durabilityUncertain`, sin afirmar rollback ni marcar clean.
No hay autosave, backups, relink, paquetes, medios embebidos ni persistencia
asíncrona. Detalles y pruebas: [validación 0.4.3](docs/validation-0.4.3.md).

VitaDAW 0.5.0 reemplaza los botones de edición por una timeline visual funcional.
Las pistas vacías y los clips del proyecto se dibujan como primitivas JUCE, con
regla en segundos, playhead, zoom y scroll. Selección y preview de drag son estado
efímero; Move y Trim emiten exactamente un comando al soltar. Split usa el
playhead, Duplicate crea un clip contiguo y Delete conserva la fuente. Undo/Redo
y Save/Load reconstruyen la vista desde un `TimelineSnapshot` portable.

No existe Seek todavía: la regla solo observa el transporte. Tampoco hay
waveforms, snapping musical, multiselección ni edición durante Play. Detalles y
pruebas: [validación 0.5.0](docs/validation-0.5.0.md).

El release blocker de lifecycle de la build Debug 0.5.0 está corregido sin
cambiar de versión. JUCE puede llamar a `shutdown()` sin haber llamado antes a
`initialise()` cuando rechaza una segunda instancia. El cierre usa ahora la
presencia real de cada owner, es idempotente y aplica un orden explícito:
detener productores de eventos, retirar callbacks, aquietar el dispositivo,
destruir UI/dispatcher/aplicación y destruir finalmente el adaptador. Los
diagnósticos distinguen arranque completo, fallo de dispositivo, fallo de
startup y cierre parcial, siempre fuera del callback RT.

VitaDAW 0.5.1 completa el transporte inicial con `Stopped`, `Playing` y
`Paused`, Seek por project frame, primer Stop conservando posición y segundo
Stop volviendo a cero. Play continúa desde la posición actual; desde el final
natural reinicia a cero. La regla permite click-to-seek y la UI muestra
`mm:ss.xxx` y el frame lógico. Space alterna Play/Pause y Home/End navegan al
inicio/final. Seek durante Playing se rechaza deliberadamente en esta versión.

El proyecto mantiene ahora una escala temporal explícita. Su sample rate se fija
al crear el proyecto: usa el del dispositivo activo y, si la apertura falla,
usa `48000 Hz` como valor de reserva. No cambia automáticamente si después se
reconfigura el dispositivo.

VitaDAW 0.5.2 añade `MusicalTimeMap` portable: PPQ fijo 15360, tempo step
(20–400 BPM, también fraccional) anclado a ticks y métrica anclada a compases.
El reloj maestro de project frames sigue siendo la única autoridad: cambiar
tempo no mueve ni estira clips, no amplía la duración y conserva el playhead.
Los ocho comandos musicales tienen Undo/Redo y requieren **Stopped** (no Paused).
La representación preparada es inmutable, se consulta fuera de RT y no se
reconstruye a cada refresco visual.

El ruler permite Seconds, Frames y Bars / Beats sobre el mismo eje absoluto.
El transporte muestra además `Bar|Beat|Tick` (bar/beat desde 1). Tres botones
provisionales permiten fijar el tempo inicial a 100, añadir 60 BPM en quarter 17
y añadir 7/8 en bar 9; repetir una adición rechaza el ancla duplicada.
Save escribe **schemaVersion 2**; Load valida y migra v1 en memoria a 120 BPM,
4/4 y PPQ15360, sin modificar el archivo original ni marcar dirty la sesión.
Consulta los contratos y límites en `docs/architecture.md` y la prueba real en
[`docs/validation-0.5.2.md`](docs/validation-0.5.2.md).

VitaDAW 0.5.3 añade un rango documental de loop `[startTick,endTick)` cuya
única autoridad son ticks PPQ15360. Se compila fuera de RT junto al mapa musical
y ambos se publican como una sola revisión inmutable. Loop enabled es estado de
sesión: no se guarda, no entra en Undo y no marca dirty. El callback divide cada
bloque en tramos temporales monótonos y conserva el exceso fraccional al
envolver, sin redondear la duración a frames de dispositivo por vuelta.

El metrónomo enumera beats desde el mismo mapa, prepara dos bursts internos al
sample rate del dispositivo y los reproduce desde un pool fijo de cuatro voces.
El acento aparece solo al inicio de compás. El click se suma tras Master Inserts
y antes de Master Gain/Meter. Enabled y nivel son estado de sesión aplicado por
la cola RT y pueden cambiar durante Play sin alterar historial. La política de
playback queda separada de la duración de clips, por lo que loop o metrónomo
pueden mantener activo un proyecto vacío o una región más allá del contenido.
Save escribe **schemaVersion 3** y v2 migra a un rango nulo. Detalles:
[`docs/validation-0.5.3.md`](docs/validation-0.5.3.md).

El release blocker de importación manual de 0.5.3 queda corregido sin cambiar
de versión. El callback asíncrono del selector usa `SafePointer`, conserva el
chooser durante la operación y trata Cancel como un resultado explícito. Una
ruta elegida siempre llega al backend de archivos; cualquier fallo produce un
diagnóstico provisional visible y conserva el proyecto y su `StateToken`.
Import sigue siendo una barrera no undoable: solo el commit exitoso marca dirty.

VitaDAW 0.5.4 incorpora el ciclo estructural completo de pistas de audio. Los
botones provisionales crean pistas mono o estéreo con nombre `Audio N`, identidad
monotónica, mixer e inserts por defecto, sends vacíos y salida directa a Master.
Delete retira atómicamente la pista, sus clips, inserts, route y sends de origen,
pero conserva las fuentes compartidas y los buses no relacionados. Add y Delete
son Stopped-only y undoables; Undo restaura exactamente los mismos TrackId,
ClipId, SendId y ProcessorInstanceId sin rebobinar contadores.

La selección de timeline mantiene un `selectedTrackId` efímero. Import exige esa
selección cuando existen pistas y conserva la creación automática de la primera
pista para un proyecto vacío. El gesto Move admite ahora tiempo y lane destino
en un solo comando: el clip cambia de owner sin cambiar ClipId, SourceId, offset
ni duración. Mono→mono y stereo→stereo están permitidos; no existe upmix/downmix.
Las operaciones reconstruyen el plan fuera de RT y conservan la posición de
transporte detenida. El formato de proyecto continúa siendo schema v3.

VitaDAW 0.5.5 añade una caché derivada de formas de onda por `SourceId`, propiedad
de la sesión y completamente ajena a `ProjectState`. JUCE decodifica cada WAV
una sola vez; inmediatamente después, fuera de RT, el núcleo portable obtiene
picos min/max por canal en buckets base de 128 source frames y construye niveles
superiores agregando pares. Diez clips de una misma fuente comparten el mismo
handle inmutable, incluidos Duplicate, Split, Trim, Move, Delete/Undo y cambios
de pista.

La timeline selecciona el nivel según source-frames-per-pixel, recorre solo la
franja visible y convierte project frames a source frames usando los sample
rates lógico y de fuente; el device rate no participa. Mono se dibuja centrado
y estéreo conserva picos L/R independientes. `paint()` nunca lee archivos,
decodifica ni genera caché. El presupuesto provisional es 64 MiB: si se supera,
el audio sigue preparado y se muestra un placeholder/diagnóstico. Load crea una
caché candidata aislada y la intercambia junto al proyecto; schemaVersion sigue
siendo 3 y el waveform no se persiste. Detalles y pruebas:
[`docs/validation-0.5.5.md`](docs/validation-0.5.5.md).

VitaDAW 0.5.6 no añade funciones de producto. Endurece transporte, loops,
edición, Undo/Redo y persistencia mediante un harness hardware-free que utiliza
el `RealtimeAudioEngine`, el compilador de planes y `processBlock` reales. La
matriz existente permanece intacta: Seek durante Playing se rechaza; durante
Paused se admite; el primer Stop conserva posición y el segundo vuelve a cero;
las ediciones de clip en la misma pista y Undo/Redo pueden ejecutarse en Paused,
pero Move entre pistas, Add/Delete Track y Save/Load requieren Stopped.

La nueva cobertura combina comandos pendientes, FIFO lleno, cancelación por
lifecycle, loop mínimo válido, loops a través de tempo/métrica, fronteras de
bloque, splits extremos, cadenas largas de historial, round-trip documental y
64 pistas/1000 clips positivos con pistas vacías y huecos. El hardening detectó
y corrigió una discrepancia de tolerancia entre `ProjectState` y la preparación
del plan para duraciones 44,1↔48 kHz; no se modificó el callback ni el render.
Detalles: [`docs/validation-0.5.6.md`](docs/validation-0.5.6.md).

VitaDAW 0.6.0 refuerza los fundamentos temporales. `ProjectFramePosition`
entero es la única posición
autoritativa; los segundos son solo presentación y el residuo fraccionario del
reloj queda confinado a la conversión device/project. La duración describe el
contenido, no el dominio navegable: un Seek detenido o pausado puede situarse
después del último clip sin crear contenido. El límite técnico provisional es
`maximumSupportedProjectFrame()` (2^53−1), encapsulado como límite del dominio
numérico certificado actual y no como definición conceptual del timeline.

Play, Pause, Stop y Seek se proyectan mediante un reductor portable siguiendo el
orden de comandos realmente aceptados por el motor. Así dos Stop pendientes
conservan la semántica de segundo Stop y una ráfaga de Seek termina en el último
destino. Seek durante Playing sigue rechazado provisionalmente. Un Seek aceptado
marca una discontinuidad específica que permanece pendiente durante Pause y se
entrega al primer bloque DSP posterior, sin llamar a `reset()` universalmente.
Si antes hay una reconstrucción, `hardDiscontinuity` subsume esa notificación.
El reductor se utiliza también en RT: el reloj aplica sus efectos, y la proyección
reconcilia snapshots parciales mediante replay puro de un registro acotado de
comandos aceptados. La aceptación requiere espacio en FIFO y registro.
Los identificadores de comandos son tickets de reserva/resolución: una reserva
rechazada puede dejar un hueco o aparecer en el watermark, sin ejecutar su acción.
Las fronteras usan entero/residuo y el render conserva esa separación hasta
calcular coordenadas locales; un locator redondeado al final no implica que DSP
haya terminado. La proyección no garantiza observación instantánea de RT.
La disponibilidad de Play procede del snapshot más replay y se valida contra la
generación del claim, sin flags auxiliares. Las coordenadas fuente conservan el
resto racional hasta comprobar límites; el final natural no usa una epsilon anticipatoria.
La migración numérica usa componentes de 128 bits y temporales fijos de 256,
backend portable y oráculo independiente Python/Fraction (también con fast-math).
El render decide índice y pertenencia antes de interpolar. El scheduler RT del
metrónomo también usa datos preparados y comparaciones enteras.
La preparación DSP del mapa musical interpreta los bits binary64 contractuales
de project rate y BPM sin evaluar antes segundos ni frames en floating point.
Los anchors de tempo, beats, límites de loop y eventos de metrónomo comparten
ahora una integral racional exacta preparada fuera de RT. Las APIs de ruler,
etiquetas y presentación conservan sus doubles, pero no realimentan decisiones
discretas de audio. Las configuraciones cuya integral o integración con el reloj
exceda la capacidad fija 128/256 se rechazan transaccionalmente, sin aproximación.
El reloj base admite un denominador de hasta 125 bits y el extendido musical
hasta 128, sujeto a certificación conjunta. El adaptador JUCE prepara plan,
contexto y checkpoint antes de reemplazarlos; el checkpoint conserva
explícitamente la política efectiva `runUntilStop`, sin inferirla de los flags
visibles de loop o metrónomo. El adaptador retira las referencias del motor
antes de destruir sus propietarios. Un rechazo conserva el estado DSP anterior,
aunque el dispositivo físico puede quedar indisponible. La validación incluye
un harness JUCE sin hardware para bootstrap, lifetime, checkpoint, reprepare y
rollback de `runUntilStop`;
no equivale a un smoke acústico ni a un benchmark RT profesional.
Detalles: [`docs/validation-0.6.0.md`](docs/validation-0.6.0.md).

VitaDAW 0.6.1 consolida Musical Time como una vista bidireccional exacta
sobre la autoridad de project frames. `MusicalTickPosition` es la coordenada
musical canónica y `MusicalPosition` su descomposición bar/beat/tick. El mapa
preparado convierte tick→posición exacta y posición exacta→tick mediante
aritmética fija 128/256; las decisiones de segmento, frontera y redondeo no
dependen de floating point. Los segmentos de tempo son half-open y una posición
igual al anchor siguiente pertenece siempre al segmento nuevo. Los doubles se
conservan exclusivamente para segundos, negras y píxeles de presentación.

Todo `PreparedMusicalTimeMap` publicado queda certificado para la ruta exacta.
Un documento inválido, una coordenada fuera de rango, un resultado que exceda
el timeline soportado y una configuración que no cabe en la capacidad exacta
producen errores distintos. La inversa queda acotada a una búsqueda de segmento
y un máximo de 41 comparaciones de ticks. Grid, loop y metrónomo comparten los
mismos anchors exactos; no se añade un segundo reloj ni estado musical mutable.
El schema permanece en v3 y los BPM conservan exactamente sus bits binary64.

VitaDAW 0.6.2 endurece Loop Foundation sobre la autoridad temporal existente:
formaliza `[startTick,endTick)`, conserva el preroll anterior a loopStart, aplica
wrap racional con overshoot íntegro y valida enable/disable contra el transporte
proyectado. Una vista preparada read-only entrega a timeline rango documental,
fronteras exactas/de presentación y revisión coherentes. Los rebuilds pueden
cancelar voces del metrónomo, pero preservan la obligación musical de un click de
loopStart atravesado para emitirlo exactamente una vez al reanudar. El scheduler
de hasta 8191 segmentos continúa acotado y pendiente de benchmark profesional.

VitaDAW 0.6.3 consolida el metrónomo como función de producto sin sustituir
esa infraestructura. `metronomeEnabled` y el nivel permanecen como estado de
sesión no persistente, con defaults disabled y -12 dB. El beat es la figura del
denominador y solo el primer beat de cada compás recibe accent: 7/8 produce un
accent y seis clicks normales, sin agrupación 2+2+3. Pause cancela cualquier
cola de click ya iniciada pero conserva una obligación musical todavía no
emitida; Resume no reanuda PCM antiguo ni inventa un evento.

El click conserva su posición explícita en la cadena: Master Inserts →
Metronome → Master Gain → Master Meter/Output. Un proyecto vacío con metrónomo
activo puede reproducirse hasta Stop. Desactivarlo durante esa reproducción
silencia nuevos clicks sin ejecutar Stop ni retirar `runUntilStop` de la sesión
en curso. `MetronomeReadModel` publica enabled, nivel y revisión temporal como
un único valor coherente de presentación.

VitaDAW 0.6.4 consolida la edición estructural del arrange existente sobre los
contratos temporales certificados de 0.6.x. Add/Delete Audio Track y Move Clip
horizontal o entre pistas conservan su flujo transaccional: comando, modelo
candidato, preparación fuera de RT y commit conjunto de modelo, plan e
historial. No se añaden comandos, subsistemas ni políticas de edición nuevas.

Toda creación, restauración o edición de un clip valida ahora tanto
`projectStart` como su final exclusivo mediante el dominio definido por
`maximumSupportedProjectFrame()`. Un inicio o final fuera de ese dominio se
rechaza como `invalidPosition` antes de preparar el processing plan, sin cambiar
modelo, plan ni historial. Continúan permitidos los huecos, los movimientos más
allá del contenido actual, los clips adyacentes y los solapes parciales o
completos; los solapes se suman antes de los inserts de la pista. El schema
documental permanece en v3.

VitaDAW 0.6.5 añade reorder documental de pistas por IDs estables y edición
atómica de varios clips. La selección múltiple es efímera y se reconcilia por
`ClipId`; Move horizontal, Delete y Duplicate canonicalizan el conjunto,
validan completamente el candidato y publican una sola preparación, commit y
acción de historial. Duplicate desplaza el bloque por su ancho temporal total,
incluidos huecos, y devuelve sus nuevos IDs monotónicos para seleccionar el
resultado. El orden del array de pistas conserva el reorder en Save/Load sin
cambiar schema v3 ni el orden DSP independiente por `TrackId`.

VitaDAW 0.6.6 completa la base del arrange con una Time Selection half-open
efímera y Snap de interacción desactivado por defecto. El ruler conserva click
como Seek y usa drag para seleccionar tiempo; la selección sobrevive a edición y
Undo/Redo, coexiste con clips/pista activa y se limpia al cargar otro proyecto.
Snap resuelve en project frames definitivos contra cero, beat, playhead, bordes
de clips y de Time Selection, con desempate estable e independiente del orden de
los snapshots. Move, MoveClips y Trim reciben la posición ya resuelta; Split @
Playhead no cambia.

`Set Loop From Selection` copia start floor/end ceil hacia el comando musical
existente, conserva la selección, no activa el loop y mantiene su contrato
Stopped-only y undoable. Time Selection, Snap y previews no entran en
`ProjectState`, dirty, historial, persistencia ni RT. Un benchmark Release
opt-in mide mutación, preparación, publicación y transacción sin imponer todavía
umbrales. El documento permanece en schema v3.

## 0.7.1 - Recording Recovery Hardening

VitaDAW 0.7.1 endurece la infraestructura de grabación ya introducida en 0.7.0.
El objetivo es que los metadatos auxiliares de recovery no conviertan una toma
válida en un Record rechazado, sin rebajar las barreras que protegen el WAV y el
commit del proyecto.

Durante el preflight no-RT el adaptador intenta persistir un recovery marker
exclusivo junto a la media. Ese marker es auxiliar y **best-effort**: un fallo
exclusivo de creación, escritura, sincronización o cierre no impide preparar el
writer, aceptar Record ni finalizar una toma. El aviso conserva la operación, la
ruta, el código del sistema y su mensaje legible. Una sesión que empieza sin
marker mantiene esa condición degradada de forma explícita, pero puede limpiar,
cerrar la aplicación y permitir otra grabación desde un estado limpio.

La frontera con la media real permanece estricta. La creación y finalización del
WAV, sus escrituras, el `fsync` de media, la publicación sin reemplazo, la
verificación de identidad y el `fsync` del directorio son pasos críticos: un
fallo impide el commit de `ProjectState`. Si un archivo no puede eliminarse con
ownership demostrable de forma atómica, se conserva como artefacto seguro en vez
de borrarse por pathname.

El cleanup terminaliza tanto una captura `prepared` como una que ya está
capturando antes de resetearla. Así no queda una sesión activa retenida entre
preflight, cancelación, shutdown o reintento. Esta ruta no publica ni añade
historial durante el teardown.

La cobertura de integración hardware-free ejecuta
`JuceAudioDeviceAdapter::prepareRecording()` real sobre el `AudioDeviceManager`
productivo y un `AudioIODevice` virtual de test. Demuestra la secuencia de fallo
del marker, writer/captura preparados, solicitud Record aceptada, cleanup y una
segunda preparación correcta; no introduce una ruta alternativa de producción.

En Windows, la persistencia del marker usa `CreateFileW` con `CREATE_NEW`,
`WriteFile`, `FlushFileBuffers` y `CloseHandle`. Las cuatro fases conservan un
diagnóstico diferenciado. Esta rama se revisó por inspección; no se compiló ni
ejecutó con un SDK/toolchain Windows en el entorno macOS de cierre. No se afirma
durabilidad integral de la finalización de grabación en Windows.

El marker, el filesystem I/O y la finalización siguen fuera del callback RT; el
callback continúa limitado a `processBlock`. 0.7.1 no añade recuperación
automática tras crash, escaneo al arrancar, adopción de huérfanos, garbage
collection de media, monitoring, compensación de latencia, punch/loop recording
ni grabación multipista. Detalles de la evidencia de cierre:
[`docs/validation-0.7.1.md`](docs/validation-0.7.1.md).

## 0.7.2 - Input Monitoring Foundation

VitaDAW 0.7.2 añade *software input monitoring* manual, apagado por defecto y
separado de Playback y Recording. Record no habilita Monitoring, Stop Record no
lo deshabilita y Play, Stop, Seek o Loop no cambian su estado. No existe modo
Auto: sólo un Disable explícito, una pérdida real de dispositivo/input/output o
el shutdown pueden apagarlo; una pérdida no se auto-reactiva.

El gain de monitor es efímero, con rango `[-100, 0] dB`, default `-12 dB`,
smoothing RT de aproximadamente 5 ms y publicación *latest-value-wins* para
cambios continuos. La UI mínima despacha Enable, Disable, Toggle y gain a
través del Command System; no modifica `ProjectState`, historial, dirty state
ni persistencia. El medidor de input publica el pico pre-gain y no abre hardware
por sí mismo.

Capture consume siempre input raw antes de cualquier escritura de salida. La
ruta de monitor usa staging estéreo preasignado para proteger aliasing total o
parcial entre input/output, admite la base mono/estéreo y no reserva en el
callback. Un preflight de hardware fuera de RT abre o certifica el input; las
demandas de Recording y Monitoring coexisten y el lifecycle reconfigura de forma
segura ante cambios de device, sample rate o buffer size. Si Monitoring seguía
activo, la finalización de una toma conserva su input y su ruta tras Stop.

Una pérdida real de device, input u output fuerza Monitoring OFF por seguridad.
No hay compensación de latencia, direct monitoring hardware, monitor por pista,
plugins, sends/inserts, buses de monitor, routing avanzado, grabación multipista,
punch/loop recording, MIDI monitoring ni talkback. Detalles de diseño y
validación: [`docs/design-input-monitoring-0.7.2.md`](docs/design-input-monitoring-0.7.2.md)
y [`docs/validation-0.7.2.md`](docs/validation-0.7.2.md).

## 0.7.3 - Device Latency, Recording Placement & Physical Loopback Validation

VitaDAW 0.7.3A expone el buffer efectivo y las latencias de input/output que
reporta el backend, y permite cambiar entre tamaños soportados mediante una
transacción control-side. Requested y effective permanecen separados; un valor
efectivo no se presenta falsamente como soportado. La transacción conserva el
intent de Monitoring cuando tiene éxito y, si falla, sólo declara rollback
correcto tras verificar device/contexto, sample rate, buffer y máscaras I/O.

0.7.3B aplica Recording Placement Compensation al inicio documental del clip,
sin modificar WAV ni PCM. Cada toma congela la latencia de input reportada,
convierte device frames a project frames y combina ese valor con el Recording
Offset manual efímero. Undo/Redo y Save/Load reutilizan la posición ya
comprometida: no recalculan clips históricos al cambiar el dispositivo.

0.7.3C añade Physical Loopback Latency Validation como diagnóstico efímero y
exclusivo. Con Playback detenido, Recording inactivo y Monitoring OFF, configura
temporalmente un par físico output→input, emite una MLS bipolar determinista de
1023 muestras a `-24 dBFS`, captura en device frames y analiza fuera de RT cinco
trials mediante correlación matched. Publica Reported Input, Reported Output,
Reported RTT, Measured Physical RTT, residual signed, min/max/jitter y calidad;
después restaura y verifica la configuración efectiva anterior.

El residual es `measured RTT - reported RTT` y es exclusivamente diagnóstico.
La medida incluye output, conversión/ruta física e input, por lo que 0.7.3 no la
convierte en `Suggested Recording Offset`, no modifica el offset manual y no
auto-calibra Recording Placement. Loopback no atraviesa mixer, plugins, buses,
ProjectState, WAV/Capture musical, historial ni persistencia.

El detector MLS se validó físicamente con una Universal Audio Volt 176 en
macOS/CoreAudio a 48 kHz, Output 1 MONITOR L → Input 1, Direct Monitor OFF y
Monitoring de VitaDAW OFF: 25/25 trials válidos en buffers 64, 128, 256, 512 y
1024, residual estable de `+108` device frames (`+2.25 ms`) y jitter de cero.
La validación ampliada con Aggregate Devices, rutas cross-interface, múltiples
I/O e interfaces adicionales queda pendiente y no bloquea esta foundation.

El smoke también confirmó que un segundo comando Record terminaliza la toma en
Stopped sin Play implícito, y que las capacidades físicas de input permanecen
enumeradas con Monitoring OFF. Detalles: [`docs/validation-0.7.3.md`](docs/validation-0.7.3.md).

## Tecnología propuesta

- **C++20** para el núcleo y el callback de audio.
- **CMake** para builds reproducibles y separación por targets.
- **nlohmann/json 3.12.0**, fijado con SHA-256 en CMake, únicamente dentro de
  persistence; el build core-only sigue sin depender de JUCE.
- **JUCE 9.0.2** como adaptador para ventana y dispositivo de audio. CMake lo
  descarga de su repositorio oficial si no encuentra una instalación local.

JUCE no aparece en el modelo del proyecto ni en el sistema de comandos. Así se
puede probar el comportamiento sin hardware y sustituir un adaptador sin
reescribir el dominio.

## Licencia de JUCE 9

JUCE 9 usa un modelo de licencia dual. Sus módulos pueden utilizarse bajo:

- **AGPLv3**, cumpliendo todas sus condiciones; o
- la **JUCE 9 EULA**, con uno de sus niveles Starter, Indie, Pro o Educational.

A fecha de 11 de septiembre de 2026, JUCE publica Starter gratuito con límite
de ingresos o financiación de 20.000 USD, Indie con límite de 300.000 USD, Pro
sin límite y Educational gratuito sujeto a requisitos y sin uso comercial. Los
niveles, límites, precios, requisitos de distribución y duración deben
comprobarse en la [página oficial de licencias](https://juce.com/get-juce/) y en
la [EULA de JUCE 9](https://juce.com/legal/juce-9-licence/) antes de distribuir.

Este proyecto no adopta todavía una opción de licencia para su distribución.
La integración técnica de JUCE no debe interpretarse como esa decisión.

## Build actual

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

La primera configuración descarga JUCE 9.0.2 si no está instalado. Para
compilar únicamente el núcleo y sus tests, sin JUCE:

```sh
cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

## Ejecución

En macOS:

```sh
open build/vitadaw_app_artefacts/Debug/VitaDAW.app
```

En Windows y Linux, ejecuta `VitaDAW` desde el directorio de artefactos que
muestre CMake al terminar el build.

La ventana muestra provisionalmente el dispositivo de salida activo, sample
rate, tamaño de buffer y canales de entrada/salida disponibles. Si el dispositivo
no puede abrirse, conserva un estado de error coherente y muestra el diagnóstico
fuera del callback de audio.

El motor distingue explícitamente `unavailable`, `initializing`, `operational`,
`stopped` y `error`. `audioDeviceAboutToStart()` solo conduce a
`initializing`: VitaDAW exige además callback registrado y `isPlaying()` del
dispositivo, o la entrada efectiva en `processBlock`, antes de permitir Play.
Retirar y volver a registrar el callback no basta por sí solo para declarar el
dispositivo operativo.

La aceptación usa un gate atómico único que contiene estado y generación. El
productor reclama temporalmente el gate, reserva y escribe el comando todavía
invisible, y lo acepta mediante un CAS final. Ese CAS es el punto de
linearización; el índice de cola se publica después. El cierre de lifecycle
compite sobre el mismo gate: si queda después de la aceptación, su watermark
incluye la secuencia; si queda antes, el CAS de aceptación falla.

Un proyecto nuevo contiene cero pistas, cero fuentes y cero buses, además del
Master, el routing vacío y los mapas musicales por defecto. La interfaz muestra
un único botón provisional `Import WAV...`. La primera importación válida crea
transaccionalmente una pista mono o estéreo según el medio, además de
`AudioSource` + `AudioClip`; no presupone que su `TrackId` sea 1. Tras el commit,
la UI se reconstruye y ofrece importación dirigida a las pistas existentes.

El chooser acepta únicamente archivos `.wav` que `juce::WavAudioFormat` pueda
decodificar. No filtra la selección con una segunda consulta silenciosa de
existencia: entrega la ruta al adaptador, que distingue cancelación, ausencia,
permiso denegado, formato no soportado, fallo de decode, preparación y commit.
Cada import crea el clip en el frame cero;
importaciones posteriores pueden solaparse en la misma pista. `Play` reproduce
simultáneamente todas las pistas disponibles y
`Stop` detiene y vuelve al inicio. Si ninguna pista
tiene un WAV válido preparado, `Play` se rechaza explícitamente.

Las pistas tienen un `TrackId` monotónico, layout mono/estéreo estable y no
tienen relojes propios. Cada `AudioSource` y `AudioClip` posee identidad fuerte
independiente del almacenamiento. Un único reloj de frames de proyecto
determina en cada muestra la posición de cada clip, incluso con sample rates,
inicios, offsets, gaps y solapes distintos.

Cada pista conserva gain de `-100 dB` (silencio) a `+12 dB`, pan normalizado de
`-1` a `+1`, mute y solo. La UI solo envía `SetTrackGain`, `SetTrackPan`,
`SetTrackMute`, `SetTrackSolo` y `SetMasterGain`; no toca el estado RT. El pan
mono usa ley equal-power con `-3 dB` al centro. Para fuentes estéreo, el control
actúa como balance equal-power: el centro conserva ambos canales a unity y cada
extremo atenúa el canal opuesto.

Cada pista puede conservar varios `SendRoute` además de su salida principal.
`SendId` es monotónico y estable; el origen portable admite `TrackId` o `BusId`,
y ambos tipos se ejecutan en 0.2.4. Cada send termina en un `BusId`: el mismo bus
puede recibir outputs principales, Track Sends y Bus Sends, por lo que no existe
una clase Aux distinta.

El tap `PreFaderPrePan` se extrae tras render y antes de gain, pan y mute. Mono
se centra una sola vez a `x/sqrt(2)` por canal y estéreo conserva L/R. El tap
`PostFaderPostPan` incluye gain, pan y mute. Después se aplican el permiso Solo
de la rama, el mute propio y el nivel del send. El pre-send sobrevive a Track
Mute y al fader en silencio por política explícita de esta versión.

En un bus, el tap pre se extrae una sola vez tras acumular todas sus entradas y
antes de Bus Gain, Balance y Mute. El tap post se obtiene una sola vez después
de esos controles. Los sends pre/post y la salida principal distribuyen esos
dos valores ya calculados; añadir fan-out no reprocesa el bus ni vuelve a
avanzar sus smoothers o meter. Bus Mute y Bus Gain a −100 dB silencian main y
post-send, pero el pre-send sigue disponible. El Bus Meter mide únicamente la
señal post-fader/post-mute del canal principal y nunca suma sends.

Si no hay solos, todas las rutas quedan abiertas y cada mute actúa localmente.
Si existe al menos un solo, la aplicación prepara los permisos de las ramas
seleccionadas y de sus caminos necesarios; mute siempre tiene precedencia
local. El gain master usa el mismo rango y se aplica después de la suma.
No existe ya atenuación fija por pista, clamp ni limiter: `float` puede superar
`[-1, 1]` internamente y una salida física puede recortar si se excede su rango.

Los valores dB y coeficientes trigonométricos de pan se convierten fuera del
callback. Un ring SPSC acotado de 64 entradas (63 pendientes utilizables)
publica estados DSP completos y el motor los aplica al inicio de bloque sobre
almacenamiento preasignado. El mismo ring admite eventos triviales de parámetro
y bypass de procesador con generación de plan, índice denso, `ParameterId`,
valor ya validado y offset futuro —cero en 0.3.0—. El estado global de solo
incluye también pistas vacías. Una cola llena rechaza el comando explícitamente
y el modelo no cambia.

Gain y coeficientes de pan por pista, y gain master, usan rampas lineales de
5 ms avanzadas por frame de dispositivo. Su duración no depende del tamaño del
bloque y un nuevo target parte del valor instantáneo. Mute y solo continúan
siendo discretos. El smoothing de gain ocurre en amplitud lineal: es económico,
alcanza silencio exactamente y no representa una velocidad perceptual constante
en dB. El paneo interpola los coeficientes equal-power ya preparados, evitando
trigonometría por muestra; los endpoints cumplen exactamente la pan law, aunque
el trayecto transitorio no conserva potencia exacta.

Cada Send Level usa el mismo rango de `-100 dB` a `+12 dB` y una rampa lineal
sample-accurate de 5 ms. El smoother avanza una vez por frame aunque el send
esté muteado, excluido por Solo o reciba silencio. `SetSendLevel` y
`SetSendMute` se publican por el ring SPSC durante Play; alta y eliminación son
cambios estructurales y exigen transporte detenido.

Los Bus Sends reutilizan exactamente ese estado y cada uno mantiene su propio
smoother. `AddBusSend` conserva tipado explícito sin alterar `AddTrackSend`;
`SetSendRoute` permite cambiar destino/tap únicamente con el transporte parado.
`RemoveSend`, `SetSendLevel` y `SetSendMute` siguen operando por `SendId` para
ambos orígenes.

El motor calcula peak absoluto por bloque para cada pista después de gain, pan y
mute/solo, y para master después del gain master. No hace clamp, por lo que el
meter puede indicar valores superiores a 1. Los snapshots RT se publican en
atomics lock-free y la UI realiza lecturas acotadas sin bloquear el audio. Son
telemetría latest-value: no se conserva cada bloque y una lectura concurrente
fallida devuelve un snapshot vacío coherente. RMS, decay y peak hold quedan para
una versión futura.

`RoutingState` es la única fuente editable de las conexiones. `OutputDestination`
representa Master o un `BusId` estable y monotónico. Cada pista y cada bus tienen
exactamente un destino principal. Los buses son nodos estéreo de acumulación
independientes de WAV, clips y sample rates fuente; Master es el terminal único.

Los cambios estructurales pasan por comandos y solo se aceptan con el transporte
parado. `ProjectState` y `RoutingState` producen una especificación candidata;
el compilador portable valida identidades, destinos, límites y memoria, resuelve
índices densos y crea `PreparedProcessingPlan` junto a `ProcessingPlanRuntime`.
El commit retira el callback, intercambia modelo, plan, buffers y recursos como
una transacción, y vuelve a registrar el consumidor. Un fallo conserva el
proyecto anterior completo.

El plan fija el orden Tracks → buses en orden topológico → Master. Cada pista se
renderiza una sola vez por subbloque. Cada bus se procesa una sola vez después
de recibir todas sus entradas, se mide y entrega su salida a otro bus o Master. Una
capacidad interna preparada de 512 frames divide callbacks mayores sin perder
continuidad de reloj, smoothing ni máximos de metering. Toda reserva, resolución
de IDs y construcción topológica ocurre fuera de RT.

El DAG incluye outputs principales y Bus Sends, incluso si una rama está muteada
o a −100 dB. Para topología, las aristas paralelas origen/destino se deduplican;
el plan DSP conserva todos sus descriptors y suma todas las contribuciones. Cada
descriptor resuelve fuera de RT tipo/índice de origen, bus y buffer de destino,
tap, smoother y permiso de audibilidad, agrupado en rangos pre/post por pista o
bus.

La ventana muestra también, de forma provisional, el estado, posición, duración
y sample rate lógico del proyecto. Al llegar al final natural, el transporte
queda en `Stopped` y conserva la posición en el final; `Stop` explícito continúa
rebobinando a cero.

`RealtimeAudioEngine::processBlock` contiene el reloj, la cola de comandos, el
recorrido del proyecto preparado, el render por pista, la acumulación y la
publicación del transporte. No depende de JUCE. `JuceAudioDeviceAdapter`
conserva la adaptación del dispositivo, la decodificación WAV y el ownership de
los buffers y de la topología preparada.

## Processor & Insert Core 0.3.0

El modelo editable conserva un `InsertChain` dentro de cada `AudioTrack`,
`AudioBus` y Master. Cada `ProcessorState` contiene solo configuración portable:
`ProcessorInstanceId` monotónico, tipo estable, parámetros deseados,
serialized state reservado y bypass. No contiene una instancia DSP viva. Los
targets de comandos son el variant fuerte `TrackId | BusId | MasterTarget`;
el resto de operaciones usa la identidad global del procesador.

La compilación candidata crea instancias nuevas mediante
`IAudioProcessorFactory`, llama a `prepare()` con un `ProcessingFormat` explícito
y resuelve descriptors, rangos de cadena, índices de parámetros, latencias,
tail y scratch. El `PreparedProcessingBundle` posee en exclusiva plan, runtime,
`unique_ptr<IAudioProcessor>`, dos buffers scratch por nodo y las líneas dry de
bypass. El commit sigue siendo transaccional y quiescente: una preparación
fallida conserva modelo, instancias y audio anteriores; el bundle retirado se
destruye fuera de cualquier región RT que pudiera observarlo.

Los flujos quedan fijados así:

```text
TrackRenderer -> Track Inserts -> Track PRE tap -> Gain/Pan/Mute
                                           -> Track POST tap -> Meter/Main

Bus accumulation -> Bus Inserts -> Bus PRE tap -> Gain/Balance/Mute
                                         -> Bus POST tap -> Meter/Main

Master accumulation -> Master Inserts -> Master Gain -> Meter -> Output
```

Una pista mono atraviesa su cadena en mono y solo después se adapta mediante la
ley de pan equal-power existente. Buses y Master procesan estéreo. Track/Bus
Sends conservan sus puntos pre/post, ahora posteriores a los inserts del nodo.
Mute, Solo y fan-out no evitan ni duplican el procesamiento: cada procesador se
invoca exactamente una vez por subbloque de su nodo, aunque la señal sea cero o
la rama no sea audible.

`IAudioProcessor` es distinto de `IRealtimeAudioProcessor`: el primero es el
contrato DSP portable para inserts; el segundo sigue siendo únicamente la
frontera del callback de dispositivo. El contrato DSP recibe vistas de bloque
mono/estéreo, contexto de tiempo y modo realtime/offline, y expone `prepare`,
`processBlock noexcept`, `reset`, latencia, tail y capacidades de aliasing. La
frecuencia de procesamiento coincide con la del dispositivo activo; una
reapertura con otro sample rate recrea y prepara las instancias fuera del
callback antes de volver a publicar el consumidor.

`internal.gain` admite de −100 a +12 dB, con unity en 0 dB, silencio exacto en
−100 dB y smoothing lineal de 5 ms propiedad del propio procesador. El cambio
de parámetro y bypass se aplica al comienzo del callback. El bypass es host-side:
el procesador continúa ejecutándose para conservar su estado y el dry se retrasa
por la latencia declarada antes de sustituir discretamente al wet.

La latencia usa `ProcessingFrameCount` y suma comprobada. El plan conserva
latencia de cada cadena, taps pre/post, cada arista main/send —incluidas ramas
paralelas—, entradas de buses y entrada/salida Master. Las convergencias se
representan como intervalos mínimo/máximo. **0.3.0 no implementa PDC**: las
rutas con latencia diferente permanecen desalineadas de forma deliberada. El
tail se clasifica como none/finite/infinite/unknown, pero aún no se drena tras
el final lógico; los tests de delay añaden silencio dentro del recurso.

`Stop`, fin natural, sustitución de proyecto y restart de dispositivo resetean
procesadores y líneas de bypass antes del siguiente Play. Un Stop→Play rápido
se consume FIFO y el reset actúa como barrera antes de arrancar de nuevo. El
ejecutor realtime y offline es el mismo; solo cambia `ProcessingMode`.

Los límites iniciales son 16 procesadores por cadena, 512 instancias preparadas,
256 pistas, 64 buses, 1024 sends y 32 MiB para plan/runtime/scratch portable.
Se reservan dos buffers estéreo máximos por nodo —las pistas mono usan un canal—;
no existe todavía reutilización avanzada, PDC, automatización, plugins externos,
inserts multicanal, tails audibles ni cambios estructurales durante Play.

Cada bus conserva ahora un `BusMixState` portable con gain de −100 a +12 dB,
balance estéreo de −1 a +1, mute y solo. Sus coeficientes DSP se preparan fuera
de RT. Gain y balance usan las mismas rampas de 5 ms que las pistas; mute y solo
son discretos. El flujo es acumulación → gain/balance → mute/solo → bus meter →
Master, de modo que un bus muteado o excluido por Solo marca cero.

Solo se resuelve separando selección y camino audible. Sin solos quedan abiertas
todas las pistas y buses. Un Track Solo abre esa pista y el bus que necesita;
un Bus Solo selecciona todo el contenido upstream que alcanza ese bus. En ambos
casos se abre después el camino downstream necesario hasta Master sin seleccionar
ramas hermanas. Varios solos forman la unión de las selecciones. Mute prevalece
localmente. La aplicación publica las máscaras densas junto al parámetro, sin
búsquedas de IDs ni interpretación del grafo en el callback.

Con sends, la resolución prepara permisos independientes para el main output de
cada pista y para cada send. Track Solo conserva su rama dry y sus auxiliares;
Bus Solo abre únicamente las rutas que lo alimentan, sin abrir sends laterales;
Aux Solo produce una escucha wet-only. Las selecciones múltiples forman una
unión. Después de converger en un bus se procesa una mezcla común: no se intenta
preservar la procedencia individual de las contribuciones.

Con Bus Sends, «contenido completo necesario» y «salida Main audible» son
permisos diferentes. Un bus en Solo abre su main y sus sends propios; un bus
abierto solo como transporte downstream no hereda sus sends laterales. Un Aux
en Solo abre todas sus aristas de entrada —Track o Bus Sends—, prepara el
contenido upstream imprescindible y conserva cerrados los dry paths paralelos.

La carga tiene dos fases. `prepareWav` realiza y captura fuera de RT cualquier
operación que puede fallar; `ProjectState` prepara también el nuevo clip y el
mensaje. El commit detiene el callback, intercambia el recurso, ejecuta el
commit portable del modelo —solo swaps y asignaciones `noexcept`— y después
reconecta el callback. El estado final contiene el recurso nuevo en ambos lados
o conserva el anterior en ambos.

La topología RT es un `span` inmutable sobre vistas construidas fuera del
callback. Cargar o sustituir un WAV prepara primero una copia completa del
proyecto RT; el commit intercambia su owner sin reservar memoria mientras el
render está activo. La duración global es el máximo final de todos los clips.

La preparación admite WAV mono o estéreo y rechaza sample rates o muestras no
finitos, tamaños con overflow y preparaciones que superarían un presupuesto
provisional total de 512 MiB. Las duraciones representan límites exclusivos y
se redondean hacia arriba; incluso un recurso de un frame conserva duración.

La arquitectura y las reglas de tiempo real se describen en
[`docs/architecture.md`](docs/architecture.md).
## Historial de incrementos

- **0.0.2:** aplicación JUCE y dispositivo básico.
- **0.0.3:** dispositivo observable y robusto.
- **0.0.4 — First Sound:** primera reproducción WAV.
- **0.0.5:** tiempo de proyecto y transporte sincronizado con el hilo RT.
- **0.0.6 — Two-Track Playback:** dos pistas desde un único reloj maestro.
- **0.0.7 — Realtime Foundation Hardening:** snapshot coherente, lifecycle
  determinista, duración exclusiva y `processBlock` portable.
- **0.0.8 — Lifecycle and Transaction Hardening:** disponibilidad real del
  consumidor, cancelación concurrente y carga WAV transaccional.
- **0.0.9 — Concurrency Closure:** admisión linealizable de comandos y lifetime
  de recursos verificado frente a regiones RT activas.
- **0.1.0 — N-Track Audio Engine:** colección variable con identidad estable,
  proyecto preparado RT, render por pista y acumulación general.
- **0.1.1 — Mixer Core:** gain, pan equal-power, mute/solo global y gain master
  con actualizaciones ligeras de parámetros hacia RT.
- **0.1.2 — Smooth Mixer & Metering:** rampas sample-accurate de gain/pan y
  peak metering portable por pista y master.
- **0.2.0 — Routing Foundation:** `RoutingState`, buses estéreo, destinos
  Track→Bus/Master y ejecución mediante un plan RT preparado.
- **0.2.1 — Bus Mixer Controls:** gain, balance, mute y solo de bus con
  smoothing y resolución portable de caminos audibles.
- **0.2.2 — Bus-to-Bus Routing DAG:** salida principal Bus→Bus/Master,
  validación de ciclos, orden topológico y Solo resuelto a través del grafo.
- **0.2.3 — Track Sends & Auxes:** sends múltiples Track→Bus con taps
  pre/post, level, mute, smoothing y audibilidad preparada por aristas.
- **0.2.4 — Bus Sends:** sends Bus→Bus audibles, taps pre/post de bus,
  integración completa en el DAG y Solo wet-only con permisos por arista.
- **0.3.0 — Processor & Insert Core:** contrato DSP portable, cadenas de
  inserts Track/Bus/Master, GainProcessor, parámetros RT, bypass con latencia y
  metadata de latencia por rutas sin PDC.
- **0.4.0 — Source / Clip / Track Foundation:** catálogo `AudioSource`, IDs
  fuertes, múltiples clips por pista, PCM compartido por fuente, índice temporal
  preparado, render random-access y suma determinista de solapes.
- **0.4.1 — Timeline Editing Operations:** Move, Duplicate, Split, Trim
  Left/Right y Delete no destructivos por `ClipId`, con sharing de fuente,
  rebuild transaccional y preparación para Undo.
- **0.4.2 — Undo / Redo Foundation:** historial acotado de operaciones de
  clips, restauración exacta de IDs, preparación del historial antes del commit,
  barreras, tokens de estado y Undo/Redo estructural solo en Stopped.
- **0.4.3 — Project Persistence:** documento JSON versionado y determinista,
  sesión/dirty, medios verificados, guardado atómico y Load transaccional aislado.
- **0.5.0 — Timeline UI Foundation:** read model portable, tracks/clips dibujados,
  selección por `ClipId`, gestos Move/Trim, acciones Split/Duplicate/Delete,
  playhead, zoom/scroll y reconstrucción tras Undo/Redo/Load.
- **0.5.1 — Seek & Transport Navigation:** estado Paused, Seek RT-safe,
  doble Stop semántico, navegación por ruler/teclado y Split manual real.
- **0.5.2 — Musical Time Foundation:** mapas portables de tempo/métrica,
  conversiones precisas y redondeos explícitos, comandos con Undo/Redo sin
  mover audio, ruler musical, schema v2 y migración real v1→v2.
- **0.5.3 — Loop & Metronome:** loop musical persistente, wrap fraccional por
  segmentos continuos, metrónomo sample-accurate y schema v3 con migración
  v2→v3.
- **0.5.4 — Track Operations & Cross-Track Editing:** Add/Delete Track con
  Undo/Redo exacto, selección explícita para importación y Move 2D de clips con
  ownership por TrackId y compatibilidad de layout.
- **0.5.5 — Waveform Foundation:** caché multirresolución por SourceId, picos
  min/max mono/estéreo, mapping project/source, render visible y reconstrucción
  transaccional al cargar proyectos.
- **0.5.6 — Editing & Transport Hardening:** matriz de transporte y edición,
  loops y límites de bloque, historial/persistencia bajo secuencias largas,
  escala combinada y validación coherente del round-trip 44,1↔48 kHz.
- **0.6.0 — Transport & Timeline Foundation:** posición entera
  autoritativa, dominio navegable separado del contenido, reducción sobre el
  orden aceptado, discontinuidad Seek explícita sin reset universal y preparación
  temporal/musical exacta con rollback íntegro del checkpoint.
- **0.6.1 — Musical Time:** mapa musical preparado bidireccional, inverse y
  rounding racionales, consultas exactas de tempo/métrica, grid anclado a ticks
  y persistencia bit-exact de BPM sin crear otra autoridad temporal.
- **0.6.2 — Loop Foundation:** contrato half-open exacto, admisión reconciliada,
  obligación musical pendiente preservada en lifecycle y read model cohesionado.
- **0.6.3 — Metronome:** contrato N/D y accent, Pause sin colas PCM,
  read model coherente, rollback integral y matriz exacta de scheduling.
- **0.6.4 — Arrange Editing I:** consolidación de Add/Delete Track y Move Clip
  sobre el dominio temporal 0.6.x, validación anticipada de inicio/final
  certificados y preservación transaccional de modelo, plan e historial.
- **0.6.5 — Arrange Editing II:** reorder de pistas por identidad estable,
  multiselección efímera y Move/Delete/Duplicate múltiples, atómicos y
  undoables con una sola preparación y commit por gesto.
- **0.6.6 — Arrange Foundation Completion:** Time Selection efímera, Snap
  portable de beat/frames/edges, copia explícita hacia Loop y primera medición
  no contractual del coste de rebuild.
- **0.7.0 — Audio Recording Foundation:** arm exclusivo y efímero, captura
  mono/estéreo por inputs 1–2 mediante ring SPSC preasignado, WAV temporal en
  `<ProjectName> Audio/`, publicación transaccional y Undo/Redo con identidad
  exacta y verificación del medio.
- **0.7.1 — Recording Recovery Hardening:** marker auxiliar best-effort con
  diagnósticos conservados, persistencia crítica de media sin rebajar, cleanup y
  shutdown seguros también sin marker, corrección de `prepared` y cobertura
  JUCE hardware-free del preflight real.
- **0.7.2 — Input Monitoring Foundation:** monitoring manual RT con gain
  suavizado, preflight/lifecycle de dispositivo, staging alias-safe, meter de
  input y UI mínima; coexistencia segura de Recording + Monitoring, incluido
  Monitoring activo tras Stop Recording, validada también en smoke físico.
- **0.7.3 — Device Latency, Recording Placement & Physical Loopback Validation:**
  buffer y latencias reportadas con reconfiguración transaccional, placement por
  toma sin tocar WAV, y medición RTT física diagnóstica mediante MLS; validación
  Volt 176 en cinco buffers y fixes de Record-toggle/capacidad de input.

Las validaciones están registradas en [`docs/validation-0.0.2.md`](docs/validation-0.0.2.md),
[`docs/validation-0.0.3.md`](docs/validation-0.0.3.md) y
[`docs/validation-0.0.4.md`](docs/validation-0.0.4.md) y
[`docs/validation-0.0.6.md`](docs/validation-0.0.6.md) y
[`docs/validation-0.0.7.md`](docs/validation-0.0.7.md) y
[`docs/validation-0.0.8.md`](docs/validation-0.0.8.md) y
[`docs/validation-0.0.9.md`](docs/validation-0.0.9.md) y
[`docs/validation-0.1.0.md`](docs/validation-0.1.0.md) y
[`docs/validation-0.1.1.md`](docs/validation-0.1.1.md) y
[`docs/validation-0.1.2.md`](docs/validation-0.1.2.md) y
[`docs/validation-0.2.0.md`](docs/validation-0.2.0.md) y
[`docs/validation-0.2.1.md`](docs/validation-0.2.1.md) y
[`docs/validation-0.2.2.md`](docs/validation-0.2.2.md) y
[`docs/validation-0.2.3.md`](docs/validation-0.2.3.md) y
[`docs/validation-0.2.4.md`](docs/validation-0.2.4.md) y
[`docs/validation-0.3.0.md`](docs/validation-0.3.0.md) y
[`docs/validation-0.4.0.md`](docs/validation-0.4.0.md) y
[`docs/validation-0.4.1.md`](docs/validation-0.4.1.md) y
[`docs/validation-0.4.2.md`](docs/validation-0.4.2.md) y
[`docs/validation-0.4.3.md`](docs/validation-0.4.3.md) y
[`docs/validation-0.5.0.md`](docs/validation-0.5.0.md) y
[`docs/validation-0.5.1.md`](docs/validation-0.5.1.md) y
[`docs/validation-0.5.2.md`](docs/validation-0.5.2.md) y
[`docs/validation-0.5.3.md`](docs/validation-0.5.3.md) y
[`docs/validation-0.5.4.md`](docs/validation-0.5.4.md) y
[`docs/validation-0.5.5.md`](docs/validation-0.5.5.md) y
[`docs/validation-0.5.6.md`](docs/validation-0.5.6.md) y
[`docs/validation-0.6.0.md`](docs/validation-0.6.0.md) y
[`docs/validation-0.6.1.md`](docs/validation-0.6.1.md) y
[`docs/validation-0.6.2.md`](docs/validation-0.6.2.md) y
[`docs/validation-0.6.3.md`](docs/validation-0.6.3.md) y
[`docs/validation-0.6.4.md`](docs/validation-0.6.4.md) y
[`docs/validation-0.6.5.md`](docs/validation-0.6.5.md) y
[`docs/validation-0.6.6.md`](docs/validation-0.6.6.md) y
[`docs/validation-0.7.0.md`](docs/validation-0.7.0.md) y
[`docs/validation-0.7.1.md`](docs/validation-0.7.1.md) y
[`docs/validation-0.7.2.md`](docs/validation-0.7.2.md) y
[`docs/validation-0.7.3.md`](docs/validation-0.7.3.md).
