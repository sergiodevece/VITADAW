# Validación de VitaDAW 0.2.1 — Bus Mixer Controls

Fecha: 11 de septiembre de 2026.

## Alcance

Este incremento añade gain, balance estéreo, mute y solo a los buses de 0.2.0,
con smoothing y peak metering post-procesamiento. No añade bus a bus, sends,
inserts, plugins, PDC, feedback, automatización ni routing durante Play.

## BusMixState y flujo de señal

Cada `AudioBus`, identificado mediante su `BusId` estable, contiene un
`BusMixState` portable:

- gain entre −100 y +12 dB, con −100 dB como silencio exacto;
- balance estéreo entre −1 y +1;
- mute;
- solo.

NaN, infinito y valores fuera de rango se rechazan antes de modificar el modelo
o publicar el parámetro. La conversión dB→lineal y los coeficientes de balance
seno/coseno se preparan fuera de RT.

El flujo validado es:

```text
entradas acumuladas
    -> Bus Gain smoothing
    -> Bus Balance smoothing
    -> Bus Mute/Solo
    -> Bus Peak Meter
    -> Master
```

El centro mantiene ambos canales a unity. En los extremos se atenúa hasta cero
el canal opuesto. Gain y balance usan rampas lineales sample-accurate de 5 ms;
mute y solo son discretos. El meter es post-mute/post-Solo y, por tanto, un bus
muteado o excluido publica cero.

## Resolución de Solo

`PreparedAudibilityState` separa selección Solo de caminos necesarios. Usa
máscaras de bits densas preparadas fuera del callback:

- sin solos, todas las pistas y buses permanecen abiertos;
- Track Solo abre esa pista y el bus que la transporta;
- Bus Solo abre el bus y todas sus pistas de entrada;
- varios solos forman la unión lógica de las selecciones;
- una pista directa a Master se excluye ante Bus Solo, salvo que tenga Solo;
- Mute prevalece en el propio nodo.

La máscara acompaña al mismo comando de parámetros que cambia el Solo. El
callback no resuelve `TrackId`/`BusId`, no consulta `ProjectState` y no interpreta
la topología.

## Actualización RT

`SetBusGain`, `SetBusPan`, `SetBusMute` y `SetBusSolo` atraviesan UI → dispatcher
→ `DawApplication` → `IAudioEngineControl`. `RealtimeAudioEngine` resuelve el
`BusId` a índice denso en el productor y publica un `BusMixCommand` en el ring
SPSC preasignado. No se detiene el transporte, no se reconstruye el routing, no
se retira el callback y no se decodifica audio. Una cola llena conserva el estado
anterior en aplicación y motor.

## Tests

Build core-only:

```sh
cmake -S . -B build-core \
  -DVITADAW_BUILD_APP=OFF \
  -DVITADAW_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core -j 4
ctest --test-dir build-core --output-on-failure
```

Resultado: **14/14 tests superados**.

Build completo JUCE:

```sh
cmake -S . -B build \
  -DVITADAW_BUILD_APP=ON \
  -DVITADAW_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Resultado: **14/14 tests superados** y aplicación nativa compilada.

La suite `BusMixerControlsTests` cubre:

- gain unity, atenuación, boost, silencio y valores inválidos;
- balance a izquierda, centro, derecha y posición intermedia con canales
  estéreo desiguales;
- smoothing, retarget y continuidad en bloques 64/128/256/512/1024;
- rampas de 5 ms a 44,1, 48 y 96 kHz;
- callback mayor que la capacidad preparada;
- mute/unmute con varias pistas, bus independiente y pista directa;
- meter post-gain, post-pan, post-mute y post-Solo;
- Track Solo atravesando bus no seleccionado;
- Bus Solo con varias pistas;
- aislamiento de buses y pistas directas durante Solo;
- dos buses en Solo y unión Track Solo + Bus Solo;
- precedencia de Track Mute y Bus Mute;
- pista directa a Master en Solo;
- 32 pistas, 8 buses, pista vacía y sample rates mixtos;
- uso del `processBlock` portable real y ausencia de allocations RT conservada
  por la instrumentación anterior.

Los tests de comandos verifican además publicación durante Play, rechazo
transaccional, persistencia del modelo y ausencia de preparación estructural.

## Sanitizers

- ASan + UBSan: **14/14**, sin diagnósticos.
- UBSan independiente: **14/14**, sin diagnósticos.
- TSan: **14/14**, sin carreras detectadas.
- `git diff --check`: correcto.

## Smoke test

Dispositivo:

- Altavoces del MacBook Air;
- 48.000 Hz;
- buffer de 512 frames;
- 0 entradas / 2 salidas.

WAV y routing:

- Track 1, `vitadaw-ntrack-330hz-44100-1s.wav` → Bus A;
- Track 2, `vitadaw-ntrack-440hz-48000-2s.wav` → Bus A;
- Track 3, `vitadaw-ntrack-550hz-44100-3s.wav` → Bus B;
- Track 4, `vitadaw-ntrack-660hz-48000-4s.wav` → Master.

Con buses unity se observaron aproximadamente Bus A 0,345, Bus B 0,177 y
Master 0,657. Bus A a −6 dB bajó aproximadamente a 0,173. Balance totalmente a
la izquierda produjo Bus A L≈0,173/R=0 y Master asimétrico. Mute de Bus A dejó
su meter a cero mientras Bus B, la pista directa y el transporte continuaron.

Track 1 Solo mantuvo abierto Bus A aunque el bus no estuviera en Solo. Bus A
Solo hizo audibles Track 1 y Track 2 y excluyó Bus B y la pista directa. Bus A y
Bus B en Solo produjeron la unión de ambos buses; la pista directa siguió
excluida. Mute de Bus A sobre esa selección dejó únicamente Bus B audible.

Stop dejó `Stopped | 0.000 / 4.000 s`; Play reinició desde cero y el final
natural quedó en `Stopped | 4.000 / 4.000 s`. El cierre fue limpio. No existe
captura acústica en la sesión, por lo que la señal se confirmó mediante meters y
pruebas numéricas offline.

## Limitaciones

- Los buses continúan dirigidos exclusivamente a Master.
- No existen sends, inserts, plugins, PDC, feedback ni automatización.
- Solo no incluye solo-safe, PFL, AFL ni SIP avanzado.
- Mute/Solo son discretos y pueden producir discontinuidad si cambian con señal.
- El balance es balance estéreo, no dual-pan, width ni MS.
- El ring sigue siendo SPSC con un único productor.
- El procesamiento sigue siendo escalar y el metering peak latest-value.
- No hay limiter ni clamp en Master.
