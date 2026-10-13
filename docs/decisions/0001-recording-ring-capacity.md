# 0001 — Capacidad del ring de captura

- **Fecha de registro:** 2026-10-13. La fecha de adopción original no está identificada aquí.
- **Estado:** vigente en 0.8.3-C2.
- **Decisión:** dimensionar el ring SPSC de captura a aproximadamente 2 s de
  audio (`ceil(sampleRate × 2,0)` frames; 96.000 frames a 48 kHz), con
  overflow explícito y fallo seguro si no cabe un callback completo.
- **Motivo:** disponer de un margen finito frente al jitter y los retrasos
  del servicio del writer en el hilo de mensajes, sin fingir que un bloqueo
  arbitrariamente largo es recuperable.
- **Evidencia y límite:** en 0.8.3-B/B2 las pruebas de resistencia y hardware
  caracterizaron el límite del ring y los retrasos reales; se observó un
  retraso de servicio de 643 ms dejando unos 1,36 s de margen. No demuestra
  inmunidad universal; un writer independiente sigue siendo una alternativa
  a reevaluar si las mediciones lo exigen.

**Fuentes:** [0.8.3-B](../audit-recording-endurance-0.8.3B.md) y
[0.8.3-B2](../audit-recording-physical-latency-0.8.3B2.md).
