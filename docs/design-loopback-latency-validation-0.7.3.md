# VitaDAW 0.7.3C - Loopback & Physical Latency Validation

## Propósito y alcance

0.7.3C define una operación diagnóstica reproducible para medir la latencia física
de ida y vuelta de una ruta concreta:

```text
VitaDAW -> Output N -> cable/ruta física -> Input M -> VitaDAW
```

La autoridad de la medida son frames del dispositivo observados en el callback de
audio. Los milisegundos son únicamente una presentación derivada. La prueba no
calibra automáticamente, no modifica audio grabado, no desplaza clips y no cambia
el Recording Offset.

La salida de esta foundation es un resultado efímero que compara:

- latencia de input reportada por el backend;
- latencia de output reportada por el backend;
- round trip reportado/estimado, cuando ambos datos existen;
- round trip físico medido;
- residual entre medición y estimación;
- dispersión entre varias mediciones.

Quedan fuera la aplicación automática de offsets, una base de datos de
calibraciones, la calibración simultánea de varias rutas, el descubrimiento de la
topología interna de Aggregate Devices, direct-monitor control, plugin delay
compensation, MIDI, inserts hardware, red, cambios automáticos de buffer y
metadata de calibración en `ProjectState`.

## Arquitectura existente auditada

| Responsabilidad actual | Clase/archivo | Hilo o frontera | Consecuencia para 0.7.3C |
| --- | --- | --- | --- |
| Entrada serializada de acciones | `commands::CommandDispatcher` y `application::DawApplication::handle()` | JUCE message/application thread | La admisión de la prueba debe ser un comando efímero y debe comprobar Playback, Recording y Monitoring antes de tocar el device. |
| Dueño del dispositivo | `platform::juce_adapter::JuceAudioDeviceAdapter` | Control para configuración; callback JUCE para audio | Es la única capa que puede resolver canales físicos, consultar latencias y preparar/restaurar el device. |
| Callback productivo | `JuceAudioDeviceAdapter::audioDeviceIOCallbackWithContext()` | RT | Adapta los arrays JUCE y llama a `RealtimeAudioEngine::processBlock()`. La sonda debe ser un modo diagnóstico exclusivo, no un nodo de Playback. |
| Playback actual | `RealtimeAudioEngine::processBlock()` y `processSubBlock()` | RT | Renderiza el proyecto según `RealtimeProjectClock`; no debe usarse para generar el estímulo porque incorporaría transporte, routing, mixer y posibles procesadores. |
| Reloj de proyecto | `RealtimeProjectClock` | RT, preparado fuera de RT | Su `ProjectFrame` puede tener conversión de rate, seek y loop. No es la autoridad adecuada para una medida física raw. |
| Capture raw | `RealtimeAudioEngine::processBlock()` llama a `RealtimeCapture::capture(input)` antes de staging, limpieza de output, Playback y Monitoring | RT | Confirma dónde está disponible el input raw y el orden alias-safe. La prueba no debe reutilizar la sesión musical ni su writer. |
| Ring de Recording | `RealtimeCapture` | RT producer / control consumer | Es una referencia válida para un buffer acotado y preasignado, pero está ligado al lifecycle de Recording y debe permanecer independiente. |
| WAV, media y recovery | `JuceAudioDeviceAdapter::{prepareRecording,serviceRecording,finalizeRecording,...}` | Control/filesystem fuera de RT | Loopback no necesita WAV, marker, publicación ni `ProjectState`; reutilizar esta ruta introduciría circularidad y riesgo innecesario. |
| Monitoring | staging y mezcla en `RealtimeAudioEngine::processBlock()`; demanda en `JuceAudioDeviceAdapter` | Preparación control-side, mezcla RT | Debe estar OFF y la prueba nunca lo activa. Así se evita feedback software y una segunda copia de la señal. |
| Lifecycle de device | `pollDeviceLifecycle()`, `detachAudioCallback()`, `reprepareForCurrentDevice()`, `attachAudioCallback()` | Message/control thread; eventos de callback reducidos a atomics | La preparación de la ruta y su rollback deben seguir la transacción certificada de 0.7.3A. |
| Estado físico | `AudioDeviceStateModel` y `DeviceLatencyReadModel` | Control/UI | Ya exponen rate, buffer y latencias reportadas. El resultado de loopback debe congelar una copia, no mantener referencias al modelo vivo. |
| Placement | `computeEffectiveRecordingCompensation()` y `computeRecordingPlacement()` | Preflight/finalización no-RT | Usa input latency para placement documental. No participa en emisión, captura ni análisis de loopback. |
| Actualización periódica | `VitaDawJuceApplication::timerCallback()` llama a lifecycle y sincronización | JUCE message thread, 30 Hz | Puede prestar el punto de servicio que observa el terminal RT y ejecuta el análisis corto fuera de RT. |
| UI de latencia | `MainWindow.cpp`, `AudioStatusComponent::{updateDeviceLatencyControls,updateLoopbackControls}()` | JUCE message thread | Presenta el diagnóstico efímero separado de Recording Offset y Effective Compensation. |

