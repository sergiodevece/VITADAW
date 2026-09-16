# Arquitectura inicial

## Flujo de control

```text
UI / teclado / MIDI futuro / voz futura
                  |
                  v
          ICommandDispatcher
                  |
                  v
            DawApplication
           /       |       \
  ProjectState  Transport  IAudioEngineControl
                                |
                    RealtimeAudioEngine portable
                                |
                                v
                    adaptador/callback JUCE
```

`DawApplication` es la fachada y el único manejador de comandos. La UI recibe
un `ICommandDispatcher`, nunca una referencia al motor de audio. Añadir otra
fuente de entrada significa traducirla al mismo `Command`, no crear otro camino
al motor.

## Módulos

- `application`: composición y coordinación de un comando completo.
- `project`: estado editable y propiedad de pistas.
- `commands`: mensajes de intención, resultados y despacho.
- `audio`: contratos de control no-RT y procesamiento RT.
- `mixer`: estado portable, unidades y preparación DSP de gain/pan.
- `processors`: configuración editable, contrato DSP portable, factory e
  implementación interna de procesadores.
- `transport`: estado lógico de reproducción y posición.
- `media`: catálogo portable de fuentes y referencias de medio; nunca posee PCM.
- `tracks`: pistas mono/estéreo de layout estable con clips ordenados.
- `clips`: colocaciones no destructivas por `SourceId` en el timeline.
- `timeline`: unidades temporales fuertes y conversiones entre archivo,
  proyecto, dispositivo y segundos.
- `ui`: frontera de UI; se implementará con JUCE sin acceder a `audio`.
- `platform/juce`: composición nativa, ventana y adaptador de dispositivo; es
  la única capa que depende de JUCE.

El modelo editable pertenece al hilo de aplicación/UI. El callback de audio no
lee esas estructuras mutables. Cada WAV se decodifica completamente fuera de RT;
el adaptador detiene el callback antes de publicar una topología preparada.

`RealtimeAudioEngine` es la frontera real de procesamiento portable. Posee el
reloj maestro, las colas SPSC, el render N-track, el mixer y la
intercambio de transporte. Su `processBlock` recibe vistas de salida y el sample
rate del dispositivo, y es exactamente el método invocado por JUCE y por los
tests offline. El adaptador JUCE conserva dispositivo, filesystem, decodificación
y ownership de `AudioBuffer`; no decide cómo avanza o mezcla el proyecto.

`audio/AudioDeviceState` es un modelo portable de observación del dispositivo.
No forma parte del callback: contiene strings y se actualiza únicamente en el
hilo de aplicación. El adaptador convierte desde tipos JUCE y la composición
entrega copias de ese estado a la ventana; la ventana no recibe acceso al motor.

`DeviceProcessingState` es la autoridad portable para la capacidad de consumir
comandos RT. Sus estados son `unavailable`, `initializing`, `operational`,
`stopped` y `error`. El modelo de observación anterior conserva strings para la
UI; no decide si Play es aceptable.

## Reglas del hilo de audio

Dentro de `RealtimeAudioEngine::processBlock()`:

- no reservar ni liberar memoria;
- no usar mutex, esperas, logging, filesystem ni llamadas de UI;
- no abrir/decodificar archivos ni cambiar el dispositivo;
- usar buffers y recursos preparados de antemano;
- ejecutar trabajo con coste acotado y métodos `noexcept`;
- comunicar estado hacia la UI mediante atomics o colas SPSC preasignadas.

La carga y decodificación WAV, la creación de recursos y cualquier I/O se hacen
en hilos no-RT. La vida de los recursos publicados cubre todo callback que pueda
seguir observándolos; no se destruyen desde el hilo RT.

Estas garantías corresponden al render propio de VitaDAW. No se afirma que toda
la ruta hasta el hardware sea lock-free: JUCE 9.0.2 `AudioDeviceManager` protege
su lista de callbacks con un mutex y ajusta un buffer temporal en la ruta de
callback. VitaDAW no añade sus propios bloqueos o reservas dentro de
`processBlock`.

## Reproducción mínima 0.0.4 — First Sound (histórico)

`LoadAudioFile` recorre `ICommandDispatcher` y `DawApplication` antes de llamar
al puerto neutral `IAudioEngineControl::prepareWav`. El adaptador valida la
extensión y usa directamente `juce::WavAudioFormat`; no registra MP3, AIFF,
FLAC ni otros formatos. Decodifica el archivo completo a un `AudioBuffer<float>`
fuera del callback. En 0.4.0 ese recurso se publica junto al plan completo por
`SourceId`; la antigua operación separada de commit quedó eliminada.

Play y Stop viajan por una cola SPSC fija de ocho posiciones. El callback vacía
primero la salida, consume esos comandos y lee únicamente el buffer preparado y
un cursor RT. Stop deja el cursor en el frame cero. No se consulta el
`ProjectState` desde audio.

Cuando los sample rates difieren, el callback avanza por el archivo con el ratio
`fileSampleRate / deviceSampleRate` e interpola linealmente entre dos frames. No
se usa el resampler de JUCE en este incremento. La interpolación lineal mantiene
duración y tono correctos, aunque no es la solución de calidad final para un DAW.

En 0.0.4 la pista única conservaba la longitud en frames del archivo fuente y su
sample rate. No existe todavía un timeline gráfico.

## Tiempo de proyecto y sincronización del transporte

`ProjectState` posee un `SampleRate` explícito. En este incremento se fija una
sola vez durante la composición de la aplicación: adopta el sample rate del
dispositivo activo o `48000 Hz` si el dispositivo no pudo inicializarse. Una
reinicialización posterior del dispositivo no cambia esa escala. Esta política
está encapsulada en la composición JUCE y permite introducir más adelante una
selección o persistencia propia del proyecto sin cambiar el dominio.

El módulo `timeline` evita valores numéricos sin unidad mediante:

- `SourceFrameCount`, `SourceFramePosition` y `SourceFrameDuration` para el WAV;
- `ProjectFrameCount` y `ProjectFramePosition` para el timeline lógico;
- `DeviceFrameCount` para bloques procesados por el dispositivo;
- `Seconds` y `SampleRate` para conversiones explícitas.

La duración de un clip se convierte una sola vez de frames fuente a frames de
proyecto. Desde 0.6.0 la posición autoritativa del transporte es
siempre un `ProjectFramePosition` entero. Un residuo fraccionario acotado existe
como estado del reloj RT para convertir device frames a project frames; el render
recibe una copia efímera para conversiones locales. No se publica a la UI ni
funciona como segundo locator. Las posiciones derivadas de audio
se redondean al frame más próximo, mientras
que un límite exclusivo de duración se redondea hacia arriba. Así la escala de
proyecto cubre todo recurso no vacío, incluso si uno, dos o tres frames fuente
equivalen a menos de un frame de proyecto. `TransportState` nunca contiene
frames del archivo ni tipos JUCE.

El motor publica `playing`, posición, duración y la última secuencia resuelta en
`RealtimeTransportExchange`. Todos sus campos y su revisión son atomics con
`is_always_lock_free` y orden `seq_cst`. El orden total de C++20 garantiza que si
el lector observa un campo de una generación nueva, la revisión impar que lo
precede también queda antes de su comprobación final; solo acepta el snapshot si
ambas revisiones son iguales y pares. La lectura realiza como máximo tres
intentos y, si todos coinciden con una publicación, devuelve el último snapshot
coherente del único consumidor. No espera indefinidamente.

La cola conserva capacidad fija de ocho posiciones. `CommandLifecycleGate`
empaqueta estado y generación en un único `atomic<uint64_t>` siempre lock-free;
el estado interno transitorio `claimed` pertenece al único productor SPSC. El
protocolo de enqueue es:

1. el productor observa `operational` y cambia mediante CAS el token completo a
   `claimed`, conservando la generación;
2. comprueba espacio antes de reservar una secuencia; si la cola está llena,
   devuelve el gate y rechaza sin crear huecos de secuencia;
3. reserva una secuencia y escribe el comando en el slot, todavía invisible para
   RT porque el índice de escritura no ha cambiado;
4. intenta el CAS `claimed(g) -> operational(g)`. El éxito de este CAS es el
   punto de linearización de la aceptación;
5. solo después del éxito publica el índice con release. Si el CAS falla, el
   comando se rechaza y el slot nunca se publica.

El cierre de lifecycle ejecuta un CAS sobre el mismo gate hacia un estado no
operativo y la generación siguiente. Ese CAS es su punto de linearización. A
continuación lee el siguiente número de secuencia, resuelve hasta `next - 1`,
rebobina y publica el snapshot final. Las relaciones son:

- si el CAS de aceptación precede al cierre en el orden de modificación del
  gate, el acquire del cierre observa el release de aceptación. La reserva de
  secuencia, anterior al release, sucede antes de la lectura del watermark y
  queda incluida en la cancelación;
- si el cierre sustituye primero `operational` o `claimed`, el CAS final del
  productor no puede coincidir con su token completo y el comando se rechaza;
- si el productor comienza después del cierre, no puede reclamar el estado no
  operativo. Solo una confirmación de consumidor abre la nueva generación.

Por ello ninguna ejecución C++20 puede aceptar una secuencia fuera tanto de la
cola consumible como del watermark de una generación cerrada. Que el índice se
publique después de aceptar no abre una ventana: un cierre intermedio ya ha
resuelto esa secuencia, y el comando publicado conserva la generación antigua,
por lo que un consumidor posterior lo descarta. El contador no envuelve: al
agotar el espacio de 64 bits se rechazan nuevas reservas. La generación tampoco
envuelve; tras agotar sus 61 bits el gate queda en error terminal. El wrap-around
normal del índice circular sigue siendo seguro y está probado.

`audioDeviceAboutToStart` no demuestra que exista consumo: JUCE también lo
invoca al añadir un callback a un objeto retenido. Por eso solo establece
`initializing`. El paso a `operational` exige callback registrado y
`AudioIODevice::isPlaying()`, o la entrada efectiva en `processBlock`. Si el
objeto persiste pero deja de procesar, `audioDeviceStopped` mantiene el estado
`stopped`; un re-registro permanece `initializing` hasta obtener una de esas
confirmaciones. Play solo se acepta en `operational`.

JUCE serializa retirada, `audioDeviceStopped`, `audioDeviceError` y callback de
audio mediante su bloqueo interno de callbacks. VitaDAW aprovecha esa frontera
para que las mutaciones del reloj y la publicación tengan un solo escritor cada
vez, sin añadir ningún lock al render. Los callbacks de lifecycle escriben solo
atomics y estado portable; el diagnóstico con strings se materializa después en
el hilo de aplicación mediante polling.

Al alcanzar el último frame global, el reloj RT deja de producir, publica
`playing = false` y conserva la posición lógica exactamente en la duración. El
hilo de aplicación converge así a `Stopped` en el final. Esta política distingue
el final natural de `Stop`, que conserva su semántica de `Stopped` en cero. Un
Play posterior al final reinicia el reloj en cero.

## Motor N-track 0.1.0 (histórico)

Esta sección documenta el hito original. Su `optional<AudioClip>` y ownership
por `TrackId` quedaron sustituidos por la arquitectura Source/Clip/Track de
0.4.0 descrita más adelante.

`ProjectState` conserva un `vector<AudioTrack>` editable solo desde aplicación.
Cada pista recibe un `TrackId` monotónico de 64 bits que no depende de su índice;
`LoadAudioFile` dirige la carga mediante esa identidad. Una pista mantiene como
máximo un `optional<AudioClip>`. No se admiten todavía múltiples clips ni
operaciones de edición.

El adaptador posee un `PreparedProject` inmutable durante el render. Contiene
ownership compartido de los buffers decodificados y un vector estable de
`PreparedTrackView`. Cada vista incluye identidad, canales, frames fuente,
sample rate fuente, inicio y duración del clip y offset fuente. El motor recibe
solo un `PreparedProjectView`: sample rate de proyecto, duración global y un
`span<const PreparedTrackView>`. No recorre `ProjectState` ni posee la colección.

La topología candidata se construye completamente fuera de RT. Para una carga,
se copian los owners de los recursos vigentes, se sustituye o añade el recurso
dirigido por `TrackId`, y se reconstruyen vistas y duración. Con el callback
quiescente, el commit intercambia el owner del proyecto preparado, configura el
span y ejecuta el commit `noexcept` de `ProjectState`. No hay `push_back`, resize,
destrucción de recursos ni otra mutación estructural dentro del callback.

`RealtimeProjectClock` es el único estado temporal que avanza en el motor.
En cada frame de dispositivo produce una posición precisa en frames de proyecto:

```text
device frame
    -> RealtimeProjectClock (project frames)
        -> for each prepared track
            source position = source offset
                            + (project position - clip start)
                            × source rate / project rate
```

No existe un cursor por pista. Todas las posiciones se recalculan desde la misma
posición global, por lo que el número de pistas no introduce deriva relativa. El
reloj de 0.6.0 conserva un resto racional exacto preparado si dispositivo
y proyecto difieren de sample rate. La duración global es el máximo `clip start + clip duration` de todas las
pistas preparadas.

`TrackRenderer` convierte la posición común y produce una contribución estéreo
por pista mediante interpolación lineal. Una pista fuera de su rango devuelve
silencio. Desde 0.1.1, `TrackMixerProcessing` aplica gain, pan y la decisión
mute/solo; `StereoAccumulator` suma a unity y el gain master procesa el resultado
estéreo. No hay atenuación fija, limiter ni clipping no lineal.

## Mixer Core 0.1.1

`AudioTrack` contiene un `TrackMixState` portable con gain en dB, pan normalizado,
mute y solo. `ProjectState` posee además `MasterMixState`. Los gains válidos van
de `-100 dB` —silencio práctico representado como coeficiente cero— a `+12 dB`;
`0 dB` equivale a unity. Pan admite `-1` izquierda, `0` centro y `+1` derecha.
Todos los valores numéricos no finitos o fuera de rango se rechazan antes de
modificar el modelo.

El flujo de señal por muestra es:

```text
master project clock
    -> TrackRenderer
    -> track linear gain
    -> mono equal-power pan / stereo equal-power balance
    -> mute and global solo eligibility
    -> track peak meter
    -> StereoAccumulator
    -> master linear gain
    -> master peak meter
    -> stereo output
```

Para mono, el ángulo recorre `[0, pi/2]`: izquierda=`cos(angle)` y
derecha=`sin(angle)`. El centro entrega `sqrt(1/2)` a cada canal (-3 dB) y
conserva potencia. Para estéreo se usa balance: al centro ambos canales quedan a
unity; hacia un extremo el canal opuesto sigue una curva seno/coseno hasta cero,
sin alterar el canal del lado elegido. No se implementan width ni pan dual.

La elegibilidad es global por bloque. Sin pistas en solo suena toda pista no
muteada. Con uno o más solos solo suenan pistas cuyo solo está activo. Mute tiene
precedencia incluso sobre solo. Estas decisiones no detienen render, reloj ni
transporte: una pista inaudible sigue avanzando.

