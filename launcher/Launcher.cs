// Launcher del port (DS Kart Recompiled): portada con botón de jugar, ajustes (resolución, pantalla completa,
// nombre), controles y "acerca de". Escribe ajustes.ini junto al juego; mkds.exe lo lee al arrancar
// (lee_ajustes en main_mkds.c).
// Se compila con el csc de .NET Framework 4 (viene con Windows): sh launcher/compilar.sh
// Ojo: ese csc solo entiende C# 5 (nada de $"...", ?. ni métodos con =>).
// Launcher.exe --captura salida.png [inicio|ajustes|controles|acerca]: dibuja la ventana en una imagen sin
// mostrarla (para revisar el diseño sin abrir nada en pantalla).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Microsoft.Win32;

partial class Launcher {
    const string TITULO = "DS Kart Recompiled";  // nombre del proyecto
    const string VERSION = " " + VERSION_NUM;  // VERSION_NUM está en Preparar.cs (la compara con GitHub)
    // La ROM que entiende el port: Mario Kart DS europeo (AMCP). El código traducido es de esa versión exacta,
    // así que se comprueba el código de los dos procesadores (CRC32), no el tamaño (hay ROMs recortadas).
    const string CODIGO_JUEGO = "AMCP";
    const uint CRC_ARM9 = 0x7DF73400, CRC_ARM7 = 0x3CB95F4C;

    static string carpeta;
    static Window ventana;
    static Dictionary<string, string> ajustes = new Dictionary<string, string>();
    static readonly string[] paginas = { "Inicio", "Ajustes", "Controles", "Acerca" };

