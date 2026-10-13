# 0002 — Política ante discontinuidades de captura

- **Fecha de registro:** 2026-10-13 (decisión adoptada durante la planificación de 0.8.3-C).
- **Estado:** aprobada como contrato; cobertura técnica todavía incompleta.
- **Decisión:** una discontinuidad de **captura confirmada** debe fallar la
  toma, impedir publicarla como clip válido, conservar el material recuperable
  según el contrato existente y exigir un Record nuevo para reanudar.
- **Motivo:** no presentar como temporalmente continua una interpretación
  que ha sufrido una interrupción. Un xrun genérico, por sí solo, es una
  señal diagnóstica: no siempre prueba pérdida de muestras de entrada.
- **Pendiente:** caracterización y corrección de H-1 (continuidad temporal) y
  H-2 (cambio de sample rate), sin falsos abortos.

**Fuentes:** [auditoría C1](../audit-recording-device-reconfiguration-0.8.3C1.md)
y [tests C2-1](../audit-recording-device-tests-0.8.3C2-1.md).
