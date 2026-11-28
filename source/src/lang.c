/* lang.c - traductions des textes à l'écran (voir lang.h) */
#include "lang.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static int g_lang = LANG_FR;

typedef struct { const char *t[LANG_COUNT]; } Entry;

/* { français, anglais, espagnol, allemand, italien } */
static const Entry k_tab[] = {
    /* messages */
    { { "Garde appuye pour te relever", "Hold to get up", "Manten pulsado para levantarte", "Gedrueckt halten zum Aufstehen", "Tieni premuto per rialzarti" } },
    { { "Garde appuye pour monter sur la moto", "Hold to get on the bike", "Manten pulsado para subir a la moto", "Gedrueckt halten zum Aufsteigen", "Tieni premuto per salire in moto" } },
    { { "Garde appuye pour le respawn sur la piste", "Hold to respawn on the track", "Manten pulsado para reaparecer en la pista", "Gedrueckt halten fuer Respawn auf der Strecke", "Tieni premuto per il respawn in pista" } },
    { { "Respawn sur la piste", "Respawn on the track", "Reaparecer en la pista", "Respawn auf der Strecke", "Respawn in pista" } },
    { { "Respawn pas fait : appuie toi-meme sur ta touche de respawn", "Respawn failed: press your respawn key yourself", "Reaparicion fallida: pulsa tu tecla de reaparicion", "Respawn fehlgeschlagen: druecke selbst deine Respawn-Taste", "Respawn non riuscito: premi tu il tasto di respawn" } },
    { { "Clavier prioritaire a pied : OUI (souris pour la camera)", "Keyboard priority on foot: ON (mouse moves the camera)", "Prioridad del teclado a pie: SI (raton para la camara)", "Tastatur-Vorrang zu Fuss: AN (Maus fuer die Kamera)", "Priorita tastiera a piedi: SI (mouse per la telecamera)" } },
    { { "Clavier prioritaire a pied : NON (souris libre, le jeu voit les touches)", "Keyboard priority on foot: OFF (free mouse, the game sees the keys)", "Prioridad del teclado a pie: NO (raton libre, el juego ve las teclas)", "Tastatur-Vorrang zu Fuss: AUS (Maus frei, das Spiel sieht die Tasten)", "Priorita tastiera a piedi: NO (mouse libero, il gioco vede i tasti)" } },
    { { "Manette prioritaire a pied : OUI", "Gamepad priority on foot: ON", "Prioridad del mando a pie: SI", "Controller-Vorrang zu Fuss: AN", "Priorita controller a piedi: SI" } },
    { { "Manette prioritaire a pied : NON (le jeu voit aussi la manette)", "Gamepad priority on foot: OFF (the game also sees the gamepad)", "Prioridad del mando a pie: NO (el juego tambien ve el mando)", "Controller-Vorrang zu Fuss: AUS (das Spiel sieht den Controller auch)", "Priorita controller a piedi: NO (il gioco vede anche il controller)" } },
    { { "Serveur avec le plugin Ragdoll : marche activee", "Server with the Ragdoll plugin: walking enabled", "Servidor con el plugin Ragdoll: caminar activado", "Server mit Ragdoll-Plugin: Laufen aktiviert", "Server con il plugin Ragdoll: camminata attivata" } },
    { { "Serveur sans le plugin Ragdoll : marche desactivee", "Server without the Ragdoll plugin: walking disabled", "Servidor sin el plugin Ragdoll: caminar desactivado", "Server ohne Ragdoll-Plugin: Laufen deaktiviert", "Server senza il plugin Ragdoll: camminata disattivata" } },
    { { "En ligne : marche seulement apres une chute (F seulement hors ligne)", "Online: walking only after a crash (F works offline only)", "En linea: caminar solo despues de una caida (F solo sin conexion)", "Online: Laufen nur nach einem Sturz (F nur offline)", "Online: camminata solo dopo una caduta (F solo offline)" } },
    /* menu P */
    { { "MXRP Ragdoll  -  reglages", "MXRP Ragdoll  -  settings", "MXRP Ragdoll  -  ajustes", "MXRP Ragdoll  -  Einstellungen", "MXRP Ragdoll  -  impostazioni" } },
    { { "LANGUE", "LANGUAGE", "IDIOMA", "SPRACHE", "LINGUA" } },
    { { "Langue", "Language", "Idioma", "Sprache", "Lingua" } },
    { { "AFFICHAGE", "DISPLAY", "PANTALLA", "ANZEIGE", "SCHERMO" } },
    { { "Fleche sur la moto au sol", "Arrow over the bike", "Flecha sobre la moto", "Pfeil ueber dem Motorrad", "Freccia sulla moto" } },
    /* v1.8.162 */
    { { "Champ de vision a pied (1re personne)", "On-foot field of view (1st person)", "Campo de vision a pie (1a persona)", "Sichtfeld zu Fuss (1. Person)", "Campo visivo a piedi (1a persona)" } },
    { { "comme sur la moto", "same as on the bike", "como en la moto", "wie auf dem Motorrad", "come sulla moto" } },
    { { "SOURIS", "MOUSE", "RATON", "MAUS", "MOUSE" } },
    /* v1.8.119 : rampes FMX */
    { { "Rampe trop haute ici : roues sans contact au-dessus de %.1f m",
        "Ramp too high here: no wheel contact above %.1f m",
        "Rampa demasiado alta aqui: sin contacto sobre %.1f m",
        "Rampe hier zu hoch: kein Radkontakt ueber %.1f m",
        "Rampa troppo alta qui: ruote senza contatto sopra %.1f m" } },
    { { "Comp 8 ft", "Comp 8 ft", "Comp 8 ft", "Comp 8 ft", "Comp 8 ft" } },
    { { "Super kicker", "Super kicker", "Super kicker", "Super-Kicker", "Super kicker" } },
    { { "Saut 120 ft", "120 ft jump", "Salto 120 ft", "120-ft-Sprung", "Salto 120 ft" } },
    { { "Quarter pipe", "Quarter pipe", "Quarter pipe", "Quarterpipe", "Quarter pipe" } },
    { { "Piste d'elan", "Run-in", "Pista de impulso", "Anlauf", "Rincorsa" } },
    { { "Forme", "Shape", "Forma", "Form", "Forma" } },
    { { "changer", "change", "cambiar", "aendern", "cambia" } },
    { { "Couleur", "Color", "Color", "Farbe", "Colore" } },
    { { "texture d'origine", "original texture", "textura original", "Originaltextur", "texture originale" } },
    { { "Cette forme garde sa texture d'origine", "This shape keeps its original texture", "Esta forma mantiene su textura original", "Diese Form behaelt ihre Originaltextur", "Questa forma mantiene la texture originale" } },
    { { "Reception", "Landing", "Recepcion", "Landung", "Atterraggio" } },
    { { "Rampes", "Ramps", "Rampas", "Rampen", "Rampe" } },
    { { "FMX", "FMX", "FMX", "FMX", "FMX" } },
    { { "une rampe", "a ramp", "una rampa", "eine Rampe", "una rampa" } },
    { { "toutes les rampes", "all ramps", "todas las rampas", "alle Rampen", "tutte le rampe" } },
    { { "Rampes FMX", "FMX ramps", "Rampas FMX", "FMX-Rampen", "Rampe FMX" } },
    { { "Forme et couleur", "Shape and color", "Forma y color", "Form und Farbe", "Forma e colore" } },
    { { "Rampe", "Ramp", "Rampa", "Rampe", "Rampa" } },
    { { "Kicker", "Kicker", "Kicker", "Kicker", "Kicker" } },
    { { "F-Head", "F-Head", "F-Head", "F-Head", "F-Head" } },
    { { "Grand saut", "Long jump", "Salto largo", "Weitsprung", "Salto lungo" } },
    { { "bois", "wood", "madera", "Holz", "legno" } },
    { { "noir", "black", "negro", "schwarz", "nero" } },
    { { "bleu", "blue", "azul", "blau", "blu" } },
    { { "rouge", "red", "rojo", "rot", "rosso" } },
    { { "Plus de place : %d rampes au plus", "No room: %d ramps at most", "Sin sitio: %d rampas como maximo", "Kein Platz: hoechstens %d Rampen", "Niente spazio: al massimo %d rampe" } },
    { { "Rampe posee (%d)", "Ramp placed (%d)", "Rampa colocada (%d)", "Rampe aufgestellt (%d)", "Rampa posata (%d)" } },
    { { "Rampe retiree (%d)", "Ramp removed (%d)", "Rampa quitada (%d)", "Rampe entfernt (%d)", "Rampa rimossa (%d)" } },
    { { "Aucune rampe a moins de %d m", "No ramp within %d m", "Ninguna rampa a menos de %d m", "Keine Rampe naeher als %d m", "Nessuna rampa entro %d m" } },
    { { "%d rampes retirees", "%d ramps removed", "%d rampas quitadas", "%d Rampen entfernt", "%d rampe rimosse" } },
    /* v1.8.110 : tutoriel */
    { { "Relever la moto", "Lift the bike", "Levantar la moto", "Motorrad aufheben", "Rialza la moto" } },
    { { "Monter sur la moto (manette : garder appuye)", "Get on the bike (gamepad: hold)", "Subir a la moto (mando: mantener)", "Aufsteigen (Controller: halten)", "Sali in moto (controller: tieni premuto)" } },
    { { "fleche haut", "D-pad up", "cruceta arriba", "Steuerkreuz oben", "croce su" } },
    { { "fleche bas", "D-pad down", "cruceta abajo", "Steuerkreuz unten", "croce giu" } },
    { { "fleche gauche", "D-pad left", "cruceta izquierda", "Steuerkreuz links", "croce sinistra" } },
    { { "fleche droite", "D-pad right", "cruceta derecha", "Steuerkreuz rechts", "croce destra" } },
    { { "Messages et barres de progression", "Messages and progress bars", "Mensajes y barras de progreso", "Meldungen und Fortschrittsbalken", "Messaggi e barre di avanzamento" } },
    { { "AIDE", "HELP", "AYUDA", "HILFE", "AIUTO" } },
    { { "Afficher le tutoriel", "Show the tutorial", "Mostrar el tutorial", "Tutorial anzeigen", "Mostra il tutorial" } },
    { { "Bienvenue dans MXRP Ragdoll", "Welcome to MXRP Ragdoll", "Bienvenido a MXRP Ragdoll", "Willkommen bei MXRP Ragdoll", "Benvenuto in MXRP Ragdoll" } },
    { { "Quand tu chutes, ton pilote tombe vraiment et roule sur la piste.", "When you crash, your rider really falls and rolls on the track.", "Cuando te caes, tu piloto cae de verdad y rueda por la pista.", "Wenn du stuerzt, faellt dein Fahrer wirklich und rollt ueber die Strecke.", "Quando cadi, il tuo pilota cade davvero e rotola sulla pista." } },
    { { "Ensuite il se releve, marche jusqu'a sa moto, la releve et remonte.", "Then he gets up, walks to his bike, lifts it and gets back on.", "Luego se levanta, camina hasta su moto, la levanta y vuelve a subir.", "Dann steht er auf, geht zu seinem Motorrad, hebt es auf und steigt wieder auf.", "Poi si rialza, cammina fino alla moto, la rialza e risale." } },
    { { "Se relever (garder appuye) :", "Get up (hold):", "Levantarse (mantener):", "Aufstehen (halten):", "Rialzarsi (tieni premuto):" } },
    { { "Respawn sur la piste : garde la touche de respawn du jeu.", "Respawn on the track: hold the game's respawn button.", "Reaparecer en la pista: manten el boton de reaparicion del juego.", "Respawn auf der Strecke: die Respawn-Taste des Spiels halten.", "Respawn in pista: tieni premuto il tasto di respawn del gioco." } },
    { { "A pied", "On foot", "A pie", "Zu Fuss", "A piedi" } },
    { { "Marcher :", "Walk:", "Caminar:", "Gehen:", "Camminare:" } },
    { { "stick gauche", "left stick", "stick izquierdo", "linker Stick", "stick sinistro" } },
    { { "Courir :", "Run:", "Correr:", "Rennen:", "Correre:" } },
    { { "Sauter :", "Jump:", "Saltar:", "Springen:", "Saltare:" } },
    { { "Vue 1re / 3e personne :", "1st / 3rd person view:", "Vista 1a / 3a persona:", "Ansicht 1. / 3. Person:", "Vista 1a / 3a persona:" } },
    { { "Relever la moto (a cote) :", "Lift the bike (next to it):", "Levantar la moto (al lado):", "Motorrad aufheben (daneben):", "Rialzare la moto (accanto):" } },
    { { "Monter : appuie encore (manette : garde appuye).", "Get on: press again (gamepad: hold).", "Subir: pulsa otra vez (mando: manten).", "Aufsteigen: nochmal druecken (Controller: halten).", "Salire: premi di nuovo (controller: tieni premuto)." } },
    { { "Descendre (moto arretee, hors ligne) :", "Get off (bike stopped, offline):", "Bajar (moto parada, sin conexion):", "Absteigen (Motorrad steht, offline):", "Scendere (moto ferma, offline):" } },
    { { "Objets et reglages", "Objects and settings", "Objetos y ajustes", "Objekte und Einstellungen", "Oggetti e impostazioni" } },
    { { "Roue des objets (a pied) : B   -   manette : croix bas", "Objects wheel (on foot): B   -   gamepad: D-pad down", "Rueda de objetos (a pie): B   -   mando: cruceta abajo", "Objektrad (zu Fuss): B   -   Controller: Steuerkreuz unten", "Ruota oggetti (a piedi): B   -   controller: croce giu" } },
    { { "Plots et bottes de foin : poser, ramasser, porter, pousser.", "Cones and hay bales: place, pick up, carry, push.", "Conos y balas de paja: poner, recoger, llevar, empujar.", "Kegel und Heuballen: aufstellen, aufheben, tragen, schieben.", "Coni e balle di fieno: posare, raccogliere, portare, spingere." } },
    { { "Menu P : touches, langue, manette, pilote et equipement.", "Menu P: keys, language, gamepad, rider and gear.", "Menu P: teclas, idioma, mando, piloto y equipo.", "Menue P: Tasten, Sprache, Controller, Fahrer und Ausruestung.", "Menu P: tasti, lingua, controller, pilota e equipaggiamento." } },
    { { "En ligne : serveurs avec RAGDOLL dans le nom.", "Online: servers with RAGDOLL in the name.", "En linea: servidores con RAGDOLL en el nombre.", "Online: Server mit RAGDOLL im Namen.", "Online: server con RAGDOLL nel nome." } },
    { { "Serveurs officiels : sors MXRP_Ragdoll.dlo du dossier plugins avant.", "Official servers: move MXRP_Ragdoll.dlo out of the plugins folder first.", "Servidores oficiales: saca antes MXRP_Ragdoll.dlo de la carpeta plugins.", "Offizielle Server: vorher MXRP_Ragdoll.dlo aus dem Ordner plugins nehmen.", "Server ufficiali: prima togli MXRP_Ragdoll.dlo dalla cartella plugins." } },
    { { "Revoir ce tutoriel : menu P, Afficher le tutoriel.", "See this tutorial again: menu P, Show the tutorial.", "Ver de nuevo este tutorial: menu P, Mostrar el tutorial.", "Tutorial erneut ansehen: Menue P, Tutorial anzeigen.", "Rivedere il tutorial: menu P, Mostra il tutorial." } },
    { { "Entree / A : suivant   -   Retour / B : fermer", "Enter / A: next   -   Backspace / B: close", "Intro / A: siguiente   -   Retroceso / B: cerrar", "Enter / A: weiter   -   Ruecktaste / B: schliessen", "Invio / A: avanti   -   Backspace / B: chiudi" } },
    { { "Entree / A : fermer", "Enter / A: close", "Intro / A: cerrar", "Enter / A: schliessen", "Invio / A: chiudi" } },
    /* v1.8.107 : boutons de la manette (Xbox / PlayStation) */
    { { "MANETTE", "GAMEPAD", "MANDO", "CONTROLLER", "CONTROLLER" } },
    { { "Boutons de la manette", "Gamepad buttons", "Botones del mando", "Controller-Tasten", "Tasti del controller" } },
    { { "Auto (PlayStation)", "Auto (PlayStation)", "Auto (PlayStation)", "Auto (PlayStation)", "Auto (PlayStation)" } },
    { { "Auto (Xbox)", "Auto (Xbox)", "Auto (Xbox)", "Auto (Xbox)", "Auto (Xbox)" } },
    { { "Gauche/Droite : Auto / Xbox / PlayStation", "Left/Right: Auto / Xbox / PlayStation", "Izquierda/Derecha: Auto / Xbox / PlayStation", "Links/Rechts: Auto / Xbox / PlayStation", "Sinistra/Destra: Auto / Xbox / PlayStation" } },
    { { "Croix", "Cross", "Cruz", "Kreuz", "Croce" } },
    { { "Rond", "Circle", "Circulo", "Kreis", "Cerchio" } },
    { { "Carre", "Square", "Cuadrado", "Quadrat", "Quadrato" } },
    { { "Triangle", "Triangle", "Triangulo", "Dreieck", "Triangolo" } },
    { { "Respawn non fait : appuie sur ta touche de respawn", "No respawn: press your respawn button", "Sin respawn: pulsa tu boton de respawn", "Kein Respawn: druecke deine Respawn-Taste", "Nessun respawn: premi il tuo tasto di respawn" } },
    { { "Sans plugin", "Without plugin", "Sin plugin", "Ohne Plugin", "Senza plugin" } },
    { { "PILOTE ET EQUIPEMENT", "RIDER AND GEAR", "PILOTO Y EQUIPO", "FAHRER UND AUSRUESTUNG", "PILOTA E EQUIPAGGIAMENTO" } },
    { { "Pilote : hauteur", "Rider: height", "Piloto: altura", "Fahrer: Hoehe", "Pilota: altezza" } },
    { { "Casque : hauteur", "Helmet: height", "Casco: altura", "Helm: Hoehe", "Casco: altezza" } },
    { { "Casque : avant / arriere", "Helmet: forward / back", "Casco: delante / detras", "Helm: vor / zurueck", "Casco: avanti / indietro" } },
    { { "Casque : angle haut / bas", "Helmet: tilt up / down", "Casco: angulo arriba / abajo", "Helm: Neigung hoch / runter", "Casco: angolo su / giu" } },
    { { "Bottes : hauteur", "Boots: height", "Botas: altura", "Stiefel: Hoehe", "Stivali: altezza" } },
    { { "Bottes : avant / arriere", "Boots: forward / back", "Botas: delante / detras", "Stiefel: vor / zurueck", "Stivali: avanti / indietro" } },
    { { "Casque", "Helmet", "Casco", "Helm", "Casco" } },
    { { "Bottes", "Boots", "Botas", "Stiefel", "Stivali" } },
    { { "Pilote", "Rider", "Piloto", "Fahrer", "Pilota" } },
    { { "(pas encore vu en piste)", "(not seen on track yet)", "(aun no visto en pista)", "(noch nicht auf der Strecke gesehen)", "(non ancora visto in pista)" } },
    { { "TOUCHES", "KEYS", "TECLAS", "TASTEN", "TASTI" } },
    { { "Sensibilite souris", "Mouse sensitivity", "Sensibilidad del raton", "Mausempfindlichkeit", "Sensibilita mouse" } },
    { { "Avancer", "Forward", "Avanzar", "Vorwaerts", "Avanti" } },
    { { "Reculer", "Back", "Retroceder", "Rueckwaerts", "Indietro" } },
    { { "Gauche", "Left", "Izquierda", "Links", "Sinistra" } },
    { { "Droite", "Right", "Derecha", "Rechts", "Destra" } },
    { { "Courir", "Run", "Correr", "Laufen", "Correre" } },
    { { "Se relever", "Get up", "Levantarse", "Aufstehen", "Rialzarsi" } },
    { { "Relever / monter", "Lift bike / get on", "Levantar moto / subir", "Motorrad aufheben / aufsteigen", "Alza moto / sali" } },
    { { "Descendre", "Get off", "Bajarse", "Absteigen", "Scendere" } },
    { { "Vue 1re / 3e personne", "1st / 3rd person view", "Vista 1a / 3a persona", "1./3.-Person-Ansicht", "Vista 1a / 3a persona" } },
    { { "Sauter", "Jump", "Saltar", "Springen", "Saltare" } },
    { { "S'accroupir", "Crouch", "Agacharse", "Ducken", "Accovacciarsi" } },
    { { "Mises a jour automatiques", "Automatic updates", "Actualizaciones automaticas", "Automatische Updates", "Aggiornamenti automatici" } },
    { { "Multi : partager avec les autres joueurs du plugin", "Online: share with other plugin players",
        "Multijugador: compartir con otros jugadores del plugin", "Online: mit anderen Plugin-Spielern teilen",
        "Online: condividi con gli altri giocatori del plugin" } },
    { { "Activer le multi ?", "Turn on online play?", "Activar el multijugador?", "Online-Spiel einschalten?",
        "Attivare il gioco online?" } },
    { { "Sur les serveurs RAGDOLL, ton pilote, tes chutes et tes objets sont envoyes", "On RAGDOLL servers, your rider, crashes and objects are sent",
        "En servidores RAGDOLL, tu piloto, caidas y objetos se envian", "Auf RAGDOLL-Servern werden dein Fahrer, Stuerze und Objekte",
        "Sui server RAGDOLL, pilota, cadute e oggetti vengono inviati" } },
    { { "aux autres joueurs du plugin, et tu vois les leurs.", "to other plugin players, and you see theirs.",
        "a los otros jugadores del plugin, y ves los suyos.", "an andere Plugin-Spieler gesendet, und du siehst ihre.",
        "agli altri giocatori del plugin, e vedi i loro." } },
    { { "Entree / A : activer   -   Retour / B : non (menu P pour changer)", "Enter / A: turn on   -   Backspace / B: no (menu P to change)",
        "Intro / A: activar   -   Retroceso / B: no (menu P para cambiar)", "Enter / A: einschalten   -   Ruecktaste / B: nein (Menue P zum Aendern)",
        "Invio / A: attiva   -   Backspace / B: no (menu P per cambiare)" } },
    { { "Serveur RAGDOLL : ton multi est coupe (menu P pour l'activer).", "RAGDOLL server: your online play is off (menu P to turn it on).",
        "Servidor RAGDOLL: tu multijugador esta desactivado (menu P para activarlo).", "RAGDOLL-Server: dein Online-Spiel ist aus (Menue P zum Einschalten).",
        "Server RAGDOLL: il tuo gioco online e spento (menu P per attivarlo)." } },
    { { "Admin : plugin pas detecte chez", "Admin: plugin not detected for", "Admin: plugin no detectado en",
        "Admin: Plugin nicht erkannt bei", "Admin: plugin non rilevato per" } },
    { { "(pas installe ou multi coupe)", "(not installed or online play off)", "(no instalado o multijugador desactivado)",
        "(nicht installiert oder Online-Spiel aus)", "(non installato o gioco online spento)" } },
    { { "Expulser les joueurs sans plugin (mon serveur)", "Kick players without the plugin (my server)",
        "Expulsar a los jugadores sin el plugin (mi servidor)", "Spieler ohne Plugin kicken (mein Server)",
        "Espelli i giocatori senza plugin (mio server)" } },
    { { "Expulsion auto des joueurs sans plugin : COUPEE. A activer dans le menu P si tu heberges.",
        "Auto-kick of players without the plugin: OFF. Turn it on in menu P if you host.",
        "Expulsion automatica de jugadores sin el plugin: DESACTIVADA. Activala en el menu P si alojas.",
        "Auto-Kick von Spielern ohne Plugin: AUS. Im Menue P einschalten, wenn du hostest.",
        "Espulsione automatica dei giocatori senza plugin: SPENTA. Attivala nel menu P se ospiti." } },
    { { "Tu heberges : expulsion auto des joueurs sans plugin COUPEE (menu P pour l'activer).",
        "You are hosting: auto-kick of players without the plugin is OFF (turn it on in menu P).",
        "Estas alojando: expulsion automatica de jugadores sin el plugin DESACTIVADA (activala en el menu P).",
        "Du hostest: Auto-Kick von Spielern ohne Plugin ist AUS (im Menue P einschalten).",
        "Stai ospitando: espulsione automatica dei giocatori senza plugin SPENTA (attivala nel menu P)." } },
    { { "Tu heberges : les joueurs sans plugin seront expulses apres 90 s (menu P pour couper).",
        "You are hosting: players without the plugin will be kicked after 90 s (turn it off in menu P).",
        "Estas alojando: los jugadores sin el plugin seran expulsados tras 90 s (desactivalo en el menu P).",
        "Du hostest: Spieler ohne Plugin werden nach 90 s gekickt (im Menue P ausschalten).",
        "Stai ospitando: i giocatori senza plugin saranno espulsi dopo 90 s (disattiva nel menu P)." } },
    { { "Demander avant d'installer une mise a jour", "Ask before installing an update", "Preguntar antes de instalar una actualizacion",
        "Vor dem Installieren eines Updates fragen", "Chiedi prima di installare un aggiornamento" } },
    { { "Mise a jour disponible", "Update available", "Actualizacion disponible", "Update verfuegbar", "Aggiornamento disponibile" } },
    { { "MXRP Ragdoll %s est disponible (tu as la %s).", "MXRP Ragdoll %s is available (you have %s).",
        "MXRP Ragdoll %s esta disponible (tienes la %s).", "MXRP Ragdoll %s ist verfuegbar (du hast %s).",
        "MXRP Ragdoll %s e disponibile (hai la %s)." } },
    { { "Elle sera active au prochain lancement du jeu.", "It will be active the next time you start the game.",
        "Estara activa la proxima vez que inicies el juego.", "Sie ist beim naechsten Spielstart aktiv.",
        "Sara attivo al prossimo avvio del gioco." } },
    { { "Entree / A : installer   -   Retour / B : plus tard", "Enter / A: install   -   Backspace / B: later",
        "Intro / A: instalar   -   Retroceso / B: mas tarde", "Enter / A: installieren   -   Ruecktaste / B: spaeter",
        "Invio / A: installa   -   Backspace / B: piu tardi" } },
    { { "Mise a jour %s : plus tard (redemandee au prochain lancement)", "Update %s: later (asked again next launch)",
        "Actualizacion %s: mas tarde (se preguntara en el proximo inicio)", "Update %s: spaeter (beim naechsten Start erneut gefragt)",
        "Aggiornamento %s: piu tardi (richiesto al prossimo avvio)" } },
    { { "MXRP Ragdoll %s installe : actif au prochain lancement du jeu",
        "MXRP Ragdoll %s installed: active next time you start the game",
        "MXRP Ragdoll %s instalado: activo la proxima vez que inicies el juego",
        "MXRP Ragdoll %s installiert: aktiv beim naechsten Spielstart",
        "MXRP Ragdoll %s installato: attivo al prossimo avvio del gioco" } },
    { { "MXRP Ragdoll %s disponible : l'installateur s'ouvrira quand tu quitteras le jeu",
        "MXRP Ragdoll %s available: the installer will open when you quit the game",
        "MXRP Ragdoll %s disponible: el instalador se abrira al salir del juego",
        "MXRP Ragdoll %s verfuegbar: der Installer oeffnet sich beim Beenden des Spiels",
        "MXRP Ragdoll %s disponibile: l'installer si aprira quando esci dal gioco" } },
    { { "S'accroupir :", "Crouch:", "Agacharse:", "Ducken:", "Accovacciarsi:" } },
    { { "Clavier prio (bascule)", "Keyboard priority (toggle)", "Prioridad teclado (alternar)", "Tastatur-Vorrang (umschalten)", "Priorita tastiera (alterna)" } },
    { { "Manette prio (bascule)", "Gamepad priority (toggle)", "Prioridad mando (alternar)", "Controller-Vorrang (umschalten)", "Priorita controller (alterna)" } },
    { { "Ce menu", "This menu", "Este menu", "Dieses Menue", "Questo menu" } },
    { { "manette", "pad", "mando", "Pad", "pad" } },
    { { "OUI", "ON", "SI", "AN", "SI" } },
    { { "NON", "OFF", "NO", "AUS", "NO" } },
    { { "Haut/Bas : choisir      P : fermer et enregistrer", "Up/Down: select      P: close and save", "Arriba/Abajo: elegir      P: cerrar y guardar", "Hoch/Runter: waehlen      P: schliessen und speichern", "Su/Giu: scegli      P: chiudi e salva" } },
    { { "Gauche/Droite : regler (Maj = fin)   Suppr : 0", "Left/Right: adjust (Shift = fine)   Del: 0", "Izq/Der: ajustar (Mayus = fino)   Supr: 0", "Links/Rechts: einstellen (Umschalt = fein)   Entf: 0", "Sin/Des: regola (Maiusc = fine)   Canc: 0" } },
    { { "Gauche/Droite ou Entree : oui / non", "Left/Right or Enter: on / off", "Izq/Der o Intro: si / no", "Links/Rechts oder Enter: an / aus", "Sin/Des o Invio: si / no" } },
    { { "Gauche/Droite : regler (Maj = fin)", "Left/Right: adjust (Shift = fine)", "Izq/Der: ajustar (Mayus = fino)", "Links/Rechts: einstellen (Umschalt = fein)", "Sin/Des: regola (Maiusc = fine)" } },
    { { "Gauche/Droite : changer de langue", "Left/Right: change language", "Izq/Der: cambiar idioma", "Links/Rechts: Sprache wechseln", "Sin/Des: cambia lingua" } },
    { { "Appuie sur une touche ou un bouton...  (Echap : annuler)", "Press a key or a button...  (Esc: cancel)", "Pulsa una tecla o un boton...  (Esc: cancelar)", "Druecke eine Taste oder einen Knopf...  (Esc: abbrechen)", "Premi un tasto o un pulsante...  (Esc: annulla)" } },
    { { "Entree : changer la touche / le bouton", "Enter: change the key / button", "Intro: cambiar la tecla / el boton", "Enter: Taste / Knopf aendern", "Invio: cambia tasto / pulsante" } },
    /* v1.8.45 : cônes */
    { { "L'autre joueur n'a pas donne son cone", "The other player did not hand over the cone", "El otro jugador no dio su cono", "Der andere Spieler hat den Kegel nicht gegeben", "L'altro giocatore non ha dato il cono" } },
    { { "Reposer", "Put down", "Dejar", "Abstellen", "Posa" } },
    { { "le cone", "the cone", "el cono", "den Kegel", "il cono" } },
    { { "Supprimer", "Delete", "Borrar", "Loeschen", "Elimina" } },
    { { "le cone tenu", "the held cone", "el cono", "den Kegel", "il cono" } },
    { { "Ramasser le", "Pick up the", "Coger el", "Naechsten", "Raccogli il" } },
    { { "Cone supprime (%d)", "Cone deleted (%d)", "Cono borrado (%d)", "Kegel geloescht (%d)", "Cono eliminato (%d)" } },
    { { "Cone en main (%d)", "Cone in hand (%d)", "Cono en mano (%d)", "Kegel in der Hand (%d)", "Cono in mano (%d)" } },
    { { "sur place", "where they are", "en su sitio", "an Ort und Stelle", "sul posto" } },
    { { "Remettre", "Put back", "Devolver", "Zuruecksetzen", "Rimetti" } },
    { { "au depart", "at the start", "al inicio", "an den Start", "alla partenza" } },
    { { "%d cones remis au depart", "%d cones put back", "%d conos devueltos", "%d Kegel zurueckgesetzt", "%d coni rimessi" } },
    { { "Vise (stick, souris), A / clic : choisir   -   B : fermer", "Aim (stick, mouse), A / click: choose   -   B: close", "Apunta (stick, raton), A / clic: elegir   -   B: cerrar", "Zielen (Stick, Maus), A / Klick: waehlen   -   B: schliessen", "Mira (stick, mouse), A / clic: scegli   -   B: chiudi" } },
    { { "Vise (stick, souris), A / clic : choisir   -   LB / RB, fleches : page   -   B : retour", "Aim (stick, mouse), A / click: choose   -   LB / RB, arrows: page   -   B: back", "Apunta (stick, raton), A / clic: elegir   -   LB / RB, flechas: pagina   -   B: volver", "Zielen (Stick, Maus), A / Klick: waehlen   -   LB / RB, Pfeile: Seite   -   B: zurueck", "Mira (stick, mouse), A / clic: scegli   -   LB / RB, frecce: pagina   -   B: indietro" } },
    { { "Variantes", "Variants", "Variantes", "Varianten", "Varianti" } },
    { { "Vise (stick, souris), A / clic : choisir   -   LB / RB, fleches : page   -   B : fermer", "Aim (stick, mouse), A / click: choose   -   LB / RB, arrows: page   -   B: close", "Apunta (stick, raton), A / clic: elegir   -   LB / RB, flechas: pagina   -   B: cerrar", "Zielen (Stick, Maus), A / Klick: waehlen   -   LB / RB, Pfeile: Seite   -   B: schliessen", "Mira (stick, mouse), A / clic: scegli   -   LB / RB, frecce: pagina   -   B: chiudi" } },
    { { "Menu", "Menu", "Menu", "Menue", "Menu" } },
    { { "Variante", "Variant", "Variante", "Variante", "Variante" } },
    { { "Retour", "Back", "Volver", "Zurueck", "Indietro" } },
    { { "Classique", "Classic", "Clasico", "Klassisch", "Classico" } },
    { { "Tres sale", "Very dirty", "Muy sucio", "Sehr schmutzig", "Molto sporco" } },
    { { "Use", "Worn", "Usado", "Abgenutzt", "Usurato" } },
    { { "Delave", "Faded", "Destenido", "Verblasst", "Sbiadito" } },
    { { "Rouge vif", "Bright red", "Rojo vivo", "Knallrot", "Rosso vivo" } },
    { { "(choisie)", "(chosen)", "(elegida)", "(gewaehlt)", "(scelta)" } },
    { { "Variante %d choisie", "Variant %d chosen", "Variante %d elegida", "Variante %d gewaehlt", "Variante %d scelta" } },
    { { "Vise et valide (clic, A, Entree)   -   clic droit / B : retour", "Aim and select (click, A, Enter)   -   right click / B: back", "Apunta y acepta (clic, A, Intro)   -   clic derecho / B: volver", "Zielen und bestaetigen (Klick, A, Enter)   -   Rechtsklick / B: zurueck", "Mira e conferma (clic, A, Invio)   -   clic destro / B: indietro" } },
    { { "Poser", "Place", "Poner", "Aufstellen", "Piazza" } },
    { { "un cone", "a cone", "un cono", "einen Kegel", "un cono" } },
    { { "Relever", "Stand up", "Levantar", "Aufstellen", "Rialza" } },
    { { "les cones", "the cones", "los conos", "die Kegel", "i coni" } },
    { { "Retirer", "Remove", "Quitar", "Entfernen", "Rimuovi" } },
    { { "tous les cones", "all cones", "todos los conos", "alle Kegel", "tutti i coni" } },
    { { "Retirer le", "Remove the", "Quitar el", "Naechsten", "Rimuovi il" } },
    { { "plus proche", "nearest", "mas cercano", "entfernen", "piu vicino" } },
    { { "Cones", "Cones", "Conos", "Kegel", "Coni" } },
    /* v1.8.82 : bottes de foin */
    { { "Objets optimises (moins beaux)", "Optimized objects (less pretty)", "Objetos optimizados (menos bonitos)", "Optimierte Objekte (weniger schoen)", "Oggetti ottimizzati (meno belli)" } },
    { { "Porter la", "Carry the", "Llevar la", "Kleinen Ballen", "Porta la" } },
    { { "petite botte", "small bale", "paca pequena", "tragen", "balla piccola" } },
    { { "la botte", "the bale", "la paca", "den Ballen", "la balla" } },
    { { "la botte tenue", "the carried bale", "la paca llevada", "getragenen Ballen", "la balla portata" } },
    { { "Botte en main", "Bale in hand", "Paca en mano", "Ballen in der Hand", "Balla in mano" } },
    { { "Aucune petite botte a moins de %d m", "No small bale within %d m", "Ninguna paca pequena a menos de %d m", "Kein kleiner Ballen naeher als %d m", "Nessuna balla piccola entro %d m" } },
    { { "Bottes", "Hay", "Pacas", "Heu", "Balle" } },
    { { "de foin", "bales", "de heno", "ballen", "di fieno" } },
    { { "Bottes de foin", "Hay bales", "Pacas de heno", "Heuballen", "Balle di fieno" } },
    { { "une botte", "a bale", "una paca", "einen Ballen", "una balla" } },
    { { "Retirer la", "Remove the", "Quitar la", "Naechsten", "Rimuovi la" } },
    { { "toutes les bottes", "all bales", "todas las pacas", "alle Ballen", "tutte le balle" } },
    { { "Carree sanglee", "Strapped square", "Cuadrada atada", "Eckig, gegurtet", "Quadrata legata" } },
    { { "Carree", "Square", "Cuadrada", "Eckig", "Quadrata" } },
    { { "Petite", "Small", "Pequena", "Klein", "Piccola" } },
    { { "Table", "Table", "Mesa", "Tisch", "Tavolo" } },
    { { "Chaise", "Chair", "Silla", "Stuhl", "Sedia" } },
    { { "Chaise bois", "Wooden chair", "Silla de madera", "Holzstuhl", "Sedia di legno" } },
    { { "et mobilier", "and furniture", "y muebles", "und Moebel", "e mobili" } },
    { { "botte, table, chaise", "bale, table, chair", "paca, mesa, silla", "Ballen, Tisch, Stuhl", "balla, tavolo, sedia" } },
    { { "Bottes et mobilier", "Bales and furniture", "Pacas y muebles", "Ballen und Moebel", "Balle e mobili" } },
    { { "Objet", "Object", "Objeto", "Objekt", "Oggetto" } },
    { { "Porter", "Carry", "Llevar", "Tragen", "Portare" } },
    { { "Mobilier", "Furniture", "Muebles", "Moebel", "Mobili" } },
    { { "Chaise rouge", "Red chair", "Silla roja", "Roter Stuhl", "Sedia rossa" } },
    { { "%s en main", "%s in hand", "%s en la mano", "%s in der Hand", "%s in mano" } },
    { { "Ronde", "Round", "Redonda", "Rund", "Rotonda" } },
    { { "S'asseoir", "Sit down", "Sentarse", "Hinsetzen", "Sedersi" } },
    { { "sur la chaise", "on the chair", "en la silla", "auf den Stuhl", "sulla sedia" } },
    { { "Aucune chaise a moins de %d m", "No chair within %d m", "Ninguna silla a menos de %d m", "Kein Stuhl im Umkreis von %d m",
        "Nessuna sedia entro %d m" } },
    { { "table, chaises", "table, chairs", "mesa, sillas", "Tisch, Stuehle", "tavolo, sedie" } },
    { { "l'objet", "the object", "el objeto", "das Objekt", "l'oggetto" } },
    { { "l'objet tenu", "the held object", "el objeto llevado", "das gehaltene Objekt", "l'oggetto tenuto" } },
    { { "tout le mobilier", "all furniture", "todos los muebles", "alle Moebel", "tutti i mobili" } },
    { { "le plus proche", "the nearest", "el mas cercano", "das naechste", "il piu vicino" } },
    { { "Objet pose (%d)", "Object placed (%d)", "Objeto colocado (%d)", "Objekt gesetzt (%d)", "Oggetto posato (%d)" } },
    { { "Objet retire (%d)", "Object removed (%d)", "Objeto quitado (%d)", "Objekt entfernt (%d)", "Oggetto rimosso (%d)" } },
    { { "Aucun objet a moins de %d m", "No object within %d m", "Ningun objeto a menos de %d m", "Kein Objekt im Umkreis von %d m",
        "Nessun oggetto entro %d m" } },
    { { "%d objets remis au depart", "%d objects reset", "%d objetos devueltos al inicio", "%d Objekte zurueckgesetzt",
        "%d oggetti riportati all'inizio" } },
    { { "%d objets retires", "%d objects removed", "%d objetos quitados", "%d Objekte entfernt", "%d oggetti rimossi" } },
    { { "Rien a porter a moins de %d m", "Nothing to carry within %d m", "Nada que llevar a menos de %d m",
        "Nichts zum Tragen im Umkreis von %d m", "Niente da portare entro %d m" } },
    { { "Botte posee (%d)", "Bale placed (%d)", "Paca puesta (%d)", "Ballen gestellt (%d)", "Balla piazzata (%d)" } },
    { { "Botte retiree (%d)", "Bale removed (%d)", "Paca quitada (%d)", "Ballen entfernt (%d)", "Balla rimossa (%d)" } },
    { { "Aucune botte a moins de %d m", "No bale within %d m", "Ninguna paca a menos de %d m", "Kein Ballen naeher als %d m", "Nessuna balla entro %d m" } },
    { { "%d bottes remises au depart", "%d bales put back", "%d pacas devueltas", "%d Ballen zurueckgesetzt", "%d balle rimesse" } },
    { { "%d bottes retirees", "%d bales removed", "%d pacas quitadas", "%d Ballen entfernt", "%d balle rimosse" } },
    { { "Botte %d choisie", "Bale %d chosen", "Paca %d elegida", "Ballen %d gewaehlt", "Balla %d scelta" } },
    { { "Plus de place : %d bottes au plus", "No room: %d bales at most", "Sin sitio: %d pacas como maximo", "Kein Platz: hoechstens %d Ballen", "Niente spazio: %d balle al massimo" } },
    { { "Vise (souris, stick, fleches) et relache   -   clic droit / B : fermer", "Aim (mouse, stick, arrows) and release   -   right click / B: close", "Apunta (raton, stick, flechas) y suelta   -   clic derecho / B: cerrar", "Zielen (Maus, Stick, Pfeile) und loslassen   -   Rechtsklick / B: schliessen", "Mira (mouse, stick, frecce) e rilascia   -   clic destro / B: chiudi" } },
    { { "Cones  (%d poses)", "Cones  (%d placed)", "Conos  (%d puestos)", "Kegel  (%d gestellt)", "Coni  (%d piazzati)" } },
    { { "Poser un cone", "Place a cone", "Poner un cono", "Kegel aufstellen", "Piazza un cono" } },
    { { "Retirer le cone le plus proche", "Remove the nearest cone", "Quitar el cono mas cercano", "Naechsten Kegel entfernen", "Rimuovi il cono piu vicino" } },
    { { "Relever les cones", "Stand the cones back up", "Levantar los conos", "Kegel wieder aufstellen", "Rialza i coni" } },
    { { "Retirer tous les cones", "Remove all cones", "Quitar todos los conos", "Alle Kegel entfernen", "Rimuovi tutti i coni" } },
    { { "Fermer", "Close", "Cerrar", "Schliessen", "Chiudi" } },
    { { "Haut / Bas : choisir   Entree / A : valider   B : fermer", "Up / Down: choose   Enter / A: select   B: close", "Arriba / Abajo: elegir   Intro / A: aceptar   B: cerrar", "Hoch / Runter: waehlen   Enter / A: ok   B: schliessen", "Su / Giu: scegli   Invio / A: conferma   B: chiudi" } },
    { { "Cone pose (%d)", "Cone placed (%d)", "Cono puesto (%d)", "Kegel gestellt (%d)", "Cono piazzato (%d)" } },
    { { "Cone retire (%d)", "Cone removed (%d)", "Cono quitado (%d)", "Kegel entfernt (%d)", "Cono rimosso (%d)" } },
    { { "Aucun cone a moins de %d m", "No cone within %d m", "Ningun cono a menos de %d m", "Kein Kegel naeher als %d m", "Nessun cono entro %d m" } },
    { { "%d cones releves", "%d cones stood back up", "%d conos levantados", "%d Kegel aufgestellt", "%d coni rialzati" } },
    { { "%d cones retires", "%d cones removed", "%d conos quitados", "%d Kegel entfernt", "%d coni rimossi" } },
    { { "Plus de place : %d cones au plus", "No room: %d cones at most", "Sin sitio: %d conos como maximo", "Kein Platz: hoechstens %d Kegel", "Niente spazio: al massimo %d coni" } },
};

