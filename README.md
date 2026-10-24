# CellNet — réseau cellulaire local à trois ESP32 (RS485 + Wi‑Fi)

Prototype fonctionnel d'un réseau local composé de **trois stations ESP32**
(`SITE_A`, `SITE_B`, `SITE_C`) reliées par un **bus RS485 half‑duplex partagé**,
chacune diffusant son propre **point d'accès Wi‑Fi**, et d'une **application
Android** qui échange des messages texte et vocaux entre les cellules.

Aucun serveur central, aucun routeur, aucun accès Internet n'est nécessaire au
fonctionnement : le téléphone se connecte au Wi‑Fi d'une cellule, la station
achemine les messages sur le bus RS485 vers la station de destination, qui les
remet au téléphone destinataire.

---

## 0. État réel du prototype (à lire avant tout)

Ce dépôt contient du code **compilé et testé pour la partie logicielle**, pas un
système validé sur banc avec trois cartes physiques.

| Élément | État |
|---|---|
| Compilation `site_a`, `site_b`, `site_c` | ✅ réalisée sur ce poste (PlatformIO 6.2.0, espressif32 6.6.0) — RAM 27,1 %, Flash 65,2 % |
| Tests unitaires du cœur de transport, des appels, de la dorsale Wi‑Fi, de l'identité mobile et de la messagerie intercellulaire (124 tests) | ✅ exécutés sur ce poste (`pio test -e native`) — 124/124 |
| Tests unitaires JVM de l'application Android (77 tests) | ✅ **exécutés** sur ce poste (JDK 17.0.20.1 + Gradle 8.7 + SDK 34) — 77/77 |
| Compilation de l'application Android | ✅ `testDebugUnitTest assembleDebug assembleRelease` → `BUILD SUCCESSFUL` (`app-debug.apk` 15,9 Mo, `app-release.apk` 10,3 Mo) |
| Génération des trois binaires distincts | ✅ vérifiée (empreintes SHA‑256 différentes) |
| Appels audio en direct, même cellule | ✅ implémentés (signalisation HTTP + média WebSocket binaire local) |
| **Appels audio en direct entre cellules** | ✅ **implémentés** : dorsale IP TCP `WIFI_AP_STA` entre les trois stations (port 8082), signalisation et audio relayés par IP, jamais par le bus |
| Tests RS485 entre deux / trois cartes | ❌ non réalisés (pas de matériel accessible depuis cet environnement) |
| Tests Wi‑Fi réels, téléphone ↔ station | ❌ non réalisés |
| Tests audio de bout en bout | ❌ non réalisés |
| Appel réel entre deux téléphones (même cellule et intercellulaire) | ❌ non réalisé (aucun téléphone ni carte accessible depuis cet environnement) |

Les sections « Tests » et « Limitations » précisent ce qui reste à valider sur
le matériel.

---

## 1. Objectif

Démontrer une infrastructure de messagerie locale :

* trois cellules Wi‑Fi indépendantes, sans routeur ni Internet ;
* transport inter‑cellules explicite sur un bus RS485 partagé (pas de tunnel IP) ;
* découverte des utilisateurs et routage par table de présence ;
* messages texte avec accusés de livraison et de lecture ;
* messages vocaux de **60 secondes maximum**, stockés sur le téléphone et
  transmis par blocs ;
* états réels (en attente, transmis, livré, lu/écouté, échec) ;
* LED d'activité représentant l'émission et la réception **réelles** sur le bus.

---

## 2. Architecture

### 2.1 Les trois cellules

| Cellule | Environnement PlatformIO | Nœud RS485 | SSID | Adresse du point d'accès | HTTP | WebSocket |
|---|---|---|---|---|---|---|
| Cellule A | `site_a` | 1 | `CELL_SITE_A` | `10.0.0.1` | 8080 | 8081 |
| Cellule B | `site_b` | 2 | `CELL_SITE_B` | `10.1.0.1` | 8080 | 8081 |
| Cellule C | `site_c` | 3 | `CELL_SITE_C` | `10.2.0.1` | 8080 | 8081 |

Chaque ESP32 exécute **le même firmware** ; seuls les paramètres de compilation
changent (`platformio.ini`). Aucune duplication de code source.

### 2.2 Rôle du Wi‑Fi et du RS485

* **Wi‑Fi (AP local)** : accès des téléphones, identification de la cellule,
  service applicatif REST + WebSocket, découverte des utilisateurs.
* **RS485 (bus partagé)** : transport inter‑cellules. Les stations s'échangent
  des trames binaires explicites, avec adressage de nœud, CRC, accusés et
  arbitrage d'accès au bus.

### 2.3 Schéma logique du routage

Message d'un utilisateur connecté à `SITE_A` vers un utilisateur de `SITE_C` :

```
Téléphone A ──Wi-Fi CELL_SITE_A──> ESP32 SITE_A ──RS485──> ESP32 SITE_C ──Wi-Fi CELL_SITE_C──> Téléphone C
        <── accusés (livré / lu) ── ESP32 SITE_C <──RS485── ESP32 SITE_A <── accusés ── Téléphone C
```

Le chemin inverse est utilisé pour les accusés. `SITE_B` participe au bus (jeton,
présence, routage) et peut transmettre ses propres messages ; le bus étant un
média partagé, toute station voit passer les trames mais **ne traite que celles
qui lui sont adressées**.

Un message entre deux utilisateurs de la **même** cellule n'utilise pas le bus :
la station le remet directement au téléphone destinataire connecté à son AP.
---

## 3. Matériel et câblage imposé

### 3.1 Liste du matériel

* 3 × ESP32 (module `esp32dev`, 4 Mo de flash).
* 3 × module RS485 compatible **3,3 V** (transceiver type MAX3485 / SP3485,
  ou module MAX485 alimenté en 3,3 V **uniquement s'il est donné compatible**).
* 6 × LED (une verte + une rouge par station) + 6 × résistance 220 Ω.
* 2 × résistance de terminaison 120 Ω (une seule par extrémité du bus).
* Fils, plaquettes, alimentation 5 V ou USB pour chaque ESP32.

### 3.2 Tableau de câblage (identique pour les trois stations)

| Fonction | ESP32 | Module RS485 / LED |
|---|---|---|
| Alimentation RS485 | `3V3` | `VCC` (compatible 3,3 V) |
| Masse | `GND` | `GND` + cathodes des deux LED |
| Transmission UART | `GPIO17` / `TX2` | `DI` |
| Réception UART | `GPIO16` / `RX2` | `RO` |
| Direction RS485 | `GPIO4` | `DE` **et** `RE` reliées ensemble |
| LED verte (émission réelle) | `GPIO25` | anode via résistance 220 Ω |
| LED rouge (réception réelle) | `GPIO26` | anode via résistance 220 Ω |
| Conducteur A | — | `A` ↔ `A` ↔ `A` (les trois modules) |
| Conducteur B | — | `B` ↔ `B` ↔ `B` (les trois modules) |

Rappels :

* Les LED sont **actives à l'état haut** : `GPIO25 = HIGH` allume la verte,
  `GPIO26 = HIGH` allume la rouge.
* `GPIO4 = HIGH` ⇒ émission ; `GPIO4 = LOW` ⇒ réception.
* Le firmware attend la fin **réelle** de la transmission UART (buffer matériel
  vide) avant de remettre `GPIO4` à `LOW`.
* Ces broches sont figées dans `src/common/station_config.h`
  (`kRs485TxPin`, `kRs485RxPin`, `kRs485DirPin`, `kLedTxPin`, `kLedRxPin`) et ne
  doivent pas être réaffectées.

### 3.3 Précautions électriques

* **Le module RS485 doit être compatible 3,3 V.** Un MAX485 alimenté en 5 V
  présente une sortie `RO` proche de 5 V : elle n'est pas compatible avec les
  entrées de l'ESP32 et nécessite une adaptation de niveau (diviseur, buffer
  level‑shifter). Le logiciel ne corrige pas une incompatibilité électrique.
* Relier tous les `GND` ensemble (masse commune des trois stations).
* Ne jamais alimenter le module RS485 en 5 V depuis l'ESP32 : `3V3` uniquement.
* Vérifier la sérigraphie du module (`DI`, `RO`, `DE`, `RE`, `A`, `B`) : certains
  modules inversent `A` et `B`. En cas de non‑communication, essayer d'inverser
  `A`/`B` **sur une seule station** pour diagnostiquer.

### 3.4 Terminaisons du bus

* `SITE_A` : **120 Ω entre A et B** (extrémité physique du bus).
* `SITE_B` : **aucune terminaison**.
* `SITE_C` : **120 Ω entre A et B** (autre extrémité).

Les trois modules sont sur le **même** bus différentiel. Ne pas créer trois
liaisons point‑à‑point et ne pas ajouter de terminaison sur `SITE_B`.
`platformio.ini` porte l'information documentaire via
`-DAPP_RS485_TERMINATED=1/0/1`, affichée dans `/api/info` et dans les logs.

### 3.5 Débit et longueur

Le bus est configuré à **115200 bit/s, 8N1** (`kRs485Baud`). C'est compatible
avec les modules MAX3485/SP3485 et avec un câblage de quelques mètres à quelques
dizaines de mètres. Une paire torsadée améliore nettement la fiabilité.
Réduire à 57600 ou 38400 si le câblage est long ou bruité (modifier
`kRs485Baud`, la valeur est commune aux trois firmwares).

---

## 4. Identité et configuration des stations

### 4.1 Trois environnements, un seul code

`platformio.ini` définit `site_a`, `site_b`, `site_c` qui héritent de la section
`[firmware]` et ne changent que des `build_flags` :

| Macro | Rôle | Valeurs |
|---|---|---|
| `SITE_NODE_ID` | identifiant de nœud sur le bus | 1 / 2 / 3 |
| `SITE_ID` | identifiant stable affiché | `SITE_A` / `SITE_B` / `SITE_C` |
| `APP_WIFI_SSID` | SSID du point d'accès | `CELL_SITE_A` … |
| `APP_WIFI_PASSWORD` | mot de passe WPA2 (8 caractères min.) | `cellsitea` … |
| `APP_AP_IP` | adresse statique de l'AP | `10.0.0.1` / `10.1.0.1` / `10.2.0.1` |
| `APP_SERVICE_PORT` | port HTTP | 8080 |
| `APP_WS_PORT` | port WebSocket | 8081 |
| `APP_RS485_TERMINATED` | terminaison 120 Ω installée | 1 / 0 / 1 |

Pour renommer une cellule ou changer un mot de passe, modifier **uniquement**
`platformio.ini`, puis recompiler et reflasher la station concernée. Côté
Android, les mêmes informations sont centralisées dans
`mobile/app/src/main/java/com/jpo/cellnet/net/CellApi.kt`, objet `Cells`
(SSID, site, adresse IP, mot de passe, ports).

### 4.2 Paramètres du protocole (constantes du firmware)

| Paramètre | Valeur | Fichier |
|---|---|---|
| Charge utile max. d'une trame | 480 octets | `lib/cell_protocol/include/cell_protocol.h` |
| Taille max. d'une trame encodée | 496 octets | idem |
| Fragments max. par message applicatif | 12 (≈ 5 712 octets) | `lib/cell_core/include/cell_core.h` |
| File d'émission par station | 8 trames applicatives + 6 trames de contrôle | idem |
| Suivi d'accusés | 3 messages × 4 fragments | idem |
| Budget d'un tour de bus | 4 trames / 120 ms | `src/common/mesh_node.cpp` |
| Récupération du jeton (silence) | 400 ms + 40 ms × n° de nœud | idem |
| Circulation du jeton au repos | passage au nœud vivant suivant, libération après un tour complet sans données | idem |
| Ping de présence | 5 s | idem |
| Publication de la table de routage | 10 s (ou immédiate si changement) | idem |
| Péremption d'un pair | 15 s | idem |
| Péremption d'un utilisateur local | 90 s | idem |
| Accusé de transport, première fenêtre / tentatives | 400 ms (doublée à chaque essai) / 4 | idem |
| Bloc audio | 400 octets utiles | `app_server.cpp` + Android |
| Bloc de texte long | 256 octets UTF‑8 utiles (max 32 ko par message) | Android `TextChunker` |
| Durée max. d'un vocal | 60 s | Android `VoiceProtocol` |
| Outbox Android | nouvel essai toutes les 3 s, backoff 2 s → 60 s, 8 tentatives | Android `OutboxPolicy` |

---

## 5. Protocole RS485

Le protocole est implémenté dans `lib/cell_protocol` (encodage/décodage, testé
nativement) et `lib/cell_core` (file de priorité, arbitrage, routage,
fragmentation). Aucun envoi de chaînes terminées par un retour à la ligne n'est
utilisé.

### 5.1 Format d'une trame

Avant encodage COBS :

| Offset | Taille | Champ |
|---|---|---|
| 0 | 1 | version du protocole (`1`) |
| 1 | 1 | type de message |
| 2 | 1 | drapeaux |
| 3 | 1 | nœud source (1..3) |
| 4 | 1 | nœud destination (1..3, `0` = diffusion) |
| 5 | 2 | numéro de séquence (little endian) |
| 7 | 2 | longueur de la charge utile (0..480, little endian) |
| 9 | n | charge utile |
| 9+n | 2 | CRC‑16/CCITT‑FALSE sur l'en‑tête + la charge utile |

La trame est ensuite **encodée en COBS** puis terminée par un octet `0x00`.
COBS garantit que le délimiteur n'apparaît jamais dans les données : après une
trame tronquée ou corrompue, le récepteur retrouve la synchronisation sur le
délimiteur suivant (`FrameAssembler::push`). Toute trame dont le CRC, la version
ou la longueur est incohérente est rejetée et comptée dans `bus.crcErrors`
(`/api/health`).

### 5.2 Types et drapeaux

| Code | Type | Rôle |
|---|---|---|
| `0x01` | `PING` | présence d'un nœud (diffusion) |
| `0x02` | `PONG` | réservé |
| `0x03` | `TOKEN` | jeton d'accès au bus |
| `0x04` | `ROUTE_STATE` | liste de présence d'une station |
| `0x05` | `ROUTE_REQUEST` | demande de publication |
| `0x06` | `BUS_ACK` | accusé de transport (numéro de séquence) |
| `0x10` | `TEXT` | message texte (charge utile opaque JSON) |
| `0x11` | `RECEIPT` | accusé de livraison / lecture |
| `0x12` | `VOICE_BLOCK` | bloc audio |
| `0x13` | `VOICE_FEEDBACK` | début, fin, nack, lecture d'un vocal |
| `0x14` | `APP_CONTROL` | contrôle applicatif générique |

Drapeaux : `0x01` accusé requis, `0x02` retransmission, `0x04` fragment,
`0x08` jeton attaché à cette trame.

### 5.3 Enveloppe applicative

Chaque charge utile applicative commence par les deux identifiants utilisateurs :

```
[dstLen:1][dst][srcLen:1][src][body...]
```

`body` est opaque pour l'ESP32 : JSON pour le texte et les accusés, bloc binaire
pour l'audio. L'enveloppe permet à la station destinataire de savoir à quel
téléphone remettre le message **sans analyser le JSON applicatif**, et évite de
confondre l'adresse de destination avec l'identifiant de l'expéditeur.

### 5.4 Contrôle d'accès au bus partagé (jeton)

Deux stations ne doivent jamais émettre en même temps. Le mécanisme retenu est
un **jeton circulant avec échéances déterministes** :

