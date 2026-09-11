# UI boundary

La futura UI JUCE dependerá de `commands/ICommandDispatcher` y de vistas de
estado de solo lectura. Este módulo no puede incluir cabeceras de `audio` ni
conservar referencias a implementaciones del motor.

