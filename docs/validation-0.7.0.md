# VitaDAW 0.7.0 — Audio Recording Foundation validation

## Resultado final

VitaDAW 0.7.0 incorpora la foundation de grabación y queda preparado para su
publicación. El proyecto, la aplicación nativa y todas las validaciones
indicadas a continuación se ejecutaron sin diagnósticos. El schema de proyecto
permanece en v3 y los documentos recién guardados identifican
`writerAppVersion` como `0.7.0`.

## Arquitectura final de grabación

- `ProjectSession` conserva como estado efímero un único `TrackId` armado: no se
  serializa, no crea historial ni modifica el estado dirty.
- `Record` sólo se admite desde Stopped, con loop desactivado, pista armada y
  proyecto guardado. El arranque sigue abriendo sólo salida (`0 in / 2 out`).
- Durante el preflight no-RT, Record selecciona de forma perezosa el input
  predeterminado del backend actual, conserva la salida elegida y activa input 1
  para mono o inputs 1–2 para estéreo. Sólo después verifica el dispositivo
  abierto, actualiza su rate y prepara el motor.
- El bundle macOS generado contiene `NSMicrophoneUsageDescription` con el texto
  `VitaDAW needs microphone access to record audio.`; la apertura CoreAudio del
  input activa la ruta nativa de permisos.
- `RealtimeCapture` es un ring PCM SPSC portable y preasignado. El callback fija
  el inicio en ProjectFrame y el sample rate real del dispositivo, acepta bloques
  completos, publica el terminal de forma atómica y no hace I/O, locks, strings,
  mutations de proyecto/historial ni allocations.
- `beginRecord` incluye el callback que lo consume. `Stop` cierra la toma antes
  de copiar ese callback. Pérdida, reinicio o cambio de sample rate invalidan la
  generación y no pueden unir capturas incompatibles.
- El source conserva rate y frame count del dispositivo; los rates de proyecto y
  dispositivo pueden diferir sin resampling RT. ProjectFrame sigue siendo la
  autoridad temporal.
- La finalización prepara media, waveform y plan antes de un único commit
  transaccional de ProjectState, caché, historial y locator. Undo/Redo preserva
  las identidades exactas y Redo vuelve a verificar el WAV.

## Modelo final de ownership de media

- El temporal se crea exclusivamente con `O_EXCL`.
- Su descriptor tiene ownership RAII move-only y el writer WAV float de 32 bits
  usa ese descriptor original: no existe cierre/reapertura por pathname.
- La publicación final usa hard-link no reemplazante dentro de
  `<ProjectName> Audio/`; exige un filesystem local del mismo volumen con soporte
  de hard links.
- La identidad estable del media se verifica antes y después de la publicación,
  antes de que ProjectState pueda referenciarlo.
- POSIX no ofrece unlink condicionado atómicamente por identidad. Ningún rollback
  borra un temporal o final por pathname; cuando no se puede demostrar limpieza
  segura, el objeto se retiene y se informa como huérfano ownership-safe.
- 0.7.0 no implementa garbage collection de huérfanos ni recuperación tras crash.

## Validación final

- suite completa: **49/49** passed;
- ASan+UBSan recording: **2/2**;
- TSan recording: **2/2**;
- ASan+UBSan integración JUCE: **10/10**;
- la instrumentación de callback RT observó **0 allocations**;
- `git diff --check`: limpio.

`vitadaw_recording_capture_tests` cubre las fronteras Record/Stop, mono/estéreo,
wrap, overflow all-or-nothing, input nulo/insuficiente, rates 44,1/48 kHz,
particiones, lifecycle, captura concurrente, PCM reproducible y allocations RT.
`vitadaw_recording_application_tests` cubre arm, preflight, commit atómico,
fallos de finalización, huérfanos, Undo/Redo, Save/Load y pérdida previa al
primer callback. La política de selección lazy de input y la integración JUCE
están cubiertas por sus tests de backend y sanitizers.

## Smoke físico — 2026-09-17

En macOS se abrió correctamente el micrófono físico y la ruta de permisos. Se
grabaron tres pistas de audio reales; las tres se reprodujeron correctamente,
Undo y Redo funcionaron, y no se observó ningún error de grabación, playback ni
lifecycle.

## Deuda LOW no bloqueante

Si la solicitud RT de Record se rechaza después de un preflight ya completado,
un `discardRecording(true)` puede retener un huérfano ownership-safe sin incluir
esa información en ese mensaje concreto. No provoca pérdida de datos,
corrupción de proyecto ni bloqueo de grabación; es deuda diagnóstica no
bloqueante para 0.7.0.

## Limitaciones aprobadas de 0.7.0

- Una única pista armada y mapping fijo input 1 / inputs 1–2.
- El proyecto debe guardarse antes de Record.
- Ring de grabación de dos segundos.
- El input permanece abierto tras el primer preflight correcto.
- El filesystem debe estar en el mismo volumen y soportar hard-link; los
  huérfanos ownership-safe no tienen GC.
- Sin monitoring, punch, preroll, take lanes/comping, cycle recording ni ruta
  configurable de grabación.
- Sin agregación multi-dispositivo.
