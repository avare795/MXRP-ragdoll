MXRP Ragdoll - Beta 3.3 (1.8.203)
Plugin pour MX Bikes beta21e : vraies chutes (ragdoll), se relever, marcher jusqu'a la moto, la relever et remonter, cones, bottes de foin et rampes FMX, en solo et en ligne.

Langues / Languages : English - Francais - Espanol - Deutsch - Italiano


==================================================================
ENGLISH
==================================================================

1. INSTALL (close MX Bikes first - beta21e, update of 23 Nov 2026 supported)
A) Automatic: run "MXRP Beta 3.3.exe" (MXRP_Beta_3.3.zip). It finds MX Bikes (Steam) by
   itself, shows the folder and lets you pick another one. It removes old versions
   (mxb_ragdoll.dlo, duplicate copies) and installs into the "plugins" folder:
   MXRP_Ragdoll.dlo, MXRP_Ragdoll.ini, this guide, and MXRP_Ragdoll.fnt (on-screen text
   font, copied from your own game files). Your settings are kept.
B) Manual (MXRP_Beta_3.3_manual.zip): copy the content of the zip's "plugins" folder
   into the "plugins" folder of MX Bikes (next to mxbikes.exe). Keep your own
   MXRP_Ragdoll.ini if you already have one. Remove old versions (mxb_ragdoll.dlo,
   "MXRP_Ragdoll (1).dlo"...), otherwise the plugin runs twice.
   Optional, for on-screen text: open the game's misc.pkz with 7-Zip, copy misc.fnt into
   "plugins" and rename it MXRP_Ragdoll.fnt.

2. TURN IT ON OR OFF
Click the "Plugin Ragdoll : ON / OFF" button in the game menus. The choice is remembered.

3. CONTROLS (the ragdoll starts by itself when you crash)
  Action                              Keyboard                  Gamepad
  Get up                              hold E or forward (2 s)   hold Y (2 s)
  Walk                                Z/W, S, Q/A, D            left stick
  Run                                 Ctrl                      RT
  Jump                                Space                     A
  Crouch (press again: stand up)      Shift                     right stick click
  1st / 3rd person view               V                         game view button
  Zoom (1st person, 4 levels, cycle)  mouse wheel / wheel click D-pad up
  Lift the bike (stand next to it)    E                         Y
  Get on the bike                     E again                   hold Y (2 s)
  Get off (bike stopped, offline)     F                         hold Y (0.8 s)
  Cones / hay bales wheel (on foot)   B                         D-pad down
  Settings                            P                         -
While you walk, the game respawn is blocked: walk back to your bike, or hold the
respawn button 20 s. Change keys in menu P or in the [touches] part of MXRP_Ragdoll.ini.

UPDATES: when a new version is out, the plugin downloads it in the background and asks
you on the track (Enter / A: install, Backspace / B: later). It is active the next time
you start the game. Menu P: "Automatic updates" (ON / OFF) and "Ask before installing an
update" (OFF = installed without asking). In 1st person on foot, the field of view is set
in menu P ("same as on the bike" by default).

4. CONES AND HAY BALES
- Open the wheel on foot (B / D-pad down). LB / RB change page, highlight then A.
- Cones (5 models): place, pick up, carry, put down (even on slopes) or delete. Stand
  them back up where they are, or put all cones back to their starting spot. Legs and
  bikes push them.
- Hay bales: the round bale is a solid wall. The small bale can be pushed, picked up,
  carried and put down. Bikes hit both.
- Furniture (wheel, "Furniture" page): a table and two camping chairs. Next to a chair,
  press the crouch key to sit down, press again to stand up.
- FMX ramps (wheel, "FMX ramps" page): 8 shapes (Kicker, F-Head, Long jump, Landing,
  Comp 8 ft, Super kicker, 120 ft jump, Quarter pipe), 4 colours. Place them anywhere:
  they follow the slope and become part of the track, like the bridges of the tracks:
  rideable at any height. Their sides and back are solid: bikes and riders bump into them.
  Ramps are shared online and shown in replays.
