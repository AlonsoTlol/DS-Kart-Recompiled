# Crea el zip de una versión con rutas "a/b" (el estándar de los zip: Compress-Archive de PowerShell 5 ponía "a\b",
# y en Linux salían archivos llamados "DS-Kart-Recompiled\runtime\..." en vez de carpetas) y los .sh ejecutables.
# Uso: python zip_version.py carpeta salida.zip   (la carpeta entra en el zip con su nombre)
import os, sys, zipfile
carpeta, salida = sys.argv[1], sys.argv[2]
base = os.path.dirname(os.path.abspath(carpeta))
with zipfile.ZipFile(salida, "w", zipfile.ZIP_DEFLATED) as z:
    for raiz, dirs, archivos in os.walk(carpeta):
        dirs.sort()
        for a in sorted(archivos):
            ruta = os.path.join(raiz, a)
            nombre = os.path.relpath(ruta, base).replace(os.sep, "/")
            info = zipfile.ZipInfo.from_file(ruta, nombre)
            info.external_attr = (0o755 if a.endswith(".sh") else 0o644) << 16
            info.create_system = 3  # Unix: para que se respeten los permisos al descomprimir en Linux
            info.compress_type = zipfile.ZIP_STORED if a.endswith(".mp4") else zipfile.ZIP_DEFLATED
            with open(ruta, "rb") as f:
                z.writestr(info, f.read())
print(salida)
