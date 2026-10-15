// Tariflogik des Parksystems (Testkonzept 2.5)
//   - CHF 1.50 pro angebrochene 15 Minuten
//   - Karenzzeit 15:00 Minuten ab Bezahlung, bis und mit 15:00 gilt sie noch
// Alle Beträge in Rappen, alle Zeiten in Sekunden (Unix-Zeit).
#pragma once
#include <stdint.h>

namespace tarif {

const int32_t RAPPEN_PRO_EINHEIT = 150;
const int32_t SEKUNDEN_PRO_EINHEIT = 15 * 60;
const int32_t KARENZ_SEKUNDEN = 15 * 60;

// Anzahl angebrochene Viertelstunden (mindestens 1)
inline int32_t einheiten(int32_t sekunden) {
  if (sekunden <= 0) return 1;
  return (sekunden + SEKUNDEN_PRO_EINHEIT - 1) / SEKUNDEN_PRO_EINHEIT;
}

// Gesamtgebühr für die Zeit von der Einfahrt bis jetzt
inline int32_t gesamtRappen(uint32_t einfahrt, uint32_t jetzt) {
  return einheiten((int32_t)(jetzt - einfahrt)) * RAPPEN_PRO_EINHEIT;
}

// true, solange die Zahlung höchstens 15:00 Minuten zurückliegt
inline bool inKarenz(uint32_t bezahltUm, uint32_t jetzt) {
  if (bezahltUm == 0) return false;
  if (jetzt < bezahltUm) return true;
  return (jetzt - bezahltUm) <= (uint32_t)KARENZ_SEKUNDEN;
}

// Noch zu bezahlender Betrag. 0 = bezahlt und Karenzzeit läuft noch.
// Nach Ablauf der Karenzzeit wird die Differenz zur Gesamtgebühr verlangt.
inline int32_t offenerBetrag(uint32_t einfahrt, uint32_t bezahltUm, int32_t bezahltRappen, uint32_t jetzt) {
  if (inKarenz(bezahltUm, jetzt)) return 0;
  int32_t rest = gesamtRappen(einfahrt, jetzt) - bezahltRappen;
  if (rest < RAPPEN_PRO_EINHEIT) rest = RAPPEN_PRO_EINHEIT;
  return rest;
}

}  // namespace tarif
