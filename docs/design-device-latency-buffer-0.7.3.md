# VitaDAW 0.7.3A — Diseño previo de Device Latency & Buffer Control

> Estado: diseño arquitectónico previo a implementación. Este documento no cambia el comportamiento publicado de VitaDAW 0.7.2 ni describe una capacidad ya disponible.

## Objetivo, alcance y separación de fases

0.7.3A permitirá que la aplicación consulte y cambie de forma controlada el *buffer* del dispositivo, y que presente las latencias que el backend reporta. Su objetivo práctico es que la persona usuaria pueda elegir, entre los valores que el dispositivo exponga, un compromiso entre menor latencia de *software monitoring* y mayor presión de CPU/riesgo de *dropouts*.

No es una corrección de posición de clips. Reducir el buffer reduce el tiempo de ida de la señal de Monitoring cuando el hardware lo permite; no desplaza audio grabado, no mide una latencia física absoluta y no compensa la colocación de Recording. Esas responsabilidades pertenecen respectivamente a 0.7.3B (Recording Placement Compensation) y 0.7.3C (loopback/validación física).

Quedan fuera de 0.7.3A: compensación de grabación manual o automática, calibración loopback, plugin delay compensation, MIDI latency, hardware direct monitoring, selección automática de buffer/modo de baja latencia, buffers por pista, multitrack compensation y cualquier desplazamiento destructivo de WAV.

## Arquitectura actual auditada (0.7.2)

| Responsabilidad | Implementación actual | Hilo / frontera | Implicación para 0.7.3A |
| --- | --- | --- | --- |
| Dueño del dispositivo | `src/vitadaw/platform/juce/JuceAudioDeviceAdapter.{h,cpp}` posee `juce::AudioDeviceManager`, el callback y `RealtimeAudioEngine`. | Control/UI para el manager; callback JUCE para render. | Sólo este adaptador puede leer o cambiar configuración física. |
| Estado de dispositivo de UI | `audio::AudioDeviceStateModel` en `src/vitadaw/audio/AudioDeviceState.{h,cpp}` publica `AudioDeviceState` con nombre, rate, buffer y canales. | Modelo de aplicación, explícitamente no-RT. | Se extenderá con un read model ambiental específico; no con `ProjectState`. |
| Configuración certificada | `JuceAudioDeviceAdapter::{certifiedDeviceSampleRate_, certifiedDeviceBufferSize_, certifiedDevice_}`. | Control. | Rate, buffer y puntero forman una certificación única tras `reprepareForCurrentDevice()`. |
| Detección de divergencia | `pollDeviceLifecycle()` compara rate, buffer, puntero y planes preparados con la certificación. | Temporizador de `JuceApplication::timerCallback()`, antes de `DawApplication::synchroniseTransport()`. | Un cambio externo de buffer ya causa un reprepare controlado; 0.7.3A debe reutilizarlo. |
| Quiescencia | `detachAudioCallback(true)` / `attachAudioCallback(true)` encapsulan `removeAudioCallback` / `addAudioCallback` y preservan transport. | Control; nunca RT. | El cambio solicitado debe atravesar esta misma frontera. |
| Preparación de Core | `reprepareForCurrentDevice()` construye planes/temporal context, retira referencias, configura motor, restaura checkpoint y vuelve a certificar. | Callback retirado. | No se puede confirmar el nuevo buffer antes de que este paso termine. |
| Capacidad de bloque y staging | `prepareProjectPlan()` toma `getCurrentBufferSizeSamples()` y lo pasa a `prepareProcessingPlanFromSources`; `RealtimeAudioEngine::configure()` instala `plan.blockCapacity` y llama a `prepareMonitoringStaging(blockCapacity_)`. | Preparación no-RT. | Cada buffer efectivo exige un plan y staging nuevos antes de volver a RT. |
| Smoothers y estado temporal | `RealtimeAudioEngine::configure()` y `restoreTemporalCheckpoint()` reconfiguran reloj/smoothers; `restoreInputMonitoringAfterControlledReconfigure()` repone Monitoring sólo tras éxito controlado. | Callback retirado. | El cambio preserva estado lógico, pero reinicia la transición de ganancia de Monitoring de forma segura. |
| Entrada y demandas | `prepareRecording()` y `prepareInputMonitoring()` abren entradas mediante `AudioDeviceManager` con rollback; `monitoringInputDemand_` es del adaptador. | Control. | Buffer no debe abrir/cerrar input ni alterar quién demanda input. |
| Render RT | `audioDeviceIOCallbackWithContext()` sólo adapta bloques y llama a `RealtimeAudioEngine::processBlock()`. Capture consume input raw antes de staging/limpieza; Monitoring mezcla desde staging preasignado. | RT. | No habrá consultas JUCE, cambio de device, realloc ni lógica de transacción aquí. |

