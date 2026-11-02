> Nota histórica del port. Para el funcionamiento actual, consultar [la guía de mejoras](UNRELEASED-IMPROVEMENTS.md).

# Paquete 4: lanzamiento aleatorio

Base: 0.1.7-experimental.2 con paquetes 1 a 3. Parche 0007 (original 0010).

El interruptor **Random rating** elige Nice, Great o Excellent para cada lanzamiento
válido. El motor C++ elige la calificación y el radio; el impacto emparejado reutiliza
exactamente ambos. Conserva las comprobaciones de objeto, valor, hilo Unity y permiso
temporal del motor original. Un cambio de política, Off o vencimiento del permiso
descarta la pareja pendiente.

El ajuste aleatorio se guarda como `throwRandom`; `grade` sigue guardando la opción
fija 0..3. Al apagar Random se recupera esa opción. Compatible con las preferencias
de curva, AR y Always hit, cuyos propios permisos siguen siendo necesarios.

El campo existente `throwGrade` transporta el modo 4 para Random, conservando los
modos 0..3 anteriores. No cambia el tamaño de las tablas ABI ni el esquema binario:
Java, el host nativo y la política de throws deben proceder de este mismo paquete.
La telemetría informa la calificación concreta 1..3, nunca 4. No se añade un hook.

Pruebas Java: persistencia y reinicio, ajustes anteriores, copias de seguridad,
composición, Off y snapshot binario real. Harnesses Android: política de throws,
reutilización submit/hit, las tres bandas y conexión del host quickball a la política.
Compilar esos harnesses no equivale a ejecutarlos; falta probarlos en Android y
comprobar los lanzamientos dentro del juego.
