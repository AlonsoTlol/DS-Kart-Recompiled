# Actualizaciones del launcher de Linux y macOS (launcher/lanzador.cpp). Va con curl, que ya usa el juego para el online.
#   python3 linux/actualiza.py mira         -> "versión url" si hay una más nueva en GitHub; si no, nada
#   python3 linux/actualiza.py instala url  -> baja el zip y lo copia aquí, sin tocar lo del usuario
import json, os, platform, shutil, subprocess, sys, zipfile

REPO = "AlonsoTlol/DS-Kart-Recompiled"
DEL_USUARIO = {"rom", "ajustes.ini", "partida.sav", "repeticiones", "build"}


def numeros(v):
    return tuple(int(x) for x in v.split(".") if x.isdigit())


def mira():
    with open("linux/version.txt") as f:
        mia = f.read().strip()
    r = subprocess.run(["curl", "-fsSL", "-m", "15", "https://api.github.com/repos/%s/releases/latest" % REPO],
                       capture_output=True, check=True)
    d = json.loads(r.stdout)
    tag = d["tag_name"].lstrip("v")
    marca = "-Mac-" if platform.system() == "Darwin" else "-Linux-"
    url = next((a["browser_download_url"] for a in d["assets"] if marca in a["name"]), None)
    if url and numeros(tag) > numeros(mia):
        print(tag, url)


def instala(url):
    tmp = ".actualizacion"
    shutil.rmtree(tmp, ignore_errors=True)
    os.mkdir(tmp)
    z = os.path.join(tmp, "nueva.zip")
    subprocess.run(["curl", "-fsSL", "-m", "300", "-o", z, url], check=True)
    dentro = os.path.join(tmp, "nueva")
    with zipfile.ZipFile(z) as f:
        for m in f.infolist():
            p = f.extract(m, dentro)
            os.chmod(p, (m.external_attr >> 16) or 0o644)
    # el zip lo trae todo dentro de una carpeta
    raiz = dentro
    if not os.path.exists(os.path.join(raiz, "linux")) and len(os.listdir(raiz)) == 1:
        raiz = os.path.join(raiz, os.listdir(raiz)[0])
    if not os.path.exists(os.path.join(raiz, "linux", "jugar.sh")):
        raise SystemExit("zip sin linux/jugar.sh")
    for n in os.listdir(raiz):
        if n.lower() in DEL_USUARIO:
            continue
        a = os.path.join(raiz, n)
        if os.path.isdir(a):
            shutil.rmtree(n, ignore_errors=True)
            shutil.copytree(a, n)
        else:
            shutil.copy2(a, n)
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    if sys.argv[1] == "mira":
        mira()
    elif sys.argv[1] == "instala":
        instala(sys.argv[2])
