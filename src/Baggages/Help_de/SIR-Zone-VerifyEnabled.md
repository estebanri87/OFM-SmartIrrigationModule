# Post-Bewässerungs-Verifizierung

Aktiviert die Überprüfung der Bodenfeuchte nach einer Bewässerung.

Nach Abschluss eines Bewässerungszyklus wird nach der konfigurierten Verzögerung die Bodenfeuchte erneut geprüft. Wenn die Feuchte nicht um den erwarteten Mindest-Deltawert gestiegen ist, wird ein Alarm ausgelöst.

**Mögliche Ursachen für Alarm:**
- Defektes Ventil (öffnet nicht)
- Unterbrochene Wasserleitung
- Verstopfte Sprinklerköpfe
- Defekter Bodenfeuchte-Sensor

**Hinweis:** Diese Funktion erfordert einen Bodenfeuchte-Sensor für diese Zone.

Der Verifizierungs-Alarm wird über das Zone-KO "VerifyFailed" gemeldet.
