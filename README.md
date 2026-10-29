<p align="center"><img src="logo/logo_horizontal.png" alt="DS Kart Recompiled" width="620"></p>

<p align="center"><b>English</b> · <a href="#español">Español</a></p>

# DS Kart Recompiled

**A native PC port of the European Mario Kart DS, made by static recompilation — no emulator.**

> 🚧 **Work in progress.** The code and a ready-to-play launcher will be published here when the project is
> finished. Star or watch the repository to know when it's out.

## What it is

Instead of emulating a Nintendo DS, the game's own code (for both of the DS processors, the ARM9 and the ARM7)
is translated into C and compiled into a normal Windows program. Around it, a small "runtime" does the job of the
console: memory, interrupts, timers, the 2D and 3D graphics engines, sound, saving and controls.

Because it runs natively, it can go beyond the original:

- **3D at 2×, 3× or 4× resolution** — menus and HUD stay pixel-perfect like on the DS
- **Up to 240 fps** — in-between 3D frames are generated while the game logic stays at 60
- **Full screen with both screens side by side**
- **PS5, PS4 and Xbox controllers**, and your **PC microphone** for Balloon Battle
- **Instant replays** — F10 saves the last 30 seconds as a video
- **Its own launcher** in English, Spanish, French, German and Italian
- **Everything unlocked** from the start

## What you will need

- Windows
- A dump of **your own** European *Mario Kart DS* cartridge (game code `AMCP`).
  **The game is not and will never be included here.**

## System requirements

|  | Minimum | Recommended |
|---|---|---|
| **OS** | Windows 10 64-bit | Windows 11 64-bit |
| **Processor** | 4 cores (Intel Core i5-4590 / AMD Ryzen 5 1500X) | 8 cores or more (Intel Core i7-8700 / AMD Ryzen 7 3700X) |
| **Memory** | 4 GB RAM | 8 GB RAM |
| **Graphics** | Any graphics card with OpenGL (integrated is fine) | Any graphics card with OpenGL (integrated is fine) |
| **Storage** | 1.5 GB free space | 1.5 GB free space |
| **Network** | Internet connection the first time (about 200 MB of downloads) | Broadband Internet connection |
| **Other** | | Controller (PS5, PS4 or Xbox) and microphone for Balloon Battle |

The 3D is drawn by the processor, so the processor matters much more than the graphics card.
The first time you press PLAY, the launcher translates and compiles the game on your PC:
it takes about 5 minutes with 8 cores (longer with fewer). After that the game starts instantly.
These figures are estimates: so far it has been tested on an Intel Core i7-14700KF with 32 GB of RAM.

## Planned

- Local multiplayer over the internet (for example with Radmin VPN)
- Online play through community servers

## Credits

- Made by **alontube** — [alontube.pages.dev](https://alontube.pages.dev), together with **Claude** (Anthropic's AI), who co-wrote most of the code
- Inspired by [WiiCompiled](https://github.com/patchzyy/wiicompiled), the Mario Kart Wii recompilation.
  It doesn't use its code: the idea of translating the game instead of emulating it comes from there.

## Disclaimer

Fan project **not affiliated with Nintendo**. *Mario Kart* and *Nintendo DS* are trademarks of Nintendo.

---

<a id="español"></a>

# DS Kart Recompiled (Español)

**Port nativo para PC de Mario Kart DS (versión europea) hecho por recompilación estática: sin emulador.**

> 🚧 **En desarrollo.** El código y un launcher listo para jugar se publicarán aquí cuando el proyecto esté
> terminado. Dale a la estrella o a "Watch" para enterarte cuando salga.

## Qué es

En vez de emular una Nintendo DS, el propio código del juego (el de los dos procesadores de la DS, el ARM9 y el
ARM7) se traduce a C y se compila como un programa normal de Windows. Alrededor, un pequeño "runtime" hace de
consola: memoria, interrupciones, temporizadores, los motores de gráficos 2D y 3D, el sonido, el guardado y los
controles.

Como funciona de forma nativa, puede ir más allá del original:

- **3D a 2×, 3× o 4× de resolución**: los menús y el marcador se ven como en la DS, sin difuminar
- **Hasta 240 imágenes por segundo**: se inventan imágenes intermedias del 3D mientras el juego sigue a 60
- **Pantalla completa con las dos pantallas lado a lado**
- **Mandos de PS5, PS4 y Xbox**, y el **micrófono del PC** para la Batalla de globos
- **Repeticiones instantáneas**: F10 guarda los últimos 30 segundos en vídeo
- **Launcher propio** en español, inglés, francés, alemán e italiano
- **Todo desbloqueado** de serie

## Qué hará falta

- Windows
- Una copia de **tu propio cartucho** de *Mario Kart DS* europeo (código `AMCP`).
  **El juego no está ni estará nunca aquí.**

## Requisitos del sistema

|  | Mínimo | Recomendado |
|---|---|---|
| **Sistema operativo** | Windows 10 de 64 bits | Windows 11 de 64 bits |
| **Procesador** | 4 núcleos (Intel Core i5-4590 / AMD Ryzen 5 1500X) | 8 núcleos o más (Intel Core i7-8700 / AMD Ryzen 7 3700X) |
| **Memoria** | 4 GB de RAM | 8 GB de RAM |
| **Gráficos** | Cualquier tarjeta gráfica con OpenGL (vale la integrada) | Cualquier tarjeta gráfica con OpenGL (vale la integrada) |
| **Almacenamiento** | 1,5 GB de espacio libre | 1,5 GB de espacio libre |
| **Red** | Conexión a Internet la primera vez (unos 200 MB de descargas) | Conexión de banda ancha a Internet |
| **Otros** | | Mando (PS5, PS4 o Xbox) y micrófono para la Batalla de globos |

El 3D lo dibuja el procesador, así que importa mucho más el procesador que la tarjeta gráfica.
La primera vez que le das a JUGAR, el launcher traduce y compila el juego en tu PC:
tarda unos 5 minutos con 8 núcleos (más con menos). Después el juego arranca al momento.
Son cifras aproximadas: de momento se ha probado en un Intel Core i7-14700KF con 32 GB de RAM.

## Planeado

- Multijugador local por internet (por ejemplo con Radmin VPN)
- Juego en línea con servidores de la comunidad

## Créditos

- Hecho por **alontube**: [alontube.pages.dev](https://alontube.pages.dev), junto con **Claude** (la IA de Anthropic), con quien ha escrito la mayor parte del código
- Inspirado en [WiiCompiled](https://github.com/patchzyy/wiicompiled), la recompilación de Mario Kart Wii.
  No usa su código: la idea de traducir el juego en vez de emularlo viene de ahí.

## Aviso

Proyecto de aficionado **sin relación con Nintendo**. *Mario Kart* y *Nintendo DS* son marcas de Nintendo.