Los comandos `SetTrackGain`, `SetTrackPan`, `SetTrackMute`, `SetTrackSolo` y
`SetMasterGain` siguen `ICommandDispatcher -> DawApplication -> ProjectState /
IAudioEngineControl`. `DawApplication` valida y prepara un estado DSP completo;
la conversión dB-lineal y los coeficientes trigonométricos se calculan fuera de
RT. El adaptador no decodifica ni reconstruye recursos para estos cambios.

`RealtimeAudioEngine` dispone de un ring SPSC de parámetros con 64 entradas,
63 pendientes utilizables por reservar una como discriminador lleno/vacío, y un
array fijo para un máximo preparado de 256 pistas. El productor único escribe
un estado completo y publica el índice con release; el callback adquiere el
índice, consume FIFO al inicio de bloque y actualiza solo su almacenamiento
preasignado. Una cola llena rechaza explícitamente el comando; entonces ni
`PreparedProject` ni `ProjectState` cambian. Cada descriptor preparado conserva
también el estado vigente para que una posterior reconstrucción estructural lo
publique sin volver a valores por defecto. La carga de un WAV para una pista
vacía recibe su estado de mezcla actual desde el modelo. Desde 0.2.1, cada
cambio de pista o bus publica además un `PreparedAudibilityState` que representa
la resolución completa de Solo contra el routing, incluidas pistas vacías.

La suma interna usa `float` y puede superar `[-1, 1]`. No se aplica clamp ni
limitador; el recorte depende del backend/hardware final.

## Smooth Mixer y metering 0.1.2

`TrackMixSmoother` contiene cinco rampas lineales preasignadas: gain lineal y
los cuatro coeficientes de pan preparados para mono y estéreo. `MasterMixSmoother`
contiene la rampa de gain master. Cada nuevo target calcula `round(0.005 ×
device sample rate)` pasos y cada frame procesado avanza exactamente uno. Con
ello la transición dura 5 ms tanto con bloques de 64 como de 1024 frames y se
adapta a 44,1, 48 o 96 kHz. Si el transporte está parado pero el callback sigue
operativo, las rampas continúan avanzando sobre el tiempo de dispositivo aunque
la salida permanezca en silencio.

Gain se suaviza en amplitud lineal, no en dB. Esto evita conversiones por muestra,
alcanza cero de forma exacta y genera una trayectoria lineal de amplitud, no una
velocidad perceptual constante. Pan suaviza los coeficientes seno/coseno
precalculados fuera de RT: no hay trigonometría por muestra. Los extremos y el
target estable cumplen exactamente la ley equal-power; durante la rampa la
interpolación continua de coeficientes puede apartarse ligeramente de potencia
constante. Un target nuevo conserva el valor instantáneo de cada rampa como
origen y recalcula solo su incremento y pasos restantes, sin volver a un valor
anterior. Mute y solo se aplican discretamente al límite de bloque.

El punto de medición por pista es la contribución que sale de
`TrackMixerProcessing`, después de gain, pan y elegibilidad mute/solo, justo
antes de `StereoAccumulator`. El master se mide después de la suma y del gain
master, justo antes de copiar a output. La semántica 0.1.2 es peak absoluto
instantáneo por bloque, separado por canal y sin clamp. Una pista no audible por
mute/solo publica cero. No se calculan RMS, decay ni peak hold.

`RealtimeMeterExchange` asocia cada medición con `TrackId` y usa únicamente
`atomic<uint32_t>`, `atomic<uint64_t>` y `atomic<size_t>` garantizados lock-free.
Los floats se publican como sus bits de 32 bits. Un único escritor RT rodea cada
snapshot con una revisión impar/par y todas las operaciones usan el orden total
`seq_cst`; el lector acepta solo revisiones iguales y pares. Hace un máximo de
tres intentos y, si coincide continuamente con el escritor, devuelve un snapshot
vacío coherente. Metering es telemetría latest-value: perder una lectura o un
bloque no modifica audio ni comandos. La UI consulta mediante `DawApplication`,
nunca directamente al motor, y actualiza labels numéricos a 30 Hz.

El callback limpia acumuladores preasignados, consume parámetros, avanza
rampas, calcula máximos absolutos y publica atomics. No reserva, bloquea, crea ni
destruye recursos. Buses, sends, inserts, plugins, automatización, RMS, decay,
grabación y routing configurable quedan fuera de 0.1.2.

Una carga válida se construye y decodifica por completo antes de desconectar
brevemente el callback. `DawApplication` prepara antes el `AudioClip`, su
`filesystem::path` y el mensaje de resultado; cualquier excepción conserva
intacto el estado anterior. Con el render ya quiescente, el adaptador intercambia
el proyecto preparado, configura las vistas RT y ejecuta la acción de modelo
`noexcept`. Ese es el único punto de commit. Solo entonces vuelve a registrar el
callback. El proyecto anterior queda temporalmente en el objeto preparado y sus
recursos sustituidos se destruyen en aplicación cuando RT ya no los referencia.

La garantía de lifetime depende de la misma serialización que usa producción:
`AudioDeviceManager::removeAudioCallback` espera el bloqueo interno que protege
la invocación del callback. Cuando retorna, ningún callback puede conservar una
vista del recurso sustituido. El adaptador configura las vistas nuevas y el
último owner antiguo se libera fuera de RT. El test de lifetime reproduce esta
frontera con una región RT controlada: sustitución y cierre quedan bloqueados
mientras la vista está activa, y el destructor instrumentado falla si coincide
con cualquier usuario RT.

Rutas, apertura, creación del reader, decodificación, buffers y metadatos están
dentro de la frontera de excepciones de preparación. `std::bad_alloc`,
`std::exception` y excepciones desconocidas se convierten en resultados de error
fuera de RT; `DawApplication` contiene además la misma frontera defensiva. Una
carga fallida no publica nada: todos los clips y recursos válidos permanecen
intactos.
Solo se aceptan uno o dos canales. Se validan sample rates finitos y positivos,
longitud, productos de tamaño, identidades, muestras float finitas y un presupuesto
provisional de preparación de 512 MiB. El presupuesto incluye buffers ya
publicados y el candidato, limitando también el pico durante una sustitución.

### Límites conscientes de este incremento

- La decodificación es síncrona en el hilo de aplicación. No compromete el hilo
  RT, pero un archivo grande puede congelar temporalmente la ventana.
- El archivo completo se conserva en memoria como `float`; una sustitución
  próxima al presupuesto puede rechazarse aunque el tamaño final cupiera tras
  liberar el recurso anterior.
- La interpolación lineal es suficiente para validar sample rates distintos,
  pero no tiene la calidad de un resampler final de producción.
- La cola es SPSC porque la UI es el único productor actual. Futuras fuentes de
  comandos necesitarán serialización previa, no productores concurrentes sobre
  esta cola.
- La posición pública se redondea al frame de proyecto más cercano; internamente
  el reloj conserva precisión fraccional.
- WAV float con muestras fuera de `[-1, 1]` puede superar el margen previsto. No
  se introduce todavía limitador ni clipping no lineal.
- El render actual es escalar y recorre todas las pistas por frame. Valida la
  arquitectura hasta decenas de pistas, pero no constituye una garantía de
  rendimiento profesional.

## Routing Foundation 0.2.0

`ProjectState` posee un `RoutingState` portable. Este conserva buses con `BusId`
monotónico de 64 bits y una única ruta por `TrackId`; una ruta termina en Master
o en un bus existente. La relación solo vive en `RoutingState`. Un bus no es una
pista vacía: no contiene WAV, clip, reloj ni sample rate fuente.

La frontera estructural es:

```text
ProjectState + RoutingState
    -> ProcessingPlanSpecification
    -> prepareProcessingPlan (portable, application thread)
    -> PreparedProcessingPlan + ProcessingPlanRuntime
    -> quiescent transactional commit
    -> RealtimeAudioEngine
```

El plan inmutable conserva tracks y buses resueltos a índices densos, duración,
orden de pasos, capacidad del subbloque y presupuesto de memoria. El runtime
separado posee buffers estéreo por bus, buffer master y una tabla temporal. Los
recursos WAV, el plan y el runtime comparten el owner publicado por el adaptador;
el motor solo recibe vistas prestadas mientras el callback puede ejecutarse.

El orden preparado de 0.2.0 contiene todos los pasos de pista, después todos los
buses y finalmente Master. Es el punto de extensión para una futura ordenación
topológica. Como Bus→Bus no existe todavía, un ciclo no puede representarse:
Master es terminal y cada bus tiene salida fija a Master. Destinos inexistentes,
IDs inválidos o duplicados, fuentes desconocidas, formatos inválidos, límites y
presupuesto insuficiente se rechazan antes del commit.

El ejecutor divide cualquier callback en subbloques de hasta 512 frames. Para
cada subbloque escribe una tabla preasignada de posiciones obtenidas del único
`RealtimeProjectClock`. Después cada pista se renderiza una vez, avanza sus
smoothers una vez por frame y acumula en su bus o en Master. Los buses se miden y
se acumulan a Master; por último se aplica el gain master y se escribe output.
Ningún nodo posee o avanza un reloj global independiente.

```text
TrackRenderer -> gain/pan smoothing -> mute/solo -> track meter
    -> main destination
        -> optional stereo bus accumulation -> identity -> bus meter
        -> master accumulation -> master smoothing -> master meter -> output
```

Los buffers se reservan al preparar el plan. El presupuesto inicial es 16 MiB y
los límites son 256 pistas, 64 buses y estéreo fijo. El callback solo limpia y
reutiliza memoria existente. Los peaks de todos los subbloques se combinan para
publicar un único snapshot del callback, identificado por `TrackId` y `BusId`.

`AddAudioTrack`, `AddBus` y `SetTrackOutputDestination` son cambios
estructurales. `DawApplication` copia el modelo, aplica el cambio al candidato y
solicita preparación antes de mutar el proyecto vigente. Solo se admiten con el
transporte parado. El adaptador conserva los owners de WAV existentes sin
decodificarlos otra vez, retira el callback, intercambia el conjunto preparado,
ejecuta el swap `noexcept` del modelo y reconecta. Preparación o commit fallidos
no publican ninguna parte del candidato.

La carga WAV usa el mismo compilador, pero no define la topología: el routing
puede prepararse con cero recursos y mantener buses vacíos. JUCE continúa
limitado a dispositivo, decodificación, ownership y barrera del callback; el
modelo, compilador, plan y ejecutor son core-only.

### Límites conscientes de 0.2.0

- Cada pista tiene un solo destino principal.
- Todos los buses son estéreo, procesan a unity y terminan en Master.
- No existen Bus→Bus, sends, inserts, plugins, PDC ni feedback.
- Los cambios de routing durante Play se rechazan.
- Solo y Mute pertenecen exclusivamente a pistas; los buses transportan la
  contribución resultante.
- Se usa un buffer completo por bus; no hay reutilización avanzada de scratch.

## Bus Mixer Controls 0.2.1

`AudioBus` conserva su `BusId` estable y añade un `BusMixState` portable:
gain, balance estéreo, mute y solo. La especificación del plan transforma ese
estado en `PreparedBusMixState`; ninguna conversión dB→lineal ni trigonometría
ocurre por muestra.

El punto de procesamiento queda fijado explícitamente:

```text
entradas acumuladas
    -> gain smoothing
    -> balance smoothing
    -> mute/solo
    -> bus peak meter
    -> Master accumulator
```

Este orden deja entre acumulación y metering el punto natural para futuros
inserts y decisiones pre/post, sin implementar todavía ninguna de ellas. El
meter es post-mute y post-Solo: refleja exclusivamente la señal audible que el
bus entrega a Master.

`BusMixSmoother` mantiene una rampa lineal de 5 ms para amplitud y coeficientes
de balance. Se avanza exactamente una vez por frame procesado, aunque el callback
se divida en subbloques. Mute y solo cambian discretamente al inicio de bloque.

### Selección Solo y caminos audibles

La selección del usuario no se aplica como una condición local ingenua. Una
etapa portable resuelve `PreparedAudibilityState`, formado por máscaras densas
de pistas y buses:

- sin solos, todas las rutas permanecen abiertas;
- Track Solo selecciona la pista y abre el bus necesario para llegar a Master;
- Bus Solo selecciona el bus y todas las pistas cuyo destino es ese bus;
- varios solos forman la unión de todas las selecciones válidas;
- una pista directa a Master solo permanece cuando está seleccionada o no hay
  ningún solo;
- mute prevalece en el nodo que lo contiene, incluso si está seleccionado.

La resolución usa `TrackId` y `BusId` únicamente en el hilo de aplicación. El
resultado acompaña atómicamente al comando de parámetros que cambia Solo. El RT
solo consulta bits por índice denso; no busca identidades ni recorre
`ProjectState`.

`SetBusGain`, `SetBusPan`, `SetBusMute` y `SetBusSolo` atraviesan la misma
frontera UI → comandos → aplicación → motor. Son parámetros, no cambios
estructurales: no detienen Play, no reconstruyen el plan, no retiran el callback
y no decodifican WAV. Una cola llena rechaza tanto la publicación como el cambio
del modelo.

## Bus-to-Bus Routing DAG 0.2.2

`RoutingState` sigue siendo la única fuente editable. El tipo portable
`OutputDestination` representa Master o un `BusId`; cada pista y cada `AudioBus`
tiene exactamente una salida principal y Master no tiene destino. Los buses
nuevos se dirigen a Master por defecto. `SetBusOutputDestination` es un comando
estructural y se rechaza mientras el transporte está reproduciendo.

El compilador portable ordena pistas y buses densos por sus identidades públicas,
valida todos los destinos y recorre todos los buses mediante DFS de tres colores.
Una arista hacia un nodo gris rechaza el candidato y genera un diagnóstico con
la cadena de `BusId`, incluidos ciclos formados únicamente por buses vacíos. A
continuación, Kahn produce un orden topológico estable: entre buses disponibles
se escoge el menor `BusId`. Los pasos de pista preceden a los buses y Master
aparece exactamente una vez al final.

`PreparedBusNode` conserva por separado `BusId`, índice denso, `bufferIndex`,
estado DSP y destino denso. El orden topológico contiene índices de nodo, no
identidades ni posiciones editables. El runtime mantiene un acumulador estéreo
preasignado por bus, el acumulador Master y la tabla temporal. Smoothers y meters
RT permanecen en las tablas preasignadas del motor, indexados por la misma
correspondencia densa de `BusId`.

En cada subbloque se limpian todos los acumuladores; las pistas se renderizan una
vez y suman en su destino; después cada bus lee su entrada completa, aplica
gain/balance/mute/Solo, actualiza su meter y suma una vez en su destino. Ningún
nodo resuelve topología ni avanza `RealtimeProjectClock`. Un bus smoother avanza
una vez por frame con independencia del fan-in o de la profundidad.

