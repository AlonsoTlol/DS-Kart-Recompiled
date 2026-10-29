// Preparar el juego en el PC del usuario y actualizar el launcher.
//
// El port no reparte código de Nintendo: lo que se descarga es nuestro código (tools, runtime, launcher). La
// primera vez que se le da a JUGAR, el launcher saca el código de la ROM del usuario (tools/nds_rom.py), lo
// traduce a C (tools/traducir_mkds.py) y lo compila con clang. Python y el compilador (llvm-mingw) se bajan de
// sus páginas oficiales a %LOCALAPPDATA%\DSKartRecompiled\herramientas y se comprueban con su SHA-256.
//
// build\preparado.txt guarda huellas de lo que se usó. Tras una actualización solo se rehace lo que cambió:
// si cambian tools/, faltan*.txt o los encabezados del runtime, todo; si cambian los .c del runtime, solo
// esos y el enlace.
//
// Actualizar: GitHub Releases de REPO. La versión es una etiqueta "vX.Y.Z"; el zip de la versión trae la
// carpeta del juego entera menos lo del usuario (rom\, ajustes.ini, partida.sav, repeticiones\), que no se
// toca nunca.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows;
using System.Windows.Controls;

partial class Launcher {
    const string VERSION_NUM = "0.1.0";
    const string REPO = "AlonsoTlol/DS-Kart-Recompiled";

    const string URL_PYTHON = "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip";
    const string SHA_PYTHON = "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3";
    const string URL_LLVM = "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip";
    const string SHA_LLVM = "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd";

    // lo que es del usuario: una actualización no lo pisa nunca
    static readonly string[] DEL_USUARIO = { "rom", "ajustes.ini", "partida.sav", "repeticiones", "build", "portada.mp4" };

    static readonly string[] RUNTIME = { "cpu", "memoria", "bios", "hilos", "ds", "video", "gpu3d", "ventana", "sonido",
                                         "repeticion", "microfono", "wifi", "main_mkds" };
    const string LIBRERIAS = "-lwinmm -lgdi32 -lopengl32 -luser32 -lxinput -lhid -lsetupapi -lws2_32 -liphlpapi -lole32";

    static volatile bool cancelado;
    static readonly List<Process> enMarcha = new List<Process>();

    static string Ruta(params string[] partes) {
        string r = carpeta;
        foreach (string p in partes) r = Path.Combine(r, p);
        return r;
    }

    // ---- ¿hace falta preparar? -------------------------------------------------------------------------
    static string Huella(IEnumerable<string> archivos) {
        using (var sha = SHA256.Create()) {
            var todo = new MemoryStream();
            foreach (string a in archivos) {
                if (!File.Exists(a)) continue;
                byte[] n = Encoding.UTF8.GetBytes(Path.GetFileName(a));
                todo.Write(n, 0, n.Length);
                byte[] d = File.ReadAllBytes(a);
                todo.Write(d, 0, d.Length);
            }
            return BitConverter.ToString(sha.ComputeHash(todo.ToArray())).Replace("-", "").Substring(0, 16);
        }
    }

    static List<string> Lista(string dir, string patron) {
        var l = new List<string>();
        if (Directory.Exists(dir)) l.AddRange(Directory.GetFiles(dir, patron));
        l.Sort(StringComparer.Ordinal);
        return l;
    }

    // la traducción depende de las herramientas, de faltan*.txt y de los encabezados que usa el código traducido
    static string HuellaTraduccion() {
        var l = Lista(Ruta("tools"), "*.py");
        l.Add(Ruta("faltan.txt"));
        l.Add(Ruta("faltan7.txt"));
        l.AddRange(Lista(Ruta("runtime"), "*.h"));
        return Huella(l);
    }

    static string HuellaRuntime(string nombre) { return Huella(new[] { Ruta("runtime", nombre + ".c") }); }

    static Dictionary<string, string> LeePreparado() {
        var d = new Dictionary<string, string>();
        string r = Ruta("build", "preparado.txt");
        if (!File.Exists(r)) return d;
        foreach (string l in File.ReadAllLines(r)) {
            int i = l.IndexOf('=');
            if (i > 0) d[l.Substring(0, i)] = l.Substring(i + 1);
        }
        return d;
    }

