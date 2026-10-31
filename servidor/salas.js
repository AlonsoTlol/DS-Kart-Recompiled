// Servidor de salas del online de DS Kart Recompiled (Worker de Cloudflare con un Durable Object).
//
// Solo presenta a los jugadores: el anfitrión crea una sala y le da un código de 6 letras; los demás se unen con
// el código y se intercambian las direcciones (la pública, sacada con STUN, y la de su red de casa). La partida va
// directa entre los PCs (UDP, "hole punching"): por aquí no pasa nada de la carrera.
// Todo está en memoria (una sala caduca a los 30 s sin noticias del anfitrión) y no se guarda nada en disco.
//
// Peticiones (POST, parámetros en la URL; respuestas en texto, una cosa por línea, para leerlas fácil desde C):
//   /crear?ip=&puerto=&ipl=&puertol=&v=      -> "codigo XXXXXX" y "clave ..." (la clave solo la sabe el anfitrión)
//   /latido?codigo=&clave=                   -> "invitado ip puerto ipl puertol" por cada uno que ha entrado
//   /unirse?codigo=&ip=&puerto=&ipl=&puertol=&v=  -> "anfitrion ip puerto ipl puertol" (o "error ...")
//   /sigo?codigo=&ip=&puerto=                -> el invitado sigue ahí (si no, se quita a los 30 s)
//   /cerrar?codigo=&clave=
// "v" es la versión del juego: solo se juntan jugadores con la misma (si no, el juego se desincronizaría).

const LETRAS = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // sin O/0 ni I/1, que se confunden
const CADUCA = 30000;
const MAX_SALAS = 2000, MAX_INVITADOS = 7;

function texto(t, estado = 200) {
  return new Response(t, { status: estado, headers: { "content-type": "text/plain; charset=utf-8" } });
}

function direccion(p, ip, puerto) {
  const a = (p.get(ip) || "").trim(), n = parseInt(p.get(puerto) || "", 10);
  if (!/^\d{1,3}(\.\d{1,3}){3}$/.test(a) || !(n > 0 && n < 65536)) return null;
  return { ip: a, puerto: n };
}

function linea(etiqueta, pub, loc) {
  return `${etiqueta} ${pub.ip} ${pub.puerto} ${loc ? loc.ip : "-"} ${loc ? loc.puerto : 0}\n`;
}

// ---- Conexión Wi-Fi de Nintendo (el menú Wi-Fi del juego) ----------------------------------------------------------
// Cada juego imita en local el servidor WFC (wfc.c y wfc_servicios.c); lo único que tiene que ser común a todos es
// el "master server" de GameSpy: quién está buscando partida (las claves que anuncia cada DS por QR2) y los mensajes
// que se mandan entre ellas para juntarse. A cada DS su juego le da una IP virtual (10.x.y.z) y la partida va
// directa entre los PCs (UDP, hole punching) igual que en las salas.
//   /wfc/latido?vip=&puerto=&ip=&ipp=&ipl=&iplp=&v=   cuerpo "clave\tvalor\n"...  -> "ok"
//   /wfc/lista?vip=&v=      -> por cada uno: "s vip puerto ip ipp ipl iplp" + "\tclave\tvalor"... + "\n"
//   /wfc/manda?de=&a=       cuerpo: los datos en hex   -> "ok" o "error nadie"
//   /wfc/espera?vip=        -> los mensajes pendientes, "m de puerto ip ipp ipl iplp hex\n" cada uno; si no hay
//                              ninguno, espera hasta 20 s a que llegue alguno ("nada" si no)
//   /wfc/natneg?vip=&cookie=&indice=&puerto=   NAT negotiation: cuando han llegado los dos lados de un mismo
//                              cookie, a cada uno le llega por /wfc/espera "n cookie vip puerto ip ipp ipl iplp\n"
//   /wfc/adios?vip=
const VIP = /^10\.\d{1,3}\.\d{1,3}\.\d{1,3}$/;
const MAX_WFC = 5000;