Solo se prepara fuera de RT en dos fases. `U` contiene el contenido completo
seleccionado upstream por Bus Solo. Las pistas seleccionadas son las que tienen
Solo o alcanzan un bus de `U`. `A` añade los buses downstream que solo son
necesarios para transportar esa selección a Master. No se vuelve a expandir
upstream desde `A`, evitando abrir ramas hermanas. RT recibe exclusivamente las
máscaras densas finales; Mute sigue siendo una decisión local y prevalece sin
buscar rutas alternativas.

La preparación incluye topología, audibilidad y todos los buffers antes del
commit existente. El adaptador retira el callback, publica conjuntamente modelo,
plan, runtime y owners mediante operaciones `noexcept`, y vuelve a registrar el
consumidor. Un fallo de referencia, ciclo, capacidad, memoria o preparación deja
intacto el proyecto anterior. El test de lifetime extiende esta garantía a una
cadena de buses preparada.

### Límites conscientes de 0.2.2

- Una pista o bus tiene una sola salida principal.
- No existen sends, inserts, plugins, PDC, feedback ni routing durante Play.
- El grafo y el audio son estéreo; no hay multicanal, PFL, AFL ni solo-safe.
- Se reserva un buffer completo por bus; no existe reutilización de scratch.
- La suma no incluye limiter ni clamp y puede superar `[-1, 1]`.
- El orden y DSP son escalares; no se introduce procesamiento paralelo.

## Track Sends & Auxes 0.2.3

`RoutingState` conserva `SendRoute` como una rama auxiliar distinta de la salida
principal. `SendId` es una identidad pública monotónica; `SendSource` puede
representar `TrackId` o `BusId`; `SendTapPoint` distingue
`preFaderPrePan` y `postFaderPostPan`; `SendMixState` contiene level y mute.
0.2.3 ejecuta exclusivamente Track Sends y rechaza Bus Sends con un diagnóstico
explícito, aunque el validador ya incluye sus aristas al detectar ciclos.

```text
render original
    |-> adaptación estéreo -> PRE-FADER/PRE-PAN TAP -> send gate/level -> Bus
    `-> track gain/pan/mute -> POST-FADER/POST-PAN TAP
                                  |-> main gate -> destino principal
                                  `-> send gate/level -> Bus
```

Una fuente mono produce en el pre-tap `x/sqrt(2)` por canal. El post-path se
calcula desde el render mono original, evitando aplicar dos veces la atenuación
central. Track Mute y el fader en silencio no cierran el pre-send; sí cierran el
main y los post-sends. Send Mute solo afecta a su rama.

`PreparedSendDescriptor` separa `SendId`, índice denso, pista origen, bus
destino, tap y runtime index. Los descriptors se agrupan en rangos pre/post por
pista y `ProcessingPlanRuntime` posee un `SendMixSmoother` por send. No existe un
buffer de audio por send: cada frame calcula los dos taps una vez y acumula cada
rama directamente en el buffer preparado del bus destino.

La topología estructural es la unión de outputs Bus→Bus y futuras aristas Send
Bus→Bus. DFS y Kahn consideran incluso sends muteados o a -100 dB; una arista
silenciosa no puede esconder feedback. Track Sends no cambian el orden porque
todas las pistas preceden a los buses.

`PreparedAudibilityState` contiene permisos diferentes para main outputs,
sends, buses de transporte y Track Meter. Track Solo abre dry, sus sends y las
rutas downstream. Bus Solo selecciona las rutas que lo alimentan sin abrir
sends laterales; Aux Solo abre sus sends de entrada y queda wet-only. No se
vuelve a expandir upstream desde buses abiertos solo para transporte. Tras una
convergencia, el bus contiene una mezcla común y no conserva procedencia.

Send Level y Send Mute son parámetros ligeros publicados por el ring SPSC.
Level usa el rango -100..+12 dB y smoothing lineal de 5 ms; su smoother avanza
una vez por frame aunque la rama no sea audible. Crear o eliminar un send es
estructural y reutiliza preparación candidata y commit con callback quiescente.

La preparación limita el proyecto a 1024 sends totales y 64 por pista. El
presupuesto incluye descriptors, mapping `SendId`, rangos, smoothers y máscaras
además de los buffers. Los límites vigentes siguen siendo 256 pistas, 64 buses,
estéreo y 16 MiB para el plan/runtime portable.

### Límites conscientes de 0.2.3

- Bus Sends se validan estructuralmente pero no se ejecutan.
- No existen inserts, plugins, PDC, feedback ni automatización.
- No hay meter por send, solo-safe, PFL, AFL ni multicanal.
- Mute/Solo y Send Mute son discretos.
- Los buses suman una mezcla común y no conservan procedencia por rama.
- No hay limiter ni clamp; varias ramas pueden superar `[-1, 1]`.

## Bus Sends 0.2.4

`SendSource = TrackId | BusId` deja de ser solo una previsión del modelo: ambos
orígenes se compilan y ejecutan. Se conservan `AddTrackSend` y `AddBusSend` como
comandos explícitos para mantener el mejor tipado en los puntos de creación;
los parámetros y la eliminación continúan siendo genéricos por `SendId`.

El flujo de cada bus es:

```text
acumulación completa de entradas
    |-> PRE-FADER/PRE-BALANCE TAP -> sends pre -> buses destino
    `-> Bus Gain -> Balance -> Mute -> POST TAP
                                      |-> sends post -> buses destino
                                      |-> Bus Meter
                                      `-> main output -> Bus/Master
```

El bus se visita exactamente una vez por subbloque y por frame obtiene un único
estado suavizado, un `preTap` y un `postTap`. Cada rango de sends distribuye uno
de esos valores al buffer de destino. Cada `SendMixSmoother` avanza una vez por
frame incluso con mute, audibilidad cerrada o entrada silenciosa. No hay buffers
por send ni búsqueda de `SendId` en RT.

`PreparedSendDescriptor` resuelve `sourceKind`, índice denso de origen, bus e
índice de buffer destino, tap, índice de smoother e índice de audibilidad. Los
descriptors quedan en rangos contiguos pre/post dentro de `PreparedTrackRoute` o
`PreparedBusNode`. `ProcessingPlanRuntime` mantiene un smoother por send y un
buffer estéreo por bus.

El grafo estructural contiene todas las aristas Bus Output y Bus Send. Mute,
Send Mute y −100 dB no retiran dependencias. Las aristas paralelas se deduplican
solo para DFS/Kahn; sus descriptors permanecen separados y se suman en DSP. El
orden estable por `BusId` garantiza que todo origen preceda a cada destino y que
las convergencias estén completas antes de procesar el bus receptor.

Solo distingue formalmente dos permisos. `needsFullBusContent` selecciona las
aristas upstream necesarias para calcular un bus; `busMain` autoriza únicamente
su salida principal. Por ello, un bus explícitamente en Solo abre su main y sus
sends propios, mientras un bus usado solo como transporte no abre ramas
laterales. Un Aux en Solo abre todas sus entradas Track/Bus, conserva los dry
paths paralelos cerrados y abre solo el camino necesario desde el Aux a Master.
Varios solos forman la unión de estas selecciones.

Bus Mute y Bus Gain a −100 dB cierran main y post-send; el pre-send ignora Gain,
Balance y Mute. Send Mute y la máscara de audibilidad cierran solo su arista. El
Bus Meter continúa leyendo el post-tap del canal, no las salidas de sus sends;
por eso puede marcar cero mientras un pre-send alimenta un Aux.

Crear, eliminar, retargetear o cambiar el tap exige transporte detenido. Se
prepara y valida un plan candidato completo, se comprueban referencias, límites,
memoria y ciclos, y el commit existente intercambia modelo/plan/runtime con el
callback quiescente. Si falla cualquier fase, el estado publicado anterior se
conserva. Los límites conscientes son 1024 sends totales, 64 por pista y 64 por
bus, 256 pistas, 64 buses, estéreo y 16 MiB para el plan/runtime portable.

### Diferencias entre Track Send y Bus Send

- El Track pre-tap adapta primero mono a estéreo; el Bus pre-tap recibe una
  acumulación ya estéreo.
- El Track post-tap incluye Track Gain, Pan y Mute; el Bus post-tap incluye Bus
  Gain, Balance y Mute.
- Ambos aplican después audibilidad, Send Mute y Send Level, y ambos usan el
  mismo smoother y destino `BusId`.
- Track Sends no crean dependencias entre buses; Bus Sends sí participan en el
  DAG y el orden topológico.

No se añaden inserts, plugins, PDC, feedback, automatización, PFL/AFL,
solo-safe, multicanal, routing durante Play ni meters por send.

## Processor & Insert Core 0.3.0

### Dos fronteras distintas

`IRealtimeAudioProcessor` conserva una única responsabilidad: adaptar el buffer
de salida que entrega el dispositivo a `RealtimeAudioEngine`. No representa un
insert. El nuevo `processors::IAudioProcessor` es core-only y define la unidad
DSP insertable:

```text
prepare(ProcessingFormat)       // no RT; puede reservar o fallar
processBlock(context, in, out)  // noexcept, acotado, sin reservas
reset()                         // noexcept y con procesamiento quiescente
applyParameter(event)           // noexcept; valor ya validado
latency / tail / capabilities
```

`ProcessingFormat` fija sample rate de procesamiento, block size máximo,
layout mono/estéreo y modo realtime/offline. `AudioBlockView` y
`ConstAudioBlockView` expresan canales y frames explícitos. El tiempo del
contexto sigue siendo `PreciseProjectFramePosition`; la latencia DSP utiliza el
tipo distinto `ProcessingFrameCount`, por lo que no puede sumarse
accidentalmente a frames de archivo, proyecto o dispositivo.

El sample rate lógico del proyecto continúa siendo la autoridad temporal. El
sample rate del procesador es el del contexto de ejecución/dispositivo y no
puede ser redefinido por un WAV ni por el procesador. El adaptador detecta una
reapertura con frecuencia distinta, retira el callback, recrea/prepara el bundle
y vuelve a registrar el consumidor. El motor rechaza procesar un bundle cuyo
formato no coincida con el callback recibido.

### Modelo editable, compilación y ownership

Cada `AudioTrack`, `AudioBus` y Master posee su `InsertChain`. Sus
`ProcessorState` contienen `ProcessorInstanceId` global y monotónico, tipo
estable —`internal.gain` en esta versión—, parámetros deseados, bypass y un
campo portable reservado para serialized state. Nunca contienen el objeto DSP.
`InsertTarget = TrackId | BusId | MasterTarget` identifica el owner solo al
crear; remove, move, bypass y parámetros usan después `ProcessorInstanceId`.

```text
ProjectState editable
  -> ProcessingPlanSpecification con InsertChains
  -> factory crea instancias candidatas
  -> prepare + validación de formato/latencia/memoria
  -> PreparedProcessorDescriptor + rangos por nodo
  -> ProcessingPlanRuntime posee instancias/scratch/bypass delay
  -> PreparedProcessingBundle candidato
  -> commit quiescente y transaccional
```

No se comparte una instancia mutable entre planes. Cada compilación recrea
todo el runtime candidato; si type, estado, formato, capacidad, memoria,
latencia o `prepare()` fallan, no se publica ninguna parte. Tras el swap, el
owner viejo se destruye en aplicación y solo después de retirar el callback.
Los tests de lifetime bloquean una región `processBlock` real y demuestran que
la instancia anterior sigue viva hasta la quiescencia.

El plan limita 16 inserts por cadena y 512 procesadores totales. Su presupuesto
portable sube a 32 MiB e incluye descriptors, mappings, runtime declarado,
líneas dry de bypass, dos scratch por nodo, buses, master, sends y tabla de
posiciones. Toda multiplicación y suma de tamaño se comprueba antes de reservar.

### Orden de señal y granularidad DSP

```text
Track:
source render -> INSERT CHAIN (layout fuente)
              -> PRE tap -> Gain -> Pan -> Mute/Solo -> POST tap -> Meter/Main

Bus:
input accumulation -> INSERT CHAIN (stereo)
                   -> PRE tap -> Gain -> Balance -> Mute/Solo
                   -> POST tap -> Meter/Main

Master:
master accumulation -> INSERT CHAIN (stereo)
                    -> Master Gain -> Master Meter -> Output
```

El Track pre-tap ya no significa «antes de insert»: significa después de la
cadena y antes del channel fader/pan. El Bus pre-tap sigue la misma regla. Una
pista mono atraviesa todos sus inserts como mono; la conversión equal-power a
estéreo ocurre después y no cambia la ley de pan existente.

Cada nodo llena un scratch con todo el subbloque, ejecuta cada insert una vez y
solo entonces recorre las muestras para gain/pan, taps, sends, metering y
routing. Fan-out, sends paralelos, mute, solo o silencio no vuelven a invocar ni
omiten el procesador. Las llamadas virtuales son por procesador/subbloque, nunca
por muestra. Callbacks de 64, 128, 256, 512, 1024 frames y mayores que la
capacidad conservan reloj, smoothers y orden.

Hay dos buffers scratch máximos por nodo. Un procesador out-of-place alterna
entre ambos; para uno exclusivamente in-place, el host copia la entrada al
scratch alterno y entrega allí input/output con aliasing. Así conserva además
la entrada dry original para el bypass. La capacidad queda resuelta en el plan
y no requiere consultas virtuales adicionales en RT. No existen buffers de
audio por send.

### Parámetros, bypass y generaciones

`SetProcessorParameter` direcciona
`ProcessorInstanceId + ParameterId`. La aplicación valida y prepara el valor;
el motor resuelve fuera de RT el índice denso y publica un evento trivial con
generación de plan, índice de procesador, `ParameterId`, valor y frame offset.
Solo se acepta offset cero en 0.3.0. El callback consume el ring SPSC de 64
entradas al principio del callback y descarta generaciones obsoletas. Si está
lleno, motor y `ProjectState` conservan el valor anterior.

`GainProcessor` soporta mono/estéreo, −100..+12 dB, 0 dB unity, −100 dB como
cero exacto, latencia cero y tail none. Su rampa lineal de 5 ms vive dentro de
la instancia: el host no duplica estado de smoothing de parámetros internos.

El bypass es host-controlled y discreto al comienzo de callback. El procesador
sigue recibiendo bloques para conservar su historia. Paralelamente, una línea
dry preasignada avanza siempre; si el insert está bypassed, sustituye el wet por
el dry retrasado exactamente por la latencia declarada. No hay crossfade en esta
versión y un toggle puede producir discontinuidad audible.

### Latencia, tail y reset

La suma de latencia de cadena es comprobada. `PreparedProcessingPlan` conserva:

- latencia de cada insert y cadena;
- latencia en pre/post tap de pista y bus;
- latencia de cada output principal;
- latencia individual de cada Track/Bus Send, sin colapsar aristas paralelas;
- intervalos mínimo/máximo de llegada a buses convergentes;
- entrada y salida de Master.