    // Sin build\mkds.exe hay que prepararlo. Con él y sin preparado.txt es una copia hecha a mano (la de
    // desarrollo): se deja como está.
    static bool HayQuePreparar() {
        if (!File.Exists(Ruta("build", "mkds.exe"))) return true;
        var p = LeePreparado();
        if (p.Count == 0) return false;
        string h;
        if (!p.TryGetValue("traduccion", out h) || h != HuellaTraduccion()) return true;
        foreach (string n in RUNTIME)
            if (!p.TryGetValue(n, out h) || h != HuellaRuntime(n)) return true;
        return false;
    }

    // ---- la ventana de "preparando" --------------------------------------------------------------------
    static void Progreso(string paso, double fraccion) {
        ventana.Dispatcher.BeginInvoke(new Action(delegate {
            Busca<TextBlock>("PrepPaso").Text = paso;
            var b = Busca<ProgressBar>("PrepBarra");
            b.IsIndeterminate = fraccion < 0;
            if (fraccion >= 0) b.Value = fraccion * 100;
        }));
    }

    static void EnseñaPreparando(bool si, string titulo) {
        Busca<FrameworkElement>("Preparando").Visibility = si ? Visibility.Visible : Visibility.Collapsed;
        if (titulo != null) Busca<TextBlock>("PrepTitulo").Text = titulo;
        Busca<Button>("PrepCancelar").IsEnabled = true;
    }

    // Prepara el juego en segundo plano y, si sale bien, lo arranca
    static void PreparaYJuega() {
        cancelado = false;
        EnseñaPreparando(true, T("Preparando el juego"));
        Busca<TextBlock>("PrepNota").Text = T("Solo la primera vez (y tras actualizar). Se traduce el código de tu ROM y se compila para tu PC: tarda unos minutos.");
        var hilo = new Thread(delegate() {
            string error = null;
            try { Prepara_(); }
            catch (Exception ex) { error = ex.Message; }
            ventana.Dispatcher.BeginInvoke(new Action(delegate {
                EnseñaPreparando(false, null);
                if (cancelado) Estado(T("Preparación cancelada"), true);
                else if (error != null) {
                    Estado(T("No se pudo preparar el juego"), true);
                    MessageBox.Show(T("No se pudo preparar el juego:") + "\n\n" + error + "\n\n" +
                                    T("Los detalles están en build\\preparar.log."), TITULO, MessageBoxButton.OK, MessageBoxImage.Error);
                } else Juega();
            }));
        });
        hilo.IsBackground = true;
        hilo.Start();
    }

    static StreamWriter log;

