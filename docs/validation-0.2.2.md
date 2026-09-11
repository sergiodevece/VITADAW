# Validación de VitaDAW 0.2.2 — Bus-to-Bus Routing DAG

Fecha: 11 de septiembre de 2026.

## Alcance

Este incremento permite una única salida principal Bus→Bus o Bus→Master,
manteniendo pistas y buses en un DAG portable preparado fuera de RT. No añade
sends, múltiples destinos, inserts, plugins, PDC, feedback, routing durante
Play, PFL/AFL, solo-safe ni multicanal.

## Modelo y preparación

`RoutingState` es la única fuente editable. `OutputDestination` representa
Master o un `BusId`, y cada `AudioBus` lo conserva junto a su identidad, nombre
y `BusMixState`. Los buses nuevos terminan en Master por defecto.

El compilador ordena los buses densos por `BusId`, resuelve cada destino a índice
y valida todos los componentes mediante DFS de tres colores. Una arista hacia
un bus gris produce un error con la cadena de IDs implicada. Kahn genera después
un orden topológico determinista, usando el menor `BusId` para desempatar.
Tracks preceden a los buses y Master es el último paso único.

Un acumulador estéreo completo por bus y otro para Master se reservan junto al
plan. En cada subbloque se limpian, las pistas se renderizan una vez, y cada bus
se procesa y distribuye una vez en orden topológico. Todos comparten la misma
tabla temporal y ningún nodo avanza el reloj.

## Solo, Mute e identidad

La resolución portable diferencia contenido seleccionado upstream de buses
abiertos únicamente para transporte downstream. Bus Solo selecciona todas las
fuentes que alcanzan ese bus; Track Solo selecciona solo la pista. Después se
abren los buses necesarios hasta Master sin volver a expandir upstream, por lo
que las ramas hermanas permanecen excluidas. Varios Solo forman una unión.

Mute sigue siendo local y prevalece. Los meters son post-procesamiento local:
un bus upstream puede mostrar señal aunque otro bus downstream la silencie.
Parámetros, masks y meters se asocian por `BusId`; índice denso, orden
topológico y `bufferIndex` son conceptos independientes.

## Tests automatizados

Build core-only y build completo JUCE: **15/15 suites superadas**.

La suite nueva `BusToBusRoutingTests` y las extensiones de las suites existentes
cubren:

- Bus→Master, cadenas, ramas independientes/convergentes y buses vacíos;
- almacenamiento de especificación permutado e identidad densa estable;
- autoruta, ciclos de dos y tres buses, ciclo desconectado y destino inexistente;
- un paso por pista/bus, dependencias topológicas y Master final;
- suma, unity, gain acumulado, balance acumulado y ausencia de duplicación;
- smoothing una vez por muestra con fan-in y subbloques;
- Track Solo a través de varios buses sin abrir ramas hermanas;
- Bus Solo hijo/padre, ambos, unión con Track Solo y pista directa a Master;
- Mute intermedio/downstream y meters correspondientes;
- sample rates mixtos, final natural, replay y ausencia de residuos;
- 32 pistas y ocho buses distribuidos en cadenas;
- rechazo transaccional de ciclos y routing durante Play;
- lifetime del propietario de una cadena bajo una región RT activa.

Comandos de validación:

```sh
cmake -S . -B build -DVITADAW_BUILD_APP=ON -DVITADAW_BUILD_TESTS=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure

cmake -S . -B build-core -DVITADAW_BUILD_APP=OFF -DVITADAW_BUILD_TESTS=ON
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

## Sanitizers

- ASan + UBSan: **15/15**, sin diagnósticos.
- UBSan independiente: **15/15**, sin diagnósticos.
- TSan: **15/15**, sin carreras detectadas.
- `git diff --check`: correcto.

## Smoke test nativo

Dispositivo observado:

- Altavoces del MacBook Air;
- 48.000 Hz;
- buffer de 512 frames;
- 0 entradas / 2 salidas.

Sesión provisional:

- Track 1: `vitadaw-ntrack-440hz-48000-2s.wav` → Bus A;
- Track 2: `vitadaw-ntrack-330hz-44100-1s.wav` → Bus A;
- Track 3: `vitadaw-ntrack-550hz-44100-3s.wav` → Bus B;
- Track 4: `vitadaw-ntrack-660hz-48000-4s.wav` → Master;
- Bus A → Bus B → Master.

En unity se observaron aproximadamente Bus A 0,345, Bus B 0,504 y Master
0,657. Bus A a −6 dB produjo aproximadamente Bus A 0,173, Bus B 0,338 y
Master 0,501. Track 1 Solo atravesó ambos buses y dejó fuera Track 2, Track 3 y
la pista directa; buses y Master marcaron aproximadamente 0,089 con Bus A a
−6 dB. Bus A Solo seleccionó sus dos pistas y mantuvo Bus B como transporte.
Bus A y Bus B en Solo seleccionaron la unión upstream de Bus B y dejaron fuera
la pista directa a Master.

Con ambos buses en Solo, Mute de Bus A anuló su rama mientras Bus B conservó la
contribución directa de Track 3; Bus A marcó cero y Bus B/Master aproximadamente
0,177. Stop dejó `Stopped | 0.000 / 4.000 s`; replay comenzó desde cero y el
final natural quedó en `Stopped | 4.000 / 4.000 s`. La aplicación y el dispositivo
se cerraron limpiamente. La presencia de señal se comprobó mediante meters y
pruebas numéricas; la automatización de esta sesión no realiza captura acústica.

## Riesgos y límites pendientes

- Un nodo solo tiene salida principal; sends requerirán aristas adicionales y
  audibilidad por camino, sin invalidar el DAG actual.
- No existe PDC. El orden preparado admite futuras latencias y operaciones, pero
  todavía no calcula delays compensatorios.
- Se reserva un buffer completo por bus y el procesamiento es escalar.
- El máximo preparado sigue siendo 256 pistas y 64 buses.
- Mute/Solo continúan siendo discretos; no hay solo-safe, PFL ni AFL.
- No hay limiter ni clamp en Master.