Mute, Solo, bypass o −100 dB no modifican este grafo. **No hay PDC en 0.3.0**:
la metadata hace visible el problema, pero dos caminos con latencias diferentes
continúan llegando desalineados.

`TailInfo` distingue none, finite, infinite y unknown. No se drenan tails tras
el final lógico: los tests del `FixedLatencyTestProcessor` incluyen los ceros
necesarios dentro de la duración del recurso. Esta es una limitación deliberada.

Stop, fin natural, sustitución de bundle y salida del lifecycle operativo
resetean instancias y bypass delays. El consumo FIFO de Stop→Play ejecuta el
reset antes de arrancar, incluso si ambos comandos llegan al mismo callback.
El primer bloque posterior marca `discontinuity` en su contexto. El mismo
`RealtimeAudioEngine` se usa en modo realtime y offline; con los mismos límites
de bloque y eventos produce el mismo resultado.

### Límites conscientes de 0.3.0

- Solo existe `internal.gain`; no hay hosting AU/VST3 ni ABI de plugins.
- No hay PDC, automatización sample-accurate, tails audibles ni crossfade de
  bypass.
- No hay inserts multicanal ni negociación de layouts más allá de mono/estéreo.
- No se modifica estructura durante Play.
- No hay procesamiento paralelo, pools avanzados ni reutilización de scratch.
- La cadena completa se recrea en cada cambio estructural; el future state
  externo podrá reconstruir únicamente los valores aceptados.

## Source / Clip / Track Foundation 0.4.0

El modelo editable separa tres responsabilidades:

```text
AudioSource (medio original y metadata)
    <- SourceId - AudioClip (projectStart, duration, sourceOffset)
                      <- colección ordenada - AudioTrack (layout estable)
```

`SourceId` y `ClipId` son tipos fuertes de 64 bits, con cero inválido y
contadores monotónicos que forman parte del proyecto. La ruta nunca es la
identidad: cada importación explícita crea otra fuente. `MediaReference` guarda
la ruta original y reserva una ruta relativa preferida para un futuro proyecto
portable, media folder, missing media y relink. `ProjectState` contiene solo
metadata serializable; no contiene PCM, índices densos, smoothers ni objetos
DSP.

Un `AudioClip` conserva exclusivamente `SourceId`, inicio entero en frames de
proyecto, duración precisa `double` en frames de proyecto y offset preciso en
frames fuente. Los finales se derivan. La preparación valida finitud, signo,
overflow, límites fuente y compatibilidad del layout fijo mono/estéreo de la
pista. El final preciso gobierna el renderer; el transporte publica
`ceil(max(clipStart + duration))`, por lo que nunca se pierde una fracción final
ni suena contenido fuera del clip.

El adaptador JUCE decodifica WAV una vez y conserva un owner PCM inmutable por
`SourceId`. El plan contiene un array denso de `PreparedSourceView`; cada
`PreparedClipView` guarda directamente su índice fuente, límites precisos,
offset y ratio `sourceRate/projectRate`. Diez clips de una fuente comparten un
único buffer. Quitar clips no elimina la fuente; quitar una fuente referenciada
se rechaza. No existe cleanup automático.

Por pista, los clips preparados mantienen orden `(projectStart, ClipId)` y un
array paralelo `prefixMaximumEnd`. Para cada subbloque RT se hacen búsquedas
binarias de los posibles intersectores y solo se recorren candidatos vivos. El
render recalcula cada posición fuente desde el reloj maestro global; no existe
cursor ni reloj por clip. El scratch de entrada se limpia, suma todos los clips
activos en orden canónico y después atraviesa exactamente una vez la cadena de
inserts, mixer, sends, meter y routing de la pista.

Import, alta, eliminación y movimiento mínimo de clips son cambios
estructurales y se rechazan durante Play. La aplicación construye un
`ProjectState` candidato, valida/prepara o reutiliza las fuentes, compila clips,
índice y plan, retira el callback y publica modelo/owners/plan en un único commit
`noexcept`. Un fallo deja intacto el bundle activo. El bundle retirado se
destruye después de la quiescencia, nunca desde el callback.

El modelo queda preparado para persistencia con versión de esquema futura y
para Undo/Redo por IDs y valores: no se persistirán PCM, ratios, scratch,
generaciones ni posiciones RT. Split, trim y duplicate futuros podrán conservar
`SourceId` y ajustar únicamente nuevos valores de clip, sin mutar la fuente.
Gain/fades de clip pertenecerán entre render de fuente y suma de pista, pero no
se incluyen en 0.4.0.

Límites de preparación: 256 pistas, 2.048 fuentes, 32.768 clips totales y 4.096
clips por pista. El presupuesto PCM JUCE de 512 MiB permanece separado del
presupuesto portable de plan/runtime de 32 MiB. Continúan pendientes timeline
visual, edición avanzada, recording, stretch, MIDI y hosting de
plugins externos.

## Timeline Editing Operations 0.4.1

Las operaciones `MoveClip`, `DuplicateClip`, `SplitClip`, `TrimClipLeft`,
`TrimClipRight` y `DeleteClip` entran exclusivamente como comandos portables.
`ClipId` es único en el proyecto, por lo que la API pública no expone índices
de almacenamiento ni exige `TrackId` para editar. Cada operación produce un
`ClipEditResult` explícito (`success`, `clipNotFound`, `invalidPosition`,
`zeroLengthClip`, `sourceBoundsExceeded` o `capacityExceeded`); las excepciones
quedan reservadas a fallos extraordinarios como memoria agotada.

Move conserva todo salvo `projectStart`. Duplicate crea un `ClipId` monotónico
y conserva pista, fuente, offset y duración. Split conserva el ID izquierdo y
crea el derecho; el punto debe ser estrictamente interior y el offset derecho
se calcula como `oldOffset + deltaProject * sourceRate / projectRate`, sin
redondear a source frames enteros. Trim Left avanza conjuntamente inicio y
offset y reduce duración; repetir el mismo inicio es un no-op válido. Trim Right
solo reduce duración y repetir el mismo final también es un no-op. Delete no
retira la fuente ni su PCM.

Tras cambiar un inicio se restaura el orden `(projectStart, ClipId)`. Overlaps
continúan sumándose y gaps producen silencio. El final lógico sigue derivándose
como el máximo final preciso de los clips. `findClip`, `clipsForTrack`,
`clipsIntersectingRange` y `projectContentDuration` forman la consulta portable
mínima para una futura UI.

Cada comando copia el modelo activo, edita el candidato, prepara de nuevo clips
e índice temporal reutilizando la caché PCM por `SourceId`, espera quiescencia y
publica modelo/owners/plan con commit `noexcept`. Un rechazo o fallo de
preparación no consume estado activo. No hay edición durante Play.

Para un Undo futuro deben capturarse valores, no punteros: Move necesita el
inicio anterior; Trim Left, inicio/offset/duración; Trim Right, duración;
Split, el clip original y el nuevo ID derecho; Duplicate, el ID creado; Delete,
el clip completo y su pista. Los contadores monotónicos nunca retrocederán.
0.4.1 no implementa el stack de Undo.

La ventana contiene controles de prueba para IDs iniciales y posiciones fijas.
No representan una interfaz de timeline ni forman parte del dominio.

## Undo / Redo Foundation 0.4.2

`Command` sigue representando intención. `UndoableOperation` representa el
cambio reversible confirmado; el dispatcher no registra historial. Undo y Redo
son comandos y `DawApplication` coordina el modelo candidato, la preparación del
plan y el historial. `UndoManager` es portable, pertenece a la aplicación y solo
se consulta/muta en su hilo serializado. No hay lógica de historial en RT ni
dependencia de JUCE. Una futura `ProjectSession` podrá asumir este ownership.

El payload es una variante cerrada: Move/TrimLeft/TrimRight guardan TrackId y
AudioClip before/after; Duplicate guarda el clip creado; Delete el eliminado;
Split guarda original, izquierdo y derecho como una entrada atómica. Guardar
el AudioClip completo también para Move conserva todos sus invariantes por un
coste pequeño. Tipo y label key `history.*` se derivan de la variante. Todos los
payloads son valores fijos: no contienen PCM, pointers, índices densos, planes,
processors runtime, transporte, smoothers, meters ni scratch.

`ProjectState` ofrece restauración/reemplazo privados, accesibles únicamente
desde `UndoableOperation`. Restaurar exige ID válido, ausente y menor que el
high-water mark, track/source existentes, layout compatible y límites válidos.
Reemplazar exige la identidad existente en la pista. Undo/Redo comprueba primero
el estado esperado de las entidades afectadas. Una aplicación fallida puede
haber alterado su candidato desechable, nunca el estado activo. Los contadores
no retroceden: Undo de Duplicate/Split consume definitivamente el ID y Redo lo
restaura; abandonar Redo no permite reciclarlo.

El almacenamiento es `vector<HistoryEntry> + cursor`: `[0,cursor)` aplicado,
`[cursor,size)` Redo. Antes de preparar el plan se construye un `PendingAppend`
con las entradas aplicadas retenidas y la nueva operación. Allí se calcula la
expulsión y se reserva/copía la memoria; la rama Redo activa sigue intacta.
El commit bajo quiescencia intercambia modelo y vector mediante `swap noexcept`
y fija duración, cursor, token y revision. El vector sustituido se destruye al
salir del comando, en el hilo de aplicación. Para Undo/Redo no se copia el
historial: se mantiene cursor/entrada, se aplica al candidato, se prepara un
plan nuevo y solo dentro del commit exitoso se mueve el cursor.

Cada append cuesta O(historial retenido), acotado a 512 entradas pequeñas.
Esto simplifica la garantía fuerte sin guardar snapshots completos de proyecto.
Durante staging coexisten temporalmente los dos vectores. El presupuesto del
historial confirmado es 8 MiB, con estimate `sizeof(HistoryEntry)` por entrada y
capacidad real del vector para informar memoria. El límite de 512 se alcanza
antes que el de bytes con los payloads actuales. Las pruebas reducen el límite
de bytes para ejercitar esa misma rama sin introducir payloads artificiales de
varios MiB. Se expulsan solo entradas aplicadas antiguas; una entrada que no cabe
se rechaza antes de tocar proyecto o historial.

Una nueva edición solo descarta Redo cuando hace commit. Cualquier mutación
persistente no soportada por Undo actúa como barrera al hacer commit, incluyendo
el comando legado RemoveClip; DeleteClip es la operación undoable. Import,
AddClip, RemoveSource, mixer, routing, sends e inserts son barreras. Para los
parámetros ligeros se preparan los mensajes antes de publicar a la cola, y la
barrera se confirma tras actualizar el modelo sin allocations posteriores.
Fallos y Play/Stop no limpian historial. Los no-ops de Move/Trim no generan
entrada, token ni invalidación de Redo.

Los errores incluyen `nothingToUndo`, `nothingToRedo`,
`transportMustBeStopped`, `historyInvalid`, `historyCapacityExceeded` y
`preparationFailed`; `validationFailed` queda definido en el contrato común.
Las validaciones de edición conservan sus errores precisos existentes. Una
divergencia de payload/entidades durante recorrido produce `historyInvalid`;
un fallo preparando o publicando el plan deja operación y cursor disponibles.
Las excepciones de memoria se capturan fuera de RT. No se salta una operación
fallida ni se detiene automáticamente Play para permitir Undo.

Cada commit persistente nuevo/barrera recibe un StateToken nuevo. Undo restaura
beforeStateToken y Redo afterStateToken. `revision` aumenta en todos esos
commits. Un cliente futuro podrá conservar `savedStateToken` y comparar tokens
para dirty state; comparar solo revision sería incorrecto. Tokens/revision no
se publican hacia RT y su agotamiento se rechaza sin wrap-around. `clearHistory`
solo elimina entradas, sin cambiar la identidad del documento. La historia no
se guarda en 0.4.2; 0.4.3 añade la persistencia descrita debajo. No hay gestures/coalescing, composites genéricos,
Undo de parámetros/routing/processors ni Undo durante Play. En el futuro,
composites y snapshots de submodelos reutilizarán el mismo límite de commit.

## Project Persistence 0.4.3

### Documento y sesión

`ProjectSession` contiene ProjectState, UndoManager, projectFilePath y
savedStateToken. UndoManager sigue siendo la única autoridad de token/revision;
`dirty = currentStateToken != savedStateToken`. No posee dispositivo, planes ni
DSP. DawApplication coordina SaveProject, SaveProjectAs(path) y
LoadProject(path, discardUnsaved). La UI proporciona paths portables, muestra
errores y solicita consentimiento de descarte; no accede a audio ni JSON.

`.vitadaw` es JSON UTF-8, `format=VitaDAWProject`, inicialmente `schemaVersion=1`
(desde 0.5.2 se guarda v2 y se migra v1 en memoria) y
writerAppVersion opcional. ProjectDocument contiene un DTO explícito; el codec
enumera campos, no serializa automáticamente structs. Incluye settings, todos
los IDs/counters, fuentes, pistas/clips/layout/mix/inserts, routing editable,
buses, sends y master. Excluye PCM, índices densos, orden topológico, scratch,
smoothers/meters/queues, historial, dispositivo, posición de transporte y UI.

IDs y nextIds uint64 son strings decimales canónicos, sin signo ni ceros a la
izquierda. Cero no es ID válido; nextId debe superar todos los IDs existentes.
No se genera ningún ID durante Load ni se reconstruye nextId con max+1.
ProjectState::fromDocumentData valida el conjunto completo antes de adoptar
privadamente los valores. RoutingState no expone mutación privada al parser.
Se validan referencias, bounds/layouts, parámetros y DAG incluyendo sends.

Los enums son strings estables; floats/doubles finitos conservan round-trip.
Los keys se ordenan lexicográficamente; fuentes/rutas/sends por ID; clips por
(projectStart, ClipId). Orden de pistas, buses y cadenas se conserva por ser
semántico; parámetros se ordenan por ParameterId. Dump usa dos espacios,
newline final y conversión independiente de locale, sin timestamps.
El mismo estado/destino/versión produce bytes idénticos.

El registro de migraciones sobre DOM contiene desde 0.5.2 el migrador v1→v2.
No se inventan migraciones, no se abre parcialmente un esquema futuro y se rechazan
campos desconocidos. `internal.gain` guarda parámetros por ID, bypass y estado
serializado vacío. Un tipo desconocido devuelve processorUnavailable.

### Medios y aislamiento

MediaReference guarda kind localFile, relativePath opcional, absoluteFallback
opcional y fingerprint SHA-256 completo/tamaño decimal. Al menos una ruta es
obligatoria. Las rutas no expanden variables, comandos, home ni URLs. Se resuelven
contra el directorio del documento, nunca contra cwd. No equivalen a identidad.