- First launch: a short tutorial (show it again in menu P). Gamepad buttons are drawn as
  Xbox or PlayStation symbols. The language follows Windows (Auto) unless you pick one.
- Menu P: gamepad buttons shown as Xbox or PlayStation (Auto / Xbox / PlayStation).

5. ONLINE
- Online play is OFF until you turn it on: the plugin asks you once ("Turn on online play?"),
  and menu P has "Online: share with other plugin players". While it is off, the plugin
  pauses itself on every online server and sends nothing.
- With the button ON, the plugin works everywhere: offline (practice and race) and online.
  When you host with the button ON, RAGDOLL is added to the server name for you: on a
  RAGDOLL server, players see each other's ragdolls, cones, bales and ramps.
- Nothing to set: plugins talk through the MX Bikes server connection and Steam.
- Every player needs the plugin. Hosting a RAGDOLL server, you can kick players without
  it automatically (60 s to install it): off by default, menu P.
- Players collide (bikes and riders on foot). Cones and hay bales are shared: everyone
  sees them and can push, pick up or remove them.

6. OFFICIAL SERVERS
Before playing on official servers (or any server that does not allow mods), REMOVE the
plugin: close the game, move MXRP_Ragdoll.dlo out of the "plugins" folder (for example
into a "plugins_off" folder), then start the game. Put it back to use it again.
The ON/OFF button is not enough: the plugin is still loaded by the game.

7. PROBLEMS
- Game crash or bug: send Documents\PiBoSo\MX Bikes\MXRP_Ragdoll\MXRP_Ragdoll.log BEFORE restarting
  the game (the last 5 logs are kept: MXRP_Ragdoll_1.log ... _5.log).
- New track: the ground height is measured while you ride, exact after your first stop.

Walking animation: CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu).
FMX ramps: PROCworks FMX Ramp PACK (PROCworks - Proud Racers Of Canada). Landing: "FMX Ramps" pack (fmxLanding). Comp, Super kicker, 120 ft jump, quarter pipe: "thefluffygecko fmx ramps".
Furniture: green camping chair by lpfeffer (CC-BY-4.0), folding table by Innerscene (CC0).


==================================================================
FRANCAIS
==================================================================

1. INSTALLATION (ferme MX Bikes avant - beta21e, mise a jour du 23/11/2026 comprise)
A) Automatique : lance "MXRP Beta 3.3.exe" (MXRP_Beta_3.3.zip). Il trouve MX Bikes (Steam)
   tout seul, montre le dossier et propose d'en choisir un autre. Il supprime les
   anciennes versions (mxb_ragdoll.dlo, copies en double) et installe dans le dossier
   "plugins" : MXRP_Ragdoll.dlo, MXRP_Ragdoll.ini, ce guide et MXRP_Ragdoll.fnt (police
   des textes, copiee depuis tes fichiers du jeu). Tes reglages sont gardes.
B) Manuelle (MXRP_Beta_3.3_manual.zip) : copie le contenu du dossier "plugins" du zip
   dans le dossier "plugins" de MX Bikes (a cote de mxbikes.exe). Garde ton propre
   MXRP_Ragdoll.ini si tu en as deja un. Enleve les anciennes versions (mxb_ragdoll.dlo,
   "MXRP_Ragdoll (1).dlo"...), sinon le plugin tourne en double.
   Facultatif, pour les textes a l'ecran : ouvre misc.pkz du jeu avec 7-Zip, copie
   misc.fnt dans "plugins" et renomme-le MXRP_Ragdoll.fnt.

2. ACTIVER OU COUPER
Clique sur le bouton "Plugin Ragdoll : ACTIF / COUPE" dans les menus du jeu. Le choix
est garde.