    static void Prepara_() {
        Directory.CreateDirectory(Ruta("build"));
        using (log = new StreamWriter(Ruta("build", "preparar.log"), false, new UTF8Encoding(false))) {
            log.AutoFlush = true;
            log.WriteLine("DS Kart Recompiled " + VERSION_NUM + " · " + DateTime.Now);
            Herramientas();
            var antes = LeePreparado();
            var ahora = new Dictionary<string, string>();
            string huella = HuellaTraduccion();
            ahora["traduccion"] = huella;
            string h;
            bool traducir = !antes.TryGetValue("traduccion", out h) || h != huella || !Directory.Exists(Ruta("build", "obj", "mkds"));
            if (traducir) {
                // lo de antes ya no vale: se borra para que no se mezcle
                if (Directory.Exists(Ruta("build", "obj"))) Directory.Delete(Ruta("build", "obj"), true);
                File.Delete(Ruta("build", "preparado.txt"));
                Progreso(T("Leyendo tu ROM…"), -1);
                Python("tools\\nds_rom.py rom\\mkds.nds extraido");
                Progreso(T("Traduciendo el código del juego a C…"), -1);
                Python("tools\\traducir_mkds.py");
            }
            // trabajos de compilación: el código traducido (si toca) y los .c del runtime que hayan cambiado
            var trabajos = new List<string[]>();  // { fuente, objeto, opciones }
            if (traducir) {
                foreach (string d in Directory.GetDirectories(Ruta("generado"))) {
                    string nombre = Path.GetFileName(d);
                    Directory.CreateDirectory(Ruta("build", "obj", nombre));
                    foreach (string c in Lista(d, "*.c"))
                        trabajos.Add(new[] { "generado\\" + nombre + "\\" + Path.GetFileName(c),
                                             "build\\obj\\" + nombre + "\\" + Path.GetFileNameWithoutExtension(c) + ".o",
                                             (nombre == "mkds7" ? "-DCPU7 " : "") + "-Iruntime -Igenerado\\" + nombre });
                }
            }
            Directory.CreateDirectory(Ruta("build", "obj", "runtime"));
            foreach (string n in RUNTIME) {
                ahora[n] = HuellaRuntime(n);
                string o = "build\\obj\\runtime\\" + n + ".o";
                if (traducir || !antes.TryGetValue(n, out h) || h != ahora[n] || !File.Exists(Ruta(o)))
                    trabajos.Add(new[] { "runtime\\" + n + ".c", o, "-Iruntime" });
            }
            Compila(trabajos);
            Progreso(T("Uniendo las piezas…"), -1);
            var objetos = new StringBuilder();
            foreach (string d in Directory.GetDirectories(Ruta("build", "obj")))
                foreach (string o in Lista(d, "*.o"))
                    objetos.Append("\"build\\obj\\" + Path.GetFileName(d) + "\\" + Path.GetFileName(o) + "\" ");
            string exe = Ruta("build", "mkds.exe");
            if (File.Exists(exe)) File.Delete(exe);
            Ejecuta(Clang(), objetos + LIBRERIAS + " -Wl,--stack,268435456 -o build\\mkds.exe", true);
            if (!File.Exists(exe)) throw new Exception(T("El compilador no generó build\\mkds.exe."));
            var sb = new StringBuilder();
            foreach (var kv in ahora) sb.AppendLine(kv.Key + "=" + kv.Value);
            File.WriteAllText(Ruta("build", "preparado.txt"), sb.ToString());
            // el código traducido ya está compilado: fuera (son cientos de MB)
            try {
                if (Directory.Exists(Ruta("generado"))) Directory.Delete(Ruta("generado"), true);
                if (Directory.Exists(Ruta("extraido"))) Directory.Delete(Ruta("extraido"), true);
            } catch (Exception) { }
            log.WriteLine("terminado");
        }
        log = null;
    }

    // Python y el compilador van fuera de la carpeta del juego, en una ruta corta (el compilador tiene rutas
    // largas y Windows no pasa de 260 caracteres) y compartida si hay varias copias del juego
    static string Herramientas_() {
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DSKartRecompiled", "herramientas");
    }

    static string Clang() { return Path.Combine(Herramientas_(), "llvm", "bin", "clang.exe"); }

    static void Python(string argumentos) {
        // -I: sin variables de entorno ni la carpeta actual en el path; -X utf8: los mensajes con tildes
        Ejecuta(Path.Combine(Herramientas_(), "python", "python.exe"), "-I -X utf8 " + argumentos, true);
    }

    // Lanza un programa en la carpeta del juego, sin ventana, y apunta lo que diga en el log
    static int Ejecuta(string exe, string argumentos, bool debeIrBien) {
        if (cancelado) throw new OperationCanceledException();
        var psi = new ProcessStartInfo(exe, argumentos);
        psi.WorkingDirectory = carpeta;
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.StandardOutputEncoding = Encoding.UTF8;
        psi.StandardErrorEncoding = Encoding.UTF8;
        var p = new Process();
        p.StartInfo = psi;
        var salida = new StringBuilder();
        DataReceivedEventHandler apunta = delegate(object s, DataReceivedEventArgs e) {
            if (e.Data != null) lock (salida) salida.AppendLine(e.Data);
        };
        p.OutputDataReceived += apunta;
        p.ErrorDataReceived += apunta;
        p.Start();
        lock (enMarcha) enMarcha.Add(p);
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        p.WaitForExit();
        lock (enMarcha) enMarcha.Remove(p);
        lock (log) {
            log.WriteLine("> " + Path.GetFileName(exe) + " " + argumentos);
            log.Write(salida.ToString());
        }
        if (cancelado) throw new OperationCanceledException();
        if (debeIrBien && p.ExitCode != 0) {
            string s = salida.ToString().Trim();
            if (s.Length > 600) s = "…" + s.Substring(s.Length - 600);
            throw new Exception(Path.GetFileName(exe) + " (" + p.ExitCode + "):\n" + s);
        }
        return p.ExitCode;
    }

