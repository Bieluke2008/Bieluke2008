# Versiones

Cada versión es una etiqueta de git (`vX.Y.Z`) y, a partir de la 0.2.0, una
*release* de GitHub con su `.zip` listo para copiar a la microSD (lo crea el
workflow **OpenFront Switch release** al subir la etiqueta). El número de
versión está en `VERSION` y se ve en la pantalla de inicio de la app.

## 0.9.2

- NSP: el `main.npdm` lleva ahora la marca ForceDebug (bit 19 de las
  capacidades de depuración, en ACI0 y ACID), como el `hbl.json` de
  nx-hbloader. Atmosphère 1.8.0 y posteriores (firmware 19+) no arrancan los
  títulos caseros sin ella, y en firmware 23 eso tiraba al gestor de
  aplicaciones (AM, 0100000000000023) con el error 2128-0007 al abrir
  OpenFront instalado.

## 0.9.1

- NSP: el NACP es ahora el de `nacptool`, el que usan los NSP caseros, en vez
  de uno escrito a mano que activaba la grabación de vídeo y no pedía usuario.
  Con aquel, el sistema (AM) se caía al abrir la aplicación con el error
  2128-0007. Sigue llevando el identificador propio en
  `LocalCommunicationId`, que es lo que necesita el inalámbrico local. Al
  abrir OpenFront instalado, la consola pide elegir usuario, como los juegos.

## 0.9.0

- Multijugador «Inalámbrico local», como los juegos de la Switch: sin router,
  las consolas se conectan directamente. En Multijugador, el selector
  Wi-Fi / Inalámbrico local (o L/R): la anfitriona crea la red local y la
  anuncia; las demás la ven en la lista y entran. Por dentro es la misma
  partida que por Wi-Fi (el servidor de la anfitriona), sobre la red LDN.
- Kit para crear un NSP instalable (openfront-nsp-kit): el programa (NSO), su
  `main.npdm` con un identificador de título propio (01004f50e0fa0000), el
  NACP con ese identificador en `LocalCommunicationId` (lo que exige el
  inalámbrico local), icono y datos, y `crear-nsp-windows.bat` /
  `crear-nsp.sh`, que lo empaquetan con hacBrewPack y las claves de tu
  consola (prod.keys de Lockpick_RCM; no se pueden incluir).
- Con el NSP instalado, el inalámbrico local funciona entre cualquier Switch
  que lo tenga. Con el .nro solo si todas abren el Homebrew Menu desde el
  mismo juego (LDN usa el identificador del juego en ejecución).
- Sin probar en consola: este entorno no tiene Switch ni claves; el modo LDN
  y el NSP se han compilado y revisado (hactool), no ejecutado.

## 0.8.0

- Multijugador entre Switch sin PC: «Crear partida en esta Switch» convierte
  la consola en el servidor de la partida; las demás Switch de la misma red
  Wi-Fi (o del punto de acceso de un móvil) la ven en «Partidas en tu red» y
  entran con un toque. También se puede entrar con la IP que muestra la
  anfitriona y el código.
- La anfitriona juega a la vez que sirve la partida (el servidor va en su
  propio hilo); avisa si otra Switch tiene una versión distinta del port.
- Comprobado con dos copias del port: la segunda encontró la partida por la
  red, las dos jugaron y simularon exactamente igual (186 hashes iguales,
  unos 3 minutos de partida).
- Arreglado: al desactivar en la sala una opción (multiplicador de oro, oro
  inicial, duración máxima, alianzas, bombas en agua, reloj del juicio final,
  tiempo extra) el servidor conservaba el valor anterior.

## 0.7.0

- Multijugador en red local con jugadores en el navegador y otras Switch,
  sobre el servidor de OpenFront arrancado en un PC de tu red
  (`servidor-pc/iniciar-servidor-windows.bat` o `iniciar-servidor.sh`, que
  descargan la versión de OpenFront que usa el port y la arrancan).
- Portada: botón Multijugador (o X). Pantalla para escribir la IP:puerto del
  servidor, «Crear lobby» y «Unirse al lobby» con el código.
- Crear sala: la pantalla Solo con el código de la sala y los jugadores que
  entran; los cambios de opciones se envían al servidor al momento, y
  «Iniciar partida» la arranca para todos.