int lang_system(void)
{
    static int cached = -1;
    if (cached < 0) {
#ifdef _WIN32
        /* langue de l'interface de Windows (sinon celle de l'utilisateur) */
        LANGID id = GetUserDefaultUILanguage();
        if (!id)
            id = GetUserDefaultLangID();
        switch (PRIMARYLANGID(id)) {
        case LANG_FRENCH: cached = 0; break;
        case LANG_SPANISH: cached = 2; break;
        case LANG_GERMAN: cached = 3; break;
        case LANG_ITALIAN: cached = 4; break;
        default: cached = 1; break; /* anglais */
        }
#else
        cached = 1;
#endif
    }
    return cached;
}

void lang_set(int lang)
{
    if (lang == LANG_AUTO)
        lang = lang_system(); /* v1.8.118 */
    g_lang = lang >= 0 && lang < LANG_COUNT ? lang : LANG_FR;
}

int lang_get(void)
{
    return g_lang;
}

const char *lang_name(int lang)
{
    static const char *k_names[LANG_COUNT] = { "Francais", "English", "Espanol", "Deutsch", "Italiano" };
    static char b[40];
    if (lang == LANG_AUTO) {
        snprintf(b, sizeof(b), "Auto (%s)", k_names[lang_system()]);
        return b;
    }
    return lang >= 0 && lang < LANG_COUNT ? k_names[lang] : k_names[0];
}

const char *tr(const char *fr)
{
    size_t i;
    if (g_lang == LANG_FR || !fr)
        return fr;
    for (i = 0; i < sizeof(k_tab) / sizeof(k_tab[0]); ++i)
        if (!strcmp(k_tab[i].t[0], fr))
            return k_tab[i].t[g_lang];
    return fr;
}