3. COMMANDES (le ragdoll demarre tout seul quand tu chutes)
  Action                              Clavier                   Manette
  Se relever                          garder E ou avancer (2 s) garder Y (2 s)
  Marcher                             Z/W, S, Q/A, D            stick gauche
  Courir                              Ctrl                      RT
  Sauter                              Espace                    A
  S'accroupir (encore : se relever)   Maj                       clic stick droit
  Vue 1re / 3e personne               V                         bouton de vue du jeu
  Zoom (1re personne, 4 crans)        molette / clic molette    croix Haut
  Relever la moto (a cote d'elle)     E                         Y
  Monter sur la moto                  E encore                  garder Y (2 s)
  Descendre (moto arretee, hors ligne) F                        garder Y (0,8 s)
  Roue cones / bottes (a pied)        B                         croix Bas
  Reglages                            P                         -
Pendant que tu marches, le respawn du jeu est bloque : retourne a ta moto, ou garde la
touche de respawn 20 s. Les touches se changent dans le menu P ou dans la partie
[touches] de MXRP_Ragdoll.ini.

MISES A JOUR : quand une nouvelle version sort, le plugin la telecharge en arriere-plan
et te demande en piste (Entree / A : installer, Retour / B : plus tard). Elle est active
au prochain lancement du jeu. Menu P : "Mises a jour automatiques" (OUI / NON) et
"Demander avant d'installer une mise a jour" (NON = installee sans demander). A pied en
1re personne, le champ de vision se regle dans le menu P ("comme sur la moto" par defaut).

4. CONES ET BOTTES DE FOIN
- Ouvre la roue a pied (B / croix Bas). LB / RB changent de page, surligne puis A.
- Cones (5 modeles) : poser, ramasser, porter, reposer (meme en pente) ou supprimer.
  Les relever sur place, ou remettre tous les cones a leur place de depart. Les jambes
  et les motos les poussent.
- Bottes de foin : la ronde est un mur. La petite se pousse, se ramasse, se porte et se
  repose. Les motos butent dans les deux.
- Mobilier (roue, page "Mobilier") : une table et deux chaises de camping. A cote d'une
  chaise, touche s'accroupir pour s'asseoir, encore une fois pour se relever.
- Rampes FMX (roue, page "Rampes FMX") : 8 formes (Kicker, F-Head, Grand saut,
  Reception, Comp 8 ft, Super kicker, Saut 120 ft, Quarter pipe), 4 couleurs. Pose-les
  ou tu veux : elles suivent la pente et font partie de la piste, comme les passerelles
  des circuits : roulables a n'importe quelle hauteur. Les cotes et l'arriere sont
  solides : motos et pilotes butent dedans. Partagees en ligne et visibles en replay.
- Premier lancement : un petit tutoriel (a revoir dans le menu P). Boutons de la manette
  dessines en symboles Xbox ou PlayStation. Langue de Windows (Auto) sauf si tu en choisis une.
- Menu P : boutons de la manette affiches facon Xbox ou PlayStation (Auto / Xbox / PlayStation).

5. EN LIGNE
- Le multi est COUPE tant que tu ne l'actives pas : le plugin te le demande une fois
  ("Activer le multi ?"), et le menu P a "Multi : partager avec les autres joueurs du plugin".
  Coupe, le plugin se met en pause sur tous les serveurs en ligne et n'envoie rien.
- Bouton ACTIF : le plugin marche partout, hors ligne (essai et course) et en ligne.
  Quand tu heberges avec le bouton ACTIF, RAGDOLL est ajoute au nom du serveur tout seul :
  sur un serveur RAGDOLL, les joueurs voient les ragdolls, cones, bottes et rampes des autres.
- Rien a regler : les plugins se parlent par la connexion du serveur MX Bikes et Steam.
- Chaque joueur doit avoir le plugin. Si tu heberges un serveur RAGDOLL, tu peux expulser
  automatiquement ceux qui ne l'ont pas (60 s pour l'installer) : coupe de base, menu P.
- Collisions entre joueurs (motos et pilotes a pied). Cones et bottes sont partages :
  tout le monde les voit et peut les pousser, les ramasser ou les retirer.

