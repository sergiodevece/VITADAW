# VitaDAW 0.7.3 — Device Latency, Placement & Physical Loopback validation

## Estado

La validación automatizada de 0.7.3A, 0.7.3B y 0.7.3C está completada. El smoke
físico de Device Latency & Buffer Control fue correcto. Tras descartar el antiguo
patrón de siete impulsos, el detector MLS quedó validado físicamente con 25/25
trials válidos en cinco buffers sobre una Universal Audio Volt 176.

## Validado automáticamente

- build completo y suite completa: 55/55;
- dispositivo JUCE virtual y `AudioDeviceManager` productivo para buffer
  confirmado, lista normalizada sin duplicados y valores 16–4096;
- `SetAudioBufferSize` por Command System, cambio controlado y valor efectivo
  distinto del solicitado, incluso si no pertenece a la lista seleccionable;
- rollback validado contra checkpoint efectivo completo: contexto de device,
  rate, buffer y máscaras I/O; divergencias de rate/layout y rollback imposible
  entran en `device error` seguro sin read model stale;
- fallo determinista de preparación no-RT del staging aborta la transacción y
  hace rollback; staging failure más rollback incoherente sigue la misma
  política segura;
- reprepare de `blockCapacity` y staging de Monitoring, preservación de
  Monitoring y coherencia de Playback;
- rechazo durante Recording preparado/capturando/finalizando, sin mutar
  hardware, writer, ring, ProjectState, historial ni dirty state;
- latencias de input/output reportadas, conversión a ms, estimación sólo con
  ambas direcciones conocidas y `Unknown` sin valor inventado;
- cambios de sample-rate/device que refrescan o invalidan los datos
  ambientales; ninguna consulta/configuración de dispositivo desde RT;
- regresión de Monitoring 0.7.2 y Recording/Recovery 0.7.1;
- ASan+UBSan focalizado, incluida integración JUCE/media: OK;
- TSan focalizado: OK;
- instrumentación de allocations de `processBlock`: sin allocations; el
  harness de dispositivo verifica además que el callback no consulta ni
  configura el device;
- `git diff --check`: limpio durante la validación.

## Smoke físico

El smoke físico final de 0.7.3A fue correcto con el backend macOS, incluido el
flujo `64 → 1024 → 64`: Monitoring, Playback y Recording permanecieron
operativos después de los fixes transaccionales.

## Recording Placement Compensation 0.7.3B

- snapshot por toma antes de `beginRecord`, con project/device rate, input latency reportada, offset manual y compensation efectiva;
- placement aplicado sólo al `AudioClip::projectStart` durante el commit documental; WAV/PCM no se modifican;
- conversión determinista input device frames → project frames, con rounding nearest y cobertura 44.1 ↔ 48 kHz;
- offset manual efímero de `[-2.0 s, +2.0 s]`, expresado internamente en project frames y fuera de ProjectState/history/dirty state;
- clamp documental en frame 0 con diagnóstico, protección checked del límite superior y degradación automática a cero cuando latency no está disponible o es inválida;
- con latency unavailable, `Effective Recording Compensation` conserva el
  término manual (`-Recording Offset`) y no se muestra como cero;
- clamp a frame 0 publica `unappliedEarlyFrames` como diagnóstico efímero de
  la última toma comprometida, sin mutar ProjectState;
- Undo/Redo y Save/Load reutilizan la posición ya comprometida, sin recalcular con el dispositivo posterior;
- tests focalizados cubren fórmula, rounding, frame 0, límite superior,
  inmutabilidad del snapshot, cambio de buffer/latency entre tomas,
  Undo/Redo y Save/Load;
- la integración JUCE hardware-free graba las mismas muestras con Monitoring
  OFF y ON (gain distinto) y compara los bytes WAV, start y duración: idénticos;
- la integración JUCE hardware-free cubre Take A → cambio real de buffer/
  latency → Take B, Undo/Redo y Save/Load sin recálculo retrospectivo;
- la integración JUCE cubre input latency unavailable con offset manual y
  placement independiente de Monitoring; los límites exactos ±2 s se cubren a
  44.1 y 96 kHz;
- build completo y suite completa 54/54: OK;
- ASan+UBSan focalizados (placement, aplicación de Recording e integración
  JUCE/device): 3/3 OK;
- TSan focalizado sobre los mismos tres binarios: 3/3 OK;
- la ruta nueva queda fuera de `processBlock`; la instrumentación RT existente
  sigue cubriendo el callback de Monitoring sin allocations.

## Loopback & Physical Latency Validation 0.7.3C

- componente diagnóstico `RealtimeLoopbackProbe` separado de Playback, mixer,
  Recording/Capture musical, WAV, recovery y Recording Placement;
- MLS bipolar determinista de 1023 muestras a `-24 dBFS`, generada fuera de RT,
  reproducible y detectable con inversión global de polaridad;
- contador monotónico local de device frames, captura raw mono preasignada y
  análisis por correlación directa fuera de RT;