### Playback y Capture actuales

El orden relevante de `RealtimeAudioEngine::processBlock()` es:

1. consumir comandos y parámetros bounded;
2. validar el contexto preparado;
3. entregar input raw a `RealtimeCapture`;
4. medir input y, si procede, copiarlo al staging de Monitoring;
5. limpiar output;
6. producir Playback;
7. mezclar Monitoring;
8. publicar meters y transport.

Este orden protege Capture frente a aliasing input/output, pero una prueba física no
debe insertar clicks en el render normal. Se implementa una sonda separada y exclusiva
en el adaptador. Mientras la sonda esté activa, el callback entrega los bloques a la
sonda y no a `RealtimeAudioEngine`; cuando está inactiva, el camino normal no cambia
más allá de una comprobación bounded del modo diagnóstico.

## Arquitectura implementada

### Componentes

- `LoopbackLatency.h/.cpp`: unidades, configuración, estados, resultado y análisis sin JUCE.
- `RealtimeLoopbackProbe`: productor RT acotado, sin strings ni memoria
  dinámica durante el callback.
- `LoopbackLatencyAnalysis`: correlación directa y estadística fuera de RT.
- `JuceAudioDeviceAdapter`: preflight físico, checkpoint/rollback, snapshot de
  latencias y selección exclusiva entre motor normal y sonda.
- `IAudioEngineControl`: puerto de control para preparar, iniciar, cancelar,
  servir y observar la operación diagnóstica.
- `DawApplication`: gate de lifecycle y read model efímero.
- `Command System`: `StartLoopbackLatencyTest` y `CancelLoopbackLatencyTest`; nunca
  comandos persistentes ni operaciones de historial.

### Sesión exclusiva

La prueba sólo se admite cuando se cumplen simultáneamente:

```text
projected Playback == Stopped
Recording no está prepared/capturing/finalizing
Input Monitoring efectivo y solicitado == OFF
device operativo y configuración certificada
no existe otra sesión loopback
input y output elegidos son válidos
```

Durante la sesión se rechazan Record, Play/Pause/Seek, Enable/Toggle Monitoring,
cambios de buffer y cualquier operación que obligue a reconfigurar el motor. Dado
que la prueba dura pocos segundos, la política más segura es bloquear también
cambios documentales hasta terminalización o cancelación. Las lecturas de UI siguen
permitidas.

La sonda no comparte `RealtimeCapture`, writer, recovery marker, media paths ni
session id de una grabación musical. No crea WAV. El output normal permanece en
silencio y sólo el canal seleccionado recibe el estímulo.

### Ruta física mínima

Una sesión representa un par mono explícito:

```text
logical device + output channel N + input channel M
```

El selector muestra nombres e índices que JUCE expone para el device
lógico actual. La configuración conserva tanto el índice físico solicitado como el
ordinal que ese canal ocupa en los arrays compactos del callback, derivado de las
máscaras activas certificadas. Nunca se asume que `output[0]` significa siempre
Output 1 si la máscara física no empieza en cero.

El preflight de control guarda un checkpoint completo equivalente al de 0.7.3A:
tipo/contexto del device, nombres de input/output, rate, buffer, máscaras activas,
callback registrado y checkpoint temporal. Con callback quiescente configura
exclusivamente el par requerido, o certifica que ya está activo, prepara buffers y
restaura el callback. La prueba usa el rate y buffer efectivos posteriores, no los
valores solicitados.

Al terminar o cancelar se restaura el checkpoint anterior y se vuelve a certificar
Core. Si la restauración exacta falla se entra en el estado seguro de device error:
Monitoring permanece OFF, la configuración/read model se invalidan y nunca se
declara éxito de rollback. El resultado de una prueba ya completada conserva su
snapshot histórico; un cambio ocurrido mientras se medía invalida la sesión.

