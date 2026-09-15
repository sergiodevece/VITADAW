# VitaDAW 0.5.1 — Seek & Transport Navigation

## Contrato validado

El transporte portable formaliza `Stopped`, `Playing` y `Paused` sobre un único
`ProjectFramePosition`. Play arranca/continúa desde la posición actual; Pause
conserva posición; el primer Stop conserva posición y el segundo Stop vuelve a
cero. El final natural queda en `contentEnd` y Play posterior reinicia a cero.

Seek está permitido en Stopped y Paused dentro de `[0, contentEnd]`. Durante
Playing se rechaza explícitamente. El comando usa el ring SPSC y gate de
lifecycle existentes y entra en vigor antes del render del siguiente callback.
No existe cursor UI paralelo.

Pause produce silencio sin ejecutar processors ni avanzar su estado. Seek y
Stop resetean Track/Bus/Master processors y dry-delay de bypass y marcan la
siguiente llamada como discontinuidad. El final natural conserva la política
de reset existente. Smoothers y meters mantienen su política provisional.

## UI

La barra incluye Play, Pause y Stop. El label muestra estado, `mm:ss.xxx`,
duración, `Frames: N` y sample rate lógico. Click en ruler despacha Seek; Space
alterna Play/Pause y Home/End navegan por comandos. Esto permite seleccionar un
clip, colocar el playhead estrictamente dentro y ejecutar Split, luego Undo.

## Pruebas

Las suites cubren transiciones Play/Pause/Stop, doble Stop sin timeout, Seek en
Stopped/Paused, rechazo en Playing en la capa de aplicación, límites
-1/0/1/end/end+1, navegación inicio/final, replay natural, 300 seeks repetidos,
random access del renderer y preservación de Undo/StateToken. También se prueba
el orden SPSC `Pause → Seek` publicado antes de un mismo callback: ambos comandos
se consumen en orden y el bloque resulta Paused en el destino, sin depender de
un snapshot RT atrasado. Se mantienen las pruebas de clips solapados, sample
rates mixtos, inserts, gran sesión, RT allocations y lifecycle.

Matriz final:

- build completo Debug: **26/26** suites;
- build core-only Debug: **25/25** suites;
- ASan + UBSan: **25/25**, sin diagnósticos;
- UBSan independiente: **25/25**, sin diagnósticos;
- TSan: **25/25**, sin carreras detectadas;
- `git diff --check`: limpio.

## Smoke test nativo

Se cargó `build/timeline-0.5.0-smoke.vitadaw` en la aplicación 0.5.1. Dispositivo
observado: **Altavoces del MacBook Air**, 48.000 Hz, buffer de 512 frames,
0 entradas y 2 salidas. El proyecto cargado duró 6,666 s.

Se comprobó click real en ruler de 0 a 3,600 s (172.800 project frames), Play,
Pause conservando posición, Play posterior desde esa posición, primer Stop
conservando 5,451 s y segundo Stop volviendo a 0. También se colocó el playhead
en 1,800 s, se seleccionó el clip, Split creó una entrada de Undo y Undo restauró
el modelo. La reproducción llegó al final natural y reinició desde cero.

La carga se validó mediante el selector nativo. Save conservó el playhead hasta
que macOS solicitó permiso persistente para la carpeta Documentos; el permiso se
denegó deliberadamente en esta sesión, por lo que no se afirma escritura manual
del archivo. La ruta Save/Load, el reset a Stopped/0 tras Load y la ausencia de
mutación de history/dirty por transporte sí quedan cubiertos por las suites
portables. Se verificaron cierre limpio y arranque limpio de una segunda
instancia.

## Límites

- Seek durante Playing se rechaza en `DawApplication`; no se implementa una
  discontinuidad dentro de un callback ya iniciado. El motor conserva el orden
  FIFO para permitir `Pause → Seek` antes del siguiente callback.
- No hay drag continuo del playhead: click-to-seek emite un solo comando.
- No hay tempo, bars/beats, loop, metronome, waveform ni recording.