La ruta actual de un cambio externo de buffer es: `pollDeviceLifecycle()` detecta que el valor real ya no coincide con `certifiedDeviceBufferSize_`, retira el callback, llama a `reprepareForCurrentDevice()`, vuelve a adjuntar el callback si estaba registrado y publica estado. Si el reprepare falla, limpia la demanda de Monitoring, invalida la certificación y deja el dispositivo en error. Esta base es la que debe usar el cambio solicitado por UI; no se introduce un segundo lifecycle.

`DawApplication::handle()` es la entrada serializada del Command System. Ya protege una sesión cuya `activeRecording_.session != 0` mediante `recordingBusy()`, lo cual abarca Recording preparado, capturando o finalizando. La UI (`MainWindow`) no tiene acceso directo al manager.

## APIs JUCE y semántica de buffers

El dispositivo operativo se obtiene con `juce::AudioDeviceManager::getCurrentAudioDevice()`. Las APIs que deben usarse en el adaptador, nunca en RT, son:

| Necesidad | API JUCE | Unidad y validez |
| --- | --- | --- |
| Buffer efectivo actual | `juce::AudioIODevice::getCurrentBufferSizeSamples()` | `int` en samples/frames de callback. JUCE advierte que fuera de un dispositivo abierto el valor no es significativo. |
| Rate efectivo actual | `juce::AudioIODevice::getCurrentSampleRate()` | `double` Hz; igualmente sólo significativo con dispositivo abierto. |
| Valores ofrecidos | `juce::AudioIODevice::getAvailableBufferSizes()` | `juce::Array<int>` de samples/frames. Debe consultarse de nuevo para el device/rate efectivos. |
| Setup actual | `juce::AudioDeviceManager::getAudioDeviceSetup()` | Copia de `AudioDeviceSetup`, que conserva selección de device, rate, buffer y masks de I/O. |
| Aplicar setup | `juce::AudioDeviceManager::setAudioDeviceSetup(setup, false)` | Devuelve `juce::String`: vacío significa que JUCE no informó error; no sustituye la lectura/validación posterior. |
| Input activo | `getActiveInputChannels()` y `getInputChannelNames()` | Masks/nombres; `activeFoundationInputChannels()` ya los limita a la fundación mono/estéreo. |
| Output activo | `getActiveOutputChannels()` y `getOutputChannelNames()` | Masks/nombres; no se altera al solicitar otro buffer. |

JUCE usa la lista para escoger valores cercanos al abrir un dispositivo. Además, la implementación CoreAudio vendorizada genera una lista de potencias de dos dentro del rango hardware, mientras que otros backends pueden informar una lista distinta o cambiarla con rate, driver o dispositivo. Por ello `64/128/256/512` son candidatos habituales para el smoke, no un contrato ni una lista codificada por VitaDAW.

### Política de lista soportada

