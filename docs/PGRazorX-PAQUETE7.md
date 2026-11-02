> Nota histórica del port. Para el funcionamiento actual, consultar [la guía de mejoras](UNRELEASED-IMPROVEMENTS.md).

# Paquete 7: saltar eclosión, incluida la pantalla inicial

Port del parche original 0008 de 0.1.6. Requiere los paquetes 1 a 6 completos.
Se distribuye como `0010-Saltar-eclosion-nativo.patch`.

En la sección de huevos aparecen **Skip hatch** y **Keep shiny & 100% open**.
Skip hatch está apagado por defecto. La protección de shiny/100% está activada.
Ambas opciones se guardan y participan en la copia de ajustes.

La aparición de `EggHatchGuiController` inicia la detección aunque todavía no
existan `PokemonInfoEggHatch` ni un cambio observable del estado activo. Tras
700 ms y dos observaciones consecutivas se permite el toque inicial. Cuando
aparece la animación se intenta `SkipEggHatchAnimation()` una sola vez, después
de 600 ms desde el inicio de la sesión. Si funciona se esperan tres segundos
antes de recurrir a los toques. Como respaldo se activa FastForward y se permiten
como máximo 20 toques, separados al menos 900 ms, en una sesión de hasta 45 s.

Una GUI que permanece visible o cambia de objeto mientras sigue visible no
constituye una nueva apertura. Se conservan los flancos de visibilidad al
suspender/reactivar la función y se descartan las nuevas aperturas de GUI durante
los tres segundos posteriores al cierre. La animación visible aporta su identidad
a la sesión aunque aparezca después de la pantalla inicial.

Al aparecer la ficha del Pokémon se revocan los toques. Se conserva si es shiny,
15/15/15 o sus datos no se pueden leer con certeza. Si se desactiva esa protección,
también puede cerrarse un shiny/100%. La ficha ordinaria se cierra después de un
segundo mediante su botón de salida. Una ficha diferente o abierta manualmente
fuera de una eclosión no se cierra. Si el contrato de cierre no está disponible,
se deja la ficha abierta.

La lógica de juego es C++, sobre el planificador Unity existente, sin un segundo
hook ni un host JavaScript de eclosión. Las clases, métodos y campos se resuelven
por metadatos con tipos comprobados. Las referencias administradas se mantienen
mediante GC handles, y los recorridos están limitados a 4096 objetos por clase.
Dos excepciones consecutivas del salto nativo desactivan esa ayuda y mantienen
el respaldo. Los errores generales de lectura suspenden las acciones 15 s.

Los toques se envían desde Java en el hilo principal y caducan a los 200 ms desde
la autorización nativa. Se descartan al apagar la función, perder permisos o
primer plano, recibir una revocación o expirar. No se envían con ajustes o menú
rápido abiertos. Los pares DOWN/UP cancelados terminan en CANCEL. Se marcan como
sintéticos para que no activen caminar/teletransportar mediante toque en el mapa.
La observación y la entrega del toque son asíncronas; deben verificarse en el juego.

Java y C++ usan conjuntamente el transporte binario versión 5: añade los booleanos
eggHatchAllowed/eggKeepSpecial y la acción 59 eggHatchUpdate; conserva los índices
anteriores y los desplazamientos del resto del snapshot. No mezclar binarios de
paquetes diferentes.

## Preparación y prueba

Desde la carpeta principal:

```powershell
.\aplicar-parches-017.ps1
.\compilar-apk-017.ps1
```

Los scripts seleccionan por defecto el paquete 7 y preparan
`PGRazorX-v0.1.7-experimental.2-local-paquete7`. Conservan las carpetas anteriores,
las rutas locales de herramientas y la clave existente. El APK resultante queda
en `pgrazor/build/release/PGRazor.apk` de esa carpeta.

En el móvil comprobar: pantalla inicial anterior a la animación, varios huevos
seguidos, cierre de un Pokémon ordinario, conservación de shiny/100%, Off y vuelta
desde segundo plano sin toques residuales. Abrir una ficha manualmente también
debe dejarla intacta. La opción de incubación permanece independiente.

Los diagnósticos usan el canal `egg-hatch`: metadata-ready/unavailable, session,
native-skip/fallback, scan, closed, kept, timeout y paused. Permiten distinguir una
GUI inicial, animación, ficha de Pokémon y fallo de contrato de metadatos.

## Validación

La política C++ se ejecuta en el host mediante WebAssembly. Las pruebas de Control
comprueban ajustes persistentes, caducidad y cancelación. También se compilan
Java Android, las bibliotecas ARM64/x86_64 y los harnesses Android de política,
transporte y planificador. El informe de distribución enumera los resultados.
Los harnesses Android no se han ejecutado en un dispositivo y esta función
todavía requiere validación dentro del juego. No se ha instalado ninguna APK.


## Corrección tras el cierre reportado al desactivar Keep shiny & 100%

El usuario reportó que el juego se cerraba inmediatamente al apagar el interruptor.
No hay dispositivo ADB conectado ni traza de ese cierre: no se considera confirmada
su causa ni resuelto en el móvil hasta repetir la prueba con esta revisión.

Se suspenden las acciones nativas mientras los ajustes están abiertos y durante
un segundo después de cambiar la protección o cerrar los ajustes. Se cancelan los
toques pendientes; cambiar la protección cancela la sesión anterior y requiere una
nueva escena antes de actuar. Las identidades GC se conservan durante la suspensión
para no confundir la escena anterior con una nueva.

Antes de cerrar una ficha se exige STATE_OPEN (2, como el cierre nativo ya existente),
un objeto vivo, datos/lista/índice/Pokémon válidos y un callback de salida no nulo que
coincida en GUI y datos. Tras las lecturas administradas se comprueba de nuevo que
la ficha no cambió y que todavía hay permiso. Si no coincide, se omite el cierre y
se registra `close-skipped`. Se registra `close-before` antes de una llamada válida.

Para volver a probar basta recompilar la carpeta local-paquete7, ya actualizada.
Si vuelve a cerrarse, conectar el móvil por USB con depuración autorizada y ejecutar
`capturar-cierre-017.ps1` desde el kit antes de reproducirlo; guarda el registro en
`build/logs/cierre-017-*.log` hasta detenerlo con Ctrl+C.