6. SERVEURS OFFICIELS
Avant de jouer sur les serveurs officiels (ou tout serveur qui n'autorise pas les mods),
ENLEVE le plugin : ferme le jeu, deplace MXRP_Ragdoll.dlo hors du dossier "plugins"
(par exemple dans un dossier "plugins_off"), puis lance le jeu. Remets-le pour
l'utiliser a nouveau. Le bouton ACTIF / COUPE ne suffit pas : le plugin reste charge
par le jeu.

7. PROBLEMES
- Crash du jeu ou bug : envoie Documents\PiBoSo\MX Bikes\MXRP_Ragdoll\MXRP_Ragdoll.log AVANT de
  relancer le jeu (les 5 derniers journaux sont gardes : MXRP_Ragdoll_1.log ... _5.log).
- Nouvelle piste : la hauteur du sol est mesuree en roulant, exacte apres ton premier arret.

Animation de marche : CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu).
Rampes FMX : PROCworks FMX Ramp PACK (PROCworks - Proud Racers Of Canada). Reception : pack "FMX Ramps" (fmxLanding). Comp, Super kicker, Saut 120 ft, Quarter pipe : "thefluffygecko fmx ramps".
Mobilier : chaise de camping verte par lpfeffer (CC-BY-4.0), table pliante par Innerscene (CC0).


==================================================================
ESPANOL
==================================================================

1. INSTALACION (cierra MX Bikes antes - beta21e, actualizacion del 23/11/2026 incluida)
A) Automatica: ejecuta "MXRP Beta 3.3.exe" (MXRP_Beta_3.3.zip). Encuentra MX Bikes
   (Steam) solo, muestra la carpeta y permite elegir otra. Borra las versiones antiguas
   (mxb_ragdoll.dlo, copias duplicadas) e instala en la carpeta "plugins":
   MXRP_Ragdoll.dlo, MXRP_Ragdoll.ini, esta guia y MXRP_Ragdoll.fnt (fuente de los
   textos, copiada de tus archivos del juego). Tus ajustes se conservan.
B) Manual (MXRP_Beta_3.3_manual.zip): copia el contenido de la carpeta "plugins" del
   zip en la carpeta "plugins" de MX Bikes (junto a mxbikes.exe). Conserva tu propio
   MXRP_Ragdoll.ini si ya tienes uno. Quita las versiones antiguas (mxb_ragdoll.dlo,
   "MXRP_Ragdoll (1).dlo"...), si no el plugin funciona dos veces.
   Opcional, para los textos en pantalla: abre misc.pkz del juego con 7-Zip, copia
   misc.fnt en "plugins" y renombralo MXRP_Ragdoll.fnt.

2. ACTIVAR O DESACTIVAR
Haz clic en el boton "Plugin Ragdoll : ON / OFF" en los menus del juego. La eleccion
se guarda.

3. CONTROLES (el ragdoll empieza solo cuando te caes)
  Accion                              Teclado                   Mando
  Levantarse                          mantener E o avanzar (2 s) mantener Y (2 s)
  Andar                               Z/W, S, Q/A, D            stick izquierdo
  Correr                              Ctrl                      RT
  Saltar                              Espacio                   A
  Agacharse (otra vez: levantarse)    Mayus                     clic stick derecho
  Vista 1a / 3a persona               V                         boton de vista del juego
  Zoom (1a persona, 4 niveles)        rueda / clic rueda        cruceta arriba
  Levantar la moto (a su lado)        E                         Y
  Subir a la moto                     E otra vez                mantener Y (2 s)
  Bajar (moto parada, sin conexion)   F                         mantener Y (0,8 s)
  Rueda conos / pacas (a pie)         B                         cruceta abajo
  Ajustes                             P                         -
Mientras andas, el respawn del juego esta bloqueado: vuelve a tu moto o manten el boton
de respawn 20 s. Cambia las teclas en el menu P o en la parte [touches] de
MXRP_Ragdoll.ini.

ACTUALIZACIONES: cuando sale una version nueva, el plugin la descarga en segundo plano
y te pregunta en la pista (Intro / A: instalar, Retroceso / B: mas tarde). Estara activa
la proxima vez que inicies el juego. Menu P: "Actualizaciones automaticas" (SI / NO) y
"Preguntar antes de instalar una actualizacion" (NO = se instala sin preguntar). A pie en
1a persona, el campo de vision se ajusta en el menu P ("como en la moto" por defecto).