1. Con dispositivo operativo y certificado, obtener la lista fresca, descartar valores no positivos, ordenar y eliminar duplicados en la capa de control.
2. La futura UI sólo ofrece selección directa si la lista resultante no está vacía. Un backend sin lista utilizable muestra el buffer efectivo como lectura y deshabilita el selector: no se inventan tamaños comunes.
3. La solicitud debe ser un miembro exacto de la lista fresca. Tras aplicar setup, se lee el buffer real: si es válido, esa configuración efectiva se prepara, certifica y muestra aunque el backend haya elegido otro valor permitido. Si ese efectivo no está en la lista fresca, se presenta como estado físico confirmado separado de las opciones seleccionables; no se añade fraudulentamente a la lista ni se muestra el solicitado. La UI mantiene las opciones reportadas disponibles para la siguiente solicitud.
4. La lista se reemplaza después de cada certificación correcta y se invalida en cierre, error, parada o cambio de dispositivo. Así no queda una lista de otro backend/rate.

## Read model ambiental propuesto

0.7.3A añadirá un modelo de lectura efímero, de control/UI, por ejemplo `DeviceLatencyReadModel`. Puede vivir junto a `AudioDeviceStateModel` en `audio/` y ser expuesto por `IAudioEngineControl`/`DawApplication`; el adaptador conserva la única fuente de verdad física. No contiene punteros JUCE ni referencias a arrays que puedan invalidarse.

| Campo | Semántica |
| --- | --- |
| `configurationAvailable` | Sólo `true` para device abierto, operativo y certificado. |
| `confirmedBufferFrames` | Buffer efectivo certificado, no el último solicitado. |
| `supportedBufferFrames` | Copia control-side de la lista fresca normalizada; vacía significa «no disponible», no «sin buffers». |
| `sampleRateHz` | Rate efectivo certificado. |
| `reportedInputLatencyFrames` / `reportedOutputLatencyFrames` | Opcionales, en la unidad natural del backend. |
| `inputLatencyMs` / `outputLatencyMs` | Opcionales, derivados sólo para presentación. |
| `estimatedMonitoringLatencyMs` | Opcional y explícitamente estimada; véase la sección de latencia. |
| `lastBufferChangeError` | Diagnóstico control-side de la última petición fallida, sin sustituir la configuración confirmada. |

No hace falta un estado `pending` persistente en 0.7.3A: `SetAudioBufferSize` es síncrono en la entrada serializada de control y devuelve su resultado final. Mientras dure, la UI puede deshabilitar su selector localmente; el read model sigue mostrando el último setup certificado. Si en el futuro se hace asíncrono, un estado de progreso deberá diseñarse sin publicar una configuración aún no certificada.

El read model no entra en `ProjectState`, `ProjectSession`, historial, Undo/Redo, documento, dirty state ni archivo `.vitadaw`. El acceso y las actualizaciones se hacen en el mismo hilo serial de aplicación/control que actualiza el `AudioDeviceStateModel`; no se lee en el callback.

## Command System y flujo de cambio propuesto

Se añadirá un comando discreto mínimo `commands::SetAudioBufferSize { frames }`. No hace falta un Command de consulta: `pollDeviceLifecycle()` y el read model ya actualizan la observación. El flujo es:

```text
UI selecciona un valor real
  -> ICommandDispatcher
  -> DawApplication::handle(SetAudioBufferSize)
  -> valida lifecycle, recording y read model
  -> IAudioEngineControl::setAudioBufferSize(frames) [control-side síncrono]
  -> JuceAudioDeviceAdapter: detach -> setAudioDeviceSetup -> leer efectivo
  -> reprepareForCurrentDevice -> attach -> certificar/publicar
  -> CommandResult confirmado + DeviceLatencyReadModel actualizado
```

El método del puerto puede devolver un resultado estructurado de control con `success`, configuración efectiva y diagnóstico; `DawApplication` lo traduce a `CommandResult`. No se encola como comando RT: configurar hardware puede bloquear, abrir/cerrar el device y asignar preparación. La UI nunca llama a `AudioDeviceManager` ni a `JuceAudioDeviceAdapter` directamente.

