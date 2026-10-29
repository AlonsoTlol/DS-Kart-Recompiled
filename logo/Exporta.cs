// Dibuja el logo (logo_xaml.txt, de genera_logo.py) a PNG y monta el icono de Windows.
//   Exporta.exe logo_xaml.txt carpeta_salida
// Deja logo_512.png, una hoja de prueba (prueba.png: tamaños 256..16 sobre fondo claro y oscuro) e icono.ico
// (entradas PNG de 16, 24, 32, 48, 64, 128 y 256; Windows Vista en adelante las entiende).
// Compilar: sh logo/compilar.sh (csc de .NET Framework 4, C# 5).
using System;
using System.IO;
using System.Windows;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;

class Exporta {
    static Drawing dibujo, dibujo_pequeno;  // el pequeño (sin detalles) para 32 píxeles o menos

    static Drawing Lee(string ruta) {
        string xaml = File.ReadAllText(ruta).Replace("<DrawingGroup>",
            "<DrawingGroup xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'>");
        return (Drawing)XamlReader.Parse(xaml);
    }

    static BitmapSource Pinta(int tam, Brush fondo) {
        var dv = new DrawingVisual();
        using (var dc = dv.RenderOpen()) {
            if (fondo != null) dc.DrawRoundedRectangle(fondo, null, new Rect(0, 0, tam, tam), tam * 0.22, tam * 0.22);
            dc.PushTransform(new ScaleTransform(tam / 512.0, tam / 512.0));
            dc.DrawDrawing(tam <= 32 ? dibujo_pequeno : dibujo);
            dc.Pop();
        }
        var bmp = new RenderTargetBitmap(tam, tam, 96, 96, PixelFormats.Pbgra32);
        bmp.Render(dv);
        return bmp;
    }

    static byte[] Png(BitmapSource b) {
        var enc = new PngBitmapEncoder();
        enc.Frames.Add(BitmapFrame.Create(b));
        using (var ms = new MemoryStream()) { enc.Save(ms); return ms.ToArray(); }
    }

    [STAThread]
    static void Main(string[] args) {
        dibujo = Lee(args[0]);
        dibujo_pequeno = Lee(args[0].Replace(".txt", "_pequeno.txt"));
        string sal = args[1];
        File.WriteAllBytes(Path.Combine(sal, "logo_512.png"), Png(Pinta(512, null)));

        // hoja de prueba: cada tamaño sobre claro y sobre oscuro
        int[] tams = { 256, 128, 64, 48, 32, 24, 16 };
        var dv = new DrawingVisual();
        using (var dc = dv.RenderOpen()) {
            dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(0xEE, 0xEF, 0xF5)), null, new Rect(0, 0, 640, 290));
            dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(0x12, 0x15, 0x2A)), null, new Rect(0, 290, 640, 290));
            for (int fila = 0; fila < 2; fila++) {
                double x = 10;
                foreach (int t in tams) {
                    dc.DrawImage(Pinta(t, null), new Rect(x, fila * 290 + 10, t, t));
                    x += t + 12;
                }
            }
        }
        var hoja = new RenderTargetBitmap(640, 580, 96, 96, PixelFormats.Pbgra32);
        hoja.Render(dv);
        File.WriteAllBytes(Path.Combine(sal, "prueba.png"), Png(hoja));

        // icono: cabecera ICONDIR + una entrada por tamaño con su PNG
        int[] ico = { 16, 24, 32, 48, 64, 128, 256 };
        var pngs = new byte[ico.Length][];
        for (int i = 0; i < ico.Length; i++) pngs[i] = Png(Pinta(ico[i], null));
        using (var f = new BinaryWriter(File.Create(Path.Combine(sal, "icono.ico")))) {
            f.Write((short)0); f.Write((short)1); f.Write((short)ico.Length);
            int off = 6 + 16 * ico.Length;
            for (int i = 0; i < ico.Length; i++) {
                f.Write((byte)(ico[i] >= 256 ? 0 : ico[i])); f.Write((byte)(ico[i] >= 256 ? 0 : ico[i]));
                f.Write((byte)0); f.Write((byte)0);
                f.Write((short)1); f.Write((short)32);
                f.Write(pngs[i].Length); f.Write(off);
                off += pngs[i].Length;
            }
            foreach (var p in pngs) f.Write(p);
        }
    }
}