4. CONOS Y PACAS DE HENO
- Abre la rueda a pie (B / cruceta abajo). LB / RB cambian de pagina, resalta y pulsa A.
- Conos (5 modelos): poner, coger, llevar, dejar (incluso en pendiente) o borrar.
  Levantarlos donde estan, o devolver todos los conos a su sitio inicial. Las piernas
  y las motos los empujan.
- Pacas de heno: la redonda es un muro. La pequena se empuja, se coge, se lleva y se
  deja. Las motos chocan con las dos.
- Muebles (rueda, pagina "Muebles"): una mesa y dos sillas de camping. Junto a una silla,
  la tecla de agacharse para sentarse, otra vez para levantarse.
- Rampas FMX (rueda, pagina "Rampas FMX"): 8 formas y 4 colores. Colocalas donde
  quieras: siguen la pendiente y forman parte de la pista, como los puentes de los
  circuitos: se ruedan a cualquier altura. Los lados y la parte trasera son solidos.
  Compartidas en linea y visibles en las repeticiones.
- Primer inicio: un pequeno tutorial (se puede volver a ver en el menu P). Botones del
  mando dibujados como Xbox o PlayStation. Idioma de Windows (Auto) si no eliges uno.
- Menu P: botones del mando al estilo Xbox o PlayStation (Auto / Xbox / PlayStation).

5. EN LINEA
- El multijugador esta DESACTIVADO hasta que lo actives: el plugin lo pregunta una vez y el
  menu P tiene la opcion. Desactivado, el plugin se pausa en todos los servidores y no envia nada.
- Con el boton en ON, el plugin funciona en todas partes, sin conexion y en linea. Si
  alojas con el boton en ON, RAGDOLL se anade al nombre: en un servidor RAGDOLL los
  jugadores ven los ragdolls, conos, pacas y rampas de los demas.
- Nada que ajustar: los plugins se comunican por la conexion del servidor MX Bikes y Steam.
- Cada jugador necesita el plugin. Si alojas un servidor RAGDOLL, puedes expulsar
  automaticamente a quien no lo tiene (60 s para instalarlo): desactivado por defecto, menu P.
- Colisiones entre jugadores (motos y pilotos a pie). Conos y pacas se comparten: todos
  los ven y pueden empujarlos, cogerlos o quitarlos.

6. SERVIDORES OFICIALES
Antes de jugar en servidores oficiales (o cualquier servidor que no permita mods), QUITA
el plugin: cierra el juego, mueve MXRP_Ragdoll.dlo fuera de la carpeta "plugins" (por
ejemplo a una carpeta "plugins_off") y abre el juego. Vuelve a ponerlo para usarlo.
El boton ON / OFF no basta: el juego sigue cargando el plugin.

7. PROBLEMAS
- Cierre del juego o fallo: envia Documents\PiBoSo\MX Bikes\MXRP_Ragdoll\MXRP_Ragdoll.log ANTES de
  volver a abrir el juego (se guardan los 5 ultimos: MXRP_Ragdoll_1.log ... _5.log).
- Pista nueva: la altura del suelo se mide mientras ruedas, exacta tras tu primera parada.

Animacion de andar: CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu).


==================================================================
DEUTSCH
==================================================================

1. INSTALLATION (vorher MX Bikes schliessen - beta21e, Update vom 23.11.2026 inklusive)
A) Automatisch: "MXRP Beta 3.3.exe" starten (MXRP_Beta_3.3.zip). Es findet MX Bikes
   (Steam) selbst, zeigt den Ordner und laesst dich einen anderen waehlen. Es entfernt
   alte Versionen (mxb_ragdoll.dlo, doppelte Kopien) und installiert in den Ordner
   "plugins": MXRP_Ragdoll.dlo, MXRP_Ragdoll.ini, diese Anleitung und MXRP_Ragdoll.fnt
   (Schrift fuer Bildschirmtext, aus deinen Spieldateien kopiert). Deine Einstellungen
   bleiben erhalten.
