# Arquitectura inicial

## Flujo de control

```text
UI / teclado / MIDI futuro / voz futura
                  |
                  v
          ICommandDispatcher
                  |
                  v
            DawApplication
           /       |       \
  ProjectState  Transport  IAudioEngineControl
                                |
                    cola SPSC acotada lock-free
                                |
                                v
                      callback de audio RT
```

`DawApplication` es la fachada y el único manejador de comandos. La UI recibe
un `ICommandDispatcher`, nunca una referencia al motor de audio. Añadir otra
fuente de entrada significa traducirla al mismo `Command`, no crear otro camino
al motor.

## Módulos

- `application`: composición y coordinación de un comando completo.
- `project`: estado editable y propiedad de pistas.
- `commands`: mensajes de intención, resultados y despacho.
- `audio`: contratos de control no-RT y procesamiento RT.
- `transport`: estado lógico de reproducción y posición.
- `tracks`: pistas de audio y su colección de clips.
- `clips`: referencia a un archivo y región colocada en el timeline.
- `timeline`: posiciones y rangos expresados en frames enteros.
- `ui`: frontera de UI; se implementará con JUCE sin acceder a `audio`.
- `platform/juce`: composición nativa, ventana y adaptador de dispositivo; es
  la única capa que depende de JUCE.

El modelo editable pertenece al hilo de aplicación/UI. El callback de audio no
lee esas estructuras mutables. En una iteración posterior, la aplicación
construirá fuera del hilo RT un snapshot de reproducción inmutable y publicará
su sustitución mediante una cola acotada o intercambio atómico.

`audio/AudioDeviceState` es un modelo portable de observación del dispositivo.
No forma parte del callback: contiene strings y se actualiza únicamente en el
hilo de aplicación. El adaptador convierte desde tipos JUCE y la composición
entrega copias de ese estado a la ventana; la ventana no recibe acceso al motor.

## Reglas del hilo de audio

Dentro de `process()`:

- no reservar ni liberar memoria;
- no usar mutex, esperas, logging, filesystem ni llamadas de UI;
- no abrir/decodificar archivos ni cambiar el dispositivo;
- usar buffers y recursos preparados de antemano;
- ejecutar trabajo con coste acotado y métodos `noexcept`;
- comunicar estado hacia la UI mediante atomics o colas SPSC preasignadas.

La carga y decodificación WAV, la creación de recursos y cualquier I/O se hacen
en hilos no-RT. La vida de los recursos publicados cubre todo callback que pueda
seguir observándolos; no se destruyen desde el hilo RT.

## Reproducción mínima 0.0.4 — First Sound

`LoadAudioFile` recorre `ICommandDispatcher` y `DawApplication` antes de llamar
al puerto neutral `IAudioEngineControl::loadWav`. El adaptador valida la extensión
y usa directamente `juce::WavAudioFormat`; no registra MP3, AIFF, FLAC ni otros
formatos. Decodifica el archivo completo a un `AudioBuffer<float>` fuera del
callback. Solo después de una decodificación válida detiene brevemente el
callback, sustituye el recurso y vuelve a conectarlo. Así ningún callback puede
observar un buffer parcialmente construido ni destruir su recurso.

Play y Stop viajan por una cola SPSC fija de ocho posiciones. El callback vacía
primero la salida, consume esos comandos y lee únicamente el buffer preparado y
un cursor RT. Stop deja el cursor en el frame cero. No se consulta el
`ProjectState` desde audio.

Cuando los sample rates difieren, el callback avanza por el archivo con el ratio
`fileSampleRate / deviceSampleRate` e interpola linealmente entre dos frames. No
se usa el resampler de JUCE en este incremento. La interpolación lineal mantiene
duración y tono correctos, aunque no es la solución de calidad final para un DAW.

La pista única conserva la longitud en frames del archivo fuente y su sample
rate. Aún no existe un sample rate de proyecto ni un timeline gráfico.

### Límites conscientes de este incremento

- La decodificación es síncrona en el hilo de aplicación. No compromete el hilo
  RT, pero un archivo grande puede congelar temporalmente la ventana.
- El archivo completo se conserva en memoria como `float`; el consumo crece con
  duración y número de canales.
- La interpolación lineal es suficiente para validar sample rates distintos,
  pero no tiene la calidad de un resampler final de producción.
- La cola es SPSC porque la UI es el único productor actual. Futuras fuentes de
  comandos necesitarán serialización previa, no productores concurrentes sobre
  esta cola.
- El final natural del archivo detiene el cursor RT, pero aún no publica esa
  transición de vuelta al estado de transporte de la aplicación.

## Evolución hasta 0.1

1. **Completado:** integrar una ventana JUCE vacía y un adaptador de dispositivo,
   manteniendo los tests del núcleo independientes de JUCE.
2. **Completado:** añadir `LoadAudioFile` y decodificar/validar WAV fuera de RT.
3. **Completado:** preparar y publicar un recurso con una pista y un clip.
4. **Completado:** implementar `Play` y `Stop` sobre una cola SPSC acotada; al detener, limpiar
   la salida y confirmar el estado al hilo de aplicación. La inserción en la
   cola devuelve éxito o fallo: nunca se pierde silenciosamente un comando.
5. Añadir pruebas de render offline y smoke tests del dispositivo.

Cada paso debe compilar, pasar pruebas y poder validarse aisladamente antes del
siguiente.

## Estrategia de pruebas

- **Unitarias**: comandos, transiciones del transporte, pistas, clips y límites
  de timeline; sin JUCE ni dispositivo.
- **Integración**: `DawApplication` con un motor falso para verificar que la UI
  solo provoca solicitudes mediante comandos.
- **Audio offline**: cursores deterministas, diferencias de sample rate y casos de
  inicio/fin de clip, sample rates y tamaños de bloque.
- **RT**: instrumentación en builds de desarrollo para detectar allocations y
  locks dentro del callback, más ThreadSanitizer fuera del callback duro.
- **Sistema**: smoke tests manuales por backend/plataforma para abrir, cargar,
  reproducir y detener; no hacer depender CI de hardware de audio.
