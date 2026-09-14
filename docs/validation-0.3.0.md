# Validación de VitaDAW 0.3.0 — Processor & Insert Core

Fecha: 14 de septiembre de 2026.

## Alcance

Este incremento introduce una frontera DSP portable y cadenas de inserts
ordenadas en pistas, buses y Master. La implementación de producto disponible
es únicamente `GainProcessor`; `FixedLatencyTestProcessor` existe solo como
doble de test. No se añade hosting VST3/AU, escaneo o GUI de plugins, PDC,
automatización, sidechain, draining de tails, presets externos, sandboxing,
crash recovery, freeze, render-in-place ni multicanal.

## Modelo editable e identidad

Cada `AudioTrack`, `AudioBus` y el estado Master posee una `InsertChain`. Sus
elementos contienen exclusivamente estado portable y persistible:

- `ProcessorInstanceId` fuerte, monotónico y de 64 bits;
- identidad estable de tipo (`internal.gain` en 0.3.0);
- parámetros deseados identificados por `ParameterId`;
- bypass;
- espacio reservado para estado serializable versionado.

La posición en el vector define el orden, pero no la identidad. Mover un insert
conserva su ID; eliminarlo y crear otro consume un ID nuevo. `InsertTarget`
discrimina `TrackId`, `BusId` y Master sin exponer índices densos. El
`ProjectState` nunca contiene instancias DSP mutables.

## Interfaz DSP y formato

`IAudioProcessor` es independiente tanto de JUCE como de
`IRealtimeAudioProcessor`, que continúa representando exclusivamente la
frontera dispositivo -> motor. La nueva interfaz ofrece `prepare`,
`processBlock noexcept`, `reset`, latencia, tail y capacidades/layout.

`ProcessingFormat` explicita:

- sample rate de procesamiento;
- tamaño máximo de subbloque;
- layout mono o estéreo;
- modo realtime u offline.

La latencia usa `ProcessingFrameCount`, siempre en frames del formato DSP, y no
se mezcla con frames del WAV, del proyecto o del dispositivo. Las vistas
`ConstAudioBlockView` y `AudioBlockView` son portables, multicanal en su API y
limitadas a mono/estéreo por las validaciones de esta versión. Un cambio de
sample rate del dispositivo obliga al adaptador a retirar el callback,
reconstruir/preparar el bundle con el nuevo formato y reconectarlo antes de
volver a consumir audio.

## Preparación, ownership y transacción

El `PreparedProcessingBundle` posee conjuntamente el plan inmutable y su
`ProcessingPlanRuntime`. Fuera de RT, la preparación:

1. valida modelo, IDs, tipos, parámetros, layouts, capacidades y límites;
2. crea una instancia DSP independiente por descriptor;
3. llama a `prepare` y obtiene latencia/tail;
4. calcula rangos densos y metadata de llegada por tap/arista;
5. reserva scratch y delay dry de bypass;
6. publica el bundle completo únicamente tras el éxito total.

El runtime posee las instancias mediante `unique_ptr`; nunca se comparten
instancias mutables entre bundle activo y candidato. Un fallo de factoría,
prepare, overflow, capacidad o memoria destruye el candidato fuera de RT y
conserva intactos modelo, bundle y procesadores activos. Los cambios
estructurales se aceptan solo con transporte detenido y el intercambio se hace
con el callback quiescente; el bundle retirado se destruye después de esa
quiescencia.

## Flujo de señal y ejecución por bloques

Por cada subbloque, cada procesador recibe exactamente una llamada, sin importar
fan-out, sends, mute o Solo:

```text
TrackRenderer -> Track InsertChain -> PRE send tap
              -> Track Gain/Pan -> Mute/Solo -> POST send tap
              -> Track Meter -> Main Output

Bus accumulation -> Bus InsertChain -> PRE send tap
                 -> Bus Gain/Balance -> Mute/Solo -> POST send tap
                 -> Bus Meter -> Main Output

Master accumulation -> Master InsertChain -> Master Gain
                    -> Master Meter -> Output
```

