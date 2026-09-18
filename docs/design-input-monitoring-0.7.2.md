# VitaDAW 0.7.2 — Diseño previo de Input Monitoring Foundation

> Estado: propuesta arquitectónica previa a implementación. Este documento no
> cambia el comportamiento de 0.7.1 ni describe una capacidad ya publicada.

## Objetivo y contrato

0.7.2 introducirá una ruta mínima de *software input monitoring*: una copia de
la entrada física se suma directamente a la salida del dispositivo. No es una
variante de Recording ni una pista de proyecto.

Las tres responsabilidades son independientes:

```text
Input Capture        -> ring SPSC -> writer/WAV (solo Recording)
Input Monitoring     -> gain de monitor -> suma directa a la salida
Playback             -> render de proyecto -> salida
```

Antes de cualquier limpieza o escritura de `output`, Capture consume el input
raw por su propia ruta. Si Monitoring está activo o termina una rampa, el Core
copia únicamente su par mono/estéreo soportado a staging RT preasignado durante
la preparación. Así la mezcla posterior no depende de que input y output no
compartan almacenamiento. El staging tiene la capacidad de bloque preparada;
un callback mayor no mezcla Monitoring en ese bloque y publica la ruta como
limitada, sin realloc ni lectura fuera de límites. Capture no depende de ese
staging y permanece intacto.

Por tanto, las ocho combinaciones siguientes son legales y no implican una
transición entre las otras responsabilidades:

| Playback | Recording | Monitoring |
| --- | --- | --- |
| Off | Off | Off |
| Off | Off | On |
| On | Off | Off |
| On | Off | On |
| Off | On | Off |
| Off | On | On |
| On | On | Off |
| On | On | On |

En particular, Record no activa Monitoring, Stop Record no lo desactiva y
Monitoring no crea una sesión, writer, WAV, media, cambio de proyecto ni
historial de Recording.

El estado funcional inicial será deliberadamente pequeño: `Disabled` o
`Enabled`, efímero y apagado al arrancar una nueva aplicación. La disponibilidad
del hardware no es un tercer estado de Monitoring: es un resultado de preflight
o un diagnóstico del dispositivo.

## Arquitectura actual auditada

| Responsabilidad | Implementación actual | Consecuencia para 0.7.2 |
| --- | --- | --- |
| Dispositivo y callback nativos | `src/vitadaw/platform/juce/JuceAudioDeviceAdapter.{h,cpp}` posee `juce::AudioDeviceManager`, `AudioDeviceStateModel`, el callback y `RealtimeAudioEngine`. | El adaptador debe seguir siendo el único dueño de la configuración física de entrada/salida. |
| Entrada física | `JuceAudioDeviceAdapter::audioDeviceIOCallbackWithContext()` adapta punteros JUCE a `ConstAudioBlockView` y `AudioBlockView`, y llama a `RealtimeAudioEngine::processBlock()`. | No se debe crear un segundo callback ni una ruta de UI al dispositivo. |
| Capture | `RealtimeAudioEngine::processBlock()` llama a `RealtimeCapture::capture(input)` antes de renderizar Playback. `src/vitadaw/audio/RealtimeCapture.cpp` copia los samples raw necesarios al ring SPSC sólo durante `capturing`. | Monitoring debe leer `input` como const y no tocar el ring, el writer ni los samples antes de Capture. |
| Writer/media | `JuceAudioDeviceAdapter::prepareRecording()`, `serviceRecording()`, finalización y cleanup poseen el temporal, writer y medio publicado. | El preflight de Monitor no puede llamar a `prepareRecording()`: éste reserva ring, temporal, writer y metadata de recovery que Monitoring no necesita. |
| Playback/mix actual | `RealtimeAudioEngine::processSubBlock()` procesa tracks, buses, inserts, metrónomo y master; finalmente escribe los canales 0/1 de `output`. | La ruta de monitor no entra en tracks, buses, inserts, sends, `PreparedProcessingPlan` ni el master de proyecto. |
| Control de UI | `MainWindow` y demás superficies despachan por `ICommandDispatcher`; `DawApplication::handle()` es la entrada de aplicación. | La futura UI debe emitir Commands; no puede hablar con el adaptador ni con el motor directamente. |
| Estado y publicación RT | `RealtimeAudioEngine` usa la cola fija de comandos, `CommandLifecycleGate` y `RealtimeTransportExchange`; `DawApplication::synchroniseTransport()` consume el snapshot/proyección. | Monitoring debe reutilizar esta frontera en lugar de compartir un `bool` ordinario entre UI y callback. |
| Apertura de input | El arranque llama a `initialiseWithDefaultDevices(0, 2)`. La entrada se abre hoy sólo en `prepareRecording()`, mediante detach, configuración de `AudioDeviceManager`, reprepare, attach y rollback. | Enable Monitoring requiere su propio preflight no-RT para abrir/validar input cuando no hay Recording. |

Dos detalles actuales determinan el punto de inserción:

1. `processBlock()` retorna tras `advanceSmoothers()` cuando el reloj no está
   reproduciendo. `processSubBlock()` no se ejecuta entonces ni en la porción
   restante de un bloque después del final natural.
2. La salida se limpia al inicio de `processBlock()` y Playback la escribe
   después, dentro de `processSubBlock()`.

Por ello Monitoring no puede vivir sólo en `processSubBlock()` ni antes de
Capture.

## Flujo de señal propuesto

```text
                              control no-RT
UI -> ICommandDispatcher -> DawApplication -> IAudioEngineControl
                                         |             |
                                         |             +-> preflight de input
                                         |                 en JuceAudioDeviceAdapter
                                         v
                         Enable/Disable: cola RT + lifecycle gate
                         SetMonitorGain: mailbox latest-value SPSC
                                         |
                                         v
AudioIODevice / JUCE callback -> RealtimeAudioEngine::processBlock(input, output)
       |                                      |
       |                                      +-> 1. Capture raw (si Recording)
       |                                      |       -> ring SPSC -> writer/WAV no-RT
       |                                      |
       |                                      +-> 2. Playback (si corresponde)
       |                                      |       -> proyecto/master -> output accumulator
       |                                      |
       +------------------------------->      +-> 3. Monitor Gain -> mismo accumulator
                                                      -> handoff final actual a JUCE/hardware
```

La fase futura `mixInputMonitoring()` es un helper privado y acotado de
`RealtimeAudioEngine`. En la arquitectura actual no existe una segunda etapa
de política final después de `AudioBlockView`: `processSubBlock()`/la variante
legacy ya escriben Playback en `output[0/1]` y el callback devuelve ese buffer
a JUCE. Por ello el punto mínimo correcto es sumar en ese mismo accumulator una
vez que las ramas de Playback han convergido: después del bucle de
`processSubBlock()`, y en la rama `!clock_.isPlaying()` antes de retornar, antes
de publicar snapshots/meters.

La suma cubre todo el bloque, incluido el transporte parado y la parte restante
después de un final natural. Bypassa tracks, buses, sends, inserts y procesamiento
master documentales, pero no es un bypass permanente de una política final de
dispositivo: en 0.7.2 el accumulator es la única frontera común de salida. Si
en el futuro se añade una política final de salida/safety, deberá consumir el
accumulator combinado de Playback + Monitoring.

No se introduce limiter, compressor, normalización ni compensación de gain. El
meter master actual permanece pre-monitor y no se reinterpretará como nivel
físico final; la validación comprobará explícitamente la suma de muestras y el
riesgo de clipping de Playback + Monitoring.

## Ownership de estado

Se proponen tres propietarios, cada uno con una responsabilidad distinta:

| Estado | Propietario | Vida y alcance |
| --- | --- | --- |
| Selección/apertura física de input, input/output activos y diagnóstico de dispositivo | `JuceAudioDeviceAdapter` | No-RT; depende de `AudioDeviceManager`; nunca lo toca el callback. |
| Estado aplicado al render: `enabled`, ganancia lineal objetivo y `LinearSmoother` | `RealtimeAudioEngine` | Exclusivo de RT; no conoce `ProjectState`, writer ni filesystem. |
| Intención de control y read model de UI: `enabled` y gain solicitados/confirmados | `DawApplication` | Efímero de aplicación, fuera de `ProjectState`, `DocumentData`, persistencia, Undo/Redo y `ProjectSession::dirty()`. |

El read model de aplicación debe ser un miembro dedicado de
`DawApplication`, no un campo documental de `ProjectState`. Así no cambia al
guardar/cargar un proyecto ni se convierte por accidente en una preferencia
persistida. Puede contener intención pendiente para ordenar Commands, pero la
UI mostrará el valor confirmado/proyectado por RT cuando haya una respuesta
válida del motor.

No se propone una máquina de estados adicional. Un fallo de preflight o de
cola deja el estado en `Disabled` o en el último valor confirmado; el mensaje
de fallo procede del resultado de Command o de `AudioDeviceState`, no de una
tercera fase de Monitoring.

## Frontera control-thread / audio-thread

### Fuera de RT

`DawApplication` normaliza los Commands, valida ganancia y llama al puerto
`IAudioEngineControl`. Para Enable cuando todavía no hay input activo, el
adaptador debe ejecutar una operación conceptual
`prepareMonitoringInput`/`ensureMonitoringInput`:

1. inspeccionar el dispositivo y guardar su configuración, checkpoint e
   intención de monitor;
2. retirar el callback de forma quiescente;
3. seleccionar un input válido sin cambiar la selección de output;
4. activar los canales necesarios mediante `AudioDeviceManager`;
5. repreparar la configuración actual del motor y volver a registrar el
   callback;
6. verificar canales/rate y, si falla un paso, hacer rollback de la
   configuración previa antes de devolver un error.

