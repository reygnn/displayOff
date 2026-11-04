# displayOff – Monitore aus nach Zeit, Starfield auf Knopfdruck (für mwc)

Eine C-Datei, nur libwayland, kein Toolkit. Drei unabhängige Betriebsarten:

    displayOff [-t SEKUNDEN]   läuft weiter: Monitore aus nach so langer Zeit ohne
                               Eingabe (Standard 600 = 10 Minuten), bei der nächsten
                               Eingabe wieder an
    displayOff --now           Monitore sofort aus, bei der nächsten Eingabe wieder an,
                               dann ist das Programm beendet
    displayOff --stars         rollendes Starfield auf allen Monitoren, bis zur
                               nächsten Eingabe

Die Monitore werden wirklich abgeschaltet, nicht schwarz gezeichnet. Fenster behalten
dabei ihren Platz.

## Bauen

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols
    make
    sudo make install        # -> /usr/local/bin/displayOff

## In mwc

Der Timer gehört in `~/.config/mwc/autostart`:

    #!/bin/sh
    displayOff &

**Super+Esc** startet das Starfield. Ein anderes Programm oder eine andere Option stellt
in `~/.config/mwc/mwmrc` ein:

    Mwm*screenSaver:   displayOff --stars

Fürs Root-Menü, ebenfalls in `mwmrc`:

    Mwm*rootMenuItem:  Starfield: displayOff --stars
    Mwm*rootMenuItem:  Display Off: displayOff --now

mwc braucht dafür `ext-idle-notify-v1`, `idle-inhibit-unstable-v1` und
`wlr-output-power-management-unstable-v1`. Andere Compositoren mit diesen Protokollen
gehen auch; fürs Starfield genügt `wlr-layer-shell`.

## Verhalten

- **Starfield beenden:** Taste, Maustaste, Mausrad oder eine deutliche Mausbewegung
  (mehr als 8 Pixel). Ein Anstoßen des Tisches zählt nicht.
- **`--now`:** Eingaben in der ersten Sekunde wecken nicht, sonst schaltete das Loslassen
  des Menüklicks gleich wieder ein. Danach weckt jede Eingabe, auch wenn gerade ein
  Video-Player die Zeitabschaltung anhält. Das braucht `ext-idle-notify` Version 2.
- **Beides zusammen:** Das Starfield läuft auch nach Ablauf der Zeit nicht weiter: Die
  Monitore gehen aus, und gezeichnet wird dann nichts mehr. Eine Taste schaltet ein und
  beendet das Starfield.
- **Aufwecken:** Die Eingabe, die aufweckt, geht wie jede andere ans Fenster. Eine Taste
  landet also auch im fokussierten Programm.
- **Videos:** Wer `idle-inhibit` nutzt (z. B. mpv oder Firefox beim Abspielen), hält die
  Monitore an, auch minimiert. Programme, die das nicht tun, schützt nichts.
- **Beenden:** `displayOff` schaltet bei SIGTERM, SIGINT und SIGHUP die Monitore wieder
  ein. Nach `kill -9` bleiben sie aus, bis sie jemand einschaltet.
- **Kein Sperrbildschirm:** Jede Eingabe beendet das Starfield.

Das Starfield rechnet mit 256 Zeilen wie ein PAL-Amiga, höchstens 50 Bilder pro Sekunde,
und vergrößert ohne Glättung.

## Tests

    make test

fährt displayOff unter einem headless laufenden mwc mit simulierter Maus und Tastatur
und schaltet keinen echten Monitor. Es braucht einen gebauten mwc samt `build/vinput` und
`build/pngtool` in `MWC_DIR` (sonst `../mwc` oder `../mwm`), `grim` und `foot`. Mit GPU
läuft der Test unter GL, sonst unter pixman: Dort bleibt das Bild nach dem ersten Aufwecken
schwarz, bis zum nächsten, was der Test auslässt.

## Lizenz

MIT, siehe [LICENSE](LICENSE).
