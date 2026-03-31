# Tank-Aktion bei niedrigem Füllstand

Definiert das Verhalten bei Erreichen des Mindestfüllstands:

- **Pausieren:** Laufende Bewässerung wird pausiert, bis der Füllstand wieder über dem Mindestfüllstand liegt (mit Hysterese)
- **Stoppen:** Alle laufenden Zonen werden sofort gestoppt, keine neuen Starts bis Füllstand wieder ausreichend

**Empfehlung:** "Pausieren" für Systeme mit Nachspeisung, "Stoppen" für reine Regenwasserzisternen.