Esta foundation mide un único par. Los tipos deben reservar identidad de canal y
device de forma que una extensión futura pueda repetir el mismo protocolo para más
pares, sin convertir ahora la UI ni el callback en una matriz multicanal.

## Dominio de medida

### Contador de frames del callback

La autoridad temporal será un contador `uint64_t` monotónico y relativo a la sesión
diagnóstica. En el primer callback aceptado por la sonda vale cero. Para un bloque de
`numFrames`, cada posición comparte el mismo dominio full-duplex:

```text
sessionDeviceFrame = callbackBaseFrame + sampleOffsetWithinCallback
nextCallbackBaseFrame = callbackBaseFrame + numFrames
```

El contador no es `ProjectFrame`, no sigue el transporte y no se convierte mediante
el sample rate del proyecto. Emisión y captura ocurren en el mismo dominio del
device lógico y al mismo rate certificado.

Un overflow del contador, capacidad insuficiente o bloque mayor que el máximo
preparado terminaliza la prueba como fallo y silencia output. No se trunca una
medición ni se reasigna memoria.

### Referencia de emisión

Cada trial tiene un `emittedStartDeviceFrame` conocido antes del callback. La sonda
espera un pre-roll de silencio de:

```text
max(2 * confirmedBufferFrames, 2 * 1023 frames)
```

La misma guarda se deja entre trials y después del último estímulo para que una
respuesta transitoria no contamine el siguiente. Sirve además para observar ruido
de fondo y garantizar que el modo diagnóstico está armado. Al
alcanzar el frame programado copia el template preasignado al canal de output
seleccionado y registra el índice exacto de su primera muestra. Todos los demás
outputs se mantienen en cero.

No se usa timestamp de UI, hora de pared, `AudioIODeviceCallbackContext`, posición
del transporte ni momento de despacho del comando.

### Referencia de retorno

Antes de escribir o limpiar output, la sonda copia el input raw del canal elegido al
buffer mono preasignado y conserva su índice inicial de sesión. Así mantiene la
disciplina alias-safe ya demostrada por Recording.

El análisis obtiene `returnedStartDeviceFrame` sumando el índice de la mejor
correlación al frame base del buffer capturado. No usa `AudioClip::projectStart`,
Recording Placement Compensation, el Recording Offset ni metadata WAV.

## Señal de prueba

### Elección

El primer estímulo de siete impulsos permitió validar lifecycle, routing y rollback,
pero el primer smoke físico con una Volt 176 devolvió señal sin clipping y SNR útil
con sólo `0.15–0.19` de correlación: la reconstrucción DAC, el filtrado analógico y
el ADC dispersan un impulso de una muestra. Por ello el estímulo físico implementado
es una secuencia máxima pseudoaleatoria bipolar, o MLS, de 1023 muestras. Se genera
fuera de RT con un LFSR de 10 bits y el polinomio primitivo
`x^10 + x^3 + 1`, partiendo del estado all-ones. Recorre los `2^10 - 1` estados no
nulos antes de repetirse.

El template se conserva en un buffer inmutable. Sus propiedades son:

- patrón temporal único y reproducible;
- ganancia de procesamiento de una secuencia densa de 1023 muestras;
- autocorrelación impulsiva con sidelobes pequeños;
- pico acotado y conocido;
- tolerancia a inversión global de polaridad mediante correlación absoluta;
- detección por correlación directa en tiempo, sin FFT;
- posición de inicio inequívoca y robustez ante una respuesta LTI corta.

Cada muestra vale `+0.0630957344` o `-0.0630957344`. La secuencia se construye una
vez durante `prepare()`; el callback no ejecuta PRNG, normalización ni análisis.

### Nivel

El pico permanece en `-24 dBFS` (`0.0631` lineal), con al menos
24 dB de headroom digital. No se usa 0 dBFS ni se ajusta automáticamente la ganancia
del hardware. La foundation no ofrece un barrido de amplitud.

La ejecución requiere acción explícita del usuario. La UI debe indicar que se use
una conexión line-level apropiada, que se bajen o desconecten altavoces/auriculares
y que se desactive el direct monitoring hardware. La prueba nunca activa Input
Monitoring.

## Captura y análisis offline

### Buffer RT

