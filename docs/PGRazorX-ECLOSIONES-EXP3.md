# Eclosiones — mejoras sin publicar para 0.1.8

En Settings → Auto incubate eggs, Skip hatch animation permite avanzar los avisos
de huevos normales y Daily Adventure Eggs mediante el mismo tap validado.
La observación opcional permite inspeccionar el estado sin ejecutar saltos/cierres.
Keep shiny & 100% conserva abiertas las fichas que cumplan esa protección; con la
protección apagada se intenta cerrar también la ficha final de Pokémon normales.

Los handles administrados conservan los 64 bits y se liberan de forma segura ante
reentrada. Las acciones se revocan al abrir ajustes, detener el módulo o perder las
condiciones de ejecución. El cierre de ficha confirma la desaparición y utiliza
reintentos acotados. Una referencia incierta impide actuar.

El transporte coordinado usa frame 17 y acción 60 para eggHatchUpdate.
Los canales egg-hatch-setting, egg-hatch-java-gate y egg-hatch registran decisiones
del módulo sin demostrar por sí mismos una acción aceptada por el juego.

Pruebas automatizadas: políticas nativas, handles LP64, reentrada, gates Java y
validación del transporte. El contribuidor informa validación móvil en HONOR WIN
con Android 17; esa observación se registra por separado de los fixtures del host.
