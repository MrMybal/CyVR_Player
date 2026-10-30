# Current dependency profile — 0.4.5

The actual implemented scope is in the English [README](../README.md). FFmpeg now enables its built-in compatible decoders/demuxers and zlib. Libass (ISC), FreeType (FTL), HarfBuzz (MIT) and FriBidi (LGPL-2.1-or-later) provide subtitle styling/shaping. The pinned SplatKit Vulkan subset and VulkanMemoryAllocator are MIT; the player adapter borrows the host device instead of using their reference Android view. No INRIA noncommercial renderer is included. Exact source archive hashes are in `dependencies.lock.json`; corresponding notices ship in the APK. CyVR_UI retains MIT; CyVR_Player retains GPL-3.0-only.

The remaining sections record the earlier selection rationale and proposed coverage; they are not an assertion that every protocol/format is implemented.

# Choix des bibliothèques pour Meta Quest / Android

Étude du 29 octobre 2026. Les choix ci-dessous sont des recommandations d'architecture ; les performances sur Quest restent à mesurer. Aucune dépendance à Unity, Unreal ou à un SDK applicatif propriétaire Meta n'est proposée. Android, le runtime Quest et les pilotes matériels restent des composants de la plateforme, dont certains sont propriétaires : la contrainte open source peut couvrir le code et les dépendances applicatives, pas rendre le casque lui-même entièrement ouvert.

## Socle recommandé

