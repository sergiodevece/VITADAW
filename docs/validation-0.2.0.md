# Validación de VitaDAW 0.2.0 — Routing Foundation

Fecha: 11 de septiembre de 2026.

## Alcance validado

Este incremento sustituye la suma directa de pistas al master por routing
portable preparado fuera del hilo de audio. Admite N pistas, N buses estéreo,
un único destino principal por pista (`Master` o `BusId`) y buses dirigidos
exclusivamente a Master. Añade peak metering por bus y no añade bus a bus,
sends, inserts, plugins, PDC, feedback ni routing durante reproducción.

## Modelo y plan preparado

`RoutingState` es la única fuente editable de relaciones de routing. Cada pista
tiene exactamente un `TrackRoute`; un destino de bus solo es válido si su
`BusId` fuerte, monotónico y de 64 bits existe. Master es único, terminal y no
necesita una identidad pública.

`PreparedProcessingPlan` compila fuera de RT las identidades, destinos densos,
recursos, duración, asignación de buffers y orden explícito de pasos. Un
`ProcessingPlanRuntime` separado contiene los buffers estéreo de buses y master
y la tabla temporal mutable. Plan, runtime y propietarios de recursos se
publican y conservan juntos.

El orden preparado es:

```text
Tracks -> Buses -> Master
```

Cada pista se renderiza una sola vez por subbloque y se acumula únicamente en su
destino principal. Cada bus aplica procesamiento identidad, se mide y se suma al
master. El master conserva gain smoothing y peak meter. Ningún nodo mantiene ni
avanza un reloj propio: todas las pistas reciben la misma tabla de posiciones
derivada del único `RealtimeProjectClock`.

## Buffers, subbloques y límites

La preparación reserva un buffer estéreo por bus, un buffer estéreo master y una
tabla temporal para 512 frames por defecto. Si el dispositivo entrega un bloque
mayor, `processBlock` lo divide sin redimensionar: el reloj y los smoothers
continúan entre subbloques y los meters conservan el máximo del callback
completo.

Límites deliberados de este incremento:

- 256 pistas preparadas;
- 64 buses;
- presupuesto RT por defecto de 16 MiB;
- mono o estéreo como formatos fuente preparados;
- buses con procesamiento identidad y destino fijo a Master.

La preparación rechaza identidades inválidas o duplicadas, destinos inexistentes,
formatos incompatibles, capacidades inválidas y presupuestos de memoria
excedidos.

## Cambios estructurales y lifetime

Crear pistas, crear buses y cambiar el destino de una pista son operaciones
estructurales y se rechazan mientras el transporte está reproduciendo. Con el
transporte detenido se construyen y validan primero un modelo y plan candidatos.
El adaptador retira el callback, intercambia conjuntamente recursos, plan y
runtime, ejecuta el commit `noexcept` del modelo y vuelve a registrar el callback.
Un fallo de preparación conserva íntegramente el modelo y el plan anteriores.

La preparación estructural reutiliza los recursos ya decodificados y no llama a
la carga WAV. Por tanto, un proyecto vacío, buses vacíos y routing sin archivos
pueden prepararse independientemente.

## Builds y tests

Configuración core-only:

```sh
cmake -S . -B build-core \
  -DVITADAW_BUILD_APP=OFF \
  -DVITADAW_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

Resultado: **13/13 tests superados**.

Configuración completa con JUCE:

```sh
cmake -S . -B build \
  -DVITADAW_BUILD_APP=ON \
  -DVITADAW_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Resultado: **13/13 tests superados** y aplicación nativa compilada.

La nueva cobertura verifica mediante el `processBlock` portable real:

- proyecto sin WAV, cero, uno y varios buses, incluido un bus vacío;
- equivalencia numérica entre pista directa y pista a bus unity;
- suma de varias pistas en un bus y aislamiento entre buses;
- mute y solo atravesando un bus;
- sample rates y duraciones diferentes, incluido final dentro de subbloque;
- bloques mayores que la capacidad preparada, sin repetición ni salto temporal;
- continuidad de reloj, smoothing y máximos de metering entre subbloques;
- cero allocations durante el callback de routing instrumentado;
- 32 pistas directas y 32 pistas distribuidas entre 8 buses;
- destinos e identidades inválidos, límites de pistas, buses y memoria;
- cambios detenidos, rechazo durante Play y conservación ante fallo de preparación
  o commit;
- reconstrucción estructural sin decodificación WAV;
- lifetime conjunto de recurso, plan y buffers durante una región RT real.

## Sanitizers

- ASan + UBSan: **13/13**, sin diagnósticos.
- UBSan independiente: **13/13**, sin diagnósticos.
- TSan: **13/13**, sin carreras detectadas.
- `git diff --check`: correcto.

## Smoke test

Dispositivo observado:

- salida: Altavoces del MacBook Air;
- sample rate de dispositivo/proyecto: 48.000 Hz;
- buffer: 512 frames;
- canales: 0 entradas / 2 salidas.

Routing probado:

- Track 1 -> Bus A;
- Track 2 -> Bus A;
- Track 3 -> Bus B;
- Track 4 -> Master;
- Bus A -> Master;
- Bus B -> Master.

WAV utilizados:

- `vitadaw-ntrack-330hz-44100-1s.wav`: mono, 44,1 kHz, 1 s;
- `vitadaw-ntrack-440hz-48000-2s.wav`: mono, 48 kHz, 2 s;
- `vitadaw-ntrack-550hz-44100-3s.wav`: mono, 44,1 kHz, 3 s;
- `vitadaw-ntrack-660hz-48000-4s.wav`: mono, 48 kHz, 4 s.

Durante reproducción se observaron los cuatro meters de pista, Bus A, Bus B y
Master activos. En una lectura representativa, las pistas mostraron
aproximadamente 0,177, Bus A 0,345, Bus B 0,177 y Master 0,657, coherente con la
suma configurada. Con solo Track 1 en solo, únicamente Track 1, Bus A y Master
mostraron aproximadamente 0,177; al silenciar esa pista manteniendo solo, todos
los meters quedaron a cero.

Stop volvió a `Stopped | 0.000 / 4.000 s`, Play posterior reinició desde cero y
el final natural quedó en `Stopped | 4.000 / 4.000 s`. La aplicación cerró y
liberó el dispositivo limpiamente. La sesión no dispone de captura acústica; la
presencia, suma y aislamiento de señales se verificaron mediante meters y las
pruebas numéricas offline.

## Riesgos y limitaciones pendientes

- El grafo actual no puede representar ciclos porque solo admite pista a bus o
  master y bus a master; una topología futura deberá añadir detección general de
  ciclos durante la preparación.
- Los buses no tienen gain, pan, mute, solo ni inserts.
- El plan solo cambia con el transporte detenido; no existe hot-swap RT.
- Se usa un buffer estéreo completo por bus; es deliberadamente simple y deberá
  medirse antes de escalar a grafos grandes.
- El procesamiento sigue siendo escalar y el resampling lineal es provisional.
- El metering es peak latest-value y su publicación tiene coste O(pistas+buses)
  por callback.
- No hay limiter ni clamp en master.