function wfc_linea(j) {
  return `${j.vip} ${j.puerto} ${j.pub ? j.pub.ip : "-"} ${j.pub ? j.pub.puerto : 0} ${j.loc ? j.loc.ip : "-"} ${j.loc ? j.loc.puerto : 0}`;
}

export class Salas {
  constructor() {
    this.salas = new Map();  // código -> { clave, v, anfitrion: {pub, loc}, invitados: Map(clave -> {pub, loc, visto}), visto }
    this.wfc = new Map();    // vip -> { vip, puerto, pub, loc, v, claves: "\tk\tv...", cola: [], despierta, visto }
  }

  limpia(ahora) {
    for (const [c, s] of this.salas) {
      if (ahora - s.visto > CADUCA) { this.salas.delete(c); continue; }
      for (const [k, i] of s.invitados) if (ahora - i.visto > CADUCA) s.invitados.delete(k);
    }
    for (const [v, j] of this.wfc) if (ahora - j.visto > CADUCA && !j.despierta) this.wfc.delete(v);
  }

  async wfc_peticion(req, ruta, p, ahora) {
    const vip = p.get("vip") || "";
    const j = this.wfc.get(vip);
    switch (ruta) {
      case "/wfc/latido": {
        if (!VIP.test(vip)) return texto("error vip\n", 400);
        if (!j && this.wfc.size >= MAX_WFC) return texto("error lleno\n", 503);
        const cuerpo = (await req.text()).slice(0, 4000);
        let claves = "";
        for (const l of cuerpo.split("\n")) {
          const [k, v] = l.split("\t");
          if (k && /^[\w]{1,32}$/.test(k)) claves += `\t${k}\t${(v || "").replace(/[\t\r\n]/g, " ").slice(0, 100)}`;
        }
        const n = j || { vip, cola: [], despierta: null };
        n.puerto = parseInt(p.get("puerto") || "0", 10) || 0;
        n.pub = direccion(p, "ip", "ipp");
        n.loc = direccion(p, "ipl", "iplp");
        n.v = p.get("v") || "";
        n.claves = claves;
        n.anuncia = claves.length > 0;
        n.visto = ahora;
        this.wfc.set(vip, n);
        return texto("ok\n");
      }
      case "/wfc/lista": {
        const v = p.get("v") || "";
        let r = "";
        for (const o of this.wfc.values()) if (o.v === v && o.anuncia) r += `s ${wfc_linea(o)}${o.claves}\n`;
        return texto(r || "nadie\n");
      }
      case "/wfc/manda": {
        const de = this.wfc.get(p.get("de") || ""), a = this.wfc.get(p.get("a") || "");
        if (!de || !a) return texto("error nadie\n", 404);
        const hex = (await req.text()).trim().slice(0, 4000);
        if (!/^[0-9a-fA-F]*$/.test(hex)) return texto("error datos\n", 400);
        if (a.cola.length < 64) a.cola.push(`m ${wfc_linea(de)} ${hex}\n`);
        if (a.despierta) a.despierta();
        return texto("ok\n");
      }
      case "/wfc/espera": {
        if (!j) return texto("error nadie\n", 404);
        j.visto = ahora;
        if (j.despierta) j.despierta();  // otra espera anterior: que termine
        if (j.cola.length) { const r = j.cola.join(""); j.cola = []; return texto(r); }
        return new Promise(res => {
          const t = setTimeout(() => fin(), 20000);
          const fin = () => {
            clearTimeout(t);
            j.despierta = null;
            j.visto = Date.now();
            const r = j.cola.join("");
            j.cola = [];
            res(texto(r || "nada\n"));
          };
          j.despierta = fin;
        });
      }
      case "/wfc/natneg": {  // NAT negotiation: cuando las dos DS han dado el mismo número, cada una sabe dónde está la otra
        const cookie = (p.get("cookie") || "").slice(0, 12), indice = p.get("indice") === "1" ? 1 : 0;
        const puerto = parseInt(p.get("puerto") || "0", 10) || 0;
        if (!j || !cookie) return texto("error nadie\n", 404);
        if (!this.natneg) this.natneg = new Map();
        for (const [c, n] of this.natneg) if (ahora - n.visto > 60000) this.natneg.delete(c);
        const n = this.natneg.get(cookie) || { lados: [null, null] };
        n.lados[indice] = { vip, puerto };
        n.visto = ahora;
        this.natneg.set(cookie, n);
        const [a, b] = n.lados;
        if (a && b) {
          for (const [yo, otro] of [[a, b], [b, a]]) {
            const x = this.wfc.get(yo.vip), o = this.wfc.get(otro.vip);
            if (!x || !o) continue;
            x.cola.push(`n ${cookie} ${wfc_linea({ ...o, puerto: otro.puerto })}\n`);
            if (x.despierta) x.despierta();
          }
          this.natneg.delete(cookie);
        }
        return texto("ok\n");
      }
      case "/wfc/adios": {
        if (j) { if (j.despierta) j.despierta(); this.wfc.delete(vip); }
        return texto("ok\n");
      }
    }
    return texto("error que\n", 404);
  }

