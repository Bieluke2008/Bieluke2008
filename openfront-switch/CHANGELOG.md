# Versiones

Cada versión es una etiqueta de git (`vX.Y.Z`) y, a partir de la 0.2.0, una
*release* de GitHub con su `.zip` listo para copiar a la microSD (lo crea el
workflow **OpenFront Switch release** al subir la etiqueta). El número de
versión está en `VERSION` y se ve en la pantalla de inicio de la app.

## 0.3.0

Rendimiento: la simulación (no el dibujado) era lo que iba lento, porque el
motor original es JavaScript interpretado por QuickJS en la CPU de la Switch.

- Las rutinas del motor que se ejecutan por casilla del mapa ahora son C
  nativo (`src/native.c`): accesos al mapa, cola de prioridad y conjuntos de
  casillas, avance de los ataques, fronteras tras cada conquista, grupos de
  fronteras, búsqueda de costa para barcos y colocación de nombres. Hacen
  exactamente lo mismo que el original (misma partida con la misma semilla,
  comprobado con sumas de control): unas 3,4 veces menos tiempo por tick.
- CPU a 1785 MHz (el perfil oficial de «carga» de Nintendo) en lugar de
  1020 MHz, también al abrirlo desde el álbum, sin bajar la GPU; se restaura
  al salir.
- El motor corre en su propio núcleo de la CPU en vez de compartirlo con el
  dibujado.
- Menos trabajo del adaptador por tick (relaciones e iconos de los nombres).


- Interfaz copiada del cliente de PC, con sus medidas, colores, fuentes e
  iconos originales: panel de control, barra de construcciones,
  clasificación, barra lateral, notificaciones, ataques en curso.
- Menú radial completo: construir, mejorar varias veces de golpe, bombas
  atómicas, de hidrógeno y MIRV, barcos, alianzas, borrar, donar.
- Vista previa al construir, radios de alcance y trayectoria de los misiles.
- Tarjeta del jugador, panel de jugador, enviar oro/tropas, emojis, chat
  rápido, menú, confirmación de salida, pantalla de victoria/derrota.
- Mapa dibujado como el renderizador WebGL del juego; vista alternativa
  terreno/países.
- Pantalla «Solo» como la web, con miniaturas de los 135 mapas.
- Controles: pantalla táctil como un ratón; mando con cursor virtual.

## 0.1.0

- Primer port: motor original de OpenFront en QuickJS-ng, interfaz propia
  sencilla. Sin compilado publicado.