### Política durante Recording

La política de 0.7.3A es **rechazar**, no diferir, cualquier `SetAudioBufferSize` si `recordingBusy()` es verdadero. Incluye preflight, captura y finalización. Se devuelve un diagnóstico claro («Recording is capturing or finalizing; change the audio buffer before Record»), no se toca hardware, no se modifica el read model confirmado ni se deja una petición pendiente.

Esta decisión coincide con la arquitectura actual: Capture/writer están preparados contra el rate y plan actuales, y los cambios de input ya emplean una transacción delicada. No hay requisito funcional que justifique introducir una discontinuidad de callback durante Recording. Antes de iniciar Record y después de su terminalización, la selección vuelve a estar permitida.

## Transacción controlada y rollback

El adaptador reutilizará el patrón de `prepareRecording()` y de `restoreMonitoringPreflight()`, no un camino nuevo:

1. Confirmar que hay device operativo, que rate/buffer/certificación son válidos y que `frames` pertenece a la lista fresca. Guardar `previousSetup = getAudioDeviceSetup()`, el `TemporalCheckpoint`, el valor certificado anterior y `callbackWasRegistered_`.
2. Ejecutar `detachAudioCallback(true)`, que deja el callback quiescente y conserva el transport lógico.
3. Copiar `previousSetup`, cambiar exclusivamente `bufferSize` y ejecutar `setAudioDeviceSetup(candidate, false)`. Se conservan device type, nombres, rate y masks de input/output.
4. Leer `getCurrentAudioDevice()`, `getCurrentSampleRate()` y `getCurrentBufferSizeSamples()`. Rechazar sólo si el device/rate/buffer efectivo no son válidos; un backend que aplique otro tamaño válido se acepta con ese valor real como configuración certificada.
5. Llamar a `reprepareForCurrentDevice()`. Esto deriva de nuevo `blockCapacity`, prepara el staging de Monitoring, reinstala el plan y contexto temporal, restaura checkpoint y certifica device/rate/buffer. El staging no-RT es obligatorio para certificar: un fallo de reserva/preparación aborta la transacción y entra en rollback; no se degrada silenciosamente a una ruta `limited`.
6. Si el callback estaba registrado, ejecutar `attachAudioCallback(true)`; luego `refreshState()` y reconstruir el `DeviceLatencyReadModel` desde la configuración efectiva certificada. Sólo aquí se devuelve `accepted`.

No se prometen samples de salida durante el detach/attach: puede haber una interrupción física breve. Sí se preservan ProjectState e intención temporal si la transacción llega a éxito.

### Rollback exacto

Cualquier error de los pasos 3–6 (incluido que JUCE no deje un buffer real válido) inicia rollback con el callback aún retirado:

1. Conservar antes de mutar un checkpoint efectivo: tipo y contexto de device seleccionado (nombres input/output de `AudioDeviceSetup`), rate certificado, buffer certificado y máscaras activas de input/output. Llamar a `setAudioDeviceSetup(previousSetup, false)`.
2. Volver a leer la configuración efectiva. Sólo es rollback correcto si el rate y buffer coinciden con el checkpoint y las máscaras activas y el contexto seleccionado de input/output también coinciden. El éxito de `setAudioDeviceSetup()` no sustituye estas comprobaciones.
3. Ejecutar `reprepareForCurrentDevice()` contra el device restaurado y `restoreTemporalCheckpoint(checkpoint)` siguiendo la disciplina existente.
4. Restaurar la intención de Monitoring que existía antes; el reprepare controlado llama a `restoreInputMonitoringAfterControlledReconfigure()` y vuelve a preparar staging/smoother para el buffer restaurado. No abre ni cierra rutas de input como efecto del cambio de buffer.
5. Volver a adjuntar sólo si antes había callback, refrescar estado/read model y devolver rechazo con el error original más cualquier error de rollback.