B) Manuell (MXRP_Beta_3.3_manual.zip): den Inhalt des Ordners "plugins" aus dem Zip in
   den Ordner "plugins" von MX Bikes kopieren (neben mxbikes.exe). Deine eigene
   MXRP_Ragdoll.ini behalten, falls vorhanden. Alte Versionen entfernen
   (mxb_ragdoll.dlo, "MXRP_Ragdoll (1).dlo"...), sonst laeuft das Plugin doppelt.
   Optional, fuer Bildschirmtext: misc.pkz des Spiels mit 7-Zip oeffnen, misc.fnt nach
   "plugins" kopieren und in MXRP_Ragdoll.fnt umbenennen.

2. EIN- UND AUSSCHALTEN
Im Spielmenue auf "Plugin Ragdoll : ON / OFF" klicken. Die Wahl wird gespeichert.

3. STEUERUNG (der Ragdoll startet bei einem Sturz von selbst)
  Aktion                              Tastatur                  Gamepad
  Aufstehen                           E oder vorwaerts halten (2 s) Y halten (2 s)
  Gehen                               Z/W, S, Q/A, D            linker Stick
  Laufen                              Strg                      RT
  Springen                            Leertaste                 A
  Ducken (nochmal: aufstehen)         Umschalt                  rechten Stick druecken
  Ansicht 1. / 3. Person              V                         Ansichtstaste des Spiels
  Zoom (1. Person, 4 Stufen)          Mausrad / Mausradklick    Steuerkreuz oben
  Motorrad aufheben (daneben stehen)  E                         Y
  Aufsteigen                          nochmal E                 Y halten (2 s)
  Absteigen (steht, nur offline)      F                         Y halten (0,8 s)
  Rad Kegel / Heuballen (zu Fuss)     B                         Steuerkreuz unten
  Einstellungen                       P                         -
Beim Gehen ist der Respawn des Spiels gesperrt: geh zurueck zum Motorrad oder halte die
Respawn-Taste 20 s. Tasten aenderst du im Menue P oder im Teil [touches] von
MXRP_Ragdoll.ini.

UPDATES: Wenn eine neue Version erscheint, laedt das Plugin sie im Hintergrund herunter
und fragt dich auf der Strecke (Enter / A: installieren, Ruecktaste / B: spaeter). Sie ist
beim naechsten Spielstart aktiv. Menue P: "Automatische Updates" (AN / AUS) und "Vor dem
Installieren eines Updates fragen" (AUS = ohne Nachfrage installiert). Zu Fuss in der
1. Person stellst du das Sichtfeld im Menue P ein (Standard: "wie auf dem Motorrad").

4. KEGEL UND HEUBALLEN
- Rad zu Fuss oeffnen (B / Steuerkreuz unten). LB / RB wechseln die Seite, markieren
  und A druecken.
- Kegel (5 Modelle): aufstellen, aufheben, tragen, abstellen (auch am Hang) oder
  loeschen. An Ort und Stelle aufrichten oder alle Kegel an ihren Startplatz
  zuruecksetzen. Beine und Motorraeder schieben sie.
- Heuballen: der runde Ballen ist eine Wand. Der kleine laesst sich schieben, aufheben,
  tragen und abstellen. Motorraeder prallen gegen beide.
- Moebel (Rad, Seite "Moebel"): ein Tisch und zwei Campingstuehle. Neben einem Stuhl
  mit der Ducken-Taste hinsetzen, nochmal zum Aufstehen.
- Menue P: Controller-Tasten im Xbox- oder PlayStation-Stil (Auto / Xbox / PlayStation).

- FMX-Rampen (Rad, Seite "FMX-Rampen"): 8 Formen und 4 Farben. Ueberall platzierbar:
  sie folgen dem Gelaende und werden Teil der Strecke, wie die Bruecken der Strecken:
  in jeder Hoehe befahrbar. Seiten und Rueckseite sind fest. Online geteilt und in
  Wiederholungen sichtbar.
- Erster Start: ein kurzes Tutorial (im Menue P erneut anzeigen). Gamepad-Tasten als
  Xbox- oder PlayStation-Symbole. Sprache wie Windows (Auto), ausser du waehlst eine.