- Unirse: sala de espera con el mapa, modo, dificultad, tribus y jugadores;
  avisa si el anfitrión no activó «Mapa compacto».
- En la partida: el ritmo lo marca el servidor (sin botón de acelerar), solo
  el creador de la sala puede pausar, y se avisa de pausa, desconexión,
  desincronización o «poniéndose al día».
- La Switch simula la partida igual que la web, turno a turno: comprobado
  contra el motor original durante 7 minutos de partida sin una sola
  diferencia en los hashes que compara el servidor.

## 0.6.0

- Pantalla de inicio como la página principal de openfront.io: fondo del
  mapa, título, tu nombre (se guarda; Y o tocar para cambiarlo), Solo,
  Audio, Controles y Salir. Al salir de una partida vuelves aquí.
- Ventana Solo como la de la web, por secciones y con sus iconos y colores:
  mapa, dificultad (Fácil… Imposible, con calaveras; se apaga si no hay
  naciones), modo (todos contra todos / equipos: 2-7 equipos, cuartetos,
  tríos, dúos, humanos contra naciones), tribus (0-400), naciones (0-400 o
  las del mapa), interruptores (aparición aleatoria, bombas en agua, oro y
  tropas infinitos, construcción instantánea, mapa compacto),
  reloj del juicio final con su ritmo y las tarjetas numéricas de la web
  (duración máxima, multiplicador de oro, oro inicial, duración de las
  alianzas, con 0 = sin alianzas, tiempo extra) con −/+ o teclado. Rejilla «Desactivar unidades».
- En la partida, Menú lista los modificadores activos y las unidades
  desactivadas, como el menú de ajustes de la web.
- Sonido: los efectos originales de OpenFront (construir, bombas y su
  alerta, MIRV, conquistas, alianzas, mensajes, tren, inicio, victoria y
  derrota), sonido ambiente de las ciudades/fábricas al acercar la cámara y
  clic en los menús. Pestaña Audio como la de la web (general, música,
  efectos, alertas, ambiente, interfaz, con «Probar» y «Restablecer»).
- Música opcional: la de OpenFront no es libre y no va incluida; si copias
  sus mp3 a /switch/openfront/music/ suena en el menú y en la partida.
- Los ajustes se guardan en /switch/openfront/openfront-settings.ini.

## 0.5.0

Menos bajones al atacar. Medido en PC en una partida en Europa con el
jugador atacando (en la Switch los tiempos son unas 3-4 veces mayores):

- Clic de ataque: hasta 145 ms → 7 ms. Comprobar si puedes atacar tierra de
  nadie recorría en JS toda la tierra libre en 200 casillas; ahora es C.
  También acelera el menú radial y a los bots.
- Ticks lentos: de 83 por encima de 60 ms (20 por encima de 100 ms) a unos
  4-9 (0-1); tick medio ~28 → ~22 ms. Pasan a C las rutas de barcos por
  agua, la búsqueda de costas, los vecinos de cada jugador, la elección de
  ataque de las naciones y los logaritmos/exponenciales deterministas del
  cálculo de cada casilla conquistada. La partida sigue siendo idéntica a la
  del motor original (comprobado con sumas de control; las matemáticas, bit a
  bit también en ARM).
- Mapa: cada alianza o embargo entre dos jugadores cualesquiera repintaba el
  mapa entero y lo subía a la GPU (19 MB en Europa, cada ~2 s); ahora solo
  se repintan esos dos jugadores. Los cambios se suben en bloques de 64×64
  en vez de un rectángulo que acababa cubriendo casi todo el mapa.

## 0.4.0

- Barra de progreso bajo cada edificio, como en PC (14×3 casillas, de rojo a
  verde): construcción, cuenta atrás al borrarlo y recarga de silos y SAM.
- Número de tropas de cada ataque en su frente, como en PC: azul para tus
  ataques contra jugadores, rojo para los que recibes (salvo de tribus);
  se mueve suavemente con el frente y baja a medida que se gastan tropas.
- Lista de mapas por secciones: arriba «Mundo y continentes» (Mundo, Mundo
  gigante, Europa, África, Norteamérica, Sudamérica, Asia, Oceanía,
  Antártida), luego «Lugares reales» por regiones y al final «Mapas
  ficticios» (ficción, cósmico, arcade, torneo), con las categorías del
  juego original.

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
