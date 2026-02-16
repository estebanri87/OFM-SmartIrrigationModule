# Applikationsbeschreibung SmartIrrigationModule

Dieses Dokument beschreibt die ETS-Parameter des OFM-SmartIrrigationModule.

## Inhalte

* [System](#system)
* [Woche](#woche)
* [Wetter](#wetter)
* [Strategie](#strategie)
* [Zonen](#zonen)

## ETS-Oberflaeche (Regeln)

**System-Konfiguration:** Spinner haben Pfeil-Buttons; "Max. gleichzeitige Zonen" darf nicht groesser sein als "Anzahl Zonen".

**Wochenverwaltung:** Reset-Zeit wird im 24h-Format (HH:MM) eingegeben.

**Wetter-Einstellungen (Sensoren):** Checkboxen sind nur aktivierbar, wenn "Sensorwerte aktivieren" = Ja. MUSS-Sensoren (Temperatur, Regen, Regenmenge) sind visuell hervorgehoben.

**Wetter-Einstellungen (Prognose):** Checkboxen sind nur aktivierbar, wenn "Prognose aktivieren" = Ja. MUSS-Parameter (Tagestemperatur, Regen, Regenmenge) sind visuell hervorgehoben.

**Strategie:** Im "Smart Core"-Modus sind erweiterte Parameter ausgeblendet; im "Smart Pro"-Modus sind alle Parameter sichtbar.

**Zonen (Basis):** Alle Zonen-Parameter sind nur editierbar, wenn "Zone aktiviert" = Ja. "Flaeche" und "Durchfluss System" sind numerische Felder mit Dezimalstellen.

**Pflanzentyp & Faktoren:** Bei vordefinierten Pflanzentypen werden ET-Faktor und Interzeptionsfaktor automatisch gesetzt und sind read-only; bei "Benutzerdefiniert" sind beide editierbar.

**Wassermenge & Frequenz:** "Max Menge pro Woche" muss >= "Min Menge pro Woche" sein.

**Zeitfenster:** Zeitfenster-Parameter sind nur editierbar, wenn das jeweilige Zeitfenster aktiv ist. "Ende" liegt nach "Start".

**Erweiterte Einstellungen (Smart Pro):** Diese Gruppe ist nur im "Smart Pro"-Modus sichtbar.

## System

<!-- DOC HelpContext="System-ZoneCount" -->
### **Anzahl Zonen**

Legt die Anzahl aktiver Bewaesserungszonen fest (1-10).

<!-- DOC HelpContext="System-MaxConcurrent" -->
### **Max. gleichzeitige Zonen**

Maximale Anzahl Zonen, die gleichzeitig bewaessert werden duerfen.

## Woche

<!-- DOC HelpContext="Week-Start" -->
### **Wochenbeginn**

Erster Tag der Woche fuer die Wochenzaehler.

<!-- DOC HelpContext="Week-ResetTime" -->
### **Reset-Zeit**

Zeitpunkt, zu dem die Wochenzaehler zurueckgesetzt werden. Empfohlen: Mitternacht (00:00).

## Wetter

### **Sensoren**

<!-- DOC HelpContext="Weather-SensorEnable" -->
#### **Sensorwerte aktivieren**

Schaltet die Verwendung realer Sensorwerte ein oder aus.

<!-- DOC HelpContext="Weather-SensorTemp" -->
#### **Temperatur (Sensor)**

MUSS-Sensor. Dieser Sensor ist fuer optimale Funktion erforderlich. Bei Ausfall wird Fallback-Modus verwendet.

<!-- DOC HelpContext="Weather-SensorRain" -->
#### **Regen (Sensor)**

MUSS-Sensor. Dieser Sensor ist fuer optimale Funktion erforderlich. Bei Ausfall wird Fallback-Modus verwendet.

<!-- DOC HelpContext="Weather-SensorRainAmount" -->
#### **Regenmenge (Sensor)**

MUSS-Sensor. Dieser Sensor ist fuer optimale Funktion erforderlich. Bei Ausfall wird Fallback-Modus verwendet.

<!-- DOC HelpContext="Weather-SensorHumidity" -->
#### **Luftfeuchte (Sensor)**

Optional. Verbessert die ET0-Berechnung.

<!-- DOC HelpContext="Weather-SensorWind" -->
#### **Wind (Sensor)**

Optional. Verbessert die ET0-Berechnung.

<!-- DOC HelpContext="Weather-SensorWindDir" -->
#### **Windrichtung (Sensor)**

Optional. Zusaetzliche Eingabe fuer erweiterte Berechnung.

<!-- DOC HelpContext="Weather-SensorSoil" -->
#### **Bodenfeuchte (Sensor)**

Optional. Unter Schwellwert wird Bewaesserung erzwungen.

### **Prognose**

<!-- DOC HelpContext="Weather-ForecastEnable" -->
#### **Prognose aktivieren**

Schaltet die Verwendung von Wetterprognosen ein oder aus.

<!-- DOC HelpContext="Weather-ForecastTemp" -->
#### **Tagestemperatur (Prognose)**

MUSS-Parameter. Dieser Parameter ist fuer optimale Funktion erforderlich. Bei Ausfall wird Fallback-Modus verwendet.

<!-- DOC HelpContext="Weather-ForecastRain" -->
#### **Regen (Prognose)**

MUSS-Parameter. Dieser Parameter ist fuer optimale Funktion erforderlich. Bei Ausfall wird Fallback-Modus verwendet.

<!-- DOC HelpContext="Weather-ForecastRainAmount" -->
#### **Regenmenge (Prognose)**

MUSS-Parameter. Dieser Parameter ist fuer optimale Funktion erforderlich. Bei Ausfall wird Fallback-Modus verwendet.

<!-- DOC HelpContext="Weather-ForecastHumidity" -->
#### **Luftfeuchte (Prognose)**

Optional. Verbessert die ET0-Berechnung.

<!-- DOC HelpContext="Weather-ForecastWind" -->
#### **Wind (Prognose)**

Optional. Verbessert die ET0-Berechnung.

<!-- DOC HelpContext="Weather-ForecastWindDir" -->
#### **Windrichtung (Prognose)**

Optional. Zusaetzliche Eingabe fuer erweiterte Berechnung.

<!-- DOC HelpContext="Weather-ForecastUV" -->
#### **UV-Index (Prognose)**

Optional. Zusaetzlicher Verdunstungsfaktor.

<!-- DOC HelpContext="Weather-MinTemp" -->
#### **Min. Temperatur Freigabe**

Bewaesserung wird verhindert, wenn Temperatur unter diesem Wert liegt (Frostschutz).

<!-- DOC HelpContext="Weather-ForecastUpdate" -->
#### **Prognose-Update**

Intervall fuer die Aktualisierung der Wetterprognose in Minuten.

## Strategie

<!-- DOC HelpContext="Strategy-Mode" -->
### **Bewaesserungs-Modus**

Smart Core: einfache Bedienung. Smart Pro: erweiterte Parameter sichtbar.

<!-- DOC HelpContext="Strategy-Mode-Core" -->
#### **Smart Core**

Einfache Bedienung mit Standardparametern. Empfohlen fuer Einsteiger.

<!-- DOC HelpContext="Strategy-Mode-Pro" -->
#### **Smart Pro**

Erweiterte Einstellungen fuer maximale Kontrolle. Fuer erfahrene Anwender.

<!-- DOC HelpContext="Strategy-Program" -->
### **Programm-Auswahl**

Zeit + Wetter, nur Zeit oder Manuell.

<!-- DOC HelpContext="Strategy-WeightHigh" -->
### **Gewicht hoch**

Gewichtung fuer aktuelle Werte. Summe der Gewichte wird auf 1 normiert.

<!-- DOC HelpContext="Strategy-WeightMid" -->
### **Gewicht mittel**

Gewichtung fuer +48h Prognose. Summe der Gewichte wird auf 1 normiert.

<!-- DOC HelpContext="Strategy-WeightLow" -->
### **Gewicht niedrig**

Gewichtung fuer +7d Prognose. Summe der Gewichte wird auf 1 normiert.

<!-- DOC HelpContext="Strategy-WeightReal" -->
### **Gewicht real**

Gewichtung fuer reale Sensoren (kombinierter Modus). Summe wird normiert.

<!-- DOC HelpContext="Strategy-WeightForecast" -->
### **Gewicht Prognose**

Gewichtung fuer Prognose (kombinierter Modus). Summe wird normiert.

## Zonen

Die folgenden Parameter gelten je Zone.

<!-- DOC HelpContext="Zone-Enabled" -->
### **Zone aktiviert**

Aktiviert die Zone. Weitere Parameter sind nur dann editierbar.

<!-- DOC HelpContext="Zone-Name" -->
### **Bezeichnung**

Benutzerdefinierter Name der Zone, z. B. "Rasen Vorgarten".

<!-- DOC HelpContext="Zone-Area" -->
### **Flaeche**

Groesse der zu bewaessernden Flaeche in Quadratmetern.

<!-- DOC HelpContext="Zone-Flow" -->
### **Durchfluss System**

Wasserdurchfluss des Bewaesserungssystems (Tropfschlauch, Sprinkler, etc.).

<!-- DOC HelpContext="Zone-PlantType" -->
### **Pflanzentyp**

Vordefinierte Pflanzen. Bei "Benutzerdefiniert" sind Faktoren editierbar.

<!-- DOC HelpContext="Zone-ETFactor" -->
### **ET-Faktor**

Evapotranspirations-Faktor (Kc) in Prozent.

<!-- DOC HelpContext="Zone-Interception" -->
### **Interzeptionsfaktor**

Abfangfaktor fuer Niederschlag in Prozent.

<!-- DOC HelpContext="Zone-MinWeek" -->
### **Min. Menge pro Woche**

Mindestmenge in l/m2 pro Woche.

<!-- DOC HelpContext="Zone-MaxWeek" -->
### **Max. Menge pro Woche**

Maximalmenge in l/m2 pro Woche. Muss >= Min. Menge sein.

<!-- DOC HelpContext="Zone-AbsMax" -->
### **Absolutes Wochen-Maximum**

Hartes Wochenlimit in l/m2, das nicht ueberschritten wird.

<!-- DOC HelpContext="Zone-BaseTemp" -->
### **Basistemperatur**

Ab dieser Temperatur wird Bewaesserung freigegeben. Bei Hitze wird Wassermenge dynamisch erhoeht.

<!-- DOC HelpContext="Zone-BaseTempHyst" -->
### **Basistemperatur-Hysterese**

Verhindert staendige Neuberechnung bei Temperaturschwankungen. Evaluation erfolgt einmal taeglich.

<!-- DOC HelpContext="Zone-MaxCycles" -->
### **Max. Zyklen pro Woche**

Maximale Bewaesserungsdurchgaenge pro Woche. Bei Wert 1 und hohem Bedarf wird Fehler ausgegeben.

<!-- DOC HelpContext="Zone-MinPerCycle" -->
### **Min. Menge pro Zyklus**

Mindestmenge je Bewaesserungszyklus in l/m2.

<!-- DOC HelpContext="Zone-TW1Active" -->
### **Zeitfenster 1 aktiv**

Aktiviert das erste Zeitfenster fuer die Zone.

<!-- DOC HelpContext="Zone-TW1Start" -->
### **Zeitfenster 1 Start**

Startzeit (HH:MM). Ende muss nach Start liegen.

<!-- DOC HelpContext="Zone-TW1End" -->
### **Zeitfenster 1 Ende**

Endzeit (HH:MM). Muss nach der Startzeit liegen.

<!-- DOC HelpContext="Zone-TW2Active" -->
### **Zeitfenster 2 aktiv**

Aktiviert das zweite Zeitfenster fuer die Zone.

<!-- DOC HelpContext="Zone-TW2Start" -->
### **Zeitfenster 2 Start**

Startzeit (HH:MM). Ende muss nach Start liegen.

<!-- DOC HelpContext="Zone-TW2End" -->
### **Zeitfenster 2 Ende**

Endzeit (HH:MM). Muss nach der Startzeit liegen.

<!-- DOC HelpContext="Zone-TimeType" -->
### **Zeitfenster-Typ**

Flexibel: Start innerhalb des Fensters. Fix: Start exakt zur Startzeit.

<!-- DOC HelpContext="Zone-TimeType-Flexible" -->
#### **Flexibel**

Bewasserung startet flexibel innerhalb des Zeitfensters (empfohlen).

<!-- DOC HelpContext="Zone-TimeType-Fixed" -->
#### **Fix**

Bewasserung startet exakt zur Startzeit. Bei Konflikt wird Fehler ausgegeben.

<!-- DOC HelpContext="Zone-MaxRuntime" -->
### **Max. Laufzeit pro Zyklus**

Maximale Bewaesserungsdauer je Zyklus in Minuten.

<!-- DOC HelpContext="Zone-SoilThreshold" -->
### **Bodenfeuchte-Schwellwert**

Bei Unterschreitung wird Bewaesserung erzwungen, unabhaengig von anderen Parametern.

<!-- DOC HelpContext="Zone-RainDelayFactor" -->
### **Regen-Verzoegerungs-Faktor**

Sperrzeit [h] = Regenmenge [mm] * Faktor. Beispiel: 10mm * 2 = 20h Sperrzeit.

<!-- DOC HelpContext="Zone-ManualRuntime" -->
### **Manuelle Laufzeit**

Dauer der Bewaesserung bei manueller Aktivierung.