El preflight de Enable se mantiene provisional hasta que el Command RT ha sido
aceptado. Si la reconfiguración tuvo éxito pero el gate/cola rechaza ese Command
antes de que haya Recording activa, el adaptador restaura el setup y checkpoint
previos y devuelve rechazo; no deja un input recién abierto asociado a un
Monitoring que sigue Disabled. Durante Recording no hay tal transacción de
setup: Enable usa los canales ya activos y un rechazo sólo deja el estado RT
sin cambio.

Este patrón debe reutilizar la disciplina ya usada por
`JuceAudioDeviceAdapter::prepareRecording()`, pero no sus recursos de
Recording. Nunca se configura `AudioDeviceManager` desde el callback.

Disable no cierra ni reconfigura físicamente el input. Sólo solicita que la
ruta RT alcance ganancia cero. Esto evita afectar una captura en curso y evita
que una alternancia de Monitor haga cambios de dispositivo innecesarios.

### En RT

El callback sólo podrá:

- consumir un Command discreto preasignado o el último payload del mailbox;
- actualizar `enabled` y el target lineal de un `LinearSmoother`;
- comprobar punteros, número de canales y frames disponibles;
- llamar a `LinearSmoother::next()` y sumar floats a `output`;
- publicar el snapshot existente.

No podrá reservar/liberar memoria, crear strings, lanzar/capturar excepciones,
usar locks, hacer logging, filesystem, I/O, espera, configurar JUCE ni llamar a
la UI. La conversión dB -> lineal se hace antes de publicar el payload; no se ejecuta
`pow()` en el render.

### Publicación y orden

Enable y Disable son acciones discretas: reutilizan la cola principal de
`RealtimeAudioEngine`, su `CommandLifecycleGate`, ticket y proyección. Un
Enable sólo actualiza la intención después de que su preflight y el enqueue
sean aceptados; un reconfigure controlado puede reemitir esa intención en la
nueva generación.

`SetMonitorGain` tiene una frontera distinta. La cola principal tiene capacidad
efectiva de siete entradas y es FIFO; la cola de parámetros existente tiene 64
entradas pero también es FIFO y no sustituye una entrada pendiente. Ninguna de
las dos cumple *latest value wins* para un slider y no se deben sobrescribir sus
slots pendientes.

La solución mínima será un mailbox SPSC dedicado de un único payload, propiedad
de `RealtimeAudioEngine`, no una infraestructura genérica de automatización:

1. `commands::SetMonitorGain` llega por `ICommandDispatcher` a
   `DawApplication`, que valida `[-100, 0] dB`, conserva el valor efímero
   solicitado y prepara el factor lineal.
2. `DawApplication` es el único productor y llama a un método específico de
   `IAudioEngineControl`, por ejemplo `publishMonitorGain`; la UI nunca escribe
   en el motor.
3. El método almacena con release un `atomic<uint64_t>` siempre lock-free que
   empaqueta el dB validado y el factor lineal ya calculado. No se usan dos
   atomics de float/estructura cuya lectura pudiera quedar incoherente.
4. El callback carga ese payload con acquire una vez por bloque. Si difiere del
   último visto, actualiza el target del `LinearSmoother`; los valores
   intermedios se descartan intencionadamente.

Una ráfaga `A -> B -> A` antes del siguiente callback produce `A`, que es el
último valor y por tanto el resultado correcto. `SetMonitorGain` no reserva un
ticket ni puede llenar ninguna cola; mientras Monitoring está Disabled conserva
el gain para el próximo Enable, pero la ruta sigue a cero.

`RealtimeTransportExchange` sigue siendo sólo la publicación RT -> no-RT. Se
extenderá para exponer `monitoringEnabled`, el gain aplicado y un diagnóstico
compacto de ruta cuando corresponda; no es la entrada del slider. El read model
de `DawApplication` distingue el último valor solicitado del aplicado. No se
ofrece acknowledgement de cada valor intermedio, sólo del estado final.

La sincronización de Monitoring no puede quedar después del retorno temprano
actual de `DawApplication::synchroniseTransport()` cuando
`pendingAudioCommandSequence_` aún no está resuelto. La futura sincronización
de `Enabled/Disabled`, gain aplicado y diagnóstico debe procesarse antes de ese
guard o en una función separada, para que un OFF por pérdida real sea observable
sin quedar retenido por un ticket de transporte ajeno.

La falta de input u output observada dentro de un callback requiere un handoff
explícito, no una inferencia que hoy ya exista. Si Monitoring está Enabled y no
hay una ruta mínima segura, RT hace no-op para ese bloque y deja un latch
atómico sin strings ni logs. Un bloque aislado inválido no prueba por sí solo
una pérdida física. El control thread debe comprobar el lifecycle/input activo
y emitir el diagnóstico; `deviceStopped`, error y cierre ya son confirmaciones.
Al confirmar pérdida real de input/output, `JuceAudioDeviceAdapter` no escribe
el snapshot ni muta el estado RT directamente. Publica al motor una solicitud
atómica específica, lock-free y no descartable de *lifecycle-off*; el callback
la consume antes de mezclar, lleva el target a cero, fija `Enabled = false` y,
como escritor único, publica el snapshot. Si ya no puede haber callback por
`stopped`, error o cierre,
`RealtimeAudioEngine::transitionAwayFromOperational()` aplica el mismo reset
desde la transición serializada/quiescente antes de su `publishTransport()`.
`DawApplication` sólo consume ese resultado y borra su intención. No se
auto-reactiva al volver hardware. El latch es una causa/diagnóstico, no un
tercer estado funcional.