1. Seul le détenteur du jeton peut transmettre.
2. Un détenteur émet au plus **4 trames ou 120 ms** (priorité respectée).
3. Fin de tour :
   * s'il reste des trames en file → envoi d'une trame `TOKEN` au **prochain
     nœud vivant** ;
   * si la file est vide → le jeton est **transmis au nœud vivant suivant**
     (champ `hops` = nombre de tours consécutifs sans données). Il n'est
     **libéré** qu'après un tour complet où **aucune** station n'avait de données :
     le bus redevient alors silencieux (les deux LED s'éteignent). Abandonner le
     jeton dès que la file locale était vide obligeait auparavant chaque nouveau
     message à attendre 400–520 ms de silence avant d'être revendiqué, ce qui
     divisait le débit et déclenchait des retransmissions inutiles.
4. La dernière trame de données d'un tour transporte le jeton
   (`kFlagTokenPass`) : le tour suivant démarre sans trame supplémentaire.
5. Un nœud qui a des trames en attente et **n'a pas** le jeton attend un silence
   de `400 ms + 40 ms × numéro de nœud`, puis **revendique** le jeton. C'est ce
   qui traite la perte du jeton et le redémarrage d'une station.
6. Un nœud sans `PING` depuis 15 s est retiré du masque des nœuds vivants : le
   jeton ne lui est plus adressé.
7. En cas de revendication simultanée (collision), les deux trames échouent au
   CRC, le bus redevient silencieux et les créneaux différents par numéro de
   nœud garantissent qu'un seul nœud revendique au cycle suivant.

Les compteurs `tokensReceived`, `tokenPasses`, `retries` et `failures` sont
exposés par `/api/health` pour diagnostiquer un bus bloqué.

### 5.5 Priorités et anti‑famine

La file d'émission (8 trames) sert par ordre de priorité :

1. contrôle du réseau (jeton, ping, table de routage, récupération) ;
2. accusés et retours vocaux ;
3. texte ;
4. blocs audio.

Un bloc audio qui attend depuis plus de **2,5 s** est promu en priorité 2 : un
flux de texte continu ne peut donc pas affamer l'audio indéfiniment.

### 5.6 Accusés de transport, doublons, retransmissions

* Les trames `TEXT`, `RECEIPT` et `VOICE_FEEDBACK` portent `0x01` (accusé requis).
* Le récepteur répond par une trame `BUS_ACK` (numéro de séquence) — cet accusé
  est mis dans une **file de contrôle dédiée** (6 emplacements) servie avant les
  données : il ne peut ni être perdu parce que la file applicative est pleine, ni
  prendre la place d'un message utilisateur.
* L'émetteur confie la trame à un **suivi d'accusés par fragment** (`AckTracker`) :
  une entrée par message, un bit par fragment. Un accusé efface **exactement** le
  fragment qu'il nomme, jamais un autre message ni un autre fragment, et seuls les
  fragments encore non acquittés sont renvoyés (**retransmission sélective**).
* Une retransmission n'est programmée qu'après un **silence réel du bus** : si le
  pair a émis une trame après notre envoi, il est vivant et travaille sur notre
  trame, l'échéance est simplement repoussée. C'est ce qui a supprimé les
  tempêtes de retransmissions (l'ancien délai fixe de 300 ms était plus court que
  la fenêtre de revendication du jeton de 400 ms). Le délai double ensuite à
  chaque essai (400 → 800 → 1600 → 3200 ms), pour **4 tentatives** au total.
* Le récepteur mémorise les 8 derniers couples `(source, séquence)` : une trame
  retransmise est détectée, **acquittée de nouveau mais jamais livrée deux
  fois**.
* Les blocs audio ne sont pas acquittés trame par trame (cela saturerait le bus) :
  la détection des blocs manquants et la retransmission sélective sont faites par
  le téléphone destinataire (`voice_nack`), décrites en section 8.

### 5.7 Fragmentation

Une charge utile supérieure à 480 octets est découpée :

```
[index:1][count:1][messageSeq:2][chunk ≤ 476 octets]
```

* 12 fragments maximum, soit **≈ 5 712 octets** de charge utile par message de
  bus. Les textes plus longs sont découpés par l'application Android elle‑même en
  blocs de 256 octets (voir section 8.5), ce qui garde chaque bloc dans une seule
  trame et rend la reprise sélective possible ;
* le réassemblage accepte les fragments **dans n'importe quel ordre**, ignore un
  doublon, expire un assemblage incomplet après **15 s** et ne dispose que de
  **2 contextes** simultanés (mémoire bornée, buffer statique alloué une fois pour
  toutes) ;
* au-delà, la station répond HTTP `413 too-large`.

### 5.8 Paramètres UART et limites physiques

* UART2, `TX = GPIO17`, `RX = GPIO16`, `115200 8N1`, buffers de 1024 octets.
* Émission **non bloquante** : la trame est déposée dans le buffer UART,
  `GPIO4` passe à l'état haut, et la boucle principale continue de servir le
  Wi‑Fi. La libération de `GPIO4` n'a lieu qu'après la fin réelle de l'émission
  (`uart_wait_tx_done` + buffer vide), avec un garde‑temps de sécurité.
* Débit utile : une trame de 480 octets dure ≈ 43 ms à 115200 bit/s ; avec le
  tour de jeton, le débit audio utile est d'environ **9–10 kB/s**. Un vocal de
  60 s (≈ 180 kB) demande donc **20 à 30 s** de transfert, pendant lequel les
  textes et accusés gardent la priorité.
* Le bus n'est **pas** un tunnel IP : il ne transporte que nos trames.

---

## 6. Service applicatif local (Wi‑Fi)

Chaque station expose deux services sur son point d'accès :

* **HTTP REST** sur `http://<ip-cellule>:8080` (bibliothèque `WebServer`) ;
* **WebSocket** sur `ws://<ip-cellule>:8081` (bibliothèque `links2004/WebSockets`),
  pour les événements temps réel.

Aucun de ces services n'est accessible depuis Internet : il faut être connecté au
Wi‑Fi de la cellule.

### 6.1 Endpoints REST

| Méthode | Chemin | Corps | Réponse |
|---|---|---|---|
| GET | `/api/info` | — | site, SSID, nœud, IP, ports, uptime, pairs vivants, nombre d'utilisateurs |
| GET | `/api/health` | — | compteurs du bus (trames émises/reçues, CRC, retries, failures, doublons, jetons), tas libre |
| GET | `/api/users` | — | annuaire connu : `id`, `name`, `station`, `stationNode`, `online` |
| POST | `/api/presence` | `{"userId","displayName"}` | enregistre/rafraîchit la présence locale |
| POST | `/api/presence/leave` | `{"userId"}` | retire l'utilisateur |
| POST | `/api/messages/text` | `{"kind":"text","id","from","to","ts","body"}` | `202 queued` + station de destination, `200 delivered-local`, `409 unknown-recipient` / `recipient-offline`, `413 too-large`, `503 busy` |
| POST | `/api/messages/receipt` | `{"kind":"receipt","id","from","to","state":"delivered"\|"read","ts"}` | `202` / `409` |
| POST | `/api/voice/block` | `{"msgId","from","to","seq","total","crc","data"<base64>}` | `202` / `409` |
| POST | `/api/voice/feedback` | `{"kind":"voice_begin"\|"voice_nack"\|"voice_done"\|"voice_played","msgId","from","to",…}` | `202` / `409` |

`from` est l'identifiant **déclaré** par l'application : la station ne peut pas
vérifier qu'il appartient réellement à l'utilisateur (voir limitations).
Le corps est relayé tel quel au téléphone destinataire : la station n'interprète
ni le texte ni l'audio.

### 6.2 WebSocket

À la connexion, le téléphone s'identifie :

```json
{"type":"hello","userId":"u-1a2b3c4d5e"}
```

La station répond `hello_ack` (cellule, SSID, nœud, annuaire) puis pousse :

| Événement | Contenu |
|---|---|
| `presence` | annuaire complet mis à jour |
| `text` | corps JSON du message envoyé, **relayé tel quel** (champ `kind`) |
| `receipt` | accusé relayé tel quel (`kind":"receipt"`, `state`) |
| `voice_block` | en‑tête JSON (`msgId`, `from`, `to`, `seq`, `total`, `crc`, `length`) **suivi d'une trame binaire** auto‑identifiée : `[0xA5][0x01][msgId:16][seq:2][total:2][octets audio]`. L'identité est répétée dans la trame binaire : un en‑tête JSON perdu ne peut donc **jamais** associer les octets audio au mauvais bloc (ou au mauvais vocal). L'application accepte aussi l'ancien format non versionné en repli |
| `voice_begin` / `voice_nack` / `voice_done` / `voice_played` | JSON relayé tel quel (champ `kind`) |

Les messages issus d'un téléphone portent un champ `kind` ; les événements émis
par la station portent un champ `type`. L'application accepte les deux.

Un battement de cœur WebSocket (15 s / 3 s) détecte les sockets mortes ; le
téléphone se reconnecte automatiquement (boucle de 5 s côté application).

### 6.3 LED d'émission et de réception

* **Verte (`GPIO25`)** : allumée pendant l'émission réelle sur le bus, maintenue
  ~45 ms après la fin pour rester visible. Elle ne s'allume **jamais** sur un
  simple appui sur « Envoyer » : si le message est livré dans la même cellule, il
  n'y a aucune émission RS485 et la LED reste éteinte.
* **Rouge (`GPIO26`)** : impulsion à chaque octet réellement reçu du bus (donc
  aussi pour une trame corrompue, ce qui est une réception réelle). Elle ne
  signale jamais une réception Wi‑Fi.
* Impulsions non bloquantes (`millis()`), pas de `delay()` long.
* Bus au repos : les deux LED sont éteintes. Les trames de présence (ping toutes
  les 5 s, table de routage toutes les 10 s) produisent de brèves impulsions :
  c'est une activité réelle du bus.

---

## 7. Découverte des utilisateurs et routage

### 7.1 Annonce de présence

1. Au premier lancement, l'application génère un identifiant stable
   (`u-` + 10 caractères hexadécimaux) et l'enregistre dans les préférences
   locales ; le nom d'affichage est modifiable.
2. À chaque connexion (et toutes les 30 s), elle poste `/api/presence` sur la
   station de la cellule courante.
3. La station enregistre l'utilisateur dans sa **table de routage** avec sa
   station, son état et l'horodatage de dernière mise à jour, puis publie sa
   liste sur le bus (`ROUTE_STATE`, immédiatement si la liste change, sinon
   toutes les 10 s).
4. Chaque pair fusionne la liste reçue : la liste est **autoritaire** pour la
   station qui l'émet ; un utilisateur absent de la nouvelle liste passe
   `offline`.
5. Une station sans `PING` depuis 15 s est considérée absente : tous ses
   utilisateurs passent `offline`.
6. Un utilisateur local qui n'a plus rafraîchi sa présence depuis **90 s**
   (téléphone déconnecté, application tuée) passe `offline`.

### 7.2 Table de routage

16 entrées maximum par station. Chaque entrée contient : identifiant
utilisateur, nom d'affichage, station (nœud 1..3), état de présence, horodatage.
Le format binaire échangé sur le bus est
`[count][idLen][id][nameLen][name]…` (≤ 480 octets).

### 7.3 Identifiants dupliqués

Un identifiant n'existe qu'en **une seule entrée** : si le même identifiant est
annoncé depuis une autre cellule, l'entrée est **déplacée** (dernière annonce
gagnante) et l'ancienne station n'est plus utilisée pour le routage. C'est un
choix de simplicité : le prototype ne détecte pas une usurpation d'identifiant
(voir limitations).

### 7.4 Sélection d'un destinataire

L'application affiche la liste fusionnée des utilisateurs des trois cellules
(`/api/users` + événements `presence`) et permet une recherche par nom **ou par
identifiant**. Un utilisateur dont la présence est connue mais `offline` reste
visible (avec la mention « hors ligne ») : il ne peut pas recevoir de message
tant qu'il n'est pas revenu.

---

## 8. Messagerie texte

### 8.1 Cycle de vie d'un message

```
créé localement (pending)
   → envoi HTTP (sending)
   → accepté par la station (sent)            ← accusé de transport RS485 côté ESP32
   → reçu et enregistré par le téléphone (delivered)   ← accusé « delivered » du destinataire
   → ouvert par le destinataire (read)        ← accusé « read »
   → échec (failed) après 3 tentatives HTTP
```

Un message est **toujours enregistré dans la base Room avant** la tentative
d'envoi. Un identifiant unique (`m-` + 14 caractères) sert de clé primaire, ce qui
rend l'insertion idempotente : une trame retransmise ne crée jamais deux lignes.

### 8.2 Accusés

* **Livraison** : dès que le téléphone destinataire a **enregistré** le message
  dans Room, il poste `/api/messages/receipt` avec `state = "delivered"` ;
  l'accusé traverse le bus en sens inverse et l'expéditeur passe en `delivered`.
  Si le destinataire est hors ligne, l'accusé n'existe pas : le message reste
  `sent` — l'application **n'invente jamais** une confirmation.
* **Lecture** : à l'ouverture de la conversation, chaque message entrant non lu
  est marqué lu et un accusé `read` est envoyé. L'expéditeur passe alors en `read`.

### 8.3 Doublons et reprises

* Le firmware mémorise les 8 derniers `(source, séquence)` (section 5.6).
* L'application utilise l'identifiant du message comme clé primaire Room
  (`OnConflictStrategy.IGNORE`) : un doublon est ignoré et **aucun accusé de
  livraison n'est renvoyé** pour un message déjà présent (donc pas de double
  comptage).
* Si l'envoi HTTP échoue (station injoignable, file pleine), l'application
  réessaie 3 fois avec un délai croissant puis marque `failed`.

### 8.4 Taille des messages texte

Deux chemins cohabitent, choisis automatiquement par l'application :

* **message court** (≤ 256 octets UTF‑8) : un seul payload `kind:"text"`, comme
  avant. L'enveloppe complète (JSON + en‑tête utilisateurs) tient dans une seule
  trame de bus ;
* **message long** (> 256 octets) : voir 8.5.

La station refuse avec un code explicite (`413 too-large`) tout corps dépassant sa
limite, ce qui évite toute troncature silencieuse.

### 8.5 Messages longs : fragmentation applicative et reprise sélective

Un texte long n'est plus limité par la capacité d'une trame de bus :

1. l'application découpe le corps en **blocs de 256 octets UTF‑8** (jamais au
   milieu d'un caractère multi‑octets), numérotés `seq` sur `total`, avec la
   taille et le CRC32 du message complet ;
2. chaque bloc est envoyé comme payload `kind:"text_block"` sur
   `/api/messages/text` : la station le relaie **sans l'interpréter**, et il tient
   toujours dans une seule trame RS485 (aucune fragmentation de bus nécessaire) ;
3. le destinataire écrit les blocs reçus dans la table `transfers` (index déjà
   validés) et ne publie le message — donc n'envoie l'accusé `delivered` — que
   lorsque **tous** les blocs sont présents ;