5. ONLINE
- Online-Spiel ist AUS, bis du es einschaltest: das Plugin fragt einmal, und Menue P hat die
  Option. Aus pausiert das Plugin auf allen Online-Servern und sendet nichts.
- Mit ON funktioniert das Plugin ueberall, offline und online. Wenn du mit ON hostest,
  wird RAGDOLL zum Namen hinzugefuegt: auf einem RAGDOLL-Server sehen die Spieler
  Ragdolls, Kegel, Ballen und Rampen der anderen.
- Nichts einzustellen: die Plugins reden ueber die MX-Bikes-Serververbindung und Steam.
- Jeder Spieler braucht das Plugin. Als Host eines RAGDOLL-Servers kannst du Spieler ohne
  Plugin automatisch kicken (60 s Zeit): standardmaessig aus, Menue P.
- Kollisionen zwischen Spielern (Motorraeder und Fahrer zu Fuss). Kegel und Heuballen
  werden geteilt: alle sehen sie und koennen sie schieben, aufheben oder entfernen.

6. OFFIZIELLE SERVER
Vor dem Spielen auf offiziellen Servern (oder Servern ohne Mods) das Plugin ENTFERNEN:
Spiel schliessen, MXRP_Ragdoll.dlo aus dem Ordner "plugins" verschieben (zum Beispiel in
einen Ordner "plugins_off"), dann das Spiel starten. Zum Benutzen wieder zuruecklegen.
Der ON / OFF-Knopf reicht nicht: das Spiel laedt das Plugin trotzdem.

7. PROBLEME
- Absturz oder Fehler: Documents\PiBoSo\MX Bikes\MXRP_Ragdoll\MXRP_Ragdoll.log schicken, BEVOR du das
  Spiel neu startest (die letzten 5 bleiben: MXRP_Ragdoll_1.log ... _5.log).
- Neue Strecke: die Bodenhoehe wird beim Fahren gemessen, genau nach dem ersten Halt.

Geh-Animation: CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu).


==================================================================
ITALIANO
==================================================================

1. INSTALLAZIONE (chiudi prima MX Bikes - beta21e, aggiornamento del 23/11/2026 incluso)
A) Automatica: avvia "MXRP Beta 3.3.exe" (MXRP_Beta_3.3.zip). Trova MX Bikes (Steam) da
   solo, mostra la cartella e ti lascia sceglierne un'altra. Rimuove le vecchie versioni
   (mxb_ragdoll.dlo, copie doppie) e installa nella cartella "plugins":
   MXRP_Ragdoll.dlo, MXRP_Ragdoll.ini, questa guida e MXRP_Ragdoll.fnt (carattere dei
   testi, copiato dai tuoi file del gioco). Le tue impostazioni restano.
B) Manuale (MXRP_Beta_3.3_manual.zip): copia il contenuto della cartella "plugins" dello
   zip nella cartella "plugins" di MX Bikes (accanto a mxbikes.exe). Tieni il tuo
   MXRP_Ragdoll.ini se ne hai gia uno. Togli le vecchie versioni (mxb_ragdoll.dlo,
   "MXRP_Ragdoll (1).dlo"...), altrimenti il plugin gira due volte.
   Facoltativo, per i testi a schermo: apri misc.pkz del gioco con 7-Zip, copia misc.fnt
   in "plugins" e rinominalo MXRP_Ragdoll.fnt.

2. ATTIVARE O DISATTIVARE
Clicca sul pulsante "Plugin Ragdoll : ON / OFF" nei menu del gioco. La scelta viene
salvata.

3. COMANDI (il ragdoll parte da solo quando cadi)
  Azione                              Tastiera                  Controller
  Rialzarsi                           tieni E o avanti (2 s)    tieni Y (2 s)
  Camminare                           Z/W, S, Q/A, D            stick sinistro
  Correre                             Ctrl                      RT
  Saltare                             Spazio                    A
  Accovacciarsi (di nuovo: in piedi)  Maiusc                    clic stick destro
  Vista 1a / 3a persona               V                         tasto vista del gioco
  Zoom (1a persona, 4 livelli)        rotella / clic rotella    croce su
  Rialzare la moto (accanto a lei)    E                         Y
  Salire sulla moto                   di nuovo E                tieni Y (2 s)
  Scendere (moto ferma, offline)      F                         tieni Y (0,8 s)
  Ruota coni / balle (a piedi)        B                         croce giu
  Impostazioni                        P                         -
