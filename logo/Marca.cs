// Logo horizontal de DS Kart Recompiled, estilo pegatina: la rueda (logo_xaml.txt) a la izquierda y el texto
// "DS KART" en cromo y "RECOMPILED" en rojo, letra gruesa inclinada, con bordes oscuro + blanco + oscuro y
// destellos alrededor. Todo vectorial: el texto se convierte en contornos (no hace falta la fuente después).
//   Marca.exe logo_xaml.txt carpeta_salida
// Deja logo_horizontal_xaml.txt (para el launcher), logo_horizontal.svg y logo_horizontal.png.
// Compilar: sh logo/compilar.sh (csc de .NET Framework 4, C# 5).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;

class Marca {
    const string NOCHE = "#12152A", BLANCO = "#FFFFFF";
    static readonly CultureInfo inv = CultureInfo.InvariantCulture;

    static Geometry Texto(string t, double tam) {
        var ft = new FormattedText(t, inv, FlowDirection.LeftToRight,
            new Typeface(new FontFamily("Arial Black"), FontStyles.Normal, FontWeights.Black, FontStretches.Normal),
            tam, Brushes.Black);
        return ft.BuildGeometry(new Point(0, 0));
    }

    // coloca una geometría: escala para que mida 'ancho', inclinada hacia delante, con su esquina en (x, y)
    static Geometry Coloca(Geometry g, double ancho, double x, double y) {
        Rect b = g.Bounds;
        double s = ancho / b.Width;
        var tg = new TransformGroup();
        tg.Children.Add(new TranslateTransform(-b.X, -b.Y));
        tg.Children.Add(new ScaleTransform(s, s));
        tg.Children.Add(new SkewTransform(-12, 0, 0, b.Height * s));  // inclinada: velocidad
        tg.Children.Add(new TranslateTransform(x, y));
        var r = g.Clone();
        r.Transform = tg;
        return PathGeometry.CreateFromGeometry(r);
    }

    // destello de cuatro puntas
    static Geometry Destello(double cx, double cy, double r) {
        double k = r * 0.22;
        return Geometry.Parse(string.Format(inv,
            "M{0},{1} Q{2},{3} {4},{5} Q{2},{6} {0},{7} Q{8},{6} {9},{5} Q{8},{3} {0},{1} Z",
            cx, cy - r, cx + k, cy - k, cx + r, cy, cy + k, cy + r, cx - k, cx - r));
    }

    static string D(Geometry g) {
        string s = PathGeometry.CreateFromGeometry(g).ToString(inv);
        return s;
    }

    class Capa { public string geom, relleno, trazo; public double grosor; }