El preflight calcula y reserva toda la capacidad antes de publicar la sesión. Para
cinco trials se propone un pre-roll, ventanas de búsqueda de hasta un segundo y
silencio suficiente entre estímulos para que las ventanas no se solapen. La
capacidad se valida contra el rate efectivo y un presupuesto explícito.

El callback sólo puede:

- leer el estado prepublicado;
- copiar un canal de input a memoria existente;
- limpiar output;
- copiar el fragmento de template correspondiente al output elegido;
- incrementar contadores;
- observar clipping con comparaciones bounded;
- publicar estados/flags atomically.

Al publicar `complete`, el RT producer deja definitivamente de escribir. El
consumer de control adquiere ese estado antes de leer el buffer. Reset o reutilización
sólo ocurre después del análisis.

### Detección

`LoopbackLatencyAnalysis` ejecuta correlación directa fuera de RT. Para cada lag
entero `k` calcula:

```text
c[k]   = sum(n=0..1022) captured[k+n] * mls[n]
rho[k] = abs(c[k]) / sqrt(energy(captured[k..k+1022]) * energy(mls))
```

El lag candidato se elige por el máximo `abs(c[k])`; `rho` es la similitud
normalizada publicada. Se conserva el signo de `c[k]` como diagnóstico de polaridad.
Para una ruta LTI, correlacionar `h * MLS` contra la MLS aproxima la respuesta
impulsional `h`, en vez de exigir que el retorno reproduzca muestra a muestra un
impulso digital ideal. No se necesita FFT para 1023 muestras y cinco trials.

La búsqueda física tiene una capacidad máxima de un segundo de device frames. Si el
backend ofrece una suma input+output, se usa sólo para acotar una ventana amplia de
`±max(4096 frames, 0.25 s)`, siempre limitada por esa capacidad; no fija ni desplaza
el resultado. Sin suma reportada se busca el segundo completo.

Para aceptar un retorno deben cumplirse conjuntamente:

- similitud normalizada mínima del pico matched;
- relación señal/ruido suficiente respecto al pre-roll;
- pico no saturado;
- segundo candidato fuera de la zona del pico claramente inferior al primero;
- índice dentro de la ventana prevista.

Los gates son `rho >= 0.08`, segundo pico matched menor que `0.70` del mejor, SNR
mínimo `6 dB`, clipping desde `0.999` y señal absoluta mínima `1e-6`. No es una
relajación aislada del antiguo `0.80`: cambia el estímulo, la semántica matched y se
exige simultáneamente prominencia frente a sidelobes, SNR y ausencia de clipping.
Los valores están cubiertos por filtros FIR, ringing, delays fraccionales, ruido y
falsos positivos sintéticos; no están ajustados a una interfaz concreta.

El segundo pico se busca excluyendo `±64` frames alrededor del máximo. Esa vecindad
pertenece a la misma respuesta física y cubre dispersión corta y un pico repartido
entre dos lags por fractional delay. Fuera de ella, el ratio publicado es
`secondMatchedPeak / bestMatchedPeak`; dos retornos separados y equivalentes son
ambiguos. La autoridad sigue siendo el lag entero con mayor matched peak; no se
aplica estimación sub-frame a Recording Placement.

El noise floor se estima sólo en la guarda anterior al primer estímulo. En el lag
ganador se resta esa potencia de la potencia total de las 1023 muestras y se publica
`10*log10(signalPower/noisePower)`. Así el template/respuesta no contamina el ruido
ni se penaliza dos veces una forma filtrada. El resultado conserva score, segundo
score, ratio, SNR, polaridad, peak, delay y clipping para que un fallo sea observable.

El coste worst-case sin latencia reportada es
`5 * sampleRate * 1023` multiply-adds; a 48 kHz son aproximadamente 245 millones,
fuera de RT. Con la ventana amplia guiada por el backend suele ser menor. Captura,
guards y búsqueda permanecen acotadas por el presupuesto de 4 Mi frames.

### Múltiples trials

Se ejecutan cinco mediciones cortas. Una sola no caracteriza jitter; tres ya
permiten mediana, pero cinco toleran hasta dos outliers con un coste de pocos
segundos y memoria mono acotada.

La mediana de los trials válidos es la medida autoritativa. También se publican
mínimo, máximo y:

```text
jitterFrames = maximumMeasuredFrames - minimumMeasuredFrames
```

La sesión necesita al menos tres trials válidos de cinco. Con menos, falla; no
promedia silenciosamente datos ambiguos. Los trials inválidos y su motivo siguen
siendo observables.

