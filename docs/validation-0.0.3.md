# Validación local de VitaDAW 0.0.3 — dispositivo observable y robusto

Fecha: 11 de septiembre de 2026  
Plataforma: macOS / CoreAudio

## Dispositivo observado

- Estado: activo
- Dispositivo de salida: `Altavoces del MacBook Air`
- Sample rate: `48000 Hz`
- Tamaño de buffer: `512 frames`
- Canales de entrada disponibles: `0`
- Canales de salida disponibles: `2`

El valor de entrada describe los canales disponibles en el dispositivo de salida
abierto por VitaDAW; la aplicación solicita cero canales de captura.

## Comprobaciones

- estado `closed/active/error`: correcto;
- publicación de nombre, sample rate, buffer y canales: correcta;
- fallo y recuperación sin metadatos obsoletos: correctos;
- build completo y `core-only`: correctos;
- cierre limpio: correcto.