    [STAThread]
    static void Main(string[] args) {
        string sal = args[1];
        // rueda: el dibujo del logo, a 400x400 en (0, 0)
        string ruedaXaml = File.ReadAllText(args[0]);

        Geometry ds = Coloca(Texto("DS KART", 200), 980, 400, 40);
        Geometry re = Coloca(Texto("RECOMPILED", 200), 1040, 370, 205);
        var destellos = new List<Geometry> {
            Destello(1385, 45, 46), Destello(1455, 120, 24), Destello(395, 352, 30), Destello(1420, 355, 34),
        };
        var capas = new List<Capa>();
        string[] grupos = { D(ds), D(re) };
        // bordes de pegatina: de fuera a dentro, de los dos renglones juntos para que se fundan
        foreach (string g in grupos) capas.Add(new Capa { geom = g, trazo = NOCHE, grosor = 52 });
        foreach (string g in grupos) capas.Add(new Capa { geom = g, trazo = BLANCO, grosor = 36 });
        foreach (string g in grupos) capas.Add(new Capa { geom = g, trazo = NOCHE, grosor = 16 });
        capas.Add(new Capa { geom = grupos[0], relleno = "url(cromo)" });
        capas.Add(new Capa { geom = grupos[1], relleno = "url(rojo)" });

        // ---- XAML para el launcher y para pintar el PNG ----
        var x = new StringBuilder();
        // lienzo de 1560x500 con 40 de margen: los bordes de pegatina sobresalen de las letras
        x.AppendLine("<DrawingGroup>");
        x.AppendLine("  <GeometryDrawing Brush='#00000000' Geometry='M0,0 L1560,0 L1560,500 L0,500 Z'/>");
        x.AppendLine("  <DrawingGroup Transform='1,0,0,1,40,40'>");
        foreach (var c in capas) {
            if (c.trazo != null)
                x.AppendFormat(inv, "  <GeometryDrawing Geometry='{0}'><GeometryDrawing.Pen><Pen Brush='{1}' Thickness='{2}' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>\n", c.geom, c.trazo, c.grosor);
            else
                x.AppendFormat("  <GeometryDrawing Geometry='{0}'><GeometryDrawing.Brush>{1}</GeometryDrawing.Brush></GeometryDrawing>\n",
                               c.geom, c.relleno == "url(cromo)" ? CromoXaml() : RojoXaml());
        }
        // destellos: borde oscuro y relleno blanco
        foreach (var g in destellos) {
            x.AppendFormat(inv, "  <GeometryDrawing Brush='{0}' Geometry='{1}'><GeometryDrawing.Pen><Pen Brush='{2}' Thickness='10' LineJoin='Round'/></GeometryDrawing.Pen></GeometryDrawing>\n",
                           BLANCO, D(g), NOCHE);
        }
        // rueda encima, con su borde de pegatina
        x.AppendLine("  <GeometryDrawing Brush='" + NOCHE + "' Geometry='M200,200 m-212,0 a212,212 0 1 0 424,0 a212,212 0 1 0 -424,0'/>");
        x.AppendLine("  <GeometryDrawing Brush='" + BLANCO + "' Geometry='M200,200 m-200,0 a200,200 0 1 0 400,0 a200,200 0 1 0 -400,0'/>");
        x.AppendLine("  <DrawingGroup Transform='0.69,0,0,0.69,23.4,23.4'>");
        x.AppendLine(ruedaXaml.Replace("<DrawingGroup>", "").Replace("</DrawingGroup>", ""));
        x.AppendLine("  </DrawingGroup>");
        x.AppendLine("  </DrawingGroup>");
        x.AppendLine("</DrawingGroup>");
        string xaml = x.ToString().Replace("<DrawingGroup>\r\n", "<DrawingGroup>\n");
        File.WriteAllText(Path.Combine(sal, "logo_horizontal_xaml.txt"), xaml, new UTF8Encoding(false));

        // ---- PNG ----
        var dib = (Drawing)XamlReader.Parse(xaml.Replace("<DrawingGroup>",
            "<DrawingGroup xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'>").Replace(
            "<DrawingGroup xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Transform", "<DrawingGroup Transform"));
        Pinta(dib, 1560, 500, Path.Combine(sal, "logo_horizontal.png"), null);
        Pinta(dib, 1560, 500, Path.Combine(sal, "logo_horizontal_oscuro.png"), new SolidColorBrush(Color.FromRgb(0x10, 0x13, 0x2A)));

        // ---- SVG ----
        var svg = new StringBuilder();
        svg.AppendLine("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1560 500\" width=\"1560\" height=\"500\">");
        svg.AppendLine("  <!-- DS Kart Recompiled: logo horizontal (logo/Marca.cs) -->");
        svg.AppendLine("  <defs>");
        svg.AppendLine("    <linearGradient id=\"cromo\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\"><stop offset=\"0\" stop-color=\"#FFFFFF\"/><stop offset=\"0.42\" stop-color=\"#C7D0E4\"/><stop offset=\"0.5\" stop-color=\"#56607F\"/><stop offset=\"0.62\" stop-color=\"#8E99B8\"/><stop offset=\"1\" stop-color=\"#EEF1F8\"/></linearGradient>");
        svg.AppendLine("    <linearGradient id=\"rojo\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\"><stop offset=\"0\" stop-color=\"#FFB38F\"/><stop offset=\"0.4\" stop-color=\"#FF5A3C\"/><stop offset=\"0.52\" stop-color=\"#E1263A\"/><stop offset=\"1\" stop-color=\"#7E0F2B\"/></linearGradient>");
        svg.AppendLine("  </defs>");
        svg.AppendLine("  <g transform=\"translate(40 40)\">");
        foreach (var c in capas) {
            string d = c.geom.StartsWith("F1") ? c.geom.Substring(2) : c.geom.StartsWith("F0") ? c.geom.Substring(2) : c.geom;
            if (c.trazo != null)
                svg.AppendFormat(inv, "  <path d=\"{0}\" fill=\"none\" stroke=\"{1}\" stroke-width=\"{2}\" stroke-linejoin=\"round\"/>\n", d, c.trazo, c.grosor);
            else
                svg.AppendFormat("  <path d=\"{0}\" fill=\"{1}\"/>\n", d, c.relleno == "url(cromo)" ? "url(#cromo)" : "url(#rojo)");
        }
        foreach (var g in destellos) {
            string d = D(g);
            if (d.StartsWith("F")) d = d.Substring(2);
            svg.AppendFormat("  <path d=\"{0}\" fill=\"#FFFFFF\" stroke=\"{1}\" stroke-width=\"10\" stroke-linejoin=\"round\"/>\n", d, NOCHE);
        }
        svg.AppendLine("  <circle cx=\"200\" cy=\"200\" r=\"212\" fill=\"" + NOCHE + "\"/><circle cx=\"200\" cy=\"200\" r=\"200\" fill=\"#FFFFFF\"/>");
        svg.AppendLine("  <g transform=\"matrix(0.69 0 0 0.69 23.4 23.4)\">");
        foreach (string l in File.ReadAllLines(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(args[0])), "logo.svg")))
            if (l.TrimStart().StartsWith("<path")) svg.AppendLine("  " + l);
        svg.AppendLine("  </g>");
        svg.AppendLine("  </g>");
        svg.AppendLine("</svg>");
        File.WriteAllText(Path.Combine(sal, "logo_horizontal.svg"), svg.ToString(), new UTF8Encoding(false));
    }

    static string CromoXaml() {
        return "<LinearGradientBrush StartPoint='0,0' EndPoint='0,1'><GradientStop Color='#FFFFFF' Offset='0'/><GradientStop Color='#C7D0E4' Offset='0.42'/><GradientStop Color='#56607F' Offset='0.5'/><GradientStop Color='#8E99B8' Offset='0.62'/><GradientStop Color='#EEF1F8' Offset='1'/></LinearGradientBrush>";
    }
    static string RojoXaml() {
        return "<LinearGradientBrush StartPoint='0,0' EndPoint='0,1'><GradientStop Color='#FFB38F' Offset='0'/><GradientStop Color='#FF5A3C' Offset='0.4'/><GradientStop Color='#E1263A' Offset='0.52'/><GradientStop Color='#7E0F2B' Offset='1'/></LinearGradientBrush>";
    }

    static void Pinta(Drawing d, int w, int h, string ruta, Brush fondo) {
        var dv = new DrawingVisual();
        using (var dc = dv.RenderOpen()) {
            if (fondo != null) dc.DrawRectangle(fondo, null, new Rect(0, 0, w, h));
            dc.DrawDrawing(d);
        }
        var bmp = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32);
        bmp.Render(dv);
        var enc = new PngBitmapEncoder();
        enc.Frames.Add(BitmapFrame.Create(bmp));
        using (var f = File.Create(ruta)) enc.Save(f);
    }
}