    [STAThread]
    static void Main(string[] args) {
        carpeta = AppDomain.CurrentDomain.BaseDirectory;
        LeeAjustes();
        ventana = (Window)XamlReader.Parse(Xaml());
        Prepara();
        if (args.Length >= 3 && args[0] == "--comprueba") {  // prueba: ¿valdría esta ROM? (resultado en un archivo)
            string r = CompruebaRom(args[1]);
            File.WriteAllText(args[2], r == null ? "vale" : r, new UTF8Encoding(false));
            return;
        }
        if (args.Length >= 2 && args[0] == "--prepara") {  // prueba: prepara el juego sin ventana (resultado en un archivo)
            string r = "ok";
            try { Prepara_(); } catch (Exception ex) { r = ex.ToString(); }
            File.WriteAllText(args[1], r, new UTF8Encoding(false));
            return;
        }
        if (args.Length >= 2 && args[0] == "--instala-ultima") {  // prueba: instala la última versión de GitHub sin ventana
            string r = "ok";
            try {
                ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
                using (var w = new WebClient()) {
                    w.Headers.Add("User-Agent", "DS-Kart-Recompiled-Launcher");
                    string json = w.DownloadString("https://api.github.com/repos/" + REPO + "/releases/latest");
                    var m = System.Text.RegularExpressions.Regex.Match(json, "\"browser_download_url\"\\s*:\\s*\"([^\"]+\\.zip)\"");
                    Actualiza_(m.Groups[1].Value);
                    r += " " + m.Groups[1].Value;
                }
            } catch (Exception ex) { r = ex.ToString(); }
            File.WriteAllText(args[1], r, new UTF8Encoding(false));
            return;
        }
        if (args.Length >= 2 && args[0] == "--captura") {
            if (args.Length >= 4) PonIdiomaSinGuardar(args[3]);  // prueba: --captura x.png pagina idioma|bienvenida
            Captura(args[1], args.Length >= 3 ? args[2] : "inicio");
            return;
        }
        BorraLauncherViejo();
        BuscaActualizacion(false);
        ventana.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e) {
            if (e.ButtonState == MouseButtonState.Pressed) ventana.DragMove();
        };
        new Application().Run(ventana);
    }

    // Dibuja el contenido de la ventana en un PNG sin enseñarla
    static void Captura(string ruta, string pagina) {
        foreach (string p in paginas)
            if (p.ToLowerInvariant() == pagina.ToLowerInvariant()) Busca<RadioButton>("Nav" + p).IsChecked = true;
        var raiz = (FrameworkElement)ventana.Content;
        ventana.Content = null;
        var tam = new Size(ventana.Width, ventana.Height);
        raiz.Measure(tam);
        raiz.Arrange(new Rect(tam));
        raiz.UpdateLayout();
        var bmp = new RenderTargetBitmap((int)tam.Width, (int)tam.Height, 96, 96, PixelFormats.Pbgra32);
        bmp.Render(raiz);
        var png = new PngBitmapEncoder();
        png.Frames.Add(BitmapFrame.Create(bmp));
        using (var f = File.Create(ruta)) png.Save(f);
    }

    // ---- ajustes.ini ----------------------------------------------------------------
    static string RutaAjustes() { return Path.Combine(carpeta, "ajustes.ini"); }

    static void LeeAjustes() {
        ajustes["ESCALA"] = "2";
        ajustes["PANTALLA_COMPLETA"] = "0";
        ajustes["NOMBRE"] = "";
        ajustes["IDIOMA"] = "5";  // como el firmware de la DS: 1 inglés, 2 francés, 3 alemán, 4 italiano, 5 español
        ajustes["FPS"] = FpsSegunMonitor();
        ajustes["MICRO"] = "1";   // micrófono del PC para soplar (Batalla de globos)
        ajustes["MICRO_DISPOSITIVO"] = "";  // cuál (nombre de Windows); vacío = el predeterminado  // imágenes por segundo: 60 (como la DS), 120, 180 o 240 (con intermedias)
        if (!File.Exists(RutaAjustes())) return;
        foreach (string l in File.ReadAllLines(RutaAjustes(), Encoding.UTF8)) {
            int i = l.IndexOf('=');
            if (i <= 0 || l.StartsWith("#")) continue;
            ajustes[l.Substring(0, i).Trim()] = l.Substring(i + 1).Trim();
        }
        ajustes.Remove("TODO_DESBLOQUEADO");  // ya no es una opción: el juego lo trae siempre
        string li;
        if (ajustes.TryGetValue("LAUNCHER_IDIOMA", out li)) Textos.idioma = li;
    }

    // La primera vez: lo más alto que el monitor principal puede enseñar (60, 120, 180 o 240)
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern IntPtr GetDC(IntPtr hwnd);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
    [System.Runtime.InteropServices.DllImport("gdi32.dll")]
    static extern int GetDeviceCaps(IntPtr dc, int indice);

    static string FpsSegunMonitor() {
        int hz = 60;
        try {
            IntPtr dc = GetDC(IntPtr.Zero);
            hz = GetDeviceCaps(dc, 116);  // VREFRESH
            ReleaseDC(IntPtr.Zero, dc);
        } catch (Exception) { }
        foreach (int f in new[] { 240, 180, 120 })
            if (hz >= f - 2) return f.ToString();  // 239 Hz cuenta como 240
        return "60";
    }

    // ---- micrófonos de Windows (winmm: los mismos nombres que usa el juego) ----
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential,
        CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    struct WaveInCaps {
        public ushort wMid, wPid;
        public uint vDriverVersion;
        [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 32)]
        public string szPname;
        public uint dwFormats;
        public ushort wChannels, wReserved1;
    }
    [System.Runtime.InteropServices.DllImport("winmm.dll")]
    static extern uint waveInGetNumDevs();
    [System.Runtime.InteropServices.DllImport("winmm.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    static extern uint waveInGetDevCapsW(UIntPtr dispositivo, ref WaveInCaps caps, uint tam);

    static List<string> micros = new List<string>();
    static bool rellenando;

    // Desactivado, el predeterminado de Windows y cada micrófono conectado, con el elegido marcado
    static void RellenaMicros() {
        micros.Clear();
        try {
            uint n = waveInGetNumDevs();
            for (uint i = 0; i < n; i++) {
                var c = new WaveInCaps();
                if (waveInGetDevCapsW(new UIntPtr(i), ref c, (uint)System.Runtime.InteropServices.Marshal.SizeOf(c)) == 0)
                    micros.Add(c.szPname);
            }
        } catch (Exception) { }
        var cb = Busca<ComboBox>("Micro");
        rellenando = true;
        cb.Items.Clear();
        cb.Items.Add(T("Desactivado"));
        cb.Items.Add(T("El predeterminado de Windows"));
        foreach (string m in micros) cb.Items.Add(m);
        string elegido = ajustes["MICRO_DISPOSITIVO"];
        if (ajustes["MICRO"] == "0") cb.SelectedIndex = 0;
        else if (elegido.Length > 0 && micros.Contains(elegido)) cb.SelectedIndex = 2 + micros.IndexOf(elegido);
        else cb.SelectedIndex = 1;  // el elegido ya no está conectado: el predeterminado
        rellenando = false;
    }

    static void GuardaAjustes() {
        var sb = new StringBuilder();
        sb.AppendLine("# Ajustes del juego (los escribe el launcher; mkds.exe los lee al arrancar)");
        foreach (var kv in ajustes) sb.AppendLine(kv.Key + "=" + kv.Value);
        try { File.WriteAllText(RutaAjustes(), sb.ToString(), new UTF8Encoding(false)); }
        catch (Exception ex) { Estado(T("No se pudieron guardar los ajustes: ") + ex.Message, true); }
    }

    // ---- interfaz ---------------------------------------------------------------------
    static T Busca<T>(string nombre) where T : class { return ventana.FindName(nombre) as T; }
    static string T(string es) { return Textos.T(es); }

    // ---- idioma del launcher -------------------------------------------------------------
    // Los textos del XAML están en español; cada elemento guarda su texto original en Tag la primera vez y se
    // le pone la traducción. Al elegir idioma la primera vez, el juego se pone en el mismo (IDIOMA: número
    // del firmware de la DS); luego cada uno se puede cambiar por separado en Ajustes.
    static readonly string[] codigos = { "es", "en", "fr", "de", "it" };
    static readonly string[] numJuego = { "5", "1", "2", "3", "4" };

    static void Traduce(DependencyObject o) {
        var tb = o as TextBlock;
        if (tb != null && !(tb.Tag is string) && tb.Text.Length > 0) tb.Tag = tb.Text;
        if (tb != null && tb.Tag is string) tb.Text = T((string)tb.Tag);
        var cc = o as ContentControl;
        if (cc != null && !(cc.Tag is string) && cc.Content is string) cc.Tag = cc.Content;
        if (cc != null && cc.Tag is string && cc.Content is string) cc.Content = T((string)cc.Tag);
        foreach (object h in LogicalTreeHelper.GetChildren(o)) {
            var d = h as DependencyObject;
            if (d != null) Traduce(d);
        }
    }

    static void PonIdiomaSinGuardar(string codigo) {
        if (codigo == "bienvenida") { Busca<FrameworkElement>("Bienvenida").Visibility = Visibility.Visible; return; }
        Textos.idioma = codigo;
        Busca<FrameworkElement>("Bienvenida").Visibility = Visibility.Collapsed;
        foreach (string c in codigos) Busca<ToggleButton>("Lan_" + c).IsChecked = c == codigo;
        Traduce((DependencyObject)ventana.Content);
        Busca<TextBlock>("Version").Text = T("versión de prueba") + VERSION;
        Resumen();
        EstadoRom();
        Estado(T("Listo para jugar"), false);
    }

    static void PonIdioma(string codigo, bool tambienJuego) {
        Textos.idioma = codigo;
        RellenaMicros();
        ajustes["LAUNCHER_IDIOMA"] = codigo;
        if (tambienJuego) {
            ajustes["IDIOMA"] = numJuego[Array.IndexOf(codigos, codigo)];
            for (int k = 1; k <= 5; k++) Busca<ToggleButton>("Idi" + k).IsChecked = k.ToString() == ajustes["IDIOMA"];
        }
        foreach (string c in codigos) Busca<ToggleButton>("Lan_" + c).IsChecked = c == codigo;
        Traduce((DependencyObject)ventana.Content);
        Busca<TextBlock>("Version").Text = T("versión de prueba") + VERSION;
        Resumen();
        EstadoRom();
        Estado(T("Listo para jugar"), false);
        GuardaAjustes();
    }

    static void Prepara() {
        ventana.Title = TITULO;
        Busca<TextBlock>("Version").Text = T("versión de prueba") + VERSION;

        // navegación: cada botón de la barra enseña su página
        foreach (string p in paginas) {
            string pagina = p;
            var b = Busca<RadioButton>("Nav" + p);
            b.Checked += delegate {
                foreach (string q in paginas)
                    Busca<FrameworkElement>("Pag" + q).Visibility = q == pagina ? Visibility.Visible : Visibility.Collapsed;
            };
        }
        Busca<RadioButton>("NavInicio").IsChecked = true;

        // resolución: cuatro botones, uno marcado
        var escalas = new[] { "Esc1", "Esc2", "Esc3", "Esc4" };
        for (int i = 0; i < 4; i++) {
            var b = Busca<ToggleButton>(escalas[i]);
            string valor = (i + 1).ToString();
            b.IsChecked = ajustes["ESCALA"] == valor;
            b.Click += delegate {
                ajustes["ESCALA"] = valor;
                foreach (var n in escalas) Busca<ToggleButton>(n).IsChecked = n == "Esc" + valor;
                Resumen();
            };
        }
        // idioma del juego: cinco botones, uno marcado
        for (int i = 1; i <= 5; i++) {
            var b = Busca<ToggleButton>("Idi" + i);
            string valor = i.ToString();
            b.IsChecked = ajustes["IDIOMA"] == valor;
            b.Click += delegate {
                ajustes["IDIOMA"] = valor;
                for (int k = 1; k <= 5; k++) Busca<ToggleButton>("Idi" + k).IsChecked = k.ToString() == valor;
            };
        }
        // imágenes por segundo (MKDS_FPS en el juego): cuatro botones, uno marcado
        var fpss = new[] { "60", "120", "180", "240" };
        foreach (string f in fpss) {
            string valor = f;
            var b = Busca<ToggleButton>("Fps" + f);
            b.IsChecked = ajustes["FPS"] == valor;
            b.Click += delegate {
                ajustes["FPS"] = valor;
                foreach (string g in fpss) Busca<ToggleButton>("Fps" + g).IsChecked = g == valor;
                Resumen();
            };
        }
        RellenaMicros();
        Busca<ComboBox>("Micro").SelectionChanged += delegate {
            if (rellenando) return;
            int i = Busca<ComboBox>("Micro").SelectedIndex;
            if (i < 0) return;
            ajustes["MICRO"] = i == 0 ? "0" : "1";
            ajustes["MICRO_DISPOSITIVO"] = i >= 2 ? micros[i - 2] : "";
        };
        var pc = Busca<ToggleButton>("PantallaCompleta");
        pc.IsChecked = ajustes["PANTALLA_COMPLETA"] == "1";
        pc.Click += delegate { ajustes["PANTALLA_COMPLETA"] = pc.IsChecked == true ? "1" : "0"; Resumen(); };

        var nombre = Busca<TextBox>("Nombre");
        nombre.Text = ajustes["NOMBRE"];
        nombre.TextChanged += delegate { ajustes["NOMBRE"] = nombre.Text.Replace("=", "").Trim(); Resumen(); };

        Busca<Button>("Cerrar").Click += delegate { GuardaAjustes(); ventana.Close(); };
        Busca<Button>("Minimizar").Click += delegate { ventana.WindowState = WindowState.Minimized; };
        Busca<Button>("Jugar").Click += delegate { Juega(); };
        Busca<Button>("Repeticiones").Click += delegate {
            string rep = Path.Combine(carpeta, "repeticiones");
            Directory.CreateDirectory(rep);
            Process.Start("explorer.exe", "\"" + rep + "\"");
        };
        Busca<Button>("ElegirRom").Click += delegate { EligeRom(); };
        foreach (string c in codigos) {
            string cod = c;
            Busca<ToggleButton>("Lan_" + c).Click += delegate { PonIdioma(cod, false); };
            Busca<Button>("Bien_" + c).Click += delegate {
                PonIdioma(cod, true);
                Busca<FrameworkElement>("Bienvenida").Visibility = Visibility.Collapsed;
            };
        }
        // la primera vez: pantalla de elegir idioma
        if (!ajustes.ContainsKey("LAUNCHER_IDIOMA")) Busca<FrameworkElement>("Bienvenida").Visibility = Visibility.Visible;
        foreach (string c in codigos) Busca<ToggleButton>("Lan_" + c).IsChecked = c == Textos.idioma;
        Traduce((DependencyObject)ventana.Content);
        Resumen();
        EstadoRom();
        PreparaVideo();
        if (!File.Exists(Path.Combine(carpeta, "build", "mkds.exe")))
            Estado(T("La primera vez que juegues se prepara el juego"), false);
        Busca<Button>("Actualizar").Click += delegate { Actualiza(); };
        Busca<Button>("BuscarActualizacion").Click += delegate {
            Estado(T("Buscando actualizaciones…"), false);
            BuscaActualizacion(true);
        };
        Busca<Button>("PrepCancelar").Click += delegate { Cancela(); };
        ventana.Closing += delegate { Cancela(); };
    }

    // ---- vídeo de la portada --------------------------------------------------------------
    // portada.mp4 se graba en el PC del usuario con su propio juego (el modo demostración del título), así
    // que el launcher no lleva imágenes de Nintendo. Si no está, se queda la ilustración.
    static void PreparaVideo() {
        string ruta = Path.Combine(carpeta, "portada.mp4");
        if (!File.Exists(ruta)) return;
        var v = Busca<MediaElement>("Video");
        v.Source = new Uri(ruta, UriKind.Absolute);
        v.MediaOpened += delegate { v.Visibility = Visibility.Visible; };
        v.MediaEnded += delegate { v.Position = TimeSpan.Zero; v.Play(); };
        v.MediaFailed += delegate { v.Visibility = Visibility.Collapsed; };
        v.Play();
    }

    // ---- la ROM del usuario -----------------------------------------------------------------
    static string RutaRom() { return Path.Combine(carpeta, "rom", "mkds.nds"); }

    static uint[] tablaCrc;
    static uint Crc32(byte[] d, int off, int len) {
        if (tablaCrc == null) {
            tablaCrc = new uint[256];
            for (uint i = 0; i < 256; i++) {
                uint c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
                tablaCrc[i] = c;
            }
        }
        uint crc = 0xFFFFFFFF;
        for (int i = off; i < off + len; i++) crc = tablaCrc[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFF;
    }

    // null si es la buena; si no, por qué no vale
    static string CompruebaRom(string ruta) {
        byte[] d;
        try { d = File.ReadAllBytes(ruta); }
        catch (Exception ex) { return T("No se pudo leer: ") + ex.Message; }
        if (d.Length < 0x200) return T("Ese archivo no es una ROM de DS.");
        string codigo = Encoding.ASCII.GetString(d, 12, 4);
        if (!codigo.StartsWith("AMC"))
            return string.Format(T("Esa ROM no es Mario Kart DS (su código es {0})."), codigo);
        if (codigo != CODIGO_JUEGO)
            return string.Format(T("Es Mario Kart DS, pero de otra región ({0}). El port es para la versión europea (AMCP)."), codigo);
        int a9o = BitConverter.ToInt32(d, 0x20), a9t = BitConverter.ToInt32(d, 0x2C);
        int a7o = BitConverter.ToInt32(d, 0x30), a7t = BitConverter.ToInt32(d, 0x3C);
        if (a9o < 0 || a7o < 0 || a9t < 0 || a7t < 0 || (long)a9o + a9t > d.Length || (long)a7o + a7t > d.Length)
            return T("La ROM está incompleta o dañada.");
        long usado = BitConverter.ToUInt32(d, 0x80);  // tamaño usado de la ROM (una recortada llega justo hasta aquí)
        if (d.Length < usado) return T("La ROM está incompleta o dañada (le faltan datos del juego).");
        if (Crc32(d, a9o, a9t) != CRC_ARM9 || Crc32(d, a7o, a7t) != CRC_ARM7)
            return T("Es la versión europea, pero no la misma revisión o está modificada.");
        return null;
    }

    static void EstadoRom() {
        bool hay = File.Exists(RutaRom());
        var t = Busca<TextBlock>("RomEstado");
        t.Text = T(hay ? "Mario Kart DS (Europa) listo para jugar" : "Todavía no has elegido tu ROM");
        t.Foreground = new SolidColorBrush(hay ? Color.FromRgb(0x7D, 0xE0, 0x9A) : Color.FromRgb(0xFF, 0xC9, 0x40));
        Busca<TextBlock>("ChipJuego").Text = T(hay ? "ROM lista" : "Falta la ROM");
    }

    // Se copia a rom\mkds.nds: el archivo que elige el usuario no se mueve ni se borra
    static bool EligeRom() {
        var dlg = new OpenFileDialog();
        dlg.Title = T("Elige tu ROM de Mario Kart DS (versión europea)");
        dlg.Filter = T("ROM de Nintendo DS (*.nds)|*.nds|Todos los archivos|*.*");
        if (dlg.ShowDialog() != true) return false;
        string error = CompruebaRom(dlg.FileName);
        if (error != null) {
            MessageBox.Show(error, TITULO, MessageBoxButton.OK, MessageBoxImage.Warning);
            return false;
        }
        try {
            Directory.CreateDirectory(Path.GetDirectoryName(RutaRom()));
            if (Path.GetFullPath(dlg.FileName) != Path.GetFullPath(RutaRom())) {
                if (File.Exists(RutaRom()) && CompruebaRom(RutaRom()) != null)
                    File.Copy(RutaRom(), RutaRom() + ".antes", true);  // la que había no valía: se guarda aparte
                File.Copy(dlg.FileName, RutaRom(), true);
            }
        } catch (Exception ex) {
            MessageBox.Show(T("No se pudo copiar la ROM: ") + ex.Message, TITULO, MessageBoxButton.OK, MessageBoxImage.Error);
            return false;
        }
        EstadoRom();
        Estado(T("ROM comprobada: lista para jugar"), false);
        return true;
    }

    // las etiquetas de la portada con los ajustes actuales
    static void Resumen() {
        Busca<TextBlock>("ChipEscala").Text = (ajustes["ESCALA"] == "1" ? T("Resolución original") : string.Format(T("3D a {0}×"), ajustes["ESCALA"]))
                                              + " · " + ajustes["FPS"] + " FPS";
        Busca<TextBlock>("ChipPantalla").Text = T(ajustes["PANTALLA_COMPLETA"] == "1" ? "Pantalla completa" : "Ventana");
        string n = ajustes["NOMBRE"];
        Busca<TextBlock>("ChipNombre").Text = n.Length > 0 ? n : T("Sin nombre");
    }

    static void Juega() {
        GuardaAjustes();
        // el nombre es obligatorio (lo necesitará el juego en línea): sin él, a Ajustes con el cursor en la casilla
        if (ajustes["NOMBRE"].Length == 0) {
            Busca<RadioButton>("NavAjustes").IsChecked = true;
            var caja = Busca<TextBox>("Nombre");
            caja.Focus();
            caja.BringIntoView();
            Estado(T("Pon tu nombre para poder jugar"), true);
            MessageBox.Show(T("Pon tu nombre para poder jugar") + "\n" + T("Lo necesita el juego en línea."), TITULO,
                            MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        if (!File.Exists(RutaRom()) && !EligeRom()) return;
        if (!ActualizadoParaJugar()) return;  // con una versión nueva en GitHub, primero actualizar (Preparar.cs)
        if (HayQuePreparar()) {  // la primera vez, o tras actualizar: se genera desde la ROM (Preparar.cs)
            PreparaYJuega();
            return;
        }
        string exe = Path.Combine(carpeta, "build", "mkds.exe");
        var psi = new ProcessStartInfo(exe);
        psi.WorkingDirectory = carpeta;
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;  // sin la consola negra de los mensajes de depuración
        Process p;
        try { p = Process.Start(psi); }
        catch (Exception ex) { Estado(T("No se pudo abrir el juego: ") + ex.Message, true); return; }
        Estado(T("Jugando… el launcher vuelve al cerrar el juego"), false);
        ventana.WindowState = WindowState.Minimized;
        p.EnableRaisingEvents = true;
        p.Exited += delegate {
            ventana.Dispatcher.Invoke(new Action(delegate {
                ventana.WindowState = WindowState.Normal;
                ventana.Activate();
                if (p.ExitCode == 0) Estado(T("Listo para jugar"), false);
                else Estado(string.Format(T("El juego se cerró con un error (código {0})"), p.ExitCode), true);
            }));
        };
    }

    static void Estado(string texto, bool error) {
        var t = Busca<TextBlock>("Estado");
        t.Text = texto;
        t.Foreground = new SolidColorBrush(error ? Color.FromRgb(0xFF, 0x8A, 0x7A) : Color.FromRgb(0xC9, 0xCF, 0xEA));
    }

    // ---- diseño (XAML) ------------------------------------------------------------------
    // Estilo propio: azul noche, acento rojo/naranja de atardecer y amarillo; portada dibujada con formas.
    static string Xaml() {
        return @"
<Window xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'
        xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'
        Width='980' Height='880' WindowStyle='None' ResizeMode='NoResize' AllowsTransparency='True'
        Background='Transparent' WindowStartupLocation='CenterScreen' FontFamily='Segoe UI'
        TextOptions.TextFormattingMode='Display'>
  <Window.Resources>
    <SolidColorBrush x:Key='Fondo' Color='#10132A'/>
    <SolidColorBrush x:Key='Lateral' Color='#0B0D1F'/>
    <SolidColorBrush x:Key='Panel' Color='#181C3A'/>
    <SolidColorBrush x:Key='Panel2' Color='#20254A'/>
    <SolidColorBrush x:Key='Borde' Color='#2A3062'/>
    <SolidColorBrush x:Key='Texto' Color='#F3F4FF'/>
    <SolidColorBrush x:Key='Suave' Color='#9AA3C7'/>
    <SolidColorBrush x:Key='Rojo' Color='#FF4D3D'/>
    <SolidColorBrush x:Key='Amarillo' Color='#FFC940'/>

    <Style x:Key='Nav' TargetType='RadioButton'>
      <Setter Property='Foreground' Value='{StaticResource Suave}'/>
      <Setter Property='FontSize' Value='15'/>
      <Setter Property='FontWeight' Value='SemiBold'/>
      <Setter Property='Cursor' Value='Hand'/>
      <Setter Property='Margin' Value='0,3'/>
      <Setter Property='Template'>
        <Setter.Value>
          <ControlTemplate TargetType='RadioButton'>
            <Border x:Name='b' CornerRadius='10' Padding='14,11' Background='Transparent'>
              <DockPanel>
                <Border x:Name='marca' Width='4' Height='18' CornerRadius='2' Background='Transparent' Margin='0,0,12,0'/>
                <ContentPresenter VerticalAlignment='Center'/>
              </DockPanel>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property='IsMouseOver' Value='True'><Setter TargetName='b' Property='Background' Value='#161A36'/></Trigger>
              <Trigger Property='IsChecked' Value='True'>
                <Setter TargetName='b' Property='Background' Value='#1C2145'/>
                <Setter TargetName='marca' Property='Background' Value='{StaticResource Rojo}'/>
                <Setter Property='Foreground' Value='{StaticResource Texto}'/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key='Seg' TargetType='ToggleButton'>
      <Setter Property='Foreground' Value='{StaticResource Suave}'/>
      <Setter Property='FontSize' Value='15'/>
      <Setter Property='FontWeight' Value='SemiBold'/>
      <Setter Property='Cursor' Value='Hand'/>
      <Setter Property='Template'>
        <Setter.Value>
          <ControlTemplate TargetType='ToggleButton'>
            <Border x:Name='b' Background='{StaticResource Panel2}' CornerRadius='10' Padding='0,10' Margin='4,0'>
              <ContentPresenter HorizontalAlignment='Center'/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property='IsMouseOver' Value='True'><Setter TargetName='b' Property='Background' Value='#2A3060'/></Trigger>
              <Trigger Property='IsChecked' Value='True'>
                <Setter TargetName='b' Property='Background' Value='{StaticResource Amarillo}'/>
                <Setter Property='Foreground' Value='#17150C'/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key='Interruptor' TargetType='ToggleButton'>
      <Setter Property='Cursor' Value='Hand'/>
      <Setter Property='VerticalAlignment' Value='Center'/>
      <Setter Property='Template'>
        <Setter.Value>
          <ControlTemplate TargetType='ToggleButton'>
            <Border x:Name='pista' Width='50' Height='28' CornerRadius='14' Background='#2A3060'>
              <Ellipse x:Name='bola' Width='22' Height='22' Fill='#C9CFEA' HorizontalAlignment='Left' Margin='3,0'/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property='IsChecked' Value='True'>
                <Setter TargetName='pista' Property='Background' Value='{StaticResource Rojo}'/>
                <Setter TargetName='bola' Property='HorizontalAlignment' Value='Right'/>
                <Setter TargetName='bola' Property='Fill' Value='White'/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key='Ventanita' TargetType='Button'>
      <Setter Property='Foreground' Value='#D9DDF5'/>
      <Setter Property='FontSize' Value='13'/>
      <Setter Property='Cursor' Value='Hand'/>
      <Setter Property='Template'>
        <Setter.Value>
          <ControlTemplate TargetType='Button'>
            <Border x:Name='b' Width='34' Height='30' CornerRadius='8' Background='#5510132A'>
              <ContentPresenter HorizontalAlignment='Center' VerticalAlignment='Center'/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property='IsMouseOver' Value='True'><Setter TargetName='b' Property='Background' Value='#AA10132A'/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key='Grande' TargetType='Button'>
      <Setter Property='Cursor' Value='Hand'/>
      <Setter Property='Template'>
        <Setter.Value>
          <ControlTemplate TargetType='Button'>
            <Border x:Name='b' CornerRadius='16' Padding='46,15'>
              <Border.Background>
                <LinearGradientBrush StartPoint='0,0' EndPoint='1,1'>
                  <GradientStop Color='#FF6A3D' Offset='0'/>
                  <GradientStop Color='#FF3D4E' Offset='1'/>
                </LinearGradientBrush>
              </Border.Background>
              <Border.Effect><DropShadowEffect Color='#FF4D3D' BlurRadius='28' ShadowDepth='0' Opacity='0.6'/></Border.Effect>
              <StackPanel Orientation='Horizontal'>
                <Path Data='M0,0 L14,8 L0,16 Z' Fill='White' VerticalAlignment='Center' Margin='0,0,12,0'/>
                <ContentPresenter TextElement.Foreground='White' TextElement.FontSize='24' TextElement.FontWeight='Black'
                                  VerticalAlignment='Center'/>
              </StackPanel>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property='IsMouseOver' Value='True'><Setter TargetName='b' Property='Opacity' Value='0.92'/></Trigger>
              <Trigger Property='IsPressed' Value='True'><Setter TargetName='b' Property='Opacity' Value='0.8'/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key='Secundario' TargetType='Button'>
      <Setter Property='Foreground' Value='{StaticResource Texto}'/>
      <Setter Property='FontSize' Value='14'/>
      <Setter Property='FontWeight' Value='SemiBold'/>
      <Setter Property='Cursor' Value='Hand'/>
      <Setter Property='Template'>
        <Setter.Value>
          <ControlTemplate TargetType='Button'>
            <Border x:Name='b' CornerRadius='14' Background='#66101330' BorderBrush='#55FFFFFF' BorderThickness='1' Padding='20,14'>
              <ContentPresenter VerticalAlignment='Center'/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property='IsMouseOver' Value='True'><Setter TargetName='b' Property='Background' Value='#99101330'/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key='Desplegable' TargetType='ComboBox'>
      <Setter Property='Foreground' Value='{StaticResource Texto}'/>
      <Setter Property='FontSize' Value='15'/>
      <Setter Property='Cursor' Value='Hand'/>
      <Setter Property='ItemContainerStyle'>
        <Setter.Value>
          <Style TargetType='ComboBoxItem'>
            <Setter Property='Foreground' Value='{StaticResource Texto}'/>
            <Setter Property='Template'>
              <Setter.Value>
                <ControlTemplate TargetType='ComboBoxItem'>
                  <Border x:Name='b' CornerRadius='8' Padding='12,8' Background='Transparent'>
                    <ContentPresenter/>
                  </Border>
                  <ControlTemplate.Triggers>
                    <Trigger Property='IsHighlighted' Value='True'><Setter TargetName='b' Property='Background' Value='#2E3466'/></Trigger>
                    <Trigger Property='IsSelected' Value='True'><Setter Property='Foreground' Value='{StaticResource Amarillo}'/></Trigger>
                  </ControlTemplate.Triggers>
                </ControlTemplate>
              </Setter.Value>
            </Setter>
          </Style>
        </Setter.Value>
      </Setter>
      <Setter Property='Template'>
        <Setter.Value>
          <ControlTemplate TargetType='ComboBox'>
            <Grid>
              <ToggleButton Focusable='False' ClickMode='Press'
                            IsChecked='{Binding IsDropDownOpen, Mode=TwoWay, RelativeSource={RelativeSource TemplatedParent}}'>
                <ToggleButton.Template>
                  <ControlTemplate TargetType='ToggleButton'>
                    <Border x:Name='b' Background='{StaticResource Panel2}' BorderBrush='{StaticResource Borde}' BorderThickness='1' CornerRadius='10'>
                      <Path Data='M0,0 L6,6 L12,0' Stroke='{StaticResource Suave}' StrokeThickness='2' HorizontalAlignment='Right'
                            VerticalAlignment='Center' Margin='0,0,16,0'/>
                    </Border>
                    <ControlTemplate.Triggers>
                      <Trigger Property='IsMouseOver' Value='True'><Setter TargetName='b' Property='Background' Value='#2A3060'/></Trigger>
                    </ControlTemplate.Triggers>
                  </ControlTemplate>
                </ToggleButton.Template>
              </ToggleButton>
              <ContentPresenter IsHitTestVisible='False' Content='{TemplateBinding SelectionBoxItem}' Margin='14,10,40,10'
                                VerticalAlignment='Center'/>
              <Popup IsOpen='{TemplateBinding IsDropDownOpen}' Placement='Bottom' AllowsTransparency='True' Focusable='False'>
                <Border Background='{StaticResource Panel2}' BorderBrush='{StaticResource Borde}' BorderThickness='1' CornerRadius='10'
                        Padding='4' Margin='0,4,0,0'
                        MinWidth='{Binding ActualWidth, RelativeSource={RelativeSource TemplatedParent}}'>
                  <ScrollViewer MaxHeight='260' VerticalScrollBarVisibility='Auto'><ItemsPresenter/></ScrollViewer>
                </Border>
              </Popup>
            </Grid>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key='Chip' TargetType='Border'>
      <Setter Property='Background' Value='{StaticResource Panel}'/>
      <Setter Property='BorderBrush' Value='{StaticResource Borde}'/>
      <Setter Property='BorderThickness' Value='1'/>
      <Setter Property='CornerRadius' Value='12'/>
      <Setter Property='Padding' Value='16,12'/>
      <Setter Property='Margin' Value='0,0,12,0'/>
    </Style>
    <Style x:Key='Tarjeta' TargetType='Border'>
      <Setter Property='Background' Value='{StaticResource Panel}'/>
      <Setter Property='CornerRadius' Value='14'/>
      <Setter Property='Padding' Value='22,14'/>
      <Setter Property='Margin' Value='0,0,0,10'/>
    </Style>
    <Style x:Key='H1' TargetType='TextBlock'>
      <Setter Property='Foreground' Value='{StaticResource Texto}'/>
      <Setter Property='FontSize' Value='26'/>
      <Setter Property='FontWeight' Value='Bold'/>
      <Setter Property='Margin' Value='0,0,0,12'/>
    </Style>
    <Style x:Key='Etiqueta' TargetType='TextBlock'>
      <Setter Property='Foreground' Value='{StaticResource Texto}'/>
      <Setter Property='FontSize' Value='15'/>
      <Setter Property='FontWeight' Value='SemiBold'/>
    </Style>
    <Style x:Key='Nota' TargetType='TextBlock'>
      <Setter Property='Foreground' Value='{StaticResource Suave}'/>
      <Setter Property='FontSize' Value='13'/>
      <Setter Property='TextWrapping' Value='Wrap'/>
      <Setter Property='Margin' Value='0,4,0,0'/>
    </Style>
    <Style x:Key='Celda' TargetType='TextBlock'>
      <Setter Property='Foreground' Value='{StaticResource Texto}'/>
      <Setter Property='FontSize' Value='14'/>
      <Setter Property='Margin' Value='0,8'/>
    </Style>
    <Style x:Key='Tecla' TargetType='Border'>
      <Setter Property='Background' Value='{StaticResource Panel2}'/>
      <Setter Property='BorderBrush' Value='{StaticResource Borde}'/>
      <Setter Property='BorderThickness' Value='1,1,1,3'/>
      <Setter Property='CornerRadius' Value='6'/>
      <Setter Property='Padding' Value='9,3'/>
      <Setter Property='Margin' Value='0,5,6,5'/>
      <Setter Property='HorizontalAlignment' Value='Left'/>
    </Style>
  </Window.Resources>

  <Border CornerRadius='18' Background='{StaticResource Fondo}' BorderBrush='{StaticResource Borde}' BorderThickness='1'
          ClipToBounds='True'>
    <Grid>
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width='230'/>
        <ColumnDefinition Width='*'/>
      </Grid.ColumnDefinitions>

      <!-- ===== barra lateral ===== -->
      <Border Grid.Column='0' Background='{StaticResource Lateral}' CornerRadius='18,0,0,18'>
        <DockPanel Margin='18,22,18,18'>
          <!-- logotipo propio: rueda con bandera de meta -->
          <StackPanel DockPanel.Dock='Top' Orientation='Horizontal' Margin='6,0,0,30'>
            <!-- logo: rueda con dos flechas (logo/genera_logo.py; aquí copiado de logo/logo_xaml.txt) -->
            <Image Width='48' Height='48'>
              <Image.Source>
                <DrawingImage>
                  <DrawingImage.Drawing>
                    <DrawingGroup>
                      <GeometryDrawing Brush='#00000000' Geometry='M0,0 L512,512'/>
                      <GeometryDrawing Brush='#232A57' Geometry='F1 M39.0,256.0 A217.0,217.0 0 1 1 473.0,256.0 A217.0,217.0 0 1 1 39.0,256.0 Z'/>
                      <GeometryDrawing Brush='#FF4D3D' Geometry='F1 M57.7,183.8 A211.0,211.0 0 0 1 438.7,150.5 L464.7,135.5 L437.0,237.0 L362.5,194.5 L388.5,179.5 A153.0,153.0 0 0 0 112.2,203.7 Z'/>
                      <GeometryDrawing Brush='#FFC940' Geometry='F1 M454.3,328.2 A211.0,211.0 0 0 1 73.3,361.5 L47.3,376.5 L75.0,275.0 L149.5,317.5 L123.5,332.5 A153.0,153.0 0 0 0 399.8,308.3 Z'/>
                      <GeometryDrawing Brush='#C9322A' Geometry='F1 M125.4,155.8 L138.6,140.6 L113.8,116.2 L97.8,134.6 Z M172.5,114.2 L190.4,105.1 L176.5,73.1 L154.8,84.2 Z M231.7,93.2 L251.7,91.5 L250.8,56.7 L226.5,58.8 Z M294.4,95.9 L313.6,101.8 L325.8,69.2 L302.5,62.1 Z M351.6,122.0 L367.2,134.6 L390.7,109.0 L371.8,93.7 Z'/>
                      <GeometryDrawing Brush='#D99A22' Geometry='F1 M386.6,356.2 L373.4,371.4 L398.2,395.8 L414.2,377.4 Z M339.5,397.8 L321.6,406.9 L335.5,438.9 L357.2,427.8 Z M280.3,418.8 L260.3,420.5 L261.2,455.3 L285.5,453.2 Z M217.6,416.1 L198.4,410.2 L186.2,442.8 L209.5,449.9 Z M160.4,390.0 L144.8,377.4 L121.3,403.0 L140.2,418.3 Z'/>
                      <GeometryDrawing Brush='#F3F4FF' Geometry='F1 M134.0,256.0 A122.0,122.0 0 1 1 378.0,256.0 A122.0,122.0 0 1 1 134.0,256.0 Z'/>
                      <GeometryDrawing Brush='#232A57' Geometry='F1 M164.0,256.0 A92.0,92.0 0 1 1 348.0,256.0 A92.0,92.0 0 1 1 164.0,256.0 Z'/>
                      <GeometryDrawing Brush='#F3F4FF' Geometry='F1 M245.4,223.7 L266.6,223.7 L277.6,162.5 L234.4,162.5 Z M283.4,235.9 L290.0,256.1 L351.6,247.6 L338.3,206.6 Z M283.6,275.9 L266.4,288.4 L293.5,344.4 L328.5,319.0 Z M245.6,288.4 L228.4,275.9 L183.5,319.0 L218.5,344.4 Z M222.0,256.1 L228.6,235.9 L173.7,206.6 L160.4,247.6 Z'/>
                      <GeometryDrawing Brush='#FF4D3D' Geometry='F1 M208.0,256.0 A48.0,48.0 0 1 1 304.0,256.0 A48.0,48.0 0 1 1 208.0,256.0 Z'/>
                      <GeometryDrawing Brush='#F3F4FF' Geometry='F1 M221.0,256.0 A35.0,35.0 0 1 1 291.0,256.0 A35.0,35.0 0 1 1 221.0,256.0 Z'/>
                      <GeometryDrawing Brush='#12152A' Geometry='F1 M256.0,256.0 L256.0,221.0 A35.0,35.0 0 0 1 291.0,256.0 Z M256.0,256.0 L256.0,291.0 A35.0,35.0 0 0 1 221.0,256.0 Z'/>
                    </DrawingGroup>
                  </DrawingImage.Drawing>
                </DrawingImage>
              </Image.Source>
            </Image>
            <StackPanel Margin='12,0,0,0' VerticalAlignment='Center'>
              <TextBlock Text='DS KART' Foreground='{StaticResource Texto}' FontSize='19' FontWeight='Black'/>
              <TextBlock Text='RECOMPILED' Foreground='{StaticResource Amarillo}' FontSize='11' FontWeight='Bold' Margin='1,-2,0,0'/>
            </StackPanel>
          </StackPanel>

          <TextBlock DockPanel.Dock='Bottom' x:Name='Version' Foreground='#5C6590' FontSize='11' Margin='8,0,0,0'
                     TextWrapping='Wrap'/>

          <StackPanel>
            <RadioButton x:Name='NavInicio' GroupName='nav' Style='{StaticResource Nav}' Content='Inicio'/>
            <RadioButton x:Name='NavAjustes' GroupName='nav' Style='{StaticResource Nav}' Content='Ajustes'/>
            <RadioButton x:Name='NavControles' GroupName='nav' Style='{StaticResource Nav}' Content='Controles'/>
            <RadioButton x:Name='NavAcerca' GroupName='nav' Style='{StaticResource Nav}' Content='Acerca de'/>
          </StackPanel>
        </DockPanel>
      </Border>

      <!-- ===== contenido ===== -->
      <Grid Grid.Column='1'>

        <!-- Inicio: portada dibujada + jugar -->
        <Grid x:Name='PagInicio'>
          <Grid.RowDefinitions>
            <RowDefinition Height='*'/>
            <RowDefinition Height='Auto'/>
          </Grid.RowDefinitions>
          <Grid Grid.Row='0' ClipToBounds='True'>
            <Viewbox Stretch='UniformToFill' VerticalAlignment='Bottom'>
              <Canvas Width='750' Height='470'>
                <Rectangle Width='750' Height='470'>
                  <Rectangle.Fill>
                    <LinearGradientBrush StartPoint='0,0' EndPoint='0,1'>
                      <GradientStop Color='#1A1A4A' Offset='0'/>
                      <GradientStop Color='#5B2A86' Offset='0.38'/>
                      <GradientStop Color='#E8556A' Offset='0.6'/>
                      <GradientStop Color='#FFB24A' Offset='0.7'/>
                    </LinearGradientBrush>
                  </Rectangle.Fill>
                </Rectangle>
                <Ellipse Canvas.Left='300' Canvas.Top='170' Width='150' Height='150'>
                  <Ellipse.Fill>
                    <RadialGradientBrush>
                      <GradientStop Color='#FFE9A8' Offset='0'/>
                      <GradientStop Color='#FFC24A' Offset='0.7'/>
                      <GradientStop Color='#00FFC24A' Offset='1'/>
                    </RadialGradientBrush>
                  </Ellipse.Fill>
                </Ellipse>
                <Ellipse Canvas.Left='80' Canvas.Top='40' Width='3' Height='3' Fill='#AAFFFFFF'/>
                <Ellipse Canvas.Left='190' Canvas.Top='90' Width='2' Height='2' Fill='#88FFFFFF'/>
                <Ellipse Canvas.Left='560' Canvas.Top='60' Width='3' Height='3' Fill='#AAFFFFFF'/>
                <Ellipse Canvas.Left='660' Canvas.Top='120' Width='2' Height='2' Fill='#88FFFFFF'/>
                <Ellipse Canvas.Left='430' Canvas.Top='30' Width='2' Height='2' Fill='#77FFFFFF'/>
                <Path Fill='#6A2E7E' Data='M0,300 L90,235 L170,280 L260,215 L345,275 L430,230 L520,285 L610,220 L700,270 L750,250 L750,330 L0,330 Z'/>
                <Path Fill='#3B1F5E' Data='M0,320 L120,270 L230,315 L330,285 L470,320 L590,280 L750,315 L750,340 L0,340 Z'/>
                <Rectangle Canvas.Top='325' Width='750' Height='145' Fill='#1F1A44'/>
                <Path Fill='#2E2F52' Data='M355,325 L395,325 L640,470 L110,470 Z'/>
                <Path Data='M355,325 L349,325 L96,470 L110,470 Z'>
                  <Path.Fill>
                    <LinearGradientBrush StartPoint='0,0' EndPoint='0,1' SpreadMethod='Repeat'>
                      <LinearGradientBrush.RelativeTransform><ScaleTransform ScaleY='0.12'/></LinearGradientBrush.RelativeTransform>
                      <GradientStop Color='#FF4D3D' Offset='0'/><GradientStop Color='#FF4D3D' Offset='0.5'/>
                      <GradientStop Color='White' Offset='0.5'/><GradientStop Color='White' Offset='1'/>
                    </LinearGradientBrush>
                  </Path.Fill>
                </Path>
                <Path Data='M395,325 L401,325 L654,470 L640,470 Z'>
                  <Path.Fill>
                    <LinearGradientBrush StartPoint='0,0' EndPoint='0,1' SpreadMethod='Repeat'>
                      <LinearGradientBrush.RelativeTransform><ScaleTransform ScaleY='0.12'/></LinearGradientBrush.RelativeTransform>
                      <GradientStop Color='#FF4D3D' Offset='0'/><GradientStop Color='#FF4D3D' Offset='0.5'/>
                      <GradientStop Color='White' Offset='0.5'/><GradientStop Color='White' Offset='1'/>
                    </LinearGradientBrush>
                  </Path.Fill>
                </Path>
                <Path Fill='#FFC940' Data='M374,330 L376,330 L377,338 L373,338 Z M371,350 L379,350 L381,366 L369,366 Z M366,384 L384,384 L388,410 L362,410 Z M358,436 L392,436 L398,470 L352,470 Z'/>
                <Path Data='M232,395 L518,395 L540,410 L210,410 Z'>
                  <Path.Fill>
                    <DrawingBrush TileMode='Tile' Viewport='0,0,0.0625,0.5' ViewportUnits='RelativeToBoundingBox'>
                      <DrawingBrush.Drawing>
                        <DrawingGroup>
                          <GeometryDrawing Brush='#F3F4FF' Geometry='M0,0 H2 V2 H0 Z'/>
                          <GeometryDrawing Brush='#141634' Geometry='M0,0 H1 V1 H0 Z M1,1 H2 V2 H1 Z'/>
                        </DrawingGroup>
                      </DrawingBrush.Drawing>
                    </DrawingBrush>
                  </Path.Fill>
                </Path>
              </Canvas>
            </Viewbox>
            <!-- encima de la ilustración: portada.mp4 (si existe), en bucle y sin sonido -->
            <MediaElement x:Name='Video' Stretch='UniformToFill' IsMuted='True' LoadedBehavior='Manual'
                          Visibility='Collapsed'/>
            <Rectangle>
              <Rectangle.Fill>
                <LinearGradientBrush StartPoint='0,0' EndPoint='0,1'>
                  <GradientStop Color='#0010132A' Offset='0.45'/>
                  <GradientStop Color='#E610132A' Offset='1'/>
                </LinearGradientBrush>
              </Rectangle.Fill>
            </Rectangle>
            <StackPanel Orientation='Horizontal' HorizontalAlignment='Right' VerticalAlignment='Top' Margin='0,14,14,0'>
              <Button x:Name='Minimizar' Style='{StaticResource Ventanita}' Content='—' Margin='0,0,6,0'/>
              <Button x:Name='Cerrar' Style='{StaticResource Ventanita}' Content='✕'/>
            </StackPanel>
            <StackPanel VerticalAlignment='Bottom' Margin='36,0,36,26'>
              <TextBlock Text='PORT NATIVO PARA PC' Foreground='{StaticResource Amarillo}' FontSize='12' FontWeight='Bold'/>
              <!-- logo horizontal (logo/Marca.cs; copiado de logo/logo_horizontal_xaml.txt) -->
              <Image Width='470' HorizontalAlignment='Left' Margin='-14,0,0,-6'>
                <Image.Effect><DropShadowEffect BlurRadius='18' ShadowDepth='3' Opacity='0.45'/></Image.Effect>
                <Image.Source><DrawingImage><DrawingImage.Drawing>
<DrawingGroup>
  <GeometryDrawing Brush='#00000000' Geometry='M0,0 L1560,0 L1560,500 L0,500 Z'/>
  <DrawingGroup Transform='1,0,0,1,40,40'>
  <GeometryDrawing Geometry='F1M469.249807482985,75.3500726071607L452.390911683491,154.664941398017 463.382110729415,154.664941398017C472.755983613364,154.664941398017 479.644316909207,153.625233380159 484.047102881022,151.545817344444 488.449888852837,149.466401308729 492.324236977236,145.835685211726 495.67016930335,140.653638109745 499.016082868976,135.471606479609 501.923824039778,127.071423248753 504.39338343551,115.453096153099 507.662723518298,100.072023966836 507.39225029839,89.5429147946523 503.581969867387,83.8657763724711 499.771672320217,78.1886456862127 492.057363866767,75.3500803430834 480.439046151358,75.3500726071607L469.249807482985,75.3500726071607z M431.381540121154,42.4754952806135L498.02187307527,42.4754952806135C511.158498785751,42.4754952806135 521.391267719007,44.2578518826552 528.72018761096,47.8225650867386 536.04909976699,51.3872782908221 541.645491546858,56.5033070280385 545.509366239208,63.1706358265423 549.373231551314,69.8379723609689 551.553296196634,77.5945268188654 552.049546347645,86.4402914643091 552.545794854335,95.2860638456756 551.741554399286,104.659936729624 549.636823338176,114.561917852078 546.339410246222,130.075026767871 542.016298392238,142.105933831653 536.667518719916,150.654639043423 531.318723575748,159.203344255193 524.894818593501,166.365787581665 517.395810350462,172.141938079148 509.8967943715,177.918088576631 502.346107787072,181.763368227081 494.743757174463,183.677746086807 484.346523968341,186.31827954341 475.154111246198,187.638538535789 467.166515719393,187.638538535789L400.526182765278,187.638538535789 431.381540121154,42.4754952806135z M648.365293244649,40C666.914999390098,40 680.325189229185,43.4492004055518 688.595822923659,50.3475702729644 696.866484273181,57.2459556122225 700.3264007333,68.2206461989938 698.975569015374,83.2716575051239L656.167942329063,85.7471527857373C656.434843815145,79.2118529808405 655.085132242396,74.4589020420626 652.118838554508,71.4882999694036 649.152541577976,68.5177133685902 644.500752216763,67.0324162002221 638.163485942714,67.0324084642994 632.948435749952,67.0324162002221 628.785632465558,68.1381348469219 625.675043501522,70.3495721403214 622.564452893164,72.5610171696437 620.672409177291,75.2510527959361 619.998895237735,78.4196867551214 619.507789131048,80.730159331591 620.155013531685,82.8095753673063 621.940568439646,84.6579348622674 623.646069057578,86.5723281938388 628.28422639257,88.3546847958806 635.855024972776,90.0050046683926 654.562927912947,94.2298551045881 667.698690954407,98.5042077104731 675.262312452835,102.82807022197 682.825963250632,107.151940469391 687.858115411173,112.515510998746 690.358829177519,118.918789545958 692.859541299544,125.322075829094 693.268006580434,132.48450368372 691.584221731545,140.406088581683 689.605774534102,149.71395083679 685.207158093087,158.295672966865 678.388375697145,166.151239500064 671.569593301202,174.006806033262 663.107816382133,179.964503165887 653.003017284891,184.024315426093 642.898249131339,188.084127686299 630.815442985776,190.114033816402 616.754629791891,190.114033816402 592.065695350521,190.114033816402 575.978540364468,185.361082877624 568.493180305578,175.855181000069 561.007820246688,166.349279122513 558.195520888658,154.268862153119 560.056266759642,139.613930091887L603.302082783918,136.940395188824C602.752950788622,143.871781974542 603.511801776868,149.152848887748 605.578642325941,152.783564984751 608.95076600699,158.658735293458 614.927671796385,161.596328183735 623.50939392646,161.596328183735 629.912680209596,161.596328183735 635.166380952153,160.094522556214 639.270508337334,157.090926773018 643.374635722516,154.087330989822 645.84764727165,150.605129137811 646.689539696094,146.644336688829 647.489337499316,142.881583862297 646.554610715736,139.514910280662 643.885374817199,136.544315943926 641.216138918663,133.57372160719 634.154935907169,130.768165446443 622.70180001505,128.12763198984 603.969971687015,123.704749667119 591.190983055801,117.829571622488 584.364801533397,110.502105591872 577.472617118073,103.174639561256 575.233228553311,93.8337758596899 577.646654599598,82.4794990153275 579.23221756942,75.0200117270274 582.892012991912,67.9730966709325 588.626053050276,61.3387693188883 594.36009310864,54.7044419668442 601.970873230911,49.4894088902487 611.458430938064,45.693639145411 620.945985356576,41.8978848724188 633.248263573754,40 648.365293244649,40z M802.805656560369,42.4754952806135L847.661631045086,42.4754952806135 836.001420966535,97.3324706990085 894.64807865568,42.4754952806135 954.305967733915,42.4754952806135 889.687065316924,97.2674889478924 925.827085847427,187.638538535789 870.587955845087,187.638538535789 852.670777318299,127.940422659244 824.335620196103,152.21574825476 816.806273689209,187.638538535789 771.950299204492,187.638538535789 802.805656560369,42.4754952806135z M1027.45311592827,80.1030235459386L1000.50563596389,132.286464061271 1032.38382643999,132.286464061271 1027.45311592827,80.1030235459386z M1011.53780954977,42.4754952806135L1060.46597377109,42.4754952806135 1084.15815492353,187.638538535789 1037.18563197389,187.638538535789 1035.01659686228,163.67574421945 984.091017386409,163.67574421945 971.922293269619,187.638538535789 926.122536209168,187.638538535789 1011.53780954977,42.4754952806135z M1168.6907416789,71.7853594030773L1162.41864311679,101.29326314799 1181.33142706068,101.29326314799C1183.37785712178,101.29326314799 1187.47896442673,100.633133651801 1193.63475061984,99.3128669234993 1196.73162882025,98.7187480561521 1199.4803385284,97.2004494413243 1201.88088303296,94.7579556071705 1204.28142589319,92.3154695089394 1205.81845511525,89.5099056122699 1206.4919690548,86.3412716530846 1207.48820732751,81.6543390790715 1206.76762965946,78.0566166926057 1204.33023440631,75.5481122296098 1201.89283750884,73.0396155025366 1196.58134082528,71.785367139 1188.39568411258,71.7853594030773L1168.6907416789,71.7853594030773z M1129.86673151262,42.4754952806135L1204.62668898715,42.4754952806135C1218.48946255858,42.4754952806135 1228.83201463227,43.663733015308 1235.65434520822,46.0402084846969 1242.47667578417,48.4166839540859 1247.44827316084,52.8230655535779 1250.56907545083,59.259353283173 1253.68987774083,65.695641012768 1254.26810090479,73.5347145586593 1252.30368634397,82.7765584490012 1250.59183786616,90.8301723405713 1247.39764393546,97.7780598495189 1242.72110290755,103.620228711767 1238.04456187965,109.462397574015 1232.3152340731,114.198847789563 1225.533244907,117.829571622488 1221.21131149788,120.140036463035 1215.55325191396,122.054422058684 1208.55906779955,123.572720673512 1213.52270052634,125.357142766928 1217.03574130082,127.14104655352 1219.09812823561,128.924432033288 1220.49463889995,130.113706381631 1222.34538241755,132.657540803835 1224.65030018966,136.555919828054 1226.95521467312,140.454314324119 1228.41056835278,143.460756926887 1229.0163026299,145.575232164514L1241.79794194253,187.638538535789 1191.11372325652,187.638538535789 1176.566199433,143.277663107195C1174.7503152751,137.534514056172 1172.83657423801,133.804778147944 1170.82492101164,132.088424438822 1168.06732504159,129.84397796304 1164.64212876132,128.721750857188 1160.5493305265,128.721750857188L1156.58853807751,128.721750857188 1144.06538826391,187.638538535789 1099.01137415674,187.638538535789 1129.86673151262,42.4754952806135z M1275.03126006496,42.4754952806135L1411.38154012115,42.4754952806135 1403.76241367993,78.3206669438969 1358.0152608942,78.3206669438969 1334.77902997954,187.638538535789 1289.92305549482,187.638538535789 1313.15928640948,78.3206669438969 1267.41213362374,78.3206669438969 1275.03126006496,42.4754952806135z'><GeometryDrawing.Pen><Pen Brush='#12152A' Thickness='52' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>
  <GeometryDrawing Geometry='F1M419.909607838052,227.596430286554L415.450729896425,248.573801705224 428.895957886244,248.573801705224C430.350762572038,248.573801705224 433.266267919603,248.104511112924 437.642475097898,247.165924428803 439.844067078468,246.743561245877 441.798143686843,245.664192333634 443.504707260942,243.927806693035 445.211269666081,242.191426551956 446.303954295634,240.19693191052 446.782759980641,237.944328268247 447.490992610408,234.612355713731 446.978728997597,232.054710161727 445.245967973249,230.271397111753 443.513205779942,228.488089561301 439.737209812179,227.596435786074 433.917986738439,227.596430286554L419.909607838052,227.596430286554z M392.309352385794,206.759846595526L445.456719570679,206.759846595526C455.311860505624,206.759846595526 462.664451802598,207.604572961378 467.5144934616,209.294025693083 472.364535120601,210.983478424788 475.898855840183,214.116005364825 478.117477618427,218.691606513192 480.33608839763,223.26720766156 480.747140216811,228.840058880406 479.350624414847,235.410149170689 478.133659575801,241.135518594641 475.862885174246,246.074819539577 472.538300041222,250.228057505018 469.213714908198,254.381295470459 465.140718422277,257.748470456406 460.319333750499,260.329576963337 457.246841608952,261.972102285668 453.224488193848,263.333052152715 448.252274674145,264.412421064958 451.780966266555,265.68097898574 454.278400080004,266.94916843864 455.744565115453,268.21698942366 456.737354137313,269.062452725275 458.053043915446,270.870882085879 459.691642287291,273.64226650643 461.330232821697,276.413661926024 462.364842748211,278.550962619166 462.79546340571,280.054157586817L471.882018571351,309.95725095717 435.850259432079,309.95725095717 425.508324840385,278.420799965345C424.217411320934,274.337955863724 422.856908675954,271.686460992493 421.42681058224,270.466293353567 419.4664276683,268.87070094013 417.031432110014,268.072901983652 414.121822738425,268.072901983652L411.306068185583,268.072901983652 402.403274979985,309.95725095717 370.374066941412,309.95725095717 392.309352385794,206.759846595526z M503.954207389664,206.759846595526L589.412347069365,206.759846595526 584.72902896289,228.793125971511 531.229703457941,228.793125971511 527.743399563984,245.194896241814 577.371062558776,245.194896241814 572.897221939492,266.242661524305 523.2695589447,266.242661524305 518.945345101981,286.586488168585 573.993335610993,286.586488168585 569.025726629046,309.95725095717 482.018921945282,309.95725095717 503.954207389664,206.759846595526z M656.941984111389,205C669.941376964314,205 679.601604005187,207.628044915346 685.922691907928,212.884112747956 692.243755474668,218.140191579608 696.053953802997,226.21201946458 697.353285723957,237.099601902394L667.864061884116,243.364655782467C667.54688033629,240.220398364967 667.003202748652,237.92086731332 666.233074286324,236.466057128005 664.968461505935,234.025738348716 663.185256075335,232.148570479995 660.883459163485,230.834548022322 658.581637915633,229.52053656369 655.835141546751,228.863525334853 652.643968887878,228.863519835333 645.416849701237,228.863525334853 639.261450056071,231.769840493596 634.177749123256,237.582459812042 630.351508105635,241.894820906842 627.457097098869,248.667662023492 625.494492935915,257.900988661511 623.063368759884,269.338528659948 623.13335232876,277.178282274656 625.704467978543,281.420238506594 628.275559292325,285.662205737574 632.705380034237,287.783183853543 638.993905868278,287.783183853543 645.09470006674,287.783183853543 650.069589190686,286.070270166911 653.918575578034,282.644431794607 657.767537629381,279.218604421344 661.089251581376,274.244101044977 663.883671099936,267.720932664547L690.034513718112,276.168196323071C686.49148301313,284.005387161175 682.143414048514,290.55200549749 676.990299810507,295.808084329141 671.837163574417,301.064163160793 665.984577536506,305.029680489698 659.432517360771,307.704647314897 652.880457185037,310.379614140097 644.981882040867,311.717097552696 635.736835924425,311.717097552696 624.520739622912,311.717097552696 615.70421470141,310.087402611951 609.287236823919,306.828023729501 602.870258946427,303.568644847051 597.952907773622,297.835394610064 594.53516130742,289.62827301854 591.1174125033,281.421162426057 590.769297088892,270.915791171188 593.490790728193,258.112170252975 597.119238890928,241.041663775893 604.448312155609,227.922007406247 615.478010522237,218.753201144036 626.507707719906,209.584400381345 640.329032249624,205 656.941984111389,205z M764.721719228828,229.145095290617C758.292427665228,229.145095290617 752.646432016528,231.421165387337 747.783689455522,235.973300081258 742.920989721722,240.525440274699 739.372426979458,248.05758370355 737.138001228732,258.569730367811 734.923524935575,268.988022213325 735.268323241236,276.473243732323 738.172378823472,281.025372926723 741.076454065871,285.577513120164 745.860498407669,287.853577717364 752.524421518618,287.853577717364 759.376120261309,287.853577717364 765.152926570948,285.624446029058 769.854889119537,281.166160654365 774.556893326373,276.707886278713 778.134792170415,268.706446758041 780.588671306076,257.161853091391 782.653520822669,247.447499884087 782.202975496092,240.349455615146 779.236993668101,235.867709285526 776.271054667315,231.385968455427 771.432630243877,229.145095290617 764.721719228828,229.145095290617z M769.924311528817,205C787.100414301151,205 799.354405313008,209.610798080278 806.686284564388,218.832394240834 814.018162646809,228.053995900911 815.918506309036,240.971269912072 812.387315551068,257.584216274317 809.823708553981,269.645038941682 805.691777268462,279.535376808539 799.991477698347,287.255229874885 794.291219786478,294.975093940273 787.148220756569,300.982033319988 778.562566263031,305.276059013071 769.976911769493,309.570084706155 759.958731597335,311.717097552696 748.507981750391,311.717097552696 736.869544264034,311.717097552696 727.631348295557,309.863396138423 720.793374184797,306.155982310834 713.955400074037,302.448568483246 709.01937605921,296.582416831173 705.98526048207,288.557516355574 702.95114490493,280.532615879976 702.715899276026,270.489765307535 705.279506273114,258.428942640169 708.860573012922,241.581348232014 716.341535927939,228.464623106853 727.722415847287,219.078772764208 739.103294597676,209.692927921083 753.17057193941,205 769.924311528817,205z M840.013402368145,206.759846595526L881.950546739529,206.759846595526 884.776828482843,269.551173123894 914.181037232466,206.759846595526 956.092883809039,206.759846595526 934.157598364657,309.95725095717 908.041474887051,309.95725095717 924.769748506983,231.256911205248 887.918728971334,309.95725095717 864.279589576931,309.95725095717 860.9225140213,231.256911205248 844.194240401368,309.95725095717 818.078116923762,309.95725095717 840.013402368145,206.759846595526z M1003.86656079578,227.737218014196L998.973765202113,250.756011483676 1006.78748408625,250.756011483676C1012.93520019456,250.756011483676 1017.4796533945,249.688373048897 1020.42075686269,247.553090679819 1023.36185916192,245.417813810261 1025.18654979346,242.684183598704 1025.89478359218,239.352205544667 1026.58306676438,236.1140878089 1026.08725327978,233.368727119879 1024.40729914222,231.116123477606 1022.72734500466,228.863519835333 1018.90739769759,227.737218014196 1012.94736922869,227.737218014196L1003.86656079578,227.737218014196z M976.296230698832,206.759846595526L1029.30281015607,206.759846595526C1040.84740382273,206.759846595526 1048.91055766809,209.505207284547 1053.49231568832,214.995928662588 1058.07402971239,220.486650040629 1059.28761792933,228.300368924764 1057.13299234679,238.437085314994 1054.91851605364,248.855377160508 1050.05530528536,256.997604408822 1042.54336237989,262.863756060895 1035.03141830546,268.729913212488 1024.84611129296,271.662989038525 1011.98752816576,271.662989038525L994.529849938142,271.662989038525 986.390153293023,309.95725095717 954.36094525445,309.95725095717 976.296230698832,206.759846595526z M1081.81645658191,206.759846595526L1113.77527075666,206.759846595526 1091.83998531228,309.95725095717 1059.88117113753,309.95725095717 1081.81645658191,206.759846595526z M1136.65424441413,206.759846595526L1168.54266472506,206.759846595526 1152.00890591466,284.545066117775 1201.77736763614,284.545066117775 1196.37584100215,309.95725095717 1114.71895896975,309.95725095717 1136.65424441413,206.759846595526z M1232.74301243014,206.759846595526L1318.20116310888,206.759846595526 1313.5178450024,228.793125971511 1260.01850849841,228.793125971511 1256.53220460446,245.194896241814 1306.15987859829,245.194896241814 1301.68603797901,266.242661524305 1252.05836398517,266.242661524305 1247.73415014245,286.586488168585 1302.78215165051,286.586488168585 1297.81454266856,309.95725095717 1210.80772698575,309.95725095717 1232.74301243014,206.759846595526z M1364.26884142019,230.130609384111L1352.28373661736,286.516094304764 1360.0974555015,286.516094304764C1366.76137861245,286.516094304764 1371.6583695559,285.776958734643 1374.78834033953,284.298687594401 1377.91831112316,282.82041645416 1380.67264480912,280.239315446749 1383.05125808092,276.555362574087 1385.4298690148,272.871420700466 1387.49702967973,266.899672753141 1389.2526509144,258.640124231632 1391.57685273107,247.705614384445 1391.38454396395,240.220398364967 1388.67581143642,236.184481672721 1385.96707773993,232.148570479995 1380.48292288212,230.130614883632 1372.22334803197,230.130609384111L1364.26884142019,230.130609384111z M1337.34803009121,206.759846595526L1384.72310044277,206.759846595526C1394.06199037892,206.759846595526 1401.33659658214,208.026936144305 1406.54674306779,210.561115241862 1411.75688955343,213.09529433942 1415.73540290312,216.732314303187 1418.48228545479,221.472164134123 1421.2291668375,226.21201946458 1422.7789567684,231.726207297069 1423.13174440876,238.014722132068 1423.48453088017,244.303242466587 1422.91279081753,250.967193075139 1421.41652305189,258.006579457243 1419.07236943973,269.03495512222 1415.99901753555,277.587809576476 1412.19655533168,283.665142820012 1408.3940931278,289.742476063548 1403.82729495956,294.834306212631 1398.4961655028,298.940611269178 1393.16503604604,303.046916325725 1387.7972010986,305.780552036802 1382.39266533633,307.141496404329 1375.00117334918,309.01866977257 1368.46622401584,309.95725095717 1362.78781499839,309.95725095717L1315.41274464683,309.95725095717 1337.34803009121,206.759846595526z'><GeometryDrawing.Pen><Pen Brush='#12152A' Thickness='52' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>
  <GeometryDrawing Geometry='F1M469.249807482985,75.3500726071607L452.390911683491,154.664941398017 463.382110729415,154.664941398017C472.755983613364,154.664941398017 479.644316909207,153.625233380159 484.047102881022,151.545817344444 488.449888852837,149.466401308729 492.324236977236,145.835685211726 495.67016930335,140.653638109745 499.016082868976,135.471606479609 501.923824039778,127.071423248753 504.39338343551,115.453096153099 507.662723518298,100.072023966836 507.39225029839,89.5429147946523 503.581969867387,83.8657763724711 499.771672320217,78.1886456862127 492.057363866767,75.3500803430834 480.439046151358,75.3500726071607L469.249807482985,75.3500726071607z M431.381540121154,42.4754952806135L498.02187307527,42.4754952806135C511.158498785751,42.4754952806135 521.391267719007,44.2578518826552 528.72018761096,47.8225650867386 536.04909976699,51.3872782908221 541.645491546858,56.5033070280385 545.509366239208,63.1706358265423 549.373231551314,69.8379723609689 551.553296196634,77.5945268188654 552.049546347645,86.4402914643091 552.545794854335,95.2860638456756 551.741554399286,104.659936729624 549.636823338176,114.561917852078 546.339410246222,130.075026767871 542.016298392238,142.105933831653 536.667518719916,150.654639043423 531.318723575748,159.203344255193 524.894818593501,166.365787581665 517.395810350462,172.141938079148 509.8967943715,177.918088576631 502.346107787072,181.763368227081 494.743757174463,183.677746086807 484.346523968341,186.31827954341 475.154111246198,187.638538535789 467.166515719393,187.638538535789L400.526182765278,187.638538535789 431.381540121154,42.4754952806135z M648.365293244649,40C666.914999390098,40 680.325189229185,43.4492004055518 688.595822923659,50.3475702729644 696.866484273181,57.2459556122225 700.3264007333,68.2206461989938 698.975569015374,83.2716575051239L656.167942329063,85.7471527857373C656.434843815145,79.2118529808405 655.085132242396,74.4589020420626 652.118838554508,71.4882999694036 649.152541577976,68.5177133685902 644.500752216763,67.0324162002221 638.163485942714,67.0324084642994 632.948435749952,67.0324162002221 628.785632465558,68.1381348469219 625.675043501522,70.3495721403214 622.564452893164,72.5610171696437 620.672409177291,75.2510527959361 619.998895237735,78.4196867551214 619.507789131048,80.730159331591 620.155013531685,82.8095753673063 621.940568439646,84.6579348622674 623.646069057578,86.5723281938388 628.28422639257,88.3546847958806 635.855024972776,90.0050046683926 654.562927912947,94.2298551045881 667.698690954407,98.5042077104731 675.262312452835,102.82807022197 682.825963250632,107.151940469391 687.858115411173,112.515510998746 690.358829177519,118.918789545958 692.859541299544,125.322075829094 693.268006580434,132.48450368372 691.584221731545,140.406088581683 689.605774534102,149.71395083679 685.207158093087,158.295672966865 678.388375697145,166.151239500064 671.569593301202,174.006806033262 663.107816382133,179.964503165887 653.003017284891,184.024315426093 642.898249131339,188.084127686299 630.815442985776,190.114033816402 616.754629791891,190.114033816402 592.065695350521,190.114033816402 575.978540364468,185.361082877624 568.493180305578,175.855181000069 561.007820246688,166.349279122513 558.195520888658,154.268862153119 560.056266759642,139.613930091887L603.302082783918,136.940395188824C602.752950788622,143.871781974542 603.511801776868,149.152848887748 605.578642325941,152.783564984751 608.95076600699,158.658735293458 614.927671796385,161.596328183735 623.50939392646,161.596328183735 629.912680209596,161.596328183735 635.166380952153,160.094522556214 639.270508337334,157.090926773018 643.374635722516,154.087330989822 645.84764727165,150.605129137811 646.689539696094,146.644336688829 647.489337499316,142.881583862297 646.554610715736,139.514910280662 643.885374817199,136.544315943926 641.216138918663,133.57372160719 634.154935907169,130.768165446443 622.70180001505,128.12763198984 603.969971687015,123.704749667119 591.190983055801,117.829571622488 584.364801533397,110.502105591872 577.472617118073,103.174639561256 575.233228553311,93.8337758596899 577.646654599598,82.4794990153275 579.23221756942,75.0200117270274 582.892012991912,67.9730966709325 588.626053050276,61.3387693188883 594.36009310864,54.7044419668442 601.970873230911,49.4894088902487 611.458430938064,45.693639145411 620.945985356576,41.8978848724188 633.248263573754,40 648.365293244649,40z M802.805656560369,42.4754952806135L847.661631045086,42.4754952806135 836.001420966535,97.3324706990085 894.64807865568,42.4754952806135 954.305967733915,42.4754952806135 889.687065316924,97.2674889478924 925.827085847427,187.638538535789 870.587955845087,187.638538535789 852.670777318299,127.940422659244 824.335620196103,152.21574825476 816.806273689209,187.638538535789 771.950299204492,187.638538535789 802.805656560369,42.4754952806135z M1027.45311592827,80.1030235459386L1000.50563596389,132.286464061271 1032.38382643999,132.286464061271 1027.45311592827,80.1030235459386z M1011.53780954977,42.4754952806135L1060.46597377109,42.4754952806135 1084.15815492353,187.638538535789 1037.18563197389,187.638538535789 1035.01659686228,163.67574421945 984.091017386409,163.67574421945 971.922293269619,187.638538535789 926.122536209168,187.638538535789 1011.53780954977,42.4754952806135z M1168.6907416789,71.7853594030773L1162.41864311679,101.29326314799 1181.33142706068,101.29326314799C1183.37785712178,101.29326314799 1187.47896442673,100.633133651801 1193.63475061984,99.3128669234993 1196.73162882025,98.7187480561521 1199.4803385284,97.2004494413243 1201.88088303296,94.7579556071705 1204.28142589319,92.3154695089394 1205.81845511525,89.5099056122699 1206.4919690548,86.3412716530846 1207.48820732751,81.6543390790715 1206.76762965946,78.0566166926057 1204.33023440631,75.5481122296098 1201.89283750884,73.0396155025366 1196.58134082528,71.785367139 1188.39568411258,71.7853594030773L1168.6907416789,71.7853594030773z M1129.86673151262,42.4754952806135L1204.62668898715,42.4754952806135C1218.48946255858,42.4754952806135 1228.83201463227,43.663733015308 1235.65434520822,46.0402084846969 1242.47667578417,48.4166839540859 1247.44827316084,52.8230655535779 1250.56907545083,59.259353283173 1253.68987774083,65.695641012768 1254.26810090479,73.5347145586593 1252.30368634397,82.7765584490012 1250.59183786616,90.8301723405713 1247.39764393546,97.7780598495189 1242.72110290755,103.620228711767 1238.04456187965,109.462397574015 1232.3152340731,114.198847789563 1225.533244907,117.829571622488 1221.21131149788,120.140036463035 1215.55325191396,122.054422058684 1208.55906779955,123.572720673512 1213.52270052634,125.357142766928 1217.03574130082,127.14104655352 1219.09812823561,128.924432033288 1220.49463889995,130.113706381631 1222.34538241755,132.657540803835 1224.65030018966,136.555919828054 1226.95521467312,140.454314324119 1228.41056835278,143.460756926887 1229.0163026299,145.575232164514L1241.79794194253,187.638538535789 1191.11372325652,187.638538535789 1176.566199433,143.277663107195C1174.7503152751,137.534514056172 1172.83657423801,133.804778147944 1170.82492101164,132.088424438822 1168.06732504159,129.84397796304 1164.64212876132,128.721750857188 1160.5493305265,128.721750857188L1156.58853807751,128.721750857188 1144.06538826391,187.638538535789 1099.01137415674,187.638538535789 1129.86673151262,42.4754952806135z M1275.03126006496,42.4754952806135L1411.38154012115,42.4754952806135 1403.76241367993,78.3206669438969 1358.0152608942,78.3206669438969 1334.77902997954,187.638538535789 1289.92305549482,187.638538535789 1313.15928640948,78.3206669438969 1267.41213362374,78.3206669438969 1275.03126006496,42.4754952806135z'><GeometryDrawing.Pen><Pen Brush='#FFFFFF' Thickness='36' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>
  <GeometryDrawing Geometry='F1M419.909607838052,227.596430286554L415.450729896425,248.573801705224 428.895957886244,248.573801705224C430.350762572038,248.573801705224 433.266267919603,248.104511112924 437.642475097898,247.165924428803 439.844067078468,246.743561245877 441.798143686843,245.664192333634 443.504707260942,243.927806693035 445.211269666081,242.191426551956 446.303954295634,240.19693191052 446.782759980641,237.944328268247 447.490992610408,234.612355713731 446.978728997597,232.054710161727 445.245967973249,230.271397111753 443.513205779942,228.488089561301 439.737209812179,227.596435786074 433.917986738439,227.596430286554L419.909607838052,227.596430286554z M392.309352385794,206.759846595526L445.456719570679,206.759846595526C455.311860505624,206.759846595526 462.664451802598,207.604572961378 467.5144934616,209.294025693083 472.364535120601,210.983478424788 475.898855840183,214.116005364825 478.117477618427,218.691606513192 480.33608839763,223.26720766156 480.747140216811,228.840058880406 479.350624414847,235.410149170689 478.133659575801,241.135518594641 475.862885174246,246.074819539577 472.538300041222,250.228057505018 469.213714908198,254.381295470459 465.140718422277,257.748470456406 460.319333750499,260.329576963337 457.246841608952,261.972102285668 453.224488193848,263.333052152715 448.252274674145,264.412421064958 451.780966266555,265.68097898574 454.278400080004,266.94916843864 455.744565115453,268.21698942366 456.737354137313,269.062452725275 458.053043915446,270.870882085879 459.691642287291,273.64226650643 461.330232821697,276.413661926024 462.364842748211,278.550962619166 462.79546340571,280.054157586817L471.882018571351,309.95725095717 435.850259432079,309.95725095717 425.508324840385,278.420799965345C424.217411320934,274.337955863724 422.856908675954,271.686460992493 421.42681058224,270.466293353567 419.4664276683,268.87070094013 417.031432110014,268.072901983652 414.121822738425,268.072901983652L411.306068185583,268.072901983652 402.403274979985,309.95725095717 370.374066941412,309.95725095717 392.309352385794,206.759846595526z M503.954207389664,206.759846595526L589.412347069365,206.759846595526 584.72902896289,228.793125971511 531.229703457941,228.793125971511 527.743399563984,245.194896241814 577.371062558776,245.194896241814 572.897221939492,266.242661524305 523.2695589447,266.242661524305 518.945345101981,286.586488168585 573.993335610993,286.586488168585 569.025726629046,309.95725095717 482.018921945282,309.95725095717 503.954207389664,206.759846595526z M656.941984111389,205C669.941376964314,205 679.601604005187,207.628044915346 685.922691907928,212.884112747956 692.243755474668,218.140191579608 696.053953802997,226.21201946458 697.353285723957,237.099601902394L667.864061884116,243.364655782467C667.54688033629,240.220398364967 667.003202748652,237.92086731332 666.233074286324,236.466057128005 664.968461505935,234.025738348716 663.185256075335,232.148570479995 660.883459163485,230.834548022322 658.581637915633,229.52053656369 655.835141546751,228.863525334853 652.643968887878,228.863519835333 645.416849701237,228.863525334853 639.261450056071,231.769840493596 634.177749123256,237.582459812042 630.351508105635,241.894820906842 627.457097098869,248.667662023492 625.494492935915,257.900988661511 623.063368759884,269.338528659948 623.13335232876,277.178282274656 625.704467978543,281.420238506594 628.275559292325,285.662205737574 632.705380034237,287.783183853543 638.993905868278,287.783183853543 645.09470006674,287.783183853543 650.069589190686,286.070270166911 653.918575578034,282.644431794607 657.767537629381,279.218604421344 661.089251581376,274.244101044977 663.883671099936,267.720932664547L690.034513718112,276.168196323071C686.49148301313,284.005387161175 682.143414048514,290.55200549749 676.990299810507,295.808084329141 671.837163574417,301.064163160793 665.984577536506,305.029680489698 659.432517360771,307.704647314897 652.880457185037,310.379614140097 644.981882040867,311.717097552696 635.736835924425,311.717097552696 624.520739622912,311.717097552696 615.70421470141,310.087402611951 609.287236823919,306.828023729501 602.870258946427,303.568644847051 597.952907773622,297.835394610064 594.53516130742,289.62827301854 591.1174125033,281.421162426057 590.769297088892,270.915791171188 593.490790728193,258.112170252975 597.119238890928,241.041663775893 604.448312155609,227.922007406247 615.478010522237,218.753201144036 626.507707719906,209.584400381345 640.329032249624,205 656.941984111389,205z M764.721719228828,229.145095290617C758.292427665228,229.145095290617 752.646432016528,231.421165387337 747.783689455522,235.973300081258 742.920989721722,240.525440274699 739.372426979458,248.05758370355 737.138001228732,258.569730367811 734.923524935575,268.988022213325 735.268323241236,276.473243732323 738.172378823472,281.025372926723 741.076454065871,285.577513120164 745.860498407669,287.853577717364 752.524421518618,287.853577717364 759.376120261309,287.853577717364 765.152926570948,285.624446029058 769.854889119537,281.166160654365 774.556893326373,276.707886278713 778.134792170415,268.706446758041 780.588671306076,257.161853091391 782.653520822669,247.447499884087 782.202975496092,240.349455615146 779.236993668101,235.867709285526 776.271054667315,231.385968455427 771.432630243877,229.145095290617 764.721719228828,229.145095290617z M769.924311528817,205C787.100414301151,205 799.354405313008,209.610798080278 806.686284564388,218.832394240834 814.018162646809,228.053995900911 815.918506309036,240.971269912072 812.387315551068,257.584216274317 809.823708553981,269.645038941682 805.691777268462,279.535376808539 799.991477698347,287.255229874885 794.291219786478,294.975093940273 787.148220756569,300.982033319988 778.562566263031,305.276059013071 769.976911769493,309.570084706155 759.958731597335,311.717097552696 748.507981750391,311.717097552696 736.869544264034,311.717097552696 727.631348295557,309.863396138423 720.793374184797,306.155982310834 713.955400074037,302.448568483246 709.01937605921,296.582416831173 705.98526048207,288.557516355574 702.95114490493,280.532615879976 702.715899276026,270.489765307535 705.279506273114,258.428942640169 708.860573012922,241.581348232014 716.341535927939,228.464623106853 727.722415847287,219.078772764208 739.103294597676,209.692927921083 753.17057193941,205 769.924311528817,205z M840.013402368145,206.759846595526L881.950546739529,206.759846595526 884.776828482843,269.551173123894 914.181037232466,206.759846595526 956.092883809039,206.759846595526 934.157598364657,309.95725095717 908.041474887051,309.95725095717 924.769748506983,231.256911205248 887.918728971334,309.95725095717 864.279589576931,309.95725095717 860.9225140213,231.256911205248 844.194240401368,309.95725095717 818.078116923762,309.95725095717 840.013402368145,206.759846595526z M1003.86656079578,227.737218014196L998.973765202113,250.756011483676 1006.78748408625,250.756011483676C1012.93520019456,250.756011483676 1017.4796533945,249.688373048897 1020.42075686269,247.553090679819 1023.36185916192,245.417813810261 1025.18654979346,242.684183598704 1025.89478359218,239.352205544667 1026.58306676438,236.1140878089 1026.08725327978,233.368727119879 1024.40729914222,231.116123477606 1022.72734500466,228.863519835333 1018.90739769759,227.737218014196 1012.94736922869,227.737218014196L1003.86656079578,227.737218014196z M976.296230698832,206.759846595526L1029.30281015607,206.759846595526C1040.84740382273,206.759846595526 1048.91055766809,209.505207284547 1053.49231568832,214.995928662588 1058.07402971239,220.486650040629 1059.28761792933,228.300368924764 1057.13299234679,238.437085314994 1054.91851605364,248.855377160508 1050.05530528536,256.997604408822 1042.54336237989,262.863756060895 1035.03141830546,268.729913212488 1024.84611129296,271.662989038525 1011.98752816576,271.662989038525L994.529849938142,271.662989038525 986.390153293023,309.95725095717 954.36094525445,309.95725095717 976.296230698832,206.759846595526z M1081.81645658191,206.759846595526L1113.77527075666,206.759846595526 1091.83998531228,309.95725095717 1059.88117113753,309.95725095717 1081.81645658191,206.759846595526z M1136.65424441413,206.759846595526L1168.54266472506,206.759846595526 1152.00890591466,284.545066117775 1201.77736763614,284.545066117775 1196.37584100215,309.95725095717 1114.71895896975,309.95725095717 1136.65424441413,206.759846595526z M1232.74301243014,206.759846595526L1318.20116310888,206.759846595526 1313.5178450024,228.793125971511 1260.01850849841,228.793125971511 1256.53220460446,245.194896241814 1306.15987859829,245.194896241814 1301.68603797901,266.242661524305 1252.05836398517,266.242661524305 1247.73415014245,286.586488168585 1302.78215165051,286.586488168585 1297.81454266856,309.95725095717 1210.80772698575,309.95725095717 1232.74301243014,206.759846595526z M1364.26884142019,230.130609384111L1352.28373661736,286.516094304764 1360.0974555015,286.516094304764C1366.76137861245,286.516094304764 1371.6583695559,285.776958734643 1374.78834033953,284.298687594401 1377.91831112316,282.82041645416 1380.67264480912,280.239315446749 1383.05125808092,276.555362574087 1385.4298690148,272.871420700466 1387.49702967973,266.899672753141 1389.2526509144,258.640124231632 1391.57685273107,247.705614384445 1391.38454396395,240.220398364967 1388.67581143642,236.184481672721 1385.96707773993,232.148570479995 1380.48292288212,230.130614883632 1372.22334803197,230.130609384111L1364.26884142019,230.130609384111z M1337.34803009121,206.759846595526L1384.72310044277,206.759846595526C1394.06199037892,206.759846595526 1401.33659658214,208.026936144305 1406.54674306779,210.561115241862 1411.75688955343,213.09529433942 1415.73540290312,216.732314303187 1418.48228545479,221.472164134123 1421.2291668375,226.21201946458 1422.7789567684,231.726207297069 1423.13174440876,238.014722132068 1423.48453088017,244.303242466587 1422.91279081753,250.967193075139 1421.41652305189,258.006579457243 1419.07236943973,269.03495512222 1415.99901753555,277.587809576476 1412.19655533168,283.665142820012 1408.3940931278,289.742476063548 1403.82729495956,294.834306212631 1398.4961655028,298.940611269178 1393.16503604604,303.046916325725 1387.7972010986,305.780552036802 1382.39266533633,307.141496404329 1375.00117334918,309.01866977257 1368.46622401584,309.95725095717 1362.78781499839,309.95725095717L1315.41274464683,309.95725095717 1337.34803009121,206.759846595526z'><GeometryDrawing.Pen><Pen Brush='#FFFFFF' Thickness='36' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>
  <GeometryDrawing Geometry='F1M469.249807482985,75.3500726071607L452.390911683491,154.664941398017 463.382110729415,154.664941398017C472.755983613364,154.664941398017 479.644316909207,153.625233380159 484.047102881022,151.545817344444 488.449888852837,149.466401308729 492.324236977236,145.835685211726 495.67016930335,140.653638109745 499.016082868976,135.471606479609 501.923824039778,127.071423248753 504.39338343551,115.453096153099 507.662723518298,100.072023966836 507.39225029839,89.5429147946523 503.581969867387,83.8657763724711 499.771672320217,78.1886456862127 492.057363866767,75.3500803430834 480.439046151358,75.3500726071607L469.249807482985,75.3500726071607z M431.381540121154,42.4754952806135L498.02187307527,42.4754952806135C511.158498785751,42.4754952806135 521.391267719007,44.2578518826552 528.72018761096,47.8225650867386 536.04909976699,51.3872782908221 541.645491546858,56.5033070280385 545.509366239208,63.1706358265423 549.373231551314,69.8379723609689 551.553296196634,77.5945268188654 552.049546347645,86.4402914643091 552.545794854335,95.2860638456756 551.741554399286,104.659936729624 549.636823338176,114.561917852078 546.339410246222,130.075026767871 542.016298392238,142.105933831653 536.667518719916,150.654639043423 531.318723575748,159.203344255193 524.894818593501,166.365787581665 517.395810350462,172.141938079148 509.8967943715,177.918088576631 502.346107787072,181.763368227081 494.743757174463,183.677746086807 484.346523968341,186.31827954341 475.154111246198,187.638538535789 467.166515719393,187.638538535789L400.526182765278,187.638538535789 431.381540121154,42.4754952806135z M648.365293244649,40C666.914999390098,40 680.325189229185,43.4492004055518 688.595822923659,50.3475702729644 696.866484273181,57.2459556122225 700.3264007333,68.2206461989938 698.975569015374,83.2716575051239L656.167942329063,85.7471527857373C656.434843815145,79.2118529808405 655.085132242396,74.4589020420626 652.118838554508,71.4882999694036 649.152541577976,68.5177133685902 644.500752216763,67.0324162002221 638.163485942714,67.0324084642994 632.948435749952,67.0324162002221 628.785632465558,68.1381348469219 625.675043501522,70.3495721403214 622.564452893164,72.5610171696437 620.672409177291,75.2510527959361 619.998895237735,78.4196867551214 619.507789131048,80.730159331591 620.155013531685,82.8095753673063 621.940568439646,84.6579348622674 623.646069057578,86.5723281938388 628.28422639257,88.3546847958806 635.855024972776,90.0050046683926 654.562927912947,94.2298551045881 667.698690954407,98.5042077104731 675.262312452835,102.82807022197 682.825963250632,107.151940469391 687.858115411173,112.515510998746 690.358829177519,118.918789545958 692.859541299544,125.322075829094 693.268006580434,132.48450368372 691.584221731545,140.406088581683 689.605774534102,149.71395083679 685.207158093087,158.295672966865 678.388375697145,166.151239500064 671.569593301202,174.006806033262 663.107816382133,179.964503165887 653.003017284891,184.024315426093 642.898249131339,188.084127686299 630.815442985776,190.114033816402 616.754629791891,190.114033816402 592.065695350521,190.114033816402 575.978540364468,185.361082877624 568.493180305578,175.855181000069 561.007820246688,166.349279122513 558.195520888658,154.268862153119 560.056266759642,139.613930091887L603.302082783918,136.940395188824C602.752950788622,143.871781974542 603.511801776868,149.152848887748 605.578642325941,152.783564984751 608.95076600699,158.658735293458 614.927671796385,161.596328183735 623.50939392646,161.596328183735 629.912680209596,161.596328183735 635.166380952153,160.094522556214 639.270508337334,157.090926773018 643.374635722516,154.087330989822 645.84764727165,150.605129137811 646.689539696094,146.644336688829 647.489337499316,142.881583862297 646.554610715736,139.514910280662 643.885374817199,136.544315943926 641.216138918663,133.57372160719 634.154935907169,130.768165446443 622.70180001505,128.12763198984 603.969971687015,123.704749667119 591.190983055801,117.829571622488 584.364801533397,110.502105591872 577.472617118073,103.174639561256 575.233228553311,93.8337758596899 577.646654599598,82.4794990153275 579.23221756942,75.0200117270274 582.892012991912,67.9730966709325 588.626053050276,61.3387693188883 594.36009310864,54.7044419668442 601.970873230911,49.4894088902487 611.458430938064,45.693639145411 620.945985356576,41.8978848724188 633.248263573754,40 648.365293244649,40z M802.805656560369,42.4754952806135L847.661631045086,42.4754952806135 836.001420966535,97.3324706990085 894.64807865568,42.4754952806135 954.305967733915,42.4754952806135 889.687065316924,97.2674889478924 925.827085847427,187.638538535789 870.587955845087,187.638538535789 852.670777318299,127.940422659244 824.335620196103,152.21574825476 816.806273689209,187.638538535789 771.950299204492,187.638538535789 802.805656560369,42.4754952806135z M1027.45311592827,80.1030235459386L1000.50563596389,132.286464061271 1032.38382643999,132.286464061271 1027.45311592827,80.1030235459386z M1011.53780954977,42.4754952806135L1060.46597377109,42.4754952806135 1084.15815492353,187.638538535789 1037.18563197389,187.638538535789 1035.01659686228,163.67574421945 984.091017386409,163.67574421945 971.922293269619,187.638538535789 926.122536209168,187.638538535789 1011.53780954977,42.4754952806135z M1168.6907416789,71.7853594030773L1162.41864311679,101.29326314799 1181.33142706068,101.29326314799C1183.37785712178,101.29326314799 1187.47896442673,100.633133651801 1193.63475061984,99.3128669234993 1196.73162882025,98.7187480561521 1199.4803385284,97.2004494413243 1201.88088303296,94.7579556071705 1204.28142589319,92.3154695089394 1205.81845511525,89.5099056122699 1206.4919690548,86.3412716530846 1207.48820732751,81.6543390790715 1206.76762965946,78.0566166926057 1204.33023440631,75.5481122296098 1201.89283750884,73.0396155025366 1196.58134082528,71.785367139 1188.39568411258,71.7853594030773L1168.6907416789,71.7853594030773z M1129.86673151262,42.4754952806135L1204.62668898715,42.4754952806135C1218.48946255858,42.4754952806135 1228.83201463227,43.663733015308 1235.65434520822,46.0402084846969 1242.47667578417,48.4166839540859 1247.44827316084,52.8230655535779 1250.56907545083,59.259353283173 1253.68987774083,65.695641012768 1254.26810090479,73.5347145586593 1252.30368634397,82.7765584490012 1250.59183786616,90.8301723405713 1247.39764393546,97.7780598495189 1242.72110290755,103.620228711767 1238.04456187965,109.462397574015 1232.3152340731,114.198847789563 1225.533244907,117.829571622488 1221.21131149788,120.140036463035 1215.55325191396,122.054422058684 1208.55906779955,123.572720673512 1213.52270052634,125.357142766928 1217.03574130082,127.14104655352 1219.09812823561,128.924432033288 1220.49463889995,130.113706381631 1222.34538241755,132.657540803835 1224.65030018966,136.555919828054 1226.95521467312,140.454314324119 1228.41056835278,143.460756926887 1229.0163026299,145.575232164514L1241.79794194253,187.638538535789 1191.11372325652,187.638538535789 1176.566199433,143.277663107195C1174.7503152751,137.534514056172 1172.83657423801,133.804778147944 1170.82492101164,132.088424438822 1168.06732504159,129.84397796304 1164.64212876132,128.721750857188 1160.5493305265,128.721750857188L1156.58853807751,128.721750857188 1144.06538826391,187.638538535789 1099.01137415674,187.638538535789 1129.86673151262,42.4754952806135z M1275.03126006496,42.4754952806135L1411.38154012115,42.4754952806135 1403.76241367993,78.3206669438969 1358.0152608942,78.3206669438969 1334.77902997954,187.638538535789 1289.92305549482,187.638538535789 1313.15928640948,78.3206669438969 1267.41213362374,78.3206669438969 1275.03126006496,42.4754952806135z'><GeometryDrawing.Pen><Pen Brush='#12152A' Thickness='16' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>
  <GeometryDrawing Geometry='F1M419.909607838052,227.596430286554L415.450729896425,248.573801705224 428.895957886244,248.573801705224C430.350762572038,248.573801705224 433.266267919603,248.104511112924 437.642475097898,247.165924428803 439.844067078468,246.743561245877 441.798143686843,245.664192333634 443.504707260942,243.927806693035 445.211269666081,242.191426551956 446.303954295634,240.19693191052 446.782759980641,237.944328268247 447.490992610408,234.612355713731 446.978728997597,232.054710161727 445.245967973249,230.271397111753 443.513205779942,228.488089561301 439.737209812179,227.596435786074 433.917986738439,227.596430286554L419.909607838052,227.596430286554z M392.309352385794,206.759846595526L445.456719570679,206.759846595526C455.311860505624,206.759846595526 462.664451802598,207.604572961378 467.5144934616,209.294025693083 472.364535120601,210.983478424788 475.898855840183,214.116005364825 478.117477618427,218.691606513192 480.33608839763,223.26720766156 480.747140216811,228.840058880406 479.350624414847,235.410149170689 478.133659575801,241.135518594641 475.862885174246,246.074819539577 472.538300041222,250.228057505018 469.213714908198,254.381295470459 465.140718422277,257.748470456406 460.319333750499,260.329576963337 457.246841608952,261.972102285668 453.224488193848,263.333052152715 448.252274674145,264.412421064958 451.780966266555,265.68097898574 454.278400080004,266.94916843864 455.744565115453,268.21698942366 456.737354137313,269.062452725275 458.053043915446,270.870882085879 459.691642287291,273.64226650643 461.330232821697,276.413661926024 462.364842748211,278.550962619166 462.79546340571,280.054157586817L471.882018571351,309.95725095717 435.850259432079,309.95725095717 425.508324840385,278.420799965345C424.217411320934,274.337955863724 422.856908675954,271.686460992493 421.42681058224,270.466293353567 419.4664276683,268.87070094013 417.031432110014,268.072901983652 414.121822738425,268.072901983652L411.306068185583,268.072901983652 402.403274979985,309.95725095717 370.374066941412,309.95725095717 392.309352385794,206.759846595526z M503.954207389664,206.759846595526L589.412347069365,206.759846595526 584.72902896289,228.793125971511 531.229703457941,228.793125971511 527.743399563984,245.194896241814 577.371062558776,245.194896241814 572.897221939492,266.242661524305 523.2695589447,266.242661524305 518.945345101981,286.586488168585 573.993335610993,286.586488168585 569.025726629046,309.95725095717 482.018921945282,309.95725095717 503.954207389664,206.759846595526z M656.941984111389,205C669.941376964314,205 679.601604005187,207.628044915346 685.922691907928,212.884112747956 692.243755474668,218.140191579608 696.053953802997,226.21201946458 697.353285723957,237.099601902394L667.864061884116,243.364655782467C667.54688033629,240.220398364967 667.003202748652,237.92086731332 666.233074286324,236.466057128005 664.968461505935,234.025738348716 663.185256075335,232.148570479995 660.883459163485,230.834548022322 658.581637915633,229.52053656369 655.835141546751,228.863525334853 652.643968887878,228.863519835333 645.416849701237,228.863525334853 639.261450056071,231.769840493596 634.177749123256,237.582459812042 630.351508105635,241.894820906842 627.457097098869,248.667662023492 625.494492935915,257.900988661511 623.063368759884,269.338528659948 623.13335232876,277.178282274656 625.704467978543,281.420238506594 628.275559292325,285.662205737574 632.705380034237,287.783183853543 638.993905868278,287.783183853543 645.09470006674,287.783183853543 650.069589190686,286.070270166911 653.918575578034,282.644431794607 657.767537629381,279.218604421344 661.089251581376,274.244101044977 663.883671099936,267.720932664547L690.034513718112,276.168196323071C686.49148301313,284.005387161175 682.143414048514,290.55200549749 676.990299810507,295.808084329141 671.837163574417,301.064163160793 665.984577536506,305.029680489698 659.432517360771,307.704647314897 652.880457185037,310.379614140097 644.981882040867,311.717097552696 635.736835924425,311.717097552696 624.520739622912,311.717097552696 615.70421470141,310.087402611951 609.287236823919,306.828023729501 602.870258946427,303.568644847051 597.952907773622,297.835394610064 594.53516130742,289.62827301854 591.1174125033,281.421162426057 590.769297088892,270.915791171188 593.490790728193,258.112170252975 597.119238890928,241.041663775893 604.448312155609,227.922007406247 615.478010522237,218.753201144036 626.507707719906,209.584400381345 640.329032249624,205 656.941984111389,205z M764.721719228828,229.145095290617C758.292427665228,229.145095290617 752.646432016528,231.421165387337 747.783689455522,235.973300081258 742.920989721722,240.525440274699 739.372426979458,248.05758370355 737.138001228732,258.569730367811 734.923524935575,268.988022213325 735.268323241236,276.473243732323 738.172378823472,281.025372926723 741.076454065871,285.577513120164 745.860498407669,287.853577717364 752.524421518618,287.853577717364 759.376120261309,287.853577717364 765.152926570948,285.624446029058 769.854889119537,281.166160654365 774.556893326373,276.707886278713 778.134792170415,268.706446758041 780.588671306076,257.161853091391 782.653520822669,247.447499884087 782.202975496092,240.349455615146 779.236993668101,235.867709285526 776.271054667315,231.385968455427 771.432630243877,229.145095290617 764.721719228828,229.145095290617z M769.924311528817,205C787.100414301151,205 799.354405313008,209.610798080278 806.686284564388,218.832394240834 814.018162646809,228.053995900911 815.918506309036,240.971269912072 812.387315551068,257.584216274317 809.823708553981,269.645038941682 805.691777268462,279.535376808539 799.991477698347,287.255229874885 794.291219786478,294.975093940273 787.148220756569,300.982033319988 778.562566263031,305.276059013071 769.976911769493,309.570084706155 759.958731597335,311.717097552696 748.507981750391,311.717097552696 736.869544264034,311.717097552696 727.631348295557,309.863396138423 720.793374184797,306.155982310834 713.955400074037,302.448568483246 709.01937605921,296.582416831173 705.98526048207,288.557516355574 702.95114490493,280.532615879976 702.715899276026,270.489765307535 705.279506273114,258.428942640169 708.860573012922,241.581348232014 716.341535927939,228.464623106853 727.722415847287,219.078772764208 739.103294597676,209.692927921083 753.17057193941,205 769.924311528817,205z M840.013402368145,206.759846595526L881.950546739529,206.759846595526 884.776828482843,269.551173123894 914.181037232466,206.759846595526 956.092883809039,206.759846595526 934.157598364657,309.95725095717 908.041474887051,309.95725095717 924.769748506983,231.256911205248 887.918728971334,309.95725095717 864.279589576931,309.95725095717 860.9225140213,231.256911205248 844.194240401368,309.95725095717 818.078116923762,309.95725095717 840.013402368145,206.759846595526z M1003.86656079578,227.737218014196L998.973765202113,250.756011483676 1006.78748408625,250.756011483676C1012.93520019456,250.756011483676 1017.4796533945,249.688373048897 1020.42075686269,247.553090679819 1023.36185916192,245.417813810261 1025.18654979346,242.684183598704 1025.89478359218,239.352205544667 1026.58306676438,236.1140878089 1026.08725327978,233.368727119879 1024.40729914222,231.116123477606 1022.72734500466,228.863519835333 1018.90739769759,227.737218014196 1012.94736922869,227.737218014196L1003.86656079578,227.737218014196z M976.296230698832,206.759846595526L1029.30281015607,206.759846595526C1040.84740382273,206.759846595526 1048.91055766809,209.505207284547 1053.49231568832,214.995928662588 1058.07402971239,220.486650040629 1059.28761792933,228.300368924764 1057.13299234679,238.437085314994 1054.91851605364,248.855377160508 1050.05530528536,256.997604408822 1042.54336237989,262.863756060895 1035.03141830546,268.729913212488 1024.84611129296,271.662989038525 1011.98752816576,271.662989038525L994.529849938142,271.662989038525 986.390153293023,309.95725095717 954.36094525445,309.95725095717 976.296230698832,206.759846595526z M1081.81645658191,206.759846595526L1113.77527075666,206.759846595526 1091.83998531228,309.95725095717 1059.88117113753,309.95725095717 1081.81645658191,206.759846595526z M1136.65424441413,206.759846595526L1168.54266472506,206.759846595526 1152.00890591466,284.545066117775 1201.77736763614,284.545066117775 1196.37584100215,309.95725095717 1114.71895896975,309.95725095717 1136.65424441413,206.759846595526z M1232.74301243014,206.759846595526L1318.20116310888,206.759846595526 1313.5178450024,228.793125971511 1260.01850849841,228.793125971511 1256.53220460446,245.194896241814 1306.15987859829,245.194896241814 1301.68603797901,266.242661524305 1252.05836398517,266.242661524305 1247.73415014245,286.586488168585 1302.78215165051,286.586488168585 1297.81454266856,309.95725095717 1210.80772698575,309.95725095717 1232.74301243014,206.759846595526z M1364.26884142019,230.130609384111L1352.28373661736,286.516094304764 1360.0974555015,286.516094304764C1366.76137861245,286.516094304764 1371.6583695559,285.776958734643 1374.78834033953,284.298687594401 1377.91831112316,282.82041645416 1380.67264480912,280.239315446749 1383.05125808092,276.555362574087 1385.4298690148,272.871420700466 1387.49702967973,266.899672753141 1389.2526509144,258.640124231632 1391.57685273107,247.705614384445 1391.38454396395,240.220398364967 1388.67581143642,236.184481672721 1385.96707773993,232.148570479995 1380.48292288212,230.130614883632 1372.22334803197,230.130609384111L1364.26884142019,230.130609384111z M1337.34803009121,206.759846595526L1384.72310044277,206.759846595526C1394.06199037892,206.759846595526 1401.33659658214,208.026936144305 1406.54674306779,210.561115241862 1411.75688955343,213.09529433942 1415.73540290312,216.732314303187 1418.48228545479,221.472164134123 1421.2291668375,226.21201946458 1422.7789567684,231.726207297069 1423.13174440876,238.014722132068 1423.48453088017,244.303242466587 1422.91279081753,250.967193075139 1421.41652305189,258.006579457243 1419.07236943973,269.03495512222 1415.99901753555,277.587809576476 1412.19655533168,283.665142820012 1408.3940931278,289.742476063548 1403.82729495956,294.834306212631 1398.4961655028,298.940611269178 1393.16503604604,303.046916325725 1387.7972010986,305.780552036802 1382.39266533633,307.141496404329 1375.00117334918,309.01866977257 1368.46622401584,309.95725095717 1362.78781499839,309.95725095717L1315.41274464683,309.95725095717 1337.34803009121,206.759846595526z'><GeometryDrawing.Pen><Pen Brush='#12152A' Thickness='16' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>
  <GeometryDrawing Geometry='F1M469.249807482985,75.3500726071607L452.390911683491,154.664941398017 463.382110729415,154.664941398017C472.755983613364,154.664941398017 479.644316909207,153.625233380159 484.047102881022,151.545817344444 488.449888852837,149.466401308729 492.324236977236,145.835685211726 495.67016930335,140.653638109745 499.016082868976,135.471606479609 501.923824039778,127.071423248753 504.39338343551,115.453096153099 507.662723518298,100.072023966836 507.39225029839,89.5429147946523 503.581969867387,83.8657763724711 499.771672320217,78.1886456862127 492.057363866767,75.3500803430834 480.439046151358,75.3500726071607L469.249807482985,75.3500726071607z M431.381540121154,42.4754952806135L498.02187307527,42.4754952806135C511.158498785751,42.4754952806135 521.391267719007,44.2578518826552 528.72018761096,47.8225650867386 536.04909976699,51.3872782908221 541.645491546858,56.5033070280385 545.509366239208,63.1706358265423 549.373231551314,69.8379723609689 551.553296196634,77.5945268188654 552.049546347645,86.4402914643091 552.545794854335,95.2860638456756 551.741554399286,104.659936729624 549.636823338176,114.561917852078 546.339410246222,130.075026767871 542.016298392238,142.105933831653 536.667518719916,150.654639043423 531.318723575748,159.203344255193 524.894818593501,166.365787581665 517.395810350462,172.141938079148 509.8967943715,177.918088576631 502.346107787072,181.763368227081 494.743757174463,183.677746086807 484.346523968341,186.31827954341 475.154111246198,187.638538535789 467.166515719393,187.638538535789L400.526182765278,187.638538535789 431.381540121154,42.4754952806135z M648.365293244649,40C666.914999390098,40 680.325189229185,43.4492004055518 688.595822923659,50.3475702729644 696.866484273181,57.2459556122225 700.3264007333,68.2206461989938 698.975569015374,83.2716575051239L656.167942329063,85.7471527857373C656.434843815145,79.2118529808405 655.085132242396,74.4589020420626 652.118838554508,71.4882999694036 649.152541577976,68.5177133685902 644.500752216763,67.0324162002221 638.163485942714,67.0324084642994 632.948435749952,67.0324162002221 628.785632465558,68.1381348469219 625.675043501522,70.3495721403214 622.564452893164,72.5610171696437 620.672409177291,75.2510527959361 619.998895237735,78.4196867551214 619.507789131048,80.730159331591 620.155013531685,82.8095753673063 621.940568439646,84.6579348622674 623.646069057578,86.5723281938388 628.28422639257,88.3546847958806 635.855024972776,90.0050046683926 654.562927912947,94.2298551045881 667.698690954407,98.5042077104731 675.262312452835,102.82807022197 682.825963250632,107.151940469391 687.858115411173,112.515510998746 690.358829177519,118.918789545958 692.859541299544,125.322075829094 693.268006580434,132.48450368372 691.584221731545,140.406088581683 689.605774534102,149.71395083679 685.207158093087,158.295672966865 678.388375697145,166.151239500064 671.569593301202,174.006806033262 663.107816382133,179.964503165887 653.003017284891,184.024315426093 642.898249131339,188.084127686299 630.815442985776,190.114033816402 616.754629791891,190.114033816402 592.065695350521,190.114033816402 575.978540364468,185.361082877624 568.493180305578,175.855181000069 561.007820246688,166.349279122513 558.195520888658,154.268862153119 560.056266759642,139.613930091887L603.302082783918,136.940395188824C602.752950788622,143.871781974542 603.511801776868,149.152848887748 605.578642325941,152.783564984751 608.95076600699,158.658735293458 614.927671796385,161.596328183735 623.50939392646,161.596328183735 629.912680209596,161.596328183735 635.166380952153,160.094522556214 639.270508337334,157.090926773018 643.374635722516,154.087330989822 645.84764727165,150.605129137811 646.689539696094,146.644336688829 647.489337499316,142.881583862297 646.554610715736,139.514910280662 643.885374817199,136.544315943926 641.216138918663,133.57372160719 634.154935907169,130.768165446443 622.70180001505,128.12763198984 603.969971687015,123.704749667119 591.190983055801,117.829571622488 584.364801533397,110.502105591872 577.472617118073,103.174639561256 575.233228553311,93.8337758596899 577.646654599598,82.4794990153275 579.23221756942,75.0200117270274 582.892012991912,67.9730966709325 588.626053050276,61.3387693188883 594.36009310864,54.7044419668442 601.970873230911,49.4894088902487 611.458430938064,45.693639145411 620.945985356576,41.8978848724188 633.248263573754,40 648.365293244649,40z M802.805656560369,42.4754952806135L847.661631045086,42.4754952806135 836.001420966535,97.3324706990085 894.64807865568,42.4754952806135 954.305967733915,42.4754952806135 889.687065316924,97.2674889478924 925.827085847427,187.638538535789 870.587955845087,187.638538535789 852.670777318299,127.940422659244 824.335620196103,152.21574825476 816.806273689209,187.638538535789 771.950299204492,187.638538535789 802.805656560369,42.4754952806135z M1027.45311592827,80.1030235459386L1000.50563596389,132.286464061271 1032.38382643999,132.286464061271 1027.45311592827,80.1030235459386z M1011.53780954977,42.4754952806135L1060.46597377109,42.4754952806135 1084.15815492353,187.638538535789 1037.18563197389,187.638538535789 1035.01659686228,163.67574421945 984.091017386409,163.67574421945 971.922293269619,187.638538535789 926.122536209168,187.638538535789 1011.53780954977,42.4754952806135z M1168.6907416789,71.7853594030773L1162.41864311679,101.29326314799 1181.33142706068,101.29326314799C1183.37785712178,101.29326314799 1187.47896442673,100.633133651801 1193.63475061984,99.3128669234993 1196.73162882025,98.7187480561521 1199.4803385284,97.2004494413243 1201.88088303296,94.7579556071705 1204.28142589319,92.3154695089394 1205.81845511525,89.5099056122699 1206.4919690548,86.3412716530846 1207.48820732751,81.6543390790715 1206.76762965946,78.0566166926057 1204.33023440631,75.5481122296098 1201.89283750884,73.0396155025366 1196.58134082528,71.785367139 1188.39568411258,71.7853594030773L1168.6907416789,71.7853594030773z M1129.86673151262,42.4754952806135L1204.62668898715,42.4754952806135C1218.48946255858,42.4754952806135 1228.83201463227,43.663733015308 1235.65434520822,46.0402084846969 1242.47667578417,48.4166839540859 1247.44827316084,52.8230655535779 1250.56907545083,59.259353283173 1253.68987774083,65.695641012768 1254.26810090479,73.5347145586593 1252.30368634397,82.7765584490012 1250.59183786616,90.8301723405713 1247.39764393546,97.7780598495189 1242.72110290755,103.620228711767 1238.04456187965,109.462397574015 1232.3152340731,114.198847789563 1225.533244907,117.829571622488 1221.21131149788,120.140036463035 1215.55325191396,122.054422058684 1208.55906779955,123.572720673512 1213.52270052634,125.357142766928 1217.03574130082,127.14104655352 1219.09812823561,128.924432033288 1220.49463889995,130.113706381631 1222.34538241755,132.657540803835 1224.65030018966,136.555919828054 1226.95521467312,140.454314324119 1228.41056835278,143.460756926887 1229.0163026299,145.575232164514L1241.79794194253,187.638538535789 1191.11372325652,187.638538535789 1176.566199433,143.277663107195C1174.7503152751,137.534514056172 1172.83657423801,133.804778147944 1170.82492101164,132.088424438822 1168.06732504159,129.84397796304 1164.64212876132,128.721750857188 1160.5493305265,128.721750857188L1156.58853807751,128.721750857188 1144.06538826391,187.638538535789 1099.01137415674,187.638538535789 1129.86673151262,42.4754952806135z M1275.03126006496,42.4754952806135L1411.38154012115,42.4754952806135 1403.76241367993,78.3206669438969 1358.0152608942,78.3206669438969 1334.77902997954,187.638538535789 1289.92305549482,187.638538535789 1313.15928640948,78.3206669438969 1267.41213362374,78.3206669438969 1275.03126006496,42.4754952806135z'><GeometryDrawing.Brush><LinearGradientBrush StartPoint='0,0' EndPoint='0,1'><GradientStop Color='#FFFFFF' Offset='0'/><GradientStop Color='#C7D0E4' Offset='0.42'/><GradientStop Color='#56607F' Offset='0.5'/><GradientStop Color='#8E99B8' Offset='0.62'/><GradientStop Color='#EEF1F8' Offset='1'/></LinearGradientBrush></GeometryDrawing.Brush></GeometryDrawing>
  <GeometryDrawing Geometry='F1M419.909607838052,227.596430286554L415.450729896425,248.573801705224 428.895957886244,248.573801705224C430.350762572038,248.573801705224 433.266267919603,248.104511112924 437.642475097898,247.165924428803 439.844067078468,246.743561245877 441.798143686843,245.664192333634 443.504707260942,243.927806693035 445.211269666081,242.191426551956 446.303954295634,240.19693191052 446.782759980641,237.944328268247 447.490992610408,234.612355713731 446.978728997597,232.054710161727 445.245967973249,230.271397111753 443.513205779942,228.488089561301 439.737209812179,227.596435786074 433.917986738439,227.596430286554L419.909607838052,227.596430286554z M392.309352385794,206.759846595526L445.456719570679,206.759846595526C455.311860505624,206.759846595526 462.664451802598,207.604572961378 467.5144934616,209.294025693083 472.364535120601,210.983478424788 475.898855840183,214.116005364825 478.117477618427,218.691606513192 480.33608839763,223.26720766156 480.747140216811,228.840058880406 479.350624414847,235.410149170689 478.133659575801,241.135518594641 475.862885174246,246.074819539577 472.538300041222,250.228057505018 469.213714908198,254.381295470459 465.140718422277,257.748470456406 460.319333750499,260.329576963337 457.246841608952,261.972102285668 453.224488193848,263.333052152715 448.252274674145,264.412421064958 451.780966266555,265.68097898574 454.278400080004,266.94916843864 455.744565115453,268.21698942366 456.737354137313,269.062452725275 458.053043915446,270.870882085879 459.691642287291,273.64226650643 461.330232821697,276.413661926024 462.364842748211,278.550962619166 462.79546340571,280.054157586817L471.882018571351,309.95725095717 435.850259432079,309.95725095717 425.508324840385,278.420799965345C424.217411320934,274.337955863724 422.856908675954,271.686460992493 421.42681058224,270.466293353567 419.4664276683,268.87070094013 417.031432110014,268.072901983652 414.121822738425,268.072901983652L411.306068185583,268.072901983652 402.403274979985,309.95725095717 370.374066941412,309.95725095717 392.309352385794,206.759846595526z M503.954207389664,206.759846595526L589.412347069365,206.759846595526 584.72902896289,228.793125971511 531.229703457941,228.793125971511 527.743399563984,245.194896241814 577.371062558776,245.194896241814 572.897221939492,266.242661524305 523.2695589447,266.242661524305 518.945345101981,286.586488168585 573.993335610993,286.586488168585 569.025726629046,309.95725095717 482.018921945282,309.95725095717 503.954207389664,206.759846595526z M656.941984111389,205C669.941376964314,205 679.601604005187,207.628044915346 685.922691907928,212.884112747956 692.243755474668,218.140191579608 696.053953802997,226.21201946458 697.353285723957,237.099601902394L667.864061884116,243.364655782467C667.54688033629,240.220398364967 667.003202748652,237.92086731332 666.233074286324,236.466057128005 664.968461505935,234.025738348716 663.185256075335,232.148570479995 660.883459163485,230.834548022322 658.581637915633,229.52053656369 655.835141546751,228.863525334853 652.643968887878,228.863519835333 645.416849701237,228.863525334853 639.261450056071,231.769840493596 634.177749123256,237.582459812042 630.351508105635,241.894820906842 627.457097098869,248.667662023492 625.494492935915,257.900988661511 623.063368759884,269.338528659948 623.13335232876,277.178282274656 625.704467978543,281.420238506594 628.275559292325,285.662205737574 632.705380034237,287.783183853543 638.993905868278,287.783183853543 645.09470006674,287.783183853543 650.069589190686,286.070270166911 653.918575578034,282.644431794607 657.767537629381,279.218604421344 661.089251581376,274.244101044977 663.883671099936,267.720932664547L690.034513718112,276.168196323071C686.49148301313,284.005387161175 682.143414048514,290.55200549749 676.990299810507,295.808084329141 671.837163574417,301.064163160793 665.984577536506,305.029680489698 659.432517360771,307.704647314897 652.880457185037,310.379614140097 644.981882040867,311.717097552696 635.736835924425,311.717097552696 624.520739622912,311.717097552696 615.70421470141,310.087402611951 609.287236823919,306.828023729501 602.870258946427,303.568644847051 597.952907773622,297.835394610064 594.53516130742,289.62827301854 591.1174125033,281.421162426057 590.769297088892,270.915791171188 593.490790728193,258.112170252975 597.119238890928,241.041663775893 604.448312155609,227.922007406247 615.478010522237,218.753201144036 626.507707719906,209.584400381345 640.329032249624,205 656.941984111389,205z M764.721719228828,229.145095290617C758.292427665228,229.145095290617 752.646432016528,231.421165387337 747.783689455522,235.973300081258 742.920989721722,240.525440274699 739.372426979458,248.05758370355 737.138001228732,258.569730367811 734.923524935575,268.988022213325 735.268323241236,276.473243732323 738.172378823472,281.025372926723 741.076454065871,285.577513120164 745.860498407669,287.853577717364 752.524421518618,287.853577717364 759.376120261309,287.853577717364 765.152926570948,285.624446029058 769.854889119537,281.166160654365 774.556893326373,276.707886278713 778.134792170415,268.706446758041 780.588671306076,257.161853091391 782.653520822669,247.447499884087 782.202975496092,240.349455615146 779.236993668101,235.867709285526 776.271054667315,231.385968455427 771.432630243877,229.145095290617 764.721719228828,229.145095290617z M769.924311528817,205C787.100414301151,205 799.354405313008,209.610798080278 806.686284564388,218.832394240834 814.018162646809,228.053995900911 815.918506309036,240.971269912072 812.387315551068,257.584216274317 809.823708553981,269.645038941682 805.691777268462,279.535376808539 799.991477698347,287.255229874885 794.291219786478,294.975093940273 787.148220756569,300.982033319988 778.562566263031,305.276059013071 769.976911769493,309.570084706155 759.958731597335,311.717097552696 748.507981750391,311.717097552696 736.869544264034,311.717097552696 727.631348295557,309.863396138423 720.793374184797,306.155982310834 713.955400074037,302.448568483246 709.01937605921,296.582416831173 705.98526048207,288.557516355574 702.95114490493,280.532615879976 702.715899276026,270.489765307535 705.279506273114,258.428942640169 708.860573012922,241.581348232014 716.341535927939,228.464623106853 727.722415847287,219.078772764208 739.103294597676,209.692927921083 753.17057193941,205 769.924311528817,205z M840.013402368145,206.759846595526L881.950546739529,206.759846595526 884.776828482843,269.551173123894 914.181037232466,206.759846595526 956.092883809039,206.759846595526 934.157598364657,309.95725095717 908.041474887051,309.95725095717 924.769748506983,231.256911205248 887.918728971334,309.95725095717 864.279589576931,309.95725095717 860.9225140213,231.256911205248 844.194240401368,309.95725095717 818.078116923762,309.95725095717 840.013402368145,206.759846595526z M1003.86656079578,227.737218014196L998.973765202113,250.756011483676 1006.78748408625,250.756011483676C1012.93520019456,250.756011483676 1017.4796533945,249.688373048897 1020.42075686269,247.553090679819 1023.36185916192,245.417813810261 1025.18654979346,242.684183598704 1025.89478359218,239.352205544667 1026.58306676438,236.1140878089 1026.08725327978,233.368727119879 1024.40729914222,231.116123477606 1022.72734500466,228.863519835333 1018.90739769759,227.737218014196 1012.94736922869,227.737218014196L1003.86656079578,227.737218014196z M976.296230698832,206.759846595526L1029.30281015607,206.759846595526C1040.84740382273,206.759846595526 1048.91055766809,209.505207284547 1053.49231568832,214.995928662588 1058.07402971239,220.486650040629 1059.28761792933,228.300368924764 1057.13299234679,238.437085314994 1054.91851605364,248.855377160508 1050.05530528536,256.997604408822 1042.54336237989,262.863756060895 1035.03141830546,268.729913212488 1024.84611129296,271.662989038525 1011.98752816576,271.662989038525L994.529849938142,271.662989038525 986.390153293023,309.95725095717 954.36094525445,309.95725095717 976.296230698832,206.759846595526z M1081.81645658191,206.759846595526L1113.77527075666,206.759846595526 1091.83998531228,309.95725095717 1059.88117113753,309.95725095717 1081.81645658191,206.759846595526z M1136.65424441413,206.759846595526L1168.54266472506,206.759846595526 1152.00890591466,284.545066117775 1201.77736763614,284.545066117775 1196.37584100215,309.95725095717 1114.71895896975,309.95725095717 1136.65424441413,206.759846595526z M1232.74301243014,206.759846595526L1318.20116310888,206.759846595526 1313.5178450024,228.793125971511 1260.01850849841,228.793125971511 1256.53220460446,245.194896241814 1306.15987859829,245.194896241814 1301.68603797901,266.242661524305 1252.05836398517,266.242661524305 1247.73415014245,286.586488168585 1302.78215165051,286.586488168585 1297.81454266856,309.95725095717 1210.80772698575,309.95725095717 1232.74301243014,206.759846595526z M1364.26884142019,230.130609384111L1352.28373661736,286.516094304764 1360.0974555015,286.516094304764C1366.76137861245,286.516094304764 1371.6583695559,285.776958734643 1374.78834033953,284.298687594401 1377.91831112316,282.82041645416 1380.67264480912,280.239315446749 1383.05125808092,276.555362574087 1385.4298690148,272.871420700466 1387.49702967973,266.899672753141 1389.2526509144,258.640124231632 1391.57685273107,247.705614384445 1391.38454396395,240.220398364967 1388.67581143642,236.184481672721 1385.96707773993,232.148570479995 1380.48292288212,230.130614883632 1372.22334803197,230.130609384111L1364.26884142019,230.130609384111z M1337.34803009121,206.759846595526L1384.72310044277,206.759846595526C1394.06199037892,206.759846595526 1401.33659658214,208.026936144305 1406.54674306779,210.561115241862 1411.75688955343,213.09529433942 1415.73540290312,216.732314303187 1418.48228545479,221.472164134123 1421.2291668375,226.21201946458 1422.7789567684,231.726207297069 1423.13174440876,238.014722132068 1423.48453088017,244.303242466587 1422.91279081753,250.967193075139 1421.41652305189,258.006579457243 1419.07236943973,269.03495512222 1415.99901753555,277.587809576476 1412.19655533168,283.665142820012 1408.3940931278,289.742476063548 1403.82729495956,294.834306212631 1398.4961655028,298.940611269178 1393.16503604604,303.046916325725 1387.7972010986,305.780552036802 1382.39266533633,307.141496404329 1375.00117334918,309.01866977257 1368.46622401584,309.95725095717 1362.78781499839,309.95725095717L1315.41274464683,309.95725095717 1337.34803009121,206.759846595526z'><GeometryDrawing.Brush><LinearGradientBrush StartPoint='0,0' EndPoint='0,1'><GradientStop Color='#FFB38F' Offset='0'/><GradientStop Color='#FF5A3C' Offset='0.4'/><GradientStop Color='#E1263A' Offset='0.52'/><GradientStop Color='#7E0F2B' Offset='1'/></LinearGradientBrush></GeometryDrawing.Brush></GeometryDrawing>
  <GeometryDrawing Brush='#FFFFFF' Geometry='M1385,-1Q1395.12,34.88 1431,45 1395.12,55.12 1385,91 1374.88,55.12 1339,45 1374.88,34.88 1385,-1z'><GeometryDrawing.Pen><Pen Brush='#12152A' Thickness='10' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>
  <GeometryDrawing Brush='#FFFFFF' Geometry='M1455,96Q1460.28,114.72 1479,120 1460.28,125.28 1455,144 1449.72,125.28 1431,120 1449.72,114.72 1455,96z'><GeometryDrawing.Pen><Pen Brush='#12152A' Thickness='10' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>
  <GeometryDrawing Brush='#FFFFFF' Geometry='M395,322Q401.6,345.4 425,352 401.6,358.6 395,382 388.4,358.6 365,352 388.4,345.4 395,322z'><GeometryDrawing.Pen><Pen Brush='#12152A' Thickness='10' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>
  <GeometryDrawing Brush='#FFFFFF' Geometry='M1420,321Q1427.48,347.52 1454,355 1427.48,362.48 1420,389 1412.52,362.48 1386,355 1412.52,347.52 1420,321z'><GeometryDrawing.Pen><Pen Brush='#12152A' Thickness='10' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>
  <GeometryDrawing Brush='#12152A' Geometry='M200,200 m-212,0 a212,212 0 1 0 424,0 a212,212 0 1 0 -424,0'/>
  <GeometryDrawing Brush='#FFFFFF' Geometry='M200,200 m-200,0 a200,200 0 1 0 400,0 a200,200 0 1 0 -400,0'/>
  <DrawingGroup Transform='0.69,0,0,0.69,23.4,23.4'>

  <GeometryDrawing Brush='#00000000' Geometry='M0,0 L512,512'/>
  <GeometryDrawing Brush='#232A57' Geometry='F1 M39.0,256.0 A217.0,217.0 0 1 1 473.0,256.0 A217.0,217.0 0 1 1 39.0,256.0 Z'/>
  <GeometryDrawing Brush='#FF4D3D' Geometry='F1 M57.7,183.8 A211.0,211.0 0 0 1 438.7,150.5 L464.7,135.5 L437.0,237.0 L362.5,194.5 L388.5,179.5 A153.0,153.0 0 0 0 112.2,203.7 Z'/>
  <GeometryDrawing Brush='#FFC940' Geometry='F1 M454.3,328.2 A211.0,211.0 0 0 1 73.3,361.5 L47.3,376.5 L75.0,275.0 L149.5,317.5 L123.5,332.5 A153.0,153.0 0 0 0 399.8,308.3 Z'/>
  <GeometryDrawing Brush='#C9322A' Geometry='F1 M125.4,155.8 L138.6,140.6 L113.8,116.2 L97.8,134.6 Z M172.5,114.2 L190.4,105.1 L176.5,73.1 L154.8,84.2 Z M231.7,93.2 L251.7,91.5 L250.8,56.7 L226.5,58.8 Z M294.4,95.9 L313.6,101.8 L325.8,69.2 L302.5,62.1 Z M351.6,122.0 L367.2,134.6 L390.7,109.0 L371.8,93.7 Z'/>
  <GeometryDrawing Brush='#D99A22' Geometry='F1 M386.6,356.2 L373.4,371.4 L398.2,395.8 L414.2,377.4 Z M339.5,397.8 L321.6,406.9 L335.5,438.9 L357.2,427.8 Z M280.3,418.8 L260.3,420.5 L261.2,455.3 L285.5,453.2 Z M217.6,416.1 L198.4,410.2 L186.2,442.8 L209.5,449.9 Z M160.4,390.0 L144.8,377.4 L121.3,403.0 L140.2,418.3 Z'/>
  <GeometryDrawing Brush='#F3F4FF' Geometry='F1 M134.0,256.0 A122.0,122.0 0 1 1 378.0,256.0 A122.0,122.0 0 1 1 134.0,256.0 Z'/>
  <GeometryDrawing Brush='#232A57' Geometry='F1 M164.0,256.0 A92.0,92.0 0 1 1 348.0,256.0 A92.0,92.0 0 1 1 164.0,256.0 Z'/>
  <GeometryDrawing Brush='#F3F4FF' Geometry='F1 M245.4,223.7 L266.6,223.7 L277.6,162.5 L234.4,162.5 Z M283.4,235.9 L290.0,256.1 L351.6,247.6 L338.3,206.6 Z M283.6,275.9 L266.4,288.4 L293.5,344.4 L328.5,319.0 Z M245.6,288.4 L228.4,275.9 L183.5,319.0 L218.5,344.4 Z M222.0,256.1 L228.6,235.9 L173.7,206.6 L160.4,247.6 Z'/>
  <GeometryDrawing Brush='#FF4D3D' Geometry='F1 M208.0,256.0 A48.0,48.0 0 1 1 304.0,256.0 A48.0,48.0 0 1 1 208.0,256.0 Z'/>
  <GeometryDrawing Brush='#F3F4FF' Geometry='F1 M221.0,256.0 A35.0,35.0 0 1 1 291.0,256.0 A35.0,35.0 0 1 1 221.0,256.0 Z'/>
  <GeometryDrawing Brush='#12152A' Geometry='F1 M256.0,256.0 L256.0,221.0 A35.0,35.0 0 0 1 291.0,256.0 Z M256.0,256.0 L256.0,291.0 A35.0,35.0 0 0 1 221.0,256.0 Z'/>


  </DrawingGroup>
  </DrawingGroup>
</DrawingGroup>
                </DrawingImage.Drawing></DrawingImage></Image.Source>
              </Image>
              <StackPanel Orientation='Horizontal' Margin='0,18,0,0'>
                <Button x:Name='Jugar' Style='{StaticResource Grande}' Content='JUGAR'/>
                <Button x:Name='Repeticiones' Style='{StaticResource Secundario}' Margin='14,0,0,0'>
                  <TextBlock Text='Repeticiones'/>
                </Button>
                <Button x:Name='Actualizar' Style='{StaticResource Secundario}' Margin='14,0,0,0' Visibility='Collapsed'
                        BorderBrush='{StaticResource Amarillo}'>
                  <TextBlock Text='Actualizar' Foreground='{StaticResource Amarillo}'/>
                </Button>
              </StackPanel>
            </StackPanel>
          </Grid>
          <DockPanel Grid.Row='1' Margin='36,4,36,24'>
            <TextBlock x:Name='Estado' DockPanel.Dock='Right' Text='Listo para jugar' Foreground='#C9CFEA' FontSize='13'
                       VerticalAlignment='Center'/>
            <StackPanel Orientation='Horizontal'>
              <Border Style='{StaticResource Chip}'><StackPanel>
                <TextBlock Text='JUEGO' Foreground='{StaticResource Suave}' FontSize='10' FontWeight='Bold'/>
                <TextBlock x:Name='ChipJuego' Foreground='{StaticResource Texto}' FontSize='14' FontWeight='SemiBold'/>
              </StackPanel></Border>
              <Border Style='{StaticResource Chip}'><StackPanel>
                <TextBlock Text='GRÁFICOS' Foreground='{StaticResource Suave}' FontSize='10' FontWeight='Bold'/>
                <TextBlock x:Name='ChipEscala' Foreground='{StaticResource Texto}' FontSize='14' FontWeight='SemiBold'/>
              </StackPanel></Border>
              <Border Style='{StaticResource Chip}'><StackPanel>
                <TextBlock Text='MODO' Foreground='{StaticResource Suave}' FontSize='10' FontWeight='Bold'/>
                <TextBlock x:Name='ChipPantalla' Foreground='{StaticResource Texto}' FontSize='14' FontWeight='SemiBold'/>
              </StackPanel></Border>
              <Border Style='{StaticResource Chip}'><StackPanel>
                <TextBlock Text='JUGADOR' Foreground='{StaticResource Suave}' FontSize='10' FontWeight='Bold'/>
                <TextBlock x:Name='ChipNombre' Foreground='{StaticResource Texto}' FontSize='14' FontWeight='SemiBold'/>
              </StackPanel></Border>
            </StackPanel>
          </DockPanel>
        </Grid>

        <!-- Ajustes -->
        <ScrollViewer x:Name='PagAjustes' Visibility='Collapsed' VerticalScrollBarVisibility='Hidden'>
        <StackPanel Margin='36,34,36,24'>
          <TextBlock Text='Ajustes' Style='{StaticResource H1}'/>
          <Border Style='{StaticResource Tarjeta}'>
            <DockPanel>
              <Button x:Name='ElegirRom' DockPanel.Dock='Right' Style='{StaticResource Secundario}' VerticalAlignment='Center'>
                <TextBlock Text='Elegir ROM…'/>
              </Button>
              <StackPanel Margin='0,0,20,0'>
                <TextBlock Text='Tu juego' Style='{StaticResource Etiqueta}'/>
                <TextBlock x:Name='RomEstado' FontSize='13' FontWeight='SemiBold' Margin='0,4,0,0'/>
                <TextBlock Text='Una copia de tu cartucho de Mario Kart DS europeo (.nds). Se comprueba y se copia a la carpeta del juego; tu archivo no se toca.'
                           Style='{StaticResource Nota}'/>
              </StackPanel>
            </DockPanel>
          </Border>
          <Border Style='{StaticResource Tarjeta}'>
            <StackPanel>
              <TextBlock Text='Resolución del 3D' Style='{StaticResource Etiqueta}'/>
              <TextBlock Text='Pinta los circuitos, los karts y los personajes a más resolución. Los menús y el marcador se ven como en la DS, sin difuminar.'
                         Style='{StaticResource Nota}'/>
              <UniformGrid Columns='4' Margin='-4,14,-4,0'>
                <ToggleButton x:Name='Esc1' Style='{StaticResource Seg}' Content='Original'/>
                <ToggleButton x:Name='Esc2' Style='{StaticResource Seg}' Content='2×'/>
                <ToggleButton x:Name='Esc3' Style='{StaticResource Seg}' Content='3×'/>
                <ToggleButton x:Name='Esc4' Style='{StaticResource Seg}' Content='4×'/>
              </UniformGrid>
              <TextBlock Text='Imágenes por segundo' Style='{StaticResource Etiqueta}' Margin='0,16,0,0'/>
              <TextBlock Text='Por encima de 60 se inventan imágenes intermedias del 3D: va más fluido, pero pide más al PC y hace falta un monitor de esa frecuencia.'
                         Style='{StaticResource Nota}'/>
              <UniformGrid Columns='4' Margin='-4,12,-4,0'>
                <ToggleButton x:Name='Fps60' Style='{StaticResource Seg}' Content='60'/>
                <ToggleButton x:Name='Fps120' Style='{StaticResource Seg}' Content='120'/>
                <ToggleButton x:Name='Fps180' Style='{StaticResource Seg}' Content='180'/>
                <ToggleButton x:Name='Fps240' Style='{StaticResource Seg}' Content='240'/>
              </UniformGrid>
            </StackPanel>
          </Border>
          <Border Style='{StaticResource Tarjeta}'>
            <StackPanel>
              <TextBlock Text='Idioma' Style='{StaticResource Etiqueta}'/>
              <DockPanel Margin='0,12,0,0'>
                <TextBlock Text='Launcher' Width='80' VerticalAlignment='Center' Foreground='{StaticResource Suave}' FontSize='13'/>
                <UniformGrid Columns='5' Margin='-4,0,-4,0'>
                  <ToggleButton x:Name='Lan_es' Style='{StaticResource Seg}' Content='Español'/>
                  <ToggleButton x:Name='Lan_en' Style='{StaticResource Seg}' Content='English'/>
                  <ToggleButton x:Name='Lan_fr' Style='{StaticResource Seg}' Content='Français'/>
                  <ToggleButton x:Name='Lan_de' Style='{StaticResource Seg}' Content='Deutsch'/>
                  <ToggleButton x:Name='Lan_it' Style='{StaticResource Seg}' Content='Italiano'/>
                </UniformGrid>
              </DockPanel>
              <DockPanel Margin='0,8,0,0'>
              <TextBlock Text='Juego' Width='80' VerticalAlignment='Center' Foreground='{StaticResource Suave}' FontSize='13'/>
              <UniformGrid Columns='5' Margin='-4,0,-4,0'>
                <ToggleButton x:Name='Idi5' Style='{StaticResource Seg}' Content='Español'/>
                <ToggleButton x:Name='Idi1' Style='{StaticResource Seg}' Content='English'/>
                <ToggleButton x:Name='Idi2' Style='{StaticResource Seg}' Content='Français'/>
                <ToggleButton x:Name='Idi3' Style='{StaticResource Seg}' Content='Deutsch'/>
                <ToggleButton x:Name='Idi4' Style='{StaticResource Seg}' Content='Italiano'/>
              </UniformGrid>
              </DockPanel>
            </StackPanel>
          </Border>
          <Border Style='{StaticResource Tarjeta}'>
            <DockPanel>
              <ToggleButton x:Name='PantallaCompleta' DockPanel.Dock='Right' Style='{StaticResource Interruptor}'/>
              <StackPanel Margin='0,0,20,0'>
                <TextBlock Text='Empezar en pantalla completa' Style='{StaticResource Etiqueta}'/>
                <TextBlock Text='Las dos pantallas lado a lado: la principal a la izquierda y la táctil a la derecha. F11 cambia mientras juegas.'
                           Style='{StaticResource Nota}'/>
              </StackPanel>
            </DockPanel>
          </Border>
          <Border Style='{StaticResource Tarjeta}'>
            <StackPanel>
              <TextBlock Text='Micrófono del PC' Style='{StaticResource Etiqueta}'/>
              <TextBlock Text='En la Batalla de globos, sopla al micrófono para inflar el globo. Solo se usa mientras el juego lo pide.'
                         Style='{StaticResource Nota}'/>
              <ComboBox x:Name='Micro' Style='{StaticResource Desplegable}' Margin='0,10,0,0'/>
            </StackPanel>
          </Border>
          <Border Style='{StaticResource Tarjeta}'>
            <StackPanel>
              <TextBlock Text='Tu nombre' Style='{StaticResource Etiqueta}'/>
              <TextBox x:Name='Nombre' MaxLength='10' Margin='0,10,0,0' Padding='12,9' FontSize='15'
                       Background='{StaticResource Panel2}' Foreground='{StaticResource Texto}' BorderBrush='{StaticResource Borde}'
                       CaretBrush='{StaticResource Amarillo}'/>
              <TextBlock Text='Hasta 10 letras. Es el nombre que sale en las carreras y se guarda en tu partida. Es obligatorio.'
                         Style='{StaticResource Nota}'/>
            </StackPanel>
          </Border>
        </StackPanel>
        </ScrollViewer>

        <!-- Controles -->
        <StackPanel x:Name='PagControles' Margin='36,34,36,24' Visibility='Collapsed'>
          <TextBlock Text='Controles' Style='{StaticResource H1}'/>
          <Border Style='{StaticResource Tarjeta}' Padding='22,10'>
            <Grid>
              <Grid.ColumnDefinitions>
                <ColumnDefinition Width='150'/>
                <ColumnDefinition Width='*'/>
                <ColumnDefinition Width='*'/>
              </Grid.ColumnDefinitions>
              <Grid.RowDefinitions>
                <RowDefinition/><RowDefinition/><RowDefinition/><RowDefinition/><RowDefinition/>
                <RowDefinition/><RowDefinition/><RowDefinition/>
              </Grid.RowDefinitions>
              <TextBlock Grid.Row='0' Grid.Column='0' Text='EN LA DS' Foreground='{StaticResource Suave}' FontSize='11' FontWeight='Bold' Margin='0,8'/>
              <TextBlock Grid.Row='0' Grid.Column='1' Text='TECLADO' Foreground='{StaticResource Suave}' FontSize='11' FontWeight='Bold' Margin='0,8'/>
              <TextBlock Grid.Row='0' Grid.Column='2' Text='MANDO (PS5 / PS4 / XBOX)' Foreground='{StaticResource Suave}' FontSize='11' FontWeight='Bold' Margin='0,8'/>

              <TextBlock Grid.Row='1' Text='Cruceta' Style='{StaticResource Celda}'/>
              <Border Grid.Row='1' Grid.Column='1' Style='{StaticResource Tecla}'><TextBlock Text='Flechas' Foreground='{StaticResource Texto}'/></Border>
              <TextBlock Grid.Row='1' Grid.Column='2' Text='Cruceta o stick izquierdo' Style='{StaticResource Celda}'/>

              <TextBlock Grid.Row='2' Text='A  ·  B' Style='{StaticResource Celda}'/>
              <StackPanel Grid.Row='2' Grid.Column='1' Orientation='Horizontal'>
                <Border Style='{StaticResource Tecla}'><TextBlock Text='X' Foreground='{StaticResource Texto}'/></Border>
                <Border Style='{StaticResource Tecla}'><TextBlock Text='Z' Foreground='{StaticResource Texto}'/></Border>
              </StackPanel>
              <TextBlock Grid.Row='2' Grid.Column='2' Text='✕ y ○  ·  A y B' Style='{StaticResource Celda}'/>

              <TextBlock Grid.Row='3' Text='X  ·  Y' Style='{StaticResource Celda}'/>
              <StackPanel Grid.Row='3' Grid.Column='1' Orientation='Horizontal'>
                <Border Style='{StaticResource Tecla}'><TextBlock Text='S' Foreground='{StaticResource Texto}'/></Border>
                <Border Style='{StaticResource Tecla}'><TextBlock Text='A' Foreground='{StaticResource Texto}'/></Border>
              </StackPanel>
              <TextBlock Grid.Row='3' Grid.Column='2' Text='△ y □  ·  X e Y' Style='{StaticResource Celda}'/>

              <TextBlock Grid.Row='4' Text='L  ·  R' Style='{StaticResource Celda}'/>
              <StackPanel Grid.Row='4' Grid.Column='1' Orientation='Horizontal'>
                <Border Style='{StaticResource Tecla}'><TextBlock Text='Q' Foreground='{StaticResource Texto}'/></Border>
                <Border Style='{StaticResource Tecla}'><TextBlock Text='W' Foreground='{StaticResource Texto}'/></Border>
              </StackPanel>
              <TextBlock Grid.Row='4' Grid.Column='2' Text='L1 / L2  ·  R1 / R2' Style='{StaticResource Celda}'/>

              <TextBlock Grid.Row='5' Text='Start  ·  Select' Style='{StaticResource Celda}'/>
              <StackPanel Grid.Row='5' Grid.Column='1' Orientation='Horizontal'>
                <Border Style='{StaticResource Tecla}'><TextBlock Text='Enter' Foreground='{StaticResource Texto}'/></Border>
                <Border Style='{StaticResource Tecla}'><TextBlock Text='Retroceso' Foreground='{StaticResource Texto}'/></Border>
              </StackPanel>
              <TextBlock Grid.Row='5' Grid.Column='2' Text='Options · Create  (Start · Back)' Style='{StaticResource Celda}'/>

              <TextBlock Grid.Row='6' Text='Pantalla táctil' Style='{StaticResource Celda}'/>
              <TextBlock Grid.Row='6' Grid.Column='1' Grid.ColumnSpan='2' Text='Ratón sobre la pantalla de abajo' Style='{StaticResource Celda}'/>

              <TextBlock Grid.Row='7' Text='Extras' Style='{StaticResource Celda}'/>
              <StackPanel Grid.Row='7' Grid.Column='1' Grid.ColumnSpan='2' Orientation='Horizontal'>
                <Border Style='{StaticResource Tecla}'><TextBlock Text='F11' Foreground='{StaticResource Texto}'/></Border>
                <TextBlock Text='pantalla completa' Style='{StaticResource Celda}' Margin='0,8,18,8'/>
                <Border Style='{StaticResource Tecla}'><TextBlock Text='F10' Foreground='{StaticResource Texto}'/></Border>
                <TextBlock Text='guardar los últimos 30 s en vídeo' Style='{StaticResource Celda}'/>
              </StackPanel>
            </Grid>
          </Border>
          <TextBlock Text='En la Batalla de globos, sopla al micrófono o pulsa Select para inflar un globo.'
                     Style='{StaticResource Nota}'/>
        </StackPanel>

        <!-- Acerca de -->
        <StackPanel x:Name='PagAcerca' Margin='36,34,36,24' Visibility='Collapsed'>
          <TextBlock Text='Acerca de' Style='{StaticResource H1}'/>
          <Border Style='{StaticResource Tarjeta}'>
            <StackPanel>
              <TextBlock Text='Qué es' Style='{StaticResource Etiqueta}'/>
              <TextBlock Style='{StaticResource Nota}' LineHeight='20'>
                Una recompilación estática: el código del juego de DS se traduce a C y se compila para Windows,
                sin emulador. Encima van mejoras propias: 3D a más resolución, pantalla completa con las dos
                pantallas lado a lado, mandos de PS5, PS4 y Xbox y repeticiones instantáneas.
              </TextBlock>
            </StackPanel>
          </Border>
          <Border Style='{StaticResource Tarjeta}'>
            <StackPanel>
              <TextBlock Text='Tu juego' Style='{StaticResource Etiqueta}'/>
              <TextBlock Style='{StaticResource Nota}' LineHeight='20'>
                El port no incluye el juego: necesita una copia sacada de tu propio cartucho. Proyecto de
                aficionado sin relación con Nintendo.
              </TextBlock>
            </StackPanel>
          </Border>
          <Border Style='{StaticResource Tarjeta}'>
            <StackPanel>
              <TextBlock Text='Hecho por' Style='{StaticResource Etiqueta}'/>
              <TextBlock Text='alon tube · alontube.pages.dev' Style='{StaticResource Nota}'/>
            </StackPanel>
          </Border>
          <Border Style='{StaticResource Tarjeta}'>
            <DockPanel>
              <Button x:Name='BuscarActualizacion' DockPanel.Dock='Right' Style='{StaticResource Secundario}' VerticalAlignment='Center'>
                <TextBlock Text='Buscar actualizaciones'/>
              </Button>
              <StackPanel Margin='0,0,20,0'>
                <TextBlock Text='Actualizaciones' Style='{StaticResource Etiqueta}'/>
                <TextBlock Text='El launcher mira al abrirse si hay una versión nueva y la instala con un botón. Tu ROM, tus ajustes y tu partida no se tocan.'
                           Style='{StaticResource Nota}'/>
              </StackPanel>
            </DockPanel>
          </Border>
        </StackPanel>
      </Grid>
      <!-- ===== preparando el juego / actualizando ===== -->
      <Grid x:Name='Preparando' Grid.ColumnSpan='2' Visibility='Collapsed'>
        <Border CornerRadius='18' Background='#F70B0D1F'/>
        <StackPanel VerticalAlignment='Center' HorizontalAlignment='Center' Width='560'>
          <TextBlock x:Name='PrepTitulo' Text='Preparando el juego' Foreground='{StaticResource Texto}' FontSize='30' FontWeight='Black' HorizontalAlignment='Center'/>
          <TextBlock x:Name='PrepNota' Foreground='{StaticResource Suave}' FontSize='14' TextWrapping='Wrap' TextAlignment='Center' Margin='0,10,0,26'/>
          <ProgressBar x:Name='PrepBarra' Height='10' Minimum='0' Maximum='100' Background='{StaticResource Panel2}'
                       Foreground='{StaticResource Amarillo}' BorderThickness='0'/>
          <TextBlock x:Name='PrepPaso' Foreground='{StaticResource Texto}' FontSize='14' HorizontalAlignment='Center' Margin='0,14,0,24'/>
          <Button x:Name='PrepCancelar' Style='{StaticResource Secundario}' HorizontalAlignment='Center'><TextBlock Text='Cancelar'/></Button>
        </StackPanel>
      </Grid>
      <!-- ===== primera vez: elegir idioma ===== -->
      <Grid x:Name='Bienvenida' Grid.ColumnSpan='2' Visibility='Collapsed'>
        <Border CornerRadius='18' Background='#F70B0D1F'/>
        <StackPanel VerticalAlignment='Center' HorizontalAlignment='Center' Width='520'>
          <TextBlock Text='DS Kart Recompiled' Foreground='{StaticResource Texto}' FontSize='36' FontWeight='Black' HorizontalAlignment='Center'/>
          <TextBlock Text='Elige tu idioma · Choose your language · Choisis ta langue · Wähle deine Sprache · Scegli la lingua'
                     Foreground='{StaticResource Suave}' FontSize='14' TextWrapping='Wrap' TextAlignment='Center' Margin='0,10,0,26'/>
          <Button x:Name='Bien_es' Style='{StaticResource Secundario}' Margin='0,0,0,10'><TextBlock Text='Español' HorizontalAlignment='Center' FontSize='17'/></Button>
          <Button x:Name='Bien_en' Style='{StaticResource Secundario}' Margin='0,0,0,10'><TextBlock Text='English' HorizontalAlignment='Center' FontSize='17'/></Button>
          <Button x:Name='Bien_fr' Style='{StaticResource Secundario}' Margin='0,0,0,10'><TextBlock Text='Français' HorizontalAlignment='Center' FontSize='17'/></Button>
          <Button x:Name='Bien_de' Style='{StaticResource Secundario}' Margin='0,0,0,10'><TextBlock Text='Deutsch' HorizontalAlignment='Center' FontSize='17'/></Button>
          <Button x:Name='Bien_it' Style='{StaticResource Secundario}'><TextBlock Text='Italiano' HorizontalAlignment='Center' FontSize='17'/></Button>
        </StackPanel>
      </Grid>
    </Grid>
  </Border>
</Window>";
    }
}