JUCE decodifica un MemoryInputStream sobre los mismos bytes inmutables que se
hashean con CommonCrypto, fuera de RT. La lectura es acotada y rechaza crecimiento,
truncado o cambios de metadata durante lectura. En Load el fingerprint esperado
se comprueba antes de decodificar. Tras decode se verifica de nuevo mediante
lectura/hash por bloques, sin retener otra copia del archivo. Si no coincide,
el candidato falla. Esta segunda pasada protege frente a sustitución durante la
preparación. Es trabajo por Source, no por Clip. Save reutiliza la identidad ya
asociada al PCM y no vuelve a hashear el archivo que podría haber cambiado.

Load prueba primero la ruta relativa y luego el fallback, ambos verificados.
Si ninguna sirve, devuelve missingMedia/mediaChanged u otro fallo explícito de
preparación/I/O. No hay proyecto parcialmente offline. El path resuelto se guarda
en la referencia de sesión/modelo; Save As proyecta rutas desde ese medio hacia
el nuevo destino, mantiene fingerprint e IDs y no cambia ProjectSettings.name.

`prepareProjectReplacement` está separado de prepareProcessingPlanWithAudio:
parte de una colección de recursos vacía, completamente independiente del
proyecto abierto. Solo después crea el plan. Un SourceId tiene alcance de
documento; coincidencias A/B nunca autorizan reutilizar PCM. Las ediciones y Undo
dentro del mismo proyecto siguen compartiendo sus fuentes existentes.

### Save, Load y commit

Save requiere Stopped y destino; captura token/modelo en el único hilo de
aplicación, valida, proyecta rutas, serializa y limita bytes. La ruta futura queda
preparada antes de I/O. NativeProjectFileIO crea temporal exclusivo con mkstemp
en el mismo directorio, write completo con EINTR, fsync + F_FULLFSYNC, rename y
fsync del directorio. Solo después intercambia path y savedToken sin allocations.
Antes del rename, cualquier fallo conserva archivo/sesión. Después del rename,
un fallo de confirmación devuelve durabilityUncertain; bytes nuevos pueden estar
visibles, no se afirma rollback ni se marca clean ni se intenta otro replace.
El temporal fallido se elimina; no hay .bak ni backups visibles.

Load requiere Stopped y autorización explícita si dirty. Hace bounded read, SAX,
DOM/migración, validación, factory exacta, resolución/hash/decode de todas las
fuentes, plan aislado y staging de metadata. El commit existente retira callback
y espera quiescencia, intercambia recursos/plan, adopta modelo/path/historial/token
con función noexcept y reconfigura RT. Destruye el estado sustituido fuera de RT.
Un fallo anterior conserva todo A, incluidos PCM, plan, historial y tokens.
Si falla reconectar después del commit, B permanece cargado y el dispositivo
entra en error: no se describe como rollback del Load.

ProjectSession::adopt ejecuta una barrera específica de documento: historial
vacío, token nuevo, revision monotónica en esta sesión, savedToken=current y
dirty=false. Transporte Stopped/posición cero. Save no modifica historial:
Move→Save→Undo queda dirty; Load→Move→Undo vuelve a clean. Los cambios no
undoables continúan siendo barreras. Ninguna de estas autoridades se persiste.

### Límites y errores

El backend de archivos y fingerprint validado oficialmente es macOS. El codec,
DTO y comandos son portables; no se declara atomicidad/durabilidad equivalente
en Windows/Linux. IProjectFileIO permite dobles/fallos por fases. El SHA-256 no
es una implementación criptográfica propia.

Archivo 16 MiB (stat y lectura real), depth 32, path/name 4 KiB UTF-8, key 128 B.
SAX rechaza claves duplicadas, tamaños, nesting y cardinalidades antes del DOM;
además limita nodos a 500000, strings de valores agregadas a 8 MiB y keys por
objeto a 64. Contenedores JSON tienen allocator de 32 MiB; se reserva el resto
del sobre de 128 MiB para texto de entrada/salida, strings, DTOs y limpieza.
Estas cotas conservadoras pueden rechazar antes del máximo nominal de entidades.
El owner del DOM vacía recursivamente hijos con profundidad acotada antes de
destruirlo: evita la allocation del stack dinámico del destructor general JSON.

Modelo: 256 tracks, 2048 sources, 32768 clips/4096 por track, 64 buses,
1024 sends/64 por origen, 512 processors/16 por cadena. El presupuesto PCM
continúa en 512 MiB y durante Load cuenta PCM activo + PCM candidato + bytes WAV
retenidos; scratch/DSP mantiene su presupuesto preparado independiente.
Todo parsing/hash/decode/I/O ocurre fuera de RT; callback/clock/mixer no cambian.

PersistenceResult es code + phase + contexto opcional (path, campo/pointer,
entidad/ID, error de sistema), sin mensajes UX traducidos. La UI muestra códigos
estables; excepciones recuperables de preparación/capacidad se convierten en
resultados. El guardado solo se confirma después de la confirmación del backend.

## Timeline UI Foundation 0.5.0

La capa portable `ui/timeline` construye un `TimelineSnapshot` por valor desde
el estado confirmado de aplicación. Incluye sample rate lógico, duración de
contenido, posición/estado del transporte, revisión, pistas, clips, IDs y labels
derivados; excluye PCM, planes RT y referencias mutables. `TimelineComponent`
depende del dispatcher y de consultas const de `DawApplication`, nunca muta
`ProjectState` ni controla JUCE audio.

La única conversión geométrica es:

```
seconds = projectFrame / projectSampleRate
x = (seconds - visibleStartSeconds) * pixelsPerSecond
```

`CoordinateTransform` limita el zoom a 20–600 px/s, limita scroll a tiempo no
negativo y convierte de vuelta a project frames con redondeo al frame más
cercano. El zoom conserva el instante bajo cursor; el scroll puede extender el
viewport diez segundos más allá del contenido, con un mínimo visual de treinta.

`TimelineInteraction` contiene únicamente selección por `ClipId` y un preview
de Move/Trim. Captura el clip confirmado al mouseDown, calcula el preview durante
mouseDrag y produce cero o un `Command` en mouseUp. No toca el modelo y no genera
historial intermedio. Tras éxito o fallo se descarta el preview y se consulta de
nuevo el snapshot; si desaparece el ID seleccionado, la selección se limpia.
Durante Play los controles y gestos estructurales están deshabilitados.

Los clips se pintan directamente, sin un árbol de Components por clip ni acceso
a filesystem durante paint. El hit-test central da prioridad a handles y luego
al cuerpo; el pintado descarta rectángulos fuera del viewport. La regla adapta
sus intervalos al zoom. El playhead procede del `TransportState` sincronizado a
30 Hz por la aplicación, nunca del callback ni de otro reloj.

Duplicate sitúa la copia en el final exclusivo del original. Split requiere que
el playhead esté estrictamente dentro. Delete no borra Source. Los shortcuts
provisionales son Cmd+Z, Cmd+Shift+Z, Cmd+D, Delete/Backspace y S. No son aún un
sistema configurable. Load reconstruye el componente, limpia selección y vuelve
el viewport al origen; Save/Save As no alteran geometría.

No hay Seek formal, waveform, snapping musical, multiselección, context menu,
fades, clip gain, tempo grid ni edición durante Play. El culling sigue siendo
lineal sobre clips ordenados, suficiente para el objetivo probado de 1000 clips.

### Lifecycle nativo y cierre parcial

`moreThanOneInstanceAllowed()` devuelve false. En JUCE 9, una segunda instancia
puede hacer que `JUCEApplicationBase::initialiseApp()` retorne antes de invocar
`VitaDawJuceApplication::initialise()`, pero `JUCEApplicationBase::main()` llama
igualmente a `shutdownApp()`. Por tanto, `shutdown()` no implica que exista
ningún owner.

La fase de startup es solo diagnóstico. La autoridad de cleanup es la presencia
de los `unique_ptr`, de modo que cualquier prefijo de construcción es un estado
válido. `ApplicationShutdown` ejecuta esta secuencia:

```
stop Timer/event producers
-> clear device state callback while adapter and receiver still live
-> shutdown audio adapter and quiesce its RT callback
-> destroy MainWindow
-> destroy CommandDispatcher
-> destroy DawApplication (non-owning reference to adapter)
-> destroy JuceAudioDeviceAdapter
```

La segunda llamada solo repite el `stopTimer()` idempotente. El cierre del
adaptador también es idempotente. El callback de estado captura la aplicación
JUCE y consulta `mainWindow_`; se retira antes de que la ventana pueda morir.
`MainWindow::content_` es únicamente un observador: `DocumentWindow` posee el
componente entregado mediante `setContentOwned()` y lo sustituye de forma
síncrona. No se añade logging al hilo de audio.

## Seek & Transport Navigation 0.5.1

`ProjectFramePosition` sigue siendo la única autoridad temporal. El reloj RT
tiene tres estados: Stopped, Playing y Paused. Play continúa desde la posición
actual; Pause conserva posición y estado interno de processors; el primer Stop
desde Playing/Paused conserva posición y reinicia processors/dry delays, y Stop
estando ya Stopped vuelve a cero. El final natural queda Stopped en el límite
exclusivo y Play desde ese límite reinicia a cero.

Pause y Seek viajan por el mismo ring SPSC y `CommandLifecycleGate` que Play y
Stop. El CAS final de aceptación sigue siendo el punto de linearización; RT
consume el comando antes del render del siguiente callback/subbloque. Seek
Stopped/Paused acepta `[0, contentEnd]`, rechaza valores negativos, posteriores
al final, y `DawApplication` rechaza cualquier Seek durante Playing. El motor
no consulta un snapshot atrasado al encolar: así un `Pause → Seek` aceptado por
el productor conserva su orden FIFO aunque RT aún no haya consumido Pause. Si
un Seek interno alcanzase al consumidor todavía Playing, el reloj lo rechaza.
Su latencia máxima normal es un callback de dispositivo. Seek resetea processors
y bypass delays y marca
`discontinuity=true`; Pause no procesa audio, no avanza el reloj y no resetea.

La regla convierte pixel a frame mediante `CoordinateTransform` y despacha
`SeekToProjectFrame`; nunca escribe transporte. La posición visible y Split
usan el mirror confirmado de `TransportState`. Segundos (`mm:ss.xxx`) y frames
son vistas derivadas del mismo frame y sample rate lógico. Space alterna
Play/Pause y Home/End despachan navegación. El transporte no se persiste, no
entra en Undo, no cambia `StateToken` y Load conserva Stopped/0.

No hay tempo, beats, loop ni metronome. El roadmap inmediato reserva 0.5.2 para
Musical Time Foundation y 0.5.3 para Loop & Metronome.

## Musical Time Foundation 0.5.2

### Autoridad, unidades y anclas

`RealtimeProjectClock` / ProjectFrame es el único reloj que avanza. El mapa
musical es una transformación consultable, no un segundo acumulador. El módulo
`musical/` no depende de JUCE. El callback y los contratos de audio no cambian.
La posición precisa existente permite conservar fracciones de frame cuando
project/device sample rate difieren; el frame entero sigue siendo el destino de Seek.
El sample rate del mapa preparado procede del proyecto, nunca del dispositivo
ni de un recurso fuente. Load recompila con el sample rate del nuevo documento.

- `MusicalTickPosition`, `MusicalTickDuration`: int64 fuerte, PPQ=15360 fijo.
- `QuarterNotePosition`: double fuerte continuo; los ticks NO son resolución de audio.
- `BarIndex`, `BeatIndex`, `TickWithinBeat`: int64, cero-based. Display bar/beat +1.
- `TempoBpm`: double finito 20–400; siempre negras por minuto, incluso en 6/8.
- `MusicalPosition` necesita un mapa de métrica para tener significado absoluto.

`MusicalTimeMap` pertenece a ProjectState como submodelo documental, con
`TempoMap` y `TimeSignatureMap`. TempoEvent ancla exclusivamente tick, BPM y
curve=step. TimeSignatureEvent ancla exclusivamente BarIndex y N/D. El tick
de cada cambio de métrica se deriva de los compases anteriores durante preparación.
En N/D, ticksPerBeat=PPQ*4/D y ticksPerBar=N*ticksPerBeat. En 6/8 el display
cuenta corcheas, sin asumir agrupaciones de metronome. N=1..32; D potencia de
dos entre 1 y 64. Las anclas son únicas, ordenadas; duplicados se rechazan.

Ambos mapas requieren evento inicial (tick/bar cero), por defecto 120 y 4/4.
Se permite Set del inicial, nunca Move/Delete. Los IDs tipados son uint64 no
cero y estables, con contador monotónico que no retrocede al hacer Undo.
Se rechaza el agotamiento antes de sumar; los IDs eliminados no se reciclan.

### Preparación y conversiones

`PreparedMusicalTimeMap::compile` valida y prepara fuera de RT tres colecciones:
segmentos de tempo para presentación (tick, quarter, segundos prefijo, BPM,
segundos por negra), segmentos de tempo racionales para DSP y segmentos de
métrica (bar, tick, N/D, ticks por beat/bar). El siguiente elemento proporciona
el límite exclusivo; el último se extrapola dentro del dominio numérico. Los
prefijos de segundos usan suma compensada para UI. Las fronteras DSP no se
obtienen de esos prefijos: integran exactamente los bits binary64 de rate/BPM.

En un segmento: `seconds = startSeconds + (q-startQuarter)*60/BPM`;
inversa `q = startQuarter + (seconds-startSeconds)*BPM/60`.
`preciseFrame = seconds*projectSampleRate`. Consultas individuales por búsqueda
binaria O(log n), intervalos [start,next). El display usa floor y comprueba
las fronteras representadas en frames para normalizar el error de inversión
sin redondear anticipadamente al siguiente tick. Las políticas son explícitas:
nearest (empate hacia arriba) para Seek; floor para display; ceil únicamente
para un límite exclusivo que lo requiera. No se promete reversibilidad de
Tick→frame entero→Tick ni Frame→display→Frame. Sí se prueba el round-trip
continuo y frame entero→quarter continuo→frame entero.

Máximo 4096 eventos de cada tipo y presupuesto de segmentos de 1 MiB. Dominio
numérico conservador: ticks y project frames no negativos hasta 2^40, además
de comprobar todos los productos y resultados finitos. A 96 kHz representa
unos 132 días en frames; la intersección con el límite de ticks depende del BPM.
Las consultas fuera del dominio devuelven error, no clamp ni casts indefinidos.

Grid usa span acotado, sin allocations: bars/beats/subdivisiones exactas del beat
(divisor 1..64 que divida ticksPerBeat). Devuelve cantidad, error, hasMore y
nextStart para continuar en la primera línea omitida. Busca el segmento inicial
y avanza el cursor de métrica, sin recorrer desde el origen ni acumular frames
redondeados. Para evitar escanear miles de cambios de tempo entre dos líneas,
prepara un índice radix comprimido auxiliar (máximo 2*n-1 nodos, dentro del
mismo presupuesto de 1 MiB). Cada bifurcación elimina un bit distinto del
dominio fijo de ticks: búsqueda acotada a 41 pasos por línea, independiente
del número de eventos atravesados. Conserva las dos tablas semánticas separadas.
Así la enumeración es O(log n+k) en este dominio fijo; cada frontera de métrica
es una línea de compás y no hay recorridos de segmentos sin salida. El índice
es inmutable y se prepara fuera de RT; no es un reloj ni un mapa adicional.

