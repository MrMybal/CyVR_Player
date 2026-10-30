# Current source/track support — 0.4.5

See the [README container/audio/subtitle section](../README.md#containers-audio-and-subtitles) for the current profile. Seekable-source video/audio/subtitle track changes, chapters, external subtitles, AAudio output and software video fallback are implemented. Software decode still depends on the compiled codec and available mobile CPU. Android does not currently bundle libxml2/DASH, libSRT, SMB/NFS or DRM. A desktop integration result must not be treated as Android protocol coverage.

The notes below document earlier design and validation stages.

# Fichiers, explorateur interne et flux

Ces modules restent utilisables sans interface. L'application CyVR_UI les appelle maintenant depuis des workers de stockage/média ; le thread OpenXR consomme les images prêtes. Voir [interface.md](interface.md).

## Explorateur

`FileBrowser` prend un dossier racine, renvoie des entrées structurées et produit une `MediaSource` pour un fichier sélectionné. Il expose :

- Navigation par chemins relatifs, avec résolution canonique et rejet des chemins qui sortent de la racine.
- Dossiers en premier, tri alphabétique sans distinction de casse ASCII, recherche sur les noms.
- Catégories vidéo, audio, splats et playlists à partir des extensions ; ce sont des indications, pas une validation du contenu.
- Métadonnées de taille lorsque disponibles, sans ouvrir un décodeur ni générer de miniature.
- Limites de scan et de résultats, indicateurs `truncated`/`cancelled`, annulation coopérative.

Les résultats forment un instantané borné, pas un index récursif ou une pagination exhaustive. Quand il est tronqué, affiner la recherche ou changer de dossier. Les racines autorisées sont fournies par l'application. La protection de navigation n'est pas un mécanisme OS de confinement face à des liens modifiés simultanément.

Exemple C++ :

```cpp
cyvr::player::FileBrowser browser("/storage/emulated/0/Movies");
cyvr::player::BrowseOptions filter;
filter.search = "concert";
auto files = browser.list({}, filter);
// La racine doit déjà être accessible au processus Android.
auto source = browser.open_file(files.entries.at(0).relative_path);
```

`DocumentBrowser.java` expose le contenu d'un arbre SAF autorisé et conserve les URI du provider. `PlayerActivity`, le bridge JNI et l'explorateur interne sont raccordés : sélection d'arbre, grant persistant, listes paginées et ouverture par FD/offset/longueur. Aucun chemin n'est deviné à partir d'une URI. Les sous-plages et la fermeture du FD original sont testées sur Quest 3 ; le parcours complet avec un provider réel reste à vérifier manuellement. [Stockage documentaire Android](https://developer.android.com/training/data-storage/shared/documents-files).

## Sources communes et pistes

```cpp
using namespace cyvr::player;
auto cancel = std::make_shared<Cancellation>();
Demuxer media;
media.open(MediaSource::network("https://serveur.example/film.m3u8"), {}, cancel);
for (const auto& track : media.tracks()) {
    // index, type, codec, dimensions, langue, titre, cadence audio, canaux...
}
auto packet = media.read();
// cancel->cancel() depuis un autre thread interrompt les I/O coopératives.
```

`read()` distingue paquet, nouvelle tentative et fin de média. Le client garde les paquets qui attendent un tampon d'entrée du décodeur. Le choix des meilleures pistes audio/vidéo est disponible, tout comme les paramètres codec et la conversion des timestamps vers une origine commune.

`seek()` utilise la recherche temporelle FFmpeg, y compris les segments HLS/DASH VOD. Le résultat peut commencer sur une image clé antérieure à la cible : le lecteur doit décoder le préroll, vider les décodeurs lors du seek et ne présenter que les images dues. La génération de la file de présentation doit changer en même temps. Les flux live non navigables ne deviennent pas seekables du seul fait qu'ils utilisent HLS.

## État des protocoles

| Entrée | Windows | Android / Quest 3 |
| --- | --- | --- |
| MP4/MKV, pistes audio/vidéo, EOF, seek local | Testés | Testés |
| Descripteur, offset et longueur | Testés | Testés sur fichier réel ; provider SAF à valider |
| HTTP progressif | Testé | Testé |
| HTTPS avec vérification de certificat | Certificat non fiable rejeté | Certificat non fiable rejeté et lecture réussie avec CA de test explicite |
| HLS VOD avec segments MPEG-TS et seek | Testé | Testé |
| DASH VOD avec seek | Testé | Non compilé dans le profil actuel ; libxml2 à intégrer |
| RTSP/RTSPS, RTP, UDP | Protocoles disponibles, sessions réelles à tester | Protocoles de base compilés, sessions réelles à tester |
| SRT | Présent dans le SDK de développement ; pas de session testée | Bibliothèque/protocole non compilés |
| Live durable, changement de qualité adaptatif, reprise après coupure | À valider et compléter | À valider et compléter |

HLS/DASH sont détectés par le démuxeur, indépendamment du nom de fichier. Un protocole connu de `MediaSource` peut être absent du binaire FFmpeg choisi ; dans ce cas l'ouverture renvoie une erreur. Les playlists M3U/PLS génériques et les listes de bibliothèques ne sont pas encore interprétées par un gestionnaire de playlist.

## Délais, mémoire et TLS

Les options fixent les délais d'ouverture/lecture, le volume et la durée de sonde. La sonde est limitée à 4 Mio/3 secondes et à 64 pistes par défaut. L'interruption est coopérative via le callback FFmpeg et, pour les pipes Android, une attente `poll` courte. Certains appels système, notamment DNS, peuvent avoir leurs propres délais : ce mécanisme ne remplace pas l'exécution sur un worker.

Le mode faible latence reste optionnel. La reconnexion automatique et les politiques de retour au direct après interruption ne sont pas encore implémentées. Une erreur de réseau remonte au propriétaire de la session, qui peut rouvrir la source avec une nouvelle génération. Il n'y a pas de cache disque illimité.

Les sources réseau utilisent une liste de protocoles autorisés qui exclut les fichiers et les descripteurs locaux, y compris pour les ressources référencées par un manifeste. Les tests vérifient qu'un HLS reçu par HTTP ne peut pas charger un segment via `file://`.

La vérification TLS est activée. Sur Android, FFmpeg utilise Mbed TLS 3.6.7 ; `SystemCertificates.exportPem` exporte les autorités de confiance de la plateforme dans le stockage privé de l'application, à passer dans `StreamOptions::ca_file`. L'export est à effectuer au démarrage sur un worker pour refléter les changements de certificats. Il n'ajoute aucune autorité au système. Le binaire de test Android a vérifié aussi bien le rejet d'une CA inconnue que le succès avec une CA explicitement fournie.

L'outil de test et l'application désactivent les logs FFmpeg pouvant contenir des secrets d'URL. L'APK déclare la permission `INTERNET` et exporte les CA système Android pour Mbed TLS. Les tests sans APK restent disponibles ; `quest_app.py` exerce maintenant aussi la présentation via l'application réelle.

## Chemin matériel

`VideoBitstream` prépare AVC et HEVC avec les filtres officiels `h264_mp4toannexb` et `hevc_mp4toannexb`. Il fournit les parameter sets Android CSD et conserve les paquets jusqu'à leur consommation. `make_video_format` crée le format natif et `SurfaceDecoder` reçoit les unités d'accès, puis expose des `ImageLease` contenant l'AHardwareBuffer et son fence.

Le test matériel utilise `CodecProbe.hardwareCandidates`, puis les composants Qualcomm du Quest 3. Les tests GPU ajoutés ont importé et échantillonné les 48 images AVC et les 48 images HEVC, avec une capture finale de diagnostic inspectée. L'APK a également soumis ces images à ses swapchains OpenXR. Cela ne remplace pas le contrôle visuel avec casque porté, la qualification colorimétrique ou les essais 8K.

L'horloge de production doit choisir un décalage commun qui préserve les PTS négatifs de préroll et l'alignement A/V. Le test matériel utilise un décalage d'une seconde pour ses médias synthétiques ; ce choix n'est pas une politique générale de lecture. Changements de SPS/PPS, de résolution, HDR et adaptation multivue exigent encore une gestion dédiée.

## Dépendances et reproductibilité

Les scripts figent FFmpeg sur `611cec792aa54e2e53dbc1be95dcf5955d9a99ba` et Mbed TLS sur 3.6.7. Le build Android active les démuxeurs/codecs explicitement listés dans `tools/build_ffmpeg_android.py`, sans `--enable-gpl` ni `--enable-nonfree`, avec `--enable-version3` : les bibliothèques FFmpeg obtenues sont LGPLv3+. Mbed TLS est utilisé sous son option Apache-2.0. Les archives et leurs SHA-256 figurent dans `dependencies.lock.json`.

Le SDK Windows BtbN est utilisé pour développer et générer les médias de test. Il embarque davantage de fonctions que le profil Android ; sa présence ne démontre pas la disponibilité des mêmes codecs/protocoles dans le casque. GNU make est un outil de compilation local, sans lien avec le binaire du lecteur. Les licences et les sources correspondant aux bibliothèques effectivement distribuées devront accompagner une future livraison de l'application.