4. les blocs manquants sont réclamés par `kind:"text_nack"` (liste d'index) :
   l'expéditeur renvoie **uniquement** ceux‑là ;
5. la progression est persistée : la reprise fonctionne après une coupure, un
   changement de cellule ou un redémarrage de l'application.

Limites explicites : **32 ko maximum** par message (`TextChunker.MAX_TEXT_BYTES`),
soit 128 blocs. Au‑delà, l'application affiche une erreur et n'envoie rien.

Les blocs sont idempotents : un bloc reçu deux fois est ignoré, un message
dupliqué ne crée jamais deux lignes.

---

## 9. Messages vocaux (60 s maximum)

### 9.1 Choix du format audio

| Codec / conteneur | Débit typique | Support Android | Décision |
|---|---|---|---|
| **AAC‑LC / MPEG‑4 (`.m4a`)** | 24 kbit/s en 16 kHz mono | `MediaRecorder` natif depuis l'API 1 | **retenu** |
| Opus / OGG | 16–24 kbit/s | encodeur Opus seulement à partir d'Android 10, pas sur tous les appareils | écarté (compatibilité) |
| AMR‑WB / `.3gp` | 12 kbit/s mais qualité moindre | natif | écarté (qualité) |
| PCM WAV | 256 kbit/s | natif | écarté (volume ×10) |

AAC‑LC 16 kHz mono à 24 kbit/s donne ≈ **180 kB pour 60 s**, ce qui reste
raisonnable pour un bus à 9–10 kB/s. Le fichier est stocké sur le téléphone
(`filesDir/voice/…`), jamais sur l'ESP32.

### 9.2 Enregistrement

* Le bouton micro démarre `MediaRecorder` (`AAC`, `MPEG_4`, 16 kHz, mono,
  24 kbit/s).
* Un compteur s'affiche (secondes écoulées / 60 s) ; à **60 s** l'enregistrement
  est **arrêté et envoyé automatiquement** (`VoiceProtocol.MAX_DURATION_MS`).
* « Annuler » supprime le fichier temporaire.
* Permission `RECORD_AUDIO` demandée au premier usage, avec explication.

### 9.3 Découpage et transmission

L'application lit le fichier et le découpe en blocs de **400 octets** :

```json
{"msgId":"m-0123456789ab","from":"u-…","to":"u-…","seq":12,
 "total":455,"crc":41234,"data":"<400 octets en base64>"}
```

La station convertit ce JSON en bloc binaire sur le bus :

```
[msgId 16][seq:2][total:2][crc16:2][données ≤ 400]
```

puis l'enveloppe applicative (destinataire + expéditeur) est ajoutée. Le total
tient dans une seule trame RS485 (≈ 474 octets), donc **aucune fragmentation de
bus n'est nécessaire pour un bloc audio** : le découpage applicatif joue ce rôle.

Côté réception, la station pousse au téléphone un en‑tête JSON puis une **trame
WebSocket binaire** avec les octets audio (ordre garanti sur la même socket).

* **Fenêtre de transmission** : stop‑and‑wait (une requête à la fois). La réponse
  HTTP `202` (accepté) ou `503` (file pleine) sert de contrôle de flux ; en cas de
  `503` l'application attend et réessaie, ce qui évite de saturer la RAM de
  l'ESP32 (file de 8 trames).
* **Progression** : affichée en pourcentage pendant l'envoi et pendant la
  réception (blocs reçus / total).

### 9.4 Réassemblage et intégrité

* Le téléphone destinataire écrit chaque bloc à l'offset `seq × 400` d'un fichier
  `.part` : l'ordre d'arrivée n'a pas d'importance et un doublon ne corrompt rien.
* À réception du dernier bloc, la taille et le **CRC32 du fichier complet** sont
  vérifiés ; en cas de succès le fichier est renommé `…​.m4a` et l'état passe
  « livré » ; sinon le fichier partiel est supprimé.
* Un chien de garde de **15 s** sans progression envoie un `voice_nack` listant
  les index manquants ; l'émetteur relit le fichier local et ne renvoie **que**
  ces blocs (retransmission sélective), autant de fois que nécessaire.
* Le CRC‑16 de chaque bloc est informatif (contrôle rapide) ; le CRC‑32 global est
  la référence d'intégrité.

### 9.5 États affichés

`Enregistrement` → `Envoi en cours` (progression) → `En attente de confirmation`
→ `Livré` (fichier complet vérifié chez le destinataire) → `Écouté` (le
destinataire a effectivement lu le message). En cas d'échec : `Échec` avec la
raison.

La livraison n'est **jamais** annoncée avant le `voice_done` du destinataire, et
« écouté » n'est jamais déduit de l'absence d'erreur.

---

## 10. Application Android

### 10.1 Technologies

Aucune application n'existait dans le dépôt : elle a été créée dans `mobile/`.

| Élément | Choix |
|---|---|
| Langage | Kotlin 1.9.24 |
| UI | Jetpack Compose (Material 3), BOM 2024.06.00 |
| Base locale | Room 2.6.1 (KSP), base `cellnet.db` |
| Réseau | OkHttp 4.12 (HTTP + WebSocket) |
| Audio | `MediaRecorder` (AAC) / `MediaPlayer` |
| Build | AGP 8.5.2, Gradle 8.7, JDK 17, `compileSdk 34`, `minSdk 26` |
| Identité | `SharedPreferences` (`cellnet_profile`) |

### 10.2 Écrans

1. **Premier démarrage** : saisie du nom d'affichage → création d'un identifiant
   local stable. Les lancements suivants ne redemandent rien.
2. **Messages** : liste des conversations (nom, cellule, aperçu, date, compteur
   de non‑lus).
3. **Conversation** : bulles texte/vocal, états, bouton micro, compteur 60 s,
   progression d'envoi, lecture des vocaux.
4. **Répertoire** : tous les utilisateurs découverts sur les trois cellules,
   recherche par nom ou identifiant, indicateur en ligne/hors ligne.
5. **Cellule** : SSID et station courants, identifiant utilisateur, état HTTP et
   WebSocket, scan des cellules voisines, bouton « Rejoindre » par cellule, et
   explication des limites Android.

### 10.3 Permissions demandées

| Permission | Pourquoi | Quand |
|---|---|---|
| `INTERNET`, `ACCESS_NETWORK_STATE` | joindre la station locale et interroger l'état du réseau | installation |
| `ACCESS_WIFI_STATE`, `CHANGE_WIFI_STATE` | lire le SSID, demander un changement de réseau | installation |
| `ACCESS_FINE_LOCATION` | Android exige la localisation pour lire le SSID, **sur toutes les versions** (`WifiInfo.getSSID()`), et elle reste nécessaire sur Android 13+ pour éviter un SSID masqué | au démarrage, et bouton « Autoriser » |
| `NEARBY_WIFI_DEVICES` (API 33+) | accès aux API Wi‑Fi sur Android 13+, déclaré `neverForLocation` | au démarrage, et bouton « Autoriser » |
| `RECORD_AUDIO` | messages vocaux **et** appels en direct | au premier enregistrement, ou au premier appel |
| `MODIFY_AUDIO_SETTINGS` | mode audio d'appel et routage haut‑parleur / écouteur | permission normale, accordée à l'installation |

Sur Android 13+, **les deux** permissions Wi‑Fi sont demandées : `NEARBY_WIFI_DEVICES`
autorise l'appel des API Wi‑Fi et `ACCESS_FINE_LOCATION` évite que le SSID soit
remplacé par `unknown ssid`. Si l'utilisateur refuse la localisation, l'application
tente quand même la lecture avec `NEARBY_WIFI_DEVICES`, affiche la raison exacte
de l'échec et peut identifier la cellule par la **passerelle DHCP** de l'AP, sans
jamais inventer un nom de réseau.

### 10.4 Détection et changement de cellule

La détection est un pipeline en trois étapes indépendantes, dans
`wifi/CellWifi.kt` et `ui/AppViewModel.kt` :

1. **Le téléphone utilise-t-il le Wi‑Fi ?** `ConnectivityManager` +
   `NetworkCapabilities.hasTransport(TRANSPORT_WIFI)`, en examinant **tous** les
   réseaux connus du système et pas seulement le réseau par défaut. C'est
   indispensable ici : un AP ESP32 n'ayant pas Internet, Android garde souvent la
   4G comme réseau par défaut alors que le téléphone est bien associé à la
   cellule. Aucun scan n'est utilisé pour cela.
2. **Quel SSID ?** `WifiInfo.getSSID()` est lu avec un résultat explicite :
   `AVAILABLE`, `NO_PERMISSION`, `REDACTED` (`<unknown ssid>`), `EMPTY` ou
   `ERROR`. Une valeur inconnue n'est jamais convertie en nom de cellule.
3. **Quelle cellule ?** Le SSID est comparé aux SSID réellement configurés
   (`CELL_SITE_A/B/C`). Si le SSID est illisible, l'application utilise la
   **passerelle IPv4 du lien Wi‑Fi** (`LinkProperties`, c'est‑à‑dire l'adresse de
   l'AP ESP32 obtenue par DHCP) et la compare aux adresses configurées
   (`10.0.0.1`, `10.1.0.1`, `10.2.0.1`). Dans les deux cas, la cellule n'est
   déclarée détectée qu'après confirmation par `GET /api/info` : la réponse doit
   annoncer le `site` attendu.

Les états affichés sont distincts et ne se confondent jamais :

| État | Signification |
|---|---|
| `CONNECTED` | cellule identifiée **et** service local joignable |
| `NO_WIFI` | Wi‑Fi désactivé ou aucun réseau Wi‑Fi actif |
| `PERMISSION_MISSING` | permissions Wi‑Fi/localisation refusées |
| `SSID_UNAVAILABLE` | Wi‑Fi connecté, SSID illisible et passerelle non reconnue |
| `SSID_UNKNOWN` | SSID lisible mais ce n'est pas une cellule CellNet |
| `STATION_UNREACHABLE` | cellule identifiée, `GET /api/info` ne répond pas |
| `STATION_MISMATCH` | une station répond mais annonce une autre cellule |
| `UNKNOWN` | vérification en cours |

**Routage sans Internet.** Un AP ESP32 n'a pas d'accès Internet : Android
préfère alors la 4G comme réseau par défaut et les requêtes vers `10.x.x.x`
échouent. Dès qu'une cellule est identifiée, l'application appelle
`ConnectivityManager.bindProcessToNetwork(wifiNetwork)` : tout le trafic HTTP et
WebSocket du processus part vers l'ESP32, sans désactiver aucune sécurité du
téléphone. Le lien est retiré (`bindProcessToNetwork(null)`) dès que la cellule
n'est plus identifiée.

**Suivi des changements.** Un `NetworkCallback` sur le transport Wi‑Fi
(`onAvailable` / `onLost` / `onCapabilitiesChanged`) déclenche une nouvelle
détection, avec un anti-rebond de 700 ms. Une cellule n'est jamais conservée si le
réseau change : `clearStation()` efface le site, l'adresse, la station et l'état
WebSocket dès qu'un des contrôles échoue.

`WifiNetworkSpecifier` + `requestNetwork` (API 29+) restent utilisés pour
rejoindre une cellule depuis l'onglet « Cellule » ; sur API 26–28, l'API
historique `addNetwork`/`enableNetwork` est employée. Si le système refuse, le
parcours guidé invite à se connecter depuis les réglages : la détection reprend
seule au prochain événement réseau ou au cycle de 5 s.

Les conversations et les fichiers audio **ne sont jamais effacés** lors d'un
changement de cellule.

### 10.5 Stockage local

| Donnée | Emplacement |
|---|---|
| Identité (id + nom) | `SharedPreferences` `cellnet_profile` |
| Conversations, messages, états, métadonnées audio | table Room `messages` / `conversations` |
| Fichiers audio (enregistrés et reçus) | `filesDir/voice/<id>.m4a` |
| Transferts en cours | `filesDir/voice/<id>.part` (supprimés au redémarrage) |

Les fichiers audio sont distincts des métadonnées : supprimer un fichier ne
supprime pas l'historique de la conversation, et inversement.

### 10.6 Journalisation du diagnostic

Deux tags Logcat sont utilisés :

| Tag | Contenu journalisé |
|---|---|
| `CellNetWifi` | version d'API et `targetSdk`, permissions Wi‑Fi/localisation accordées, services de localisation, Wi‑Fi activé, transport Wi‑Fi, statut du SSID + SSID lu, passerelle, démarrage/arrêt du suivi réseau, liaison du processus, échecs de `requestNetwork` |
| `CellNetSession` | état Wi‑Fi à chaque détection, cellule identifiée et indice utilisé (SSID ou passerelle), adresse testée, réponse ou exception HTTP de la station, résultat des permissions, refus du scan |

```powershell
adb logcat -s CellNetWifi CellNetSession
```

Exemple de sortie sur Android 13, téléphone connecté à `CELL_SITE_A` :

```
I CellNetWifi: sdk=33 targetSdk=34 fineLocation=true nearbyWifi=true
   locationServices=true wifiEnabled=true transportWifi=true
   ssidStatus=AVAILABLE ssid=CELL_SITE_A gateway=10.0.0.1
I CellNetSession: cell SITE_A identified by SSID
I CellNetSession: station SITE_A reachable at 10.0.0.1
```

Aucune donnée personnelle n'est journalisée : ni identifiant utilisateur, ni
contenu de message, ni fichier audio. Seuls le SSID, la passerelle et les états
techniques apparaissent.

---

## 11. Outils, compilation et téléversement

### 11.1 Prérequis

* **Python 3.9+** et **PlatformIO Core** :
  ```powershell
  python -m pip install --upgrade platformio
  ```
  Si PlatformIO a été installé par l'extension VS Code, l'exécutable est :
  `"$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"`.
  S'il est installé globalement (pip), la commande `pio` suffit.
* Pilotes USB‑UART de vos cartes (CP210x, CH340, FTDI selon le modèle).
* Pour l'APK localement : **JDK 17**, **Gradle 8.7** et le **SDK Android 34**
  (Android Studio). Sans ces outils, passer par GitHub Actions (section 13).

### 11.2 Installation des dépendances

PlatformIO les installe automatiquement au premier build, ou manuellement :

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" pkg install -e site_a
```

Dépendances : `espressif32@6.6.0` (Arduino‑ESP32 2.0.14),
`links2004/WebSockets@^2.4.1`, `bblanchon/ArduinoJson@^7.0.4`.

### 11.3 Compilation des trois stations

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_a
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_b
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_c
```

Ou les trois d'un coup :

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_a -e site_b -e site_c
```

Résultat attendu (mesuré sur ce poste) : `RAM 17,5 %`, `Flash 62,4 %`, un
`firmware.bin` distinct par environnement dans `.pio\build\<env>\`.

### 11.4 Identifier les ports COM

Brancher **une seule** carte à la fois, puis :

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device list
```

Noter le port (par exemple `COM8`, `COM9`, `COM10`) et l'associer à la carte.
**Les ports ci‑dessous sont des exemples : remplacez‑les par ceux réellement
attribués à vos cartes.**

### 11.5 Téléversement de SITE_A

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_a
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_a -t upload --upload-port COM8
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --port COM8 --baud 115200
```

### 11.6 Téléversement de SITE_B

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_b
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_b -t upload --upload-port COM9
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --port COM9 --baud 115200
```

### 11.7 Téléversement de SITE_C

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_c
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_c -t upload --upload-port COM10
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --port COM10 --baud 115200
```

Si l'upload échoue :

* « **Failed to connect** » → maintenir le bouton **BOOT** pendant `Connecting....`
  (certaines cartes n'ont pas de circuit d'auto‑reset fiable) ;
* « **The chip stopped responding** » pendant l'écriture → le débit est trop
  élevé pour le câble/adaptateur. Le projet est réglé à `upload_speed = 115200`
  pour cette raison ; changer de câble USB, brancher directement au PC (sans hub)
  et relancer.

Chaque carte doit recevoir **son** environnement : la carte de `SITE_A` est
flashée avec `site_a`, celle de `SITE_B` avec `site_b`, celle de `SITE_C` avec
`site_c`. Étiqueter les cartes physiquement et vérifier le SSID dans le
moniteur série après le flash.

### 11.8 Vérification au démarrage

Le moniteur série doit afficher (exemple pour SITE_A) :

```
[     xxx][I][boot] local cellular prototype - SITE_A - firmware 1.0.0
[     xxx][I][wifi] access point "CELL_SITE_A" up, IP 10.0.0.1
[     xxx][I][node] SITE_A (node 1) ready on RS485 115200 baud, DE/RE GPIO4, termination=120R
[     xxx][I][node] wiring TX=GPIO17 RX=GPIO16 DIR=GPIO4 LED_TX=GPIO25 LED_RX=GPIO26
[     xxx][I][http] REST http://10.0.0.1:8080  WebSocket ws://10.0.0.1:8081
```

Puis, une fois les trois cartes alimentées, les pairs apparaissent :

```
[     xxx][I][pres] peer node 2 online
[     xxx][I][pres] peer node 3 online
```

### 11.9 Tests unitaires (sans matériel)

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native
```

Attendu : `44 test cases: 44 succeeded`.

Les tests de fiabilité comprennent une **simulation déterministe de deux
émetteurs concurrents** (`test/test_reliability.cpp`) : anneau de jetons, jeton
attaché à la dernière trame d'un tour, passages de jeton au repos, accusés par
fragment, retransmission sélective, pair muet. Elle s'exécute en quelques
millisecondes et permet de vérifier une politique d'ordonnancement sans matériel.
Un traceur (`gSimTrace = true`) imprime chaque événement du bus simulé pour
diagnostiquer un problème d'ordonnancement.

### 11.10 Compilation de l'application Android (en local)

```powershell
cd mobile
gradle assembleDebug
gradle assembleRelease
```

Build réellement exécuté sur ce poste avec JDK 17.0.20.1, Gradle 8.7 et
Android SDK 34 (platform 34 + build-tools 34.0.0) :

```powershell
$env:JAVA_HOME = "C:\chemin\vers\jdk-17"
$env:ANDROID_HOME = "C:\chemin\vers\android-sdk"
gradle -p mobile --no-daemon assembleDebug assembleRelease
```

Fichiers produits :

* `mobile\app\build\outputs\apk\debug\app-debug.apk`
* `mobile\app\build\outputs\apk\release\app-release.apk`

> ⚠️ La variante `release` est signée avec la **clé de debug** (voir
> `mobile/app/build.gradle`) : elle est installable pour les essais, mais **pas**
> publiable sur le Play Store. Pour une vraie signature, ajouter un keystore via
> les secrets GitHub et un `signingConfig` dédié.

### 11.11 Scripts d'exploitation fournis

**Téléverser les trois cartes sans se tromper de port** — le script vérifie que
les trois ports existent et sont distincts, compile puis téléverse chaque
environnement sur le port associé :

```powershell
.\scripts\flash_stations.ps1 -SiteA COM8 -SiteB COM9 -SiteC COM10
```

Options : `-SkipBuild` pour ne pas recompiler, et le script s'arrête au premier
échec avec la procédure de dépannage (BOOT, câble, débit).

**Diagnostiquer les trois stations** — à lancer depuis un PC connecté au Wi‑Fi
d'une cellule :

```powershell
# une seule cellule
.\scripts\station_doctor.ps1 -Cell A

# les trois (celles qui sont joignables depuis la cellule courante)
.\scripts\station_doctor.ps1

# surveillance continue toutes les 3 s
.\scripts\station_doctor.ps1 -Loop -IntervalSec 3
```

Le script affiche le SSID réellement connecté, puis pour chaque station :
identité, pairs vivants, compteurs du bus (`tx`, `rx`, `crc`, `retries`,
`failures`, doublons, jetons, masque des nœuds vivants) et tas libre. Il permet
de valider la phase B du protocole sans ouvrir trois moniteurs série.

---

## 12. Tests

### Phase A — validation du firmware

| Test | État | Détail |
|---|---|---|
| Compilation `site_a` / `site_b` / `site_c` | ✅ **fait** | `SUCCESS`, RAM 17,5 %, Flash 62,4 % |
| Initialisation UART (115200 8N1, GPIO16/17, buffers 1024) | ✅ compilé | `Rs485Bus::begin()` |
| GPIO des LED (25/26) et direction (4) configurés en sortie | ✅ compilé | `LedIndicators::begin()`, `Rs485Bus::begin()` |
| Commande DE/RE : HIGH à l'émission, LOW après fin réelle | ✅ compilé | `Rs485Bus::send()` / `update()` |
| Configuration des stations distincte | ✅ vérifié | empreintes SHA‑256 des trois `firmware.bin` différentes |
| Tests unitaires du protocole (COBS, CRC, resynchronisation, troncature) | ✅ **exécutés** | 7 tests |
| Tests unitaires de l'arbitrage, de la file et du routage | ✅ **exécutés** | 13 tests |
| Tests unitaires enveloppe + fragmentation | ✅ **exécutés** | 6 tests |
| Tests unitaires de fiabilité (accusés par fragment, files, jetons, messages longs, **simulation de deux émetteurs concurrents**) | ✅ **exécutés** | 16 tests |
| Tests unitaires de la livraison réassemblée (vue sans copie, absence de vue périmée) | ✅ **exécutés** | 2 tests |

### Phase B — validation RS485 (à faire sur matériel)

1. Alimenter les trois cartes, ouvrir trois moniteurs série.
2. Vérifier que chaque station voit les deux autres (`peer node X online`).
3. `/api/health` sur chaque station : `bus.received` et `bus.sent` doivent
   croître, `crcErrors` rester à 0, `tokensReceived` > 0.
4. Vérifier visuellement qu'une seule LED verte s'allume à la fois (pas
   d'émission simultanée).
5. Débrancher `SITE_B` : les deux autres doivent continuer à échanger
   (`failures` peut augmenter puis se stabiliser, le jeton est revendiqué).
6. Provoquer une trame corrompue (inverser A/B momentanément) : `crcErrors`
   augmente, la communication reprend au rétablissement.

### Phase C — validation Wi‑Fi (à faire sur matériel)

1. Chaque SSID (`CELL_SITE_A/B/C`) doit apparaître sur le téléphone.
2. S'y connecter manuellement, puis ouvrir `http://10.0.0.1:8080/api/info`.
3. Lancer l'application : la cellule affichée doit correspondre au SSID réel.
4. Couper le Wi‑Fi du téléphone : l'interface passe hors ligne ; le rétablir :
   elle revient en ligne sans perte de conversation.

### Phase D — validation de la messagerie (à faire sur matériel)

Depuis PowerShell, une fois le PC connecté au Wi‑Fi de la cellule :

```powershell
Invoke-RestMethod http://10.0.0.1:8080/api/info
Invoke-RestMethod http://10.0.0.1:8080/api/health
Invoke-RestMethod -Method Post -Uri http://10.0.0.1:8080/api/presence `
  -ContentType 'application/json' `
  -Body '{"userId":"u-test01","displayName":"Test A"}'
Invoke-RestMethod http://10.0.0.1:8080/api/users
```

Puis, avec deux téléphones :

1. message dans la même cellule (aucune LED verte ne doit s'allumer) ;
2. message `SITE_A` → `SITE_B`, puis `SITE_A` → `SITE_C` ;
3. réponse depuis l'autre cellule ;
4. accusé « livré » puis « lu » ;
5. destinataire hors ligne : le message reste `sent`/`En attente` sans fausse
   confirmation ;
6. reconnexion : aucun doublon dans la conversation.

### Phase E — validation audio (à faire sur matériel)

1. Enregistrement court (3 s) → envoi → lecture chez le destinataire.
2. Enregistrement de 60 s : arrêt automatique à 60 s, envoi complet, CRC32
   vérifié, lecture.
3. Tenter de dépasser 60 s : l'enregistrement s'arrête seul.
4. Couper le Wi‑Fi pendant un transfert : l'état passe en échec, aucun fichier
   tronqué n'est lisible ; relancer l'envoi.
5. Vérifier `/api/health` : `heap` ne doit pas s'effondrer pendant un transfert.

### Phase F — validation Android / CI

| Test | État |
|---|---|
| Compilation de l'application (debug + release) | ✅ **réalisée** : JDK 17 + Gradle 8.7 + SDK 34, `BUILD SUCCESSFUL` (re-vérifiée après la passe de fiabilisation, build propre) |
| Tests unitaires JVM de la reprise (chunking texte, trame binaire vocale, assemblage vocal, politique d'outbox) | ✅ **exécutés** : `gradle testDebugUnitTest` → 24 tests, 0 échec |
| Permissions déclarées dans l'APK final | ✅ **vérifié** avec `aapt2 dump badging` (voir §12.1) |
| Persistance de l'identité | ✅ codée (`SharedPreferences`), à vérifier sur appareil |
| Conservation des conversations après redémarrage | ✅ codée (Room), à vérifier sur appareil |
| Génération de l'APK | ✅ `app-debug.apk` (15,8 Mo) et `app-release.apk` (10,3 Mo) |
| Vérification de l'artefact CI | ⚠️ à faire après le premier run du workflow GitHub |

### 12.1 Vérification des permissions dans l'APK release

```powershell
aapt2 dump badging mobile\app\build\outputs\apk\release\app-release.apk
```

Résultat obtenu :

```
package: name='com.jpo.cellnet' versionCode='2' versionName='1.0.1' compileSdkVersion='34'
sdkVersion:'26'
targetSdkVersion:'34'
uses-permission: name='android.permission.INTERNET'
uses-permission: name='android.permission.ACCESS_NETWORK_STATE'
uses-permission: name='android.permission.ACCESS_WIFI_STATE'
uses-permission: name='android.permission.CHANGE_WIFI_STATE'
uses-permission: name='android.permission.ACCESS_FINE_LOCATION'
uses-permission: name='android.permission.NEARBY_WIFI_DEVICES' usesPermissionFlags='neverForLocation'
uses-permission: name='android.permission.RECORD_AUDIO'
```

`ACCESS_FINE_LOCATION` n'a plus de `maxSdkVersion` : elle est donc demandable et
utilisable sur Android 13/14, ce qui corrige la lecture du SSID.

> **Version applicative 1.0.1 (versionCode 2).** C'est le repère à vérifier sur le
> téléphone — *Paramètres → Applications → CellNet → version* — pour s'assurer que
> la correction Wi‑Fi est réellement installée. Un APK 1.0.0 affiche encore
> « Connectez le téléphone à CELL_SITE_A… » et les boutons « Scanner /
> Reconnecter » : c'est l'ancienne version.

### 12.2 Tests de la détection à réaliser sur téléphone

| Scénario | Résultat attendu |
|---|---|
| Connecté à `CELL_SITE_A`, permissions accordées | `CONNECTED`, cellule `SITE_A`, station joignable |
| Connecté à `CELL_SITE_B` puis `CELL_SITE_C` | bascule immédiate via le `NetworkCallback`, sans redémarrage |
| Localisation refusée (Android 13+) | `PERMISSION_MISSING`, bouton « Autoriser », aucun nom de cellule inventé |
| Localisation accordée mais SSID masqué par l'appareil | `SSID_UNAVAILABLE` **ou** cellule identifiée via la passerelle (affichée « via PASSERELLE ») |
| Connecté à un autre Wi‑Fi (box, partage) | `SSID_UNKNOWN`, aucun envoi tenté |
| Wi‑Fi désactivé | `NO_WIFI` |
| Connecté à la cellule, ESP32 éteint après coup | `STATION_UNREACHABLE`, cellule toujours affichée |
| Station flashée avec un autre site | `STATION_MISMATCH` |
| Mode avion + Wi‑Fi activé sur la cellule (sans Internet) | `CONNECTED` : le bind réseau permet d'atteindre 10.x.x.x |

---

## 13. GitHub Actions et APK

Le dépôt contient deux workflows :

| Fichier | Rôle |
|---|---|
| `.github/workflows/platformio-build.yml` | exécute les tests natifs puis compile `site_a`, `site_b`, `site_c` et publie chaque `firmware.bin` en artefact |
| `.github/workflows/android-build.yml` | installe JDK 17 + Gradle 8.7, compile `assembleDebug` et `assembleRelease`, vérifie les sorties et publie les APK |

### 13.1 Publier le dépôt

```powershell
git init
git add .
git commit -m "CellNet: prototype trois ESP32 RS485 + application Android"
git branch -M main
git remote add origin https://github.com/<votre-compte>/<votre-depot>.git
git push -u origin main
```

(Aucun fichier `.git` n'existait dans le dossier fourni : l'initialisation est
donc nécessaire avant le premier push.)

### 13.2 Récupérer les artefacts

1. Ouvrir l'onglet **Actions** du dépôt.
2. Sélectionner le workflow *Android APK* (ou *PlatformIO CI*).
3. Télécharger l'artefact :
   * `cellnet-apk` → `app-debug.apk` et `app-release.apk` ;
   * `firmware-site_a` / `site_b` / `site_c` → binaires prêts à flasher.

Aucun secret n'est nécessaire pour les builds fournis. Aucun mot de passe, jeton
ou clé privée n'est présent dans le dépôt.

### 13.3 Installer l'APK sur un téléphone

**Par USB (débogage activé) :**

```powershell
adb install -r .\app-debug.apk
```

**Sans câble :** copier l'APK sur le téléphone (câble, Drive hors ligne, etc.),
puis l'ouvrir et autoriser « Installer des applications inconnues » pour le
gestionnaire de fichiers.

L'APK ne dépend d'aucune adresse de développement : les adresses des cellules
(`10.0.0.1` / `10.1.0.1` / `10.2.0.1`) sont celles des points d'accès eux‑mêmes,
définies dans `Cells` et modifiables avant compilation.

---

## 14. Erreurs fréquentes et diagnostic

| Symptôme | Cause probable | Action |
|---|---|---|
| `Unknown board ID` / build étrange | environnement mal ciblé | vérifier `pio run -e site_a` |
| **`A fatal error occurred: The chip stopped responding` pendant l'upload** | débit de téléversement trop élevé pour l'adaptateur USB‑UART, câble défectueux, port USB/hub, ou alimentation insuffisante pendant l'écriture de la flash | 1) le projet est déjà réglé à `upload_speed = 115200` ; 2) brancher la carte directement au PC (pas de hub), essayer un autre câble ; 3) maintenir **BOOT** quand `Connecting....` s'affiche ; 4) relancer l'upload ; 5) si l'échec se répète au même pourcentage, essayer un autre port USB ou une alimentation 5 V séparée |
| Upload impossible, port occupé | moniteur série encore ouvert | fermer `pio device monitor` (Ctrl+C) puis relancer |
| Firmware correct mais mauvaise cellule | carte flashée avec le mauvais environnement | une carte = un environnement : `site_a` sur la carte A, etc. Étiqueter physiquement les trois cartes et vérifier le SSID affiché au démarrage |
| Écran série vide | mauvais port / débit | `pio device list`, puis `device monitor --baud 115200` |
| Aucun SSID visible | AP non démarré, alimentation insuffisante | vérifier le log `wifi`, alimenter en 5 V stable |
| `bus.sent` augmente, `bus.received` reste à 0 | A/B inversés, DE/RE non reliées, masse absente | inverser A/B sur une seule carte, vérifier le câblage, la masse commune |
| `crcErrors` élevé | terminaisons manquantes, câble trop long, débit trop élevé | installer 120 Ω aux extrémités A et C, torsader, réduire `kRs485Baud` |
| LED verte allumée en permanence | DE bloqué haut | vérifier `GPIO4`, module défectueux |
| Aucune LED pendant un message local | normal : le message ne passe pas par le bus | — |
| `409 recipient-offline` | destinataire non connecté ou table de routage périmée | attendre la publication de présence (10 s), vérifier `/api/users` |
| `409 unknown-recipient` | destinataire jamais découvert | ouvrir l'application de l'autre téléphone (présence) |
| `503 busy` répété | file RS485 pleine (audio soutenu) | réduire la cadence d'envoi, vérifier que les trois cartes sont vivantes |
| Message « Transmis » mais jamais « Livré » | destinataire hors ligne, ou accusé perdu | comportement attendu hors ligne (voir limitations) |
| Vocal bloqué à 0 % | Wi‑Fi perdu ou station redémarrée | vérifier `httpConnected`, relancer |
| Vocal « Échec » avec CRC32 invalide | blocs perdus et non récupérés | vérifier `bus.failures`, relancer l'envoi |
| Application : cellule « inconnue » | permission Wi‑Fi refusée | accorder « Appareils à proximité » (Android 13+) **et** la localisation |
| « Permissions Wi-Fi manquantes » | refus de `ACCESS_FINE_LOCATION` et/ou `NEARBY_WIFI_DEVICES` | bouton « Autoriser » dans l'onglet Cellule |
| « Wi-Fi connecté — identification de la cellule indisponible » | SSID masqué par Android et passerelle non reconnue | accorder la localisation, activer les services de localisation, vérifier `adb logcat -s CellNetWifi` |
| Cellule affichée « via PASSERELLE » | SSID illisible mais AP identifié par son adresse DHCP | comportement normal ; accorder la localisation pour revenir à l'identification par SSID |
| « Cellule SITE_X détectée — station inaccessible » | le téléphone est sur le bon AP mais `10.x.x.x:8080` ne répond pas | vérifier que la carte est alimentée, que le firmware tourne, tester `/api/info` depuis un PC, vérifier le bind réseau dans Logcat |
| « la station annonce SITE_Y » | firmware d'une autre cellule flashé sur cette carte | reflasher le bon environnement (`site_a`/`b`/`c`) |
| Changement de cellule impossible | restriction Android | se connecter depuis les réglages système, l'app redétecte via le `NetworkCallback` |
| APK non installable | signature de test ou version antérieure | désinstaller l'ancienne version, réinstaller |
| Room : `Cannot find setter` | version de Room/KSP désalignée | conserver les versions du `build.gradle` |

---

## 15. Sécurité, limites connues et arborescence

### 15.1 Sécurité du prototype

* **Aucune authentification.** L'identifiant utilisateur est généré localement et
  simplement *déclaré* à la station (`from` des requêtes). Un client connecté au
  Wi‑Fi de la cellule pourrait se faire passer pour un autre identifiant. Le nom
  d'affichage n'est en aucun cas une preuve d'identité.
* **Trafic en clair.** Le service local est en HTTP/WebSocket sans TLS (pas de
  certificat sur un ESP32 nu). Toute personne connaissant le mot de passe du
  point d'accès peut observer le trafic. Les mots de passe des AP sont des
  valeurs de démonstration (`cellsitea`, `cellsiteb`, `cellsitec`) à changer
  avant tout usage réel.
* **Pas de chiffrement des messages ni des fichiers audio.**
* Les points d'accès sont ouverts à 6 clients maximum par station.

Ce prototype convient à une démonstration ou à un usage pédagogique sur un site
maîtrisé, **pas** à un déploiement en production.

### 15.2 Limites fonctionnelles

| Limite | Explication |
|---|---|
| **Destinataire hors ligne** | Aucun serveur de stockage permanent : si le destinataire est totalement déconnecté, la station ne peut pas conserver le message (les files sont en RAM, bornées à 8 trames). L'expéditeur garde le message dans sa base Room ; **à la reconnexion suivante (y compris après redémarrage de l'application) il rejoue automatiquement les messages encore en `pending`, `sending` ou `failed`** — texte et vocal (le fichier audio restant sur le téléphone). Si le téléphone expéditeur reste éteint, l'envoi n'a évidemment pas lieu. |
| **Changement de Wi‑Fi Android** | Une application ne peut pas changer de réseau silencieusement. Le système affiche une confirmation ; en cas de refus, la connexion manuelle depuis les réglages est nécessaire. |
| **Débit** | 115200 bit/s ⇒ ≈ 9–10 kB/s utiles. Un vocal de 60 s prend 20–30 s ; il n'y a pas de streaming audio temps réel. |
| **Taille** | 3 nœuds, 16 utilisateurs par table de routage, 6 sockets WebSocket par station, messages texte ≈ 1350 octets, 3 fragments par message. |
| **Transferts audio interrompus** | Aucun stockage des blocs sur l'ESP32 : si le destinataire disparaît en cours de transfert, les blocs manquants doivent être renvoyés depuis le téléphone expéditeur (le fichier source y est conservé). |
| **Redémarrage d'une station** | Sa table de routage est perdue puis reconstruite en ≈ 10 s à partir des annonces des pairs. |
| **Plusieurs appareils par identifiant** | Tous les sockets liés au même identifiant reçoivent les messages ; l'état de lecture est commun. |
| **Horodatage** | L'horloge vient du téléphone expéditeur, sans synchronisation NTP (aucun accès Internet). |
| **LED** | Elles clignotent aussi pour le trafic de présence, qui est une émission réelle. |

### 15.3 Arborescence du dépôt

```
projet_jpo/
├── platformio.ini                  # env site_a / site_b / site_c + env native (tests)
├── README.md                       # ce document
├── lib/
│   ├── cell_protocol/              # trames COBS + CRC16 (pur C++, testé)
│   │   ├── include/cell_protocol.h
│   │   └── src/cell_protocol.cpp
│   ├── cell_core/                  # file de priorité, arbitrage, routage, fragmentation
│   │   ├── include/cell_core.h
│   │   └── src/cell_core.cpp
│   └── cell_call/                  # appels : sessions locales et intercellulaires,
│       ├── include/cell_call.h     #   isolation, trame média binaire (pur C++, testé,
│       └── src/cell_call.cpp       #   sans aucune dépendance au bus RS485)
│   └── cell_link/                  # dorsale Wi-Fi inter-stations : cadrage binaire,
│       ├── include/cell_link.h     #   authentification, routage à sauts limités,
│       └── src/cell_link.cpp       #   vivacité (pur C++, testé, sans le bus)
├── src/
│   ├── main.cpp                    # AP Wi-Fi + démarrage des services
│   └── common/
│       ├── station_config.h/.cpp   # identité, SSID, IP, broches imposées
│       ├── log.h/.cpp              # journalisation à niveaux
│       ├── led_indicators.h        # LED non bloquantes (GPIO25/26)
│       ├── rs485_bus.h             # UART2 + DE/RE + COBS + LED
│       ├── mesh_node.h/.cpp        # jeton, présence, ARQ, routage, fragmentation
│       ├── wifi_backbone.h/.cpp    # AP conservé + interface STA vers l'amont
│       ├── station_link.h/.cpp     # dorsale TCP inter-stations (cadrage, relais)
│       ├── app_server.h/.cpp       # HTTP REST + WebSocket + appels locaux et distants
│       └── ...
├── test/
│   ├── test_main.cpp               # point d'entrée Unity (native + ESP32)
│   ├── test_protocol.cpp           # 7 tests protocole
│   ├── test_core.cpp               # 19 tests cœur (dont validation atomique du routage)
│   ├── test_reliability.cpp        # 18 tests fiabilité + simulation 2 émetteurs
│   ├── test_call.cpp               # 38 tests appels locaux et intercellulaires
│   └── test_link.cpp               # 21 tests dorsale Wi-Fi + garde-fous « pas de RS485 »
├── scripts/
│   ├── native_toolchain.py         # MinGW fourni par PlatformIO pour env:native
│   ├── flash_stations.ps1          # compile + téléverse les 3 cartes (mapping de ports)
│   ├── serial_capture.ps1          # capture série horodatée multi-ports + console de diagnostic
│   └── station_doctor.ps1          # diagnostic /api/info + /api/health des cellules
├── mobile/                         # application Android (Kotlin + Compose + Room)
│   ├── settings.gradle
│   ├── build.gradle
│   ├── gradle.properties
│   └── app/
│       ├── build.gradle
│       └── src/
│           ├── main/
│           │   ├── AndroidManifest.xml
│           │   ├── res/…
│           │   └── java/com/jpo/cellnet/
│           │       ├── MainActivity.kt
│           │       ├── CellNetApp.kt
│           │       ├── data/  (Room, identité, table `transfers` de reprise)
│           │       ├── net/   (HTTP, WebSocket, cellules, trame binaire vocale)
│           │       ├── text/  (découpage et réassemblage des messages longs)
│           │       ├── sync/  (politique de l'outbox durable)
│           │       ├── audio/ (enregistrement, lecture, réassemblage reprenable)
│           │       ├── call/  (appels : protocole binaire, jitter buffer, moteur
│           │       │           audio microphone ↔ haut-parleur, garde cellule)
│           │       ├── wifi/  (SSID, changement de cellule)
│           │       └── ui/    (ViewModel, écrans Compose, thème)
│           └── test/java/com/jpo/cellnet/   # tests JVM (chunking, trame binaire,
│                                            # assemblage vocal, politique outbox,
│                                            # appels : protocole, jitter, garde cellule)
└── .github/workflows/
    ├── platformio-build.yml        # tests natifs + 3 firmwares + artefacts
    └── android-build.yml           # APK debug + release + artefact
```

### 15.4 Tests automatisés disponibles

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native
```

Les premiers tests couvrent : aller‑retour de trame, rejet CRC, troncature, charge utile
trop grande, resynchronisation après des octets parasites, flux octet par octet,
priorités de file, borne de file, anti‑famine audio, jeton (acquisition, budget,
revendication, saut de nœud mort), routage (déplacement, péremption, aller‑retour
de présence, données tronquées), enveloppe applicative et réassemblage de
fragments (désordre, doublon, index invalide, expiration).

La passe de fiabilisation y ajoute le suivi d'accusés par fragment (acquittement
ciblé, retransmission sélective, report tant que le pair est vivant, backoff,
échec après 4 essais), la séparation file applicative / file de contrôle, les
tours de jeton au repos, le réassemblage de 12 fragments, deux messages
concurrents dans le même contexte, le rejet d'un buffer de sortie trop petit, la
validation atomique d'une table de présence tronquée et la simulation de deux
émetteurs concurrents (livraison exactement une fois dans les deux sens, équité
sous file pleine, récupération après perte de jeton, reprise après un pair muet).

Les **appels** couvrent 38 tests (`test/test_call.cpp`) : session à deux
participants, autorisation des appels locaux, sessions intercellulaires
(invitation sortante et entrante, confirmation de sonnerie, acceptation, refus,
fin, expiration, déconnexion, nœud contradictoire, utilisateur occupé), appel à
soi‑même, utilisateur inconnu, utilisateur non connecté ici, invitation expirée,
déconnexion libérant les ressources, expiration d'un appel sans média,
limitation de cadence des invitations, borne de la table, validation du relais
média (uniquement dans une session établie, uniquement entre les deux
participants), codec de la trame binaire (aller‑retour et rejets) et les
garde‑fous structurels décrits en 17.12.

La **dorsale Wi‑Fi** couvre 21 tests (`test/test_link.cpp`) : cadrage binaire et
ses rejets, réassemblage d'un flux TCP, authentification par jeton partagé
(exactitude de la comparaison), charge utile de signalisation, routage en étoile
pour les huit paires de cellules, convergence d'une topologie en chaîne,
anti‑boucle et limite de sauts, effacement des routes après une perte de liaison,
politique de reconnexion, vivacité, et les cinq garde‑fous structurels qui
interdisent toute dépendance de la dorsale au bus RS485.

**Total : 124 tests natifs** (7 protocole + 19 cœur + 18 fiabilité = 44
d'origine, + 38 appels + 21 dorsale, + 21 ajoutés par l'audit du §19 :
8 file d'émission non bloquante et gardes structurelles, 8 identité/mobilité,
5 messagerie intercellulaire) et **77 tests JVM Android** (24 d'origine +
47 d'appel : protocole, tampon de gigue, garde cellulaire et appels
intercellulaires autorisés, + 6 message `hello` du §19.3).

---

## 16. Passe de fiabilisation : causes racines, corrections, preuves

Cette section documente la passe de fiabilisation menée sur le prototype : chaque
correction est reliée à une cause racine identifiée dans le code réel et au test
qui la protège.

### 16.1 Messages simultanés : pertes et lenteur

| Cause racine identifiée | Correction | Preuve |
|---|---|---|
| Délai d'accusé fixe de **300 ms**, plus court que la fenêtre de revendication du jeton (400 ms + 40 ms × nœud) : chaque trame était retransmise avant que le pair ait pu répondre | Première fenêtre portée à **400 ms**, doublée à chaque essai, **4 tentatives** | `testAckTrackerFailsOnlyAfterAllAttempts` |
| Retransmission déclenchée alors que le bus était actif (tempête de doublons) | La retransmission n'est programmée qu'après un **silence réel** : une trame reçue du pair après notre envoi reporte l'échéance | `testAckTrackerPostponesWhileThePeerIsAlive` |
| Le jeton était **abandonné** dès que la file locale était vide → 400–520 ms d'attente par message, désavantage systématique du nœud de plus grand identifiant | Le jeton **circule** vers le nœud vivant suivant et n'est libéré qu'après un tour complet sans données | `testArbiterCountsIdleRounds`, `testConcurrentSendersBothMessagesArriveExactlyOnce`, `testConcurrentSendersAreFairInBothDirections` |
| Valeurs de retour de `queue_.push()` **ignorées** (fragmentation, arbitrage, accusés) → perte silencieuse | Tous les appels vérifient le retour, comptent le refus et renvoient une contre‑pression explicite (`503 busy`) | `testControlQueueIsBoundedAndReported` |
| Une file pleine sacrifiait un **message utilisateur** pour faire de la place à un accusé | Deux files distinctes : **8 trames applicatives** + **6 trames de contrôle**, servies en priorité | `testControlQueueNeverTakesThePlaceOfUserMessages` |
| Suivi d'accusés limité à 4 entrées non sélectives (« ack tracking table full ») | `AckTracker` : une entrée **par message**, un bit **par fragment**, retransmission sélective | `testAckTrackerClearsOnlyTheAcknowledgedFragment`, `testAckTrackerRetransmitsOnlyTheMissingFragment` |
| Un accusé pouvait effacer l'entrée d'un autre message du même pair | `onAck()` cherche le message qui contient réellement la séquence acquittée | `testAckTrackerClearsOnlyTheAcknowledgedFragment` |
| Table de présence appliquée **partiellement** sur une annonce tronquée | Validation complète avant application (deux phases) | `testRoutingPartialPresenceStateIsRejectedAtomically` |

### 16.2 Messages longs

| Cause racine | Correction | Preuve |
|---|---|---|
| Un texte était limité à ~1,3 ko (3 fragments de bus) | Fragmentation **applicative** en blocs de 256 octets, relais opaque par la station, jusqu'à **32 ko** | `TextChunkerTest` (8 tests) |
| Fragments limités à 3 et buffers de réassemblage sur la **pile** (risque de débordement à 5,7 ko) | 12 fragments, buffers **statiques** membres de `MeshNode` | `testFragmentAssemblyTwelveFragmentsOutOfOrder` |
| Deux messages fragmentés simultanés se mélangeaient | Un contexte par `(nœud, type, messageSeq)`, 2 contextes | `testFragmentAssemblyKeepsTwoConcurrentMessagesApart` |
| Un message interrompu était perdu (aucune reprise) | Index des blocs validés persistés (table `transfers`), `text_nack` sélectif, reprise après redémarrage | tests `TextAssembler`/`BlockReassembly`, `OutboxPolicyTest` |

### 16.3 Vocaux incomplets

| Cause racine | Correction | Preuve |
|---|---|---|
| Corrélation ambiguë : un **seul** emplacement `pendingVoiceBlock` → un en‑tête JSON perdu pouvait associer les octets audio au mauvais bloc, voire mélanger deux vocaux | Trame binaire **auto‑identifiée** `[0xA5][0x01][msgId][seq][total][audio]`, en‑tête JSON informatif, repli compatible avec l'ancien format | `BinaryVoiceFrameTest` (5 tests) |
| Au démarrage, l'application **supprimait tous les `.part`** et purgeait les messages `receiving` → reprise impossible | Les transferts actifs sont **restaurés** (blocs validés + fichier partiel) et les blocs manquants redemandés ; seuls les transferts réellement abandonnés (> 24 h) sont nettoyés | `VoiceAssemblerTest.interruptedTransferIsResumedFromThePersistedBlocks`, `expiredPartialFilesAreRemoved` |
| Un vocal interrompu était rejoué **intégralement** | `voice_nack` mémorisé dans `transfers.missing` : seuls les blocs manquants sont renvoyés | `VoiceAssemblerTest` + `AppViewModel.resendVoice` |
| Un vocal pouvait être considéré comme livré sur simple réception d'un en‑tête | Le fichier n'est publié qu'après vérification de la **taille** et du **CRC32** complets | `VoiceAssemblerTest.wrongCrcNeverPublishesTheFile` |

### 16.4 Changements de cellule et persistance

| Cause racine | Correction | Preuve |
|---|---|---|
| Un seul échec de sonde HTTP (station occupée) démontait toute la session (WebSocket coupé, envois interrompus) | **Hystérésis** : jusqu'à 3 échecs consécutifs tolérés, session conservée | `MAX_PROBE_FAILURES` (`AppViewModel`) |
| La présence n'était réinscrite qu'après 30 s lors d'un changement de cellule | Réinscription **immédiate** sur la nouvelle station dès que le site change, puis reprise des transferts | `ensureConnected()` |
| La file d'envoi durable n'était rejouée qu'au démarrage de session ; un échec en cours de session restait en `failed` | **Worker d'outbox** permanent (3 s) avec backoff 2 s → 60 s et 8 tentatives, déclenché aussi à la reconnexion ; refus temporaire et refus permanent distingués | `OutboxPolicyTest` (5 tests) |
| L'identité pouvait sembler recréée lors d'un changement de cellule | L'identifiant stable vit dans `SharedPreferences` et n'est **jamais** dérivé du réseau : seul l'acheminement change | `UserProfileStore` (inchangé) |
| Ajout de la table de reprise sans perte de données | Migration Room **v1 → v2** additive (`transfers`) | `AppDatabase.MIGRATION_1_2` |

### 16.5 Limites inchangées (par conception)

* 3 nœuds, 16 utilisateurs par table de routage, 6 sockets WebSocket par station.
* Aucun stockage durable sur l'ESP32 : les files restent bornées en RAM, la
  persistance des conversations et des fichiers audio appartient au téléphone.
* Aucune authentification, trafic en clair, mots de passe d'AP de démonstration.
* Un destinataire totalement hors ligne ne reçoit rien : l'expéditeur conserve le
  message dans son outbox durable et le rejoue (texte **et** vocal).

### 16.6 Ce que la compilation et les tests réels ont révélé

La chaîne Android complète (JDK 17.0.20.1, Gradle 8.7, SDK 34) a été installée
pour compiler réellement l'application et exécuter ses tests. Trois défauts ont
été trouvés **uniquement** grâce à cela :

| Défaut | Nature | Correction |
|---|---|---|
| `unitTests.isReturnDefaultValues` dans un `build.gradle` **Groovy** | Erreur de DSL Gradle : échec à la configuration (`Could not set unknown property`) | `unitTests.returnDefaultValues` |
| `resumeVoiceTransfer(message, me, indexes)` avec `me: String` au lieu de `UserProfile` | Erreur de compilation Kotlin (`Type mismatch`) dans le gestionnaire `voice_nack` | Récupération du profil complet avant l'appel |
| `File.renameTo()` refusé alors qu'un descripteur reste ouvert sur le `.part` | Un vocal **validé** (taille + CRC32 corrects) pouvait être perdu à la publication | Repli par copie dans `VoiceAssembler.finish()` |

Les tests JVM ont également révélé que les identifiants de message doivent faire
**exactement 16 caractères** (contrat du format binaire) : le contrat est
désormais testé explicitement (`wrongMessageIdLengthIsRejected`).

---

## 17. Diagnostic de la communication du site A (bus RS485)

### 17.1 Ce qui a été observé sur les ports série

Deux cartes étaient réellement connectées (ponts USB‑UART **CP210x**, COM8 et
COM11). Identification par la bannière de démarrage :

| Port | Station | Nœud | Adresse | Firmware |
|---|---|---|---|---|
| COM11 | **SITE_A** | 1 | 10.0.0.1 | version fiabilisée (ligne `queues: 8 application + 6 control frames…`) |
| COM8 | **SITE_C** | 3 | 10.2.0.1 | idem |

Observations marquantes (capture horodatée des deux ports en parallèle) :

```
[+ 12,893s] COM8  | [    346][I][boot] local cellular prototype - SITE_C - firmware 1.0.0
[+ 12,896s] COM8  | [    448][I][node] SITE_C (node 3) ready on RS485 115200 baud, DE/RE GPIO4, termination=120R
[+ 12,906s] COM8  | [   5439][I][pres] peer node 1 online          <-- C REÇOIT les trames de A
[+ 12,910s] COM11 | [    328][I][boot] local cellular prototype - SITE_A - firmware 1.0.0
[+ 12,914s] COM11 | [    433][I][node] SITE_A (node 1) ready on RS485 115200 baud, DE/RE GPIO4, termination=120R
[+ 12,927s] COM11 | [   5971][I][pres] peer node 3 online          <-- A REÇOIT les trames de C
```

**Conséquences importantes :**

* la configuration de A est **correcte** (nœud 1, 115200 8N1, TX=GPIO17,
  RX=GPIO16, DE/RE=GPIO4, terminaison 120 Ω) : ce n'est donc **pas** une erreur de
  paramétrage de variante ;
* le lien physique **A ↔ C fonctionne au niveau trame** (les deux se voient), donc
  le problème de A n'est **pas** un simple défaut de câblage A/B ;
* en régime stable, le firmware est **silencieux** au niveau INFO (aucune ligne en
  60 s), ce qui rendait le diagnostic impossible sans instrumentation ;
* avant le redémarrage, **SITE_A émettait en continu** `local user presence
  expired` (3 123 lignes en 12,5 s, soit ≈ 250 lignes/s, saturation du port
  115200), alors que SITE_C était parfaitement silencieux. Ce spam n'est produit
  que par **un** point de log, appelé au plus **une fois par seconde** : le
  firmware ne pouvait pas le générer → preuve d'une **corruption d'état**.

### 17.2 Cause racine trouvée dans le code : débordement de pile au réassemblage

`MeshNode::handleApplicationFrame()` recopiait le message réassemblé dans un
`Frame` :

```cpp
Frame rebuilt = frame;                                        // Frame::payload = 480 octets
rebuilt.payloadLength = static_cast<uint16_t>(assembledLength);
memcpy(rebuilt.payload, assembledBuffer_, assembledLength);   // assembledLength <= 5712 !
```

`assembledLength` peut atteindre **kMaxReassembledPayload = 5 712 octets**
(12 fragments × 476) alors que `Frame::payload` en fait **480** : tout message
fragmenté provoquait un **débordement de pile de 948 à 5 232 octets** dans la
tâche `loop`. Conséquences possibles, toutes compatibles avec les symptômes
observés sur A :

* écrasement des variables locales et de l'adresse de retour des appelants →
  comportement erratique, voire redémarrage ;
* écrasement de la mémoire située au‑delà de la pile de la tâche (structures
  FreeRTOS, pilote UART) → **émission continue de données parasites** : c'est
  exactement le flood de `presence expired` observé, avec les débuts de lignes
  perdus ;
* boucle principale ralentie (bloquée dans `Serial.printf`) → trames RS485
  perdues, jeton rendu trop tard, routes non apprises → **station A isolée pour
  les données alors que ses pings passent encore** (ce que montrent les journaux).

Ce défaut est **antérieur** (il existait déjà avec 3 fragments, soit 948 octets de
débordement) ; l'élargissement de la fenêtre de réassemblage à 12 fragments l'a
aggravé. Il n'était atteint que par une **trame fragmentée**, donc par un message
applicatif dépassant une trame de bus — typiquement un texte long envoyé depuis
l'ancienne version de l'application Android (jusqu'à ~1 350 octets), la nouvelle
application découpant les textes en blocs de 256 octets qui tiennent en une seule
trame.

### 17.3 Correction appliquée

Livraison **sans copie** du message réassemblé :

* `FragmentAssembler` publie une **vue** du message complet
  (`lastPayload()` / `lastLength()`), valable jusqu'au prochain `accept()` ;
* `MeshNode` remet cette vue à l'application (`deliverToApp(type, payload, length)`) ;
  le `Frame` de 480 octets n'est plus jamais la destination d'un message réassemblé ;
* le gestionnaire applicatif (`AppServer::onAppFrame`) prend désormais
  `(type, payload, length)` au lieu d'un `Frame` ;
* le second buffer statique de 5 712 octets disparaît : **RAM 25,7 % → 24,0 %**.

Aucune modification du format de trame, du protocole, du jeton, du routage, des
accusés, de la fragmentation ni de l'application Android : les trois variantes
restent **strictement compatibles** entre elles.

### 17.4 Instrumentation série ajoutée (sans Wi‑Fi, sans HTTP)

1. **Compteurs d'octets** dans `Rs485Bus` (`rxBytes`, `txBytes`, erreurs COBS,
   débordements) : ils distinguent immédiatement « aucun octet reçu » de « des
   octets arrivent mais aucune trame ne se décode ».
2. **Résumé périodique** sur le port série, une ligne toutes les 10 s
   (≈ 150 octets, 0,01 % du débit) :

   ```
   [diag] [periodic] node=1 alive=0x7 token=1 idle=0 | rx=1234B/45f tx=2345B/50f |
   crc=0 cobsErr=0 retry=0 fail=0 dup=0 | tokRx=12 tokPass=11 tokClaim=1 |
   q=0/0 routes=3/5 presExp=0 reasm=0 | heap=210k loop=1500us
   ```

3. **Console série en lecture seule** (une lettre) : `d` = résumé, `p` = pairs et
   table de routage, `b` = compteurs du bus, `h` = aide. Aucun impact sur le
   protocole ni sur les données.
4. **Script de capture** `scripts/serial_capture.ps1` : horodatage commun à
   plusieurs ports (corrélation A/C), réinitialisation non destructive par
   DTR/RTS, et envoi de commandes (`-Send dp`).
5. Le log d'expiration de présence indique désormais les compteurs avant/après, et
   le nombre total d'expirations est exposé (`presExp`), ce qui rend le flood
   immédiatement visible s'il réapparaissait.

### 17.5 Procédure de vérification sur matériel

```powershell
# 1. Flasher A et C avec le firmware corrigé (voir §11)
# 2. Capturer le démarrage des deux cartes, en parallèle et horodaté
.\scripts\serial_capture.ps1 -Ports COM11,COM8 -Seconds 25 -Reset
# 3. Interroger les compteurs des deux stations (console série)
.\scripts\serial_capture.ps1 -Ports COM11 -Seconds 6 -Send d
.\scripts\serial_capture.ps1 -Ports COM8  -Seconds 6 -Send d
```

Lecture des résultats :

| Observation | Interprétation | Action |
|---|---|---|
| `rx=0B/0f` sur A | A ne reçoit **aucun octet** | câblage A/B, masse commune, transceiver, alimentation |
| `rx` croît mais `crc`/`cobsErr` croissent aussi | octets reçus, trames invalides | débit, terminaison, A/B inversés, bruit |
| `rx`/`tx` normaux mais `tokRx=0` | A reçoit des trames mais **pas de jeton** | ordre de circulation, masque des nœuds vivants |
| `tokRx>0` mais `tokPass=0` | A garde le jeton (masque incomplet) | voir `alive=` et la commande `p` |
| `alive` sans les nœuds 2/3 | A ne voit pas les autres | présence / pings |
| `presExp` qui grimpe très vite | régression du flood | noter la cadence et les lignes complètes |
| `routes` sans utilisateur distant | routage non appris | annonces `ROUTE_STATE` |
| `retry`/`fail` qui grimpent | transmissions non acquittées | ACK, collisions, jeton |

---

## 17. Appels audio en direct (chemin local)

> Cette section décrit le chemin **local** (deux participants sur la même
> cellule). La **section 18** décrit l'extension qui permet les appels entre
> **toutes** les cellules via une dorsale IP Wi‑Fi, sans que l'audio passe jamais
> par le bus RS485.

### 17.1 Ce qui a été ajouté

Deux utilisateurs connectés **au même point d'accès ESP32** peuvent désormais
s'appeler et se parler en temps réel, comme au téléphone : le microphone de l'un
est diffusé sur le haut‑parleur (ou l'écouteur) de l'autre, et inversement,
simultanément.

L'appel est un **ajout indépendant** : les messages textuels, les messages
vocaux enregistrés, la présence, le répertoire, les accusés, la fragmentation,
l'outbox, le routage intercellulaire et l'arbitrage RS485 ne sont pas modifiés.
Aucune route, aucun événement WebSocket, aucune table Room, aucune migration et
aucun mécanisme de fiabilité n'a été supprimé ni renommé.

### 17.2 Message vocal enregistré ≠ appel en direct

| | Message vocal enregistré | Appel en direct |
|---|---|---|
| Nature | fichier fini (AAC‑LC `.m4a`) | flux PCM continu bidirectionnel |
| Durée | 60 s maximum, puis envoi | illimitée, tant que les deux sont connectés |
| Transport | **bus RS485** (blocs de 400 octets, `kTypeVoiceBlock`) | **WebSocket binaire local**, jamais le bus |
| Portée | intercellulaire (A → C possible) | **une seule cellule**, refusé entre cellules |
| Stockage | fichier sur le téléphone, reprise possible | aucun : rien n'est stocké, ni sur le téléphone ni sur l'ESP32 |
| Codec | AAC‑LC 16 kHz mono 24 kbit/s | PCM 16 bits signé 8 kHz mono |
| Fiabilité | CRC32 global + retransmission sélective des blocs | temps réel : gigue absorbée, trous comblés par du silence |

### 17.3 Architecture

```
Téléphone A1 ──Wi-Fi CELL_SITE_A──> ESP32 SITE_A <──Wi-Fi CELL_SITE_A── Téléphone A2
        \______ signalisation HTTP /api/call/* ______/
        \______ média : trames binaires WebSocket, relayées par la station ______/
```

* **Signalisation** : routes HTTP locales `/api/call/invite|accept|reject|cancel|hangup`
  et `GET /api/call/state`, plus des événements WebSocket `call_invite`,
  `call_ringing`, `call_accepted`, `call_rejected`, `call_cancelled`,
  `call_ended`, `call_timeout`.
* **Média** : la station **relaie** les trames binaires WebSocket reçues d'un
  client vers l'autre client de la **même** station. Elle ne les décode pas,
  ne les stocke pas et ne les met jamais en file d'émission RS485.
* **Décision** : la table de sessions vit dans `lib/cell_call`, une
  bibliothèque **plateforme‑neutre qui n'inclut ni `cell_protocol.h` ni
  `cell_core.h`** : elle ne peut structurellement pas atteindre le bus.

### 17.4 Trame média (WebSocket binaire)

| Offset | Taille | Champ |
|---|---|---|
| 0 | 1 | magic `0xA7` (audio d'appel — distinct du `0xA5` des vocaux enregistrés) |
| 1 | 1 | version (`1`) |
| 2 | 2 | drapeaux, réservé (`0`) |
| 4 | 8 | identifiant d'appel, ASCII alphanumérique |
| 12 | 2 | numéro de séquence (little endian, par sens) |
| 14 | 2 | longueur de charge (little endian) |
| 16 | n | PCM 16 bits signé, mono, 8 kHz |

640 octets utiles pour 40 ms ⇒ **25 trames/s et par sens** (≈ 16 kB/s par sens,
33 kB/s par appel). Le `0xA7` est distinct de **tous** les types de trame du bus
et de la version du protocole : une trame média ne peut pas être confondue avec
une trame RS485 (vérifié par test).

### 17.5 Isolation cellulaire (règles appliquées côté station)

> **Évolution :** depuis la section 18, les appels **entre cellules** sont pris en
> charge. Les règles ci‑dessous décrivent le **chemin local** (deux participants
> sur la même station), qui reste inchangé ; le chemin intercellulaire est décrit
> en 18.9 et 18.10. Un appel vers une autre cellule n'est plus refusé : il est
> acheminé par la dorsale Wi‑Fi.

Sur le **chemin local**, un appel est refusé si :

1. le **demandeur** n'a pas de socket WebSocket actif sur **cette** station ;
2. le **destinataire** n'a pas de socket WebSocket actif sur cette station
   (`409 unavailable` : l'appel n'était pas destiné à cette cellule) ;
3. l'un des deux est déjà dans une session (`409 busy`) ;
4. le demandeur s'appelle lui‑même (`400 self-call`) ;
5. le destinataire est inconnu (`404 unknown-user`) ou hors ligne
   (`409 recipient-offline`) ;
6. le demandeur enchaîne les invitations (`429 call-too-soon`).

Sur le **chemin intercellulaire**, les mêmes règles s'appliquent au participant
**présent sur la station**, et la station distante applique les siennes à son
propre participant (voir 18.8 et 18.10).

L'identité de cellule **déclarée par l'application n'est jamais utilisée** : la
preuve d'appartenance est le socket WebSocket réellement connecté à la station.
La vérification est donc **serveur** (la station refuse) **et** cliente (le
bouton d'appel est désactivé hors cellule, avec le motif affiché).

### 17.6 Ports et protocoles

| Usage | Transport | Port |
|---|---|---|
| Signalisation d'appel | HTTP POST/GET (JSON) | **8080** (existant) |
| Événements d'appel | WebSocket texte | **8081** (existant) |
| Audio de l'appel | WebSocket **binaire** | **8081** (existant) |

Aucun port nouveau n'est ouvert, aucun service externe n'est contacté : les
appels fonctionnent **sans Internet**.


### 17.7 Gigue, pertes et contrôle de flux

* **Gigue** : un `JitterBuffer` réordonne les trames ; l'origine du flux est la
  première trame reçue. Un trou est comblé par du **silence** dès que 3 trames
  plus récentes attendent, afin que l'horloge de lecture ne s'arrête jamais. Une
  trame arrivée après le passage de son créneau est **abandonnée** (le flux ne
  se décale pas).
* **Latence bornée** : le tampon est plafonné à 12 trames (480 ms) ; les plus
  anciennes sont écartées en premier.
* **Pertes** : pas de retransmission (ce serait trop tard pour du temps réel) ;
  la perte est masquée par le comblement de silence, et comptée
  (`audioSilent`, `audioRejected` dans `/api/health`).
* **Contrôle de flux** : `AudioTrack.write()` en mode bloquant cadence
  naturellement la boucle de lecture à une trame par 40 ms ; la capture suit la
  même cadence. La station relaie sans bufferiser.
* **Micro coupé** : les trames sont remplacées par du silence et **continuent
  d'être envoyées** — le correspondant entend du silence (pas une coupure) et la
  surveillance de flux de la station reste valide.
* **Surveillance** : une invitation sans réponse expire en 30 s ; un appel établi
  sans aucun média pendant 20 s (station) / 8 s (téléphone) est terminé
  proprement des deux côtés.

### 17.8 Interface Android

* **Bouton d'appel** dans l'en‑tête de la conversation, désactivé hors cellule.
* **Écran d'appel plein écran** : nom du contact, état (« Appel en cours… »,
  « Appel entrant », « En communication »), durée de l'appel, mention « Appel
  local à la cellule uniquement ».
* **Appel entrant** : boutons « Accepter » / « Refuser ».
* **Appel établi** : boutons micro (muet), haut‑parleur / écouteur, et
  « Raccrocher » (ou « Annuler » pendant la sonnerie).
* **Journal** : une ligne discrète au‑dessus du champ de saisie rappelle le
  dernier appel avec ce contact (état, durée, heure). Les appels sont
  journalisés dans la table Room `calls` — **métadonnées uniquement, jamais
  l'audio**.
* Les conversations, les messages et les vocaux existants ne sont ni déplacés ni
  remplacés : l'appel est un écran superposé qui disparaît à la fin de l'appel.

### 17.9 Permissions et cycle de vie

| Permission | Usage |
|---|---|
| `RECORD_AUDIO` | capture du microphone (déjà nécessaire aux vocaux) |
| `MODIFY_AUDIO_SETTINGS` | mode `MODE_IN_COMMUNICATION`, routage haut‑parleur / écouteur |

* La permission microphone est demandée **au moment de l'appel** si elle n'est
  pas encore accordée ; en cas de refus, un message explique que l'appel est
  impossible et rien n'est ouvert.
* Le focus audio est demandé (`AUDIOFOCUS_GAIN_TRANSIENT`) à l'ouverture de
  l'appel et rendu à la fermeture ; le mode audio est remis à `MODE_NORMAL`.
* Une seule session audio à la fois : lancer un appel pendant un enregistrement
  (ou l'inverse) est refusé avec un message explicite.
* Perte du WebSocket, changement de cellule, `onCleared()` : le moteur audio est
  libéré, la session est fermée côté station (déconnexion) et l'autre
  participant est notifié.
* **Arrière‑plan** : l'appel est maintenu tant que l'activité vit. Pour un appel
  qui survit à la mise en arrière‑plan, un service de premier plan
  (`foregroundServiceType="microphone"`, notification persistante,
  `FOREGROUND_SERVICE_MICROPHONE` sur Android 14) serait nécessaire ; **il n'est
  pas implémenté** (voir 17.13).

### 17.10 Dépendances ajoutées

**Aucune dépendance nouvelle.** Le côté Android utilise uniquement le framework
(`AudioRecord`, `AudioTrack`, `AudioManager`, `AudioFocusRequest`), OkHttp
(déjà présent pour le WebSocket) et Room (déjà présent). Côté firmware, seuls
`WebSockets` et `ArduinoJson` déjà utilisés sont employés. Les versions Kotlin,
Gradle, AGP, SDK et Room sont inchangées.

### 17.11 Compilation et tests

```powershell
# 1. Tests natifs (44 tests d'origine + 28 tests d'appel = 72)
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native

# 2. Les trois firmwares
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_a -e site_b -e site_c

# 3. Application Android (tests JVM + APK)
cd mobile
gradle testDebugUnitTest assembleDebug assembleRelease
```

Attendu : `72 test cases: 72 succeeded`, `3 succeeded` (RAM 24,1 %, Flash
63,8 %), `BUILD SUCCESSFUL` avec **69 tests JVM, 0 échec**.


### 17.12 Preuve que l'audio d'appel ne passe pas par le RS485

Cette propriété n'est pas seulement documentée : elle est **testée**.

**Garde‑fous structurels** (dans `test/test_call.cpp`, exécutés par
`pio test -e native`) :

| Test | Ce qu'il vérifie |
|---|---|
| `testCallModuleDoesNotDependOnTheBusLibraries` | `cell_call.h` / `cell_call.cpp` n'incluent ni `cell_protocol.h` ni `cell_core.h`, et ne mentionnent aucun symbole du bus (`FrameQueue`, `BusArbiter`, `AckTracker`, `submitAppPayload`, `cobsEncode`, `crc16Ccitt`) |
| `testStationCallHandlersNeverSubmitToTheMesh` | les gestionnaires `/api/call/*` et le relais média de `app_server.cpp` ne contiennent **aucun** `submitAppPayload`, aucun `queue_.push`, et aucun nouveau type de trame (`kTypeCall`, `kTypeAudio` absents du fichier) |
| `testMeshNodeAndProtocolWereNotTouchedByTheCallFeature` | `mesh_node.h`, `mesh_node.cpp` et `cell_protocol.h` ne contiennent **aucune** référence à `CallTable`, `kCallAudio`, `handleCall`, `call_audio` ou `cell_call.h` |
| `testCallAudioMagicCannotBeMistakenForABusFrame` | le magic `0xA7` diffère de **tous** les `MessageType`, de `kProtocolVersion` et du délimiteur COBS `0x00` ; une trame média (656 o) est plus grande que la plus grande trame de bus possible (491 o) |

**Compteurs d'exécution** (`/api/health` → `calls`) : `audioFrames`,
`audioRelayed`, `audioRejected`, `audioSilent`, `transport =
"websocket-binary"`. Il n'existe **aucun** compteur d'appel dans le bloc `bus` :
par construction, un appel ne peut rien y incrémenter.

**Vérification manuelle** possible : pendant un appel actif, le moniteur série
d'une station ne doit montrer **aucune** émission sur le bus (`tx` stable), les
LED verte/rouge RS485 doivent rester éteintes, et `GET /api/health` doit
augmenter `calls.audioRelayed` sans que `bus.sent` bouge.

### 17.13 Limites connues

* **Appel en arrière‑plan** : non pris en charge (pas de service de premier
  plan). Si l'application est mise en arrière‑plan puis tuée par le système,
  l'appel se termine ; la station libère la session à la déconnexion du socket.
* **Deux participants maximum** par appel (pas de conférence).
* **4 sessions simultanées** au maximum par station (`kMaxCallSessions`), soit
  deux appels en parallèle ; au‑delà, `503 call-table-full`.
* **Qualité audio** : PCM 8 kHz mono 16 bits (qualité téléphonique). Aucun
  codec large bande (Opus) n'est utilisé, pour ne dépendre d'aucune
  bibliothèque supplémentaire.
* **Pas de chiffrement** : comme le reste du prototype, l'audio circule en clair
  sur le Wi‑Fi local. Aucun chiffrement n'a été implémenté, donc aucun
  chiffrement n'est annoncé.
* **Pas d'écho annulation (AEC)** : l'usage du haut‑parleur sans écouteur peut
  produire un écho sur certains appareils. `VOICE_COMMUNICATION` active
  l'annulation d'écho matérielle quand elle existe.
* **Un appel ne survit pas** à un changement de cellule ni à un redémarrage de
  la station : la session est fermée et les deux participants sont notifiés.
* Le journal des appels n'est pas purgé automatiquement (contrairement aux
  transferts) : la méthode `purgeOlderThan` existe mais n'est pas planifiée.

### 17.14 Validation matérielle restant à faire

> Les essais d'appel **intercellulaires** sont décrits en 18.15 ; ceux ci‑dessous
> concernent le chemin **local** et restent valables.

Aucun téléphone, aucune carte ESP32 et aucune liaison Wi‑Fi réelle n'étant
accessible depuis cet environnement, **aucun appel réel n'a été passé**. Ce qui
reste à valider sur banc :

1. **Appel autorisé (même cellule)** — deux téléphones sur `CELL_SITE_A`,
   utilisateurs A1 et A2 : A1 appelle A2, A2 accepte, la conversation
   bidirectionnelle doit être audible dans les deux sens.
2. **Appel refusé (cellules différentes)** — A1 sur `CELL_SITE_A` appelle B1 sur
   `CELL_SITE_B` : l'appel doit être refusé avec « le correspondant est sur une
   autre cellule », **sans aucune émission RS485** (vérifier les LED et
   `bus.sent`).
3. **Refus / occupation** — A2 refuse : A1 voit « Appel refusé ». A2 déjà en
   appel : A3 obtient « Correspondant déjà en communication ».
4. **Non‑régression messagerie** — pendant et après un appel, vérifier que les
   textes et les vocaux fonctionnent toujours **en local** et **entre cellules**
   (A→B, A→C, B→C), avec accusés « livré » puis « lu ».
5. **Robustesse** — couper le Wi‑Fi pendant un appel (doit se terminer proprement
   des deux côtés) ; éteindre la station pendant un appel ; laisser sonner sans
   répondre (expiration à 30 s) ; appeler en micro coupé (le correspondant doit
   entendre du silence et l'appel doit rester actif).
6. **Charge** — mesurer `calls.audioRelayed` / `calls.audioSilent`,
   `system.heapMin` et `maxLoopUs` de `/api/health` pendant un appel de 5 min, et
   vérifier qu'aucun message ni accusé n'est perdu en parallèle.
7. **Appareils** — tester sur au moins deux modèles Android (routage
   écouteur / haut‑parleur, annulation d'écho, latence perçue).

Aucun de ces points n'est couvert par un test automatisé : les tests fournis
vérifient la logique de session, l'isolation cellulaire, le format des trames,
le tampon de gigue et l'absence de dépendance au bus — **pas** la qualité audio
sur matériel réel.

---

## 18. Dorsale Wi‑Fi inter‑stations : appels entre toutes les cellules

### 18.1 Le constat qui impose une dorsale

Les trois stations diffusent chacune leur propre point d'accès, sur des
sous‑réseaux **distincts** (`10.0.0.1/24`, `10.1.0.1/24`, `10.2.0.1/24`), sans
routeur, sans câble et sans Internet. **Aucun chemin IP n'existe entre elles.**
Le seul lien inter‑stations est le bus RS485, interdit pour l'audio.

Pour permettre les appels intercellulaires, il fallait donc **créer** une
dorsale IP, en complément des points d'accès existants.

### 18.2 Topologie retenue : étoile, `SITE_A` concentrateur

```
   Téléphones A                Téléphones B                Téléphones C
        |                           |                           |
   Wi-Fi CELL_SITE_A           Wi-Fi CELL_SITE_B           Wi-Fi CELL_SITE_C
        |                           |                           |
   [ ESP32 SITE_A ]  <-- STA --  [ ESP32 SITE_B ]         [ ESP32 SITE_C ]
     (concentrateur)  <--------------- STA ----------------------
```

| Station | AP (inchangé) | Interface STA | Rôle |
|---|---|---|---|
| `SITE_A` | `CELL_SITE_A` · `10.0.0.1` | aucune | **concentrateur** : accepte les deux liaisons |
| `SITE_B` | `CELL_SITE_B` · `10.1.0.1` | connectée à `CELL_SITE_A` | feuille |
| `SITE_C` | `CELL_SITE_C` · `10.2.0.1` | connectée à `CELL_SITE_A` | feuille |

**Pourquoi l'étoile** et non un maillage complet : un ESP32 n'a **qu'une seule
radio**. Le mode `WIFI_AP_STA` impose que l'AP et la STA partagent le même canal,
et la STA ne peut être associée qu'à **un** AP à la fois. Un maillage complet
(chaque station connectée aux deux autres) est donc impossible avec ce matériel.
L'étoile offre le meilleur compromis : **au plus un relais** pour n'importe
quelle paire de cellules, et une seule association par feuille.

**Contrainte de canal** : les trois AP sont diffusés sur le **canal 1**
(`APP_WIFI_CHANNEL`), valeur identique sur les trois stations. Si l'association
STA se faisait sur un autre canal, l'AP suivrait et **les téléphones de la
cellule seraient déconnectés** : la station journalise une erreur explicite si
elle constate une divergence.

### 18.3 Aucune route IP, aucun NAT, aucun conflit DHCP

La connexion TCP est **toujours ouverte par la station feuille** vers l'adresse
connue du concentrateur (`10.0.0.1:8082`) ; le concentrateur **répond sur la
socket déjà ouverte**. Conséquences :

* l'adresse obtenue par DHCP côté STA n'a **jamais** à être connue des autres
  stations : aucune adresse statique à réserver, **aucun risque de collision
  avec la plage DHCP** des téléphones ;
* aucun transfert IP (`ip_forward`, NAPT) n'est activé : les sous‑réseaux restent
  **isolés**, ce qui évite d'exposer par inadvertance les services d'une cellule
  aux clients d'une autre ;
* les **téléphones ne changent rien** : ils restent sur l'AP de leur cellule,
  avec leur SSID, leur mot de passe et leur adresse. Aucun routage n'est demandé
  aux téléphones, qui ne peuvent de toute façon pas se joindre directement
  (sous‑réseaux distincts sans routeur).

### 18.4 Paramètres de configuration

| Macro | Rôle | A | B | C |
|---|---|---|---|---|
| `APP_WIFI_CHANNEL` | canal AP + STA (identique partout) | 1 | 1 | 1 |
| `APP_LINK_PORT` | port TCP de la dorsale (8080/8081 inchangés) | 8082 | 8082 | 8082 |
| `APP_LINK_UPSTREAM_NODE` | station à laquelle s'attacher (`0` = concentrateur) | 0 | 1 | 1 |
| `APP_LINK_UPSTREAM_SSID` | SSID de l'AP amont | `""` | `CELL_SITE_A` | `CELL_SITE_A` |
| `APP_LINK_UPSTREAM_PASSWORD` | mot de passe de l'AP amont | `""` | `cellsitea` | `cellsitea` |
| `APP_LINK_UPSTREAM_IP` | adresse de l'AP amont | `10.0.0.1` | `10.0.0.1` | `10.0.0.1` |
| `APP_LINK_TOKEN` | secret partagé authentifiant les stations | *(démo)* | *(démo)* | *(démo)* |
| `APP_LINK_RETRY_MS` | délai de reconnexion de la STA | 2000 | 2000 | 2000 |

### 18.5 Secrets : valeurs de démonstration

`APP_LINK_TOKEN` est **une valeur de démonstration** (`cellnet-demo-link-token`)
commise dans `platformio.ini`, comme le sont déjà les mots de passe des points
d'accès. Le firmware **affiche un avertissement au démarrage** tant que la valeur
de démonstration est utilisée :

```
[I][boot] ... 
[W][boot] the inter-station link token is still the DEMONSTRATION value from
          platformio.ini: change it before any real deployment
```

Pour changer les secrets sur un déploiement réel : modifier `platformio.ini`
(par environnement) ou fournir les macros par un fichier de configuration local
non commis (`extra_configs`, `build_flags` personnalisés). Aucun secret de
production ne doit être poussé dans Git. Les mots de passe des AP existants
n'ont **pas** été modifiés.



### 18.6 Protocole de la dorsale (lib/cell_link)

Flux TCP, trames binaires préfixées par un en‑tête de 12 octets :

| Offset | Taille | Champ |
|---|---|---|
| 0 | 1 | magic `0xC3` |
| 1 | 1 | version (`1`) |
| 2 | 1 | type (voir ci‑dessous) |
| 3 | 1 | drapeaux (réservé) |
| 4 | 1 | nœud source (**origine réelle**, préservée par les relais) |
| 5 | 1 | nœud destination (destination **finale**, pas le prochain saut) |
| 6 | 1 | compteur de sauts (incrémenté à chaque relais) |
| 7 | 1 | réservé |
| 8 | 4 | longueur de charge (little endian) |
| 12 | n | charge utile |

| Type | Valeur | Rôle |
|---|---|---|
| `kLinkHello` | `0x81` | identité + jeton partagé (feuille → concentrateur) |
| `kLinkHelloAck` | `0x82` | liaison acceptée |
| `kLinkReject` | `0x83` | jeton refusé, socket fermée |
| `kLinkHeartbeat` | `0x84` | vivacité, 5 s |
| `kLinkRouteState` | `0x85` | nœuds joignables depuis l'émetteur (masque 1 octet) |
| `kLinkCallSignal` | `0x90` | signalisation d'appel (58 octets fixes) |
| `kLinkCallAudio` | `0x91` | trame audio d'appel (trame média `0xA7` telle quelle) |

La numérotation du lien (`0x81`…`0x91`) est **disjointe** de celle du bus
(`0x01`…`0x14`) : une trame de dorsale ne peut pas être confondue avec une trame
RS485 (vérifié par test).

**Charge utile de signalisation** (58 octets, largeurs fixes, aucun parsing
variable) : `[callId:8][action:1][raison:1][de:24][vers:24]`. Actions :
`invite`, `ringing`, `accept`, `reject`, `cancel`, `hangup`, `ended`.

**Charge utile audio** : la **même** trame média que sur le WebSocket local
(`0xA7` magic, callId, séquence, PCM 16 bits mono 8 kHz, 40 ms = 640 octets).
Aucune conversion, aucun base64 : la trame est transmise telle quelle d'un bout à
l'autre du chemin.

### 18.7 Routage à sauts limités (et sans boucle)

Chaque station connaît :

* ses **pairs directs** (socket établie et authentifiée) ;
* sa **station amont** (`APP_LINK_UPSTREAM_NODE`) ;
* les **routes apprises** : un pair annonce périodiquement (10 s) les nœuds qu'il
  peut joindre ; une station en déduit les cellules qu'elle n'a pas en direct
  (utile à une topologie en chaîne).

Décision de relais pour une trame destinée à `dst` reçue de `incoming` :

1. `dst == moi` → traitement local (pas de relais) ;
2. `sauts >= kLinkMaxHops (3)` → **rejet** ;
3. prochain saut = direct → sinon route apprise → sinon amont ; si aucun, **rejet** ;
4. si le prochain saut est **la station d'où vient la trame** → **rejet** (anti‑boucle) ;
5. sinon `sauts++` et émission vers le prochain saut.

La perte d'une liaison **efface** la route directe **et toutes les routes
apprises à travers elle** (`forgetVia`), ce qui évite un trou noir après une
déconnexion.

### 18.8 Authentification et durcissement

| Mesure | Mise en œuvre |
|---|---|
| Authentification des stations | `kLinkHello` porte le nœud + le **jeton partagé** ; comparaison en **temps constant** (`linkTokenMatches`), jamais un `strcmp` |
| Jeton vide | refusé : une station mal configurée ne s'ouvre pas à tout le monde |
| Aucune information rendue | le refus ne renvoie que « refusé » ; le jeton n'est **jamais** réémis |
| Avant l'handshake | toute trame reçue avant authentification ferme la socket |
| Une socket par pair | une nouvelle connexion remplace la précédente (pas d'usurpation par accumulation) |
| Boucles | compteur de sauts + « jamais vers l'expéditeur » |
| Destinations non autorisées | seuls `kLinkCallSignal` et `kLinkCallAudio` sont relayés ; tout autre type vers un tiers est rejeté |
| Exposition | aucun service exposé à Internet, aucun transfert IP activé, aucun port ouvert hors du réseau CellNet |
| Libération des ressources | délai de 15 s sans octet reçu → socket fermée, routes effacées, sessions d'appel de ce pair terminées |
| Chiffrement | **aucun** chiffrement de bout en bout n'est implémenté, donc **aucun n'est annoncé** : la dorsale circule en clair sur le Wi‑Fi local, comme le reste du prototype |


### 18.9 Chemin de signalisation et chemin audio

**Appel local (A1 → A2, même cellule)** — chemin **inchangé** :

```
Téléphone A1 --HTTP /api/call/invite--> ESP32 SITE_A --WebSocket--> Téléphone A2
Téléphone A1 --WebSocket binaire 0xA7--> ESP32 SITE_A --WebSocket binaire 0xA7--> Téléphone A2
```
La dorsale n'est **pas** utilisée : un appel local ne consomme aucune bande
passante inter‑stations.

**Appel intercellulaire (A1 → B1)** :

```
1. Téléphone A1 --HTTP /api/call/invite {callId,from,to}--> ESP32 SITE_A
2. SITE_A : table de routage -> B1 est sur le nœud 2 -> session locale « remote »
3. SITE_A --TCP 8082 kLinkCallSignal{invite}--> ESP32 SITE_B
4. SITE_B : inviteIncoming() -> évènement WebSocket call_invite -> Téléphone B1
5. SITE_B --TCP 8082 kLinkCallSignal{ringing}--> SITE_A -> call_ringing -> A1
6. B1 accepte --HTTP /api/call/accept--> SITE_B -> kLinkCallSignal{accept} -> SITE_A
7. SITE_A -> call_accepted -> A1 ; les deux applications ouvrent leur micro

Audio (bidirectionnel, en parallèle) :
   A1 --WS binaire 0xA7--> SITE_A --TCP 8082 kLinkCallAudio--> SITE_B --WS binaire 0xA7--> B1
   B1 --WS binaire 0xA7--> SITE_B --TCP 8082 kLinkCallAudio--> SITE_A --WS binaire 0xA7--> A1
```

**B ↔ C** : aucune socket directe (l'étoile n'en a pas). La trame est adressée au
nœud 3 avec `dstNode=3` ; SITE_A la **relaie** vers SITE_C en incrémentant le
compteur de sauts. Un seul saut de relais, jamais deux.

**Aucune trame d'appel n'est remise au bus**, quel que soit le cas : les
gestionnaires `/api/call/*`, le relais média WebSocket et la dorsale ne
contiennent aucun appel à la file d'émission RS485.

**Un appel n'est jamais annoncé comme établi avant que le chemin audio existe** :
le côté appelant reste en « Appel en cours… » (`call_dialing`) jusqu'à ce que la
station distante confirme que le téléphone du destinataire **sonne**
(`kSignalRinging`), et ne passe « En communication » qu'après l'acceptation
réelle (`kSignalAccept`).

### 18.10 Présence et découverte : réutilisation, pas de duplication

Aucune table de présence parallèle n'a été créée. La **table de routage
existante** (`RoutingTable`, alimentée par les annonces `ROUTE_STATE` du bus
RS485) répond déjà à la seule question utile : *sur quelle station se trouve cet
utilisateur ?* Elle est donc utilisée telle quelle pour choisir la destination
d'une invitation. Le RS485 conserve donc **toutes** ses fonctions (présence,
répertoire, routage, jeton, files, accusés) : il n'est simplement **jamais**
emprunté par un paquet audio ou par une trame de signalisation d'appel.

Le mécanisme ajouté côté dorsale est limité à ce qui est indispensable :

| Besoin | Mécanisme |
|---|---|
| Une station sait qu'une autre est joignable | authentification `kLinkHello`/`kLinkHelloAck` → pair direct |
| Une station sait quelles cellules elle peut joindre | annonce `kLinkRouteState` (masque 1 octet, toutes les 10 s et à l'établissement) |
| Une liaison est morte | silence de plus de 15 s → socket fermée, routes effacées |
| Un utilisateur change de cellule | inchangé : sa station publie sa présence sur le bus, la table de routage se met à jour |
| Éviter les doublons d'invitation | `inviteRemote`/`inviteIncoming` refusent si l'utilisateur local est déjà dans une session, et l'anti‑abus de cadence s'applique |
| Informations contradictoires | la table de routage existante est seule juge (une entrée par utilisateur, mise à jour par la station propriétaire) |
| Confidentialité | seules la signalisation et l'audio **des participants** traversent la dorsale : aucun contenu de conversation ni paquet audio n'est diffusé aux stations non concernées |


### 18.11 Interface Android

* Le **bouton d'appel** de la conversation est actif dès qu'une cellule est
  identifiée et que la station répond : il n'est plus limité aux utilisateurs de
  la cellule courante.
* L'en‑tête de la conversation indique la destination quand elle est distante :
  « Vous êtes sur SITE_A · appel vers SITE_B ».
* L'écran d'appel affiche « Appel inter-cellules · Wi-Fi CellNet · vers SITE_B »
  (ou « Appel local à la cellule · Wi-Fi CellNet »).
* Les états affichés sont ceux réellement reçus : « Appel vers SITE_B… » pendant
  l'acheminement, « Appel en cours… » quand le destinataire sonne, « En
  communication » seulement après acceptation.
* Les refus sont explicites : cellule du destinataire inconnue, liaison Wi‑Fi
  entre stations indisponible, correspondant occupé / indisponible / non
  connecté, invitation expirée.
* Le reste de l'application est inchangé : messages, vocaux enregistrés,
  répertoire, persistance Room, historique, découpage du texte, reprise des
  transferts.

### 18.12 Tests exécutés

```powershell
# Tests natifs : 124 (103 de la dorsale et des appels + 21 pour l'audit du §19)
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native

# Les trois firmwares
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e site_a -e site_b -e site_c

# Application Android : 77 tests JVM + APK
cd mobile
gradle testDebugUnitTest assembleDebug assembleRelease
```

**Résultats obtenus sur ce poste** : `124 test cases: 124 succeeded` ·
`site_a`/`site_b`/`site_c` SUCCESS (RAM 27,1 %, Flash 65,2 %) ·
`BUILD SUCCESSFUL` avec **77 tests JVM, 0 échec**.

> Ces chiffres sont ceux **avant** la section 19, qui ajoute 21 tests natifs et
> 6 tests JVM : le total après audit est de **124 natifs** et **77 JVM** (§19.9).

Couverture ajoutée (natif) :

| Domaine | Tests |
|---|---|
| Trame de dorsale | aller‑retour, 6 formes de rejet (magic, version, longueur incohérente, source nulle, troncature) |
| Réassemblage de flux TCP | deux trames dans un flux, resynchronisation après octets parasites |
| Authentification | aller‑retour `hello`, jeton exact (préfixe, plus long, un caractère différent, jeton vide refusé) |
| Signalisation | aller‑retour 58 octets, rejets (taille, action inconnue, champ non terminé, identifiant invalide) |
| Routage étoile | les **8 paires** A→A, A→B, A→C, B→A, B→B, B→C, C→A, C→B, C→C (les paires locales sans saut de lien) |
| Routage chaîne | convergence par annonce, amont, pas de trou noir |
| Anti‑boucle | jamais de retour à l'expéditeur, limite de sauts, destination inconnue |
| Reconnexion / vivacité | délai de nouvelle tentative, comptage, pair muet |
| Sessions intercellulaires | invitation sortante/entrante, confirmation de sonnerie, acceptation, refus, fin, expiration, déconnexion, utilisateur occupé, nœud contradictoire, appel local toujours possible sans dorsale |
| Garde‑fous structurels | `cell_link` sans dépendance au bus ; `station_link`/`wifi_backbone` sans référence au maillage ; `mesh_node`/`cell_protocol` sans référence aux appels ni à la dorsale ; magics disjoints ; chemin des vocaux enregistrés intact |

Couverture ajoutée (JVM Android) : les **8 paires de cellules autorisées**, refus
d'une cellule hors déploiement, indicateur « appel intercellulaire », machine à
phases, plus les tests de trame média et de tampon de gigue déjà présents.

### 18.13 Limites connues

* **Topologie en étoile** : si `SITE_A` (concentrateur) s'arrête, les appels
  **locaux** de B et C continuent, mais les appels **B ↔ C** deviennent
  impossibles (B et C perdent aussi l'accès à A). Un maillage complet
  nécessiterait une seconde radio par station.
* **Une seule association STA par station** : conséquence directe du matériel
  (radio unique). La topologie en chaîne fonctionne (via les annonces de routes)
  mais ajoute un saut pour les cellules éloignées.
* **Canal unique** : les trois AP doivent rester sur le même canal. Changer le
  canal d'une seule station casserait son AP ou sa liaison.
* **Débit du concentrateur** : un appel B ↔ C traverse `SITE_A` deux fois (un
  sens par participant), soit ≈ 33 kB/s en plus de son propre trafic. C'est
  modeste pour du Wi‑Fi, mais le concentrateur porte la charge de tous les appels
  intercellulaires.
* **Pas de chiffrement** : la dorsale circule en clair, comme le reste du
  prototype.
* **Deux participants par appel**, 4 sessions par station.
* **Le relais audio reste côté ESP32** : l'option « audio direct entre
  téléphones » n'est pas utilisable ici, les téléphones étant sur des
  sous‑réseaux distincts sans routeur. C'est le relais IP par les ESP32 (option B
  de la spécification) qui a été retenu, et il est bien **distinct du RS485**.


### 18.14 Dépannage de la dorsale

| Symptôme | Cause probable | Action |
|---|---|---|
| `link.bypass` … `up=0` et le journal répète « upstream unreachable » | l'AP amont n'est pas encore démarré, ou `APP_LINK_UPSTREAM_SSID`/`PASSWORD` est faux | vérifier le SSID exact et le mot de passe ; démarrer le concentrateur **avant** les feuilles |
| « access point … failed to start » ou téléphones déconnectés dès que la liaison monte | **divergence de canal** : la STA s'est associée sur un autre canal que celui de l'AP | aligner `APP_WIFI_CHANNEL` sur les trois stations ; le firmware journalise explicitement l'erreur de canal |
| « rejected node N … invalid token » | `APP_LINK_TOKEN` différent entre les deux stations | rendre les jetons identiques, recompiler et reflasher |
| « no free link slot » | plus de deux connexions entrantes (topologie inattendue) | vérifier `APP_LINK_UPSTREAM_NODE` : une seule feuille par station attendue |
| « node N silent for 15000 ms, closing the link » | liaison Wi‑Fi instable ou station distante redémarrée | vérifier le RSSI (`/api/health` → `link`), la distance, le canal ; la reconnexion est automatique |
| Appel intercellulaire refusé avec « cell-unreachable » | la station du destinataire n'est **pas joignable** sur la dorsale (paire non authentifiée) | consulter `/api/health` → `link.peers` (masque des pairs directs) et `link.upstreamConnected` |
| `link.framesRelayed` qui grimpe anormalement | topologie en chaîne ou boucle de configuration | vérifier `APP_LINK_UPSTREAM_NODE` sur les trois stations ; `link.framesDropped` doit rester faible |
| Appel établi mais **aucun son** | la dorsale transporte la signalisation mais pas l'audio (`link.audioSent`/`audioReceived` à 0) | vérifier que `calls.audioForwarded` augmente côté appelant ; si la dorsale est tombée en cours d'appel, l'appel est terminé au bout du délai de média (20 s) |
| Les LED RS485 clignotent pendant un appel | **anomalie** : l'audio ne doit jamais passer par le bus | vérifier `bus.sent` dans `/api/health` : il doit rester stable pendant un appel ; les tests `testStationCallHandlersNeverSubmitToTheMesh` et `testStationLinkGlueNeverUsesTheMesh` couvrent ce point |

Diagnostic utile :

```powershell
# une seule cellule
.\scripts\station_doctor.ps1 -Cell A

# état de la dorsale et des appels
Invoke-RestMethod http://10.0.0.1:8080/api/health | ConvertTo-Json -Depth 4
Invoke-RestMethod "http://10.0.0.1:8080/api/call/state?userId=u-a1" | ConvertTo-Json
```

Le bloc `link` de `/api/health` expose : `upstream`, `upstreamReady`,
`upstreamConnected`, `peers` (masque des pairs directs), `framesSent`,
`framesReceived`, `framesRelayed`, `framesDropped`, `signalsSent`,
`signalsReceived`, `audioSent`, `audioReceived`, `authRejections`,
`disconnects`. Le bloc `calls` expose en plus `remoteCalls`, `audioForwarded`,
`audioFromLink`.

### 18.15 Essais matériels restant à effectuer

Aucun ESP32, aucun téléphone et aucune liaison Wi‑Fi réelle n'étant accessible
depuis cet environnement, **aucun appel intercellulaire réel n'a été passé**.
Ce qui reste à valider sur banc :

1. **Montée de la dorsale** : au démarrage des trois cartes, `SITE_B` et
   `SITE_C` doivent journaliser « backbone up: attached to node 1 » et
   `/api/health` doit montrer `link.peers = 0x3` sur `SITE_A` et
   `link.upstreamConnected = true` sur B et C.
2. **AP conservés** : les trois SSID doivent rester visibles et les téléphones
   connectés à leur cellule pendant que la liaison inter‑stations est active.
3. **Appel A → A** (même cellule) : doit continuer à fonctionner exactement comme
   avant, sans aucune trame sur la dorsale (`link.signalsSent` stable).
4. **Appel A → B**, **A → C**, **B → A**, **B → C**, **C → A**, **C → B** : sonnerie,
   acceptation, conversation **audible dans les deux sens**, raccrochage.
5. **Refus / occupation / expiration** d'un appel intercellulaire.
6. **Perte de liaison en cours d'appel** : couper la liaison Wi‑Fi de B pendant un
   appel A↔B → les deux côtés doivent terminer proprement l'appel au bout du délai
   de média, et les messages texte/vocaux continuer de fonctionner.
7. **Non‑régression messagerie** : pendant et après un appel intercellulaire,
   vérifier textes et vocaux enregistrés **en local** et **A↔B, A↔C, B↔C**, avec
   accusés « livré » puis « lu ».
8. **Charge** : `calls.audioRelayed`, `link.framesRelayed`, `system.heapMin` et
   `maxLoopUs` sur un appel B ↔ C de 5 min (le concentrateur relaie alors deux
   flux), plus la vérification que l'AP de `SITE_A` reste stable.
9. **Mesures** : latence audio perçue, pertes (`calls.audioSilent`), stabilité du
   RSSI de la liaison, maintien des points d'accès.

Aucun de ces points n'est couvert par un test automatisé : les tests fournis
vérifient la logique de session, le routage, l'authentification, le format des
trames, la reconnexion et l'absence de dépendance au bus — **pas** la qualité
audio ni la stabilité radio sur matériel réel.

---

## 19. Audit des régressions après l'ajout des appels intercellulaires

Cette section documente l'audit mené après la mise en service des appels audio
entre cellules : quelles régressions existaient réellement, quelle en était la
cause **dans le code**, et ce qui a été corrigé. Elle distingue explicitement ce
qui est **prouvé par lecture du code et par test** de ce qui reste **non
vérifiable sans matériel**.

### 19.1 Méthode : ce que le diff Git prouve

Les hunks de `git diff 583366a..HEAD` (premier commit → état des appels
intercellulaires) ont été relevés fonction par fonction. Résultat :

| Fichier | Touché par l'ajout des appels intercellulaires ? |
|---|---|
| `lib/cell_protocol/*` | **non** — identique octet pour octet |
| `lib/cell_core/*` (file, arbitre, table de routage, enveloppe, fragmentation) | **non** — identique octet pour octet |
| `src/common/mesh_node.{h,cpp}` | **non** — identique octet pour octet |
| `src/common/rs485_bus.h` | **oui**, uniquement les tailles de tampon UART (§19.4) |
| `src/common/app_server.cpp` | **oui** : constructeur, `begin()` (nouvelles routes), `loop()` (+1 appel), `handleSocketEvent` (cas `WStype_BIN` + libération d'appel à la déconnexion), `handleInfo`/`handleHealth` (JSON additif), puis le bloc d'appel inséré après `handlePresenceLeave` |
| `handleText`, `handleReceipt`, `handleVoiceBlock`, `handleVoiceFeedback` | **absents de tous les hunks** — chemin vocal enregistré inchangé |

Côté Android, `git diff 71c3997..HEAD -- AppViewModel.kt` ne touche que les zones
d'appel : `ensureConnected`, `detectCell`, `clearStation`, `sendVoiceClip`,
`uploadBlocks`, `handleVoiceBlockData`, `completeVoiceTransfer` et
`onVoiceControl` sont **inchangés**.

**Conclusion de l'audit** : le chemin d'un message vocal enregistré
(téléphone → station → bus RS485 → station → téléphone) est **inchangé de bout en
bout**. Une perte de messages ne peut donc pas venir d'une modification de ce
chemin : elle ne peut venir que de son **environnement d'exécution**, c'est‑à‑dire
du temps de boucle principale, partagé avec la dorsale Wi‑Fi.

### 19.2 Régression 1 — le raccrochage n'était pas propagé (corrigée)

**Symptôme** : un participant raccroche, l'autre reste en appel.

**Cause racine** (lecture de `AppServer::onLinkSignal`) : le gestionnaire
conservait des `const char*` **pointant à l'intérieur de la session**
(`localParticipant(*session)`, `session->caller`, `session->callee`), puis
appelait `calls_.remoteEnd(...)`, qui **libère la session** et remet ces champs à
`'\0'`. `notifyCallEnded(local, callId, peer, ...)` recevait donc un identifiant
vide et sortait immédiatement (`if (userId[0] == '\0') return;`) : l'événement
`call_ended` / `call_rejected` / `call_cancelled` n'était **jamais** poussé au
participant local. Les quatre actions terminales (`kSignalReject`,
`kSignalCancel`, `kSignalHangup`, `kSignalEnded`) étaient concernées, dans **les
deux sens**.

**Correction** : `CallTable::remoteEnd(callId, nowMs, CallSession& endedOut)`
renvoie désormais une **copie** de la session libérée, prise **avant** la
libération — le même schéma que `CallTable::endForUser(..., CallSession&
endedOut)` qui existait déjà. Le gestionnaire ne travaille plus que sur cette
copie : le bug est devenu **structurellement impossible**, pas simplement
corrigé.

**Preuve** : `testRemoteEndReleasesTheSession` vérifie que la copie rendue contient
encore `callId`, `caller`, `callee` et `remoteNode` alors que `table.find()`
renvoie `NULL`, et qu'un second appel sur une session déjà libérée rend une copie
vide sans erreur (terminaison idempotente).

### 19.3 Régression 2 — le nom d'affichage était remplacé par l'identifiant (corrigée)

**Symptôme** : après un changement de cellule, l'utilisateur apparaît comme
`u-3f2a1b9c0d` sur les autres cellules — comme s'il s'agissait d'une autre
personne.

**Ce qui n'était PAS la cause** (vérifié) :

* `UserProfileStore.save()` réutilise l'identifiant existant
  (`val id = existingId ?: generateUserId()`) : **aucun nouvel utilisateur n'est
  créé** quand le téléphone change de point d'accès ;
* `RoutingTable::upsert` est indexé par `userId` (`findMutable` puis `allocate`
  seulement si absent) : **aucun doublon n'est possible** pour un même
  identifiant ;
* le changement de cellule met bien à jour la localisation : l'entrée est
  **déplacée** (`entry->station = station`) et l'ancienne cellule cesse de
  l'annoncer, puisque son annonce ne liste que les entrées dont
  `station == ownStation`.

**Cause racine** : `AppServer::bindSocketToUser` faisait

```cpp
if (!mesh_.refreshLocalUser(id)) {
  mesh_.registerLocalUser(id, id);   // le nom devient l'identifiant
}
```

Sur la cellule d'arrivée, l'utilisateur est inconnu : il était donc enregistré
avec **son identifiant comme nom d'affichage**, et cette paire `(id, id)` était
ensuite propagée par l'annonce de présence à toutes les autres cellules. Selon
l'ordre d'arrivée entre l'enregistrement HTTP de présence et le `hello`
WebSocket, le nom réel était restauré plus ou moins vite (au plus tard au
rafraîchissement de présence suivant, 30 s) — d'où un affichage incohérent et
intermittent.

**Correction** : le `hello` WebSocket transporte désormais le **nom d'affichage**
en plus de l'identifiant stable (`HelloMessage.build`), et
`bindSocketToUser(clientNumber, userId, displayName)` l'utilise. Le repli sur
l'identifiant ne subsiste que pour un client plus ancien qui n'enverrait pas de
nom.

**Preuve** : `HelloMessageTest` (6 tests JVM) fixe le contrat exact du message,
y compris l'échappement JSON d'un nom contenant guillemets, antislash, retour à
la ligne et caractère de contrôle ; `test_mobility.cpp` (8 tests natifs) vérifie
que l'identité, le nom et la localisation survivent à A→B→C→A, aux annonces
répétées, aux annonces reçues dans le désordre, à la déconnexion et au retour, et
que deux personnes portant le **même nom** restent deux utilisateurs distincts.

### 19.4 Régression 3 — la dorsale bloquait la boucle principale (corrigée)

**Symptôme** : messages texte et vocaux perdus de façon **asymétrique** entre
cellules (certaines paires fonctionnent, d'autres non, une réponse n'arrive pas),
apparu avec les appels intercellulaires.

**Cause racine** : le chemin des messages enregistrés est inchangé (§19.1), mais
son **environnement** ne l'était plus. Deux appels bloquants avaient été
introduits dans la boucle principale :

1. `WiFiClient::connect(ip, port, 400)` est **synchrone** : la source du framework
   montre un `select()` de toute la durée du `timeout_ms`. Il était retenté
   toutes les 2 s tant que la liaison n'était pas établie.
2. `WiFiClient::write()` boucle **10 fois avec un `select()` d'une seconde**
   (`WIFI_CLIENT_MAX_WRITE_RETRY = 10`, `WIFI_CLIENT_SELECT_TIMEOUT_US =
   1000000`). Il était appelé à chaque trame de la dorsale, soit jusqu'à 50 fois
   par seconde pendant un appel.

Pendant un tel blocage, `gMesh.loop()` n'est pas appelé : le tampon RX du bus
RS485 (1024 octets ≈ **89 ms** à 115 200 bauds) **déborde**, et les trames
perdues sont exactement les messages texte, les blocs vocaux et leurs accusés.
Un blocage de plusieurs secondes explique aussi l'asymétrie observée : la cellule
qui relaie (le concentrateur) bloque plus longtemps que les feuilles.

**Corrections** :

* `lib/cell_link` gagne `LinkTxBuffer` : une file **bornée, orientée trames
  entières**, vidée par **un seul `send(MSG_DONTWAIT)`** par anneau et par
  boucle. L'ordre des octets est préservé et une trame partiellement écrite n'est
  jamais jetée, donc le flux vu par le pair reste valide même quand la socket
  n'accepte qu'une partie de la trame ;
* **deux anneaux par socket** — contrôle et audio — comme les anneaux
  « contrôle » et « applicatif » du bus : une rafale d'audio d'appel ne peut
  jamais chasser un signal d'appel ;
* `station_link.cpp` n'appelle plus **jamais** `WiFiClient::write()` ni
  `readBytes()` : une assertion de test échoue si l'un des deux revient ;
* le délai de connexion passe de 400 ms à **200 ms**, avec un **backoff
  progressif** (2 s → 4 s → … → 30 s) remis à zéro dès que la liaison montante
  répond, pour ne plus marteler la radio ;
* le tampon RX du bus RS485 passe de 1024 à **4096 octets** (≈ **355 ms** de
  trafic), au‑dessus du pire blocage synchrone restant (200 ms). Le bus et son
  protocole sont inchangés : seule la marge du tampon augmente ;
* `LinkFrame` (≈ 720 octets, initialisé) n'est plus construit **à chaque octet
  reçu** mais une seule fois par socket et par boucle ;
* deux compteurs de diagnostic sont exposés dans `/api/health` et `/api/info` :
  `link.txPending` (octets encore en attente d'écriture) et `link.maxLoopUs`
  (pire temps passé dans le service de la dorsale depuis le démarrage). Un
  `txPending` qui grimpe signale une dorsale saturée ; un `maxLoopUs` élevé
  signale que le tampon du bus a été mis en danger.

**Preuves** :

* `testLinkTxBufferKeepsTheFrameOrder`,
  `…RetriesTheRestOfAFrameWhenTheSocketIsBusy` (écriture partielle : rien n'est
  perdu ni dupliqué, l'ordre est conservé), `…IsBoundedAndCountsRejections`,
  `…ReclaimsTheBytesAlreadyWritten`, `…IgnoresDegenerateInput`,
  `…SeparatesControlFromAudio` ;
* `testStationLinkNeverBlocksTheMainLoop` : `client.write(` **interdit** dans
  `station_link.cpp`, `MSG_DONTWAIT` et le backoff **exigés**, et `LinkFrame`
  doit être déclaré **hors** de la boucle de lecture ;
* `testStationLinkProtectsTheRs485ReceiveBuffer` : 4096 octets à 115 200 bauds
  = 355 ms, vérifié par calcul, et supérieur au délai de connexion synchrone.

### 19.5 Régression 4 — en‑tête de conversation encombré (corrigée)

**Ce qui avait changé** : le sous‑titre de la barre supérieure affichait
« Vous êtes sur SITE_A · appel vers SITE_B » pour un correspondant d'une autre
cellule, et le libellé d'accessibilité du bouton d'appel était resté
« Appeler (même cellule) » alors que l'appel intercellulaire était devenu
possible.

**Correction** : le sous‑titre est **restauré à l'identique** de la version
d'origine, soit `"Vous êtes sur <site>"` ou `"Hors cellule"` sur une seule ligne
concise, et le libellé du bouton redevient `"Appeler"`. La cellule du
correspondant n'est plus affichée dans l'en‑tête : c'est une information de
diagnostic, elle reste sur l'écran d'appel (qui indique déjà « Appel vers
SITE_B ») et dans `/api/health`. La flèche de retour, le nom de la conversation
et l'action d'appel restent à leur place d'origine.

**Preuve** : le sous‑titre est comparé caractère par caractère à celui de
`git show 583366a:mobile/…/Screens.kt`, l'import devenu inutile est retiré,
`BUILD SUCCESSFUL` et 77/77 tests JVM.

### 19.6 Ce qui n'a **pas** été trouvé (et pourquoi)

| Point signalé | Constat de l'audit |
|---|---|
| Création d'un **nouvel utilisateur** au changement de cellule | **Inexistant** dans le code : l'identifiant est stable (`UserProfileStore`) et `upsert` est indexé par identifiant. La seule anomalie réelle concernait le **nom** (§19.3), corrigée. |
| **Doublons** d'utilisateurs | **Impossibles par construction** pour un même identifiant. Deux personnes de même nom restent deux entrées : c'est le comportement attendu et il est testé. |
| Fusion de deux comptes sur la base d'un nom identique | **Aucune fusion n'existe** dans le code, et aucune n'a été ajoutée. |
| Suppression de l'identité, de l'historique ou des conversations lors d'un déplacement | **Inexistante** : une déconnexion marque l'entrée *hors ligne* (`online = false`) sans la supprimer, et une station injoignable marque ses utilisateurs hors ligne sans effacer leurs entrées. Testé. |
| Messages vocaux enregistrés cassés par une modification de leur chemin | **Aucune modification** de ce chemin (§19.1) : la cause était l'environnement d'exécution (§19.4). |
| **Annonces vocales** (diffusion à plusieurs cellules) | **Cette fonctionnalité n'existe pas dans ce dépôt** : aucune route, aucun type de message, aucune entité, aucun écran, aucune trace dans le code ni dans l'historique Git. Il n'y a donc rien à auditer ni à restaurer, et rien n'a été inventé pour « combler » ce point. Le prototype fournit trois fonctions vocales distinctes : les **messages vocaux enregistrés** (§9), les **appels audio locaux** (§17) et les **appels audio intercellulaires** (§18). |
| **Salons de discussion** (conversations de groupe) | **Cette fonctionnalité n'existe pas** : l'application gère des **conversations 1‑à‑1** (`ConversationEntity` par correspondant). L'« en‑tête des salons de discussion » a donc été interprété comme la **barre supérieure de l'écran de conversation**, seule zone concernée par la régression (§19.5). |
| Adresses IP ou messages de débogage dans l'en‑tête de conversation | **Aucun** : les adresses des stations sont affichées dans l'onglet **« Cellule »** (écran de diagnostic) et dans `/api/health`, comme avant l'ajout des appels. L'en‑tête ne contenait que la cellule courante, désormais restaurée. |

### 19.7 Limite connue documentée, non corrigée

Le tableau de routage suit la règle « la dernière annonce gagne ». Si l'annonce
d'une cellule **quittée par un utilisateur** est encore en vol quand l'annonce de
la cellule d'arrivée est traitée, l'entrée peut revenir temporairement sur
l'ancienne cellule. La convergence est immédiate : enregistrer ou perdre un
utilisateur local marque la présence comme « à publier », donc la cellule
d'accueil réémet son annonce sans attendre les 10 s du cycle périodique, et il
n'y a jamais qu'**une seule** entrée pour un identifiant donné.

Ce comportement est **documenté et testé tel quel**
(`testMobilityConvergesAfterAStaleAnnouncement`) plutôt que masqué. Le supprimer
demanderait un **numéro de version monotone** dans le message de présence,
c'est‑à‑dire une modification de format sur le bus RS485 ; elle n'a pas été faite,
faute de nécessité démontrée et parce que la consigne est de ne pas modifier le
protocole sans justification.

### 19.8 Tests ajoutés par cet audit

| Fichier | Contenu |
|---|---|
| `test/test_mobility.cpp` *(nouveau)* | 8 tests d'identité et de mobilité : A→B, A→B→C→A, annonces répétées, annonces désordonnées, même nom pour deux personnes, départ = hors ligne (jamais supprimé), station injoignable, identifiant qui ne devient jamais le nom |
| `test/test_messaging.cpp` *(nouveau)* | 5 tests du chemin complet d'un message, **dans les six directions** : vocal enregistré, texte + accusé aller‑retour, routage vers la **nouvelle** cellule après un déplacement, refus explicite d'un destinataire inconnu ou hors ligne, charge utile fragmentée puis réassemblée octet pour octet |
| `test/test_link.cpp` | 8 tests ajoutés : file d'émission non bloquante (ordre, écriture partielle, bornage, récupération d'espace, entrées dégénérées, séparation contrôle/audio) et 2 gardes structurelles (aucun `client.write`, marge du tampon RS485) |
| `test/test_call.cpp` | `testRemoteEndReleasesTheSession` étendu : la copie rendue à l'appelant doit encore décrire l'appel libéré |
| `mobile/…/test/…/HelloMessageTest.kt` *(nouveau)* | 6 tests JVM du message `hello` : identifiant + nom, repli sur l'identifiant si le nom est vide, échappement JSON, une seule ligne, taille bornée |

### 19.9 Résultats réellement observés

| Vérification | Commande | Résultat |
|---|---|---|
| Tests natifs | `pio test -e native` | **124/124 réussis**, 0 échec |
| Tests JVM Android | `gradle -p mobile testDebugUnitTest` | **77/77 réussis**, 0 échec |
| Firmware `SITE_A` | `pio run -e site_a` | **SUCCESS** — RAM 27,1 %, Flash 65,2 % |
| Firmware `SITE_B` | `pio run -e site_b` | **SUCCESS** — RAM 27,1 %, Flash 65,2 % |
| Firmware `SITE_C` | `pio run -e site_c` | **SUCCESS** — RAM 27,1 %, Flash 65,2 % |
| APK Android | `gradle -p mobile assembleDebug assembleRelease` | **BUILD SUCCESSFUL** — `app-debug.apk` 15,91 Mo, `app-release.apk` 10,31 Mo |

La RAM statique passe de 24,9 % à 27,1 % : les files d'émission de la dorsale
(3 sockets × 2 anneaux) et la marge supplémentaire du tampon RX du bus.

### 19.10 Ce que cet audit ne prouve toujours pas

Aucune carte ESP32, aucun téléphone, aucun bus RS485 et aucun réseau Wi‑Fi réel
n'étaient accessibles depuis cet environnement. En conséquence :

* **aucun appel réel n'a été passé**, ni local ni intercellulaire, et le
  raccrochage corrigé (§19.2) n'a **pas** été observé sur deux téléphones ;
* la disparition effective des pertes de messages (§19.4) n'a **pas** été mesurée
  sur trois cartes : la correction supprime les blocages identifiés par lecture
  du framework, elle ne les remplace pas par une mesure de terrain ;
* la qualité audio, la latence perçue, le taux de perte, l'écho et la stabilité
  radio restent **non vérifiés** ;
* le protocole d'essai complet des §17.14 et §18.15 reste **à exécuter sur
  matériel**.