    // Compila en paralelo (un núcleo libre para que el PC no se quede tieso)
    static void Compila(List<string[]> trabajos) {
        if (trabajos.Count == 0) return;
        int hechos = 0, siguiente = 0;
        string fallo = null;
        int hilos = Math.Max(1, Math.Min(Environment.ProcessorCount - 1, 16));
        Progreso(string.Format(T("Compilando… {0} de {1}"), 0, trabajos.Count), 0);
        var lista = new List<Thread>();
        for (int k = 0; k < hilos; k++) {
            var t = new Thread(delegate() {
                while (true) {
                    int i = Interlocked.Increment(ref siguiente) - 1;
                    if (i >= trabajos.Count || fallo != null || cancelado) return;
                    string[] w = trabajos[i];
                    try { Ejecuta(Clang(), "-O2 -w " + w[2] + " -c \"" + w[0] + "\" -o \"" + w[1] + "\"", true); }
                    catch (Exception ex) { if (fallo == null) fallo = ex.Message; return; }
                    int n = Interlocked.Increment(ref hechos);
                    Progreso(string.Format(T("Compilando… {0} de {1}"), n, trabajos.Count), (double)n / trabajos.Count);
                }
            });
            t.IsBackground = true;
            t.Start();
            lista.Add(t);
        }
        foreach (var t in lista) t.Join();
        if (cancelado) throw new OperationCanceledException();
        if (fallo != null) throw new Exception(fallo);
    }

    static void Cancela() {
        cancelado = true;
        Busca<Button>("PrepCancelar").IsEnabled = false;
        lock (enMarcha) foreach (var p in enMarcha) try { p.Kill(); } catch (Exception) { }
        if (cliente != null) cliente.CancelAsync();
    }

    // ---- descargas ---------------------------------------------------------------------------------------
    static WebClient cliente;

    static void Descarga(string url, string destino, string paso) {
        ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;  // TLS 1.2 (.NET 4 no lo trae puesto)
        var hecho = new ManualResetEvent(false);
        Exception error = null;
        using (cliente = new WebClient()) {
            cliente.Headers.Add("User-Agent", "DS-Kart-Recompiled-Launcher");
            cliente.DownloadProgressChanged += delegate(object s, DownloadProgressChangedEventArgs e) {
                if (e.TotalBytesToReceive > 0)
                    Progreso(paso + string.Format(" ({0} / {1} MB)", e.BytesReceived >> 20, e.TotalBytesToReceive >> 20),
                             (double)e.BytesReceived / e.TotalBytesToReceive);
            };
            cliente.DownloadFileCompleted += delegate(object s, System.ComponentModel.AsyncCompletedEventArgs e) {
                error = e.Cancelled ? new OperationCanceledException() : e.Error;
                hecho.Set();
            };
            cliente.DownloadFileAsync(new Uri(url), destino);
            hecho.WaitOne();
        }
        cliente = null;
        if (error != null) {
            try { File.Delete(destino); } catch (Exception) { }
            throw error;
        }
    }

