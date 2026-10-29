# Gaussian Splatting et vidéos volumétriques

Une vidéo 360° conserve une image en deux dimensions projetée autour de l'observateur. Une scène de splats décrit une représentation spatiale qui peut produire des vues différentes lorsque l'on déplace la tête. Une vidéo de splats ajoute une évolution temporelle de cette représentation. Ces chemins doivent partager l'horloge du lecteur, mais leurs décodeurs et leurs rendus diffèrent.

## Dépendances examinées

- [SPZ, Niantic](https://github.com/nianticlabs/spz), MIT : bibliothèque C++ et format compact de splats. Candidat pour les scènes et les images clés ; la présence d'un `.spz` ne définit pas une animation.
- [SplatKit](https://github.com/Xget7/splatkit), MIT : moteur C++17 avec backend Android/Vulkan 1.1, visibilité, tri GPU et LOD. Le projet annonce une bêta et des validations sur téléphone Adreno 640, pas une validation OpenXR Quest soutenue. Il faudra examiner la réutilisation de notre VkDevice, les ressources par œil et le cycle OpenXR avant adoption.
- [Vulkan Gaussian Splatting, nvpro](https://github.com/nvpro-samples/vk_gaussian_splatting), Apache-2.0 : référence technique, ciblant actuellement des GPU Vulkan 1.4 Windows/Linux. Il ne faut pas supposer qu'il puisse être embarqué tel quel sur un Quest.
- [gsplat](https://github.com/nerfstudio-project/gsplat) : infrastructure centrée sur CUDA, donc pas une dépendance de rendu Android/Adreno pour ce projet.
- Le dépôt original [graphdeco-inria/gaussian-splatting](https://github.com/graphdeco-inria/gaussian-splatting/blob/main/LICENSE.md) limite l'usage à la recherche/évaluation non commerciale. Cette licence ne répond pas à l'exigence de dépendances open source sans restriction d'usage ; il n'est pas importé.

La compatibilité avec un format de données ne demande pas nécessairement de reprendre l'implémentation de recherche qui le produit. Vérifier séparément les licences du lecteur, des dépendances et des contenus de démonstration.

## Ordre d'implémentation

1. Ouvrir une scène statique PLY de Gaussian Splatting ou SPZ et produire deux vues OpenXR. Un PLY de points XYZ seul n'est pas forcément une scène de splats : il faut notamment la forme/covariance, l'opacité et la couleur appropriées.
2. Ajouter une séquence à images clés indépendantes, avec PTS explicites et préchargement borné. C'est une première voie de lecture animée prévisible, avec un coût de débit et de mémoire potentiellement élevé.
3. Définir un stockage temporel avec deltas, identifiants stables et index de seek si les contenus l'exigent. Ne pas interpoler arbitrairement deux tableaux de splats sans correspondances.
4. Ajouter des adaptateurs aux représentations 4D/déformation précises utilisées par les vidéos réelles. Les poids d'un modèle entraîné ne sont pas automatiquement un média que FFmpeg ou SPZ sait décoder.

`examples/splat-sequence.example.json` illustre un **schéma interne proposé**, non standard et pas encore chargé par du code. Les chemins y désignent des exemples fictifs. Il ne prouve aucune lecture animée actuelle.

## Pipeline performant visé

Lire et décompresser sur un worker, puis uploader dans un ensemble borné de buffers GPU. Choisir le LOD et rejeter les splats hors champ ou sous-pixel avant tri. Commencer par un tri correct par œil et valider les occlusions proches avant de tenter de partager du travail entre les deux yeux.

La géométrie et le temps d'animation sont communs aux deux vues ; la projection et les effets dépendant de la vue sont calculés par œil. À chaque nouvelle pose, le rendu doit rester cohérent même si l'animation n'avance qu'à 24 ou 30 images/s.

La consommation inclut données résidentes, SH, hiérarchie LOD, clés et index du tri, buffers temporaires, staging et images des deux yeux. À titre de calcul de budget, 500 000 splats à 64 octets représentent déjà 32 Mo par état, avant ces autres ressources ; charger toute une animation serait vite excessif. Ce chiffre est un exemple de représentation, pas une taille annoncée pour SPZ ou SplatKit.

Le premier benchmark doit comparer 100k, 250k, 500k et 1M splats selon la scène et le casque. Ces valeurs sont des charges d'essai, pas des capacités garanties. Réduire LOD/résolution pour tenir le budget GPU avant d'augmenter la netteté. Sur Quest, aucune voie CUDA ou ray tracing obligatoire n'est retenue.

Pour le son, associer un flux audio à la même time base que les états volumétriques. Un seek doit retrouver une image clé et son audio, puis appliquer les deltas requis. Il faut annuler les lectures de l'ancienne génération sans supprimer ses ressources encore utilisées par le GPU.
