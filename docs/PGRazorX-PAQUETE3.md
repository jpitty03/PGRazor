> Nota histórica del port. Para el funcionamiento actual, consultar [la guía de mejoras](UNRELEASED-IMPROVEMENTS.md).

# Paquete 3: Poffin

Base: 0.1.7-experimental.2 con los paquetes 1 y 2. Parche 0006.

- Alimentación automática del compañero con Poffin (709) o una de las cinco bayas.
- Interruptores exclusivos: activar uno desactiva los demás; todos apagados significa Off.
- Selección guardada y compatible con copias de seguridad.
- Cantidad de Poffin leída por el inventario nativo, separada del catálogo reciclable en Java.
- Poffin excluido de las bayas de captura, de reglas de reciclaje y de la selección nativa
  de objetos reciclables, incluso ante una regla importada o un indicador reciclable incorrecto.

Se conserva el transporte existente (buddyBerry y setItemCatalog), sin nuevas tablas ABI,
hooks ni hosts JavaScript de 0.1.6. Se conservan las comprobaciones de hambre, autorización,
enfriamiento, resultado y progreso del motor nativo de alimentación.

La compilación y las pruebas de host no sustituyen la prueba dentro del juego. Los harnesses
nativos se compilan para Android; no se consideran ejecutados por el mero hecho de compilarlos.