Si rollback se certifica, UI continúa mostrando el buffer anterior efectivo, Playback/Monitoring recuperan el estado lógico previo y no se confirma 128 en un ejemplo 512 → 128 fallido. Si el backend acepta 128 pero deja 256, se certifica y muestra 256 y Core/staging se preparan para 256, aunque 256 no aparezca como opción seleccionable. Si no se puede restaurar exactamente —incluido un rate, layout o contexto de device negociado distinto— se aplica la política de fallo real de 0.7.2: invalidar certificación y datos de latencia/lista, limpiar demanda de Monitoring, forzar Monitoring OFF y publicar error de device. No se intenta auto-reactivarlo. Recording no puede estar activo por la política anterior.

## Playback y Monitoring durante reconfiguración

### Playback

En una reconfiguración controlada correcta puede existir una pausa física al retirar/adjuntar callback. El `TemporalCheckpoint` y el uso de `detachAudioCallback(true)`/`attachAudioCallback(true)` preservan la coherencia del transport lógico: no cambian ProjectState, historial ni posición por una reconfiguración solicitada. Un fallo con rollback correcto vuelve a ese mismo estado lógico; un rollback imposible sigue el estado seguro de device error ya existente.

### Monitoring

Un cambio de buffer no es un cambio de intención: no activa Monitoring si estaba OFF, ni lo desactiva si estaba ON y la transacción tiene éxito. La demanda `monitoringInputDemand_` y la ruta física ya válida se conservan; el reprepare controlado recompone `blockCapacity`, staging y smoother, y restaura la intención de Monitoring sólo sobre hardware certificado. La reanudación puede reiniciar la rampa de ganancia; es una transición segura, no una autoactivación.

En cambio, una pérdida/stop/error real del dispositivo conserva la política de 0.7.2: Monitoring se fuerza OFF, se limpia la demanda y no hay auto-reactivación. Esta distinción evita interpretar un fallo de rollback como un reconfigure exitoso.

Record no activa Monitoring; Stop Record no lo desactiva; Play, Stop, Seek y Loop tampoco cambian su estado. El cambio de buffer respeta esa política manual.

## Latencia reportada y métrica estimada

### Input y output reportados

Las fuentes son `juce::AudioIODevice::getInputLatencyInSamples()` y `getOutputLatencyInSamples()`, ambas `int` en samples/frames del dispositivo:

- Input es el retraso entre la llegada de audio a la tarjeta y la entrega del bloque al callback.
- Output es el retraso entre que el callback recibe un bloque y que ese audio se reproduce.

Se consultan únicamente en control, después de una certificación correcta, y se refrescan con device, rate, buffer o layout. La cifra depende del backend y puede depender del buffer/rate/driver. JUCE define la semántica de los extremos, pero no normaliza universalmente qué parte de sus colas internas/hardware cada backend incluye; por eso no se afirma una precisión física ni se extrapola a un round-trip medido.

Un valor negativo, un device no operativo o una dirección sin canales activos se representa como `Unknown`. Para una dirección activa, un valor cero no se convierte arbitrariamente en desconocido: es el valor que el backend reportó, aunque la UI debe seguir llamándolo «Reported». Las frames son la autoridad interna; los milisegundos son derivados de presentación:

```text
milliseconds = frames / effectiveSampleRateHz * 1000
```

Sólo se calcula si frames no son negativos, rate es finito y mayor que cero, y el resultado es finito. Si falla una condición se muestra `Unknown`; no se guarda ms como tiempo autoritativo ni se usan para clock/Recording.

### `Estimated Monitoring Latency`

Cuando input y output reportados son conocidos, sus direcciones están activas y el rate/configuración están certificados, la UI puede mostrar:

```text
Estimated Monitoring Latency =
  (reportedInputLatencyFrames + reportedOutputLatencyFrames)
  / effectiveSampleRateHz * 1000
```