  async fetch(req) {
    if (req.method !== "POST") return texto("DS Kart Recompiled: servidor de salas\n");
    const url = new URL(req.url), p = url.searchParams, ahora = Date.now();
    this.limpia(ahora);
    if (url.pathname.startsWith("/wfc/")) return this.wfc_peticion(req, url.pathname, p, ahora);
    const codigo = (p.get("codigo") || "").toUpperCase();
    const sala = this.salas.get(codigo);
    switch (url.pathname) {
      case "/crear": {
        const pub = direccion(p, "ip", "puerto"), loc = direccion(p, "ipl", "puertol");
        if (!pub) return texto("error direccion\n", 400);
        if (this.salas.size >= MAX_SALAS) return texto("error lleno\n", 503);
        let c;
        do { c = Array.from(crypto.getRandomValues(new Uint8Array(6)), b => LETRAS[b % LETRAS.length]).join(""); }
        while (this.salas.has(c));
        const clave = crypto.randomUUID();
        this.salas.set(c, { clave, v: p.get("v") || "", anfitrion: { pub, loc }, invitados: new Map(), visto: ahora });
        return texto(`codigo ${c}\nclave ${clave}\n`);
      }
      case "/latido": {
        if (!sala || sala.clave !== p.get("clave")) return texto("error sala\n", 404);
        sala.visto = ahora;
        let r = "";
        for (const i of sala.invitados.values()) r += linea("invitado", i.pub, i.loc);
        return texto(r || "nadie\n");
      }
      case "/unirse": {
        if (!sala) return texto("error sala\n", 404);
        if (sala.v !== (p.get("v") || "")) return texto("error version\n", 409);
        const pub = direccion(p, "ip", "puerto"), loc = direccion(p, "ipl", "puertol");
        if (!pub) return texto("error direccion\n", 400);
        const k = `${pub.ip}:${pub.puerto}`;
        if (!sala.invitados.has(k) && sala.invitados.size >= MAX_INVITADOS) return texto("error llena\n", 409);
        sala.invitados.set(k, { pub, loc, visto: ahora });
        return texto(linea("anfitrion", sala.anfitrion.pub, sala.anfitrion.loc));
      }
      case "/sigo": {
        const i = sala && sala.invitados.get(`${p.get("ip")}:${p.get("puerto")}`);
        if (!i) return texto("error sala\n", 404);
        i.visto = ahora;
        return texto("ok\n");
      }
      case "/cerrar": {
        if (sala && sala.clave === p.get("clave")) this.salas.delete(codigo);
        return texto("ok\n");
      }
    }
    return texto("error que\n", 404);
  }
}

export default {
  async fetch(req, env) {
    // una sola instancia para todas las salas (de sobra para lo que hay): se reparte por nombre fijo
    return env.SALAS.get(env.SALAS.idFromName("salas")).fetch(req);
  },
};
