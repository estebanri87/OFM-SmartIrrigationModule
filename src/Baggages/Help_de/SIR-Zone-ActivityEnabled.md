# Aktivitätserkennung

Aktiviert den per-Zone Aktivitätsblock über ein eigenes Kommunikationsobjekt.

Solange das KO "Aktivitätsblock" den Wert **1** empfängt, wird die Bewässerung dieser Zone pausiert und das Ventil geschlossen.

**Typische Anwendungen:**
- Tür zum Garten → Bewässerung der Gartenzone pausieren
- Präsenzmelder auf der Terrasse → Terrassen-Zone pausieren
- Bewegungsmelder am Eingang → Eingangszone pausieren

**Mehrere Sensoren:** Mehrere Gruppadressen (z. B. mehrere Türen) können dem gleichen KO zugewiesen werden. Solange eines der verknüpften Objekte aktiv ist, bleibt die Zone pausiert.

**Hinweis:** Ein aktiver Bodenfeuchte-Override (Boden zu trocken) hat höhere Priorität als der Aktivitätsblock — in diesem Fall wird die Bewässerung trotz aktiver Erkennung fortgesetzt.
