# Plan: islas, archipiélagos y continentes a partir de formas geométricas

Plan para rehacer la generación de tierras de YATE (`Lands.*`) y, después, los componentes que dependen de
ella. Recoge el análisis de cuatro generadores de referencia, los criterios acordados, lo que el motor ya
tiene y se reutiliza, los algoritmos elegidos (los de las referencias o uno más eficiente para esta
arquitectura), la API, las fases con sus criterios de aceptación, el catálogo de ejemplos y la validación.

Documento local, fuera de git (`.git/info/exclude`). Lo que quede vigente al cerrar cada fase se traslada a
`docs/status.md`, `docs/todo.md` y `docs/lua_reference.md`, como siempre.

Revisión 2: inventario de lo reutilizable, algoritmos detallados, op fusionado `Lands`, herramientas para
Claude, catálogo de ejemplos con plano infinito y tamaños fijos, y sus tests.

## Estado

**Fase 1 hecha** (2026-10-28). Decisiones tomadas al implementarla, que cambian lo escrito abajo:

* **Relieve:** no se mezclan los perfiles con pesos softmin (el elemento más profundo, p. ej. un `fill`,
  anulaba a los demás). Cada elemento de tierra aplica su perfil a su propia distancia hacia el interior,
  limitada por la de la costa común, y los perfiles se unen con un máximo suave. Solo cuentan los elementos
  que contienen la muestra, lo que mantiene exacto el recorte por bins.
* **Centro de formas ovaladas y radiales:** dentro del círculo inscrito la distancia se funde con la de ese
  círculo; sin ello había una estrella y un pozo falso en el centro.
* **Fondo marino:** un tercer tipo de elemento (`seabed = true`) y un modo `seabed` del op; la fosa del
  margen activo va al pie del talud (plataforma + 0,9 del talud) y a 0,8 de la profundidad abisal.
* **Id tras un corte de agua:** la tierra anterior a un elemento de agua que cubre la muestra deja de ser su
  dueña (un islote en una bahía tiene su propio id).
* **Lagunas:** rol `lagoon` explícito en un elemento de agua.
* **`edit.scale`** sobre un elemento definido por `area` multiplica el área por la escala al cuadrado.
* **`Lands.Island` exige `preset`;** las claves del macro antiguo fallan explicando qué usar.
  `Lands.Archipelago` y `Lands.Continent` siguen en el macro de rejilla hasta las fases 2 y 3.
* Los pasos 1.7 a 1.11 quedaron en un solo commit porque tocaban los mismos ficheros a la vez.

**Fase 2 hecha** (2026-10-28). Decisiones:

* **`scatter` es una clave de elemento**, no un tipo: el resto de claves es la plantilla. Ids `"<id>/<ranura>"`.
* **Acotado:** las ranuras se colocan en orden (no de mayor a menor) para que pedir una más no mueva las
  anteriores; si no cabe, se encoge hasta 3 veces (8 sobre una curva) y si no, se omite y se informa.
* **Infinito:** regla de Wei en una pasada (sobrevive si ningún candidato en conflicto tiene más prioridad),
  sin cadenas voraces, para que dependa solo de las celdas vecinas.
* **`land_fraction`:** se reescalan las áreas de los repartos por fórmula cerrada y se queda la pasada más
  cercana; no hay bisección sobre el muestreo.
* **Margen sin pliegues:** un cuarto modo del op (`margin`) une la tierra con un mínimo suave de medio
  plataforma+talud; solo lo usa el fondo, la costa sigue siendo la unión dura. La fosa de varias islas se une
  como norma 4, y el arco insular usa una fosa propia (`arc` del fondo con cono negativo).
* **Agua repartida** (canales, lagos) no evita la tierra declarada; los repartos posteriores evitan la tierra
  de los anteriores.

**Fase 3 hecha** (2026-10-28). Decisiones:

* **`ridge` es una clave de elemento**, como `scatter`: un elemento de relieve por tramo de cresta (`saddles + 1`
  bandas que se estrechan a 0,4 de su anchura en los extremos, de donde salen los collados) y espolones
  (`capsule`) cada `spacing` (1,2 anchuras por defecto, redondeado al número entero de espolones que cabe), en
  lados alternos y girados de 20 a 40°. Ids `"<id>/crest/<n>"` y `"<id>/spur/<n>"`. El tramo más alto mide
  exactamente `height` sobre la tierra; los demás, de 0,8 a 1.
* **Elementos de relieve** (`relief = true`, un cuarto tipo de operación del op): no cambian la costa; los que
  suben se unen por un máximo suave y los que hunden (una cuenca) por un mínimo suave, y entran desde la costa en
  `blend` metros.
* **Los perfiles de tierra se unen por su máximo**, no por un máximo suave: el suave elevaba dos mesetas iguales
  que se solapan (el atolón subía 15 m sobre su altura `flat`; la meseta de `all_land` no medía 280 m). En el
  filete de una unión suave cada perfil se mide con lo que el suavizado ha crecido la tierra en esa muestra,
  `max(h - d, 0)` (unión dura de la tierra menos la costa): allí su propia distancia es positiva aunque la tierra
  siga, y medido desde ella el perfil dejaba un surco cerrado bajo el acantilado de la costa (0,23 % del
  continente, hasta 87 m). Se probó antes sumar `smooth` (agrandaba cada elemento `smooth` metros) y `smooth/4`
  (dejaba el surco). Fuera de los filetes no cambia nada. Se regeneraron los goldens de isla afectados.
* **Goldens en Debug:** los shaders de Debug se compilan sin optimizar (`-g`), así que una muestra en una
  discontinuidad puede salir distinta que en Release; `lands_mediterranean` se muestrea en una rejilla
  desplazada (±49152 m a 384 m) por eso.
* **Las bandas** unen sus segmentos con un mínimo suave de un cuarto de su anchura: sin él había un pliegue
  donde cambia el segmento más cercano.
* **Presets de tierra** en `Lands.Preset`/`Lands.Continent`: composiciones de la tabla de abajo con un `turn`
  sacado de la semilla (taklamakan no gira: sus cordilleras siguen los bordes). La tierra baja es una meseta de
  0,08 de `height` que sube en `size * 0.01` (lo que tarda en desaparecer el acantilado de la costa) con
  `cliff_height = 0.05 height`: con la meseta más lenta o más baja que el acantilado quedaba un surco cerrado
  detrás de la costa (0,4 % de la península en pozos). `all_land` no lleva lagos (`land_mask = 1`); los lleva
  `lakeland`.
* **Agua clasificada en el informe** (7.9): componentes conexas del agua sobre la rejilla del informe; el
  `role` del primer elemento de agua cuyo centro cae dentro manda, si no `ocean` si toca el marco, `sea` por
  encima de `classes.sea` (1/100 del mapa, 1000 km² en infinito) y `lake`. `land.info.waters` y
  `script_report.lands.waters`.
* **`frame.edge_margin`** y **porcentajes** del marco acotado en cualquier posición (`at = { "30%", "75%" }`).
* **`Lands.Continent` es un preset** sobre `Compose`; el macro de rejilla se retiró entero (unas 600 líneas) y
  sus claves fallan diciendo qué usar. `lands_continent` se regeneró a 192 km.
* **Fracción de tierra de los continentes**: la de la composición (0,2 a 0,7 según el preset), no el 70 % del
  macro antiguo; `land_fraction` solo reescala los repartos.
* **Bandas de referencia** (paso 3.5): medidas en los mapas de alturas de `docs/references/` (no versionados) con
  `terrain_metrics`; las de los archipiélagos son mapas pintados y no se usan. Se comprueban en `test_compose`
  (no en `test_lands`, que no tiene motor): fracción de tierra y cuota de la masa mayor. La dimensión de costa
  y la integral hipsométrica quedan fuera de banda hasta la fase 4 (detalle), como era de esperar en formas sin
  detalle, y se imprimen. Islas: tierra 0,17 a 0,45, masa mayor 0,85 a 1, dimensión 1,02 a 1,39, integral 0,26 a
  0,45; continentes: 0,20 a 0,89, 0,42 a 1, 1,12 a 1,38 y 0,15 a 0,62. `fractious` no se mide por la masa mayor
  (es tierra fragmentada a propósito; su tipo de referencia serían los archipiélagos).
* La corrección del filete fue un commit `fix:` aparte, después de 3.4, porque apareció revisando los ejemplos
  de 3.6 con la semilla de sus goldens.

**Contornos antes de la fase 4** (2026-10-29). Las imágenes de `docs/references/` son el resultado pedido y los
generadores revisados el mínimo. Decisiones:

* **Se revierte el descarte de 5.2 para mapas acotados:** `Lands.Template` porta las 14 plantillas de Azgaar tal
  cual (sus pasos y números, rejilla con jitter de 10 000 celdas, alturas enteras); su fracción de tierra cae
  en la dispersión de su propio generador (6 vistas previas por plantilla en azgaar.github.io).
* **Encima, lo que acerca a las referencias:** doblado amplio (0,08 del mapa; más pliega la tierra), fiordos
  caminados tierra adentro (8 por mapa), deshilachado fractal en distancia (2,5 celdas) por su campo de
  rugosidad y warp bajo la celda en la GPU; distancia exacta (Felzenszwalb). El ruido en unidades de altura
  (umbral de mapgen2) apenas movía la costa y se descartó; también los valles por `StreamPower`, que solo
  almenaban la costa.
* **Los presets con plantilla la usan por defecto;** `outline = "shapes"` da la composición con ids.
  `land_fraction` fija el nivel del mar por cuantil.

---

## Índice