Mentre cammini, il respawn del gioco e bloccato: torna alla moto o tieni il tasto di
respawn 20 s. Cambia i tasti nel menu P o nella parte [touches] di MXRP_Ragdoll.ini.

AGGIORNAMENTI: quando esce una nuova versione, il plugin la scarica in background e te
lo chiede in pista (Invio / A: installa, Backspace / B: piu tardi). Sara attiva al
prossimo avvio del gioco. Menu P: "Aggiornamenti automatici" (SI / NO) e "Chiedi prima di
installare un aggiornamento" (NO = installato senza chiedere). A piedi in 1a persona, il
campo visivo si regola nel menu P ("come sulla moto" di default).

4. CONI E BALLE DI FIENO
- Apri la ruota a piedi (B / croce giu). LB / RB cambiano pagina, evidenzia e premi A.
- Coni (5 modelli): piazza, raccogli, porta, posa (anche in pendenza) o elimina.
  Rialzali sul posto, o rimetti tutti i coni al loro posto di partenza. Gambe e moto
  li spingono.
- Balle di fieno: quella rotonda e un muro. Quella piccola si spinge, si raccoglie, si
  porta e si posa. Le moto urtano contro entrambe.
- Mobili (ruota, pagina "Mobili"): un tavolo e due sedie da campeggio. Vicino a una
  sedia, il tasto per accovacciarsi per sedersi, di nuovo per alzarsi.
- Rampe FMX (ruota, pagina "Rampe FMX"): 8 forme e 4 colori. Posizionale dove vuoi:
  seguono la pendenza e diventano parte della pista, come i ponti dei circuiti: si
  guidano a qualsiasi altezza. Lati e retro sono solidi. Condivise online e visibili
  nei replay.
- Primo avvio: un breve tutorial (da rivedere nel menu P). Tasti del controller disegnati
  come Xbox o PlayStation. Lingua di Windows (Auto) se non ne scegli una.
- Menu P: tasti del controller in stile Xbox o PlayStation (Auto / Xbox / PlayStation).

5. ONLINE
- Il gioco online e SPENTO finche non lo attivi: il plugin lo chiede una volta e il menu P ha
  l'opzione. Spento, il plugin si mette in pausa su tutti i server online e non invia nulla.
- Con il pulsante su ON il plugin funziona ovunque, offline e online. Se ospiti con ON,
  RAGDOLL viene aggiunto al nome: su un server RAGDOLL i giocatori vedono ragdoll, coni,
  balle e rampe degli altri.
- Niente da impostare: i plugin comunicano tramite la connessione del server MX Bikes e
  Steam.
- Ogni giocatore deve avere il plugin. Se ospiti un server RAGDOLL, puoi espellere
  automaticamente chi non ce l'ha (60 s per installarlo): spento di default, menu P.
- Collisioni tra giocatori (moto e piloti a piedi). Coni e balle sono condivisi: tutti
  li vedono e possono spingerli, raccoglierli o toglierli.

6. SERVER UFFICIALI
Prima di giocare sui server ufficiali (o su server che non permettono mod), TOGLI il
plugin: chiudi il gioco, sposta MXRP_Ragdoll.dlo fuori dalla cartella "plugins" (per
esempio in una cartella "plugins_off"), poi avvia il gioco. Rimettilo per usarlo di
nuovo. Il pulsante ON / OFF non basta: il gioco carica comunque il plugin.

7. PROBLEMI
- Crash o bug: invia Documents\PiBoSo\MX Bikes\MXRP_Ragdoll\MXRP_Ragdoll.log PRIMA di riavviare
  il gioco (restano gli ultimi 5: MXRP_Ragdoll_1.log ... _5.log).
- Pista nuova: l'altezza del terreno si misura mentre guidi, esatta dopo la prima sosta.

Animazione della camminata: CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu).