El contrato de escritor único es explícito: UI/control sólo publican Commands,
la solicitud atómica de lifecycle o una certificación de hardware ya preparada;
el callback es el único escritor de `Enabled`, ruta efectiva y smoother. Las
escrituras directas durante un reconfigure sólo son válidas después de retirar
el callback. `audioDeviceError()` no limpia demanda, `optional` ni estado RT:
publica un evento atómico que `pollDeviceLifecycle()` consume de forma
serializada y, si corresponde, primero desacopla el callback.

La configuración certificada contiene al menos sample rate y buffer size. Un
cambio de cualquiera obliga a `pollDeviceLifecycle()` a desacoplar, re-preparar
plan/staging y reacoplar; Monitoring se conserva únicamente tras éxito. Un
fallo invalida demanda e intención y fuerza OFF, sin auto-reactivación.

Hay un caso de orden que debe resolverse de forma explícita: Enable puede haber
sido aceptado por la cola pero no consumido cuando llega Record. El preflight
de Record desconecta el callback y puede cancelar ese ticket. En una
reconfiguración controlada, `DawApplication` debe conservar la intención
aceptada/proyectada, pasar su demanda de input al preflight y reemitir el estado
en la nueva generación antes de `beginRecord`. Preservar sólo el último valor
ya consumido no basta. Ante pérdida/error no controlado no se reemite.

## Commands y UI futura

Los Commands mínimos de superficie son:

| Command | Parámetros | Semántica |
| --- | --- | --- |
| `ToggleInputMonitoring` | ninguno | Atajo de UI; se resuelve en aplicación contra la intención proyectada. |
| `EnableInputMonitoring` | ninguno | Solicita preflight si hace falta y después el estado RT `true`. |
| `DisableInputMonitoring` | ninguno | Lleva la ruta RT a cero; no toca el dispositivo ni Recording. |
| `SetMonitorGain` | gain dB finito en `[-100, 0]` | Publica el último payload en el mailbox dedicado; actualiza el target incluso si está Disabled, para el próximo Enable. |

El mensaje canónico hacia el motor será equivalente a
`SetInputMonitoringEnabled(bool)` y `SetMonitorGain(preparedLinearGain)`. Los
dos Commands de estado y Toggle se normalizan al primero; el segundo se
publica por el mailbox latest-value. No hay una ruta especial desde widgets a
`RealtimeAudioEngine`.

`DawApplication::handle()` rechaza hoy todo Command excepto Stop y
CancelRecording mientras `recordingBusy()` es verdadero. En 0.7.2 debe dejar
pasar de manera explícita los cuatro Commands de Monitoring. También deben
quedar fuera de su lista de Commands persistentes. Esa excepción permite
`Recording -> Monitoring ON/OFF`, pero no concede acceso al writer, session,
capture, historial ni commit de proyecto.

La semántica de gain es explícitamente *latest value wins*: el Command System
sigue siendo autoridad de control, pero el render recibe sólo el último valor
publicado de forma bounded. No hay Undo/Redo, `ProjectState`, dirty, revisión
documental ni persistencia asociados a ese Command.

## Monitor gain y transición sin clicks

La política cerrada para 0.7.2 es un gain de monitor propio: Monitoring está
OFF por defecto, el gain inicial es `-12 dB`, el rango es `[-100, 0] dB` y
`-100 dB` significa silencio. No se permite gain positivo. No es el gain de
pista, bus o master y no se guarda en el proyecto.

Antes de entrar a RT el Command valida el valor y prepara el factor lineal. En
RT se reutiliza `audio::LinearSmoother` con los `5 ms` ya usados por
`MixerSmoother`:

- Enable fija target al gain de monitor;
- Disable fija target a cero;
- Set Monitor Gain publica el último target del mailbox;
- cada frame obtiene exactamente un `next()`.

Así Capture recibe siempre el `input` original, mientras la copia que se suma a
la salida usa `input * gainLineal`. El smoother debe avanzar incluso si falta
input durante un bloque, para que una rampa no quede congelada. En un
reconfigure controlado se rearma con el sample rate nuevo y una breve rampa de
entrada, sin reservar memoria.

## Política de canales y salida

La foundation soporta una ruta de monitor mono o estéreo, no una matriz
multicanal. No introduce selector manual de canal, pan law configurable ni
ruteo configurable.

### Apertura de input

El arranque actual abre `0` inputs, por lo que Enable fuera de Recording sigue
este flujo no-RT: petición de Command -> inspección/preparación de input en el
control thread -> validación/reprepare del dispositivo -> publicación RT de
Enabled. Reutiliza el patrón transaccional de `prepareRecording()` sin crear
ring, writer, temporal ni media. El callback nunca abre ni reconfigura
`AudioIODevice` por Monitoring.

