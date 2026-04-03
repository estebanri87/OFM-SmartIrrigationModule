### ET0 (Evapotranspiration)

Tageswerte der Referenz-Evapotranspiration nach FAO-56 Penman-Monteith (mm/Tag) fuer 7 Tage (Tag 0 = Heute bis Tag 6).

**Wochenplanung:** Wenn alle 7 Tageswerte empfangen werden, aktiviert sich automatisch die Wochenplanung. Der Bewaesserungsbedarf wird ueber die gesamte Woche berechnet und gleichmaessig auf die konfigurierten Zyklen (maxCycles) verteilt.

**Tagesbetrieb:** Wenn nur einzelne Tageswerte (z.B. nur Heute) empfangen werden, wird dieser Wert anstelle der internen Naeherungsberechnung verwendet. Die bestehende Horizont-Logik bleibt aktiv.

**Ohne externe ET0:** Automatischer Fallback auf die interne Berechnung. Keine manuelle Umschaltung noetig.

Quelle: InternetWeatherModule (Open-Meteo) oder Home Assistant.

Typische Werte: 0.5-2 mm/Tag (Winter), 4-8 mm/Tag (Sommer).
