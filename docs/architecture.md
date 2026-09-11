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
- `transport`: estado lógico de reproducción y posición.
- `tracks`: dos pistas fijas, cada una con un único clip opcional.
- `clips`: referencia a un archivo y región colocada en el timeline.
- `timeline`: unidades temporales fuertes y conversiones entre archivo,
  proyecto, dispositivo y segundos.
- `ui`: frontera de UI; se implementará con JUCE sin acceder a `audio`.
- `platform/juce`: composición nativa, ventana y adaptador de dispositivo; es
  la única capa que depende de JUCE.

El modelo editable pertenece al hilo de aplicación/UI. El callback de audio no
lee esas estructuras mutables. Cada WAV se decodifica completamente fuera de RT;
el adaptador detiene el callback antes de sustituir el recurso preparado de una
pista y conserva intacto el recurso de la otra.

`RealtimeAudioEngine` es la frontera real de procesamiento portable. Posee el
reloj maestro, la cola SPSC, el render fijo de dos pistas, la suma estéreo y el
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

La cola conserva capacidad fija de ocho posiciones y una generación de
lifecycle. Toda transición hacia `initializing`, `stopped`, `error` o
`unavailable` deja primero de aceptar comandos, incrementa la generación,
marca como resuelta toda secuencia ya emitida, rebobina y publica un snapshot
`Stopped` en cero. Los comandos antiguos que aún permanezcan en el anillo se
consumen sin ejecutarse al volver el dispositivo. La comprobación final de
estado y generación al encolar garantiza que una carrera `Play`/lifecycle acaba
o bien rechazada, o bien aceptada y cubierta por la marca de cancelación.

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

## Reproducción de dos pistas 0.0.6

El proyecto crea exactamente dos `AudioTrack`, cada una con un único
`optional<AudioClip>`. `LoadAudioFile` incluye un `AudioTrackSlot` portable y la
misma posición se usa para seleccionar el recurso del adaptador. No se admite un
tercer slot ni múltiples clips por pista.

`RealtimeProjectClock` es el único estado temporal que avanza en el motor.
En cada frame de dispositivo produce una posición precisa en frames de proyecto:

```text
device frame
    -> RealtimeProjectClock (project frames)
        -> track 1 source position = project position × rate1 / project rate
        -> track 2 source position = project position × rate2 / project rate
```

No existe un acumulador o cursor por pista. Incluso después de una secuencia
larga, ambas posiciones se recalculan desde el mismo número, por lo que no pueden
derivar entre sí. El reloj usa suma compensada cuando proyecto y dispositivo no
comparten sample rate. La duración global es el máximo de las dos duraciones en
frames de proyecto. `RealtimeProjectContext` es la única autoridad RT para
sample rate y duración global; cada recurso conserva solo sample rate, canales,
frames y punteros fuente.

`TwoTrackMixer` realiza lectura lineal provisional en la posición derivada. Una
pista que ya terminó devuelve cero; la otra continúa hasta su propio final. La
mezcla estéreo es `0.5 × track1 + 0.5 × track2`. La ganancia fija de `0.5`
(-6,02 dB) por pista evita superar la unidad al sumar dos fuentes normalizadas.
No hay clipping, faders, pan, mute, solo, buses ni estado de mixer.

Una carga válida se construye y decodifica por completo antes de desconectar
brevemente el callback. `DawApplication` prepara antes el `AudioClip`, su
`filesystem::path` y el mensaje de resultado; cualquier excepción conserva
intacto el estado anterior. Con el render ya quiescente, el adaptador intercambia
el recurso del slot, configura las vistas RT y ejecuta la acción de modelo
`noexcept`. Ese es el único punto de commit. Solo entonces vuelve a registrar el
callback. El recurso anterior queda temporalmente en el objeto preparado y se
destruye en el hilo de aplicación una vez que RT ya no lo referencia.

Rutas, apertura, creación del reader, decodificación, buffers y metadatos están
dentro de la frontera de excepciones de preparación. `std::bad_alloc`,
`std::exception` y excepciones desconocidas se convierten en resultados de error
fuera de RT; `DawApplication` contiene además la misma frontera defensiva. Una
carga fallida no publica nada: el clip y recurso válidos de ambas pistas
permanecen intactos.
Solo se aceptan uno o dos canales. Se validan sample rates finitos y positivos,
longitud, productos de tamaño, slots, muestras float finitas y un presupuesto
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

## Evolución hasta 0.1

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
   de lifecycle y publicar cada WAV como una transacción motor/modelo. La
   instrumentación permanente y los smoke tests multiplataforma continúan
   siendo trabajo futuro.

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
