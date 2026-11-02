> Nota histórica del port. Para el funcionamiento actual, consultar [la guía de mejoras](UNRELEASED-IMPROVEMENTS.md).

# Paquete 6: IV y mejores rangos PvP bajo el nombre

Base: 0.1.7-experimental.2 con paquetes 1 a 5, incluidas las correcciones LF y
Form/form_ del paquete 5. Parche nuevo 0009, procedente del original 0006 de 0.1.6.

Con **Inventory IV/level** activado, cada celda de la cuadrícula añade bajo el
nombre una línea pequeña como `67% 0/15/15 GL#1`. Muestra porcentaje entero e IV,
y el mejor rango jugable top 50 de cada liga LL/GL/UL para especie/forma o evolución.
Las ligas sin resultado top 50 no aparecen aquí; el panel detallado del paquete 5
conserva su presentación y sus alternativas grises. No modifica el apodo guardado
ni añade otra opción a los ajustes.

La calificación PvP usa el cálculo nativo del paquete 5. El resumen C++ se ejecuta
en el host como WebAssembly y se compara con el algoritmo original en 211 casos.
Se conserva el snapshot, sus licencias en el APK y las comprobaciones de procedencia.

La nueva reflexión opcional busca `PokemonInventoryCellView.nameText`,
`Initialize(Initializer)` y `Initializer.PokemonProto` por nombre y valida tipos,
propietarios, firma, ausencia de byref y el rango ejecutable del método. Sólo ese
Initialize recibe un nuevo hook; si no coincide, se conserva la función IV anterior.
La extensión API3 de IV configura el hook antes de instalarlo; las API1/API2 y el
transporte existente conservan sus contratos.

El hook siempre llama al original una vez. En el hilo Unity, con permiso vigente,
restaura la etiqueta anterior antes de reciclar la celda y captura el nombre recién
actualizado por el juego. Los huevos y entradas sin CP/IV válidos conservan su nombre.
Los registros están limitados a 96 y las referencias temporales se liberan al terminar.
El tamaño base de fuente, ajuste, alineación y desbordamiento horizontal no se cambian:
el resumen usa una etiqueta `<size>` pequeña. Off, expiración o parada restauran
richText y desbordamiento vertical. Un nombre actualizado por el juego se conserva,
incluso si cambia mientras la opción está apagada.

Verificación: Java Android y bibliotecas ARM64/x86_64; batería de host; comparación
del resumen C++ contra el original. Los harnesses Android incluyen reciclaje,
restauración, cancelación/reentrada, límite de 96, metadatos y balance de referencias;
se han compilado, pero todavía no se han ejecutado en Android.

Pendiente comprobar en el juego la resolución real del nuevo método, el espacio bajo
el nombre, desplazamiento/reciclaje de celdas, cambio de nombre y Off. No se ha generado
un APK de este paquete ni se ha instalado nada en el móvil.
