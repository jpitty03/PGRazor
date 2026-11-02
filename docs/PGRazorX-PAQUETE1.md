> Nota histórica del port. Para el funcionamiento actual, consultar [la guía de mejoras](UNRELEASED-IMPROVEMENTS.md).

> Migración 0.1.8: este documento conserva el historial del primer paquete. Quick Spawn personalizado queda excluido; se usa la base oficial. La serie activa y sus estados están en PLAN.md del kit.

# PGRazorX: paquete 1 sobre 0.1.7-experimental.2

Este paquete integra únicamente cambios de Java, interfaz, recursos y empaquetado:

1. Feeds PoGoCoordinates con sesión mediante Discord.
2. Tema PGRazorX, overlays, mapa, favoritos, historial con países y previsualización de rutas.
3. Ubicación sintética y caminata/Autowalk en segundo plano mientras el proceso y la conexión con el adaptador sigan activos. La activación de VGP se conserva, pero sus acciones siguen sujetas a los permisos de primer plano.

Se conservan el valor inicial de 50 paradas y el máximo dinámico de autowalk de
0.1.7; dejar la cantidad vacía selecciona todas las ubicaciones disponibles.
La previsualización usa la misma cantidad seleccionada. No se impone el antiguo
límite de 100 paradas del parche de 0.1.6.

Events conserva el recolector nativo original de 0.1.7 y recibe sólo el tema de
la interfaz. Las In-game Routes mantienen su parada al salir del primer plano:
continuar caminando/Autowalk no equivale a reanudar automáticamente una Route
del juego. La pantalla de inventario conserva la solicitud y actualización
asíncrona del catálogo que usa el runtime nativo nuevo.

No se modifican C++, los esquemas de transporte ni el registro de funciones del
motor. Los parches anteriores de Silver Pinap/Poffin, IV/PvP, Rocket completo,
portarán por separado. Las funciones que ya trae 0.1.7 permanecen disponibles;
este paquete no añade esas ampliaciones de PGRazorX.

Validación: compilación de todas las clases Android y suite sin dispositivo.
La entrega del paquete no incluye un APK ni una prueba dentro del juego.