### Edición transaccional y lifetime

UI→ICommandDispatcher→DawApplication. Add/Move/Set/RemoveTempoChange (Set se
llama `SetTempo`) y equivalentes de métrica identifican eventos concretos.
Solo Stopped. Candidato→edición→validación→compile→stage Undo→commit por swaps
noexcept de modelo/mapa/historial. Nada se publica si falla una allocation o
validación. El commit musical NO usa el commit de routing que rebobina.
Conserva frame, duración, PCM y plan RT. Undo/Redo guarda before/after de eventos,
no mapas preparados; reconstruye antes del commit, restaura IDs exactos y
preserva contadores. StateToken cambia con cada edición confirmada y dirty
se deriva del saved token. No se limpia el historial innecesariamente.

El mapa preparado tiene owner `unique_ptr<const ...>` en DawApplication y
revisión monotónica no persistida. UI consulta una referencia de vida limitada
en el hilo serializado de aplicación. El mapa anterior se destruye allí tras
el swap. En 0.5.2 **no se publica a RT**: añadir un consumidor RT en el futuro
requerirá un protocolo explícito de publicación/reclamación; const no basta.
Los clips siguen absolute lock en ProjectFrame. Musical lock, loop y metronome
podrán consultar este mapa, pero no se implementan ni alteran el render actual.

### Persistencia y UI

Schema2 añade `musicalTime`: ppq numérico 15360, tempoEvents, signatureEvents,
nextTempoEventId y nextTimeSignatureEventId. IDs, tick y barIndex se escriben
como strings decimales exactos. Orden canónico por ancla/ID; sin posiciones
derivadas, revisiones, UI, ni prepared maps en disco. Validación estricta y
límites SAX antes de reservar arrays; curvas desconocidas y mapas inválidos
rechazan Load sin alterar la sesión activa.

El registro contiene el migrador real v1→v2: valida esquema y semántica v1,
añade defaults solo al DOM candidato (IDs1, nextIDs2), cambia versión a2 y
valida v2 completo. Se prepara el mapa musical ANTES del audio y se adopta
junto al documento/plan mediante el commit transaccional de Load. El archivo
v1 permanece intacto; sesión migrada limpia e historial vacío. Save posterior
escribe v2. El fixture future-version ahora contiene3; los fixtures v1 se conservan.

Ruler Seconds/Frames/BarsBeats cambia etiquetas/líneas, nunca el eje absoluto.
Viewport acotado a 512 líneas; densidad bars/beats/subdivisiones según zoom,
sin generar ticks individuales ni millones de líneas. El read model incluye
revisión musical y posición opcional; UI consulta el prepared map sin copiar
4096 eventos a 30 Hz. Seleccionar modo de display no despacha mutaciones ni
marca dirty. Los tres botones musicales son exclusivamente provisionales.

## Loop & Metronome 0.5.3

`ProjectState` conserva opcionalmente `MusicalLoopRange{startTick,endTick}` con
semántica half-open. Ticks son la única autoridad persistente: bars son entrada
de UI convertida una vez, mientras frames y segundos son derivados. El mínimo
se valida sin clamp: 1024 ticks, 10 ms, un project frame y un device frame.
Tempo o métrica recompilan el mismo rango de ticks; los clips continúan fijados
a project frames absolutos.

`PreparedTemporalContext` contiene el mapa musical, el loop preciso, una
revisión común y las tablas normal/accent del click preparadas al sample rate
del dispositivo. Es inmutable: el adaptador JUCE es owner y RT recibe una vista
estable. Un cambio temporal se prepara por completo, retira el callback,
publica contexto/modelo por swaps noexcept, restaura el checkpoint preciso y
destruye el owner anterior fuera de RT. El re-registro controlado cierra la
generación de comandos y exige de nuevo confirmación del consumidor, pero no
confunde ese evento con un reinicio físico ni rebobina el checkpoint restaurado.
Load adopta plan y contexto en una sola
región quiescente. Un reinicio real vuelve a preparar las tablas y conserva el
documento, aplicando el lifecycle normal de dispositivo.

`RealtimeProjectClock` sigue siendo el único reloj. El callback calcula cuántos
frames caben antes del extremo preciso, procesa `[segmentStart,segmentEnd)`,
envuelve a `S + residual` y continúa dentro del mismo callback. No existe una
duración de loop redondeada a device frames. Cada tramo ejecuta lookup, routing,
inserts, sends, buses y master; los meters acumulan el callback completo y se
publican una vez. `LoopWrap` se comunica al processor context sin resetear
processor, bypass delay, smoother, tail ni voces. Seek/Stop son discontinuidad
dura. Sin crossfade, una forma de onda discontinua puede producir click.

La duración de contenido es descriptiva y queda separada de la política de
playback: natural end sin loop/metro, reproducción cíclica con loop y
run-until-stop con metrónomo. El loop puede superar el último clip; fuera de
contenido las fuentes aportan silencio. Un proyecto vacío sin loop ni metrónomo
rechaza Play. No se representa ejecución abierta mediante `INT64_MAX`.

El metrónomo enumera beats desde `PreparedMusicalTimeMap` por tramo. Los eventos
pertenecen a `[start,end)`, se cuantizan al primer device sample no anterior y
una ocurrencia cuantizada al frame posterior se conserva en un slot pending
preasignado. Loop end queda excluido y loop start se emite una vez por vuelta.
Cuatro voces fijas conservan clicks activos al wrap; ante agotamiento se
sustituye determinísticamente la voz más antigua. El click se suma después de
Master Inserts y antes de Master Gain/Meter, independiente de Solo/Mute de
pistas y buses. Sin PDC, audio con latencia declarada puede percibirse retrasado
respecto al click aunque el scheduling sea correcto.

`SetLoopRangeMusical` es documental, Stopped-only, undoable, persistente y
dirty. `SetLoopEnabled` es Stopped-only y de sesión. Los comandos de metrónomo
son POD de sesión, admitidos durante Play, y usan la cola/generación/cancelación
del transporte. Ninguno consume historial ni cambia `StateToken`; UI y read
model observan el estado confirmado por RT.

Schema3 añade `loopRange` nulo o `{startTick,endTick}` exacto. No guarda enabled,
metrónomo, frames compilados ni fase. La migración encadena v1→v2→v3 y el
guardado canónico sigue siendo determinista.

### Corrección de primera importación en 0.5.3

`New Project` es ahora el estado creado directamente por `ProjectSession`: cero
tracks, sources, clips, buses y sends; Master, routing vacío, sample rate lógico
y mapas musicales por defecto sí existen. Se retiró la plantilla de cuatro
pistas que la capa JUCE añadía solo para antiguos smoke tests. No se implementa
una operación general de alta/baja de pistas desde UI.

La política mínima es **B**. `ImportAudioFile` no recibe un `TrackId`: prepara y
valida primero el WAV fuera de RT y construye una copia del proyecto. Si no hay
pistas, añade en esa copia una única pista cuyo layout procede del WAV y conserva
el ID devuelto por `ProjectState::addAudioTrack`; nunca asume ID 1. Si ya existen
pistas, devuelve `noTargetTrack`: una importación posterior debe usar el comando
dirigido y no una heurística de “primera pista compatible”.
`ImportAudioToTrack` permanece disponible para selección explícita y exige que
el destino exista y tenga el layout correcto.

La transacción completa es: ruta seleccionada → lectura/fingerprint/decode →
modelo candidato → selección/creación de pista → Source+Clip → plan preparado →
commit conjunto RT/modelo. El callback de audio nunca participa en esas fases.
Un fallo destruye el recurso candidato fuera de RT y conserva modelo, plan,
historial y `StateToken`. Un éxito publica exactamente un PreparedSource, hace
la barrera no undoable existente, marca dirty y provoca un nuevo
`TimelineSnapshot`/rebuild de la UI. El mapa musical, locators de loop y estado
de sesión de loop/metrónomo no se alteran.

El chooser asíncrono permanece owned por el componente y captura un
`juce::Component::SafePointer`. Cancel despacha una ruta vacía y produce
`userCancelled`; destruir la ventana invalida el SafePointer. Una selección no
se descarta mediante `juce::File::existsAsFile()`: el backend POSIX existente es
la autoridad y clasifica ausencia y `EACCES`/`EPERM`. Formato, decode,
preparación y commit tienen errores de comando separados. VitaDAW no activa App
Sandbox ni requiere security-scoped bookmarks en esta configuración, por lo que
no se añadieron entitlements ni concesiones automáticas.

## Track Operations & Cross-Track Editing 0.5.4

`AudioTrack` continúa siendo un agregado portable identificado por `TrackId` y
con layout mono o estéreo fijo. `addAudioTrack` asigna IDs monotónicos, usa
`Audio N` cuando el nombre viene vacío, añade al final y crea exactamente una
`TrackRoute` hacia Master. Mixer queda en defaults, la cadena de inserts y los
sends están vacíos. No se crean buses ni rutas auxiliares implícitas. El límite
permanece en 256 pistas.

Add y Delete son operaciones estructurales Stopped-only. Se construye primero
un `ProjectState` candidato, después el plan completo y finalmente se publica
modelo, plan e historial en la región quiescente existente. El commit conserva
la posición detenida del transporte; si preparación o publicación fallan,
modelo, plan, historial y token activo no cambian.

El payload de historial no copia el proyecto entero. `TrackHistoryState` posee
solo el índice visual, `AudioTrack` completo, su `TrackRoute` y los sends cuyo
origen es esa pista, cada uno con su posición. No contiene PCM, objetos DSP ni
vistas prestadas. La memoria variable de clips, processors y sends participa en
el presupuesto de Undo. Undo de Add elimina exactamente la pista creada; Redo
la restaura con el mismo TrackId. Delete/Undo conserva nombre, layout, ClipId,
mix, ProcessorInstanceId, route, SendId, tap y parámetros deseados. Los
contadores monotónicos nunca retroceden ni reutilizan identidades abandonadas.

Delete elimina los clips, inserts, mixer y output que viven dentro de la pista,
además de sus sends de origen. No elimina `AudioSource`: puede seguir referenciada
por clips de otras pistas y, aunque quede sin referencias, conserva la política
explícita de lifetime de Sources. Tampoco modifica buses, sends de otros
orígenes, master ni el DAG no relacionado. El plan nuevo retira instancias DSP
y vistas del plan anterior únicamente después de la quiescencia RT.

`MoveClip` representa ahora una transición atómica
`(oldTrackId,oldProjectStart) → (newTrackId,newProjectStart)`. ClipId, SourceId,
sourceOffset y duration permanecen invariantes. El destino se valida contra el
layout de `AudioSource`: mono→mono y stereo→stereo están soportados; cualquier
otra combinación devuelve `layoutMismatch`, sin upmix/downmix. Los overlaps son
válidos y continúan sumándose antes de los inserts de la pista destino. Un Move
dentro de la misma pista reutiliza la semántica horizontal anterior; un Move
vertical requiere transporte completamente Stopped.

`TimelineInteraction` conserva selección efímera e independiente mediante
`optional<TrackId>` y `optional<ClipId>`. Click en header selecciona la pista;
click en clip selecciona ambos IDs. La geometría vertical traduce coordenada y
scroll a un TrackId procedente del snapshot, nunca a un índice persistente. El
drag solo modifica un preview: mouseUp genera un único `MoveClip`; una lane
incompatible se muestra inválida y no despacha mutación. Add/Delete/Load
reconcilian la selección por identidad y mantienen visibles las pistas vacías.

La importación de un proyecto vacío conserva la política 0.5.3 y crea su primera
pista transaccionalmente. Si existen pistas, la UI exige selección y usa
`ImportAudioToTrack`; nunca elige la primera pista. Import continúa siendo una
barrera no undoable y no se amplía en este incremento.

El modelo documental ya almacenaba múltiples pistas, orden, nombres, layouts,
clips, routing, sends, inserts y contadores; por ello 0.5.4 conserva schema v3.
TempoMap, TimeSignatureMap, PPQ y loop locators no cambian con operaciones de
pista. Loop enabled, metrónomo y su nivel siguen siendo estado de sesión.

Limitaciones deliberadas: no hay reorder, multi-selection, auto-scroll durante
drag, confirmación modal de Delete, upmix/downmix, drag externo, waveform ni UI
de routing avanzada.

## Waveform Foundation 0.5.5

`WaveformCache` es estado derivado de `ProjectSession`, no estado documental.
Su clave es exclusivamente `SourceId`; no contiene TrackId ni ClipId y no se
serializa. Cada entrada es un `shared_ptr<const PreparedWaveformData>` estable.
Una carga de proyecto prepara una caché vacía de sesión candidata, por lo que
dos documentos con `SourceId{1}` nunca comparten accidentalmente una entrada.
Un fallo de lectura, fingerprint, decode, plan o commit conserva íntegra la
caché activa anterior.

El adaptador JUCE continúa siendo dueño del PCM. Tras una única decodificación
WAV valida finitud y entrega punteros de lectura temporales a
`waveform::prepareWaveform`; el builder portable recorre ese PCM fuera del hilo
RT y no lo copia. El nivel base agrupa 128 source frames y guarda un par
`{minimum,maximum}` float por canal. Cada nivel siguiente combina dos buckets
del anterior, de modo que no vuelve a leer PCM. Mono ocupa un canal; estéreo
mantiene L/R independientes. `sourceFrameCount` es de 64 bits y el cálculo de
ceil usa `1 + (count-1)/128` para no desbordar.

El presupuesto independiente de la caché es 64 MiB por sesión. Cada entrada
expone `approximateBytes` (objeto y payload de picos); `WaveformCache` mantiene
el total. La estimación se valida antes del recorrido. Si una fuente o el total
superan el límite, la preparación de audio no falla: la fuente sigue siendo
reproducible y la timeline muestra un placeholder con diagnóstico. No hay LRU
ni GC agresivo; mientras la Source pertenezca a la sesión, su entrada puede
permanecer aunque temporalmente no tenga clips.

Import prepara primero PCM, waveform, modelo y plan candidatos. El commit
quiescente intercambia `ProjectState`, plan RT y `WaveformCache` mediante swaps
`noexcept`; el owner sustituido se destruye después, fuera de RT. Load aplica el
mismo patrón con una caché nueva y reconstruye los picos desde medios verificados.
Las operaciones no destructivas y Undo/Redo solo cambian clips o pistas y
reutilizan el handle ya publicado; sus payloads no contienen picos.

`TimelineSnapshot` transporta `SourceId`, `sourceOffset`, projectStart y duration,
pero nunca arrays de peaks. `TimelineComponent` obtiene un handle const por
SourceId y `WaveformView` transforma cada columna visible:

```
project pixel -> clip-local project frames
              -> sourceOffset + local * sourceRate/projectRate
              -> bucket min/max del nivel elegido
```

El sample rate del dispositivo no interviene. El selector usa el nivel más
grueso cuyo bucket no supera aproximadamente un pixel, manteniendo alrededor
de uno o dos buckets visitados por columna a zoom normal/bajo. A zoom alto el
bucket base de 128 frames es el límite deliberado de detalle de 0.5.5. Los clips
y rangos fuera del viewport se descartan antes de dibujar; `paint()` no hace
filesystem, decode, fingerprint, generación de picos ni espera por workers.
Mono se centra en la lane y estéreo usa mitades superior/inferior. Metering,
loop, metrónomo, tempo y el callback de audio no dependen de esta caché.

La generación continúa síncrona en 0.5.5 porque reutiliza el PCM ya residente y
la carga WAV/plan ya es una operación síncrona fuera de RT. Una cola de trabajos
cancelable será necesaria antes de admitir streaming o medios suficientemente
grandes como para bloquear perceptiblemente la UI, pero no se introduce aún.

## Editing & Transport Hardening 0.5.6

Este incremento congela, en lugar de ampliar, la máquina de estados existente.
El hilo de aplicación puede reflejar optimistamente un comando aceptado, pero
solo adopta progreso RT cuando el snapshot ha resuelto al menos la última
secuencia pendiente. La cola conserva FIFO y un cierre de lifecycle resuelve
explícitamente las solicitudes aceptadas. Seek en Playing continúa rechazado;
Pause no avanza ni produce audio; Stop conserva posición desde Playing/Paused y
rebobina únicamente cuando ya estaba Stopped.

La política estructural también queda fijada: Playing rechaza edición e
historial; Paused permite operaciones de clip dentro de la misma pista y
Undo/Redo mediante commit quiescente con checkpoint, pero no Move entre pistas,
Add/Delete Track ni persistencia. Stopped permite todas las operaciones ya
existentes. Ningún rechazo puede cambiar ProjectState, historial, plan o
posición.

Los loops mantienen los requisitos previos de al menos 1024 ticks, 10 ms, un
project frame y un device frame. Se tratan como `[start,end)`, incluso al cruzar
cambios de tempo o métrica, y el render debe ser independiente del particionado
del callback. Los clips conservan duración estrictamente positiva y límites
semiabiertos.

`ProjectState` ya toleraba el error numérico acotado de convertir una duración
fuente a `double` de proyecto y volver a frames fuente. El compilador del plan
aplica ahora exactamente la misma tolerancia relativa (64 epsilon de `double`):
esto acepta únicamente el residuo del round-trip, mientras un exceso real sigue
rechazándose. El cambio vive fuera de RT y no modifica render, interpolación ni
límites de acceso.

La suite de hardening usa PCM sintético propiedad de un adaptador hardware-free,
pero compila `PreparedProcessingPlan` y ejecuta colas, reloj, mezcla, snapshots y
`RealtimeAudioEngine::processBlock` de producción. Los dobles se limitan a
decodificación y filesystem deterministas.

## Transport & Timeline Foundation 0.6.0

### Autoridad y dominio navegable

El transporte conserva tres conceptos separados:

- `contentDuration`: límite exclusivo descriptivo del contenido preparado;
- dominio navegable: posiciones enteras no negativas admitidas por la
  implementación temporal actual;
- `ProjectFramePosition`: locator autoritativo del transporte.

Navegar después de `contentDuration` es válido y no crea ni alarga clips.
`GoToEnd` continúa apuntando al final del contenido. No existe un `projectEnd`
persistente. El dominio actual termina provisionalmente en
`maximumSupportedProjectFrame()` = 2^53−1 se conserva por compatibilidad.
No es la definición conceptual del timeline. Las búsquedas RT de clips utilizan
inicios y finales exclusivos enteros conservadores, no sumas absolutas double.

### Representación exacta preparada

`TemporalInteger.h` proporciona palabras fijas UInt128 y UInt256, carry/borrow,
shifts, los productos necesarios y divmod. El backend portable usa productos
de mitades de 32 bits; el acelerado puede usar __int128. No hay enteros dinámicos.
GCD, descomposición binary64, reducción y certificación solo se ejecutan en
preparación. La división portable recorre como máximo 256 bits.

`ProjectPhase` conserva P entero, magnitud/signo de residuo y denominador D.
El incremento preparado es cociente entero + resto. Pause/checkpoints conservan
la fase; Seek entero la pone a cero. Natural end, wrap y corte de subbloques usan
comparaciones/divmod enteros, sin epsilon/FMA. Un checkpoint no convertible
exactamente se rechaza sin modificar el reloj. El adaptador certifica el checkpoint
antes del commit de un plan que deba conservarlo.
El checkpoint captura además la política efectiva `runUntilStop`; su restore no
la reconstruye desde el estado visible de metrónomo/loop. Esto conserva el caso
en que el metrónomo se desactiva durante Playing pero la ejecución abierta debe
continuar hasta un Stop explícito.

`SourceMapping` guarda factores de hasta 128 bits. El render separa parte entera,
fase y offset, calcula los índices mediante divmod y solo convierte el resto a
floating point después de validar pertenencia y exclusive end. Prepared y legacy
comparten este kernel. No se materializa una coordenada fuente absoluta double
ni el producto absoluto expandido de 296 bits. Los temporales de 256 bits bastan.

El dominio profesional certificado incluye todos los sample rates binary64 de
[1, 1048576] Hz, locator hasta 2^53−1, duración diádica con denominador hasta 2^84,
offset hasta 2^116 y frontera de loop hasta 2^72. Las tasas fuera del intervalo
pueden admitirse en audio si pasan el certificado genérico; el reproducer de
una muestra y sourceRate=0x1.77000000002dcp-38 está incluido.
El scheduler musical requiere project/device rates en ese intervalo para acotar
también su densidad de eventos. SampleRate::isValid no cambia de significado.

El ClockFormat base exige D de hasta 125 bits; su extensión para fronteras
musicales puede ocupar hasta 128 bits, y vuelve a certificar los mappings fuente.
El certificado exige factores persistentes de hasta 128,
parte impar del denominador fuente de hasta 128 y temporales con margen para
las sumas hasta 256. No hay fallback aproximado. La ruta legacy debe preparar el
device rate fuera del callback mediante prepareLegacyDeviceRate; processBlock
no prepara ratios al recibir un rate distinto.

`DspFramePosition` copia P y la fase exacta al scratch preasignado. Su campo double
phase es exclusivamente una vista de presentación. Los presupuestos existentes
contabilizan el nuevo sizeof de descriptores y buffers de posiciones.

`PreparedMusicalTimeMap` mantiene dos rutas deliberadamente separadas. La ruta de
presentación conserva los segmentos de segundos/doubles existentes. La ruta DSP
descompone directamente los bits binary64 contractuales de project rate y BPM y
prepara por segmento `framesPerTick = projectRate / (256 * BPM)`. El anchor del
segmento siguiente es el resultado racional exacto del anterior; no se acumulan
segundos ni se convierte primero una frontera floating point. Cada segmento tiene
su propio denominador y se certifica con temporales UInt256 y componentes UInt128.

El loop musical recorre ticks -> mapa exacto -> `RationalBoundary` -> contexto
temporal certificado. El mismo tick alimenta los segmentos analíticos del
metrónomo. El reductor, la proyección y RT reciben los mismos límites preparados;
no reconstruyen fronteras desde los doubles de presentación. El reloj amplía su
denominador fuera del callback solo para los límites racionales que interactúan
con RT. Una integral, anchor, LCM o conversión de checkpoint que exceda la
capacidad fija se rechaza antes del commit y preserva el contexto anterior.

### Loop Foundation 0.6.2

El contrato del loop permanece `MusicalLoopRange[startTick,endTick)`: start está
incluido y end excluido. La validación estructural exige coordenadas dentro del
dominio, orden estricto y 1024 ticks; una única preparación temporal certifica
además 10 ms, un project frame, un device frame, las fronteras racionales y su
compatibilidad con ClockFormat. No hay clamp, epsilon ni fallback aproximado.

`PreparedLoopView` es un valor derivado e inmutable de una revisión preparada.
Expone posiciones exactas y de presentación, y consultas puras de pertenencia,
distancia a end y módulo post-wrap. No accede al engine, no asigna memoria y no
expone ClockFormat ni temporales UInt256 a presentación. `LoopReadModel` publica
documento, vista preparada, enabled y revisión como una unidad coherente; la UI
ya no reconstruye fronteras desde doubles.

Play conserva cualquier posición anterior a loopStart como preroll. Una posición
en o posterior a loopEnd se relocaliza a start. Content duration sigue siendo
descriptiva, GoToEnd continúa significando contentDuration y el loop activo evita
el final natural. Enable/disable continúa siendo Stopped-only y ahora su admisión
consulta el transporte reconciliado, incluido el orden de comandos pendientes.

Para un formato certificado, la longitud L es al menos el avance exacto de un
device frame. Si p<start, p+delta<end; si start<=p<end, p+delta<end+L. Por ello
`ProjectPhase::advance()` puede cruzar como máximo una frontera por device frame
y conservar su resultado booleano. Un callback puede efectuar varias vueltas
mediante sucesivos subbloques. End nunca se renderiza como interior y el wrap es
`start + ((p'-end) mod L)`.

El checkpoint conserva únicamente la obligación musical mínima pendiente del
metrónomo: intervalo de loopStart atravesado, evento aplazado y su acento. No
duplica el scheduler ni conserva voces o PCM. Un rebuild duro puede cancelar
voces, subsume seek/loopWrap pendientes para processors y restaura la obligación
musical, que se coalesce y consume exactamente una vez en el siguiente scheduling.
Stop, Seek y metronome-off sí limpian todo el estado pendiente.

El scheduler permanece global por subbloque y acotado a 8191 segmentos. El loop
puede aumentar el número de subbloques hasta la cota derivada de su mínimo de
10 ms, pero no añade búsqueda por pista ni trabajo no acotado. El benchmark de
deadline profesional sigue pendiente.

### Metronome 0.6.3

0.6.3 consolida el metrónomo existente sin introducir otro clock, beat map ni
scheduler. El documento conserva tempo y métrica; `PreparedTemporalContext`
conserva el grid analítico y las tablas; `RealtimeAudioEngine` conserva las
voces y obligaciones pendientes; `MetronomeReadModel` es únicamente un valor
de presentación `{enabled,level,temporalRevision}` construido desde una
publicación de sesión coherente.

Enabled y level son session-only, no dirty, no Undo y no persistentes. New y
Load restablecen disabled y -12 dB. El rango del nivel sigue siendo [-100,0] dB
y el cambio conserva el smoother de 5 ms. Enabled permite Play sin contenido,
pero no inicia ni detiene directamente el transporte. Sin loop, esa reproducción
usa run-until-Stop; desactivar el metrónomo silencia eventos nuevos sin detener
ni cambiar locator o política abierta de la reproducción en curso. Un Play
posterior vacío y disabled se rechaza.

Para N/D, una figura D es un beat y el compás tiene N beats. El único accent es
el primer beat del compás, calculado respecto al anchor exacto de la firma. Por
tanto 4/4 es accent+3 normales, 3/4 accent+2 y 7/8 accent+6; no existe grouping
2+2+3 ni métrica compuesta implícita. Un cambio de firma anclado a BarIndex
inicia un nuevo compás y su frontera recibe accent.

Pause conserva la posición exacta y cancela todas las voces activas mediante
`clearMetronomeVoices()`. No conserva PCM, cursor de tabla ni cola sonora. Los
tres bits de `PendingMetronomeBoundaryState` permanecen sin ampliación: una
obligación legítima todavía no emitida o un loopStart atravesado puede sobrevivir
Pause y un rebuild. Stop, Seek, metronome-off y reset limpian voces y pending.
Resume deja que el scheduler normal consuma una obligación válida una vez; sin
pending no inventa click.

La cadena master queda fijada como Master Inserts → Metronome → Master Gain →
Master Meter/Output. Los inserts Master no procesan el click; Master Gain sí lo
escala y Master Meter observa el resultado escalado. Mute/Solo de pistas y buses
no condicionan el metrónomo.

La preparación y el commit temporales siguen siendo transaccionales. Un rechazo
conserva semánticamente documento, contexto preparado publicado, revisión,
read model, enabled, level, playback y runUntilStop. La identidad del owner
puede comprobarse como detalle de la implementación actual, pero no constituye
una nueva garantía pública. El callback conserva zero allocations, locks e I/O,
cuantización causal exacta, almacenamiento fijo y cuatro voces. El recorrido de
hasta 8191 segmentos por subbloque sigue pendiente de benchmark RT profesional.

### Arrange Editing I 0.6.4

0.6.4 consolida Add/Delete Audio Track y Move Clip, ya existentes, sobre el
dominio temporal certificado de 0.6.x. No introduce comandos ni otra ruta de
mutación. Toda operación conserva la secuencia `Command -> ProjectState`
candidato -> preparación completa fuera de RT -> commit conjunto de modelo,
plan e historial.

`ProjectState::validateClip` es la frontera común para importación, creación,
movimiento, trim, restauración de historial y carga documental. Además de
layout, duración y límites de fuente, exige que `projectStart` sea una posición
soportada y que `checkedExclusiveProjectEnd(projectStart,duration)` exista y
sea también una posición soportada. El cálculo del final usa la utilidad
temporal comprobada y no suma en entero con posibilidad de overflow. Un Move
fuera del dominio devuelve `invalidPosition` antes de llamar a la preparación
del processing plan.

No cambia la semántica de arrange. Una posición válida posterior al contenido
actual amplía `contentDuration`; los intervalos adyacentes son válidos; los
solapes parciales y completos se suman antes de los inserts de pista. Move
conserva ClipId, SourceId, duration y sourceOffset. Un Move horizontal no-op no
crea historial ni plan. Move cross-track sigue siendo Stopped-only y exige
layout idéntico; Add/Delete Track siguen siendo Stopped-only. Delete conserva
el catálogo de Sources. Undo/Redo restaura valores e IDs sin retroceder los
contadores monotónicos.

El schema continúa en v3: pistas, clips, ownership, coordenadas y contadores ya
formaban parte del documento. El callback no lee `ProjectState` y no se modifica
el plan preparado ni su renderer.

## Temporal State Model V1 y Musical Time 0.6.1

Las autoridades quedan formalmente separadas:

- autoridad documental: `ProjectState::MusicalTimeMap`;
- autoridad preparada de consultas para la revisión N:
  `PreparedMusicalTimeMap`;
- coordenada musical canónica: `MusicalTickPosition`;
- vista musical estructurada: `MusicalPosition`;
- autoridad temporal pública/DSP: `ProjectFramePosition + ExactProjectPhase`;
- semántica de transporte: `TransportReducer`;
- ejecución RT: `RealtimeProjectClock`;
- publicación confirmada: `RealtimeTransportExchange`;
- proyección de aplicación: estado RT confirmado más comandos aceptados
  pendientes.

