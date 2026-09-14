# Validación de VitaDAW 0.4.1 — Timeline Editing Operations

Fecha: 14 de septiembre de 2026.

## Operaciones e invariantes

Se incorporan `MoveClip`, `DuplicateClip`, `SplitClip`, `TrimClipLeft`,
`TrimClipRight` y `DeleteClip`. Todos los comandos identifican el clip mediante
un `ClipId` globalmente único; ningún recorrido público depende de índices de
vector. El layout, TrackId, SourceId, routing, mixer, sends e inserts permanecen
inalterados salvo los campos temporales propios de cada operación.

Split conserva el ID izquierdo y crea un ID derecho monotónico. El offset
derecho se calcula con el ratio preciso `sourceRate/projectRate`, sin redondear
a source frames. Trim Left avanza inicio y source offset; Trim Right solo reduce
duración. Repetir exactamente el límite vigente es un no-op válido. Los puntos
que producirían longitud cero o quedan fuera del clip se rechazan.

La prueba de dos frames detectó una ida/vuelta 44,1/48 kHz que podía exceder el
límite matemático por unas ULP. La validación de bounds admite únicamente una
tolerancia proporcional a `epsilon`; el renderer mantiene su comprobación
exacta y nunca indexa PCM fuera del rango.

Overlaps se suman y gaps producen silencio. La duración se vuelve a derivar del
máximo final preciso; eliminar el último clip devuelve duración cero, pero no
elimina su `AudioSource`.

## Resultados y transacción

El dominio devuelve `ClipEditResult` con estados explícitos para éxito, clip
inexistente, posición inválida, longitud cero, exceso de bounds de fuente y
capacidad. `CommandResult` distingue además transporte no detenido y fallo de
preparación.

Cada comando trabaja sobre un `ProjectState` candidato. Solo tras validar,
reutilizar los owners PCM por SourceId, compilar clips/índice/plan y alcanzar
quiescencia se ejecuta el commit `noexcept`. Los fallos de preparación o commit
conservan modelo, plan y caché activos. Move, Duplicate, Split, Trim y Delete no
llaman al decoder.

## Query y preparación para Undo

La consulta portable mínima queda formada por `findClip`, `clipsForTrack`,
`clipsIntersectingRange` y `projectContentDuration`. Las operaciones contienen
los datos necesarios para que un futuro Undo capture:

- Move: inicio anterior;
- Trim Left: inicio, offset y duración anteriores;
- Trim Right: duración anterior;
- Split: clip original e ID derecho creado;
- Duplicate: ID creado;
- Delete: clip completo y TrackId.

No existe UndoManager y los contadores de IDs nunca retroceden.

## Pruebas automatizadas

`TimelineEditingOperationsTests` y la integración de comandos cubren:

- move posterior, retorno a cero, gaps, overlaps y overflow;
- duplicate con nuevo ClipId y SourceId/PCM compartidos;
- split equivalente al original a 48/48, 44,1/48 y 48/44,1 kHz, mono y estéreo;
- offsets fraccionales, clips de dos frames y conservación de duración precisa;
- trim izquierdo y derecho, no-ops, longitud cero y límites inválidos;
- delete de uno, último de pista y último de proyecto conservando la fuente;
- queries por rango e identidad global;
- preparación fallida, edición durante Play y ausencia de estado parcial;
- una fuente con 100 clips y un solo `PreparedSource`;
- conservación de inserts, sends y routing;
- capacidades de subbloque 64/128/256/512/1024 y callbacks mayores;
- límite de 4.096 clips por pista;
- `processBlock` real sin allocations.

Build completo y core-only: **21/21 suites superadas**.

## Sanitizers

- ASan + UBSan: **21/21**, sin diagnósticos.
- UBSan independiente: **21/21**, sin diagnósticos.
- TSan: **21/21**, sin carreras detectadas.
- `git diff --check`: sin errores.

## Smoke test nativo

Dispositivo observado:

- Altavoces del MacBook Air;
- 48.000 Hz;
- buffer de 512 frames;
- 0 entradas / 2 salidas.

Se importó `vitadaw-ntrack-660hz-48000-4s.wav` (48 kHz, mono, 4 s) como
Source/Clip 1. Mediante los controles provisionales se ejecutó:

1. Duplicate #1 a 2 s;
2. Move #1 a 0,5 s;
3. Split #1 a 1,5 s;
4. Trim Left #3 a 2 s;
5. Trim Right #3 a 3 s;
6. Delete #2.

Cada operación fue aceptada y la duración resultante quedó en 3 s. Durante
Play se observaron señal en Snare, Drum/Music, Plate/Parallel/Room y Master,
confirmando que inserts, routing, sends y meters sobrevivieron al rebuild. Una
edición enviada durante Play fue rechazada explícitamente. El final natural
quedó en `Stopped | 3.000 / 3.000 s`; Replay funcionó y Stop volvió a
`0.000 / 3.000 s`. Mute/Solo siguieron operativos y el proceso cerró
limpiamente.

## Límites

- Los botones de edición usan IDs/posiciones fijos y existen solo para smoke;
  no constituyen un timeline.
- No hay Undo/Redo, persistencia ni edición durante Play.
- No hay waveform, fades, crossfades ni clip gain.
- El render continúa escalar y el resampling lineal es provisional.
- No hay recording, automation, MIDI, stretch/pitch ni hosting externo.