La política cerrada para ese preflight usa el input actual/válido o el input
por defecto del dispositivo; solicita sus dos primeros canales y hace fallback
transaccional a uno si sólo hay mono. Conserva la selección de output. Si no
puede certificar al menos una ruta mono válida, hace rollback y devuelve
`Disabled` con diagnóstico.

Durante Recording no se reconfigura el dispositivo sólo para habilitar
Monitoring: se usa el input ya activo de la captura. Si Monitoring ya estaba
Enabled antes de Record, la demanda efectiva de input en el preflight será el
máximo entre canales de Capture y canales de Monitor. Esto corrige la política
actual de Recording, que limpia y activa exactamente sus canales solicitados:
una captura mono no debe reducir dos entradas que el monitor ya necesita.

### Mapeo en el callback

| Input/layout soportado | Output/layout soportado | Contribución de Monitoring |
| --- | --- | --- |
| cero frames comunes | cualquiera | ninguna; es un bloque vacío benigno y no cambia estado. |
| `in[0]` o ningún destino de output utilizable ausente con frames | cualquiera | ninguna; si Monitoring estaba Enabled, no se lee memoria no válida y RT deja un latch para validación no-RT. |
| 1 canal (`in[0]`) | 1 canal | `in[0] * gain` a output 0. |
| 1 canal (`in[0]`) | 2 canales | duplicar `in[0] * gain` a L/R. |
| 2 canales (`in[0]`, `in[1]`) | 1 canal | `(in[0] + in[1]) * 0.5 * gain` a output 0. |
| 2 canales (`in[0]`, `in[1]`) | 2 canales | `in[0] * gain -> L`; `in[1] * gain -> R`. |
| más de 2 canales activos entregados a Monitor | par principal 1/2 certificado | sólo el par principal se usa; se publica diagnóstico de limitación, no se afirma soporte multicanal. |
| más de 2 canales activos entregados a Monitor | sin par principal certificable | Enable se rechaza o Monitoring se deshabilita con diagnóstico `unsupported monitor layout`; Playback existente no cambia. |

El número de frames procesado es el mínimo seguro entre input y output. Un
segundo input nulo degrada a la política mono de `in[0]` y un segundo output
nulo deja sólo la contribución de output 0. La ausencia de `in[0]` o `out[0]`
en un bloque con frames con Monitoring Enabled deja un latch para comprobar la
ruta fuera de RT; no basta por sí sola para afirmar device loss. Para esta
foundation `out[0]` es el único destino mínimo utilizable, coherente con la
política actual de Playback. Un bloque vacío no cambia estado. Recording
conserva su validación estricta: si necesita un canal raw que no existe, sigue
fallando con su propio `missingInput`.

La suma es `output += monitor` en el accumulator final común actual. No se
añade limiter, clamp, plugin, normalización ni cambio del master fader en
0.7.2. Para entrada estéreo hacia una salida mono, el fold fijado es
`0.5 * (L + R)` antes de Monitor Gain: evita el incremento de +6 dB de señales
correlacionadas. Playback más monitor puede exceder 0 dBFS; el gain inicial
conservador y Monitoring apagado reducen el riesgo, pero no lo eliminan. Esta
es una limitación declarada, no un clipping silencioso.

## Interacciones de Recording

| Transición | Comportamiento requerido |
| --- | --- |
| Monitoring OFF -> Record | Se ejecuta el preflight/writer habitual de Recording; no se encola Enable de Monitor. |
| Monitoring ON -> Record | El preflight controlado preserva la intención y la demanda de input del monitor; Capture empieza con raw input independiente. |
| Recording -> Monitoring ON | El Command queda permitido por el guard de `recordingBusy()`. Usa los canales ya activos; no crea sesión, ring, temporal ni writer. |
| Recording + Monitoring -> Monitoring OFF | El target de monitor baja a cero; Capture, ring y writer continúan sin cambios. |
| Recording + Monitoring -> Stop Record | Stop mantiene su terminalización actual de Capture/clock; no modifica Monitoring. |
| Stop Record con Monitoring previamente ON | Finalización/commit de Recording siguen su flujo no-RT; la suma de monitor continúa aunque el reloj ya esté parado. |

Ninguna de estas transiciones puede cambiar el contenido de los samples que
`RealtimeCapture` ya copió ni alterar la política de recovery/persistencia de
0.7.1.

## Interacciones de Playback

Monitoring no consulta ni modifica el reloj de transporte. Por estar en la
fase final de cada bloque:

- Playback + Monitoring suma ambos componentes;
- Play, Pause, Stop y Seek sólo cambian la contribución de Playback;
- el final natural deja Monitoring activo;
- Loop modifica sólo el render/clock de Playback;
- con Playback apagado, Monitoring sigue produciendo salida si input y output
  son válidos.

Esta independencia es precisamente la razón de no insertar la ruta dentro de
`processSubBlock()` ni en la cadena de buses.

