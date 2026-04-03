# Bewässerungsintervall

Dauer des rollierenden Bewässerungsfensters in Tagen.

Nach dem **ersten abgeschlossenen Bewässerungszyklus** einer Periode startet ein Zeitfenster. Erst wenn dieses Fenster abgelaufen ist, beginnt eine neue Periode – `weekAmount` und Zykluszähler werden dann automatisch zurückgesetzt.

Innerhalb einer Periode können mehrere Zyklen stattfinden (z.B. 3× pro Woche), der Reset erfolgt aber immer erst nach Ablauf des Intervalls.

**Wertebereich:** 0–30 Tage
- `0` = Standardverhalten (7 Tage)
- `7` = wöchentliches Intervall (identisch mit 0)
- `10` = alle 10 Tage (z.B. für Gehölze)
- `14` = alle 2 Wochen

**Empfehlung:**
- Rasen: 7 Tage
- Stauden: 7–10 Tage
- Gehölze: 10–14 Tage

**Hinweis:** Der Wochenreset-KO setzt die Periode manuell zurück und startet das Fenster neu.
