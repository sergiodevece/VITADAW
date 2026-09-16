# VitaDAW 0.6.2 — Loop Foundation validation

## Contrato

El rango documental es `[startTick,endTick)`. Start está incluido, end excluido
y la posición end nunca se renderiza como interior. La única autoridad persistente
son ticks PPQ15360; enabled permanece como estado de sesión. La preparación única
rechaza sin clamp cualquier rango menor de 1024 ticks, 10 ms, un project frame o
un device frame, o incompatible con el dominio racional/ClockFormat certificado.

Play antes de loopStart conserva el preroll. Play en o después de loopEnd vuelve
al start exacto. Content duration no limita el loop, no redefine GoToEnd y no
produce final natural mientras el loop esté activo.

## Implementación

`PreparedLoopView` es un valor puro, inmutable y allocation-free. Sus consultas
`contains`, `distanceToEnd` y `positionAfterWrap` trabajan sobre datos exactos ya
preparados, sin engine ni estado global. `LoopReadModel` publica documento, vista,
enabled y revisión; el commit transaccional impide mezclar candidatos.

`trySetLoopEnabled` usa snapshot confirmado más replay de comandos aceptados. El
orden Play/Stop pendiente decide la admisión sin una máquina de estados adicional.

El checkpoint añade solo tres bits lógicos de obligación musical: loopStart
atravesado, evento aplazado y acento. Rebuild/hardDiscontinuity cancela voces pero
restaura esa obligación. El primer scheduling posterior incluye el intervalo
atravesado, coalesce por muestra y consume el estado una vez.

La longitud certificada del loop es al menos un avance de dispositivo. Esto prueba
que un `advanceDeviceFrame` solo puede envolver una vez; varias vueltas de callback
se realizan mediante subbloques. El módulo conserva todo el overshoot sin epsilon.

## Cobertura dirigida

- fronteras racionales anterior/exacta/posterior y helpers puros;
- mínimo simultáneo de 1024 ticks, 10 ms y un device frame;
- 120/123 BPM, vecinos binary64 y cambios de tempo en los extremos;
- 4/4, 3/4, 7/8 y cambio de métrica dentro del loop;
- 44.1/48/96 kHz e invariancia de partición;
- múltiples wraps, PCM, clicks, posición/fase y loopWrap;
- Play/Stop pendientes seguidos de enable/disable;
- preroll, posición exacta en end y posterior a end;
- wrap al final de callback, rebuild JUCE y un único click de loopStart;
- rollback de documento/vista/revisión/políticas sin publicación parcial;
- checkpoints, reprepare, sample-rate change, rollback y posiciones grandes.

## Rendimiento

El callback no añade allocations, locks ni I/O. El scheduler continúa recorriendo
globalmente un máximo de 8191 segmentos por subbloque; no se ha optimizado ni se
presentan tiempos Debug/sanitizados como benchmark RT profesional.

## Matriz ejecutada

Validación local en macOS, configuración Debug:

- suite completa con JUCE (`build`): 44/44;
- core-only (`build-core`, `VITADAW_BUILD_APP=OFF`): 37/37;
- ASan+UBSan core (`build-asan`): 37/37;
- integración JUCE con ASan+UBSan (`build-asan-juce`, casos `temporal_juce_*`): 6/6;
- UBSan (`build-ubsan`): 37/37;
- TSan (`build-tsan`): 37/37;
- oráculos y regresiones musical/temporales (`build`, filtro `temporal|musical`): 13/13;
- `git diff --check`: limpio.

Todos los sanitizers se ejecutaron con parada inmediata ante diagnóstico. El
runtime ASan de Apple no soporta LeakSanitizer, por lo que se usó
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`; esto no desactiva la detección de
accesos inválidos de AddressSanitizer ni los diagnósticos de UBSan.

## Riesgos residuales

- el recorrido global acotado de hasta 8191 segmentos por subbloque conserva el
  coste previo y sigue pendiente de benchmark RT profesional;
- LeakSanitizer no está disponible en el runtime local de Apple; las rutas
  críticas sí quedan cubiertas por ownership tests, ASan, UBSan y TSan;
- la integración JUCE sanitizer cubre el adaptador y los seis escenarios
  temporales automatizados, no una sesión manual prolongada con hardware real.
