/*
  Parksystem M450 – Pico 2 (Schranken)
  Atieh Rostamian & Sabina Lazri

  Aufgaben:
    - US 1  Einfahrt: Kamera-Simulation (Kennzeichen eintippen), speichern, Schranke öffnen
    - US 3  Ausfahrt: Bezahlstatus bei Pico 1 abfragen, Schranke öffnen oder zu lassen, archivieren
    - US 4  Anzeige freie Plätze / «Besetzt»
    - US 5  Admin-Panel: Login, Schranken manuell öffnen, Logbuch, Reset
  Pico 2 spannt das Test-WLAN auf (Access Point, IP 192.168.42.1).
  Pico 1 (Kasse) verbindet sich damit und meldet sich regelmässig über /api/hello.

  Verdrahtung (siehe README):
    Servo Einfahrt  Signal -> GP8 (Pin 11)
    Servo Ausfahrt  Signal -> GP9 (Pin 12)
    beide Servos    VCC -> VBUS (Pin 40), GND -> GND (z.B. Pin 8)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Servo.h>
#include <LittleFS.h>
#include <time.h>
#include "kennzeichen.h"

// ---------- Konfiguration (Testwerte aus Testkonzept 2.5) ----------
const char* WLAN_SSID = "Parksystem-TEST";
const char* WLAN_PASSWORT = "parken2026";
const char* API_KEY = "m450-parksystem";
const char* FIRMWARE_VERSION = "1.0.0";
const int KAPAZITAET = 5;
const char* ADMIN_BENUTZER = "betreiber1";
const char* ADMIN_PASSWORT = "Park2026!";

const int PIN_SERVO_EINFAHRT = 8;  // GP8 = Pin 11
const int PIN_SERVO_AUSFAHRT = 9;  // GP9 = Pin 12
const int WINKEL_ZU = 0;
const int WINKEL_OFFEN = 90;
const uint32_t OFFEN_DAUER_MS = 5000;     // Schranke schliesst nach der Durchfahrt automatisch
const uint32_t MELDUNG_DAUER_MS = 15000;  // so lange bleibt eine Meldung auf dem Display
const uint32_t PICO1_ONLINE_MS = 15000;   // Pico 1 gilt als erreichbar, wenn er sich so kürzlich gemeldet hat
const uint32_t SYNC_INTERVALL_MS = 2000;
const uint16_t HTTP_TIMEOUT_MS = 2000;
const int MAX_ARCHIV = 30;
const int MAX_LOG = 50;
const int MAX_SITZUNGEN = 4;

const char* MSG_UNLESBAR = "Kennzeichen nicht erkannt. Bitte Hilfeknopf drücken.";
const char* MSG_NICHT_BEZAHLT = "Parkgebühr nicht bezahlt. Bitte zum Automaten.";
const char* MSG_BESETZT = "Besetzt";
const char* MSG_SYSTEM_AUS = "System nicht verfügbar. Bitte Hilfeknopf drücken.";
const char* MSG_HILFE = "Hilfe wurde gerufen. Bitte warten.";

// ---------- Datentypen ----------
struct Uhr {
  bool gueltig = false;
  uint32_t basisEpoch = 0;
  uint32_t basisMillis = 0;
  int32_t tzMinuten = 0;

  uint32_t jetzt() const {
    if (!gueltig) return 0;
    return basisEpoch + (millis() - basisMillis) / 1000;
  }
  bool setzen(uint32_t epoch, int32_t tz) {
    if (epoch < 1700000000UL || tz < -840 || tz > 840) return false;
    basisEpoch = epoch;
    basisMillis = millis();
    tzMinuten = tz;
    gueltig = true;
    return true;
  }
};

struct Parkvorgang {
  String kz;
  uint32_t einfahrt;
  bool sync;  // true = Einfahrt wurde an Pico 1 übermittelt
};

struct ArchivEintrag {
  String kz;
  uint32_t einfahrt;
  uint32_t bezahlt;
  uint32_t ausfahrt;
  int32_t rappen;
};

struct LogEintrag {
  uint32_t zeit;
  String benutzer;
  String text;
};

struct Schranke {
  const char* name;
  Servo* servo;
  bool offen;
  uint32_t seit;
};

struct Display {
  String text;
  String klasse;
  uint32_t seit;
  bool aktiv;
};

// ---------- Globale Variablen ----------
WebServer server(80);
Uhr uhr;
Servo servoEin;
Servo servoAus;
Schranke schrankeEin = {"Einfahrt", &servoEin, false, 0};
Schranke schrankeAus = {"Ausfahrt", &servoAus, false, 0};
Display displayEin = {"", "", 0, false};
Display displayAus = {"", "", 0, false};

Parkvorgang offen[KAPAZITAET];
int anzahlOffen = 0;
ArchivEintrag archiv[MAX_ARCHIV];
int anzahlArchiv = 0;
LogEintrag logbuch[MAX_LOG];
int anzahlLog = 0;
String sitzungen[MAX_SITZUNGEN];
int naechsteSitzung = 0;

bool fsOk = false;
IPAddress pico1Ip;
bool pico1Bekannt = false;
uint32_t pico1Kontakt = 0;
uint32_t letzterSync = 0;

const char* CSS =
  "body{font-family:system-ui,Segoe UI,sans-serif;background:#f3f4f6;color:#111;margin:0}"
  "main{max-width:760px;margin:0 auto;padding:16px}"
  "h1{font-size:1.5rem;margin:.2em 0}h2{font-size:1.15rem;margin:.2em 0 .5em}"
  "nav a{margin-right:14px}"
  ".karte{background:#fff;border-radius:10px;padding:16px;margin:12px 0;box-shadow:0 1px 3px #0003}"
  "input,button{font-size:1.05rem;padding:9px;border-radius:6px;border:1px solid #bbb}"
  "button{background:#1d4ed8;color:#fff;border:0;cursor:pointer;margin:4px 4px 4px 0}"
  ".grau{background:#6b7280}.rot{background:#b91c1c}"
  ".display{font-family:monospace;font-size:1.1rem;padding:12px;border-radius:6px;background:#111;color:#e5e7eb;margin:8px 0;min-height:1.4em}"
  ".display.ok{color:#4ade80}.display.err{color:#f87171}"
  ".anzeige{font-size:1.7rem;font-weight:bold;text-align:center;padding:14px;border-radius:8px;background:#047857;color:#fff}"
  ".anzeige.voll{background:#b91c1c}.gross{font-size:4rem;padding:40px}"
  ".offen{color:#047857}.zu{color:#b91c1c}"
  ".msg{padding:10px;border-radius:6px;background:#dbeafe}.fehler{background:#fee2e2}"
  "table{border-collapse:collapse;width:100%;font-size:.9rem}th,td{border-bottom:1px solid #ddd;padding:5px;text-align:left}"
  "footer{color:#666;font-size:.8rem;margin-top:24px}";

// ---------- Hilfsfunktionen ----------
String html(const String& s) {
  String r;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') r += "&amp;";
    else if (c == '<') r += "&lt;";
    else if (c == '>') r += "&gt;";
    else if (c == '"') r += "&quot;";
    else if (c == '\'') r += "&#39;";
    else r += c;
  }
  return r;
}

String json(const String& s) {
  String r;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      r += '\\';
      r += c;
    } else if ((uint8_t)c < 0x20) {
      r += ' ';
    } else {
      r += c;
    }
  }
  return r;
}

String urlEncode(const String& s) {
  const char* hex = "0123456789ABCDEF";
  String r;
  for (unsigned int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      r += (char)c;
    } else {
      r += '%';
      r += hex[c >> 4];
      r += hex[c & 15];
    }
  }
  return r;
}

String wert(const String& text, const String& schluessel) {
  int start = 0;
  while (start < (int)text.length()) {
    int ende = text.indexOf('\n', start);
    if (ende < 0) ende = text.length();
    String zeile = text.substring(start, ende);
    zeile.trim();
    if (zeile.startsWith(schluessel + "=")) return zeile.substring(schluessel.length() + 1);
    start = ende + 1;
  }
  return "";
}

String feld(const String& zeile, int nummer) {
  int start = 0;
  for (int i = 0; i < nummer; i++) {
    int p = zeile.indexOf('\t', start);
    if (p < 0) return "";
    start = p + 1;
  }
  int ende = zeile.indexOf('\t', start);
  if (ende < 0) return zeile.substring(start);
  return zeile.substring(start, ende);
}

String zeitText(uint32_t epoch) {
  if (epoch == 0) return "-";
  time_t t = (time_t)epoch + (time_t)uhr.tzMinuten * 60;
  struct tm z;
  gmtime_r(&t, &z);
  char buf[32];
  snprintf(buf, sizeof(buf), "%02d.%02d.%04d %02d:%02d:%02d", z.tm_mday, z.tm_mon + 1, z.tm_year + 1900,
           z.tm_hour, z.tm_min, z.tm_sec);
  return String(buf);
}

String geld(int32_t rappen) {
  char buf[24];
  snprintf(buf, sizeof(buf), "CHF %ld.%02ld", (long)(rappen / 100), (long)(rappen % 100));
  return String(buf);
}

int freiePlaetze() {
  int frei = KAPAZITAET - anzahlOffen;
  return frei < 0 ? 0 : frei;
}

String anzeigeText() {
  if (freiePlaetze() == 0) return MSG_BESETZT;
  return "Freie Plätze: " + String(freiePlaetze());
}

int finde(const String& kz) {
  for (int i = 0; i < anzahlOffen; i++) {
    if (offen[i].kz == kz) return i;
  }
  return -1;
}

// ---------- Speicherung (LittleFS, TC-17) ----------
void speichereOffen() {
  if (!fsOk) return;
  File f = LittleFS.open("/offen.txt", "w");
  if (!f) return;
  for (int i = 0; i < anzahlOffen; i++) {
    f.print(offen[i].kz); f.print('\t');
    f.print(offen[i].einfahrt); f.print('\t');
    f.println(offen[i].sync ? 1 : 0);
  }
  f.close();
}

void speichereArchiv() {
  if (!fsOk) return;
  File f = LittleFS.open("/archiv.txt", "w");
  if (!f) return;
  for (int i = 0; i < anzahlArchiv; i++) {
    const ArchivEintrag& a = archiv[i];
    f.print(a.kz); f.print('\t');
    f.print(a.einfahrt); f.print('\t');
    f.print(a.bezahlt); f.print('\t');
    f.print(a.ausfahrt); f.print('\t');
    f.println(a.rappen);
  }
  f.close();
}

void speichereLog() {
  if (!fsOk) return;
  File f = LittleFS.open("/logbuch.txt", "w");
  if (!f) return;
  for (int i = 0; i < anzahlLog; i++) {
    f.print(logbuch[i].zeit); f.print('\t');
    f.print(logbuch[i].benutzer); f.print('\t');
    f.println(logbuch[i].text);
  }
  f.close();
}

void ladeAlles() {
  anzahlOffen = 0;
  anzahlArchiv = 0;
  anzahlLog = 0;
  if (!fsOk) return;

  File f = LittleFS.open("/offen.txt", "r");
  if (f) {
    while (f.available() && anzahlOffen < KAPAZITAET) {
      String z = f.readStringUntil('\n');
      z.trim();
      if (z.length() == 0) continue;
      offen[anzahlOffen].kz = feld(z, 0);
      offen[anzahlOffen].einfahrt = strtoul(feld(z, 1).c_str(), nullptr, 10);
      offen[anzahlOffen].sync = feld(z, 2) == "1";
      anzahlOffen++;
    }
    f.close();
  }

  f = LittleFS.open("/archiv.txt", "r");
  if (f) {
    while (f.available() && anzahlArchiv < MAX_ARCHIV) {
      String z = f.readStringUntil('\n');
      z.trim();
      if (z.length() == 0) continue;
      ArchivEintrag& a = archiv[anzahlArchiv];
      a.kz = feld(z, 0);
      a.einfahrt = strtoul(feld(z, 1).c_str(), nullptr, 10);
      a.bezahlt = strtoul(feld(z, 2).c_str(), nullptr, 10);
      a.ausfahrt = strtoul(feld(z, 3).c_str(), nullptr, 10);
      a.rappen = atol(feld(z, 4).c_str());
      anzahlArchiv++;
    }
    f.close();
  }

  f = LittleFS.open("/logbuch.txt", "r");
  if (f) {
    while (f.available() && anzahlLog < MAX_LOG) {
      String z = f.readStringUntil('\n');
      z.trim();
      if (z.length() == 0) continue;
      logbuch[anzahlLog].zeit = strtoul(feld(z, 0).c_str(), nullptr, 10);
      logbuch[anzahlLog].benutzer = feld(z, 1);
      logbuch[anzahlLog].text = feld(z, 2);
      anzahlLog++;
    }
    f.close();
  }
}

void archiviere(const Parkvorgang& p, uint32_t bezahlt, int32_t rappen) {
  if (anzahlArchiv >= MAX_ARCHIV) {
    for (int i = 0; i < MAX_ARCHIV - 1; i++) archiv[i] = archiv[i + 1];
    anzahlArchiv--;
  }
  ArchivEintrag& a = archiv[anzahlArchiv++];
  a.kz = p.kz;
  a.einfahrt = p.einfahrt;
  a.bezahlt = bezahlt;
  a.ausfahrt = uhr.jetzt();
  a.rappen = rappen;
  speichereArchiv();
}

void protokolliere(const String& benutzer, const String& text) {
  if (anzahlLog >= MAX_LOG) {
    for (int i = 0; i < MAX_LOG - 1; i++) logbuch[i] = logbuch[i + 1];
    anzahlLog--;
  }
  logbuch[anzahlLog].zeit = uhr.jetzt();
  logbuch[anzahlLog].benutzer = benutzer;
  logbuch[anzahlLog].text = text;
  anzahlLog++;
  speichereLog();
  Serial.println("Logbuch: " + benutzer + " – " + text);
}

// ---------- Schranken und Display ----------
void oeffne(Schranke& s) {
  s.servo->write(WINKEL_OFFEN);
  s.offen = true;
  s.seit = millis();
  Serial.printf("Schranke %s offen\n", s.name);
}

void schrankenAktualisieren() {
  Schranke* alle[] = {&schrankeEin, &schrankeAus};
  for (Schranke* s : alle) {
    if (s->offen && millis() - s->seit >= OFFEN_DAUER_MS) {
      s->servo->write(WINKEL_ZU);
      s->offen = false;
      Serial.printf("Schranke %s geschlossen\n", s->name);
    }
  }
  // LED nur bei Änderung schalten (beim Pico W läuft die LED über den WLAN-Chip)
  static bool ledAn = false;
  bool sollAn = schrankeEin.offen || schrankeAus.offen;
  if (sollAn != ledAn) {
    digitalWrite(LED_BUILTIN, sollAn ? HIGH : LOW);
    ledAn = sollAn;
  }
}

void zeige(Display& d, const String& text, const char* klasse) {
  d.text = text;
  d.klasse = klasse;
  d.seit = millis();
  d.aktiv = true;
}

bool meldungAktiv(const Display& d) {
  return d.aktiv && millis() - d.seit < MELDUNG_DAUER_MS;
}

String displayText(const Display& d, bool istEinfahrt) {
  if (meldungAktiv(d)) return d.text;
  if (istEinfahrt && freiePlaetze() == 0) return MSG_BESETZT;
  if (istEinfahrt) return "Bitte vorfahren. Kennzeichen wird erfasst.";
  return "Bitte vorfahren. Gute Fahrt!";
}

String displayKlasse(const Display& d, bool istEinfahrt) {
  if (meldungAktiv(d)) return d.klasse;
  if (istEinfahrt && freiePlaetze() == 0) return "err";
  return "";
}

String schrankenText(const Schranke& s) {
  if (s.offen) return "Schranke offen";
  return "Schranke geschlossen";
}

// ---------- Kommunikation mit Pico 1 ----------
bool pico1Online() {
  return pico1Bekannt && millis() - pico1Kontakt < PICO1_ONLINE_MS;
}

bool pico1Anfrage(const String& pfad, String& antwort) {
  if (!pico1Bekannt) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  String url = "http://" + pico1Ip.toString() + pfad;
  url += (pfad.indexOf('?') < 0) ? "?" : "&";
  url += "key=" + String(API_KEY);
  if (uhr.gueltig) url += "&t=" + String(uhr.jetzt()) + "&tz=" + String(uhr.tzMinuten);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = (code == 200);
  if (ok) {
    antwort = http.getString();
    pico1Kontakt = millis();
  }
  http.end();
  return ok;
}

// Einfahrten, die Pico 1 noch nicht kennt, nachliefern (z.B. nach WLAN-Unterbruch)
void synchronisiere() {
  if (!pico1Online() || millis() - letzterSync < SYNC_INTERVALL_MS) return;
  letzterSync = millis();
  for (int i = 0; i < anzahlOffen; i++) {
    if (offen[i].sync) continue;
    String antwort;
    String pfad = "/api/einfahrt?kz=" + urlEncode(offen[i].kz) + "&einfahrt=" + String(offen[i].einfahrt);
    if (pico1Anfrage(pfad, antwort) && antwort.startsWith("ok")) {
      offen[i].sync = true;
      speichereOffen();
    }
    return;  // höchstens eine Anfrage pro Durchlauf, damit die Weboberfläche flüssig bleibt
  }
}

// ---------- Admin-Login ----------
bool eingeloggt() {
  String cookie = server.header("Cookie");
  int p = cookie.indexOf("sid=");
  if (p < 0) return false;
  String token = cookie.substring(p + 4);
  int ende = token.indexOf(';');
  if (ende >= 0) token = token.substring(0, ende);
  token.trim();
  if (token.length() < 16) return false;
  for (int i = 0; i < MAX_SITZUNGEN; i++) {
    if (sitzungen[i] == token) return true;
  }
  return false;
}

bool loginNoetig() {
  if (eingeloggt()) return false;
  server.sendHeader("Location", "/admin/login");
  server.send(302, "text/plain", "");
  return true;
}

// ---------- HTML-Bausteine ----------
String kopf(const String& titel) {
  String h = "<!DOCTYPE html><html lang='de'><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'><title>";
  h += titel;
  h += "</title><style>";
  h += CSS;
  h += "</style></head><body><main>";
  return h;
}

String navigation() {
  String h = "<nav><a href='/'>Kamera-Simulation</a><a href='/anzeige'>Anzeige</a><a href='/admin'>Admin-Panel</a>";
  if (pico1Bekannt) {
    h += "<a href='http://" + pico1Ip.toString() + "/'>Kasse (Pico 1)</a>";
  }
  h += "</nav>";
  return h;
}

String fuss() {
  String h = "<footer>Pico 2 – Schranken · Firmware ";
  h += FIRMWARE_VERSION;
  h += " · Uhrzeit ";
  h += uhr.gueltig ? zeitText(uhr.jetzt()) : String("nicht gesetzt");
  h += " · Pico 1: ";
  if (pico1Online()) h += "verbunden (" + pico1Ip.toString() + ")";
  else h += "nicht erreichbar";
  h += "</footer>";
  if (!uhr.gueltig) {
    // Ohne Internet hat der Pico keine Uhrzeit: die Uhrzeit des Browsers übernehmen
    h += "<script>fetch('/api/settime?t='+Math.floor(Date.now()/1000)+'&tz='+(-new Date().getTimezoneOffset()))"
         ".then(function(r){return r.text()}).then(function(t){if(t=='ok')location.reload()});</script>";
  }
  h += "</main></body></html>";
  return h;
}

// Aktualisiert Anzeige, Displays und Schrankenstatus jede Sekunde
const char* STATUS_SKRIPT = R"(<script>
function setze(id,text,klasse){var e=document.getElementById(id);if(!e)return;e.textContent=text;if(klasse!==undefined)e.className=klasse;}
function aktualisiere(){fetch('/api/state').then(function(r){return r.json()}).then(function(s){
setze('anzeige',s.voll?'Besetzt':'Freie Plätze: '+s.frei,'anzeige'+(s.voll?' voll':'')+(document.body.dataset.gross?' gross':''));
setze('dispEin',s.dispEin,'display '+s.klEin);
setze('dispAus',s.dispAus,'display '+s.klAus);
setze('schEin',s.ein?'Schranke offen':'Schranke geschlossen',s.ein?'offen':'zu');
setze('schAus',s.aus?'Schranke offen':'Schranke geschlossen',s.aus?'offen':'zu');
}).catch(function(){});}
setInterval(aktualisiere,1000);
</script>)";

void sendeSeite(const String& h) {
  server.send(200, "text/html; charset=utf-8", h);
}

void weiterleiten(const String& ziel) {
  server.sendHeader("Location", ziel);
  server.send(303, "text/plain", "");
}

String spurHtml(const char* titel, const char* aktion, const char* spur, const char* dispId, const char* schId,
                const Display& d, const Schranke& s, bool istEinfahrt) {
  String h = "<section class='karte'><h2>";
  h += titel;
  h += "</h2><div id='";
  h += dispId;
  h += "' class='display " + displayKlasse(d, istEinfahrt) + "'>" + html(displayText(d, istEinfahrt)) + "</div>";
  h += "<p><b id='";
  h += schId;
  h += "' class='";
  h += s.offen ? "offen" : "zu";
  h += "'>" + schrankenText(s) + "</b></p>";
  h += "<form method='post' action='";
  h += aktion;
  h += "' accept-charset='UTF-8'><input name='kz' placeholder='z.B. ZH 123456' autocomplete='off'> "
       "<button>Bestätigen</button></form>";
  h += "<form method='post' action='/hilfe?spur=";
  h += spur;
  h += "'><button class='grau'>Hilfeknopf</button></form></section>";
  return h;
}

// ---------- Seiten ----------
void seiteKamera() {
  String h = kopf("Kamera-Simulation");
  h += "<h1>Parkhaus – Kamera-Simulation</h1>";
  h += navigation();
  String klasse = freiePlaetze() == 0 ? "anzeige voll" : "anzeige";
  h += "<div class='karte'><div id='anzeige' class='" + klasse + "'>" + anzeigeText() + "</div></div>";
  h += spurHtml("Einfahrt", "/einfahrt", "einfahrt", "dispEin", "schEin", displayEin, schrankeEin, true);
  h += spurHtml("Ausfahrt", "/ausfahrt", "ausfahrt", "dispAus", "schAus", displayAus, schrankeAus, false);
  h += STATUS_SKRIPT;
  h += fuss();
  sendeSeite(h);
}

void seiteAnzeige() {
  String h = kopf("Anzeige");
  h += "<script>document.body.dataset.gross='1'</script>";
  h += navigation();
  String klasse = freiePlaetze() == 0 ? "anzeige gross voll" : "anzeige gross";
  h += "<div class='karte'><div id='anzeige' class='" + klasse + "'>" + anzeigeText() + "</div></div>";
  h += STATUS_SKRIPT;
  h += fuss();
  sendeSeite(h);
}

void aktionEinfahrt() {
  String kz = normalisiereKennzeichen(server.arg("kz"));
  if (!kennzeichenGueltig(kz)) {
    zeige(displayEin, MSG_UNLESBAR, "err");
  } else if (freiePlaetze() == 0) {
    zeige(displayEin, MSG_BESETZT, "err");
  } else if (finde(kz) >= 0) {
    zeige(displayEin, "Fahrzeug " + kz + " ist bereits im Parkhaus.", "err");
  } else if (!uhr.gueltig) {
    zeige(displayEin, "Uhrzeit nicht gesetzt. Bitte Seite neu laden.", "err");
  } else {
    Parkvorgang& p = offen[anzahlOffen++];
    p.kz = kz;
    p.einfahrt = uhr.jetzt();
    p.sync = false;
    speichereOffen();  // erst speichern, dann öffnen (US 1)
    oeffne(schrankeEin);
    zeige(displayEin, "Willkommen " + kz + ". Schranke offen.", "ok");
    letzterSync = millis() - SYNC_INTERVALL_MS;  // sofort an Pico 1 melden
  }
  weiterleiten("/");
}

void aktionAusfahrt() {
  String kz = normalisiereKennzeichen(server.arg("kz"));
  int i = kennzeichenGueltig(kz) ? finde(kz) : -1;
  if (i < 0) {
    zeige(displayAus, MSG_UNLESBAR, "err");
    weiterleiten("/");
    return;
  }

  String antwort;
  if (!pico1Online() || !pico1Anfrage("/api/status?kz=" + urlEncode(kz), antwort)) {
    // Ohne Antwort der Kasse bleibt die Schranke zu (TC-13)
    zeige(displayAus, MSG_SYSTEM_AUS, "err");
    weiterleiten("/");
    return;
  }

  if (wert(antwort, "status") == "bezahlt") {
    Parkvorgang p = offen[i];
    for (int k = i; k < anzahlOffen - 1; k++) offen[k] = offen[k + 1];
    anzahlOffen--;
    speichereOffen();
    archiviere(p, strtoul(wert(antwort, "bezahlt").c_str(), nullptr, 10), atol(wert(antwort, "rappen").c_str()));
    oeffne(schrankeAus);
    zeige(displayAus, "Gute Fahrt! Schranke offen.", "ok");
    String egal;
    pico1Anfrage("/api/ausfahrt?kz=" + urlEncode(kz), egal);
  } else {
    zeige(displayAus, MSG_NICHT_BEZAHLT, "err");
  }
  weiterleiten("/");
}

void aktionHilfe() {
  bool einfahrtSpur = server.arg("spur") == "einfahrt";
  protokolliere("Gast", einfahrtSpur ? "Hilfeknopf gedrückt (Einfahrt)" : "Hilfeknopf gedrückt (Ausfahrt)");
  zeige(einfahrtSpur ? displayEin : displayAus, MSG_HILFE, "");
  weiterleiten("/");
}

// ---------- Admin-Panel ----------
void seiteLogin(const String& meldung) {
  String h = kopf("Admin-Login");
  h += "<h1>Admin-Panel – Anmeldung</h1>";
  h += navigation();
  h += "<div class='karte'>";
  if (meldung.length() > 0) h += "<p class='msg fehler'>" + html(meldung) + "</p>";
  h += "<form method='post' action='/admin/login'>"
       "<p><label>Benutzer<br><input name='benutzer' autocomplete='username'></label></p>"
       "<p><label>Passwort<br><input name='passwort' type='password' autocomplete='current-password'></label></p>"
       "<button>Anmelden</button></form></div>";
  h += fuss();
  sendeSeite(h);
}

void aktionLogin() {
  if (server.method() != HTTP_POST) {
    if (eingeloggt()) weiterleiten("/admin");
    else seiteLogin("");
    return;
  }
  if (server.arg("benutzer") == ADMIN_BENUTZER && server.arg("passwort") == ADMIN_PASSWORT) {
    String token = String(rp2040.hwrand32(), HEX) + String(rp2040.hwrand32(), HEX) + String(rp2040.hwrand32(), HEX);
    sitzungen[naechsteSitzung] = token;
    naechsteSitzung = (naechsteSitzung + 1) % MAX_SITZUNGEN;
    server.sendHeader("Set-Cookie", "sid=" + token + "; Path=/; HttpOnly; SameSite=Strict");
    weiterleiten("/admin");
    return;
  }
  Serial.println("Anmeldung fehlgeschlagen");
  seiteLogin("Anmeldung fehlgeschlagen");
}

void aktionLogout() {
  String cookie = server.header("Cookie");
  for (int i = 0; i < MAX_SITZUNGEN; i++) {
    if (sitzungen[i].length() > 0 && cookie.indexOf(sitzungen[i]) >= 0) sitzungen[i] = "";
  }
  server.sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0");
  weiterleiten("/admin/login");
}

void seiteAdmin() {
  if (loginNoetig()) return;
  String h = kopf("Admin-Panel");
  h += "<h1>Admin-Panel</h1>";
  h += navigation();

  String msg = server.arg("msg");
  if (msg == "einfahrt") h += "<p class='msg'>Einfahrtsschranke manuell geöffnet.</p>";
  else if (msg == "ausfahrt") h += "<p class='msg'>Ausfahrtsschranke manuell geöffnet.</p>";
  else if (msg == "reset") h += "<p class='msg'>System zurückgesetzt.</p>";
  else if (msg == "reset-teilweise")
    h += "<p class='msg fehler'>System zurückgesetzt, aber die Kasse (Pico 1) war nicht erreichbar.</p>";

  h += "<section class='karte'><h2>Status</h2><div id='anzeige' class='";
  h += freiePlaetze() == 0 ? "anzeige voll" : "anzeige";
  h += "'>" + anzeigeText() + "</div>";
  h += "<p>Angemeldet als <b>" + String(ADMIN_BENUTZER) + "</b> · <a href='/admin/logout'>Abmelden</a></p>";
  h += "<p>Uhrzeit: " + (uhr.gueltig ? zeitText(uhr.jetzt()) : String("nicht gesetzt")) + "<br>Kasse (Pico 1): ";
  if (pico1Online()) h += "verbunden, " + pico1Ip.toString();
  else h += "nicht erreichbar";
  h += "</p></section>";

  h += "<section class='karte'><h2>Schranken manuell steuern</h2>";
  h += "<p>Einfahrt: <b id='schEin' class='" + String(schrankeEin.offen ? "offen" : "zu") + "'>" +
       schrankenText(schrankeEin) + "</b> · Ausfahrt: <b id='schAus' class='" +
       String(schrankeAus.offen ? "offen" : "zu") + "'>" + schrankenText(schrankeAus) + "</b></p>";
  h += "<form method='post' action='/admin/oeffnen?schranke=einfahrt' style='display:inline'>"
       "<button>Einfahrt manuell öffnen</button></form>";
  h += "<form method='post' action='/admin/oeffnen?schranke=ausfahrt' style='display:inline'>"
       "<button>Ausfahrt manuell öffnen</button></form></section>";

  h += "<section class='karte'><h2>Offene Parkvorgänge (" + String(anzahlOffen) + ")</h2><table>"
       "<tr><th>Kennzeichen</th><th>Einfahrt</th><th>An Kasse übermittelt</th></tr>";
  for (int i = 0; i < anzahlOffen; i++) {
    h += "<tr><td>" + html(offen[i].kz) + "</td><td>" + zeitText(offen[i].einfahrt) + "</td><td>";
    h += offen[i].sync ? "ja" : "nein";
    h += "</td></tr>";
  }
  h += "</table></section>";

  h += "<section class='karte'><h2>Archiv</h2><table>"
       "<tr><th>Kennzeichen</th><th>Einfahrt</th><th>Zahlung</th><th>Ausfahrt</th><th>Betrag</th></tr>";
  for (int i = anzahlArchiv - 1; i >= 0; i--) {
    const ArchivEintrag& a = archiv[i];
    h += "<tr><td>" + html(a.kz) + "</td><td>" + zeitText(a.einfahrt) + "</td><td>" + zeitText(a.bezahlt) +
         "</td><td>" + zeitText(a.ausfahrt) + "</td><td>" + geld(a.rappen) + "</td></tr>";
  }
  h += "</table></section>";

  h += "<section class='karte'><h2>Logbuch</h2><table><tr><th>Uhrzeit</th><th>Benutzer</th><th>Ereignis</th></tr>";
  for (int i = anzahlLog - 1; i >= 0; i--) {
    h += "<tr><td>" + zeitText(logbuch[i].zeit) + "</td><td>" + html(logbuch[i].benutzer) + "</td><td>" +
         html(logbuch[i].text) + "</td></tr>";
  }
  h += "</table></section>";

  h += "<section class='karte'><h2>System zurücksetzen</h2>"
       "<p>Löscht alle offenen Parkvorgänge und das Archiv, setzt die Anzeige auf " + String(KAPAZITAET) +
       " freie Plätze und archiviert das Logbuch.</p>"
       "<form method='post' action='/admin/reset' onsubmit=\"return confirm('System wirklich zurücksetzen?')\">"
       "<button class='rot'>System zurücksetzen</button></form></section>";
  h += STATUS_SKRIPT;
  h += fuss();
  sendeSeite(h);
}

void aktionManuellOeffnen() {
  if (loginNoetig()) return;
  String schranke = server.arg("schranke");
  if (schranke == "einfahrt") {
    oeffne(schrankeEin);  // auch bei «Besetzt», z.B. für Rettungsfahrzeuge (TC-09)
    protokolliere(ADMIN_BENUTZER, "manuelle Öffnung Einfahrt");
    weiterleiten("/admin?msg=einfahrt");
  } else if (schranke == "ausfahrt") {
    oeffne(schrankeAus);
    protokolliere(ADMIN_BENUTZER, "manuelle Öffnung Ausfahrt");
    weiterleiten("/admin?msg=ausfahrt");
  } else {
    weiterleiten("/admin");
  }
}

void aktionReset() {
  if (loginNoetig()) return;
  if (server.method() != HTTP_POST) {
    weiterleiten("/admin");
    return;
  }
  anzahlOffen = 0;
  speichereOffen();
  anzahlArchiv = 0;
  speichereArchiv();
  if (fsOk) {
    LittleFS.remove("/logbuch_alt.txt");
    LittleFS.rename("/logbuch.txt", "/logbuch_alt.txt");
  }
  anzahlLog = 0;
  displayEin.aktiv = false;
  displayAus.aktiv = false;
  protokolliere(ADMIN_BENUTZER, "System zurückgesetzt");
  String antwort;
  bool kasseOk = pico1Anfrage("/api/reset", antwort);
  weiterleiten(kasseOk ? "/admin?msg=reset" : "/admin?msg=reset-teilweise");
}

// ---------- API ----------
void apiState() {
  String j = "{\"frei\":" + String(freiePlaetze());
  j += ",\"voll\":";
  j += freiePlaetze() == 0 ? "true" : "false";
  j += ",\"ein\":";
  j += schrankeEin.offen ? "true" : "false";
  j += ",\"aus\":";
  j += schrankeAus.offen ? "true" : "false";
  j += ",\"dispEin\":\"" + json(displayText(displayEin, true)) + "\"";
  j += ",\"klEin\":\"" + displayKlasse(displayEin, true) + "\"";
  j += ",\"dispAus\":\"" + json(displayText(displayAus, false)) + "\"";
  j += ",\"klAus\":\"" + displayKlasse(displayAus, false) + "\"}";
  server.send(200, "application/json; charset=utf-8", j);
}

void apiSetTime() {
  if (uhr.gueltig) {
    server.send(200, "text/plain", "bereits");
    return;
  }
  bool ok = uhr.setzen(strtoul(server.arg("t").c_str(), nullptr, 10), atol(server.arg("tz").c_str()));
  if (ok) Serial.println("Uhrzeit vom Browser übernommen: " + zeitText(uhr.jetzt()));
  server.send(200, "text/plain", ok ? "ok" : "ungueltig");
}

// Pico 1 meldet sich: IP merken und Uhrzeit abgleichen
void apiHello() {
  if (server.arg("key") != API_KEY) {
    server.send(403, "text/plain", "fehler=key\n");
    return;
  }
  IPAddress ip = server.client().remoteIP();
  if (!pico1Bekannt || ip != pico1Ip) {
    Serial.println("Pico 1 (Kasse) gefunden: " + ip.toString());
  }
  pico1Ip = ip;
  pico1Bekannt = true;
  pico1Kontakt = millis();
  // Nach einem Neustart von Pico 2 die Uhrzeit von Pico 1 zurückholen
  if (!uhr.gueltig && server.arg("ok") == "1") {
    uhr.setzen(strtoul(server.arg("t").c_str(), nullptr, 10), atol(server.arg("tz").c_str()));
  }
  String a = "ok=" + String(uhr.gueltig ? "1" : "0") + "\nt=" + String(uhr.jetzt()) + "\ntz=" + String(uhr.tzMinuten) + "\n";
  server.send(200, "text/plain", a);
}

// ---------- Setup und Loop ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Parksystem Pico 2 (Schranken) startet");

  pinMode(LED_BUILTIN, OUTPUT);
  servoEin.attach(PIN_SERVO_EINFAHRT, 500, 2400);
  servoAus.attach(PIN_SERVO_AUSFAHRT, 500, 2400);
  servoEin.write(WINKEL_ZU);
  servoAus.write(WINKEL_ZU);

  fsOk = LittleFS.begin();
  if (!fsOk) Serial.println("WARNUNG: LittleFS nicht verfügbar (Flash Size mit FS wählen). Daten nur im RAM.");
  ladeAlles();
  Serial.printf("%d offene Parkvorgänge geladen\n", anzahlOffen);

  WiFi.beginAP(WLAN_SSID, WLAN_PASSWORT);
  Serial.print("Access Point ");
  Serial.print(WLAN_SSID);
  Serial.println(" bereit, Kamera-Simulation unter http://192.168.42.1/");

  const char* kopfzeilen[] = {"Cookie"};
  server.collectHeaders(kopfzeilen, 1);

  server.on("/", seiteKamera);
  server.on("/anzeige", seiteAnzeige);
  server.on("/einfahrt", HTTP_POST, aktionEinfahrt);
  server.on("/ausfahrt", HTTP_POST, aktionAusfahrt);
  server.on("/hilfe", HTTP_POST, aktionHilfe);
  server.on("/admin", seiteAdmin);
  server.on("/admin/login", aktionLogin);
  server.on("/admin/logout", aktionLogout);
  server.on("/admin/oeffnen", aktionManuellOeffnen);
  server.on("/admin/reset", aktionReset);
  server.on("/api/state", apiState);
  server.on("/api/settime", apiSetTime);
  server.on("/api/hello", apiHello);
  server.onNotFound([]() { server.send(404, "text/plain", "Nicht gefunden"); });
  server.begin();
}

void loop() {
  server.handleClient();
  schrankenAktualisieren();
  synchronisiere();
}