    static string Sha256(string archivo) {
        using (var sha = SHA256.Create())
        using (var f = File.OpenRead(archivo))
            return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "").ToLowerInvariant();
    }

    // Python y el compilador, de sus páginas oficiales (solo la primera vez)
    static void Herramientas() {
        DescargaHerramienta("python", URL_PYTHON, SHA_PYTHON, "Python", null);
        // del compilador solo hace falta lo de Windows de 64 bits (sin depurador ni las otras arquitecturas)
        DescargaHerramienta("llvm", URL_LLVM, SHA_LLVM, T("el compilador"), delegate(string r) {
            string[] sobra = { "aarch64-w64-mingw32/", "armv7-w64-mingw32/", "i686-w64-mingw32/", "python/", "share/",
                               "bin/lldb", "bin/liblldb", "bin/libpython", "bin/clangd", "bin/clang-tidy" };
            foreach (string s in sobra) if (r.StartsWith(s, StringComparison.OrdinalIgnoreCase)) return false;
            return true;
        });
    }

    static void DescargaHerramienta(string nombre, string url, string sha, string quien, Func<string, bool> vale) {
        string dir = Path.Combine(Herramientas_(), nombre);
        if (File.Exists(Path.Combine(dir, "listo.txt"))) return;
        Directory.CreateDirectory(Herramientas_());
        string zip = dir + ".zip";
        if (!File.Exists(zip) || Sha256(zip) != sha)
            Descarga(url, zip, string.Format(T("Descargando {0}…"), quien));
        Progreso(string.Format(T("Comprobando {0}…"), quien), -1);
        if (Sha256(zip) != sha) {
            File.Delete(zip);
            throw new Exception(string.Format(T("La descarga de {0} no es la esperada (puede que se cortara). Vuelve a intentarlo."), quien));
        }
        Progreso(string.Format(T("Descomprimiendo {0}…"), quien), 0);
        if (Directory.Exists(dir)) Directory.Delete(dir, true);
        using (var z = ZipFile.OpenRead(zip)) {
            // el zip del compilador lo mete todo en una carpeta con su versión: se quita
            string raiz = "";
            int barra = z.Entries[0].FullName.IndexOf('/');
            if (barra > 0 && z.Entries[0].FullName.Substring(0, barra).StartsWith("llvm-mingw")) raiz = z.Entries[0].FullName.Substring(0, barra + 1);
            int i = 0;
            foreach (ZipArchiveEntry e in z.Entries) {
                if (cancelado) throw new OperationCanceledException();
                if (++i % 200 == 0) Progreso(string.Format(T("Descomprimiendo {0}…"), quien), (double)i / z.Entries.Count);
                string r = e.FullName.StartsWith(raiz) ? e.FullName.Substring(raiz.Length) : e.FullName;
                if (r.Length == 0 || r.EndsWith("/") || (vale != null && !vale(r))) continue;
                string destino = Path.Combine(dir, r.Replace('/', '\\'));
                Directory.CreateDirectory(Path.GetDirectoryName(destino));
                e.ExtractToFile(destino, true);
            }
        }
        File.WriteAllText(Path.Combine(dir, "listo.txt"), url);
        File.Delete(zip);
    }

    // ---- actualizar -------------------------------------------------------------------------------------
    static string versionNueva, zipNueva;

    // Al abrir, sin molestar: si hay una versión más nueva en GitHub, sale el botón de actualizar
    static void BuscaActualizacion(bool avisarSiNoHay) {
        var hilo = new Thread(delegate() {
            string tag = null, zip = null, error = null;
            try {
                ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
                using (var w = new WebClient()) {
                    w.Headers.Add("User-Agent", "DS-Kart-Recompiled-Launcher");
                    w.Headers.Add("Accept", "application/vnd.github+json");
                    w.Encoding = Encoding.UTF8;
                    string json = w.DownloadString("https://api.github.com/repos/" + REPO + "/releases/latest");
                    var m = Regex.Match(json, "\"tag_name\"\\s*:\\s*\"v?([0-9.]+)\"");
                    if (m.Success) tag = m.Groups[1].Value;
                    // el zip de la versión (el mismo que se descarga la primera vez)
                    foreach (Match a in Regex.Matches(json, "\"browser_download_url\"\\s*:\\s*\"([^\"]+\\.zip)\""))
                        if (zip == null) zip = a.Groups[1].Value;
                }
            } catch (Exception ex) { error = ex.Message; }
            ventana.Dispatcher.BeginInvoke(new Action(delegate {
                if (tag != null && zip != null && new Version(tag) > new Version(VERSION_NUM)) {
                    versionNueva = tag;
                    zipNueva = zip;
                    var b = Busca<Button>("Actualizar");
                    b.Visibility = Visibility.Visible;
                    ((TextBlock)b.Content).Text = string.Format(T("Actualizar a la {0}"), tag);
                    Estado(string.Format(T("Hay una versión nueva: {0}"), tag), false);
                } else if (avisarSiNoHay) {
                    if (error != null) Estado(T("No se pudo comprobar si hay actualizaciones"), true);
                    else Estado(T("Tienes la última versión"), false);
                }
            }));
        });
        hilo.IsBackground = true;
        hilo.Start();
    }

    static void Actualiza() {
        if (zipNueva == null) return;
        if (MessageBox.Show(string.Format(T("Se descargará la versión {0} y el launcher se reiniciará. Tu ROM, tus ajustes y tu partida no se tocan."), versionNueva),
                            TITULO, MessageBoxButton.OKCancel, MessageBoxImage.Information) != MessageBoxResult.OK) return;
        cancelado = false;
        EnseñaPreparando(true, string.Format(T("Actualizando a la {0}"), versionNueva));
        Busca<TextBlock>("PrepNota").Text = T("Al terminar, el launcher se abre de nuevo. La próxima vez que juegues se prepara lo que haya cambiado.");
        string url = zipNueva;
        var hilo = new Thread(delegate() {
            string error = null;
            try { Actualiza_(url); }
            catch (Exception ex) { error = ex.Message; }
            ventana.Dispatcher.BeginInvoke(new Action(delegate {
                if (error == null) {
                    GuardaAjustes();
                    Process.Start(Ruta("Launcher.exe"));
                    ventana.Close();
                    return;
                }
                EnseñaPreparando(false, null);
                if (!cancelado) MessageBox.Show(T("No se pudo actualizar:") + "\n\n" + error, TITULO, MessageBoxButton.OK, MessageBoxImage.Error);
            }));
        });
        hilo.IsBackground = true;
        hilo.Start();
    }

    static void Actualiza_(string url) {
        string tmp = Ruta("actualizacion");
        if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
        Directory.CreateDirectory(tmp);
        string zip = Path.Combine(tmp, "nueva.zip");
        Descarga(url, zip, T("Descargando la actualización…"));
        Progreso(T("Instalando…"), -1);
        string dir = Path.Combine(tmp, "nueva");
        ZipFile.ExtractToDirectory(zip, dir);
        // el zip puede traerlo todo dentro de una carpeta
        string raiz = dir;
        if (!File.Exists(Path.Combine(raiz, "Launcher.exe")) && Directory.GetDirectories(raiz).Length == 1)
            raiz = Directory.GetDirectories(raiz)[0];
        if (!File.Exists(Path.Combine(raiz, "Launcher.exe"))) throw new Exception(T("El zip de la actualización no trae Launcher.exe."));
        foreach (string f in Directory.GetFileSystemEntries(raiz)) {
            string nombre = Path.GetFileName(f);
            if (Array.IndexOf(DEL_USUARIO, nombre.ToLowerInvariant()) >= 0) continue;
            string destino = Ruta(nombre);
            if (nombre.Equals("Launcher.exe", StringComparison.OrdinalIgnoreCase)) {
                // el launcher está abierto: Windows deja renombrarlo pero no sobrescribirlo
                string viejo = Ruta("Launcher.old.exe");
                if (File.Exists(viejo)) File.Delete(viejo);
                File.Move(destino, viejo);
                File.Copy(f, destino);
            } else if (Directory.Exists(f)) {
                if (Directory.Exists(destino)) Directory.Delete(destino, true);
                CopiaCarpeta(f, destino);
            } else File.Copy(f, destino, true);
        }
        Directory.Delete(tmp, true);
    }

    static void CopiaCarpeta(string de, string a) {
        Directory.CreateDirectory(a);
        foreach (string f in Directory.GetFiles(de)) File.Copy(f, Path.Combine(a, Path.GetFileName(f)), true);
        foreach (string d in Directory.GetDirectories(de)) CopiaCarpeta(d, Path.Combine(a, Path.GetFileName(d)));
    }

    // el launcher de antes de actualizar se queda como Launcher.old.exe hasta la siguiente vez
    static void BorraLauncherViejo() {
        string viejo = Ruta("Launcher.old.exe");
        for (int i = 0; i < 10 && File.Exists(viejo); i++) {
            try { File.Delete(viejo); } catch (Exception) { Thread.Sleep(300); }
        }
    }
}