- guardas antes/después y entre trials, búsqueda máxima de un segundo y ventana
  amplia de ±0.25 s/4096 frames cuando existe una suma reportada;
- detector matched robusto a respuesta LTI, con score normalizado, segundo score
  fuera de ±64 frames, ratio de ambigüedad, SNR sin contaminar el noise floor,
  peak, polaridad, delay entero y clipping observables;
- cinco trials, mínimo tres válidos, mediana autoritativa y publicación de
  min/max/jitter (`max - min`);
- snapshot de device/generación/rate/buffer/par físico/máscaras/ordinales y
  latencias reportadas; suma reportada sólo cuando input y output son conocidos;
- residual signed mostrado como diagnóstico; no se calcula `Suggested Recording
  Offset`, no se modifica el offset manual y no se autoaplica compensación;
- selección explícita de un output y un input, activación temporal exclusiva y
  rollback verificado al setup anterior; rollback incoherente entra en device
  error seguro;
- interlocks verificados para Monitoring, Playback y Recording, cambio de buffer
  rechazado durante la prueba, invalidación por cambio de configuración/device
  loss, cancelación y Recording posterior normal;
- publicación terminal RT mediante transición CAS: Cancel, cambio de
  configuración o device loss no pueden ser sobrescritos por una completion
  tardía del último callback;
- ProjectState, historial, Undo/Redo, dirty state, WAV, media y recovery no se
  modifican;
- tests sintéticos cubren delays `0/16/64/128/513/1200/4096`, ganancias
  `-6/-24/-50 dB`, polaridad invertida, varios SNR, low-pass/FIR/ringing,
  fractional delay, clipping, ambigüedad, tres de cinco válidos, mediana/jitter
  y aritmética checked;
- falsos positivos cubiertos: silencio, ruido puro, seno estable, audio musical
  sintético, patrón binario incorrecto y señal demasiado baja;
- instrumentación RT específica cubre inicio, captura completa, estímulo cruzando
  callbacks, alias total/parcial y overflow: cero allocations detectadas;
- integración hardware-free con `AudioDeviceManager`, adaptador JUCE y device
  virtual productivos, incluidas mediciones a buffers 256 y 512: OK;
- build completo y suite completa: 55/55 OK;
- ASan + UBSan focalizados (Loopback, lifecycle JUCE, Placement, Monitoring,
  Recording y motor RT): 7/7 OK;
- TSan sobre los mismos siete binarios: 7/7 OK.

### Smoke físico 0.7.3C

El primer smoke con Universal Audio Volt 176 a 48 kHz/buffer 512 confirmó señal,
ausencia de clipping, SNR aproximado de 14–34 dB y rollback correcto. El patrón
anterior obtuvo scores 0.15–0.19 y `0/5 NotFound`, evidencia de que los impulsos
digitales ideales no sobrevivían con suficiente similitud a la ruta DAC→ADC.

La repetición final usó macOS/CoreAudio, Output 1 MONITOR L → Input 1, Direct
Monitor OFF e Input Monitoring de VitaDAW OFF. El resultado fue:

| Buffer | Reported RTT | Measured RTT | Residual | Jitter | Trials |
| -----: | -----------: | -----------: | -------: | -----: | -----: |
| 64 | 304 | 412 | +108 | 0 | 5/5 |
| 128 | 432 | 540 | +108 | 0 | 5/5 |
| 256 | 688 | 796 | +108 | 0 | 5/5 |
| 512 | 1200 | 1308 | +108 | 0 | 5/5 |
| 1024 | 2224 | 2332 | +108 | 0 | 5/5 |

Los 25/25 trials fueron válidos, sin clipping, con polaridad normal, score
aproximado `0.848`, SNR aproximado `67–68 dB`, residual constante de `+108`
device frames (`+2.25 ms` a 48 kHz) y jitter de cero. El dispositivo siguió
operativo después de cada restore.

El smoke revalidó además los fixes descubiertos durante la prueba: Record activo
→ segundo Record terminaliza mediante `tryRequestStop()` y permanece Stopped,
sin Play implícito; y la lista física de inputs permanece disponible con
Monitoring OFF, incluida la selección de input para Loopback. Record → Stop,
una segunda grabación y las transiciones Monitoring ON/OFF conservaron el
transporte esperado.

## Limitaciones

- `Estimated Monitoring Latency` es una estimación derivada de las latencias
  que el backend reporta; no es una medición física round-trip.
- No se implementa calibración loopback, selección automática de buffer ni auto-low-latency.
- Windows deberá compilarse y ejecutarse en un toolchain Windows antes de
  considerarse validado empíricamente.
- Aggregate Device, rutas dentro del mismo Aggregate, rutas cross-interface,
  múltiples salidas/entradas, jitter entre interfaces y otras interfaces del
  estudio constituyen validación física ampliada pendiente. No se intentan
  inferir resultados y esta cobertura adicional no bloquea 0.7.3.
