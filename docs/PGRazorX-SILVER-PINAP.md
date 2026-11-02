# Paquete 2: Silver Pinap 708

Base: 0.1.7-experimental.2 con los cuatro parches del paquete 1 ya aplicados.
Es el port del antiguo parche 0003 de 0.1.6; en la serie nueva corresponde al 0005.

La baya Silver Pinap usa el ID **708** en el selector de captura, la alimentación
del compañero, las preferencias, los backups y las consultas rápidas de stock.
Se renombra su recurso a `berry-708.png` sin cambiar la imagen.

Cambios de producción C++:

- `inventory_host.cpp`: permite que el ajuste 708 llegue al módulo del compañero.
- `items.cpp`: consulta 708, en lugar de 707, en el catálogo rápido de cinco bayas.

El motor ya identifica correctamente 707 como **Golden Nanab** y 708 como
**Silver Pinap**; esas equivalencias del encuentro se conservan. No se altera
la selección de objetos genéricos ni las reglas de eliminación de inventario.
No hay cambios de ABI, esquemas de transporte ni nuevas funciones registradas.
No se incluye Poffin ni otro port pendiente.

Las selecciones antiguas guardadas con 707 no se interpretan como Silver Pinap:
deben elegirse de nuevo con la opción correcta. Los backups con 707 en los
ajustes de bayas se rechazan. Las configuraciones con 708 se conservan al guardar
y volver a cargar.

Validación: compilación Java Android; suite sin dispositivo; compilación y
verificación ELF/exports de `libpgrazor-core.so` para ARM64 y x86_64; compilación
de los harnesses `items`, `buddy_feeding`, `encounter_berries` e `inventory_host`
para ambas arquitecturas. Hay escenarios que distinguen stock 707/708, verifican
la llamada Feed y el paso desde host65 a buddy12.

Los harnesses nativos no se ejecutaron porque no había dispositivo conectado.
No se ha generado un APK ni se ha probado la alimentación dentro del juego.