## Semántica y fórmulas

Todas las siguientes magnitudes usan frames del device lógico. La conversión a ms
se hace únicamente para UI:

```text
milliseconds = frames / certifiedDeviceSampleRateHz * 1000
```

### Reported Input Latency

`juce::AudioIODevice::getInputLatencyInSamples()` consultado fuera de RT después de
activar y certificar el input elegido. Es opcional; negativo significa desconocido.
No es una medición física realizada por VitaDAW.

### Reported Output Latency

`juce::AudioIODevice::getOutputLatencyInSamples()` consultado bajo el mismo snapshot.
También es opcional y dependiente del backend.

### Reported/Estimated Round Trip

Sólo existe cuando input y output reportados son conocidos y la suma cabe en la
unidad elegida:

```text
reportedRoundTripFrames = reportedInputLatencyFrames
                        + reportedOutputLatencyFrames
```

Se etiqueta `Reported Sum` o `Estimated`, nunca `Measured`. No se añade el buffer
por separado porque el backend puede haberlo incluido ya total o parcialmente.

### Measured Physical Round Trip

Para cada trial aceptado:

```text
measuredTrialFrames = returnedStartDeviceFrame
                    - emittedStartDeviceFrame
```

La medida final es:

```text
measuredRoundTripFrames = median(measuredTrialFrames)
```

La resta se valida antes de ejecutarse. Un retorno anterior a la emisión, fuera de
capacidad o no representable invalida el trial.

La medida incluye la ruta efectiva observable desde el callback: cola/output del
backend, DAC, ruta/cable externo, ADC, cola/input y cualquier procesamiento físico
o lógico interpuesto. No separa por sí sola esos componentes.

### Residual

Cuando existe `reportedRoundTripFrames`:

```text
residualFrames = measuredRoundTripFrames - reportedRoundTripFrames
```

Es signed. Positivo significa que la ruta física tarda más que la suma reportada;
negativo, que tarda menos. Si falta una latencia reportada no se inventa cero y el
residual queda `Unknown`.

### Relación con Recording Placement Compensation

0.7.3B coloca una toma según:

```text
clipStart = capturedProjectStart
          - reportedInputLatencyProjectFrames
          + manualRecordingOffsetProjectFrames
```

Loopback mide input más output en device frames. Por tanto no es válido afirmar, de
forma universal, que:

```text
physicalInputLatency = measuredRoundTrip - reportedOutputLatency
```

Los reportes del backend pueden tener fronteras diferentes, el output puede ocultar
latencia no reportada y Aggregate Devices pueden añadir resampling/drift correction.
La métrica raw y el residual sí son válidos; la descomposición unilateral no lo es.

Por decisión de producto, 0.7.3C no calcula ni presenta un `Suggested Recording
Offset`: el residual conjunto no se atribuye a la rama de input. La prueba no toca
el offset manual ni la compensación efectiva de 0.7.3B y nunca autoaplica valores.

## Aggregate Devices en macOS

JUCE presenta un Aggregate Device como un `AudioIODevice` lógico: nombre/tipo,
sample rate, buffer, máscaras de canales y latencias que CoreAudio/JUCE reporten para
ese objeto. VitaDAW no recibe mediante esta interfaz una topología física universal
y fiable que permita atribuir cada frame a un subdispositivo.

Input y output elegidos pueden pertenecer a hardware distinto. El clock source del
agregado, drift correction o sample-rate conversion entre subdispositivos puede
introducir group delay, jitter o una latencia distinta a la suma reportada. La prueba
mide la ruta lógica/física concreta en ese momento; no descubre ni corrige su
topología.

El snapshot conserva el nombre/tipo lógico y los canales elegidos. Dos resultados
con rutas, rate, buffer, clock source o drift correction diferentes no son
intercambiables, aunque el nombre visible del agregado coincida.

## Snapshot de configuración

Cada sesión congela, por valor y sin punteros JUCE:

- identificador/generación de configuración certificada;
- tipo y nombre del device lógico;
- nombres de device input/output cuando JUCE los diferencia;
- sample rate efectivo;
- buffer efectivo;
- índice y nombre del input físico;
- índice y nombre del output físico;
- máscaras activas certificadas y ordinales de callback;
- número de canales de prueba, uno en 0.7.3C;
- reported input/output latency;
- reported round trip opcional;
- template id, amplitud, trials y capacidad/timeout;
- estado de Monitoring, Recording y Playback validado al inicio.