## Lifecycle de dispositivo

| Evento | Política propuesta |
| --- | --- |
| Preflight de Monitor/Record o reprepare controlado correcto | Preservar intención, gain y Commands de Monitor aceptados pendientes; reconfigurar sólo fuera de RT y rearmar la rampa al nuevo rate. |
| Cambio controlado de buffer, sample rate o restart | Si la ruta mono/estéreo vuelve a certificarse, Monitoring puede seguir Enabled. Si hay Capture activa, 0.7.2 no modifica la política existente: la reinicialización puede terminalizar Recording con `deviceLost`. |
| Buffer de input/output inválido en callback con Monitoring Enabled | No-op seguro de ese bloque y latch RT sin strings/logs. El control thread debe validar la ruta; el buffer aislado no se interpreta por sí solo como pérdida física. |
| Pérdida real de input/output confirmada | `JuceAudioDeviceAdapter` publica sólo la solicitud atómica lifecycle-off. Si hay callback, éste aplica OFF y publica el snapshot; en stopped/error/closed, `transitionAwayFromOperational()` aplica el mismo reset quiescente antes de publicar. `DawApplication` consume el estado/diagnóstico y no hay auto-reactivación. |
| `deviceStopped`, error o cierre no controlado | Son confirmaciones de pérdida real: no hay escritura de monitor y se fuerza `Disabled`. El comportamiento existente de device loss para Recording no cambia. |
| `shutdown()` de aplicación | Primero se ejecuta el shutdown de Recording ya existente; después se cierra el dispositivo. Monitoring no hace I/O, no genera media y termina Disabled al destruirse la aplicación. |

La distinción entre reconfigure controlado y pérdida real es importante:
apagar Monitoring ante todo detach interno rompería la transición
Monitoring ON -> Record, porque el preflight actual desacopla y vuelve a acoplar
el callback. Sólo una pérdida/error/cierre confirmado debe forzar OFF.
`pollDeviceLifecycle()` certifica rate y buffer, consume eventos de parada/error
serializados y comprueba la desaparición específica de la ruta de input sin
atribuir una pérdida a un único bloque RT inválido.

## Feedback y seguridad acústica

La ruta de software no crea un bucle digital interno: lee input físico y suma a
output. El feedback posible depende de la ruta física/driver (micrófono,
altavoces, loopback del dispositivo) y no hay evidencia de que 0.7.2 pueda
detectarlo de manera fiable.

La protección mínima proporcionada por esta foundation será:

- Monitoring OFF al inicio;
- activación siempre explícita por Command;
- Record nunca lo activa;
- gain inicial atenuado propuesto;
- ninguna reconexión automática tras pérdida de dispositivo.

No se diseña detección inteligente ni control de direct monitoring hardware.
El uso de auriculares y el control de ganancia de entrada/salida siguen siendo
responsabilidad operativa durante el smoke físico futuro.

## Invariantes que debe preservar la implementación

### Contrato final de 0.7.2

1. Recording no implica Monitoring.
2. Monitoring no implica Recording.
3. Monitoring no depende de Playback.
4. Capture recibe siempre la señal raw, sin Monitor Gain.
5. Monitoring no modifica `ProjectState`.
6. Monitoring no modifica Undo ni Redo.
7. Monitoring no modifica dirty state, revisión documental ni `StateToken`.
8. Monitoring no crea media, WAV ni archivos.
9. Monitoring no participa en persistencia ni se restaura al abrir un proyecto.
10. Habilitar Monitoring nunca configura hardware desde RT.
11. `SetMonitorGain` es bounded y *latest value wins* mediante su mailbox;
    el callback consume sólo el último valor disponible.
12. Una pérdida real de dispositivo/input/output fuerza Monitoring OFF.
13. No existe auto-reactivación después de device loss.
14. Shutdown con Monitoring activo es seguro y no añade I/O propio.
15. Una aplicación nueva comienza con Monitoring OFF.

### Independencia

- Recording no es Monitoring y Monitoring no es Playback.
- Cualquier combinación de la tabla inicial es válida.
- Enable/Disable/Stop/Play/Seek no producen transiciones implícitas de los
  otros dos dominios.

### Proyecto e historial

- Ningún Command de Monitor muta `ProjectState`, `DocumentData`,
  `ProcessingPlan` documental, `StateToken`, revisión, dirty state, Undo o
  Redo.
- No hay persistencia de gain, estado, media o preferencias de Monitoring en
  0.7.2.

### Integridad de Recording

- Capture recibe los samples raw antes de la ganancia/mezcla de Monitoring.
- Monitoring no llama a `prepareRecording`, no crea writer/ring/sesión y no
  modifica sus terminalizaciones.
- Un fallo de Monitor no publica, borra ni conserva media de Recording.

### RT

- `processBlock()` sigue sin allocations, filesystem, locks, logging,
  strings, excepciones, llamadas de UI ni configuración del dispositivo por
  causa de Monitoring.
