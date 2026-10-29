// Traducciones del launcher. La clave es el texto en español (tal cual está en el XAML o en el código);
// detrás van inglés, francés, alemán e italiano. Textos.T("...") da el del idioma elegido.
// Si falta una traducción se queda en español. Los {0} son huecos para string.Format.
using System.Collections.Generic;
using System.Text.RegularExpressions;

static class Textos {
    // "es", "en", "fr", "de", "it"
    public static string idioma = "es";
    static readonly string[] orden = { "en", "fr", "de", "it" };

    public static string T(string es) {
        if (idioma == "es" || es == null) return es;
        string[] t;
        if (!tabla.TryGetValue(Normaliza(es), out t)) return es;
        int i = System.Array.IndexOf(orden, idioma);
        return i >= 0 && i < t.Length && t[i] != null ? t[i] : es;
    }

    // los textos largos del XAML vienen partidos en varias líneas: se comparan sin espacios repetidos
    public static string Normaliza(string s) { return Regex.Replace(s.Trim(), @"\s+", " "); }

    static Dictionary<string, string[]> tabla = Crea();

    static Dictionary<string, string[]> Crea() {
        var d = new Dictionary<string, string[]>();
        System.Action<string, string, string, string, string> A = delegate(string es, string en, string fr, string de, string it) {
            d[Normaliza(es)] = new[] { en, fr, de, it };
        };
        // barra lateral y portada
        A("Inicio", "Home", "Accueil", "Start", "Home");
        A("Ajustes", "Settings", "Paramètres", "Einstellungen", "Impostazioni");
        A("Controles", "Controls", "Commandes", "Steuerung", "Comandi");
        A("Acerca de", "About", "À propos", "Info", "Informazioni");
        A("versión de prueba", "test version", "version de test", "Testversion", "versione di prova");
        A("PORT NATIVO PARA PC", "NATIVE PC PORT", "PORTAGE PC NATIF", "NATIVER PC-PORT", "PORT NATIVO PER PC");
        A("JUGAR", "PLAY", "JOUER", "SPIELEN", "GIOCA");
        A("Repeticiones", "Replays", "Rediffusions", "Wiederholungen", "Replay");
        A("Listo para jugar", "Ready to play", "Prêt à jouer", "Bereit zum Spielen", "Pronto per giocare");
        A("JUEGO", "GAME", "JEU", "SPIEL", "GIOCO");
        A("GRÁFICOS", "GRAPHICS", "GRAPHISMES", "GRAFIK", "GRAFICA");
        A("MODO", "MODE", "MODE", "MODUS", "MODALITÀ");
        A("JUGADOR", "PLAYER", "JOUEUR", "SPIELER", "GIOCATORE");
        A("ROM lista", "ROM ready", "ROM prête", "ROM bereit", "ROM pronta");
        A("Falta la ROM", "ROM missing", "ROM manquante", "ROM fehlt", "ROM mancante");
        A("Resolución original", "Original resolution", "Résolution d'origine", "Originalauflösung", "Risoluzione originale");
        A("3D a {0}×", "3D at {0}×", "3D en {0}×", "3D in {0}×", "3D a {0}×");
        A("Pantalla completa", "Full screen", "Plein écran", "Vollbild", "Schermo intero");
        A("Ventana", "Window", "Fenêtre", "Fenster", "Finestra");
        A("Sin nombre", "No name", "Sans nom", "Kein Name", "Senza nome");
        A("Pon tu nombre para poder jugar", "Enter your name to play", "Entre ton nom pour jouer",
          "Gib deinen Namen ein, um zu spielen", "Inserisci il tuo nome per giocare");
        A("Lo necesita el juego en línea.", "Online play needs it.", "Il est nécessaire pour le jeu en ligne.",
          "Er wird für das Online-Spiel benötigt.", "Serve per il gioco online.");
        A("Hasta 10 letras. Es el nombre que sale en las carreras y se guarda en tu partida. Es obligatorio.",
          "Up to 10 letters. It's the name shown in races and saved in your save file. Required.",
          "Jusqu'à 10 lettres. C'est le nom affiché pendant les courses et enregistré dans ta sauvegarde. Obligatoire.",
          "Bis zu 10 Buchstaben. Dieser Name erscheint in den Rennen und wird in deinem Spielstand gespeichert. Pflichtfeld.",
          "Fino a 10 lettere. È il nome che appare nelle gare e viene salvato nei tuoi dati. Obbligatorio.");

        // ajustes
        A("Elegir ROM…", "Choose ROM…", "Choisir la ROM…", "ROM wählen…", "Scegli ROM…");
        A("Tu juego", "Your game", "Ton jeu", "Dein Spiel", "Il tuo gioco");
        A("Una copia de tu cartucho de Mario Kart DS europeo (.nds). Se comprueba y se copia a la carpeta del juego; tu archivo no se toca.",
          "A copy of your European Mario Kart DS cartridge (.nds). It is checked and copied to the game folder; your file is left untouched.",
          "Une copie de ta cartouche européenne de Mario Kart DS (.nds). Elle est vérifiée puis copiée dans le dossier du jeu ; ton fichier n'est pas modifié.",
          "Eine Kopie deines europäischen Mario-Kart-DS-Moduls (.nds). Sie wird geprüft und in den Spielordner kopiert; deine Datei bleibt unverändert.",
          "Una copia della tua cartuccia europea di Mario Kart DS (.nds). Viene verificata e copiata nella cartella del gioco; il tuo file non viene toccato.");
        A("Mario Kart DS (Europa) listo para jugar", "Mario Kart DS (Europe) ready to play", "Mario Kart DS (Europe) prêt à jouer",
          "Mario Kart DS (Europa) spielbereit", "Mario Kart DS (Europa) pronto per giocare");
        A("Todavía no has elegido tu ROM", "You haven't chosen your ROM yet", "Tu n'as pas encore choisi ta ROM",
          "Du hast noch kein ROM gewählt", "Non hai ancora scelto la tua ROM");
        A("Resolución del 3D", "3D resolution", "Résolution 3D", "3D-Auflösung", "Risoluzione 3D");
        A("Pinta los circuitos, los karts y los personajes a más resolución. Los menús y el marcador se ven como en la DS, sin difuminar.",
          "Renders tracks, karts and characters at a higher resolution. Menus and the HUD look like on the DS, without blur.",
          "Affiche les circuits, les karts et les personnages en plus haute résolution. Les menus et l'interface restent comme sur DS, sans flou.",
          "Zeichnet Strecken, Karts und Figuren in höherer Auflösung. Menüs und Anzeigen sehen aus wie auf dem DS, ohne Unschärfe.",
          "Disegna piste, kart e personaggi a una risoluzione più alta. Menu e interfaccia restano come su DS, senza sfocature.");
        A("Original", "Original", "Originale", "Original", "Originale");
        A("Imágenes por segundo", "Frames per second", "Images par seconde", "Bilder pro Sekunde", "Fotogrammi al secondo");
        A("Por encima de 60 se inventan imágenes intermedias del 3D: va más fluido, pero pide más al PC y hace falta un monitor de esa frecuencia.",
          "Above 60, in-between 3D frames are generated: it's smoother, but it needs a stronger PC and a monitor with that refresh rate.",
          "Au-delà de 60, des images 3D intermédiaires sont générées : c'est plus fluide, mais ça demande plus au PC et un écran à cette fréquence.",
          "Über 60 werden 3D-Zwischenbilder erzeugt: Es läuft flüssiger, braucht aber einen stärkeren PC und einen Monitor mit dieser Bildwiederholrate.",
          "Oltre 60 vengono generati fotogrammi 3D intermedi: è più fluido, ma richiede un PC più potente e un monitor con quella frequenza.");
        A("Idioma", "Language", "Langue", "Sprache", "Lingua");
        A("Juego", "Game", "Jeu", "Spiel", "Gioco");
        A("Empezar en pantalla completa", "Start in full screen", "Démarrer en plein écran", "Im Vollbild starten", "Avvia a schermo intero");
        A("Las dos pantallas lado a lado: la principal a la izquierda y la táctil a la derecha. F11 cambia mientras juegas.",
          "Both screens side by side: the main one on the left and the touch screen on the right. F11 switches while playing.",
          "Les deux écrans côte à côte : le principal à gauche et l'écran tactile à droite. F11 bascule en jeu.",
          "Beide Bildschirme nebeneinander: der Hauptbildschirm links, der Touchscreen rechts. F11 schaltet während des Spiels um.",
          "I due schermi affiancati: quello principale a sinistra e quello touch a destra. F11 cambia mentre giochi.");
        A("Tu nombre", "Your name", "Ton nom", "Dein Name", "Il tuo nome");
        A("Micrófono del PC", "PC microphone", "Micro du PC", "PC-Mikrofon", "Microfono del PC");
        A("Desactivado", "Off", "Désactivé", "Aus", "Disattivato");
        A("El predeterminado de Windows", "Windows default", "Celui par défaut de Windows", "Windows-Standard",
          "Quello predefinito di Windows");
        A("En la Batalla de globos, sopla al micrófono para inflar el globo. Solo se usa mientras el juego lo pide.",
          "In Balloon Battle, blow into the microphone to inflate your balloon. It's only used while the game asks for it.",
          "En Bataille de ballons, souffle dans le micro pour gonfler ton ballon. Il n'est utilisé que quand le jeu le demande.",
          "Im Ballonkampf ins Mikrofon pusten, um den Ballon aufzublasen. Es wird nur benutzt, wenn das Spiel es braucht.",
          "Nella Battaglia palloncini, soffia nel microfono per gonfiare il palloncino. Si usa solo quando il gioco lo richiede.");
        A("En la Batalla de globos, sopla al micrófono o pulsa Select para inflar un globo.",
          "In Balloon Battle, blow into the microphone or press Select to inflate a balloon.",
          "En Bataille de ballons, souffle dans le micro ou appuie sur Select pour gonfler un ballon.",
          "Im Ballonkampf ins Mikrofon pusten oder Select drücken, um einen Ballon aufzublasen.",
          "Nella Battaglia palloncini, soffia nel microfono o premi Select per gonfiare un palloncino.");
        A("Hasta 10 letras. Es el nombre que sale en las carreras y se guarda en tu partida.",
          "Up to 10 letters. It's the name shown in races and saved in your save file.",
          "Jusqu'à 10 lettres. C'est le nom affiché pendant les courses et enregistré dans ta sauvegarde.",
          "Bis zu 10 Buchstaben. Dieser Name erscheint in den Rennen und wird in deinem Spielstand gespeichert.",
          "Fino a 10 lettere. È il nome che appare nelle gare e viene salvato nei tuoi dati.");

        // controles
        A("EN LA DS", "ON THE DS", "SUR LA DS", "AUF DEM DS", "SU DS");
        A("TECLADO", "KEYBOARD", "CLAVIER", "TASTATUR", "TASTIERA");
        A("MANDO (PS5 / PS4 / XBOX)", "CONTROLLER (PS5 / PS4 / XBOX)", "MANETTE (PS5 / PS4 / XBOX)", "CONTROLLER (PS5 / PS4 / XBOX)",
          "CONTROLLER (PS5 / PS4 / XBOX)");
        A("Cruceta", "D-pad", "Croix directionnelle", "Steuerkreuz", "Croce direzionale");
        A("Flechas", "Arrow keys", "Flèches", "Pfeiltasten", "Frecce");
        A("Cruceta o stick izquierdo", "D-pad or left stick", "Croix ou stick gauche", "Steuerkreuz oder linker Stick", "Croce o levetta sinistra");
        A("✕ y ○  ·  A y B", "✕ and ○  ·  A and B", "✕ et ○  ·  A et B", "✕ und ○  ·  A und B", "✕ e ○  ·  A e B");
        A("△ y □  ·  X e Y", "△ and □  ·  X and Y", "△ et □  ·  X et Y", "△ und □  ·  X und Y", "△ e □  ·  X e Y");
        A("Retroceso", "Backspace", "Retour arrière", "Rücktaste", "Backspace");
        A("Pantalla táctil", "Touch screen", "Écran tactile", "Touchscreen", "Schermo touch");
        A("Ratón sobre la pantalla de abajo", "Mouse on the bottom screen", "Souris sur l'écran du bas",
          "Maus auf dem unteren Bildschirm", "Mouse sullo schermo inferiore");
        A("Extras", "Extras", "Extras", "Extras", "Extra");
        A("pantalla completa", "full screen", "plein écran", "Vollbild", "schermo intero");
        A("guardar los últimos 30 s en vídeo", "save the last 30 s as a video", "enregistrer les 30 dernières s en vidéo",
          "die letzten 30 s als Video speichern", "salva gli ultimi 30 s in video");
        A("En la Batalla de globos, Select infla un globo (el micrófono del PC llegará más adelante).",
          "In Balloon Battle, Select inflates a balloon (PC microphone support is coming later).",
          "En Bataille de ballons, Select gonfle un ballon (le micro du PC arrivera plus tard).",
          "Im Ballonkampf bläst Select einen Ballon auf (das PC-Mikrofon kommt später).",
          "Nella Battaglia palloncini, Select gonfia un palloncino (il microfono del PC arriverà più avanti).");

        // acerca de
        A("Qué es", "What it is", "Qu'est-ce que c'est", "Was ist das", "Cos'è");
        A("Una recompilación estática: el código del juego de DS se traduce a C y se compila para Windows, sin emulador. Encima van mejoras propias: 3D a más resolución, pantalla completa con las dos pantallas lado a lado, mandos de PS5, PS4 y Xbox y repeticiones instantáneas.",
          "A static recompilation: the DS game's code is translated to C and compiled for Windows, with no emulator. On top come our own improvements: higher-resolution 3D, full screen with both screens side by side, PS5, PS4 and Xbox controllers and instant replays.",
          "Une recompilation statique : le code du jeu DS est traduit en C et compilé pour Windows, sans émulateur. Avec en plus nos propres améliorations : 3D en plus haute résolution, plein écran avec les deux écrans côte à côte, manettes PS5, PS4 et Xbox et rediffusions instantanées.",
          "Eine statische Rekompilierung: Der Code des DS-Spiels wird nach C übersetzt und für Windows kompiliert, ganz ohne Emulator. Dazu kommen eigene Verbesserungen: 3D in höherer Auflösung, Vollbild mit beiden Bildschirmen nebeneinander, PS5-, PS4- und Xbox-Controller und Sofort-Wiederholungen.",
          "Una ricompilazione statica: il codice del gioco DS viene tradotto in C e compilato per Windows, senza emulatore. In più ci sono migliorie proprie: 3D ad alta risoluzione, schermo intero con i due schermi affiancati, controller PS5, PS4 e Xbox e replay istantanei.");
        A("El port no incluye el juego: necesita una copia sacada de tu propio cartucho. Proyecto de aficionado sin relación con Nintendo.",
          "The port does not include the game: it needs a copy dumped from your own cartridge. A fan project not affiliated with Nintendo.",
          "Le portage n'inclut pas le jeu : il faut une copie extraite de ta propre cartouche. Projet de fan sans lien avec Nintendo.",
          "Der Port enthält das Spiel nicht: Er braucht eine Kopie deines eigenen Moduls. Ein Fanprojekt ohne Verbindung zu Nintendo.",
          "Il port non include il gioco: serve una copia estratta dalla tua cartuccia. Progetto amatoriale non affiliato a Nintendo.");
        A("Hecho por", "Made by", "Créé par", "Erstellt von", "Creato da");

        // mensajes del código
        A("No se pudieron guardar los ajustes: ", "Couldn't save the settings: ", "Impossible d'enregistrer les paramètres : ",
          "Die Einstellungen konnten nicht gespeichert werden: ", "Impossibile salvare le impostazioni: ");
        A("Falta el juego compilado (build\\mkds.exe)", "The compiled game is missing (build\\mkds.exe)",
          "Le jeu compilé est introuvable (build\\mkds.exe)", "Das kompilierte Spiel fehlt (build\\mkds.exe)",
          "Manca il gioco compilato (build\\mkds.exe)");
        A("No se pudo leer: ", "Couldn't read it: ", "Lecture impossible : ", "Konnte nicht gelesen werden: ", "Impossibile leggerlo: ");
        A("Ese archivo no es una ROM de DS.", "That file isn't a DS ROM.", "Ce fichier n'est pas une ROM DS.",
          "Diese Datei ist kein DS-ROM.", "Questo file non è una ROM DS.");
        A("Esa ROM no es Mario Kart DS (su código es {0}).", "That ROM isn't Mario Kart DS (its code is {0}).",
          "Cette ROM n'est pas Mario Kart DS (son code est {0}).", "Dieses ROM ist nicht Mario Kart DS (sein Code ist {0}).",
          "Questa ROM non è Mario Kart DS (il suo codice è {0}).");
        A("Es Mario Kart DS, pero de otra región ({0}). El port es para la versión europea (AMCP).",
          "It's Mario Kart DS, but from another region ({0}). The port is for the European version (AMCP).",
          "C'est bien Mario Kart DS, mais d'une autre région ({0}). Le portage est fait pour la version européenne (AMCP).",
          "Es ist Mario Kart DS, aber aus einer anderen Region ({0}). Der Port ist für die europäische Version (AMCP).",
          "È Mario Kart DS, ma di un'altra regione ({0}). Il port è per la versione europea (AMCP).");
        A("La ROM está incompleta o dañada.", "The ROM is incomplete or damaged.", "La ROM est incomplète ou endommagée.",
          "Das ROM ist unvollständig oder beschädigt.", "La ROM è incompleta o danneggiata.");
        A("La ROM está incompleta o dañada (le faltan datos del juego).", "The ROM is incomplete or damaged (game data is missing).",
          "La ROM est incomplète ou endommagée (il manque des données du jeu).",
          "Das ROM ist unvollständig oder beschädigt (es fehlen Spieldaten).",
          "La ROM è incompleta o danneggiata (mancano dati del gioco).");
        A("Es la versión europea, pero no la misma revisión o está modificada.",
          "It's the European version, but a different revision or modified.",
          "C'est la version européenne, mais une autre révision ou une version modifiée.",
          "Es ist die europäische Version, aber eine andere Revision oder verändert.",
          "È la versione europea, ma di un'altra revisione o modificata.");
        A("Elige tu ROM de Mario Kart DS (versión europea)", "Choose your Mario Kart DS ROM (European version)",
          "Choisis ta ROM de Mario Kart DS (version européenne)", "Wähle dein Mario-Kart-DS-ROM (europäische Version)",
          "Scegli la tua ROM di Mario Kart DS (versione europea)");
        A("ROM de Nintendo DS (*.nds)|*.nds|Todos los archivos|*.*", "Nintendo DS ROM (*.nds)|*.nds|All files|*.*",
          "ROM Nintendo DS (*.nds)|*.nds|Tous les fichiers|*.*", "Nintendo-DS-ROM (*.nds)|*.nds|Alle Dateien|*.*",
          "ROM Nintendo DS (*.nds)|*.nds|Tutti i file|*.*");
        A("No se pudo copiar la ROM: ", "Couldn't copy the ROM: ", "Impossible de copier la ROM : ",
          "Das ROM konnte nicht kopiert werden: ", "Impossibile copiare la ROM: ");
        A("ROM comprobada: lista para jugar", "ROM checked: ready to play", "ROM vérifiée : prêt à jouer",
          "ROM geprüft: spielbereit", "ROM verificata: pronto per giocare");
        A("No encuentro build\\mkds.exe: hay que compilar el juego primero.",
          "Can't find build\\mkds.exe: the game has to be compiled first.",
          "build\\mkds.exe introuvable : il faut d'abord compiler le jeu.",
          "build\\mkds.exe nicht gefunden: Das Spiel muss zuerst kompiliert werden.",
          "Non trovo build\\mkds.exe: bisogna prima compilare il gioco.");
        A("No se pudo abrir el juego: ", "Couldn't start the game: ", "Impossible de lancer le jeu : ",
          "Das Spiel konnte nicht gestartet werden: ", "Impossibile avviare il gioco: ");
        A("Jugando… el launcher vuelve al cerrar el juego", "Playing… the launcher comes back when you close the game",
          "En jeu… le launcher revient quand tu fermes le jeu", "Spiel läuft… der Launcher kommt zurück, wenn du das Spiel schließt",
          "In gioco… il launcher torna quando chiudi il gioco");
        A("El juego se cerró con un error (código {0})", "The game closed with an error (code {0})",
          "Le jeu s'est fermé avec une erreur (code {0})", "Das Spiel wurde mit einem Fehler beendet (Code {0})",
          "Il gioco si è chiuso con un errore (codice {0})");
        // preparar el juego y actualizar (Preparar.cs)
        A("La primera vez que juegues se prepara el juego", "The game is prepared the first time you play", "Le jeu est préparé la première fois que tu joues", "Beim ersten Spielen wird das Spiel vorbereitet", "La prima volta che giochi il gioco viene preparato");
        A("Preparando el juego", "Preparing the game", "Préparation du jeu", "Spiel wird vorbereitet", "Preparazione del gioco");
        A("Solo la primera vez (y tras actualizar). Se traduce el código de tu ROM y se compila para tu PC: tarda unos minutos.",
          "Only the first time (and after updating). Your ROM's code is translated and compiled for your PC: it takes a few minutes.",
          "Seulement la première fois (et après une mise à jour). Le code de ta ROM est traduit et compilé pour ton PC : cela prend quelques minutes.",
          "Nur beim ersten Mal (und nach Updates). Der Code deines ROMs wird übersetzt und für deinen PC kompiliert: das dauert ein paar Minuten.",
          "Solo la prima volta (e dopo un aggiornamento). Il codice della tua ROM viene tradotto e compilato per il tuo PC: ci vuole qualche minuto.");
        A("Leyendo tu ROM…", "Reading your ROM…", "Lecture de ta ROM…", "ROM wird gelesen…", "Lettura della ROM…");
        A("Traduciendo el código del juego a C…", "Translating the game's code to C…", "Traduction du code du jeu en C…", "Spielcode wird nach C übersetzt…", "Traduzione del codice del gioco in C…");
        A("Compilando… {0} de {1}", "Compiling… {0} of {1}", "Compilation… {0} sur {1}", "Kompilieren… {0} von {1}", "Compilazione… {0} di {1}");
        A("Uniendo las piezas…", "Linking…", "Assemblage…", "Wird verknüpft…", "Collegamento…");
        A("Descargando {0}…", "Downloading {0}…", "Téléchargement de {0}…", "{0} wird heruntergeladen…", "Download di {0}…");
        A("Comprobando {0}…", "Checking {0}…", "Vérification de {0}…", "{0} wird geprüft…", "Verifica di {0}…");
        A("Descomprimiendo {0}…", "Extracting {0}…", "Extraction de {0}…", "{0} wird entpackt…", "Estrazione di {0}…");
        A("el compilador", "the compiler", "le compilateur", "den Compiler", "il compilatore");
        A("Cancelar", "Cancel", "Annuler", "Abbrechen", "Annulla");
        A("Preparación cancelada", "Preparation cancelled", "Préparation annulée", "Vorbereitung abgebrochen", "Preparazione annullata");
        A("No se pudo preparar el juego", "The game could not be prepared", "Impossible de préparer le jeu", "Das Spiel konnte nicht vorbereitet werden", "Impossibile preparare il gioco");
        A("No se pudo preparar el juego:", "The game could not be prepared:", "Impossible de préparer le jeu :", "Das Spiel konnte nicht vorbereitet werden:", "Impossibile preparare il gioco:");
        A("Los detalles están en build\\preparar.log.", "Details are in build\\preparar.log.", "Les détails sont dans build\\preparar.log.", "Details stehen in build\\preparar.log.", "I dettagli sono in build\\preparar.log.");
        A("Actualizar a la {0}", "Update to {0}", "Mettre à jour vers {0}", "Auf {0} aktualisieren", "Aggiorna alla {0}");
        A("Hay una versión nueva: {0}", "A new version is available: {0}", "Une nouvelle version est disponible : {0}", "Neue Version verfügbar: {0}", "È disponibile una nuova versione: {0}");
        A("Buscando actualizaciones…", "Checking for updates…", "Recherche de mises à jour…", "Suche nach Updates…", "Ricerca aggiornamenti…");
        A("No se pudo comprobar si hay actualizaciones", "Could not check for updates", "Impossible de vérifier les mises à jour", "Updates konnten nicht geprüft werden", "Impossibile cercare aggiornamenti");
        A("Tienes la última versión", "You have the latest version", "Tu as la dernière version", "Du hast die neueste Version", "Hai l'ultima versione");
        A("Actualizando a la {0}", "Updating to {0}", "Mise à jour vers {0}", "Aktualisierung auf {0}", "Aggiornamento alla {0}");
        A("Descargando la actualización…", "Downloading the update…", "Téléchargement de la mise à jour…", "Update wird heruntergeladen…", "Download dell'aggiornamento…");
        A("Instalando…", "Installing…", "Installation…", "Wird installiert…", "Installazione…");
        A("No se pudo actualizar:", "The update failed:", "La mise à jour a échoué :", "Das Update ist fehlgeschlagen:", "Aggiornamento non riuscito:");
        A("Se descargará la versión {0} y el launcher se reiniciará. Tu ROM, tus ajustes y tu partida no se tocan.",
          "Version {0} will be downloaded and the launcher will restart. Your ROM, settings and save are not touched.",
          "La version {0} sera téléchargée et le launcher redémarrera. Ta ROM, tes réglages et ta sauvegarde ne sont pas touchés.",
          "Version {0} wird heruntergeladen und der Launcher neu gestartet. ROM, Einstellungen und Spielstand bleiben unberührt.",
          "Verrà scaricata la versione {0} e il launcher si riavvierà. ROM, impostazioni e salvataggio non vengono toccati.");
        A("Al terminar, el launcher se abre de nuevo. La próxima vez que juegues se prepara lo que haya cambiado.",
          "When done, the launcher opens again. Whatever changed is prepared the next time you play.",
          "À la fin, le launcher se rouvre. Ce qui a changé sera préparé la prochaine fois que tu joues.",
          "Danach öffnet sich der Launcher neu. Was sich geändert hat, wird beim nächsten Spielen vorbereitet.",
          "Al termine il launcher si riapre. Ciò che è cambiato verrà preparato la prossima volta che giochi.");
        A("Actualizaciones", "Updates", "Mises à jour", "Updates", "Aggiornamenti");
        A("Buscar actualizaciones", "Check for updates", "Rechercher des mises à jour", "Nach Updates suchen", "Cerca aggiornamenti");
        A("El launcher mira al abrirse si hay una versión nueva y la instala con un botón. Tu ROM, tus ajustes y tu partida no se tocan.",
          "When it opens, the launcher checks for a new version and installs it with one button. Your ROM, settings and save are not touched.",
          "À l'ouverture, le launcher vérifie s'il y a une nouvelle version et l'installe d'un clic. Ta ROM, tes réglages et ta sauvegarde ne sont pas touchés.",
          "Beim Start prüft der Launcher auf eine neue Version und installiert sie per Knopfdruck. ROM, Einstellungen und Spielstand bleiben unberührt.",
          "All'avvio il launcher controlla se c'è una nuova versione e la installa con un pulsante. ROM, impostazioni e salvataggio non vengono toccati.");
        return d;
    }
}