La etiqueta debe incluir **Estimated** (por ejemplo, `~ 10.7 ms (Estimated)`), no «round-trip» ni «latencia física medida». VitaDAW no añade una cola de audio con frames cuantificables a esa ruta: el staging es una copia dentro del mismo callback y el smoother es de ganancia, no un delay. Tampoco se suma `currentBufferSizeSamples` por separado, pues los contratos/reportes de backend pueden incorporarlo ya; añadirlo sin una garantía común provocaría doble conteo.

Si falta cualquiera de ambos reportes, se muestran individualmente los que existan y `Estimated Monitoring Latency: Unknown`. En la UI conceptual:

```text
Audio Buffer:       [ 512 samples v ]
Input Latency:      256 samples / 5.33 ms (Reported)
Output Latency:     256 samples / 5.33 ms (Reported)
Monitoring Latency: ~10.67 ms (Estimated)
```

Debe acompañarse de una nota breve: menor buffer suele reducir la estimación pero aumenta la carga y posibilidad de dropouts. No hay ajuste automático al arrancar, al activar Monitoring ni al grabar.

## Frontera RT y lifecycle

Las consultas de `AudioIODevice`, normalización de arrays, conversión a texto, cálculos de read model, `setAudioDeviceSetup`, detach/attach y preparación ocurren exclusivamente fuera de RT. El callback continúa usando sólo punteros del bloque actual, configuración ya preparada, `blockCapacity` y estados RT preasignados.

0.7.3A no añade al callback llamadas de device, allocations/reallocations, locks, waits, logging, strings, filesystem ni acceso a la UI. La comprobación de `numFrames > blockCapacity` y la degradación de Monitoring ya diseñada en 0.7.2 siguen siendo una protección por bloque; un cambio correcto prepara una capacidad compatible antes de reanudarlo.

## Persistencia y defaults seguros

La infraestructura actual no contiene preferencias de aplicación/device. Por ello 0.7.3A mantiene el buffer elegido como estado **efímero de la sesión y del dispositivo**: no forma parte de ProjectState ni se persiste en el proyecto. No se construirá un sistema de preferencias sólo para esta fase.

Una fase posterior podrá decidir una preferencia por backend/dispositivo, sólo tras validar de nuevo el valor contra la lista disponible al abrirlo. Nunca debe convertirse en estado musical/documental, ni restaurarse cambiando silenciosamente a 64, ni realizar auto-tuning. El default de la plataforma se mantiene hasta que la persona usuaria seleccione explícitamente un valor.

## Plan de validación automatizada futuro

La implementación deberá ampliar el harness JUCE hardware-free y las pruebas de lifecycle, sin usar mocks que eludan el manager. Como mínimo:

1. lectura del buffer certificado actual;
2. lista soportada normalizada y actualizada;
3. cambio 512 → 256;
4. cambio 256 → 128;
5. solicitud no incluida en lista sin tocar device;
6. error de `setAudioDeviceSetup` con rollback certificado;
7. valor efectivo distinto al pedido con rollback;
8. `blockCapacity` efectivo llega al Core;
9. staging de Monitoring se repara para la nueva capacidad;
10. Monitoring ON se preserva tras éxito, y OFF sigue OFF;
11. fallo/rollback imposible deja estado seguro y sin datos stale;
12. cambio durante Recording preparado/capturando/finalizando se rechaza sin alterar hardware, writer, ring ni lifecycle;
13. Playback conserva checkpoint/estado lógico tras éxito y rollback;
14. input latency se refresca al cambiar configuración;
15. output latency se refresca al cambiar configuración;
16. cambio de sample rate recalcula ms desde frames sin convertir ms en estado;
17. cambio/pérdida de device invalida lista, buffer y latencias anteriores;
18. backend con latencia desconocida/negativa o dirección inactiva publica `Unknown`, sin estimación falsa;
19. instrumentación confirma cero allocations y cero operaciones device en RT, incluida la ruta de staging/alias de 0.7.2;
20. regresiones focalizadas garantizan que Monitoring 0.7.2 y Recording/Recovery 0.7.1 permanecen intactos.
21. un buffer efectivo válido pero ausente de la lista sigue observable sin contaminar las opciones seleccionables;
22. rollback detecta rate o máscaras I/O efectivos divergentes y entra en `device error` seguro;
23. fallo determinista de staging en reprepare aborta la transacción, y combinado con rollback incoherente no deja read model ni Monitoring stale.

