# Referencias de islas, archipiélagos y continentes

Material con el que se aceptan las macros `Lands.*`. Cada tipo de costa tiene su directorio con cuatro
ficheros de texto, que **sí se versionan**; las imágenes y los mapas de altura de terceros no se versionan
(`docs/references/.gitignore`) salvo que su licencia lo permita y se diga en `source.md`.

| Directorio | Qué es | Notas |
| --- | --- | --- |
| `volcanic_island/` | Isla volcánica aislada (tipo Hawái o Tenerife) | cono o escudo, costa de acantilado y playa de lava, plataforma estrecha, talud cercano |
| `rocky_headland/` | Costa rocosa recortada con cabos y calas | cabos convexos con acantilado, calas cóncavas, islotes frente al cabo, costa de alta dimensión |
| `sedimentary_bay/` | Bahía sedimentaria y costa de playa | costa suave, playas largas, barras y lagunas costeras, plataforma ancha y suave |
| `scattered_archipelago/` | Archipiélago disperso | muchas islas pequeñas y pocas grandes, canales, distancias mínimas entre masas |
| `volcanic_arc/` | Arco volcánico y cadena de punto caliente | islas siguiendo una curva o una trayectoria, edad y tamaño con tendencia |
| `atoll/` | Atolón | anillo de arrecife con laguna interior y pasos, sin relieve terrestre alto |

## Qué lleva cada directorio

| Fichero | Contenido |
| --- | --- |
| `source.md` | de dónde sale (fuente, licencia, zona y escala) y qué rasgo estudia |
| `measurement.md` | resolución a la que se midió, nivel del mar, escalas de box-counting y la salida de `terrain_metrics` |
| `notes.md` | rasgos que se quieren reproducir y rasgos que se ignoran |
| `reference.*` | la imagen o el mapa de altura (no se versiona por defecto) |

## Protocolo de medida (fijo, para que las cifras sean comparables)

1. Se remuestrea la referencia a **una resolución fija por tipo**, y se anota en `measurement.md`.
2. El nivel del mar es el de la referencia (0 si no se dice otra cosa).
3. `terrain_metrics <directorio o .raw> --box 2,7` da: tierra, masas, la mayor, lagunas, longitud de
   costa, dimensión de box-counting con su R², canal más estrecho, hipsometría y pendiente por
   profundidad.
4. Las cifras de dimensión fractal son **por tipo de costa y rango de escalas**, nunca un objetivo
   global: una costa rocosa recortada ronda 1.2 a 1.3; una sedimentaria o el borde de un atolón, menos.
   Cambian con la resolución, la ventana y el umbral, y por eso el rango de escalas se fija aquí.

## Criterios de aceptación

Salen de **medir las referencias**, no se fijan de antemano. Cada `measurement.md` termina con una
tabla `métrica | valor de la referencia | tolerancia` y esa tabla es el criterio del ejemplo del mismo
nombre. Mientras un directorio no tenga datos, su ejemplo se acepta solo por las propiedades
estructurales (una masa, el signo de la distancia, secciones idénticas), no por una cifra.
