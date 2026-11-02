# Rendimiento general — parche 0014

Revisión del trabajo repetido de la serie 0000–0013. Se entrega separada para
conservar los parches funcionales y permitir comparar el prefijo anterior.
El protocolo sigue en 14 y no cambia las preferencias del usuario.

| Trabajo | Antes | Después |
|---|---|---|
| Panel de Ajustes oculto | refresco completo cada 100 ms | sin refresco periódico oculto |
| Repintado periódico de overlays | 100 ms | 250 ms; acciones directas inmediatas |
| Movimiento/joystick | 100 ms | 100 ms |
| Pasos del gesto Rocket | 16 ms | 16 ms |
| Búsquedas de pantallas Rocket en mapa sin flujo | cada 400 ms | acopladas a la búsqueda de parada cada 2 s |
| Auto Rocket durante combate | varias pantallas cada 400 ms | resultado cada 1 s; combate/minijuego original conservado |
| Egg Hatch sin sesión activa | cuatro búsquedas globales cada 250 ms | cada 1 s; al detectar sesión vuelve a 250 ms |
| Curación sin objeto pendiente | escaneo de bolsa cada 1,5 s | cada 5 s; con petición pendiente, 1,5 s |
| Revalidación de Pokémon antes de usar objeto | segunda enumeración completa | GetPokemon(id), con enumeración como respaldo si no existe el contrato |
| Etiquetas IV/PvP estables | hasta 96 por pasada de 250 ms | revisión por turnos de 12; primer segundo, filas nuevas y restauración sin aplazar |
| Caché PvP | 48 tablas | 192 tablas, unas 2,3 MiB en total |
| Posición/cámara en log rutinario | aproximadamente 1 s | 5 s; cambios/error conservados |
| Heartbeats diagnósticos Rocket/eclosión | 3–5 s | 30 s; cambios de estado y errores inmediatos |

La auditoría de filas estables recorre 96 en ocho pasadas (unos 2 s). La
inicialización, los registros todavía no pintados y la desactivación/restauración
conservan prioridad. Esto mantiene la corrección de las primeras filas sin
volver a revisar todos los textos y formatos a máxima frecuencia durante toda
la sesión. Al apagarse una función no se espera al turno para restaurarla.

Se evita crear un SimpleDateFormat por cada línea de log; la instancia se usa
bajo el bloqueo existente y actualiza su zona horaria periódicamente. El log
sigue vaciándose por línea para conservar los diagnósticos de cierre. Los estados
de los módulos se procesan antes de limitar muestras rutinarias de cámara.

La caché PvP tiene un límite fijo. Sus pruebas comparan los mismos resultados
con el oráculo original, cubren expulsiones y confirman 100 consultas calientes
sin reconstruir tablas. La ampliación consume aproximadamente 1,7 MiB adicionales
a cambio de reducir recálculos al recorrer especies/formas en el inventario.

## Qué se ha medido y qué queda pendiente

Se verifican los presupuestos de trabajo y frecuencias con pruebas de las
funciones que utiliza producción, además de las regresiones existentes. Los
conteos de búsquedas/auditorías son límites de código, no mediciones de ahorro
de batería, FPS o temperatura. No se ha perfilado el móvil ni se modifican los
gráficos/FPS del juego. Hay que comparar en el teléfono la misma zona y opciones
con la nueva APK. Si persiste la carga, los registros permitirán acotar el módulo
activo que la produce. Las funciones activas siguen consumiendo trabajo.
