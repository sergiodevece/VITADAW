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
- `transport`: estado lógico de reproducción y posición.
- `tracks`: colección variable de pistas, cada una con identidad estable y un
  único clip opcional en este incremento.
- `clips`: referencia a un archivo y región colocada en el timeline.
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

## Reproducción mínima 0.0.4 — First Sound

`LoadAudioFile` recorre `ICommandDispatcher` y `DawApplication` antes de llamar
al puerto neutral `IAudioEngineControl::prepareWav`. El adaptador valida la
extensión y usa directamente `juce::WavAudioFormat`; no registra MP3, AIFF,
FLAC ni otros formatos. Decodifica el archivo completo a un `AudioBuffer<float>`
fuera del callback. Solo después se invoca `commitPreparedWav` con un recurso
opaco y una acción portable de commit ya preparada.

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
proyecto. Las posiciones observables se redondean al frame más próximo, mientras
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

## Motor N-track 0.1.0

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
reloj conserva suma compensada si dispositivo y proyecto difieren de sample
rate. La duración global es el máximo `clip start + clip duration` de todas las
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
    -> StereoAccumulator
    -> master linear gain
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
`PreparedProject` ni `ProjectState` cambian. Cada `PreparedTrackView` conserva
también el estado vigente para que una posterior reconstrucción estructural lo
publique sin volver a valores por defecto. La carga de un WAV para una pista
vacía recibe su estado de mezcla actual desde el modelo. Cada cambio publica
también `anySolo`, calculado sobre todas las pistas del proyecto: una pista vacía
en solo silencia correctamente las pistas cargadas que no estén en solo, sin
crear un recurso RT ficticio.

La suma interna usa `float` y puede superar `[-1, 1]`. No se aplica clamp ni
limitador; el recorte depende del backend/hardware final. No hay smoothing en
este incremento: cambios abruptos de gain o pan son coherentes al límite de
bloque, pero pueden causar discontinuidades audibles. Smoothing, buses, sends,
inserts, plugins, automatización, grabación y routing configurable quedan fuera
de 0.1.1.

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

## Evolución hasta 0.1.1

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