El adaptador incrementa una generación de configuración cuando certifica un device,
rate, buffer o layout distinto. Si la generación o cualquiera de los hechos físicos
cambia mientras la sesión está `armed` o `running`, se silencia output y el resultado
queda `invalidated`. No se mezclan trials de dos configuraciones.

Un resultado completado queda asociado a su snapshot. Un cambio posterior puede
marcar `configurationStillCurrent = false`, pero no reescribe la medición histórica.
No se reutiliza como calibración automática.

## Modelo de resultado efímero

Un `LoopbackLatencyReadModel` de control/UI puede contener:

| Campo | Semántica |
| --- | --- |
| `sessionId` | Identidad efímera para no confundir terminales antiguos. |
| `status` | `idle`, `preparing`, `armed`, `running`, `analyzing`, `complete`, `failed`, `cancelled` o `invalidated`. |
| `configuration` | Snapshot descrito arriba. |
| `requestedTrials` / `validTrials` | Normalmente 5 y 0..5. |
| `trials` | Resultado bounded por trial: emisión, retorno, medida, score, SNR, peak, polarity y fallo. |
| `reportedInputFrames` / `reportedOutputFrames` | Opcionales y etiquetados como reportados. |
| `reportedRoundTripFrames` | Suma opcional; no medición. |
| `measuredRoundTripFrames` | Mediana opcional de trials válidos. |
| `minimumFrames` / `maximumFrames` / `jitterFrames` | Dispersión opcional. |
| `residualFrames` | Signed y opcional. |
| `configurationStillCurrent` | Comparación control-side con la configuración actual. |
| `failure` / `diagnostic` | Código estructurado y texto generado sólo fuera de RT. |

Los arrays RT son de capacidad fija. El read model puede poseer strings y vectores
porque se construye después de adquirir el terminal y sólo vive fuera del callback.

No entra en `ProjectState`, `ProjectSession`, historial, Undo/Redo, dirty state,
`.vitadaw`, recovery marker ni media. Save ignora el resultado; Load puede limpiar la
presentación o conservarla como diagnóstico ambiental claramente desvinculado del
proyecto. La política más sencilla es limpiarla en Load y cambio de device.

## Lifecycle y fallos

### Invalidation inmediata

Durante una medida invalidan la sesión:

- cambio de device o route;
- cambio de sample rate;
- cambio de buffer;
- máscaras/ordinales de input u output distintos;
- device stop/error/loss;
- callback frame mayor que la capacidad preparada;
- discontinuidad de generación o configuración.

El callback sólo publica un código bounded y silencia. `pollDeviceLifecycle()` y el
servicio de control producen el diagnóstico y ejecutan restauración/estado seguro.

### Fallos de señal

Se distinguen al menos:

- `returnNotFound`;
- `signalTooLow`;
- `ambiguousPeaks`;
- `inputClipped`;
- `invalidCorrelation`;
- `timeout`;
- menos de tres trials válidos;
- input/output no disponible;
- preparación/capacidad insuficiente;
- ruta o configuración no restaurable.

Un input con `abs(sample) >= 0.999` marca clipping y detiene nuevos estímulos. Output
se vuelve cero desde el punto seguro siguiente. Ningún fallo importa media, crea
clips, toca historial o modifica Recording Offset.

### Feedback y direct monitoring

Input Monitoring de VitaDAW debe estar OFF tanto en intención como en estado
efectivo. Si está ON, `StartLoopbackLatencyTest` se rechaza; una advertencia no basta
porque el cable puede cerrar un feedback digital.

El direct monitoring físico de una interfaz no tiene una API universal fiable. Puede
alterar lo que se oye o, según routing hardware, cerrar un bucle externo. VitaDAW no
lo detecta ni controla. La UI y el protocolo deben pedir expresamente desactivarlo y
silenciar monitores acústicos antes de conectar el cable.

El bajo nivel, la duración acotada, el output mono seleccionado, el clipping abort y
un comando de cancelación reducen riesgo, pero no sustituyen una conexión adecuada.

### Recording y Playback

- Recording `prepared`, `capturing` o `finalizing`: rechazo.
- Playback proyectado distinto de `Stopped`: rechazo.
- Comando Play o Record mientras corre la prueba: rechazo.
- Buffer/rate/route change solicitado mientras corre: rechazo.
- Pérdida externa de device: invalidación y silencio.

