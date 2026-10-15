/*
  Parksystem M450 – Pico 1 (Kasse)
  Atieh Rostamian & Sabina Lazri

  Aufgaben (US 2):
    - Kassen-Weboberfläche: Kennzeichen eingeben, Betrag anzeigen, bezahlen (simuliert)
    - Tarif CHF 1.50 pro angebrochene 15 Minuten, Karenzzeit 15 Minuten
    - Quittung optional
  Schnittstelle zu Pico 2 (Schranken):
    - Pico 2 meldet jede Einfahrt        GET /api/einfahrt?kz=..&einfahrt=..
    - Pico 2 fragt bei der Ausfahrt      GET /api/status?kz=..
    - Pico 2 meldet die Ausfahrt         GET /api/ausfahrt?kz=..
    - Reset aus dem Admin-Panel          GET /api/reset
    - Ausfahrtsschranke öffnen           GET /api/schranke
  Der Servo der Ausfahrtsschranke hängt an Pico 1 (Signal GP8 = Pin 11).
  Pico 1 entscheidet nicht selbst, wann er öffnet: das macht Pico 2.
  Pico 1 verbindet sich mit dem WLAN, das Pico 2 aufspannt, und meldet sich
  alle 5 Sekunden bei Pico 2 (/api/hello). Dabei wird die Uhrzeit abgeglichen.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Servo.h>
#include <time.h>
#include "tarif.h"
#include "kennzeichen.h"

// ---------- Konfiguration ----------
const char* WLAN_SSID = "Parksystem-TEST";
const char* WLAN_PASSWORT = "parken2026";
const char* API_KEY = "m450-parksystem";
const char* PICO2_ADRESSE = "192.168.42.1";  // Standard-IP des Access Points von Pico 2
const char* FIRMWARE_VERSION = "1.0.0";
const uint32_t HELLO_INTERVALL_MS = 5000;
const uint32_t WLAN_NEUVERSUCH_MS = 10000;
const uint16_t HTTP_TIMEOUT_MS = 1500;
const int MAX_VORGAENGE = 30;
const int PIN_SERVO_AUSFAHRT = 8;  // GP8 = Pin 11
// Schranke zu = 90° (Mitte), offen = 0°. Dreht sie falsch herum, WINKEL_OFFEN auf 180 setzen.
const int WINKEL_ZU = 90;
const int WINKEL_OFFEN = 0;
const uint32_t OFFEN_DAUER_MS = 5000;

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

struct Vorgang {
  String kz;
  uint32_t einfahrt;
  uint32_t bezahltUm;     // 0 = noch nicht bezahlt
  int32_t bezahltRappen;  // Summe aller Zahlungen
  String zahlart;
};

// ---------- Globale Variablen ----------
WebServer server(80);
Uhr uhr;
Vorgang vorgaenge[MAX_VORGAENGE];
int anzahlVorgaenge = 0;
bool fsOk = false;
bool pico2Online = false;
uint32_t letzterHello = 0;
bool helloGesendet = false;
uint32_t letzterWlanVersuch = 0;
bool wlanWarVerbunden = false;
Servo servoAusfahrt;
bool ausfahrtOffen = false;
uint32_t ausfahrtSeit = 0;

const char* CSS =
  "body{font-family:system-ui,Segoe UI,sans-serif;background:#f3f4f6;color:#111;margin:0}"
  "main{max-width:640px;margin:0 auto;padding:16px}"
  "h1{font-size:1.5rem;margin:.2em 0}"
  ".karte{background:#fff;border-radius:10px;padding:16px;margin:12px 0;box-shadow:0 1px 3px #0003}"
  "input,button{font-size:1.1rem;padding:10px;border-radius:6px;border:1px solid #bbb}"
  "input{width:60%}"
  "button,.knopf{background:#1d4ed8;color:#fff;border:0;cursor:pointer;text-decoration:none;display:inline-block;padding:10px 16px;border-radius:6px;margin:4px 4px 4px 0}"
  ".grau{background:#6b7280}.gruen{background:#047857}"
  ".betrag{font-size:2rem;font-weight:bold}"
  ".msg{padding:10px;border-radius:6px;background:#dbeafe}.fehler{background:#fee2e2}.ok{background:#d1fae5}"
  "table{border-collapse:collapse;width:100%}td{padding:4px 0}td:first-child{color:#555;width:45%}"
  ".quittung{font-family:monospace;border:1px dashed #555;padding:16px;background:#fffef5}"
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

String zeitText(uint32_t epoch, bool mitDatum) {
  if (epoch == 0) return "-";
  time_t t = (time_t)epoch + (time_t)uhr.tzMinuten * 60;
  struct tm z;
  gmtime_r(&t, &z);
  char buf[32];
  if (mitDatum) {
    snprintf(buf, sizeof(buf), "%02d.%02d.%04d %02d:%02d:%02d", z.tm_mday, z.tm_mon + 1, z.tm_year + 1900,
             z.tm_hour, z.tm_min, z.tm_sec);
  } else {
    snprintf(buf, sizeof(buf), "%02d:%02d", z.tm_hour, z.tm_min);
  }
  return String(buf);
}

String datumText(uint32_t epoch) {
  if (epoch == 0) return "-";
  time_t t = (time_t)epoch + (time_t)uhr.tzMinuten * 60;
  struct tm z;
  gmtime_r(&t, &z);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d.%02d.%04d", z.tm_mday, z.tm_mon + 1, z.tm_year + 1900);
  return String(buf);
}

String dauerText(uint32_t sekunden) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%lu Min. %02lu Sek.", (unsigned long)(sekunden / 60), (unsigned long)(sekunden % 60));
  return String(buf);
}

String geld(int32_t rappen) {
  char buf[24];
  snprintf(buf, sizeof(buf), "CHF %ld.%02ld", (long)(rappen / 100), (long)(rappen % 100));
  return String(buf);
}

int finde(const String& kz) {
  for (int i = 0; i < anzahlVorgaenge; i++) {
    if (vorgaenge[i].kz == kz) return i;
  }
  return -1;
}

void entferne(int index) {
  for (int i = index; i < anzahlVorgaenge - 1; i++) vorgaenge[i] = vorgaenge[i + 1];
  anzahlVorgaenge--;
}

// ---------- Speicherung (LittleFS, damit nach einem Neustart nichts verloren geht) ----------
void speichere() {
  if (!fsOk) return;
  File f = LittleFS.open("/vorgaenge.txt", "w");
  if (!f) return;
  for (int i = 0; i < anzahlVorgaenge; i++) {
    const Vorgang& v = vorgaenge[i];
    f.print(v.kz); f.print('\t');
    f.print(v.einfahrt); f.print('\t');
    f.print(v.bezahltUm); f.print('\t');
    f.print(v.bezahltRappen); f.print('\t');
    f.println(v.zahlart);
  }
  f.close();
}

void lade() {
  anzahlVorgaenge = 0;
  if (!fsOk) return;
  File f = LittleFS.open("/vorgaenge.txt", "r");
  if (!f) return;
  while (f.available() && anzahlVorgaenge < MAX_VORGAENGE) {
    String z = f.readStringUntil('\n');
    z.trim();
    if (z.length() == 0) continue;
    Vorgang& v = vorgaenge[anzahlVorgaenge];
    v.kz = feld(z, 0);
    v.einfahrt = strtoul(feld(z, 1).c_str(), nullptr, 10);
    v.bezahltUm = strtoul(feld(z, 2).c_str(), nullptr, 10);
    v.bezahltRappen = atol(feld(z, 3).c_str());
    v.zahlart = feld(z, 4);
    anzahlVorgaenge++;
  }
  f.close();
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

String fuss() {
  String h = "<footer>Pico 1 – Kasse · Firmware ";
  h += FIRMWARE_VERSION;
  h += " · Uhrzeit ";
  h += uhr.gueltig ? zeitText(uhr.jetzt(), true) : String("nicht gesetzt");
  h += " · Pico 2: ";
  h += pico2Online ? "verbunden" : "nicht erreichbar";
  h += "</footer>";
  if (!uhr.gueltig) {
    // Ohne Internet hat der Pico keine Uhrzeit. Fallback: Uhrzeit vom Browser übernehmen.
    h += "<script>fetch('/api/settime?t='+Math.floor(Date.now()/1000)+'&tz='+(-new Date().getTimezoneOffset()))"
         ".then(function(r){return r.text()}).then(function(t){if(t=='ok')location.reload()});</script>";
  }
  h += "</main></body></html>";
  return h;
}

void sendeSeite(const String& h) {
  server.send(200, "text/html; charset=utf-8", h);
}

void weiterleiten(const String& ziel) {
  server.sendHeader("Location", ziel);
  server.send(303, "text/plain", "");
}

String fehlerSeite(const String& text) {
  String h = kopf("Kasse");
  h += "<h1>Kassenautomat</h1><div class='karte'><p class='msg fehler'>";
  h += html(text);
  h += "</p><a class='knopf grau' href='/'>Zurück</a></div>";
  h += fuss();
  return h;
}

// ---------- Seiten der Kasse ----------
void seiteStart() {
  String h = kopf("Kasse");
  h += "<h1>Kassenautomat</h1>";
  if (server.arg("msg") == "danke") {
    h += "<p class='msg ok'>Vielen Dank! Gute Fahrt.</p>";
  }
  h += "<div class='karte'><form method='get' action='/betrag' accept-charset='UTF-8'>"
       "<p><label for='kz'>Bitte Kennzeichen eingeben:</label></p>"
       "<input id='kz' name='kz' placeholder='z.B. ZH 123456' autocomplete='off' autofocus> "
       "<button>Weiter</button></form></div>";
  h += fuss();
  sendeSeite(h);
}

void seiteBetrag() {
  String kz = normalisiereKennzeichen(server.arg("kz"));
  if (kz.length() == 0) {
    weiterleiten("/");
    return;
  }
  int i = finde(kz);
  if (i < 0) {
    sendeSeite(fehlerSeite("Kein Parkvorgang für das Kennzeichen «" + kz + "» gefunden."));
    return;
  }
  if (!uhr.gueltig) {
    sendeSeite(fehlerSeite("Die Uhrzeit ist noch nicht synchronisiert. Bitte Seite neu laden."));
    return;
  }
  const Vorgang& v = vorgaenge[i];
  uint32_t jetzt = uhr.jetzt();
  int32_t offen = tarif::offenerBetrag(v.einfahrt, v.bezahltUm, v.bezahltRappen, jetzt);

  String h = kopf("Kasse");
  h += "<h1>Kassenautomat</h1><div class='karte'><table>";
  h += "<tr><td>Kennzeichen</td><td><b>" + html(v.kz) + "</b></td></tr>";
  h += "<tr><td>Einfahrt</td><td>" + zeitText(v.einfahrt, true) + "</td></tr>";
  h += "<tr><td>Parkdauer</td><td>" + dauerText(jetzt >= v.einfahrt ? jetzt - v.einfahrt : 0) + "</td></tr>";
  h += "</table>";

  if (offen == 0) {
    h += "<p class='msg ok'>Bezahlt, Ausfahrt bis " + zeitText(v.bezahltUm + tarif::KARENZ_SEKUNDEN, false) + "</p>";
    h += "<a class='knopf' href='/quittung?kz=" + urlEncode(v.kz) + "'>Quittung anzeigen</a>";
    h += "<a class='knopf grau' href='/'>Fertig</a></div>";
    h += fuss();
    sendeSeite(h);
    return;
  }

  if (server.arg("neu") == "1") {
    h += "<p class='msg fehler'>Der Betrag hat sich inzwischen geändert. Bitte neuen Betrag prüfen.</p>";
  }
  if (v.bezahltUm != 0) {
    h += "<p class='msg fehler'>Die Karenzzeit von 15 Minuten ist abgelaufen. Bitte Nachzahlung leisten.</p>";
  }
  h += "<p>Zu bezahlen:</p><p class='betrag'>" + geld(offen) + "</p>";
  h += "<p style='color:#555'>Tarif: CHF 1.50 pro angebrochene 15 Minuten</p>";
  h += "<form method='post' action='/bezahlen' accept-charset='UTF-8'>";
  h += "<input type='hidden' name='kz' value='" + html(v.kz) + "'>";
  h += "<input type='hidden' name='betrag' value='" + String(offen) + "'>";
  h += "<p>Zahlungsmittel wählen:</p>";
  h += "<button name='zahlart' value='Karte (simuliert)'>Karte (simuliert)</button>";
  h += "<button name='zahlart' value='TWINT (simuliert)'>TWINT (simuliert)</button>";
  h += "<button name='zahlart' value='Bargeld (simuliert)'>Bargeld (simuliert)</button>";
  h += "</form><a class='knopf grau' href='/'>Abbrechen</a></div>";
  h += fuss();
  sendeSeite(h);
}

void aktionBezahlen() {
  String kz = normalisiereKennzeichen(server.arg("kz"));
  int i = finde(kz);
  if (i < 0) {
    sendeSeite(fehlerSeite("Kein Parkvorgang für das Kennzeichen «" + kz + "» gefunden."));
    return;
  }
  if (!uhr.gueltig) {
    sendeSeite(fehlerSeite("Die Uhrzeit ist noch nicht synchronisiert. Bitte Seite neu laden."));
    return;
  }
  Vorgang& v = vorgaenge[i];
  uint32_t jetzt = uhr.jetzt();
  int32_t offen = tarif::offenerBetrag(v.einfahrt, v.bezahltUm, v.bezahltRappen, jetzt);
  if (offen == 0) {
    weiterleiten("/bezahlt?kz=" + urlEncode(v.kz));
    return;
  }
  if (server.arg("betrag") != String(offen)) {
    // Inzwischen hat eine neue Viertelstunde begonnen: Betrag neu anzeigen
    weiterleiten("/betrag?neu=1&kz=" + urlEncode(v.kz));
    return;
  }
  String zahlart = server.arg("zahlart");
  if (zahlart.length() == 0 || zahlart.length() > 30 || zahlart.indexOf('\t') >= 0) zahlart = "Karte (simuliert)";
  v.bezahltRappen += offen;
  v.bezahltUm = jetzt;
  v.zahlart = zahlart;
  speichere();
  Serial.printf("Zahlung: %s %s um %s\n", v.kz.c_str(), geld(offen).c_str(), zeitText(jetzt, true).c_str());
  weiterleiten("/bezahlt?kz=" + urlEncode(v.kz));
}

void seiteBezahlt() {
  String kz = normalisiereKennzeichen(server.arg("kz"));
  int i = finde(kz);
  if (i < 0 || vorgaenge[i].bezahltUm == 0) {
    weiterleiten("/");
    return;
  }
  const Vorgang& v = vorgaenge[i];
  String h = kopf("Kasse");
  h += "<h1>Kassenautomat</h1><div class='karte'>";
  h += "<p class='msg ok'>Bezahlt, Ausfahrt bis " + zeitText(v.bezahltUm + tarif::KARENZ_SEKUNDEN, false) + "</p>";
  h += "<p>Quittung drucken?</p>";
  h += "<a class='knopf' href='/quittung?kz=" + urlEncode(v.kz) + "'>Ja</a>";
  h += "<a class='knopf grau' href='/?msg=danke'>Nein</a></div>";
  h += fuss();
  sendeSeite(h);
}

void seiteQuittung() {
  String kz = normalisiereKennzeichen(server.arg("kz"));
  int i = finde(kz);
  if (i < 0 || vorgaenge[i].bezahltUm == 0) {
    sendeSeite(fehlerSeite("Für dieses Kennzeichen gibt es keine Zahlung."));
    return;
  }
  const Vorgang& v = vorgaenge[i];
  String h = kopf("Quittung");
  h += "<h1>Quittung</h1><div class='karte quittung'><table>";
  h += "<tr><td colspan='2'><b>Parkhaus M450</b></td></tr>";
  h += "<tr><td>Kennzeichen</td><td>" + html(v.kz) + "</td></tr>";
  h += "<tr><td>Datum</td><td>" + datumText(v.bezahltUm) + "</td></tr>";
  h += "<tr><td>Uhrzeit Zahlung</td><td>" + zeitText(v.bezahltUm, false) + "</td></tr>";
  h += "<tr><td>Einfahrt</td><td>" + zeitText(v.einfahrt, true) + "</td></tr>";
  h += "<tr><td>Zahlungsmittel</td><td>" + html(v.zahlart) + "</td></tr>";
  h += "<tr><td>Betrag</td><td><b>" + geld(v.bezahltRappen) + "</b></td></tr>";
  h += "<tr><td>Ausfahrt bis</td><td>" + zeitText(v.bezahltUm + tarif::KARENZ_SEKUNDEN, false) + "</td></tr>";
  h += "</table></div>";
  h += "<button onclick='window.print()'>Drucken</button><a class='knopf grau' href='/?msg=danke'>Fertig</a>";
  h += fuss();
  sendeSeite(h);
}

// ---------- Schnittstelle zu Pico 2 ----------
bool apiErlaubt() {
  if (server.arg("key") != API_KEY) {
    server.send(403, "text/plain", "fehler=key\n");
    return false;
  }
  // Pico 2 schickt seine Uhrzeit mit. Pico 2 ist die Referenzuhr.
  if (server.arg("t").length() > 0) {
    uhr.setzen(strtoul(server.arg("t").c_str(), nullptr, 10), atol(server.arg("tz").c_str()));
  }
  return true;
}

void apiEinfahrt() {
  if (!apiErlaubt()) return;
  String kz = normalisiereKennzeichen(server.arg("kz"));
  uint32_t einfahrt = strtoul(server.arg("einfahrt").c_str(), nullptr, 10);
  if (kz.length() == 0 || einfahrt == 0) {
    server.send(400, "text/plain", "fehler=parameter\n");
    return;
  }
  int i = finde(kz);
  if (i < 0) {
    if (anzahlVorgaenge >= MAX_VORGAENGE) entferne(0);  // ältesten Eintrag verwerfen
    i = anzahlVorgaenge++;
  }
  vorgaenge[i].kz = kz;
  vorgaenge[i].einfahrt = einfahrt;
  vorgaenge[i].bezahltUm = 0;
  vorgaenge[i].bezahltRappen = 0;
  vorgaenge[i].zahlart = "";
  speichere();
  Serial.printf("Einfahrt gemeldet: %s\n", kz.c_str());
  server.send(200, "text/plain", "ok\n");
}

void apiStatus() {
  if (!apiErlaubt()) return;
  String kz = normalisiereKennzeichen(server.arg("kz"));
  int i = finde(kz);
  String a;
  if (i < 0) {
    a = "status=unbekannt\n";
  } else {
    const Vorgang& v = vorgaenge[i];
    if (v.bezahltUm == 0) a = "status=offen\n";
    else if (uhr.gueltig && tarif::inKarenz(v.bezahltUm, uhr.jetzt())) a = "status=bezahlt\n";
    else a = "status=abgelaufen\n";
    a += "bezahlt=" + String(v.bezahltUm) + "\n";
    a += "rappen=" + String(v.bezahltRappen) + "\n";
  }
  server.send(200, "text/plain", a);
}

void apiAusfahrt() {
  if (!apiErlaubt()) return;
  int i = finde(normalisiereKennzeichen(server.arg("kz")));
  if (i >= 0) {
    entferne(i);
    speichere();
  }
  server.send(200, "text/plain", "ok\n");
}

void apiReset() {
  if (!apiErlaubt()) return;
  anzahlVorgaenge = 0;
  speichere();
  Serial.println("Reset durch Pico 2");
  server.send(200, "text/plain", "ok\n");
}

void apiSchranke() {
  if (!apiErlaubt()) return;
  servoAusfahrt.write(WINKEL_OFFEN);
  ausfahrtOffen = true;
  ausfahrtSeit = millis();
  Serial.println("Ausfahrtsschranke offen");
  server.send(200, "text/plain", "ok\n");
}

void schrankeAktualisieren() {
  if (ausfahrtOffen && millis() - ausfahrtSeit >= OFFEN_DAUER_MS) {
    servoAusfahrt.write(WINKEL_ZU);
    ausfahrtOffen = false;
    Serial.println("Ausfahrtsschranke geschlossen");
  }
}

void apiSetTime() {
  // Nur als Fallback, wenn Pico 2 noch keine Zeit geliefert hat
  if (uhr.gueltig) {
    server.send(200, "text/plain", "bereits");
    return;
  }
  bool ok = uhr.setzen(strtoul(server.arg("t").c_str(), nullptr, 10), atol(server.arg("tz").c_str()));
  server.send(200, "text/plain", ok ? "ok" : "ungueltig");
}

// Meldet sich bei Pico 2, damit Pico 2 die IP von Pico 1 kennt, und gleicht die Uhrzeit ab.
void hello() {
  if (WiFi.status() != WL_CONNECTED) {
    pico2Online = false;
    return;
  }
  if (helloGesendet && millis() - letzterHello < HELLO_INTERVALL_MS) return;
  helloGesendet = true;
  letzterHello = millis();

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  String url = String("http://") + PICO2_ADRESSE + "/api/hello?key=" + API_KEY + "&t=" + String(uhr.jetzt()) +
               "&tz=" + String(uhr.tzMinuten) + "&ok=" + (uhr.gueltig ? "1" : "0");
  if (!http.begin(client, url)) {
    pico2Online = false;
    return;
  }
  int code = http.GET();
  if (code == 200) {
    String antwort = http.getString();
    if (wert(antwort, "ok") == "1") {
      uhr.setzen(strtoul(wert(antwort, "t").c_str(), nullptr, 10), atol(wert(antwort, "tz").c_str()));
    }
    pico2Online = true;
  } else {
    pico2Online = false;
  }
  http.end();
}

void wlanPruefen() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wlanWarVerbunden) {
      wlanWarVerbunden = true;
      helloGesendet = false;
      Serial.print("WLAN verbunden, Kasse unter http://");
      Serial.println(WiFi.localIP());
    }
    return;
  }
  wlanWarVerbunden = false;
  if (millis() - letzterWlanVersuch < WLAN_NEUVERSUCH_MS) return;
  letzterWlanVersuch = millis();
  Serial.println("Verbinde mit WLAN " + String(WLAN_SSID) + " ...");
  WiFi.begin(WLAN_SSID, WLAN_PASSWORT);
}

// ---------- Setup und Loop ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Parksystem Pico 1 (Kasse) startet");

  servoAusfahrt.attach(PIN_SERVO_AUSFAHRT, 500, 2400);
  servoAusfahrt.write(WINKEL_ZU);

  fsOk = LittleFS.begin();
  if (!fsOk) Serial.println("WARNUNG: LittleFS nicht verfügbar (Flash Size mit FS wählen). Daten nur im RAM.");
  lade();
  Serial.printf("%d gespeicherte Parkvorgänge geladen\n", anzahlVorgaenge);

  server.on("/", seiteStart);
  server.on("/betrag", seiteBetrag);
  server.on("/bezahlen", HTTP_POST, aktionBezahlen);
  server.on("/bezahlt", seiteBezahlt);
  server.on("/quittung", seiteQuittung);
  server.on("/api/einfahrt", apiEinfahrt);
  server.on("/api/status", apiStatus);
  server.on("/api/ausfahrt", apiAusfahrt);
  server.on("/api/reset", apiReset);
  server.on("/api/schranke", apiSchranke);
  server.on("/api/settime", apiSetTime);
  server.onNotFound([]() { server.send(404, "text/plain", "Nicht gefunden"); });

  letzterWlanVersuch = millis();
  WiFi.begin(WLAN_SSID, WLAN_PASSWORT);
  server.begin();
}

void loop() {
  wlanPruefen();
  server.handleClient();
  schrankeAktualisieren();
  hello();
}