## Plan de smoke físico futuro

El smoke no se ejecuta como parte de este diseño. Con el hardware actual se prueban los tamaños relevantes que aparezcan realmente en la lista del dispositivo; como matriz principal, si están disponibles: 64, 128, 256, 512 y 1024 samples. Si el backend también ofrece 32 o 2048, se soportan y pueden cubrirse como extremos adicionales. Para cada valor disponible se verifica:

- Monitoring parado;
- Playback;
- Playback + Monitoring;
- Recording + Monitoring;
- segunda grabación;
- finalización, WAV y reproducción;
- clicks, dropouts, estabilidad y latencia percibida.

Se anota el primer valor estable práctico; no se inventan resultados ni se asume que 64 es compatible. Antes de cada intento se confirma la configuración efectiva que el read model publica, no sólo la opción pulsada.

## Riesgos y mitigaciones

| Riesgo | Mitigación de diseño |
| --- | --- |
| Driver acepta una petición pero usa otro buffer | Leer el efectivo, certificarlo y preparar Core/staging para ese valor; nunca mostrar el solicitado como confirmado. |
| Lista distinta tras rate/device/backend | Consultar y reemplazar lista sólo tras certificación; nunca hardcodear opciones. |
| Core y hardware quedan con capacidades distintas | Callback quiescente y `reprepareForCurrentDevice()` antes de attach/confirmación. |
| Fallo a mitad de reconfigure | Setup anterior + checkpoint + reprepare de rollback; si no certifica, device error seguro. |
| Discontinuidad audible | Se acepta una pausa física breve; se preserva coherencia lógica, no se intenta esconderla con RT. |
| Cambio durante Recording daña captura | Rechazo temprano y sin side effects de hardware. |
| Latencia UI engañosa | Separar Reported/Estimated/Unknown, no sumar buffer dos veces ni afirmar round-trip medido. |
| Datos ambientales persisten en proyecto | Read model efímero, fuera de ProjectState/historial/archivo. |
| Regresión de seguridad RT | Ninguna API JUCE/configuración/read model en callback; pruebas de instrumentación obligatorias. |

## Pasos de implementación posteriores a aprobación

1. Definir tipos de read model/resultados de control y extender `IAudioEngineControl` con consulta y cambio control-side.
2. Implementar en `JuceAudioDeviceAdapter` la construcción/invalidez del read model desde configuración certificada y las lecturas de latencia no-RT.
3. Implementar la transacción exacta y rollback sobre los helpers actuales, sin duplicar lifecycle.
4. Añadir `SetAudioBufferSize` al Command System y el rechazo temprano durante `recordingBusy()`; mantenerlo no persistente y fuera de Undo/Redo.
5. Conectar una UI mínima que consuma sólo el read model y despache Command.
6. Añadir el plan completo de tests automatizados y, sólo después, ejecutar el smoke físico con la lista real del hardware.

## Decisiones que requieren aprobación

No hay decisiones bloqueantes sin resolver para 0.7.3A. Este diseño adopta las preferencias ya indicadas: rechazo explícito durante Recording, buffer efímero en ausencia de infraestructura de preferencias, selector sólo con lista fiable del backend y confirmación estricta del valor efectivo. Cualquier persistencia por dispositivo o calibración física queda deliberadamente fuera de esta fase.
