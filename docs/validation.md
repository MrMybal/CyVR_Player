# Validation et performances

## 0.4.4 — vidéo indépendante du panneau immersif (29 octobre 2026)

- Le rendu vidéo utilisait la pose du panneau de commandes à chaque image. La réorientation du panneau au relâchement d'un déplacement faisait donc tourner/déplacer le cinéma ou le panorama.
- La vidéo possède maintenant un placement de scène indépendant, initialisé devant la vue courante. Couches natives et reconstruction CAS/FSR/GSR utilisent ce même placement. Déplacer/relâcher le panneau, masquer/rappeler les commandes ou recréer les ressources de filtre conserve l'ancrage vidéo.
- Le bouton « Recentrer la vidéo » repositionne uniquement la vidéo devant la vue courante. Les commandes restent à leur emplacement. Zoom et position vidéo restent disponibles. Le placement est conservé pendant l'hôte immersif courant, sans nouveau format de données ni service d'ancrage persistant.
- Le socle local transmet les changements d'origine OpenXR au lecteur. Une transformation connue est appliquée à son instant d'effet ; le passage de VIEW de secours à LOCAL transforme la pose existante. Une origine inconnue et le placement physique avec le casque porté restent à qualifier sur le runtime réel.
- Six suites CTest réussies, dont 133 contrôles de placement (déplacement continu, orientation au relâchement, zoom/pan, changement d'origine, VIEW→LOCAL, recentrage et commande UI indépendante) et les 134 cas GPU existants. Versions Windows, Android hybride et Android strictement 2D compilées : `0.4.4-placement-dev`, code 9.
- Les APK conservent le paquet et la clé de développement. Signatures, activités, bibliothèques ARM64, alignements 16 Kio, notices et shaders contrôlés. Résultats détaillés : `build/placement-validation.json`.
- Aucun test matériel ni installation sur Quest : poursuite sans casque selon la demande précédente. Restent à valider dans le casque : déplacement avec manettes/mains pendant la lecture ou la pause, maintien des vidéos mono/SBS/TB/180°/360° avec tous les filtres, masquage/rappel et comportement après un véritable changement de repère runtime.


## 0.4.3 — qualité du rendu avec upscalers (29 octobre 2026)

- Deux défauts reproduits avant correction : FSR créait un liseré au bord transparent de la projection cinéma (écart maximal de 7 niveaux RGB) ; le calcul d'entrée réduisait inutilement le deuxième axe. Une mire 128×128 était réduite à 128×64 avant agrandissement horizontal et perdait toutes ses lignes alternées (contraste 0).
- L'entrée conserve désormais le détail disponible de chaque axe séparément, avec les mêmes dimensions de sortie. La géométrie reste définie par les UV/FOV. La mire conserve ses 128 lignes ; contraste mesuré après correction : 254 avec FSR, 255 avec GSR. Le shader étend la couleur du bord sous l'alpha nul pour éviter les mélanges avec du noir pendant la reconstruction.
- Nouveau test GPU avec les shaders réels de projection et le même bloc de constantes C++ que sur Android : sélection des yeux SBS/TB, fenêtre mono-œil, cinéma, 180°/360° avant/arrière, orientation, couverture alpha, transfert SDR, contours filtrés et détails sur un seul axe. 134 cas GPU et 65 contrôles du cœur passent ; les cinq suites CTest passent. Une exécution intermédiaire a échoué au nettoyage temporaire Windows ; l'exécution finale est réussie.
- Versions Windows, APK hybride et APK 2D seul compilées : `0.4.3-render-dev`, code 8. Signatures, activités, ARM64, alignements 16 Kio, notices et 30 sorties SPIR-V contrôlés. Dépendances et traductions inchangées et vérifiées. Vidéo seule/rappel d'interface vérifiés hors écran à 1920×1080 et 1600×1100.
- Résultats : `build/render-quality-validation.json` et `build/render-quality-gpu-pc.jsonl`. Les timings du PC ne qualifient pas le Quest. Aucun test matériel ni installation sur casque, conformément à la demande de poursuivre sans casque. L'import AHB, le compositeur OpenXR et la correspondance avec le symptôme visuel précis rapporté par l'utilisateur restent à confirmer.


## 0.4.2 — cadence mesurée et traitement regroupé (29 octobre 2026)

- Livrables : version `0.4.2-perf-dev`, code Android 7 ; aperçu Windows, APK hybride et APK strictement 2D compilés. Même paquet et clé de développement pour préserver les données lors d'une mise à jour.
- Cinq suites CTest réussies ; 63 contrôles du cœur incluant 24 images vidéo/s indépendantes de 72 réaffichages/s, arrêt du compteur en cas de blocage, générations de seek, percentiles conservant les longs délais et cibles de reconstruction dépassant l'ancien plafond fixe. Les huit boutons de qualité/résolution sont présents et actifs.
- 33 cas GPU hors écran passent sur le PC. CAS, FSR et GSR donnent des pixels identiques avec passes séparées ou regroupées ; les requêtes de timestamps Vulkan retournent des valeurs mesurées sur ce GPU. Le benchmark PC distingue temps GPU et temps avec soumission/attente CPU. Résultats : `build/performance-gpu-pc.jsonl`. Aucun chiffre PC ne prouve une cadence Quest.
- Compteur du lecteur : cadence nominale du fichier, images vidéo distinctes traitées par le GPU, sortie décodeur, soumissions de l'application, fréquence runtime, retards, file d'attente, erreurs et temps GPU moyen/p95. Les réaffichages VR ne gonflent pas la cadence vidéo ; les données absentes restent « Non mesuré ». Le compteur ne mesure pas les images réellement affichées par le compositeur Quest.
- Conversion/projection et filtre sont soumis dans une seule commande avec une seule attente de fin par œil. Les ressources restent stables pendant le mouvement continu du zoom ; l'estimation de détail est réappliquée après 250 ms de stabilité. Correction de l'inversion des yeux sur une image 2D filtrée en pause. Les sauvegardes de position seules sont espacées de cinq secondes ; les transitions d'activité conservent leur sauvegarde.
- Rendus Vulkan du HUD anglais/français inspectés ; vidéo seule et rappel de l'interface vérifiés hors écran à 1920×1080 et 1600×1100. Le média SBS synthétique 1920×960 annonce bien 24 images/s dans l'inspecteur FFmpeg.
- Signature des deux APK vérifiée ; ARM64, alignements ELF/ZIP à 16 Kio, activités et notices contrôlés ; 30 fichiers SPIR-V validés sur les trois builds. Huit fichiers tiers et quatre archives de dépendances vérifiés. Détail : `build/performance-validation.json`.
- **Aucune installation ni exécution de cette version sur Quest** : l'utilisateur a demandé de continuer sans casque. Restent à vérifier sur le matériel : compteurs sous MediaCodec/OpenXR, rendu des vues par œil et coutures, zoom continu, reprise/redimensionnement/transitions, précision colorimétrique, débit et tenue thermique sur des médias représentatifs. Les essais 0.4.1 de reconstruction de vue étaient partiels ; ils ne qualifient pas ce build.


## 0.4.0 image controls, previews and GSR (29 October 2026)

- Windows and both Android variants compile; five CTest suites pass, including 30 offscreen GPU cases in the final specialized pipelines and migration of old state. English/French screens were rendered in Vulkan and visually inspected, including real thumbnail pixels.
- Dependency checks cover eight pinned source/license files, including byte-identical BSD-3-Clause GSR 1 blobs. Both APKs contain the active filter shader and notices; ARM64, 16 KiB ELF/ZIP alignment and signatures are checked.
- Quest 3: the CAS/FSR version of the offscreen shader passed 21 pixel checks. Before the subsequent pipeline-specialization change, actual APK input passed eight cases: paused CAS, paused FSR 1.5x, paused GSR 1.5x, exposure/zoom sliders, hidden-interface wake, keyboard seek, processed 2D/immersive round trips and thumbnail upload. See `build/quest-image-results.json`.
- Native HTTP AVC, HTTP HEVC and HLS AVC each decoded/presented 48 frames with zero drops on the small 320×192 fixtures. These ran before adding GSR; the native path remains separate.
- The extended Quest GPU playback run passed native/control cases for AVC and HEVC, plus AVC CAS/FSR/GSR (47/47/46 frames presented out of the 48-frame fixture, including startup). The device disconnected during HEVC CAS. The final specialized pipelines, the rest of HEVC/filter runs and high-resolution filter timings have not yet been rerun on Quest. Partial evidence is recorded separately in `build/quest-image-gpu-partial-results.json`; older GPU results are historical.
- Physical grip+trigger motion, binocular quality and sustained high-resolution/thermal behavior remain unverified. The gesture callback and consumption are exercised in application tests; ADB cannot validate physical hand movement.

The following sections retain earlier milestone results. Their old statements about disconnected shader dispatch describe those earlier builds; CAS, FSR 1 and GSR 1 are now connected. See [current processing details](image-processing.md).


## Barre de navigation — version 0.3.2

- Compilation Windows et Android hybride/strictement 2D. Barre commune CyVR_UI : position/durée, sélection pendant le glissement et un seul seek au relâchement ; désactivation quand la source n'est pas navigable ou sa durée est inconnue.
- Quatre suites CTest réussies. La navigation utilise un média GIF original de deux secondes produit par le test, sans dépendance à des fichiers téléchargés. Cas vérifiés : absence de seek pendant le glissement, validation à 75 %, maintien de la pause/lecture, annulation, bornes début/fin en dehors de la barre et absence de seek lors du rappel de l'interface masquée.
- Page Lecture rendue en Vulkan et inspectée en français. Le test de rendu vidéo seul reste réussi aux deux résolutions 1920 × 1080 et 1600 × 1100.
- Quest toujours absent d'ADB : glissement avec les vraies manettes, seek MediaCodec au début/à la fin et comportement des flux réseau restent à vérifier sur cette version. Le chemin natif conserve un dernier frame de prélecture si la cible de seek dépasse le PTS de la dernière image.

## Mode vidéo sans interface — version 0.3.1

- Windows et Android hybride/strictement 2D : compilation du bouton **Hide interface / Masquer l’interface**, rappel par toucher/gâchette/A/Retour et clavier Entrée/Espace/F11.
- Quatre suites CTest réussies. Les nouveaux cas vérifient l'absence de widgets et de dock masqués, le rappel par pointeur et Retour, l'absence de clic au relâchement du premier geste, et la conservation de la navigation.
- `cyvr_cinema_probe` exécuté avec Vulkan sur PC : contrôle de tous les pixels en 1920 × 1080 et 1600 × 1100, vidéo seule et bandes noires aux bonnes dimensions, rapport d'image conservé et premier geste consommé. Une texture verte synthétique représente les pixels vidéo ; les dimensions proviennent d'un vrai média 320 × 192. Ce test ne prétend pas décoder matériellement une vidéo sur PC.
- Le Quest n'était plus connecté à ADB pendant ce changement. Le rappel via les véritables gâchettes, le bouton A et le pincement, ainsi que les couches XR sans interface, restent à tester sur le casque avec cette version. Les résultats Quest de la section suivante concernent la migration 0.4 précédente.

## Migration CyVR_UI 0.4 — 29 octobre 2026

La version 0.3 utilise `cyvr_android` / `runAndroidApp()`, avec fenêtre native 2D au lancement et immersion facultative. Les résultats actuels remplacent les descriptions de l'ancien hôte immersif ci-dessous :

- Windows : quatre suites CTest réussies (application, cœur média, entrées/sorties et cœur CyVR_UI). Les nouveaux cas couvrent transfert d'état entre contrôleurs, document/position en pause, navigation partagée, français/anglais, fichier tronqué et rejet d'une sauvegarde dépassant le budget sans écraser l'état valide.
- Android : APK hybride et variante `CYVR_ENABLE_IMMERSIVE=OFF` compilés et signés. La variante 2D retire le loader OpenXR, l'activité immersive et les requêtes runtime du manifeste ; `immersiveEnabled=false` à l'exécution.
- Quest 3 connecté : lancement réel en `PanelActivity`, surface Vulkan **1600 × 1100**, **OpenXR inactif**. Interactions Android testées : pages, stéréo TB, langue, thème et clavier d'URL.
- Redimensionnement système de la tâche à **1920 × 1080**, recréation de la surface, interaction avec le mapping inverse, puis retour à 1600 × 1100 : réussis. Mise en arrière-plan/reprise et redémarrage du processus : navigation et état métier conservés.
- Transition **2D → immersion → 2D** réussie par le bouton de l'application puis Android Back. Source, dossier/historique, brouillon d'URL, page et paramètres stéréo/projection conservés ; position restaurée à moins de 100 ms sur le petit média d'essai.
- Immersion : deux cibles **1680 × 1760** conformes à la recommandation du runtime testé, sans l'ancien plafond à 1024 pixels ; vidéo soumise à sa résolution source. Le thread natif continue de traiter pause/stop/destroy après la sortie de l'hôte pour éviter l'ANR observé et corrigé pendant cette migration.
- Flux dans la fenêtre 2D, dernier essai : HTTP AVC, HTTP HEVC et HLS AVC, chacun **48 images décodées et présentées, zéro saut**, en 320 × 192. Une recréation Vulkan inutile lors des notifications Android à taille identique provoquait auparavant deux à trois sauts au lancement ; elle a été supprimée. Les swapchains hors date/sous-optimales restent recréées.
- Régression GPU hors écran : AVC et HEVC, 48 images chacun ; pause/seek/reprise, 36 images chacun avec contrôle de génération. Ces quatre cas passent après correction de la reprise immédiate d'un seek en pause. Les écrans anglais et français ont également été rendus en Vulkan et inspectés sur PC.

Scripts reproductibles : `tests/quest_hosts.py` et `tests/quest_app.py`. Résultats : `build/quest-hosts-results.json`, `build/quest-app-results.json`, `build/apk-validation.json`, et leurs variantes `-panel` pour l'APK strictement 2D. Les essais pilotent l'application par ADB et lisent son état privé de test ; ils ne valident pas la perception avec le casque porté.

Validation restante : gestes physiques de déplacement/redimensionnement/fermeture via le shell Quest, usage Overlay pendant une autre immersion, suivi mains/manettes et aspect binoculaire 3D/180°/360°, parcours SAF réel, fidélité colorimétrique et charge prolongée 4K/8K/thermique sur les autres modèles Quest. La résolution native des cibles ne constitue pas une certification de ces performances. Son, sous-titres, HDR, splats et dispatch CAS/FSR restent à implémenter.

## Historique avant migration : vérifié sur la machine de développement

Le 29 octobre 2026 :

- Build Windows x64 avec Clang 19.1.5, CMake 3.22.1, Ninja et SDK Windows.
- 41 vérifications du cœur passées, y compris les séparations des yeux, les rayons 180°/360° et leurs pôles, les rapports d'image, le backpressure, la libération des ressources et les générations de seek.
- Build Android `arm64-v8a`, API minimale 29, NDK 27.2.12479018 / Clang 18.0.3 : bibliothèques statiques et édition de liens de `libcyvr_player_native.so` réussies.
- Format ELF64 AArch64 et alignement des segments LOAD à 16 Kio vérifiés avec `llvm-readelf`.
- Helper Java `CodecProbe` compilé contre la plateforme Android installée.
- CAS, EASU et RCAS : GLSL compilé avec `glslc` vers Vulkan 1.1 et les trois sorties validées par `spirv-val`.
- SHA-256 des six fichiers tiers vérifiés contre le lockfile.
- 33 vérifications supplémentaires des sources, du navigateur, des limites de scan et du rejet des chemins hors racine ont passé sur Windows.
- 16 cas d'intégration média sur Windows : MP4/MKV avec vidéo/audio décodées, descripteurs et sous-plages, seek, navigateur, HTTP, HLS/DASH et seek VOD, erreurs, délais, annulation et rejet TLS.
- 12 cas d'intégration média exécutés sur le Quest 3 connecté : fichiers, descripteurs, navigateur, HTTP/HLS, seek HLS VOD, annulation/délais et HTTPS avec CA explicite/rejet d'une CA inconnue.
- 2 cas matériels Quest 3 : AVC via `c2.qti.avc.decoder` et HEVC via `c2.qti.hevc.decoder`, 48 images chacun à 320×192, reçues en AHardwareBuffer sans lecture CPU des pixels. Les candidats proviennent du helper Java Android `isHardwareAccelerated`.
- Intégration CyVR_UI : construction de l'aperçu Windows et de l'APK `com.cyvr.player`, pages rendues en Vulkan et inspectées sur PC. Test d'application CTest : navigation asynchrone, actions par pointeur, propagation d'erreur média, stéréo et clavier ; les trois suites CTest passent.
- 4 cas GPU Quest 3 : AVC puis HEVC, 48 images importées/échantillonnées chacun ; pause/seek/reprise pour chaque codec, 36 images présentées et génération vérifiée après seek. Captures finales hors écran inspectées. La seule lecture CPU des pixels se situe après la lecture, dans le test de diagnostic.
- 3 cas dans l'APK : HTTP AVC, HTTP HEVC et HLS AVC avec paramètres SPS/PPS en bande. Chaque cas a décodé et soumis 48 images 320×192 aux swapchains OpenXR. Le runtime annonce equirect2. L'APK signé est installé sur le Quest 3 connecté.
- APK : sept bibliothèques ARM64, alignements ELF/ZIP à 16 Kio, signature vérifiée, notices présentes ; neuf shaders d'application validés avec `spirv-val`.

Les résultats détaillés sont enregistrés dans `build/media-integration-results.json`, `build/quest-smoke-results.json`, `build/quest-hardware-results.json`, `build/quest-gpu-results.json` et `build/quest-app-results.json`. Les bibliothèques FFmpeg Android sont compilées depuis les sources figées, avec Mbed TLS pour HTTPS.

Ces essais vérifient de petits médias synthétiques : 160×90 en logiciel et 320×192 en matériel/GPU. Ils ne prouvent ni la qualité perçue avec casque porté, ni le décodage 8K, ni les performances soutenues. L'import Vulkan et les soumissions OpenXR sont exécutés. Le son, la précision colorimétrique, le relief/projection perçus et la tenue thermique restent à qualifier. Le bridge SAF est compilé et intégré à l'Activity, mais la sélection/parcours d'un document provider autorisé réel reste à exercer.

## Jalons de livraison

1. FFmpeg figé, lecture et décodage sans affichage, navigateur et flux HTTP(S)/HLS : exécutés sur Quest 3. Pont AVC/HEVC matériel : exécuté. La sortie audio synchronisée reste à ajouter.
2. Import AHardwareBuffer/Vulkan avec fences, ownership et crop : exécuté. Appels Java intégrés à l'Activity ; parcours SAF manuel restant.
3. Boucle OpenXR, écran mono/SBS/TB et couches équirectangulaires 180°/360° : implémentés. Contrôle visuel/binoculaire et formats supplémentaires restants.
4. Connecter la sortie Oboe et l'horloge A/V, puis les pipelines CAS/FSR ; vérifier couleurs, cadence et coût thermique.
5. Étendre les protocoles/containers du profil Android et tester RTSP, le live, les changements de qualité et la reconnexion.
6. Lire une scène de splats statique puis une première séquence animée identifiée, avec budgets de mémoire/LOD.
7. CyVR_UI et explorateur intégrés ; poursuivre ergonomie, sélection SAF réelle, suivi des mains et réglages des effets.

## Mesures à effectuer sur chaque casque

Consigner modèle, version système, runtime OpenXR, pilote Vulkan, codec sélectionné, profil/niveau, résolution, profondeur/chroma, cadence et débit du fichier. Distinguer les capacités annoncées par Android et les résultats observés. Tester au minimum les casques réellement visés parmi Quest 2, Quest Pro, Quest 3 et Quest 3S ; aucun résultat ne doit être extrapolé à tous les modèles.

| Axe | Cas d'essai / mesure |
| --- | --- |
| Décodage | H.264, HEVC, VP9, AV1 lorsque disponibles ; 8/10 bits ; fichiers à faible et fort débit, GOP longs, B-frames, VFR |
| Géométrie | Mono, SBS/TB complet et demi-résolution, 180°/360°, inversion des yeux, rotation et crop |
| Couleur | BT.709 limité/complet, BT.2020 PQ/HLG, aplats, rampes et mire HDR, références SDR |
| Fluidité | PTS présentés, frames abandonnées/répétées, cadence VR, percentiles CPU/GPU p50/p95/p99 |
| Audio | Décalage A/V, dérive sur 30 minutes, pause/reprise, seek et changement de piste |
| Robustesse | Seek rapide répété, EOF, fichiers tronqués, activité mise en arrière-plan, pertes réseau et changements de format |
| Mémoire | Pic RSS, images acquises, imports en cache, buffers GPU, absence de fuite après répétition des ouvertures |
| Effets | Sans filtre, CAS, EASU+RCAS ; coût par passe et par œil, halos, coutures et stabilité temporelle |
| Splats | 100k à 1M, transparence proche, SH/LOD, changements temporels, débit de chargement et pics de tri |
| Autonomie | Séquence de 30 minutes après chauffe : cadence, throttling, température rapportée et consommation disponible |

À 72/90/120 Hz, la durée totale disponible par image VR est respectivement environ 13,89/11,11/8,33 ms. Le budget applicatif est inférieur une fois les contraintes runtime prises en compte ; ce ne sont pas des objectifs de temps que chaque passe peut consommer seule.

Premier critère : rester fluide avec le décodage matériel et les traitements désactivés. Ajouter ensuite un filtre et garder uniquement les configurations dont le coût visuel et thermique est acceptable. Les valeurs de performance ne seront annoncées qu'après ces mesures.