Los pre-sends quedan fijados como post-insert y pre-fader/pre-pan o
pre-balance. Una fuente mono atraviesa su cadena en mono y solo después se
adapta al mixer estéreo, evitando aplicar dos veces la ley de pan central. Cada
nodo dispone de dos scratch estéreo alternables, reservados fuera de RT, para
admitir procesadores in-place y out-of-place; para un processor exclusivamente
in-place el host copia al scratch alterno y procesa con input/output aliasados,
manteniendo el dry original disponible para bypass. No existe scratch por send.

`ProcessContext` contiene inicio temporal del subbloque, sample rate, frame
count, modo realtime/offline, estado de transporte y discontinuidad. La
posición inicial se captura antes de avanzar el reloj maestro.

## GainProcessor, parámetros y bypass

`GainProcessor` admite mono/estéreo, `-100..+12 dB`, unity exacta a `0 dB` y
silencio exacto a `-100 dB`. Rechaza NaN, infinito y valores fuera de rango. La
conversión/validación ocurre fuera de RT y el procesador aplica internamente una
rampa lineal de 5 ms sin allocations, locks, I/O, logging ni excepciones en
`processBlock`. Declara latencia cero, tail `none` y aliasing in-place.

Los updates ligeros viajan por el ring SPSC ya preparado como eventos triviales
con generación de plan, índice denso resuelto, `ParameterId`, valor preparado y
offset reservado. En 0.3.0 todos usan offset cero. Cola llena o validación
fallida rechazan explícitamente el comando y no modifican el modelo; una
generación obsoleta no puede alcanzar una instancia que reutilice el mismo
índice.

El bypass es discreto al comienzo de bloque. El procesador permanece en la
cadena y continúa ejecutándose para avanzar smoothing e historial, mientras el
host selecciona dry. Si la latencia declarada es `D`, el dry pasa por un delay
preparado de `D` frames para conservar la latencia efectiva. No hay crossfade,
por lo que un cambio puede producir click.

## Latencia, tail y reset

La latencia de cadena es la suma entera verificada de sus procesadores. El plan
registra latencia de Track, Bus y Master, ambos taps post-insert y cada arista
de output/send con identidad preservada, incluidas aristas paralelas. Esta
metadata permite calcular en el futuro llegada, objetivo de convergencia y
delay requerido, pero **no se inserta PDC en 0.3.0**: dos ramas con 64 y 192
frames llegan realmente separadas 128 frames.

`TailInfo` representa `none`, `finite`, `infinite` y `unknown`. No existe
draining automático; la política de final natural permanece válida para el
único procesador de producto, cuyo tail y latencia son cero. Los tests de
latencia aportan explícitamente el silencio necesario para observar el delay.

`reset` limpia historial, smoothing y bypass delay con el procesamiento
quiescente o en la barrera del callback antes de reanudar. Se garantiza antes
del siguiente Play tras Stop, final natural/replay, recarga, reemplazo de plan y
restart de dispositivo. Stop -> Play rápido respeta el orden FIFO del gate y no
omite la barrera.

## Límites y memoria preparada

- 16 processors por chain;
- 512 processors totales;
- mono y estéreo;
- dos scratch estéreo por nodo;
- presupuesto portable conjunto de 32 MiB para plan/runtime conocido;
- ring SPSC de 64 entradas, 63 pendientes utilizables;
- sample-accurate offset reservado, pero solo offset cero en 0.3.0.

El presupuesto incluye descriptors/estado conocido, scratch, dry delays de
bypass y memoria conocida declarada por procesadores. Todas las sumas y
multiplicaciones relevantes comprueban overflow antes de reservar.

## Tests automatizados

Build completo JUCE y build core-only: **19/19 suites superadas**.

Las nuevas suites `ProcessorInsertCoreTests` y `ProcessorCommandTests`, junto
con las extensiones de lifetime, cubren:

- chain vacía y GainProcessor en Track, Bus y Master;
- mono, estéreo y conservación de la ley de pan mono;
- taps Track/Bus pre y post después de inserts, outputs y Bus -> Bus;
- Track/Bus/Master gain, balance/pan, mute, Solo, meters y sends existentes;
- orden con procesadores no conmutativos;
- una llamada por processor/subbloque con fan-out, sends y ramas inaudibles;
- parámetros, rampa numérica de 5 ms, `-100 dB`, unity, boost y bypass;
- callbacks de 64, 128, 256, 512 y 1024 frames, más callbacks mayores que la
  capacidad interna;