No se pausa automáticamente Playback, no se cancela Recording y no se apaga
Monitoring en nombre del usuario. La persona debe llevar el sistema al estado seguro
y lanzar explícitamente la prueba.

## Frontera RT

### Permitido antes de RT

- construir template y cronograma;
- resolver canales y máscaras;
- configurar/certificar device;
- consultar latencias;
- reservar y poner a cero buffers;
- validar capacidades y overflow;
- preparar el snapshot y el resultado inicial.

### Permitido en RT

- una rama de estado diagnóstico;
- copias bounded de input/template;
- limpieza bounded de output;
- aritmética de índices previamente validada;
- detección simple de clipping;
- contadores y atomics lock-free.

### Prohibido en RT

- allocations o resize;
- locks, waits o condition variables;
- filesystem/WAV/recovery;
- strings, logging o callbacks de UI;
- FFT o correlación;
- consultas/configuración de `AudioIODevice`;
- `ProjectState`, historial o persistence;
- Recording Placement Compensation;
- device reconfiguration.

La instrumentación de allocations debe cubrir el primer bloque, la emisión que cruza
dos callbacks, captura, clipping, timeout, terminal y vuelta al callback normal.

## Tests automatizados

### Análisis puro

1. delays exactos `0/16/64/128/513/1200/4096` sin sesgo por longitud;
2. ganancias de ruta `-6/-24/-50 dB`;
3. inversión global de polaridad;
4. ruido determinista a `30/12/7 dB` y rechazo por debajo de `6 dB`;
5. low-pass, rolloff, FIR corto y ringing representativos;
6. energía fraccional repartida entre dos lags;
7. clipping y dos copias separadas ambiguas;
8. rechazo de silencio, ruido puro, seno estable, audio musical sintético,
   patrón binario incorrecto y señal demasiado baja;
9. índices/overflow no representables;
10. cinco trials, mínimo tres válidos, mediana y min/max/jitter exactos.

### Sonda RT

1. emisión en el frame y offset exactos;
2. captura raw antes de cualquier escritura con alias total y parcial;
3. sólo el output elegido recibe señal;
4. input elegido se mapea desde la máscara física correcta;
5. estímulo que cruza límite de callback;
6. bloque mayor que capacidad falla sin OOB ni realloc;
7. timeout silencia;
8. clipping impide estímulos posteriores;
9. cancelación terminal y siguiente callback normal;
10. instrumentación de cero allocations/locks/waits/logging/filesystem.

### Aplicación y device lifecycle hardware-free

1. snapshot de device, rate, buffer, canales y latencias;
2. device JUCE virtual con retorno sintético conocido;
3. invalidación por buffer change;
4. invalidación por sample-rate change;
5. invalidación por device/route loss;
6. rollback exacto de máscaras/configuración tras éxito y fallo;
7. Monitoring ON rechazado;
8. Recording activo rechazado;
9. Playback activo o comando proyectado rechazado;
10. resultado sin mutación de `ProjectState`;
11. sin entrada de Undo/Redo ni dirty;
12. Save/Load no serializa el resultado;
13. 0.7.3B placement no participa y conserva sus snapshots;
14. 0.7.3A buffer control/rollback intacto;
15. 0.7.2 Monitoring manual/lifecycle intacto;
16. 0.7.1 Recording/Recovery intacto;
17. shutdown durante prueba silencia, terminaliza y restaura o entra en estado seguro.

## Protocolo de smoke físico

### Preparación

1. Guardar cualquier trabajo y detener Playback/Recording.
2. Desactivar Input Monitoring en VitaDAW.
3. Desactivar direct monitoring, mixer interno y efectos de la interfaz si es
   posible; documentar cualquier elemento que no pueda desactivarse.
4. Bajar o desconectar monitores y auriculares.
5. Preferir una conexión `line output -> line input` mediante cable apropiado entre
   el Output N y el Input M seleccionados.
6. Ajustar el input para señal line-level, sin phantom power cuando no proceda y
   con ganancia razonable que evite clipping. No asumir que la ganancia física es
   unity.
7. Confirmar device lógico, sample rate, canales y buffer efectivo mostrados.

### Medición por buffer

Para cada valor soportado de `64, 128, 256, 512, 1024`:

1. seleccionar el buffer mediante el control normal de 0.7.3A;
2. confirmar el buffer efectivo, no sólo el solicitado;
3. comprobar Playback Stopped, Recording idle y Monitoring OFF;
4. ejecutar una sesión de cinco trials;
5. confirmar al menos tres trials válidos, ausencia de clipping y score/ratio no
   ambiguos;
6. registrar reported input, reported output, reported sum, mediana medida,
   min/max, jitter y residual;
7. repetir una segunda sesión si jitter o calidad son anómalos;
8. no reutilizar el resultado para otro buffer, rate, route o device.

Al finalizar, retirar el cable con niveles bajados, restaurar routing hardware y
confirmar que Playback, Monitoring manual y Recording ordinario siguen operativos.
La prueba no debe haber creado WAV, clips, Undo, dirty state ni cambios de Recording
Offset.

## Evidencia física de cierre

El protocolo se ejecutó con Universal Audio Volt 176, macOS/CoreAudio, 48 kHz,
Output 1 MONITOR L → Input 1, Direct Monitor OFF e Input Monitoring de VitaDAW
OFF. Se obtuvieron 25/25 trials válidos en buffers 64, 128, 256, 512 y 1024.
Measured RTT fue respectivamente 412, 540, 796, 1308 y 2332 frames frente a
reported RTT 304, 432, 688, 1200 y 2224: residual estable de `+108` device frames
(`+2.25 ms`), jitter de cero, score aproximado `0.848`, SNR `67–68 dB`, polaridad
normal y ausencia de clipping.

Esta evidencia valida el detector MLS en esa configuración concreta. No atribuye
el residual a la rama de input ni autoriza auto-calibración. La validación
ampliada con Aggregate Device, rutas same-interface dentro del Aggregate,
cross-interface, múltiples salidas/entradas, jitter entre interfaces y otras
interfaces del estudio permanece pendiente por falta de routing físico adecuado;
no se considera un fallo ni un bloqueo de 0.7.3C.

## Riesgos y límites interpretativos

- Los reportes de backend no tienen fronteras físicas idénticas en todos los drivers.
- Ganancia, ruido, inversión, filtros y sample-rate conversion pueden cambiar la
  forma de la señal sin cambiar necesariamente el onset físico.
- Un cable loopback mide DAC + ruta externa + ADC; no aísla cada componente.
- Un mixer hardware, direct monitoring o routing oculto puede crear un retorno más
  corto, duplicado o feedback.
- Aggregate Devices pueden añadir drift correction y jitter dependiente del par de
  subdispositivos.
- Dropouts o carga del sistema pueden producir outliers; la mediana los reduce pero
  no convierte una ruta inestable en calibración fiable.
- La medida está ligada a device, route, sample rate y buffer efectivos.
- El resultado no prueba por sí solo cuál debe ser el offset unilateral de Recording.
- No existe evidencia suficiente para persistir o aplicar calibraciones en esta
  foundation.

## Ruta futura hacia calibración

Después de validar físicamente el modelo se podría añadir, en fases separadas:

1. comparación histórica por device/rate/buffer/route;
2. repetición guiada y clasificación de estabilidad;
3. política explícita para atribuir residual al input;
4. cualquier política futura de calibración requeriría atribución física demostrada y un diseño separado;
5. aceptación manual del usuario;
6. preferencias globales o por device, nunca metadata silenciosa del proyecto;
7. recalibración/invalidation al cambiar la configuración.

Ninguna de estas extensiones debe autoaplicar valores sólo porque exista una medida
round-trip.

## Estado de implementación 0.7.3C

Están implementados los tipos y análisis puro, `RealtimeLoopbackProbe`, el puerto
efímero, los comandos, los interlocks de aplicación, la transacción JUCE del par
físico, el snapshot, la restauración verificada, la selección exclusiva del
callback, la UI mínima y las pruebas sintéticas/RT/lifecycle hardware-free. El
smoke físico por cable y buffers reales está completado para la configuración
Volt 176 descrita; Aggregate/cross-interface continúa como validación ampliada.

## Decisiones finales aprobadas

1. **Sin Suggested Recording Offset:** 0.7.3C muestra reported input/output/sum,
   measured round trip, residual, min/max/jitter y calidad. No calcula una
   recomendación ni modifica 0.7.3B.
2. **Par físico explícito:** el preflight configura temporalmente exactamente el
   input/output elegidos, aunque no estuvieran activos, y restaura después el setup
   completo. Esta foundation permanece mono y no implementa matriz multicanal.
