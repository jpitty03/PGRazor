> Nota histórica del port. Para el funcionamiento actual, consultar [la guía de mejoras](UNRELEASED-IMPROVEMENTS.md).

# Paquete 5: rangos de IV PvP en etiquetas

Base: 0.1.7-experimental.2 con paquetes 1 a 4. Parche 0008, procedente del
original 0005 de 0.1.6. No incluye el siguiente parche de texto bajo el nombre.

Las etiquetas de IV existentes de encuentro e inventario añaden ataque/defensa/PS
y líneas LL, GL y UL por especie, forma y evoluciones. Muestran hasta tres resultados
top 50 por liga; GL/UL sin top 50 muestran el mejor resultado jugable en gris.
Cada línea indica rango, especie, nivel y CP. Un ✓ marca el nivel ya alcanzado
en inventario. Se excluyen las evoluciones que ya superarían el límite de CP al
nivel actual y las especies que no pueden alcanzar ese límite al nivel 50.

Son rangos de producto de estadísticas para las combinaciones de IV, no posiciones
en el metajuego. Se utiliza exactamente el snapshot PvPoke del parche original:
1206 entradas de especies/formas y 2194 asociaciones. Los datos, su procedencia y
la licencia MIT de PvPoke están en `pgrazor/assets/pvp/`. No hay consultas de red.
El empaquetado del núcleo incluye la licencia y la procedencia en el APK, bajo
`assets/pgrazor/native-notices/`.
`tools/pvp_native_data.py --check` verifica la cabecera C++ generada a partir de ellos.

El cálculo y el formato de líneas se ejecutan en C++, con caché fija de 48 tablas
y sin asignaciones dinámicas. Los desempates conservan el orden estable original.
Las pruebas ejecutan ese mismo C++ como WebAssembly y comparan 211 casos con el
algoritmo JS original, incluidos formas, evoluciones, niveles, límites y textos.
El JS de referencia sólo sirve para pruebas; no se incorpora como host del juego.

La forma se busca primero en `Form` y después en `form_`, validando ambos campos.
La reflexión de especie/forma y VerticalWrapMode es opcional y valida propietario,
tipos, enum Int32, métodos y literal Overflow. Si falta, las etiquetas básicas de IV
siguen funcionando. Los registros restauran el desbordamiento vertical original al
desactivar o detener la función y conservan la protección de etiquetas actualizadas
por el juego. No se añade ningún hook ni se modifica el transporte Java/nativo.

La API1 de IV conserva su tabla de 64 bytes, config y sample. La extensión API2
de 72 bytes configura una vez los metadatos opcionales antes de instalar los hooks.
Los buffers internos de pantalla son de 2048 bytes; la telemetría API1 conserva
su primera línea de IV y sus buffers de 256 bytes. No se mezclan módulos compilados
de distintos paquetes.

Verificado: Java Android, bibliotecas C++ ARM64/x86_64 y pruebas de host. Los
harnesses Android de IV/PvP, quickball_host y metadatos compilan para ambas
arquitecturas, pero todavía no se han ejecutado en Android. Pendiente probar la
lectura real de metadatos, la restauración y el espacio de las líneas en el juego.
