# 0003 — Reconexión del dispositivo de audio

- **Fecha de registro:** 2026-10-13 (decisión acordada durante la planificación de 0.8.3-C).
- **Estado:** aprobada como dirección; **no implementada** en 0.8.3-C2.
- **Decisión:** intentar recuperar automáticamente el **mismo dispositivo**
  cuando reaparezca, revalidando su identidad, formato y rutas de entrada.
  Nunca sustituir silenciosamente una interfaz por otra ni continuar una
  toma fallida. Una nueva grabación requiere Record explícito; ofrecer
  reintento manual si la recuperación automática falla.
- **Motivo:** recuperar operatividad sin reiniciar VitaDAW, manteniendo la
  seguridad de la fuente y la identidad de la grabación.
- **Pendiente:** defecto R-2 y su aceptación automatizada y física.

**Fuentes:** [auditoría C1](../audit-recording-device-reconfiguration-0.8.3C1.md)
y [tests C2-1](../audit-recording-device-tests-0.8.3C2-1.md).