Musical Time es derivado de project time; no es otro reloj. No se cachea una
posición musical mutable en snapshots, checkpoints o sesión. Dos mapas
preparados de la misma revisión pueden tener owners distintos —aplicación y
contexto temporal—, pero compilan determinísticamente el mismo documento.

Un mapa preparado publicado está siempre certificado para consultas exactas.
La compilación distingue invalidez documental, `outOfRange`,
`conversionOverflow` y `capacityExceeded`; no existe fallback silencioso a una
ruta exclusivamente floating point. `exactProjectFrameAtTick()` conserva su
nombre por compatibilidad, pero devuelve `audio::exact::Position`: puede incluir
una componente subframe y no equivale necesariamente a un
`ProjectFramePosition` entero.

La conversión forward selecciona por tick un segmento exacto y evalúa su
`LinearMapping`. La inversa selecciona primero el segmento por anchors exactos
y busca después el mayor tick de su intervalo half-open
`[startTick,nextStartTick)` cuya frontera no supera la posición. Una posición
igual al anchor siguiente selecciona el segmento nuevo; la búsqueda anterior
nunca puede devolver el tick exclusivo. La selección inicial es O(log N) y la
búsqueda interna necesita como máximo 41 comparaciones por el dominio 2^40.

`projectFrameAt` redondea la posición racional sin convertirla a double:
floor conserva la parte entera, ceil incrementa solo con resto y nearest compara
`2*resto` con el denominador en UInt256; el empate va hacia arriba. No se promete
que tick→project frame entero→tick sea reversible cuando varios ticks comparten
el mismo frame.

Tempo y métrica consultan primero la inversa exacta. La descomposición
bar/beat/tick usa exclusivamente enteros y los cambios de métrica permanecen
anclados a `BarIndex`. El grid obtiene el primer tick mediante inversa exacta,
avanza cursores monotónicos de métrica y tempo, calcula cada anchor con el mapa
exacto y convierte a double únicamente al producir coordenadas de presentación.
Segundos, `QuarterNotePosition` y píxeles no realimentan decisiones discretas.

El dominio musical continúa limitado a ticks `[0,2^40]`; consultas temporales
posteriores a su último anchor exacto devuelven `outOfRange`, sin clamp. El
schema v3 no cambia: BPM sigue siendo el valor contractual de sus bits binary64
y su serialización se verifica bit a bit, incluidos ambos vecinos de 123 BPM.
No se añaden rampas, automation, snapping, quantize ni consultas inversas por
muestra dentro del callback.

El adaptador prepara conjuntamente el plan y contexto del nuevo dispositivo,
certifica fuentes y checkpoint contra el ClockFormat **efectivo**, y solo entonces
retira las vistas del motor bajo quiescencia. Los owners anteriores permanecen
vivos hasta completar la retirada y la instalación del conjunto nuevo. El cierre
también retira primero esas vistas, antes de destruir planes y contextos.
El bootstrap anterior a DawApplication certifica un motor vacío usando la tasa
del dispositivo como política inicial de proyecto, sin cambiarla en reaperturas.
Un rechazo de reprepare conserva owners, revisión, fase y políticas lógicas;
el dispositivo físico puede quedar en error y no renderizar. No se promete
reabrir automáticamente el dispositivo físico anterior.

El metrónomo recibe segmentos analíticos preparados (como máximo 8191 por los
límites existentes de tempo y métrica). El callback selecciona beats y muestra de
salida mediante comparaciones enteras; no enumera una rejilla floating point.
La densidad queda acotada por los límites de device rate/tempo/métrica y por los
1024 frames máximos del subbloque. La preparación, propiedad y destrucción de los
segmentos permanecen fuera de RT. Seno, envolvente y amplitud de click siguen
siendo continuos floating point porque no deciden la muestra de disparo.
Un wrap deja un indicador explícito hasta el siguiente subbloque DSP: el scheduler
incluye el intervalo desde loopStart atravesado por el overshoot. Los eventos
cruzados se ubican en su primera muestra de dispositivo y se coalescen por muestra,
sin epsilon ni alterar la fase. El recorrido de hasta 8191 segmentos sigue siendo
global por subbloque, no por pista; su deadline profesional no está benchmarkeado.

### Orden de comandos

`TransportReducer` expresa de forma portable, determinista y `noexcept` las
transiciones Play/Pause/Stop/Seek y los eventos internos de final/rebobinado.
Tanto productor como consumidor usan esa decisión; el reloj aplica sus efectos.
Los hechos de frontera del snapshot permiten distinguir locator redondeado y
frontera DSP alcanzada sin publicar la fase como una posición alternativa.

El único productor conserva un registro fijo de 14 comandos pendientes. Un
snapshot coherente, incluso parcialmente resuelto, reemplaza la base confirmada;
se eliminan tickets resueltos/generaciones canceladas y se reaplican los restantes
en orden. El replay es puro: no toca reloj, DSP, FIFO, lifecycle ni discontinuidades
reales. La FIFO tiene 7 huecos utilizables: se exige capacidad simultánea en ambas
estructuras antes de reservar. Agotar el registro añade backpressure acotado,
nunca crecimiento dinámico ni pérdida de historial.

El CAS final del gate es el punto de aceptación, antes de publicar FIFO y confirmar
el registro del productor. Un rechazo de dominio/capacidad no reserva ticket.
Si lifecycle gana después de una reserva, el CAS falla y no se publica ninguna
acción. El ticket rechazado puede aparecer en el watermark o dejar un hueco.
`AudioCommandSequence` identifica reservas/resoluciones, NO un ordinal consecutivo
de acciones aceptadas. La reserva aceptada precede al CAS release y el cierre
acquire la incluye; una reserva rechazada carece de esa garantía y puede quedar
fuera del watermark sin dejar una acción aceptada sin resolver.

Play no consulta flags requestedLoopEnabled/requestedMetronomeEnabled: se han
eliminado. Su disponibilidad proviene del contenido preparado y de snapshot más
replay. El claim debe pertenecer a la misma generación que esa base confirmada;
si hubo cierre/reapertura después de validar, se rechaza antes de reservar ticket.
Si el cierre ocurre después del claim, el CAS final falla. Si ocurre después de
aceptar, la acción se cancela normalmente. Ningún store auxiliar tardío puede
reabrir disponibilidad cancelada.

El snapshot RT mantiene `lastProcessedCommandSequence` como watermark confirmado.
La vista de aplicación distingue `projectedThroughTicket`: no afirma ejecución
RT de los pendientes. La proyección es provisional frente a eventos todavía no
publicados. El corte FIFO del callback determina qué comandos aplica ese bloque;
su final natural puede preceder a comandos aceptados para el siguiente callback.
Al conocer ese final se reconcilia la proyección, no se descarta el snapshot.
No se promete frescura instantánea entre hilos.

Stop no operativo solo es `alreadySatisfied` ante Stopped @ 0 confirmado en la
generación correspondiente. Una ventana de cierre con snapshot antiguo, Paused
preservado o Stopped @ X requieren rechazo: no existe consumidor que garantice
la acción. Stop operativo aceptado es `scheduled`; no se añade una cola diferida.

Stop permanece como Stop en la FIFO. Desde Playing/Paused, el primero conserva
posición y pasa a Stopped; otro Stop aceptado contra esa proyección rebobina a
cero aunque ambos lleguen antes del mismo callback. En Paused, varios Seek se
reducen en orden y el último destino gana. Seek se admite en Stopped/Paused y se
rechaza provisionalmente en Playing. Play en o después de `contentDuration`
reinicia a cero por compatibilidad 0.5.x cuando la frontera DSP realmente está
alcanzada, no simplemente porque el locator se haya redondeado a ella. No es una política definitiva para
grabación ni proyectos vacíos.

### Discontinuidad Seek

Un Seek consumido fija `TemporalDiscontinuity::seek` pero no llama a
`IAudioProcessor::reset()`. Si el transporte está Paused, la marca permanece
pendiente porque no hay procesamiento DSP. El primer subbloque realmente
procesado desde la nueva posición recibe la marca; los siguientes reciben
`continuous`. Esto informa a cada procesador sin imponer una política universal
de reset. Si un rebuild ocurre antes del DSP, su `hardDiscontinuity` subsume a
Seek y se conservan los resets legítimos. Stop, lifecycle y loop conservan sus
políticas anteriores; no se introduce una colección de causas ni otra API de inserts.

## Evolución hasta 0.6.4

1. **Completado:** integrar una ventana JUCE vacía y un adaptador de dispositivo,
   manteniendo los tests del núcleo independientes de JUCE.
2. **Completado:** añadir `LoadAudioFile` y decodificar/validar WAV fuera de RT.
3. **Completado:** preparar y publicar un recurso con una pista y un clip.
4. **Completado:** implementar `Play` y `Stop` sobre una cola SPSC acotada; al detener, limpiar
   la salida y confirmar el estado al hilo de aplicación. La inserción en la
   cola devuelve éxito o fallo: nunca se pierde silenciosamente un comando.
5. **Completado:** introducir tiempo de proyecto explícito, posición observable
   y transición determinista al final natural.
6. **Completado:** reproducir exactamente dos pistas desde un reloj maestro y
   sumarlas a estéreo con margen fijo.
7. **Completado en 0.0.7:** endurecer intercambio RT, lifecycle, duración,
   validación y extraer el `processBlock` portable.
8. **Completado en 0.0.8:** exigir un consumidor real para Play, cerrar carreras
   de lifecycle y publicar cada WAV como una transacción motor/modelo.
9. **Completado en 0.0.9:** linearizar enqueue frente al cierre de generación y
   demostrar la destrucción real de recursos después de que RT quede quiescente.
   Los smoke tests en otros backends y plataformas continúan siendo trabajo
   futuro.
10. **Completado en 0.1.0:** sustituir la topología fija de dos pistas por una
    colección preparada N-track con identidad estable, render por pista y
    acumulación desde un único reloj.
11. **Completado en 0.1.1:** sustituir la atenuación provisional por el Mixer
    Core portable y una vía ligera, acotada y RT-safe de parámetros.
12. **Completado en 0.1.2:** añadir smoothing temporalmente estable y peak
    metering portable con intercambio RT hacia aplicación.
13. **Completado en 0.2.0:** compilar routing editable a un plan portable con
    buses, buffers y orden preparados fuera de RT.
14. **Completado en 0.2.1:** procesar gain, balance, mute y solo de buses con
    smoothing y resolución explícita de caminos audibles.
15. **Completado en 0.2.2:** admitir Bus→Bus como DAG validado, preparar un
    orden topológico determinista y resolver Solo a través de rutas encadenadas.
16. **Completado en 0.2.3:** distribuir taps pre/post de una pista hacia buses
    auxiliares, con identidad estable, parámetros RT y Solo por aristas.
17. **Completado en 0.2.4:** activar Bus Sends pre/post, incorporarlos al DAG y
    separar contenido upstream de permiso Main para Solo wet-only.
18. **Completado en 0.3.0:** introducir contrato DSP portable, cadenas de
    inserts Track/Bus/Master, ejecución por subbloques, parámetros y bypass
    generation-safe, y metadata completa de latencia sin implementar PDC.
19. **Completado en 0.4.0:** separar fuentes, clips y pistas, compartir PCM por
    `SourceId`, compilar índices temporales y sumar solapes antes de inserts.
20. **Completado en 0.4.1:** editar clips de forma no destructiva mediante
    Move/Duplicate/Split/Trim/Delete, consultas por ID y rebuild transaccional
    sin decodificar de nuevo las fuentes.

21. **Completado en 0.4.2:** Undo/Redo transaccional de las seis operaciones de
    clips, historial acotado, IDs exactos, barreras y tokens lógicos de estado.

22. **Completado en 0.4.3:** persistencia versionada, medios verificados,
    guardado atómico, sesión/dirty y adopción transaccional de proyecto aislado.

23. **Completado en 0.5.0:** timeline JUCE sobre un snapshot portable, geometría
    derivada del reloj lógico, edición por gestos/comandos, playhead, zoom/scroll
    y refresh por revisión sin introducir estado musical duplicado.

24. **Completado en 0.5.1:** Pause, Seek por frame, doble Stop, navegación
    ruler/teclado, displays derivados y Split manual desde el playhead real.
25. **Completado en 0.5.2:** mapas musicales portables/preparados, comandos
    undoables, display/ruler, schema2 y migración v1.
26. **Completado en 0.5.3:** loop PPQ persistente, segmentación sample-accurate
    sin deriva, metrónomo RT con acento y schema3/migración v2.

27. **Completado en 0.5.4:** ciclo Add/Delete de pistas con historial de
    submodelo, selección de destino, importación dirigida y Move 2D atómico.

28. **Completado en 0.5.5:** caché derivada multirresolución por SourceId,
    preparación desde PCM existente, mapping project/source, render visible y
    reconstrucción aislada/transaccional tras Load.

29. **Completado en 0.5.6:** hardening combinado de transporte, loop, edición,
    historial, persistencia, fronteras de bloque y escala sobre processBlock
    real, sin añadir funciones ni cambiar la matriz de comportamiento.

30. **Completado en 0.6.0:** autoridad entera de project frame, dominio navegable
    independiente del contenido, proyección linealizada de transporte y
    discontinuidad Seek explícita sin reset universal.

31. **Completado en 0.6.1:** Musical Time bidireccional exacto, inverse y rounding
    racionales, consultas de tempo/métrica por frontera certificada y grid con
    double exclusivamente al final de la presentación.

32. **Completado en 0.6.2:** contrato de loop half-open exacto, admisión
    reconciliada y obligación musical preservada durante rebuild y lifecycle.

33. **Completado en 0.6.3:** metrónomo con beat N/D, accent de inicio de compás,
    Pause sin PCM antiguo y read model coherente de sesión.

34. **Completado en 0.6.4:** consolidación de Arrange Editing I, con Add/Delete
    Track y Move horizontal/cross-track validados contra el inicio y final
    exclusivo del dominio temporal certificado antes de preparar DSP.

Cada paso debe compilar, pasar pruebas y poder validarse aisladamente antes del
siguiente.

## Estrategia de pruebas

- **Unitarias**: comandos, transiciones del transporte, pistas, clips y límites
  de timeline; sin JUCE ni dispositivo.
- **Integración**: `DawApplication` con un motor falso para verificar que la UI
  solo provoca solicitudes mediante comandos, incluida la transacción y la
  destrucción real de recursos sustituidos.
- **Audio offline**: cursores deterministas, diferencias de sample rate y casos de
  inicio/fin de clip, sample rates y tamaños de bloque.
- **RT**: instrumentación en builds de desarrollo para detectar allocations y
  locks dentro del callback, carreras Play/lifecycle y ThreadSanitizer fuera del
  callback duro.
- **Sistema**: smoke tests manuales por backend/plataforma para abrir, cargar,
  reproducir y detener; no hacer depender CI de hardware de audio.
