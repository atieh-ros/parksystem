// Kennzeichen normalisieren und prüfen (US 1, TC-02, TC-11)
#pragma once
#include <Arduino.h>

// Leerzeichen am Rand entfernen, mehrfache Leerzeichen zusammenfassen,
// ASCII-Buchstaben gross schreiben. UTF-8 (z.B. Ä, Ö) bleibt unverändert.
inline String normalisiereKennzeichen(const String& roh) {
  String ergebnis;
  bool letztesLeer = true;
  for (unsigned int i = 0; i < roh.length(); i++) {
    char c = roh[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!letztesLeer) ergebnis += ' ';
      letztesLeer = true;
      continue;
    }
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    ergebnis += c;
    letztesLeer = false;
  }
  ergebnis.trim();
  return ergebnis;
}

// Gültig: 2 bis 16 Bytes, nur A-Z, 0-9, Leerzeichen, Bindestrich oder
// Umlaute (UTF-8), mindestens ein Buchstabe und eine Ziffer.
inline bool kennzeichenGueltig(const String& kz) {
  if (kz.length() < 2 || kz.length() > 16) return false;
  bool buchstabe = false;
  bool ziffer = false;
  for (unsigned int i = 0; i < kz.length(); i++) {
    uint8_t c = (uint8_t)kz[i];
    if (c >= 'A' && c <= 'Z') buchstabe = true;
    else if (c >= '0' && c <= '9') ziffer = true;
    else if (c >= 0x80) buchstabe = true;
    else if (c != ' ' && c != '-') return false;
  }
  return buchstabe && ziffer;
}