| Besoin | Bibliothèque/API | Licence / statut | Décision |
| --- | --- | --- | --- |
| Conteneurs, pistes, paquets, codecs de secours, audio | [FFmpeg](https://ffmpeg.org/) : libavformat, libavcodec, libavutil, libswresample ; libswscale pour le secours | LGPLv3+ dans le profil retenu | Intégré et exécuté sur Windows et Quest 3 |
| Décodage vidéo matériel | [MediaCodec NDK](https://developer.android.com/ndk/reference/group/media), API Android | API système Android/AOSP ; implémentations matérielles de la plateforme | Voie principale ; pont AVC/HEVC testé sur Quest 3 |
| TLS Android | [Mbed TLS 3.6.7](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7) | Option Apache-2.0 de la licence amont | Compilé avec FFmpeg ; HTTPS vérifié sur Quest 3 |
| Réception GPU des images décodées | AImageReader PRIVATE, AHardwareBuffer et [interop Vulkan Android](https://docs.vulkan.org/spec/latest/chapters/memory.html) | APIs Android et Vulkan | Vise l'absence de copie des pixels vers le CPU ; import Vulkan à réaliser |
| Présentation VR | [OpenXR SDK/loader Khronos](https://github.com/KhronosGroup/OpenXR-SDK), `XR_KHR_vulkan_enable2` | Headers/loader Apache-2.0 OU MIT | Préféré ; runtime fourni par le casque |
| Rendu et traitements | Vulkan 1.1 de base, fonctions supplémentaires détectées | Headers du NDK r27 utilisés ici : Apache-2.0 | Cible minimale de développement, capacités réelles à vérifier |
| Sortie audio Android | [Oboe](https://github.com/google/oboe) | Apache-2.0 | À intégrer ; horloge audio pour synchroniser la vidéo |
| Netteté | [FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) | MIT | Noyau officiel et wrapper GLSL embarqués |
| Agrandissement spatial | [FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) EASU + RCAS | MIT | Noyau officiel et wrappers GLSL embarqués |
| Allocation des images internes | [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT | Option utile ensuite ; pas nécessaire au socle actuel |
| Compression de splats statiques / images clés | [SPZ](https://github.com/nianticlabs/spz) | MIT | Candidat pour les données ; ce n'est pas un codec vidéo temporel |
| Rendu mobile de splats | [SplatKit](https://github.com/Xget7/splatkit) | MIT, bêta | À évaluer et adapter au moteur OpenXR ; pas adopté comme dépendance |

Les bibliothèques qui ne sont pas encore embarquées devront être figées sur un commit ou une version, avec sources et licences de leurs dépendances transitives, avant leur intégration au build.

## Pourquoi FFmpeg plutôt qu'un fork de VLC

LibVLC est une option sérieuse : l'équipe VideoLAN fournit un moteur Android réutilisable, distinct de l'application VLC Android. Les licences et les modules activés diffèrent entre l'application et le moteur. [Source VideoLAN](https://github.com/videolan/vlc-android).

Pour CyVR_Player, je recommande FFmpeg et une voie de décodage Android explicite. C'est un choix de maîtrise du pipeline : même VkDevice que le futur rendu custom, contrôle de la propriété des images, des PTS et des effets. Ce n'est pas un résultat de benchmark prouvant que FFmpeg est toujours plus rapide que LibVLC. L'économie principale recherchée vient de l'utilisation du décodeur matériel et de l'absence de copies CPU des images.

[Media3/ExoPlayer](https://developer.android.com/media/media3/exoplayer) reste une alternative si les flux adaptatifs et la gestion Android priment. Son intégration demanderait un adaptateur Java vers le moteur natif. Elle n'est pas nécessaire pour préparer ce cœur C++.

Le build FFmpeg retenu reste sans `--enable-gpl` ni `--enable-nonfree`, mais active `--enable-version3` pour ses dépendances : les bibliothèques obtenues sont LGPLv3+. Les recettes et archives sont figées. La distribution devra fournir les licences et les sources exactes correspondant aux binaires, y compris les dépendances TLS. FFmpeg indique lui-même que les options activées peuvent changer la licence du binaire. [Politique FFmpeg](https://ffmpeg.org/legal.html).

## Couverture visée et limites

Un conteneur, un codec et une projection sont trois choses différentes. Une vidéo MKV peut contenir du HEVC, du H.264 ou un autre codec, puis être affichée à plat, en SBS ou sur une sphère. Le lecteur doit inspecter les pistes et les métadonnées, sans décider du décodeur à partir de l'extension du fichier.

| Famille | Couverture visée | Validation nécessaire |
| --- | --- | --- |
| Conteneurs | MP4/MOV, MKV/WebM, AVI, MPEG-TS et autres démuxeurs FFmpeg retenus | Fichiers de référence par conteneur, index et seek |
| Codecs courants | H.264, HEVC, VP9, AV1 selon capacités ; autres via secours logiciel si viable | Profil, niveau, profondeur, chroma, dimensions, cadence et débit sur chaque casque |
| Vidéo normale | Écran mono, rapport d'image, rotation, pixel aspect ratio | Métadonnées, géométrie et couleurs |
| Vidéo 3D | SBS/TB, demi ou pleine résolution, inversion des yeux | Rapport affiché par œil, ordre des yeux et absence de contamination entre vues |
| Immersion | 180°/360° équirectangulaire mono/stéréo ; cubemap et autres projections plus tard | Métadonnées, orientation, coutures et projection réelle |
| Vidéo spatiale multivue | MVC / MV-HEVC : chantier distinct | Un flux décodable ne prouve pas que les deux vues peuvent être obtenues |
| Audio et sous-titres | Décodage audio, sélection des pistes ; SRT/WebVTT puis formats complexes | Synchronisation, langue, encodage ; libass à évaluer pour ASS/SSA |
| Réseau | Lecture HTTP(S), puis HLS/DASH et partages suivant les besoins | Backend TLS et protocoles compilés, débit, reprise et buffers bornés |
| Gaussian Splatting | Scènes statiques, puis séquences temporelles selon schéma défini | Format exact, mémoire, tri GPU, LOD et cohérence temporelle |

Ce tableau décrit la couverture visée. L'état implémenté et testé par plateforme est précisé dans [Fichiers et flux](sources-et-flux.md). « Tous les formats » ne peut pas être garanti sur un SoC mobile. Le secours logiciel peut augmenter la couverture sans permettre la même résolution/cadence. Aucun support de contenu chiffré nécessitant un système DRM n'est établi ici.

## Détection matérielle

Ne pas coder une promesse « 8K », « AV1 » ou « 120 FPS » à partir du seul nom Quest 2/3/3S/Pro. Le système doit examiner `isHardwareAccelerated`, `isFormatSupported`, les profils et les tailles/cadences admises, puis tenter de configurer le codec. Le helper `CodecProbe.java` fournit des candidats, sans garantir leur débit soutenu. [Capacités Android](https://developer.android.com/reference/android/media/MediaCodecInfo.VideoCapabilities).

Mesurer ensuite une vraie séquence longue, y compris en 10 bits et à débit élevé. Le nombre de décodeurs simultanés et la conversion YCbCr peuvent aussi modifier les résultats. Le fallback logiciel doit être explicite ; il ne faut pas basculer silencieusement vers un décodage lourd qui ferait perdre la fluidité VR.