- latencias 128 y 64+128, inserts de Bus/Master, sends post-insert, bypass dry
  alineado, reemplazo y dos rutas paralelas sin PDC;
- reset en Stop, final/replay y restart, incluida secuencia Stop -> Play rápida;
- equivalencia realtime/offline con eventos en el mismo instante;
- eventos de generación obsoleta y ring lleno;
- rechazo de NaN/infinito/rango, IDs duplicados, límites, overflow, presupuesto
  insuficiente y processor `prepare` fallido;
- transacción de Add/Remove/Move, IDs monotónicos, estructuras rechazadas
  durante Play y coherencia modelo/runtime ante fallos;
- lifetime real con `processBlock`, reemplazo, remove, cierre y destrucción
  únicamente después del último uso RT;
- 32 pistas, 8 buses, 123 inserts y 256 sends, sin allocations en
  `processBlock` y dentro del presupuesto preparado.

Comandos ejecutados:

```sh
cmake -S . -B build -DVITADAW_BUILD_APP=ON -DVITADAW_BUILD_TESTS=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure

cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF -DVITADAW_BUILD_TESTS=ON
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

## Sanitizers

- ASan + UBSan: **19/19**, sin diagnósticos finales.
- UBSan independiente: **19/19**, sin diagnósticos.
- TSan: **19/19**, sin carreras detectadas.

ASan detectó durante el desarrollo una referencia colgante en la propia
instrumentación de un test de conteo. Se corrigió el owner del probe y la suite
final quedó limpia; el defecto no pertenecía al motor ni al código de producto.

## Smoke test nativo

Dispositivo observado:

- Altavoces del MacBook Air;
- 48.000 Hz;
- buffer de 512 frames;
- 0 entradas / 2 salidas.

Recursos cargados:

- Snare: `vitadaw-ntrack-440hz-48000-2s.wav`, mono, 48 kHz, 2 s;
- Kick: `vitadaw-ntrack-660hz-48000-4s.wav`, mono, 48 kHz, 4 s;
- Guitar: `vitadaw-ntrack-550hz-44100-3s.wav`, mono, 44,1 kHz, 3 s;
- Audio 4: `vitadaw-ntrack-330hz-44100-1s.wav`, mono, 44,1 kHz, 1 s.

La sesión mantuvo el routing de 0.2.4: Snare/Kick -> Drum -> Music, Guitar ->
Music, Audio 4 -> Master, Track Send Snare -> Plate, Bus Send Drum -> Parallel y
Bus Send Music -> Room. Se añadieron GainProcessor #1 en Snare, #2 en Drum y #3
en Master.

Durante Play se cambiaron los inserts a -12, -6 y -3 dB respectivamente; UI,
comandos y meters confirmaron aplicación sin reconstruir el plan. El bypass de
Snare y Master se aceptó y el procesador siguió avanzando según las pruebas
instrumentadas. Los Track/Bus Sends continuaron mostrando señal, Send Mute
cerró su rama y se ejercitaron Solo y Mute sin alterar el reloj global.

El final natural quedó en `Stopped | 4.000 / 4.000 s`. Play posterior reinició
desde cero; Stop explícito dejó `Stopped | 0.000 / 4.000 s`. El proceso VitaDAW
y el dispositivo cerraron limpiamente. La sesión no dispone de captura
acústica; la presencia y amplitud de señal se verificaron mediante meters y las
comparaciones numéricas del `processBlock` portable.

## Riesgos y límites pendientes

- Solo existe el processor de producto `internal.gain`; no hay hosting externo.
- La metadata de latencia está preparada, pero no hay PDC real.
- No hay draining de tails; solo GainProcessor es admitido en producto y no
  genera tail.
- Parámetro y bypass se aplican a offset cero; no hay scheduler de automation.
- El bypass no tiene crossfade y puede producir click.
- El ring de updates es SPSC y admite un único productor.
- El render sigue siendo escalar y reserva dos scratch estéreo por nodo.
- Solo se admiten layouts mono/estéreo.
- Los cambios estructurales de inserts siguen requiriendo transporte detenido.
- No hay limiter ni clamp de salida.