1. [Objetivo y alcance](#1-objetivo-y-alcance)
2. [Criterio fundamental y principios](#2-criterio-fundamental-y-principios)
3. [Punto de partida: qué hay hoy y qué se reutiliza](#3-punto-de-partida-qué-hay-hoy-y-qué-se-reutiliza)
4. [Análisis de los repositorios de referencia](#4-análisis-de-los-repositorios-de-referencia)
5. [Qué se toma, cómo se traduce y qué se descarta](#5-qué-se-toma-cómo-se-traduce-y-qué-se-descarta)
6. [Arquitectura: el op fusionado `Lands`](#6-arquitectura-el-op-fusionado-lands)
7. [Algoritmos detallados](#7-algoritmos-detallados)
8. [API propuesta](#8-api-propuesta)
9. [Plano infinito y mapas acotados](#9-plano-infinito-y-mapas-acotados)
10. [Herramientas para que Claude trabaje afinado](#10-herramientas-para-que-claude-trabaje-afinado)
11. [Fases prioritarias (1 a 3)](#11-fases-prioritarias-1-a-3)
12. [Fases posteriores (4 a 8)](#12-fases-posteriores-4-a-8)
13. [Catálogo de ejemplos](#13-catálogo-de-ejemplos)
14. [Validación y tests](#14-validación-y-tests)
15. [Riesgos y decisiones abiertas](#15-riesgos-y-decisiones-abiertas)
16. [Flujo de trabajo](#16-flujo-de-trabajo)
17. [Referencias](#17-referencias)

---

## 1. Objetivo y alcance

**Objetivo.** Que el motor genere islas, archipiélagos, continentes y cualquier otra distribución de tierra y
agua (todo tierra, tierra con mares interiores o lagos, todo mar con relieve submarino) de forma:

* **controlable**: "5 islas de 2 a 4 km²", "agranda la isla 3 un 20 %", "una península al sur" o
  "acantilados más altos que el interior" se traducen en parámetros, no en reescribir un script;
* **medible**: cada elemento tiene un id, un área real y unas dimensiones que se comprueban;
* **válida en el plano infinito y en mapas de tamaño fijo** con el mismo modelo;
* **eficiente**: formas analíticas evaluadas en GPU en un único op; la CPU, solo para colocar y medir;
* **con el menor código nuevo posible**: se reutiliza lo que el motor ya tiene (sección 3.4) y solo se crea
  lo que falta, más las herramientas que Claude necesita para trabajar afinado (sección 10).

La generación cubre **la superficie y la parte oceánica**: una sola superficie, continua a través del nivel
del mar. El océano empieza con una base geométrica y se mejora en la fase 6.

**Prioridad.** Islas, archipiélagos y continentes (fases 1 a 3). Detalle, hidrología, océano completo, clima,
biomas e infraestructura van después (fases 4 a 8).

**Fuera de alcance.** Política, culturas, nombres y rutas.

---

## 2. Criterio fundamental y principios

### 2.1 Criterio fundamental: primero formas geométricas simples, después detalle

Todo elemento, de tierra o de océano, se construye **primero** con primitivas analíticas (distancias con
signo combinadas por unión, intersección y resta, y perfiles sobre esas distancias). El ruido, el warp y la
erosión entran **solo** en la etapa de detalle, sobre una forma que ya es correcta y medible, con su amplitud
en metros declarada.

Consecuencias:

* Las primitivas son **locales**: valen en el plano infinito sin rejilla global.
* Son **independientes de la resolución** y se evalúan en GPU.
* La **distancia a la costa** sale de la composición; no hace falta una transformada de distancia salvo que
  la erosión desplace la costa (fase 4).
* Son **editables**: "agranda la isla 3" cambia un radio.
* **Tiempo**: hoy los goldens `lands_*` pasan 6 a 7 s (Release) y unos 27 s (Debug) en Lua, construyendo una
  rejilla de CPU celda a celda. Con formas analíticas, el script solo coloca elementos.

### 2.2 Principios

1. **Forma en planta separada del perfil.** Dónde hay tierra es una capa; cómo sube y baja la altura es otra.
   **Nada supone que el centro sea más alto.** El perfil es una curva sobre la distancia a la costa y los
   elementos de relieve se colocan por su cuenta.
2. **Tierra y agua se componen libremente.** Elementos con signo de tierra o de agua; cualquier fracción de
   tierra de 0 a 1. "Mar" se define por etiqueta o por área, no por tocar el borde.
3. **Un modelo para el plano infinito y para tamaños fijos** (sección 9).
4. **Editable por Claude**: ids estables, lista resuelta en los metadatos, edición por id.
5. **Parametrización de las referencias, no sus supuestos.** Se toman operaciones y parámetros; no la regla
   "elevación = distancia a la costa" como valor por defecto ni constantes ligadas al número de celdas.
6. **Determinismo**: todo azar sale de un hash sembrado; nada depende del orden de consulta ni de la ventana.
7. **Unidades del mundo**: metros y km².
8. **Reutilizar antes que escribir**: cada paso empieza comprobando qué parte del motor ya lo hace.

---

## 3. Punto de partida: qué hay hoy y qué se reutiliza

### 3.1 El macro actual (`assets/lua/prelude/50_lands.lua`, 839 líneas)

Rejilla de CPU (256 celdas por defecto, hasta 512) leída en GPU con `Terrain.Grid`:

1. **Cuerpos** elípticos rotados con una espina de terreno alto, vistos con un domain warp. Layouts
   `single`, `scattered`, `hotspot`, `arc`, `atoll`, `continent`.
2. **Ríos**: `Erosion.StreamPower`.
3. **Nivel del mar por cuantil** (`land_fraction`, 0.7).
4. **Distancia con signo** (chamfer en Lua) y etiquetas de masa (`region_id`).
5. **Costa**: warp en tres bandas (macro, meso, micro) con amplitud proporcional a la longitud de onda,
   omitiendo las bandas de menos de 6 muestras; tipo de costa por dureza, sedimento, exposición y curvatura.
6. **Margen**: presets `compressed` (anchos proporcionales al mapa, para el visor, que muestra unos 28 km) y
   `geologic` (plataforma 40 km a 130 m, talud 60 km hasta 3500 m); laguna de atolón.
7. Contornos dibujados opcionales (`area`, `areas`).

Salidas: `land_mask`, `land_height`, `bathymetry`, `surface_height`, `shore_mask`, `coast_type`, `region_id`
(7 de las 8 de `kMaxOutputs`).

### 3.2 Limitaciones abiertas (`docs/todo.md`, "Lands macro")

* Cadena, arco y atolón ocupan el 6 a 12 % del mapa, no el 70 %.
* La isla del 70 % toca el borde del mapa.
* La exposición de la costa no tiene fetch.
* Los macros no drenan: sin lagos ni ríos como agua.
* Rejilla de 256 celdas: drenaje tan fino como 94 m a 24 km.
* Ninguna referencia real medida con `terrain_metrics`.

### 3.3 Problemas de diseño para los objetivos nuevos

* **Cuantil de la ventana**: el nivel del mar depende del área exportada; incompatible con el plano infinito.
* **Supuesto de perfil**: espina alta y altura creciente hacia el interior.
* **Sin ids estables ni lista resuelta**.
* **Rejilla de CPU en Lua**: lenta y acotada a `size`.
* **Límites del grafo**: un `Shapes.*` es una cadena de nodos (un polígono son nodos de dos segmentos cada
  uno, y `Shapes.Spline` está limitado a 128 segmentos); el grafo admite 1024 nodos (`Graph::kMaxNodes`).
  Cien islas como `Shapes` agotan el grafo. Es el mismo problema que `Water` resolvió con un op fusionado.

### 3.4 Inventario de lo reutilizable

| Pieza existente | Dónde | Qué aporta al plan | Uso |
| --- | --- | --- | --- |
| Op `Water` fusionado + bloque en el buffer de rejilla | `water.comp.glsl`, `kernels.cpp`, helper `block` en `lua.cpp` | patrón exacto de "una lista de elementos en un bloque, un nodo" | **plantilla del op `Lands`** (sección 6) |
| Muestreador de CPU, misma aritmética que la GPU | `src/terrain/sampler.cpp` | evaluar el grafo en CPU | referencia de CPU del op y medición |
| `Terrain.Sample` | `lua.cpp` | muestrear cualquier valor R2→R1 en una rejilla o en puntos | **medir áreas, fracción de tierra, clasificar** sin código nuevo de medida |
| Distancias de `shape.comp.glsl` (círculo, elipse, caja redondeada, segmentos con paridad) | GLSL | SDF ya probadas | se mueven a `lib/shape.lib.glsl` y las comparten `Shape` y `Lands` |
| `Combine.SmoothMin` / `SmoothMax` (smin polinómico) | `arith.comp.glsl` | unión suave con radio `k` | misma fórmula dentro de `Lands` |
| `Shapes.*` (CSG, `Warp`, `Mask`, `Spline` Catmull-Rom) | `10_shapes.lua` | contornos pocos y declarados; warp del detalle | se usan tal cual; `sample_spline` se reutiliza para bandas |
| `Landforms.*` (`Cone`, `Dome`, `Bowl`, `Plateau`, `Ridge`, `Trench`...) | `20_landforms.lua` | relieve declarado (pocos elementos) | tal cual |
| `Curves.Profile` (`linear`, `smooth`, `dome`, `gaussian`), `Terrace`, `Smoothstep`, `Remap` | ops | perfiles sobre distancia | perfil global y margen |
| `Terrain.Transform` (posición, rotación, escala, pivote) | op | colocar formas | elementos declarados |
| Warp de costa en tres bandas, margen (`MARGINS`), tipo de costa (`COASTS`, exposición) | `50_lands.lua` | detalle de costa y margen | se extraen a funciones y se reutilizan |
| Etiquetado de masas (chamfer + regiones) | `50_lands.lua` | componentes conexas en Lua | clasificación sobre `Terrain.Sample` |
| `hasher`, `make_noise`, `smooth` | `30/40/50_*.lua` | hash determinista | se unifican en uno (fase 1, paso 1) |
| `generate.world_cell` | `46_network.lua` | celdas fijas del mundo + anillo | **misma regla para el plano infinito** |
| `Erosion.StreamPower`, `Erosion.Gullies`, `Ranges.Carve` | C++ / prelude | erosión y detalle | fase 4 |
| `Water.Drain`, `Water.Network`, `Hydrology.*` | `45_water`, `46_network`, `hydrology.cpp` | lagos y ríos sobre cualquier terreno | fase 5, sin algoritmo nuevo |
| `metrics::Measure` (fracción de tierra, islas, áreas, cuota dominante, lagunas, costa, dimensión fractal, canal mínimo, hipsometría) | `tests/unit/terrain_metrics.hpp` | criterios de aceptación | tests y herramienta `terrain_metrics` |
| `tools/dev.sh review` (export + métricas + hillshade) | `tools/dev.sh` | revisión visual | tal cual, más un subcomando (sección 10) |
| Goldens auto-descubiertos (`case.json` + `script.lua`) | `tests/datasets/` | tests de regresión de los ejemplos | un golden por ejemplo seleccionado |
| `test_geometry` (valores cerrados), `test_lands`, `test_cli`, `test_viewer`, `test_seams` | `tests/unit/` | tipos de prueba ya establecidos | se amplían |
| Visor: `--param`, `--resolution`, `--view`, `--target`, `--ring`, `--water`, `--screenshot` | `viewer_main.cpp` | ver cada ejemplo a distintos tamaños | comandos en la cabecera de cada ejemplo |

**Código nuevo imprescindible** (lo que no existe y no se puede expresar con lo anterior):

1. El op fusionado `Lands` (GLSL + entrada en `kOps` + binding del bloque + referencia en `sampler.cpp`).
2. La capa Lua `Lands.Compose` (colocación, ids, edición, medición con `Terrain.Sample`).
3. La salida de la lista de elementos a los metadatos (binding pequeño, sección 10).

---

## 4. Análisis de los repositorios de referencia

### 4.1 mapgen2 (Red Blob Games, JavaScript)

Malla Voronoi/Delaunay (Delaunator + dual-mesh, regiones "fantasma" en el borde), struct-of-arrays.

**Agua y tierra** (`water.js`, `assign_water_r`):

* Coordenadas normalizadas a [-1, 1]; distancia **Chebyshev** `d = max(|nx|, |ny|)`.
* `n = fbm(amplitudes, nx, ny)`; `n = lerp(n, 0.5, round)`.
* Agua si `n − (1 − inflate)·d² < 0`. `round` redondea; `inflate` reduce la penalización del borde.
* **Océano** (`assign_ocean_r`): flood fill desde la región fantasma; el agua no alcanzada es lago.

**Elevación** (`elevation.js`):

* BFS multi-fuente desde los triángulos de costa; coste 0 en aristas de lago (entran por la cabeza de la
  cola), 1 en tierra; relajación si mejora; orden de vecinos con desfase aleatorio. Guarda el lado padre como
  **puntero de bajada**.
* `e = −d/minDistance` en océano, `d/maxDistance` en tierra; región = media de triángulos; océano ≤ −0.01.
* **Redistribución** por rango: `y = 1 − (1 − x)²` ⇒ `x = √S − √(S·(1 − y))`, `S = 1.1`, recorte a 1.

**Ríos** (`rivers.js`): manantiales con `0.3 ≤ e ≤ 0.9` fuera del agua; +1 de caudal por lado recorrido.

**Humedad** (`moisture.js`): semillas en orillas de río y lago; BFS por tierra; `m = 1 − (d/max)^0.5`; agua
= 1; redistribución uniforme por rango.

**Biomas** (`biomes.js`): `t = 1 − e + lerp(bias_north, bias_south, latitud)`. `OCEAN`; agua `MARSH` (t > 0.9),
`ICE` (t < 0.2), `LAKE`; costa `BEACH`; tierra:

| Temperatura | Humedad → bioma |
| --- | --- |
| < 0.2 | > 0.50 `SNOW`, > 0.33 `TUNDRA`, > 0.16 `BARE`, si no `SCORCHED` |
| 0.2 a 0.4 | > 0.66 `TAIGA`, > 0.33 `SHRUBLAND`, si no `TEMPERATE_DESERT` |
| 0.4 a 0.7 | > 0.83 `TEMPERATE_RAIN_FOREST`, > 0.50 `TEMPERATE_DECIDUOUS_FOREST`, > 0.16 `GRASSLAND`, si no `TEMPERATE_DESERT` |
| ≥ 0.7 | > 0.66 `TROPICAL_RAIN_FOREST`, > 0.33 `TROPICAL_SEASONAL_FOREST`, > 0.16 `GRASSLAND`, si no `SUBTROPICAL_DESERT` |

### 4.2 voronoi-map-js (rjanicek, port de mapgen2)

**Formas** (`island-shape.js`, `inside(q)`, q en [-1, 1]):

* **Radial** (`islandFactor = 1.07`): `bumps ∈ [1, 5]`, `startAngle`, `dipAngle`, `dipWidth ∈ [0.2, 0.7)`;
  radio mixto `0.5·(max(|x|,|y|) + |q|)`;
  `r1 = 0.5 + 0.40·sin(start + bumps·θ + cos((bumps+3)θ))`,
  `r2 = 0.7 − 0.20·sin(start + bumps·θ − sin((bumps+2)θ))`; **bahía**: dentro de `dipWidth` de `dipAngle`,
  `r1 = r2 = 0.2`; tierra si `length < r1` o `r1·islandFactor < length < r2` (anillo de satélites).
* **Perlin** (`oceanRatio` reescalado a [0.1, 0.5]): tierra si `c > oceanRatio·(1 + d²)`.
* Square, Blob, Noise (defectuosa: no depende de la posición), Bitmap (sin límites).

**Pipeline** (`map.js`): `improveCorners` (esquinas a la media de sus centros); esquinas por BFS desde el
borde con +0.01 agua / +1.01 tierra (+aleatorio opcional), sin mínimos locales; redistribución con
`S = 1.1`; polígono agua si ≥ `lakeThreshold = 0.3` de sus esquinas lo son; océano por flood fill; costa;
bajadas al vecino más bajo; cuencas (hasta 100 pasadas); ríos `(ancho + alto)/4` intentos con fuente en
`0.3 ≤ e ≤ 0.9`; humedad río `min(3, 0.2·caudal)`, lago 1, decaimiento **0.9** por paso; biomas por elevación
(> 0.8, > 0.6, > 0.3, resto).

**Noisy edges**: subdivisión recursiva en el cuadrilátero (v0, d0, v1, d1), corte en [0.2, 0.8], factores
[0.6, 1.4], `noisyLineTradeoff = 0.5`, longitud mínima 10 por defecto, 3 entre biomas, 100 entre océanos y
**1 en costa, ríos y lava**: el detalle va donde se ve.

**Lava**: e > 0.8 y humedad < 0.3, probabilidad 0.2; defectuosa (nunca crea lava).

### 4.3 heman (Philip Rideout, C)

**Generadores** (`SEALEVEL = 0.5`, `DEFAULT_STRENGTH = 0.6`):

* **Isla**: ruido de 3 canales, frecuencias `{4, 16, 32, 64, 128}`, amplitudes
  `{0.2, 0.1, 0.05, 0.025, 0.0125}`; máscara `0.707 − √(u² + v²) + n0` con `u, v` desplazados; umbral 0.5;
  SDF; pasada final con warp `influence = z` y `z += 6·influence·n0`: **ruido creciente hacia el interior,
  costa limpia**.
* **Archipiélago**: puntos `(x, y[, fuerza])`, conos `fuerza·(1 − d/r)` combinados por **máximo**,
  `fuerza = 0.56` por defecto, `noiseamt ≈ 0.3`.
* **Roca**: `√(r² − d²)` con `r = 0.3 + n`. **Planeta**: OpenSimplex 3D lat-long. **fBm**: lacunaridad 2,
  ganancia 0.5.

**Distancia**: Felzenszwalb–Huttenlocher, envolvente de parábolas, O(n) por línea, separable; con signo
(positivo dentro), sin signo y **campo de punto más cercano** (CPCF).

**Ops**: `warp` (ganancia 0.6, lacunaridad 2, amplitud 0.05, frecuencia 8, **amortiguado en cumbres**
`(1 − h)^4`); `emboss` (tierra fino `a = 0.0005, f = 256`; océano **∝ profundidad** `a = 0.5, f = 1`);
`percentiles` (terrazas de igual área); `stairstep`; Poisson de Bridson y por densidad; oclusión por barridos
de horizonte.

Bugs: `sobel` lee la fila 0; `merge_political` usa `MIN` por `MAX`; normalización de distancias
inconsistente.

### 4.4 Fantasy-Map-Generator (Azgaar, TypeScript)

Rejilla de ~10 000 celdas, alturas 0 a 100, mar en 20.

**Lenguaje de plantillas**: `Herramienta cantidad altura rangoX% rangoY%`.

| Operación | Algoritmo |
| --- | --- |
| `Hill` | BFS: `h_vecino = h^blobPower·U(0.9, 1.1)` hasta h ≤ 1; reintenta (50) si la cima pasaría de 90 |
| `Pit` | igual restando; empieza en tierra |
| `Range` | camino voraz a un destino a `ancho/8`–`ancho/3` (15 % de pasos "acortan"); anillos `h = h^linePower − 1` hasta h < 2; **cada 6 celdas del eje baja ladera abajo** con `(2·h + h_min)/3` (estribaciones) |
| `Trough` | igual restando; 20 %; hasta `ancho/2` |
| `Strait` | camino de borde a borde por el 30–70 % central; `h^exp`, `exp = 0.9 − (0.1/ancho)·restante` |
| `Mask` | `(1 − nx²)(1 − ny²)`; negativa invierte (mar interior); mezcla `(h·(fr − 1) + h·d)/fr` |
| `Invert` | espejo con probabilidad |
| `Add`, `Multiply` | sobre `all`, `land` o un rango, respetando el nivel 20 |
| `Smooth` | `(h·(fr − 1) + media)/fr` |

`blobPower` (0.93 a 0.9973) y `linePower` (0.75 a 0.93) dependen del número de celdas.

Plantillas (peso): `volcano` 3, `highIsland` 19, `lowIsland` 9, `continents` 16, `archipelago` 18, `atoll` 1,
`mediterranean` 5, `peninsula` 3, `pangea` 5, `isthmus` 2, `shattered` 7, `taklamakan` 1, `oldWorld` 8,
`fractious` 3. Estructura de las más útiles:

* `volcano`: Hill central 90–100, `Multiply 0.8` en 50–100, Range central, Smooth, colinas laterales, `Mask 3`.
* `highIsland`: Hill grande, `Add 7`, colinas, Range, `Multiply 0.8` en tierra, `Mask 3`, Troughs, Pits.
* `lowIsland`: igual con `Multiply 0.4` en 20–100: isla llana.
* `continents`: dos Hill grandes separados, colinas, Ranges en los bordes, dos `Strait` verticales, Troughs.
* `archipelago`: `Add 11`, Ranges, Hills, `Trough 10`, `Strait` vertical y horizontal.
* `mediterranean`: relieve pegado a los bordes norte y sur y `Mask −2` (mar en el centro).
* `atoll`: Hill central, `Multiply 0.2` en 25–100 (hunde el centro).
* `peninsula`: Range desde un borde, `Add`, Hill en el borde, `Invert`.
* `pangea`: Hills en las cuatro esquinas y una banda central, Troughs, Ranges.
* `isthmus`: cadena diagonal de Hill y Trough alternos, `Invert x`.
* `shattered`: Hills, `Trough 10–20`, Ranges, `Pit 12–20`.
* `taklamakan`: Hills altos en los cuatro bordes: cuenca cerrada.
* `fractious`: 12–15 Hills, `Mask −1.5`, `Mask 3`, `Add −20`, Ranges.

**Features**: flood fill + anillos de distancia (+3 … −10). Subtipos: tierra `continent` > N/10 celdas,
`island` > N/1000, si no `isle`, `lake_island`; agua `ocean` > N/25, `sea` > N/1000, si no `gulf`; lago
`frozen` (t < −3 °C), `dry` (evaporación > 4·aporte sin entradas ni salidas), `salt` (evaporación > aporte
sin salida), `sinkhole`, `lava`, `freshwater`.

**Lagos**: nivel = orilla mínima − 0.1; cerrados si no hay salida bajo `lakeElevationLimit`; evaporación
`((700·(t + 0.006·h))/50 + 75)/(80 − t)` por celda.

**Temperatura**: trópicos entre 16° y −20° con 0.15 °C/°, tramos lineales hasta los polos, −6.5 °C/km.

**Precipitación**: vientos por franjas de 30°; humedad `maxPrec − h`; sobre agua `+5·mod` y lluvia en la costa
de llegada; en tierra `max(humedad/(10·mod), 1) + Δh·(h_sig/70)²`; por encima de 85, la humedad cae a 0
(**sombra de lluvia**); modificador por bandas de 5°
`[4, 2, 2, 2, 1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 1, 1, 1, 0.5]`.

**Ríos**: caudal acumulado por altura; río ≥ 30, navegable ≥ 100; depresiones +0.1 iterativo; erosión de cauce
con h ≥ 35, `min(caudal/caudal_superior, 5)`; ancho `min(caudal^0.7/500, 1)` + longitud; meandros.

**Costa**: fractalización (`maxDepth 4`, `baseAmplitude 1.5·√longitud`, `amplitudeDecay 0.9`, `minEdge 1`) con
**campo de rugosidad** (`roughnessScale 60`, `smoothThreshold 0.25`, `roughnessContrast 1.5`, lagos ×2 más
tranquilos).

**Océano**: anillos batimétricos a distancias −1 … −9. **Biomas**: humedad (5) × temperatura (26).

---

## 5. Qué se toma, cómo se traduce y qué se descarta

### 5.1 Traducción

| Concepto | Traducción en YATE | Sección |
| --- | --- | --- |
| Azgaar `Hill` | elemento de contorno + perfil (`dome`, `cone`) | 7.1, 7.4 |
| Azgaar `Pit` | elemento de agua o relieve `bowl` | 7.1, 7.5 |
| Azgaar `Range` con estribaciones | `ridge` sobre spline + `spurs` geométricos | 7.5 |
| Azgaar `Trough` | `trench` sobre spline | 7.5 |
| Azgaar `Strait` | `band`/`capsule` de agua | 7.1 |
| Azgaar `Mask` / negativa | elemento `frame` de tierra o de agua | 7.1 |
| Azgaar `Invert` | `mirror` del elemento | 8.3 |
| Azgaar `Add`/`Multiply`/`Smooth` | parámetros del perfil, `smooth` de la unión | 7.2, 7.4 |
| Azgaar plantillas | presets de `Lands.Compose` | 11 |
| Azgaar subtipos | clasificación por área (km²) | 7.9 |
| mapgen2 radial (`bumps`, `dip`, `islandFactor`) | `radial` + `sector` de agua + anillo de satélites | 7.1 |
| mapgen2 `inflate`, `round` | elemento `field` (fase 4) | 12 |
| mapgen2 hipsometría | curva fija sobre la altura | 7.4 |
| heman archipiélago `(x, y, fuerza)` | `centers` | 7.6 |
| heman Poisson | colocación por `scatter` | 7.6 |
| heman ruido ∝ distancia al interior | detalle (fase 4) | 12 |
| heman `emboss` océano | detalle del fondo (fase 6) | 12 |
| heman Felzenszwalb | solo si la erosión mueve la costa (fase 4) | 12 |
| Azgaar campo de rugosidad | detalle de costa (fase 4) | 7.10 |
| Azgaar clima, lagos, ríos | fases 5 y 7 | 12 |

### 5.2 Descartes

* **Malla Voronoi/Delaunay y noisy edges en polilínea**: YATE es raster en GPU; primitivas y warp cubren lo
  mismo con independencia de la resolución.
* **Elevación = distancia a la costa por defecto**: queda como preset `dome`.
* **Redistribución por rango**: depende de la ventana; se usa una curva fija.
* **Océano = agua que toca el borde**: no hay borde en el plano infinito.
* **Constantes por número de celdas** (`blobPower`, `linePower`, N/10, N/1000): pasan a metros y km².
* **Crecimiento por BFS celda a celda** (Hill, Range de Azgaar): depende de la rejilla; se sustituye por
  primitivas analíticas con el mismo efecto.
* **Política, culturas, rutas, nombres; lava de voronoi-map.**
* **Bugs**: `sobel` y `merge_political` (heman); `makeNoise`, `makeBitmap`, lava (voronoi-map).

---

## 6. Arquitectura: el op fusionado `Lands`

### 6.1 Por qué un op nuevo

Componer cada isla con `Shapes.*` cuesta varios nodos (posición, distancia, unión) y el grafo tiene 1024;
un polígono cuesta un nodo cada dos segmentos. El op `Water` ya resolvió lo mismo: **una lista de elementos en
un bloque del buffer de rejilla y un nodo**. `Lands` copia ese patrón; no inventa un mecanismo nuevo.

Los `Shapes.*` y `Landforms.*` siguen siendo la vía para pocos elementos declarados a mano y para el detalle.

### 6.2 Contrato del op (como `water.comp.glsl`)

* Clase: pointwise (halo 0); dos secciones vecinas coinciden por construcción.
* Entradas: `position` (R2→R2, por defecto `Terrain.Coords`; un `Shapes.Warp` da el detalle de costa sin
  código nuevo) y, opcional, un campo auxiliar.
* Slot 3: el bloque, ligado por el evaluador, igual que `Water` y `Grid`.
* Dos canales, como `Water` (`maxChannels = 2`).
* Modos (parámetro 2):

  | Modo | Canal 0 | Canal 1 |
  | --- | --- | --- |
  | 0 `coast` | distancia con signo a la costa (tierra < 0), en metros, recortada a `D` | id del elemento de tierra dominante |
  | 1 `relief` | altura sobre el nivel del mar del perfil de tierra mezclado por elemento | id dominante |
  | 2 `seabed` | profundidad del margen y de los elementos del fondo | id dominante |

* Referencia de CPU en `sampler.cpp`, **término a término** igual que el GLSL (como `Water`). Así funcionan
  `Terrain.Sample` sobre el op y las comparaciones GPU/CPU de los tests.

### 6.3 Disposición del bloque

```
cabecera (24 valores)
  0 modo de bloque   1 número de elementos   2 tamaño de bin (m)   3..4 origen de bins (x, z)
  5..6 bins en x, z  7 D: distancia máxima medida (m)   8 inicio de elementos   9 inicio del índice
  10 inicio de puntos (bandas y polígonos)   11 número de puntos   12..23 parámetros globales del modo
elementos (32 valores cada uno)
  0 tipo  1 operación (add, subtract)  2 id numérico  3 x  4 z  5 rotación  6 escala
  7..14 parámetros de la forma  15 suavizado k (m)  16 radio envolvente (m)
  17..25 perfil o margen (preset y parámetros)  26 inicio de sus puntos  27 número de puntos  28..31 libres
índice de bins (CSR)
  desplazamientos por bin, luego índices de elemento en orden global
puntos (x, z, anchura) para bandas, arcos muestreados y polígonos
```

El bloque lo construye un binding nuevo `Lands.Block{...}` que reutiliza el helper `block` de `lua.cpp`
(el mismo que `RiverBlock`/`LakeBlock`), y el nodo se añade como `Hydrology.Carve` añade el de `Water`.

### 6.4 Evaluación por muestra

1. Bin de la muestra: `floor((p − origen)/bin)`. Fuera de los bins: distancia `D` (mar abierto).
2. Para los elementos del bin, en orden global: distancia de su forma (7.1) en su espacio local
   (traslación, rotación, escala).
3. CSG en ese orden: `add` con smin polinómico, `subtract` con smax (7.2).
4. Id dominante y pesos de mezcla por softmin (7.3).
5. Según el modo: distancia, perfil de tierra mezclado (7.4) o margen y fondo (7.7).
6. Recorte a `±D`.

Coste por muestra: O(elementos del bin), independiente del total. El tamaño de bin se elige para que un bin
tenga pocos elementos (sección 7.11).

---

## 7. Algoritmos detallados

### 7.1 Distancias de las primitivas

Todas en espacio local, en metros, negativas dentro. Las cuatro primeras ya existen en `shape.comp.glsl` y se
mueven a `lib/shape.lib.glsl`.

| Primitiva | Distancia | Exacta | Área cerrada |
| --- | --- | --- | --- |
| `circle` (r) | `|p| − r` | sí | `πr²` |
| `ellipse` (a, b) | la de `shape.comp.glsl` | aprox. acotada | `πab` |
| `box` (hx, hz, redondeo) | la de `shape.comp.glsl` | sí | `4hx·hz − (4 − π)ρ²` |
| `polygon` (puntos) | segmentos + paridad, como `Shape`, pero sin límite de nodos | sí | fórmula del área de Gauss (lazada) |
| `superellipse` (a, b, n) | `f = (|x/a|^n + |z/b|^n)^(1/n) − 1`, dividida por `|∇f|`: `d ≈ f/|∇f|` | 1.er orden | `4ab·Γ(1+1/n)²/Γ(1+2/n)` |
| `radial` (R, armónicos `a_k`, `φ_k`) | `r(θ) = R·(1 + Σ a_k·sin(kθ + φ_k))`; `d ≈ (|p| − r(θ))/√(1 + (r′(θ)/r(θ))²)` | 1.er orden | `πR²·(1 + ½·Σ a_k²)` (armónicos distintos ortogonales) |
| `capsule` (a, b, r) | `sdSegment − r` | sí | `2r·|ab| + πr²` |
| `band` (puntos con anchura) | unión de conos redondeados por segmento (`sdUnevenCapsule` de Íñigo Quílez) | sí por tramo | integración numérica en Lua al construir |
| `ring` (R, w) | `||p| − R| − w/2` | sí | `2πR·w` |
| `arc` (R, ángulos, w) | `sdArc` de Íñigo Quílez − w/2 | sí | `R·Δθ·w + πw²/4` |
| `sector` (r, ángulo, alcance) | `sdPie` de Íñigo Quílez | sí | `α·r²` (α = semiángulo) |
| `frame` (marco del mapa, margen, exponente) | superelipse grande (n alto = rectángulo redondeado) | 1.er orden | igual que superelipse |

Notas:

* La forma **radial** generaliza la de mapgen2 sin ruido: con `bumps = k` y `a_k` se obtienen los lóbulos;
  dos armónicos (k y k+3) reproducen el `sin(kθ + cos((k+3)θ))` original con un error pequeño y área cerrada.
  Restricción de validez: `Σ|a_k| < 1` (radio positivo) y `Σ k·|a_k| < 1.5` para que la corrección por
  gradiente no se degrade.
* La **bahía** (`dip` de mapgen2) no se hornea en la forma: es un `sector` de agua restado. Se edita aparte.
* Los **satélites** (`islandFactor`) son un `scatter` en un `ring` alrededor de la isla, no un anillo de tierra.
* Las aproximaciones de 1.er orden son exactas en la costa (d = 0), que es lo que define el área y el
  contorno; lejos de ella se desvían algo, y lo absorbe el perfil.

### 7.2 Composición (CSG)

* Unión: `smin(a, b, k) = mix(b, a, h) − k·h·(1 − h)`, `h = clamp(0.5 + 0.5·(b − a)/k, 0, 1)` (la de
  `arith.comp.glsl`). Error máximo `k/4` respecto a `min`; con `k = 0`, unión exacta.
* Resta: `smax(a, −b, k)`.
* `k` por elemento (metros): con `k` del orden de la anchura de un estrecho, dos islas cercanas se unen en un
  istmo natural; con `k = 0`, quedan separadas.
* Orden: el de la lista. La primera tierra, luego el agua que la recorta, luego la tierra dentro del agua
  (isla en un lago), etc. El índice de bins conserva el orden global, así que el resultado no depende del bin.

### 7.3 Id dominante y pesos de mezcla

* Id: el elemento de tierra con menor distancia propia en la muestra.
* Pesos para mezclar perfiles y márgenes de elementos distintos sin escalón:
  `w_i = exp(−(d_i − d_min)/τ)` normalizados, con `τ = max(k_i, 1 muestra)`. Si todos los elementos comparten
  perfil, la mezcla es la identidad.

### 7.4 Perfil de tierra (sin supuestos)

Altura = `Σ w_i · P_i(s_i)` con `s = −d` (distancia hacia el interior, ≥ 0) y `P_i` el perfil del elemento.

Presets (parámetros en metros; todos editables y **ninguno implícito**: si no se da perfil, el script falla
con un mensaje que lista los presets):

| Preset | Definición | Parámetros |
| --- | --- | --- |
| `dome` | `H·profile(s/W)` con la curva `dome` de `Curves.Profile` | H, W |
| `cone` | `H·min(s/W, 1)` | H, W |
| `plateau` | `H·smoothstep(0, E, s)` | H, E (anchura del borde) |
| `rim` | `H_r·g(s; c, σ) + H_i·smoothstep(0, W, s)` con `g` gaussiana centrada a `c` del borde: **acantilado más alto que el interior** cuando `H_r > H_i` | H_r, c, σ, H_i, W |
| `caldera` | `rim` con `H_i < 0` relativo al borde: sube y vuelve a bajar a una cubeta | H_r, c, σ, profundidad, radio de la cubeta |
| `tilt` | `smoothstep(0, E, s)·(H_0 + G·dot(p − c, u))`: plano inclinado (cuesta), alto en un lado | H_0, gradiente G, dirección u, E |
| `flat` | `H·smoothstep(0, E, s)` con H pequeño | H, E |
| `curve` | `Curves.Profile`/tabla de puntos `(s, h)` interpolada | puntos |

Costa: los parámetros `beach_width`, `beach_height`, `cliff_height`, `cliff_width` del macro actual pasan a
ser el tramo inicial del perfil (s de 0 a la anchura de playa o acantilado), seleccionado por el tipo de costa.

Hipsometría opcional: `h' = H·C(h/H)` con `C` una curva fija (`low`: `C(x) = x²`, `balanced`: identidad,
`high`: `1 − (1 − x)²`, la curva de mapgen2; o una tabla). Es monótona, así que conserva el orden de alturas y
el drenaje, y no depende de la ventana.

### 7.5 Elementos de relieve

* **Pocos y declarados**: `Landforms.*` tal cual (sin código nuevo), sumados a la altura del op.
* **Muchos o generados** (un volcán por isla, cordilleras en cada continente): modo `relief` del op, con los
  mismos perfiles que `Landforms` (`linear`, `smooth`, `dome`, `gaussian`).

**Cordillera con estribaciones** (traducción geométrica del `Range` de Azgaar):

1. Eje: spline Catmull-Rom (`sample_spline` de `10_shapes.lua`) entre puntos dados o generados.
2. Sección: `Ridge` de anchura W y altura H.
3. Altura a lo largo: `H(s) = H·(1 − Σ_j a_j·exp(−((s − s_j)/σ_j)²))`, collados en posiciones `s_j` por hash
   (parámetro `saddles`, de 0 a N).
4. Estribaciones: cada `spacing` metros de arco (por defecto 2.5·W), un segmento desde el eje con dirección
   la normal del eje girada `±(20° a 40°)` (lado alterno), longitud `W·U(0.8, 1.6)`, altura de arranque
   `0.7·H(s)` y caída lineal a 0. Sustituye el "cada 6 celdas, bajar ladera abajo" de Azgaar, que depende de
   la rejilla.
5. Todo como bandas del bloque (conos redondeados), sin nodos por estribación.

**Volcán**: `Cone` + `Bowl` de cráter (radio y profundidad del cráter). **Depresión**: `Bowl`.
**Escarpe**: `band` con perfil escalonado (`Curves.Terrace` de un peldaño). **Valle**: `Trench`.

### 7.6 Colocación (`scatter`)

**Tamaños primero.** Se sortean las áreas en el rango pedido (log-uniforme por defecto: en la naturaleza hay
muchas más islas pequeñas que grandes; `distribution = "uniform"` disponible), se convierten a radio con las
fórmulas cerradas de 7.1 y se ordenan de mayor a menor (colocar primero las grandes empaqueta mejor, como ya
hace el layout `scattered` actual).

**Mapa acotado: dart throwing con rejilla de aceleración** (Bridson adaptado a radios distintos):

* Conflicto entre i y j si `|c_i − c_j| < R_i + R_j + spacing` (R = radio envolvente).
* Rejilla de aceleración con celda `2·R_max + spacing`; hasta `k = 30` intentos por elemento (Bridson).
* Dentro de `region` (cualquier elemento usado como máscara: se evalúa su distancia con la referencia de CPU)
  y a `edge_margin` del marco.
* Si un elemento no cabe tras `k` intentos, se reduce un 10 % y se reintenta; si sigue sin caber, se omite y
  se avisa con el número colocado frente al pedido.

**Plano infinito: Poisson por celda con prioridad por hash** (independiente del orden, local):

1. Cada celda del mundo (`world_cell`) se divide en `m × m` subceldas; cada subcelda propone un candidato con
   posición, área y prioridad sacadas del hash de `(celda, subcelda, seed)`.
2. Un candidato sobrevive si ningún candidato en conflicto de las celdas vecinas (3 × 3) tiene prioridad
   mayor. Es la variante de un paso del muestreo de Poisson paralelo (Wei, 2008): no necesita orden global, así
   que dos regiones que comparten celdas obtienen exactamente los mismos elementos.
3. Requisito: `world_cell ≥ 2·R_max + spacing`, comprobado con un error claro.
4. `count` se interpreta por celda del mundo (densidad), como en `Water.Network`.

**Patrones** (la posición de los centros; los tamaños siguen las reglas de arriba):

| Patrón | Algoritmo | Referencia |
| --- | --- | --- |
| `uniform` | lo anterior | Bridson, Wei |
| `cluster` | proceso de Neyman–Scott/Thomas: padres por Poisson con intensidad λ_p; hijos `~N(padre, σ²)` en número `Poisson(μ)`; conflicto igual que arriba | archipiélagos naturales en racimos |
| `line` | centros a lo largo de una spline con hueco `gap`; radio que decrece con el arco `R(s) = R_0·(1 − a·s/L)` (cadena de punto caliente que envejece) | `hotspot` actual |
| `arc` | igual sobre un `arc` | `arc` actual |
| `ring` | centros sobre un `ring`, elementos alargados tangencialmente (relación 0.38 del layout `atoll` actual) | atolón |
| `centers` | posiciones dadas `(x, z, radio, fuerza)`; la fuerza escala el perfil | heman |
| `grid` | rejilla con jitter (`points_from_grid` de heman) | utilidad |

**Ids estables**: `"<id del scatter>/<ranura>"`. En acotado, la ranura es el índice en la lista de tamaños
sorteada con el hash de la ranura (no de la posición en el bucle), así que `count = N + 1` deja intactas las N
primeras; en infinito, la ranura es `(celda, subcelda)`.

### 7.7 Margen oceánico y fondo (base geométrica)

Sobre la distancia mar adentro `d > 0`:

```
plataforma  0 ≤ d < Ws:          z = −Zs·(d/Ws)^1.5            (pendiente suave, < 1°)
talud       Ws ≤ d < Ws + Wt:    z = −Zs − (Za − Zs)·smoothstep(0, 1, (d − Ws)/Wt)^0.8
emersión    hasta + Wr:          suavizado hacia la llanura
abisal      después:             z = −Za
```

* Presets existentes reutilizados: `compressed` (anchos proporcionales al marco) y `geologic`.
* Por elemento: `margin = "passive"` (plataforma ancha) o `"active"` (plataforma estrecha y fosa: gaussiana de
  profundidad `Zf` a distancia `Df` de la costa). Mezcla entre elementos con los pesos de 7.3.
* Laguna: dentro de un `ring` de tierra, profundidad `lagoon_depth` (como ahora).
* Elementos del fondo en la fase 1: `seamount` (cono) y `guyot` (cono truncado), ya que son las mismas
  primitivas. El resto, en la fase 6.
* `D` (distancia máxima medida) = `Ws + Wt + Wr`; más allá todo es llanura abisal, y el índice de bins solo
  necesita los elementos a menos de `D`.

### 7.8 Fracción de tierra

* **Acotado**: bisección (8 a 10 iteraciones) sobre un factor de escala común a los elementos **generados**
  (no a los declarados ni a los fijados). Área medida con `Terrain.Sample` del modo `coast` en una rejilla de
  256² (contando celdas con d < 0). Coste: unas pocas evaluaciones en CPU de un op pointwise.
* **Infinito**: objetivo estadístico. Sin solapes (hard-core), cobertura ≈ `λ·E[A]` (λ = elementos por m²);
  con solapes permitidos (modelo booleano), `1 − exp(−λ·E[A])`. Se calibra λ con esa fórmula y se avisa si la
  separación pedida lo impide (en la práctica no se puede cubrir más del ~55 % con discos que no se tocan).
* "Todo tierra" no usa fracción: es un `frame` de tierra que cubre la ventana (o, en infinito, un elemento
  `fill` de tierra) al que se restan elementos de agua.

### 7.9 Clasificación

* Componentes conexas de tierra y de agua sobre `Terrain.Sample` (rejilla de 256² a 512² en la ventana de
  análisis), con el etiquetado del macro actual en Lua.
* Tierra por área: `continent` ≥ `classes.continent` km², `island` ≥ `classes.island`, si no `isle`;
  `lake_island` si la rodea agua que no es mar. Por defecto, en acotado, los umbrales de Azgaar en fracción
  del marco (1/10, 1/1000); en infinito, absolutos (por defecto 10 000 km² y 1 km²). Configurables.
* Agua: el `role` del elemento manda. Sin `role`: en acotado, `ocean` si toca el marco, `sea` si su área
  supera `classes.sea`, si no `lake`; en infinito, `ocean` si toca el borde de la ventana de análisis (que
  incluye el anillo), `sea`/`lake` por área.
* `gulf`: agua del mar que entra en un `sector`/`ellipse` de agua etiquetado así.

### 7.10 Detalle de costa (fase 4, ya decidido para no rehacer la API)

* Warp de tres bandas del macro actual, aplicado a la `position` del op (un `Shapes.Warp`): sin código nuevo.
* Campo de rugosidad (Azgaar): `r = clamp((fbm(p/escala)·0.5 + 0.5 − umbral)/(1 − umbral), 0, 1)^contraste`
  multiplica la amplitud del warp. Valores de partida: `escala` 8 a 15 km, `umbral 0.25`, `contraste 1.5`.
* Amortiguado lejos de la costa: amplitud × `exp(−|d|/L)`, para que el warp no mueva el relieve interior.
* Por elemento: `roughness` y `coast` (tipo) en el bloque, mezclados con los pesos de 7.3.

### 7.11 Tamaño de bin y coste

* `bin = max(R_med, D/2)` acotado a [500 m, 20 km]; el elemento se inscribe en todos los bins a menos de
  `R_i + k_i + D` de su centro.
* Objetivo: ≤ 16 elementos por bin de media; se mide y se registra en los metadatos (`lands.bins.mean`,
  `lands.bins.max`).
* Coste estimado: una distancia de elemento son unas 20 a 60 operaciones; 16 por muestra es comparable a un
  fBm de 4 octavas.

---

## 8. API propuesta

Borrador: los nombres se fijan al implementar y se documentan en `docs/lua_reference.md`. Código y claves en
inglés.

### 8.1 Llamada

```lua
local land = Lands.Compose{
    seed  = 1,
    frame = { size = 24000, edge_margin = 1500 },     -- o { world_cell = 16000, region = { x0, z0, x1, z1 } }
    sea   = { margin = "compressed" },                 -- margen por defecto
    profile = { "dome", height = 600, width = 4000 },  -- por defecto para los elementos sin perfil propio
    elements = { ... },                                -- 8.2
    edit = { ... }, fixed = { ... }, remove = { ... }, -- 8.4
    land_fraction = 0.4,                               -- opcional, solo acotado (7.8)
    classes = { continent = 5000, island = 1 },        -- km², opcional
}
return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
```

### 8.2 Elementos

```lua
{ kind = "radial", id = "main", at = { 0, 0 }, area = 120e6, bumps = 3, amplitude = 0.25,
  profile = { "rim", rim = 450, at = 400, sigma = 300, inner = 120, width = 3000 }, smooth = 800 },
{ kind = "sector", water = true, role = "bay", at = { 0, 0 }, angle = 200, width = 25, reach = 6000 },
{ kind = "capsule", from = { 0, -4000 }, to = { 0, -11000 }, width = 2500, profile = "plateau" },
{ kind = "ellipse", water = true, role = "sea", at = { 3000, 2000 }, radii = { 2500, 1500 } },
{ kind = "scatter", id = "islets", count = 8, area = { 0.5e6, 3e6 }, spacing = 600,
  pattern = "cluster", region = { kind = "ring", at = { 0, 0 }, radius = 9000, width = 3000 },
  shape = { kind = "radial", bumps = { 2, 4 }, amplitude = { 0.1, 0.3 } }, profile = "cone" },
{ kind = "ridge", points = { { -6000, 0 }, { 0, 1500 }, { 6000, 0 } }, width = 2500, height = 900,
  spurs = true, saddles = 2 },
{ kind = "seamount", at = { 15000, 4000 }, radius = 3000, height = 1800 },
```

Campos comunes: `id`, `water`, `role`, `area` (o las dimensiones de la forma), `rotation`, `scale`,
`mirror = "x" | "z" | "both"`, `smooth` (k, m), `profile`, `margin`, `coast`, `roughness`, `name`.

`area` y las dimensiones son excluyentes: con `area`, el tamaño se resuelve con la fórmula cerrada de 7.1.

### 8.3 Presets (sección 11)

`Lands.Island`, `Lands.Archipelago`, `Lands.Continent` y los nuevos (`Lands.Preset{ name = "mediterranean",
... }`) devuelven la tabla de `Compose` rellenada, que se puede modificar antes de construir:

```lua
local t = Lands.Preset{ name = "high_island", seed = 3, size = 24000 }
t.elements[1].area = 200e6
return Lands.Compose(t):outputs{ ... }
```

### 8.4 Ids, lista resuelta y edición

* Elemento declarado: su `id` o su índice. Generado: `"<scatter>/<ranura>"`.
* `land:elements()` devuelve la lista resuelta; la exportación la escribe en los metadatos (sección 10):

  ```json
  { "id": "islets/3", "kind": "radial", "role": "isle", "area_m2": 1.42e6, "area_geometric_m2": 1.40e6,
    "centroid": [4120, -880], "radius": 690, "max_height": 212, "nearest": ["islets/1", 1340],
    "component": 4 }
  ```

* Edición:

  ```lua
  edit   = { ["islets/3"] = { scale = 1.2 }, main = { profile = "rim" } },
  fixed  = { "islets/1" },   -- no cambia aunque cambien seed, count o land_fraction
  remove = { "islets/4" },
  ```

### 8.5 Salidas

Las actuales (`land_mask`, `land_height`, `bathymetry`, `surface_height`, `shore_mask`, `coast_type`,
`region_id`, `coast_sdf`). `region_id` pasa a ser el id numérico del elemento dominante (tabla id → nombre en
los metadatos). Salidas nuevas (`temperature`, `moisture`, `biome`) en la fase 7.

### 8.6 De instrucción a parámetros

| Instrucción | Cambio |
| --- | --- |
| "Un archipiélago de 6 islas de 2 a 4 km²" | `scatter{ count = 6, area = { 2e6, 4e6 } }` |
| "Que estén agrupadas" | `pattern = "cluster"` |
| "Agranda la isla 3 un poco" | `edit = { ["islets/3"] = { scale = 1.15 } }` (área ×1.32) |
| "Que la isla 3 tenga 5 km²" | `edit = { ["islets/3"] = { area = 5e6 } }` |
| "Acantilados más altos que el centro" | `profile = { "rim", ... }` |
| "Una isla plana" | `profile = "flat"` o preset `low_island` |
| "Un continente con un mar interior" | `superellipse` + `ellipse{ water = true, role = "sea" }` |
| "Todo tierra, sin océano" | preset `all_land` |
| "Todo tierra con lagos" | `all_land` + `scatter{ water = true, role = "lake" }` |
| "Una península al sur" | `capsule{ from = costa sur, to = ..., width = ... }` |
| "Une las islas 1 y 2 con un istmo" | `smooth` en ambas o `capsule` entre ellas |
| "Separa las islas 1 y 4" | `edit` con `offset`, o más `spacing` |
| "Un monte submarino al este" | `seamount{ at = ..., radius = ..., height = ... }` |
| "Más tierra" | `land_fraction` mayor (acotado) o `count`/`area` mayores (infinito) |

---

## 9. Plano infinito y mapas acotados

Reglas comunes:

1. **Nada depende de la ventana pedida**: sin cuantiles ni normalizaciones sobre lo exportado.
2. **Nivel del mar absoluto** (0 m).
3. **Todo lo no local tiene radio acotado** (`D`, el anillo de celdas).
4. **Generadores por celda del mundo**, como `Water.Network` con `generate.world_cell`.

**Acotado** (`frame.size`, centrado en el origen o `frame.bounds`):

* `land_fraction` exacta por bisección (7.8).
* Porcentajes del marco aceptados (`at = { "30%", "50%" }`) y resueltos a metros.
* `edge_margin`: distancia mínima de la tierra al borde.

**Infinito** (`frame.world_cell`):

* Los elementos generados se crean para las celdas que tocan `frame.region` más un anillo. La `region` es la
  ventana de trabajo del script (por defecto ±64 km, más que lo que muestra el visor), no un límite del mundo:
  dos regiones distintas que comparten celdas obtienen los mismos elementos, así que el mundo se puede exportar
  por piezas.
* La fracción de tierra es estadística (7.8).
* Los elementos declarados existen en sus coordenadas absolutas.
* Mejora futura, solo si la `region` se queda corta para el visor: generar los elementos de `scatter`
  directamente en el shader por hash de celda (los mismos números que la CPU), sin lista. No es necesario para
  las fases 1 a 3.

---

## 10. Herramientas para que Claude trabaje afinado

Lo mínimo necesario para que una instrucción se aplique y se compruebe sin mirar imágenes a ciegas:

1. **Lista de elementos en los metadatos.** Binding pequeño `engine.report(key, table)` que añade una
   sección `"script_report"` al `metadata.json` de la exportación. `Lands.Compose` escribe ahí la lista
   resuelta (8.4), el número de bins y los avisos (elementos omitidos, fracción no alcanzable).
2. **`terrain_metrics --by-id`**: amplía la herramienta existente para leer la salida `region_id` y dar, por
   id, área, centroide, caja, altura máxima y si toca el borde. Reutiliza `metrics::Measure` y el etiquetado.
3. **`tools/dev.sh lands <script> [--param k=v]...`**: exporta, ejecuta `terrain_metrics --by-id`, compara la
   lista del script con lo medido (área geométrica frente a medida, ids presentes) y escribe un resumen corto;
   con `--hillshade`, la imagen de revisión. Reutiliza el subcomando `review`.
4. **`tools/dev.sh lands-check <script> --expect count=6,area=2e6..4e6`**: comprobación de instrucción (lo
   mismo que hace el test, desde la terminal) para que Claude valide su cambio antes de dar algo por hecho.
5. **Skill `terrain-author`**: sección "Lands" con el flujo (preset más cercano → `elements()` → `edit` →
   `lands-check` → detalle) y la tabla 8.6.
6. **Errores accionables**: cada clave inválida o combinación imposible falla con qué se esperaba, como el
   resto del prelude (`check_args` guiado por tabla en `Lands`, extensible al resto en la fase 8).

---

## 11. Fases prioritarias (1 a 3)

Cada paso es un commit. Cada fase termina con su documentación, imágenes, `status.md` y `todo.md`.

### Fase 1: una isla, de punta a punta

**Objetivo.** Una tierra de uno o pocos elementos con perfil libre, margen básico, ids, edición, en mapa
acotado y en plano infinito, sobre el op `Lands`.

| Paso | Contenido | Reutiliza |
| --- | --- | --- |
| 1.1 | Unificar `hasher`, `make_noise`, `smooth` en un fichero del prelude cargado antes de `30_ranges` | las tres copias actuales |
| 1.2 | `lib/shape.lib.glsl` con las distancias de `shape.comp.glsl`; `Shape` lo incluye sin cambiar resultados (goldens de `shapes_r2` iguales) | `shape.comp.glsl` |
| 1.3 | Op `Lands` modo `coast` con `circle`, `ellipse`, `box`, `polygon`, `superellipse`, `radial`, `capsule`, `ring`, `sector`, `frame`; CSG ordenado con smin/smax; bins; referencia de CPU en `sampler.cpp`; binding `Lands.Block` | patrón `Water`, helper `block`, `SmoothMinimum` |
| 1.4 | `test_geometry`: cada primitiva frente a su distancia cerrada en puntos elegidos; GPU frente a CPU; área de cada primitiva frente a su fórmula | `test_geometry`, `Terrain.Sample` |
| 1.5 | `Lands.Compose` mínimo: `frame`, `elements` declarados, `area` → tamaño, salidas actuales a partir del modo `coast` | funciones de salida de `50_lands.lua` |
| 1.6 | Modo `relief`: perfiles de 7.4, pesos de 7.3, hipsometría | `Curves.Profile` (fórmulas) |
| 1.7 | Modo `seabed`: margen de 7.7 con presets `compressed`/`geologic`, `passive`/`active`, laguna, `seamount`/`guyot` | `MARGINS` |
| 1.8 | Ids, lista resuelta, `edit`/`fixed`/`remove`, `engine.report`, `terrain_metrics --by-id`, `dev.sh lands` y `lands-check` | `terrain_metrics`, `dev.sh` |
| 1.9 | Plano infinito: `frame.world_cell` con elementos declarados; exportaciones solapadas idénticas | regla de `world_cell` |
| 1.10 | `Lands.Island` sobre `Compose` con presets `volcano`, `high_island`, `low_island`, `atoll`, `rim_island`, `mesa_island`; claves retiradas fallan diciendo qué usar | coast type y warp actuales |
| 1.11 | Ejemplos de la fase (13.2) con sus goldens; documentación | — |

Criterios de aceptación:

* Distancia de cada primitiva exacta (o, en las de 1.er orden, error < 1 % del tamaño) en la costa.
* Área de cada primitiva (sin detalle) dentro del 1 % de su fórmula cerrada.
* `edit{ scale = 1.2 }` multiplica el área por 1.44 ± 2 %.
* `rim`: la altura máxima a menos de `at + 2σ` de la costa y por encima del interior; `dome`: en el interior.
* `all_land` y `ocean_only` exportan sin error con `land_mask` 1 y 0 en todo el mapa.
* Infinito: dos exportaciones solapadas idénticas byte a byte en la zona común; distintos tamaños de sección
  idénticos.
* Script de la isla por defecto en menos de 0.5 s en Release (hoy 6 s).
* Suite completa verde en Debug y Release.

### Fase 2: archipiélagos

| Paso | Contenido | Reutiliza |
| --- | --- | --- |
| 2.1 | `scatter` acotado: tamaños log-uniformes, dart throwing con rejilla, `spacing`, `edge_margin`, `region`; ids por ranura | layout `scattered` |
| 2.2 | `scatter` infinito: Poisson por celda con prioridad por hash | `world_cell` |
| 2.3 | Patrones `cluster`, `line`, `arc`, `ring`, `grid`, `centers` | `hotspot`, `arc`, `atoll` actuales |
| 2.4 | `land_fraction` por bisección (acotado) y por calibración (infinito) | `Terrain.Sample` |
| 2.5 | Clasificación de 7.9 | etiquetado actual |
| 2.6 | `Lands.Archipelago` como preset: `scattered`, `cluster`, `hotspot`, `arc`, `atoll`, `shattered`, `chain` (islas lado a lado, `chain_width`) | — |
| 2.7 | Ejemplos de la fase y goldens (`lands_archipelago_*` regenerados) | `tests/datasets` |

Criterios de aceptación:

* "N islas de A₁ a A₂ km²": N islas en la lista, todas con área geométrica en el rango, y `--by-id` mide lo
  mismo con un margen de 3 %.
* Separación mínima respetada en todos los pares (canal mínimo de `metrics::Measure` ≥ `spacing` − 1 muestra).
* `count` de N a N + 1: las N primeras con el mismo centroide y área.
* Infinito: la misma isla, igual desde dos ventanas; fracción de tierra medida sobre 200 × 200 km dentro del
  ±10 % del objetivo.
* `chain` y `arc` con `chain_width` alcanzan la fracción pedida (hoy se quedan en 6 a 12 %).

### Fase 3: continentes y otras tierras

| Paso | Contenido | Reutiliza |
| --- | --- | --- |
| 3.1 | Relieve generado en el modo `relief`: `ridge` con `spurs` y `saddles`, `massif`, `volcano`, `depression`, `escarpment`, `valley` | `Landforms`, `sample_spline` |
| 3.2 | Presets `continent`, `pangea`, `old_world`, `mediterranean`, `inland_sea`, `peninsula`, `isthmus`, `all_land`, `lakeland`, `fractious`, `taklamakan`, `ocean_only` como composiciones (tabla abajo) | — |
| 3.3 | `edge_margin` y porcentajes del marco | — |
| 3.4 | `Lands.Continent` como preset; golden `lands_continent` regenerado | — |
| 3.5 | Medir `docs/references/{islands,archipelagos,continents}` con `terrain_metrics` y fijar bandas de aceptación por tipo | `terrain_metrics` |
| 3.6 | Ejemplos de la fase, goldens, documentación | — |

Composición de cada preset (todas con primitivas; los números son de partida y editables):

| Preset | Composición |
| --- | --- |
| `continent` | `superellipse` (n 2.5 a 4) + 2 a 4 lóbulos `radial` unidos con `smooth`; 1 a 3 `ridge` con `spurs`; 0 a 2 golfos `sector`; margen pasivo y uno activo con fosa |
| `pangea` | 4 bloques `superellipse` en las esquinas + banda central, unidos; `trench` de agua parciales (mares interiores) |
| `old_world` | 3 bloques grandes; 2 a 3 `band` de agua (estrechos); `ridge` largo |
| `mediterranean` | `frame` de tierra + `ellipse` de agua alargada (`role = "sea"`) + islas por `scatter` dentro del mar |
| `inland_sea` | bloque continental + `superellipse` de agua interior |
| `peninsula` | bloque en un borde + `capsule`/`band` hacia el mar + islas en la punta |
| `isthmus` | dos bloques + `capsule` estrecha entre ellos |
| `all_land` | `frame` de tierra que cubre la ventana (o `fill` en infinito); relieve por elementos |
| `lakeland` | `all_land` + `scatter{ water = true, role = "lake" }` |
| `fractious` | 12 a 15 `radial` medianos por `scatter` con `smooth` grande + `band` de agua que los cortan |
| `taklamakan` | `all_land` con `ridge` en los cuatro bordes y `depression` central (cuenca cerrada) |
| `ocean_only` | sin tierra; `seamount`, `guyot`, margen ausente; llanura abisal |

Criterios de aceptación:

* `mediterranean`: un único cuerpo de agua interior clasificado `sea`, sin tocar el marco.
* `all_land`: `land_mask = 1` en todo el mapa; `lakeland`: los lagos con el área pedida.
* Una península de anchura W mide W ± 5 % en su eje.
* Cordillera: altura máxima de la cresta ± 2 % de la pedida; estribaciones a `spacing` ± 10 %.
* Métricas de forma dentro de las bandas medidas en las referencias del tipo.

---

## 12. Fases posteriores (4 a 8)

### Fase 4: detalle sobre las formas

1. Warp de costa de tres bandas con campo de rugosidad (7.10), por elemento.
2. Ruido de relieve con amplitud en metros ligada a la distancia al interior (heman: `s·n` acotado) y
   amortiguada en las cumbres (`(1 − h/H)^4`, heman).
3. Elemento `field`: umbral de ruido con `inflate`, `round` y umbral creciente
   (`n − (1 − inflate)·d² < 0`, mapgen2; `c > ratio·(1 + d²)`, voronoi-map), para costas totalmente
   procedurales cuando se pidan.
4. `Erosion.Gullies` y `Erosion.StreamPower` (vía `Terrain.Sample` → `StreamPower` → `Terrain.Grid`, como
   ahora, con la regla de `world_cell` para el infinito); `Ranges.Carve` para cordilleras.
5. Distancia exacta (Felzenszwalb + CPCF, acotada a `D`) en C++ solo si la erosión desplaza la costa y hay
   que volver a medirla.
6. Fetch: desde cada punto de costa, distancia de agua abierta a barlovento, por marcha de rayos sobre el modo
   `coast` en la rejilla de análisis.
7. Validación: desplazamiento máximo de la costa y amplitud de altura dentro de lo declarado.

### Fase 5: hidrología

1. `Water.Network` y `Water.Drain` sobre la superficie compuesta (ya analizan cualquier terreno con el
   muestreador): lagos y ríos como agua. Cierra "los macros no drenan".
2. Lagos cerrados, salados, secos y helados por balance de evaporación y aporte (Azgaar).
3. Mares interiores (`role = "sea"`) como masas de agua con su nivel, no como lagos.
4. Ancho de río `∝ caudal^0.7`.
5. Drenaje ponderado por precipitación cuando exista la fase 7.

### Fase 6: océano completo

1. Margen por tramo de costa (no solo por elemento).
2. Fondo con primitivas como elementos con id:

   | Forma | Primitiva |
   | --- | --- |
   | Fosa | `trench` sobre `arc` |
   | Dorsal oceánica | `ridge` largo con hendidura central (`trench` estrecho) y fallas transformantes como desplazamientos por tramos |
   | Cuenca | `depression` |
   | Cañón submarino | `band` desde el borde de la plataforma, profundidad creciente |
   | Arrecife | `ring` somero |

3. Montes submarinos y guyots por `scatter`.
4. Detalle: colinas abisales con amplitud ∝ profundidad (`emboss` de heman).
5. Futuro: sedimento y abanicos, erosión submarina, atolón por subsidencia.

### Fase 7: clima y biomas

1. Subir `kMaxOutputs` a 16 (o `outputs = { ... }`).
2. Temperatura por latitud (tramos de Azgaar) y −6.5 °C/km; latitud por coordenada del mundo.
3. Precipitación: marcha del viento con recarga sobre el agua, lluvia orográfica y sombra de lluvia (Azgaar),
   con radio acotado.
4. Humedad `1 − (d/max)^0.5` desde el agua dulce, `max` fijo en metros.
5. Biomas por tabla de Whittaker (mapgen2), como salida opcional.

### Fase 8: infraestructura

* `engine.memo(key, fn)` o `Terrain.Bake` si el visor se resiente.
* `check_args` guiado por tabla en todo el prelude.
* Mensaje de fallo de goldens con caja y diferencia máxima.
* Generación en shader de los elementos de `scatter` (9) si la `region` se queda corta.

---

## 13. Catálogo de ejemplos

### 13.1 Reglas comunes

* Ubicación: `assets/scripts/lands/` (sustituyen a los actuales; los nombres actuales se mantienen como
  presets).
* Cabecera de cada script: qué muestra, parámetros (`params.*`), comando del visor y de exportación, con la
  resolución recomendada para ese tamaño.
* Todos aceptan `--param seed=N` y los parámetros propios del tipo (`count`, `area`, `size`...).
* El visor muestra unos 28 km: los ejemplos de hasta 24 km se ven enteros; los mayores se revisan con
  `tools/dev.sh review` (hillshade) y en el visor por zonas con `--target`.
* Cada ejemplo tiene, como mínimo, un golden en `tests/datasets/lands_<nombre>/` a 256² (o 512² si la forma lo
  necesita), con `tolerance 0` y medición de rendimiento, como los actuales.
* Los ejemplos infinitos tienen además un test de solape y uno de estabilidad de ids.

Resoluciones de referencia por tamaño (visor con `--resolution`; golden a 256²):

| Tamaño | Visor | Golden (256²) |
| --- | --- | --- |
| 4 km (islote, atolón pequeño) | 2 m | 16 m |
| 12 km (isla pequeña) | 4 m | 48 m |
| 24 km (isla, archipiélago pequeño) | 8 m | 96 m |
| 48 km (archipiélago) | 16 m | 192 m |
| 96 km (región continental) | 32 m (por zonas) | 384 m |
| 192 km (continente) | 64 m (por zonas) | 768 m |

### 13.2 Fase 1: islas (acotado)

| Script | Qué muestra | Tamaño |
| --- | --- | --- |
| `island_dome.lua` | isla radial con perfil `dome` | 24 km |
| `island_rim.lua` | acantilados más altos que el interior (`rim`) | 12 km |
| `island_plateau.lua` | isla meseta (`plateau`) | 12 km |
| `island_caldera.lua` | volcán con caldera y laguna interior | 12 km |
| `island_tilt.lua` | cuesta: alta en un lado, playa en el otro | 24 km |
| `island_low.lua` | isla baja (`flat`), estilo cayo | 4 km |
| `island_volcano.lua` | cono con cráter, margen activo con fosa | 24 km |
| `island_bay.lua` | isla con bahías (`sector` de agua) y península (`capsule`) | 24 km |
| `atoll.lua` | anillo de islotes y laguna | 4 km |
| `island_edit.lua` | la misma isla con `edit` (escala, perfil) y `fixed`, para ver la edición | 24 km |
| `all_land.lua` | todo tierra con relieve por elementos | 24 km |
| `ocean_only.lua` | todo mar: margen ausente, montes submarinos y guyots | 48 km |

### 13.3 Fase 1: plano infinito

| Script | Qué muestra | Celda del mundo |
| --- | --- | --- |
| `infinite_islands.lua` | islas declaradas en coordenadas lejanas (±100 km) para probar el anclaje al mundo | 16 km |

### 13.4 Fase 2: archipiélagos

| Script | Qué muestra | Tamaño / celda |
| --- | --- | --- |
| `archipelago_scattered.lua` | `count` islas de un rango de áreas | 48 km |
| `archipelago_cluster.lua` | racimos (Neyman–Scott) | 48 km |
| `archipelago_hotspot.lua` | cadena que decrece | 48 km |
| `archipelago_arc.lua` | arco insular con fosa | 96 km |
| `archipelago_atoll.lua` | atolones dispersos | 24 km |
| `archipelago_shattered.lua` | tierra rota por canales | 48 km |
| `archipelago_chain.lua` | cadena ancha que alcanza la fracción pedida | 48 km |
| `archipelago_centers.lua` | islas en posiciones dadas | 24 km |
| `infinite_archipelago.lua` | archipiélago infinito por celdas | 16 km |
| `infinite_cluster.lua` | racimos infinitos | 24 km |

### 13.5 Fase 3: continentes y otras tierras

| Script | Qué muestra | Tamaño / celda |
| --- | --- | --- |
| `continent.lua` | bloque con lóbulos, cordilleras con estribaciones, golfos, margen mixto | 192 km |
| `pangea.lua` | supercontinente con mares interiores | 192 km |
| `old_world.lua` | bloques separados por estrechos | 192 km |
| `mediterranean.lua` | mar interior con islas | 96 km |
| `inland_sea.lua` | continente con mar interior cerrado | 96 km |
| `peninsula.lua` | península con islas en la punta | 48 km |
| `isthmus.lua` | dos masas unidas por un istmo | 48 km |
| `lakeland.lua` | todo tierra con lagos | 24 km |
| `fractious.lua` | tierras fragmentadas | 96 km |
| `taklamakan.lua` | cuenca cerrada rodeada de montañas | 96 km |
| `infinite_continents.lua` | continentes infinitos (elementos grandes por celda) | 128 km |
| `infinite_lakeland.lua` | tierra infinita con lagos | 16 km |

### 13.6 Galería

`assets/scripts/gallery.lua` genera una lámina por tipo para `docs/images/lands/` y `lua_reference.md`.

---

## 14. Validación y tests

| Test | Qué comprueba | Dónde |
| --- | --- | --- |
| Distancias de primitivas | valores cerrados en puntos elegidos; GPU = CPU | `test_geometry` |
| Áreas de primitivas | fórmula cerrada frente a `Terrain.Sample` | `test_geometry` |
| CSG | orden, smin con `k` (error ≤ `k/4`), resta | `test_geometry` |
| Perfiles | posición y valor del máximo por preset; continuidad entre elementos | `test_lands` |
| Margen | profundidad de plataforma, talud, abisal frente a lo pedido | `test_lands` |
| Instrucciones | un caso por fila de 8.6, con la lista resuelta y `metrics::Measure`/`--by-id` | `test_lands` |
| Estabilidad de ids | `count` N → N + 1, `seed` de otro generador, `fixed` | `test_lands` |
| Plano infinito | exportaciones solapadas idénticas; ids iguales desde dos ventanas; fracción estadística | `test_lands` (exportando como `test_cli`) |
| Secciones | tamaños de sección distintos idénticos | `test_seams` / `test_cli` |
| Goldens | cada ejemplo del catálogo, `tolerance 0`, rendimiento | `test_datasets` |
| Visor | un ejemplo acotado y uno infinito capturados con `--screenshot` contienen tierra y cambian con la cámara | `test_viewer` |
| Errores | claves inválidas, combinaciones imposibles, `world_cell` demasiado pequeña: mensaje y línea del script | `test_lua_errors` |
| Referencias | bandas por tipo medidas en `docs/references/` | `test_lands` |
| Tiempo | tiempo de script por golden registrado; ninguna fase lo empeora sin justificarlo | `test_datasets` |

Todas las suites en Debug y en Release, una cada vez, antes de cada commit que toque el motor.

---

## 15. Riesgos y decisiones abiertas

| Tema | Riesgo o decisión | Propuesta |
| --- | --- | --- |
| Op nuevo en C++/GLSL | cambio de capa (la composición sale del Lua puro) | justificado por el límite de nodos y el coste; mismo patrón que `Water`. Confirmar antes del paso 1.3 |
| `engine.report` | binding nuevo en el motor | pequeño; confirmar antes del paso 1.8 |
| Compatibilidad de `Lands.*` | goldens y scripts actuales cambian | regenerar goldens; claves retiradas fallan con la alternativa |
| Distancias de 1.er orden | error lejos de la costa en `radial`/`superellipse` | exactas en la costa; el perfil absorbe el resto; tests con tolerancia declarada |
| `smooth union` | no es una distancia exacta en la mezcla | error ≤ `k/4`; aceptable para forma; medir con `Terrain.Sample` |
| Bins con márgenes `geologic` | `D` de 100 km hace que todos los elementos caigan en todos los bins | medir; si pasa de 16 por bin, un bin grueso aparte solo para el margen |
| Fracción de tierra en infinito | solo estadística | documentarlo y medirla sobre ventanas grandes |
| `region` en infinito | la lista de elementos cubre la ventana de trabajo | ±64 km por defecto; generación en shader en la fase 8 si hace falta |
| `kMaxOutputs = 8` | Lands usa 7 | subir a 16 en la fase 7 |
| Tiempo de la suite | más goldens | goldens a 256² y op pointwise: deberían costar menos que los actuales (6 a 27 s de script cada uno) |

---

## 16. Flujo de trabajo

* **Un commit por paso**, nunca `--amend`; Conventional Commits en una línea.
* Si un commit cambia el estado de un milestone o un item, actualiza `docs/status.md` y `docs/todo.md` en el
  mismo commit.
* Iterar con `tools/dev.sh test -L fast`; antes de cada commit que toque el motor, suite completa en Debug y en
  Release, **una suite cada vez** (comparten `out/`).
* `volume_r3` en `test_datasets` puede fallar por tiempo en Release: repetir antes de investigar.
* Compilar con `build.cmd` (desde bash, `cmd //c` con ruta de Windows); `ctest` por su ruta completa.
* Código en inglés; nada de `new`/`delete`/`malloc` en el motor; `ENGINE_ASSERT` sin efectos secundarios.
* Un op nuevo necesita su entrada en `kOps` en el orden de `OpKind` (hay un `static_assert`), su shader en
  `engine_shaders` y su referencia en `sampler.cpp`.
* Al cerrar cada fase: ejemplos, goldens, imágenes, `lua_reference.md`, skill `terrain-author` y este plan.

---

## 17. Referencias

Repositorios analizados:

* mapgen2: <https://github.com/redblobgames/mapgen2> (`water.js`, `elevation.js`, `rivers.js`,
  `moisture.js`, `biomes.js`, `noisy-edges.js`); artículo "Polygonal Map Generation for Games" de Amit Patel.
* heman: <https://github.com/prideout/heman> (`include/heman.h`, `src/generate.c`, `src/ops.c`,
  `src/distance.c`).
* voronoi-map-js: <https://github.com/rjanicek/voronoi-map-js> (`src/island-shape.js`, `src/map.js`,
  `src/noisy-edges.js`, `src/lava.js`).
* Fantasy-Map-Generator: <https://github.com/Azgaar/Fantasy-Map-Generator>
  (`src/data/heightmap-templates.ts`, `src/model/physical/heightmap-generator.ts`, `features-generator.ts`,
  `lakes.ts`, `river-generator.ts`, `temperature-generator.ts`, `precipitation-generator.ts`,
  `coastline-generator.ts`, `ocean-generator.ts`, `biomes-generator.ts`).

Algoritmos citados:

* Í. Quílez, funciones de distancia 2D (`sdSegment`, `sdUnevenCapsule`, `sdArc`, `sdPie`, smin polinómico),
  iquilezles.org.
* R. Bridson, "Fast Poisson Disk Sampling in Arbitrary Dimensions" (2007).
* L.-Y. Wei, "Parallel Poisson Disk Sampling" (2008).
* Procesos de Neyman–Scott y Thomas (procesos puntuales en racimo); modelo booleano para la cobertura.
* P. Felzenszwalb, D. Huttenlocher, "Distance Transforms of Sampled Functions" (2012).
* Oclusión ambiental por barridos de horizonte de Sean Barrett (nothings.org).

Ficheros de YATE implicados:

* `assets/lua/prelude/10_shapes.lua`, `20_landforms.lua`, `30_ranges.lua`, `40_dunes.lua`, `45_water.lua`,
  `46_network.lua`, `50_lands.lua`
* `assets/shaders/ops/shape.comp.glsl`, `water.comp.glsl`, `arith.comp.glsl`, `assets/shaders/lib/`
* `src/terrain/kernels.cpp`, `src/terrain/sampler.cpp`, `src/terrain/graph.cpp`, `src/lua.cpp`,
  `include/engine/terrain/graph.hpp`, `include/engine/terrain/export.hpp`
* `assets/scripts/lands/*.lua`, `assets/scripts/gallery.lua`
* `tests/unit/test_geometry.cpp`, `test_lands.cpp`, `test_cli.cpp`, `test_viewer.cpp`, `test_seams.cpp`,
  `test_lua_errors.cpp`, `terrain_metrics.hpp`, `tests/tools/terrain_metrics.cpp`, `tests/datasets/lands_*`
* `tools/dev.sh`
* `docs/status.md`, `docs/todo.md`, `docs/lua_reference.md`, `docs/references/`, `docs/images/lands/`
* `.claude/skills/terrain-author/SKILL.md`