- El coste por bloque es lineal y acotado por el número de frames y los dos
  primeros canales; no depende de topología de proyecto ni de I/O.

### Lifecycle y persistencia

- Shutdown con Monitoring Enabled es seguro y no crea archivos.
- Los cambios de configuración controlados preservan la intención; una
  pérdida/error real la borra de forma segura.
- Una aplicación nueva empieza Disabled y no restaura Monitoring desde un
  `.vitadaw`.

## Casos de fallo esperados

| Caso | Resultado requerido |
| --- | --- |
| No hay dispositivo/input al activar | Command rechazado con diagnóstico no-RT; no cambia ProjectState ni estado confirmado. |
| Fallo de configuración/reprepare de input | Rollback del setup previo; no callback parcialmente publicado, no writer/ring/media. |
| Preflight correcto pero Enable rechazado por gate/cola | Si no hay Recording activa, rollback del setup/checkpoint provisional; no queda un input recién abierto asociado a Monitoring Disabled. |
| Gain no finito/fuera de `[-100, 0]` | Rechazo de validación antes del mailbox. |
| Cola llena o lifecycle no operacional para Enable/Disable | Rechazo explícito; la UI no asume que el estado cambió. `SetMonitorGain` no ocupa esa cola. |
| Command de Monitor pendiente y preflight Record controlado | Preservar/reemitir intención y gain en la nueva generación antes de Record; no perderlos silenciosamente. |
| `in[0]` o ningún output utilizable ausente con frames y Monitoring Enabled | No-op seguro y latch RT para validación no-RT, sin acceso fuera de límites. Sólo una pérdida real confirmada fuerza `Disabled`. Un segundo canal ausente degrada a mono/salida 0; un bloque de cero frames o Monitoring Disabled es sólo un no-op benigno. |
| Layout activo superior sin par principal certificable | Rechazo/Disable con diagnóstico de layout no soportado; no se simula soporte multicanal. |
| Device loss con Recording activo | Se conserva exactamente la terminalización de Recording de 0.7.1; Monitoring se apaga sin I/O propio. |
| Suma por encima de unidad | Float sum directo sin limiter; riesgo conocido de clipping, no corrupción de Capture ni de ProjectState. |

## Fuera de alcance de 0.7.2

Quedan explícitamente fuera de esta foundation:

- plugin monitoring;
- insert FX;
- sends;
- monitor buses configurables;
- latency compensation;
- manual recording offset;
- hardware direct-monitor control;
- punch recording;
- loop recording;
- multitrack recording;
- MIDI monitoring;
- talkback/voice-control routing;
- per-track monitoring avanzado;
- feedback detection inteligente;
- selección manual de fuente/canales de input;
- meter dedicado de monitor o de salida física;
- limitador de monitor o nueva arquitectura de mixer.

La ruta directa propuesta no bloquea una evolución futura hacia esas
capacidades, pero 0.7.2 no debe anticiparlas con buses, estados persistentes ni
abstracciones genéricas.

## Plan de pruebas para la implementación futura

### Motor portable y RT

1. Con Monitoring OFF, confirmar que `output` conserva exactamente Playback o
   silencio actual.
2. Con Monitoring ON y sin Playback, probar mono -> L/R, estéreo -> L/R,
   estéreo -> mono, canales extra, cero canales y punteros nulos.
3. Verificar que gain cambia sólo la contribución a output y que el contenido
   drenado de `RealtimeCapture` es exactamente el input raw.
4. Cubrir las ocho combinaciones Playback/Recording/Monitoring, incluyendo
   Stop Record con Monitor activo, final natural y loop.
5. Probar orden de Toggle/Enable/Disable, cancellation de lifecycle y el caso
   Enable pendiente seguido de Record.
6. Enviar cientos o miles de `SetMonitorGain` antes de un callback: ninguna
   cola crece/queda llena y el callback usa sólo el último payload. Cubrir
   también `A -> B -> A`, actualización estando Disabled y gain durante
   Recording con Capture bit-identical.
7. Verificar la suma de Playback + Monitor, incluidos valores por encima de
   unidad, sin limiter/clamp implícito y sin reinterpretar el meter master.
8. Instrumentar `processBlock()` con monitor on/off y cambios de gain para
   demostrar cero allocations; ejecutar ASan/UBSan y TSan según la convención
   existente.

### Aplicación y Commands

1. Con un fake de `IAudioEngineControl`, comprobar que los cuatro Commands no
   cambian `ProjectState`, documento, preparación de plan, Undo/Redo,
   `StateToken`, revisión ni dirty state.
2. Comprobar que son aceptables durante `recordingBusy()` y que un rechazo de
   preflight/cola no altera el read model.
3. Comprobar que el valor solicitado/aplicado de gain coalesce sin ticket por
   valor y que una pérdida real se observa aunque haya un ticket de transporte
   pendiente.
4. Comprobar que cargar/guardar/Undo/Redo no persiste ni restaura Monitoring.

### Integración JUCE sin hardware

