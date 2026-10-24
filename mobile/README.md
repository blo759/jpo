# Application Android CellNet

Application Android du prototype : elle se connecte au point d'accès Wi‑Fi d'une
cellule (`CELL_SITE_A`, `CELL_SITE_B`, `CELL_SITE_C`) et dialogue avec l'ESP32 de
cette cellule en HTTP (port 8080) et WebSocket (port 8081).

## Fonctionnalités

* création d'une identité locale au premier démarrage (identifiant stable +
  nom d'affichage), restaurée aux lancements suivants ;
* détection de la cellule réellement connectée (SSID), avec parcours guidé si
  Android refuse le changement de réseau ;
* liste des conversations, répertoire de tous les utilisateurs découverts sur
  les trois cellules, recherche par nom ou identifiant ;
* messagerie texte persistance locale (Room) avec états en attente / transmis /
  livré / lu ;
* messages vocaux AAC‑LC 16 kHz mono, **60 s maximum**, transmis par blocs de
  400 octets avec CRC32 global et retransmission sélective des blocs manquants ;
* indicateurs de progression d'envoi et de réception, gestion des pertes de
  connexion et reconnexion automatique.

## Structure

```
mobile/
├── settings.gradle, build.gradle, gradle.properties
└── app/
    ├── build.gradle
    └── src/main/
        ├── AndroidManifest.xml
        ├── res/…                      (thème, icône, config réseau)
        └── java/com/jpo/cellnet/
            ├── MainActivity.kt        point d'entrée Compose
            ├── CellNetApp.kt
            ├── data/Models.kt         entités Room + DAO + base
            ├── data/UserProfileStore.kt
            ├── net/CellApi.kt         HTTP + cellules connues
            ├── net/CellSocket.kt      WebSocket temps réel
            ├── audio/VoiceProtocol.kt constantes et CRC32
            ├── audio/VoiceRecorder.kt enregistrement AAC
            ├── audio/VoiceAssembler.kt réassemblage des blocs
            ├── audio/VoicePlayer.kt   lecture
            ├── wifi/CellWifi.kt       SSID, scan, changement de réseau
            └── ui/                    ViewModel, écrans Compose, thème
```

## Compilation

Avec JDK 17 et Gradle 8.7 :

```powershell
cd mobile
gradle assembleDebug
gradle assembleRelease
```

APK produits :

* `app/build/outputs/apk/debug/app-debug.apk`
* `app/build/outputs/apk/release/app-release.apk`

La variante `release` est signée avec la clé de debug : installable pour les
essais, **non publiable**. Le workflow `.github/workflows/android-build.yml`
produit les deux APK dans l'artefact `cellnet-apk`.

Android Studio : ouvrir le dossier `mobile/`, laisser Studio générer le wrapper
Gradle 8.7 et synchroniser (SDK 34 requis).

## Configuration des cellules

Les SSID, adresses, ports et mots de passe sont centralisés dans
`net/CellApi.kt`, objet `Cells` :

```kotlin
Cell("CELL_SITE_A", "SITE_A", "10.0.0.1", "cellsitea")
Cell("CELL_SITE_B", "SITE_B", "10.1.0.1", "cellsiteb")
Cell("CELL_SITE_C", "SITE_C", "10.2.0.1", "cellsitec")
```

Ils doivent correspondre aux `build_flags` de `platformio.ini`.

## Permissions

| Permission | Usage |
|---|---|
| `INTERNET`, `ACCESS_NETWORK_STATE` | joindre la station locale |
| `ACCESS_WIFI_STATE`, `CHANGE_WIFI_STATE` | lire le SSID, demander un changement |
| `ACCESS_FINE_LOCATION` (≤ 32) / `NEARBY_WIFI_DEVICES` (33+) | lire le SSID |
| `RECORD_AUDIO` | messages vocaux |

## Limites

* Android ne permet pas de changer de réseau Wi‑Fi sans confirmation du système.
* Aucun message n'est conservé côté ESP32 : un destinataire hors ligne ne reçoit
  rien tant qu'il n'est pas reconnecté et que l'expéditeur n'a pas renvoyé le
  message.
* Aucune authentification : l'identifiant est déclaré par l'application.
* HTTP/WebSocket en clair sur le réseau local (prototype).
