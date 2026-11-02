# Revivir y curar — parche 0013 de Experimental.3

En **Ajustes → Catch & summary** hay dos opciones independientes, apagadas por defecto:

- **Auto revive & heal**: detecta Pokémon debilitados, usa Máximo Revivir antes que
  Revivir y conserva el Pokémon en la cola hasta completar el 100% de sus PS.
  La opción del 50% no es necesaria para terminar de curar a un revivido.
- **Heal Pokemon below 50% HP**: incorpora a la cola a los Pokémon vivos con
  `2 × PS actuales < PS máximos`. Justo el 50% no inicia curación. Una vez
  incorporados, también se curan hasta el 100%, no solo hasta superar la mitad.

Se elige la poción más pequeña con existencias que complete los PS; si ninguna
alcanza, la de mayor capacidad disponible. Si faltan pociones o revivires, espera.
Cada Pokémon se valida por identificador exacto de 64 bits, PS máximos y actuales;
se excluyen huevos y datos no válidos.

Solo actúa con el mapa activo/visible y la actividad en primer plano. Pausa al
abrir Ajustes. Usa `ItemBagImpl.UseItemOnPokemon`, sin hooks nuevos ni peticiones
de red propias. Se permite un único objeto en curso y se espera un cambio de PS;
no se considera que devolver una promesa confirme el efecto del objeto.

La espera máxima es de 10 segundos. Sin cambio de PS, aplaza ese Pokémon un
minuto. Una excepción administrada pausa el módulo hasta apagarlo/encenderlo.
Cambiar los modos o la generación de sesión reinicia su cola; una pausa temporal
del mapa o del panel conserva la curación en curso. No fuerza curación durante
combates ni dentro de un lobby.

Los estados se muestran bajo las opciones. Logs: `auto-heal`, `auto-heal-request`,
`auto-heal-confirmed` (cambio de PS observado) y `auto-heal-timeout`.

Protocolo de transporte 14: añade `autoHealConfig` como texto 14; conserva 43
booleanos y 62 acciones. Los parches anteriores conservan sus hashes y protocolos
en sus respectivos prefijos. Se requiere recompilar Java y C++ juntos.

Validación local: política C++ real en el host, controles Java reales,
persistencia, límite estricto y PS impares, selección de objetos, Revivir normal
al 50% seguido de curación completa con el segundo modo apagado, espera única,
confirmación, timeout y aplazamiento. Java Android y ARM64/x86_64 compilados.
La ejecución de los contratos y el consumo real de objetos requieren prueba móvil.