1. Usar el `AudioDeviceManager` y `JuceAudioDeviceAdapter` productivos para
   demostrar que Enable abre input sin crear Recording y conserva output.
2. Inyectar un bloque por el callback JUCE real; el dispositivo virtual actual
   que sólo notifica `audioDeviceAboutToStart()` no cubre el render de input.
3. Probar Monitor ON -> Record, Record -> Monitor ON, reconfigure controlado,
   input/output loss confirmado, layout superior diagnosticable y segunda
   grabación limpia.

### Smoke físico posterior

Con auriculares y nivel seguro: activar/desactivar Monitor con Playback
parado/en marcha, grabar mono/estéreo, detener la toma conservando Monitor,
reproducir, cambiar gain y verificar que la toma guardada no varía. Validar
además el margen de suma Playback + Monitoring sin esperar protección de
clipping inexistente. No forma parte de esta fase de diseño.

## Riesgos técnicos principales

1. **Apertura de input.** El arranque actual no abre input; un flag RT sin
   preflight produciría un monitor silencioso o no fiable.
2. **Orden de lifecycle.** La cancelación de Commands pendientes durante un
   detach controlado debe preservar la intención de monitor sin reactivarla
   tras una pérdida real.
3. **Demanda de canales.** El setup actual de Recording reemplaza el conjunto
   de canales; hay que calcular el máximo de Capture/Monitor fuera de RT.
4. **Feedback y clipping.** Son riesgos acústicos reales; la foundation los
   limita por política, no por detección o limiter inexistente.
5. **Publicación continua.** Las colas FIFO actuales no sirven para slider;
   el mailbox dedicado debe permanecer SPSC, lock-free y de último valor.
6. **Diagnóstico de input.** El lifecycle actual no identifica por sí mismo la
   pérdida de input; la validación/latch no-RT debe evitar falsos device loss.
7. **Sincronización.** El read model de Monitoring no puede quedar retenido
   detrás de `pendingAudioCommandSequence_` de transporte.
8. **Semántica de meter.** El meter actual representa la mezcla de proyecto;
   no se debe hacer parecer accidentalmente que ya mide la suma física final.

## Plan mínimo de implementación futura

1. Añadir el read model efímero y los cuatro Commands, incluidos los permisos
   explícitos durante `recordingBusy()` y exclusión de persistencia/historial.
2. Extender el puerto `IAudioEngineControl` y el adaptador con un preflight de
   input exclusivo de Monitoring, rollback y cálculo de demanda efectiva de
   canales.
3. Añadir al motor el estado RT mínimo, Commands discretos, mailbox de gain
   latest-value, publicación/sincronización independiente de tickets de
   transporte y preservación de intención en reconfiguraciones controladas.
4. Añadir `mixInputMonitoring(input, output)` en el accumulator final común,
   con mapping definido, diagnóstico explícito de layouts superiores y smoother
   preasignado.
5. Implementar primero las pruebas portables y de Commands, después la
   integración JUCE/callback real y finalmente instrumentación/smoke físico.

Cada paso debe conservar los invariantes anteriores y no introducir media,
persistencia ni cambios de `ProjectState` para Monitoring.

## Decisiones que requieren aprobación antes de implementar

No quedan decisiones arquitectónicas bloqueantes. Quedan cerradas para 0.7.2:
Monitoring OFF al arranque; gain `-12 dB` y rango `[-100, 0] dB`; ruta
mono/estéreo con fold `0.5 * (L + R)`; suma en el accumulator común final sin
limiter/clamp; mailbox SPSC de último valor para gain; y OFF explícito sin
auto-reactivación tras una pérdida real de dispositivo.

Las extensiones necesarias para diagnosticar input loss y sincronizar el estado
fuera de RT son trabajo de implementación con una arquitectura ya decidida, no
decisiones de producto pendientes.

## Integración de producto de la foundation C

La aplicación presenta un único control `Monitor ON/OFF`, un gain de `-100` a
`0 dB` y un indicador de pico de entrada. La superficie sólo emite los Commands
de Monitoring existentes; no escribe el motor, el adaptador ni el dispositivo.
El botón se vuelve a pintar exclusivamente desde el read model confirmado, por
lo que un preflight o una publicación RT pendiente no aparece como ON.

El pico de entrada se calcula en `RealtimeAudioEngine::processBlock()` sobre
los samples raw de entrada, después de que Capture pueda observarlos y antes de
staging, Monitor Gain o escritura de output. Es un peak mono/estéreo por bloque
publicado por el intercambio de meters lock-free; no abre input, no modifica
samples y no depende de Recording. La UI aplica únicamente una caída visual
local. Por tanto, con Monitoring OFF el indicador sólo muestra señal cuando el
input ya está abierto, por ejemplo por Recording.

La UI muestra ruta limitada/no disponible y OFF forzado desde publicaciones
existentes de lifecycle. No reintenta ni reactiva Monitoring después de una
pérdida. Siguen fuera de alcance routing avanzado, monitor por pista, efectos,
latency compensation, direct-monitor de hardware, punch, loop recording y
multitrack recording.
