# MP3 Downloader — v1.09, CIA version 10

- Une entrée est retirée de la file uniquement après la création réussie du MP3 final. Les sources bloquées, les transferts interrompus et les conversions échouées restent visibles pour être repris.
- Le même transfert réseau est réutilisé entre les titres de la file et la pochette afin d’éviter des connexions répétées. Les sources AAC en conteneur MP4 avec paramètres complets évitent une analyse supplémentaire avant conversion.
- Sur Old 3DS, le traitement MP3 essaie d’utiliser le cœur principal avec une priorité basse pour laisser l’interface réactive ; en cas d’échec de création du thread, l’ancien emplacement reste disponible. Les temps de téléchargement et de conversion sont affichés séparément à la fin.
- Le calcul du débit affiché pendant le téléchargement ne divise plus par zéro autour de la première seconde et indique désormais la bonne unité.

## Historique : v1.08, CIA version 9

Le thread d’export dort au repos et attend un signal lors de l’arrêt du lecteur. Les tampons réseau et MP3 sont légèrement agrandis pour réduire les appels. Une erreur d’écriture SD interrompt aussitôt la tentative. Les téléchargements et les redirections de l’export MP3 sont limités à HTTPS, l’identifiant de vidéo est validé avant d’être inclus dans un nom de fichier et le fichier source temporaire est limité à 192 MiB. La vérification des certificats TLS nécessite encore un magasin de confiance compatible avec la console et reste une limite connue ; la seule restriction HTTPS ne constitue pas une authentification du serveur.

- File de téléchargement enregistrée sur la SD : X ajoute la vidéo sélectionnée, le bouton tactile File/Queue en haut de l’écran inférieur ouvre la file depuis l’accueil ou la recherche, B la ferme après avoir arrêté le traitement éventuel, et la petite croix retire une seule entrée avec possibilité d’annuler.
- Une source bloquée ou indisponible est indiquée et sautée ; les autres titres continuent. Les échecs restent dans la file et peuvent être retentés après une coupure réseau.
- La pagination des recherches conserve les résultats utilisables lorsqu’un élément spécial inconnu apparaît et reconnaît davantage de réponses de continuation.
- Les miniatures utilisent jusqu’à trois transferts au repos, un pendant la lecture et aucun pendant l’export audio. Les couvertures sont facultatives et désactivées par défaut dans la file.
- Les réponses HTTP 401/403/404/410 arrêtent rapidement une tentative impossible ; un fichier source complet est réutilisé après un échec de conversion. Le guide intégré passe à quatre pages.
- La conversion écrit d’abord un `.part` et ne crée le MP3 final qu’après fermeture complète du fichier.

## Historique : v1.04, CIA version 5

- Correction du lancement explicite depuis une playlist sans lecture préalable : A démarre la vidéo affichée, puis contrôle pause/reprise.
- Toucher un élément d’une playlist ouvre sa fiche dans l’onglet Général, où Play est accessible.
- Suppression d’un accès hors limites lors du passage entre playlists de tailles différentes. Les couleurs de sélection sont appliquées à la nouvelle liste.
- Le module de mise à jour hérité qui pointait vers FourthTube n’est plus accessible et ne démarre plus de tâche réseau.
- Le mode audio seul prépare jusqu’à 2,5 secondes de son avant le début de lecture pour mieux absorber les variations brèves du réseau ; un flux court peut démarrer avec moins.

## Historique : v1.03, CIA version 4

- Ouvrir une playlist affiche sa liste sans lancer la vidéo intégrée à son URL.
- Toucher une autre vidéo de la playlist affiche sa fiche ; seul le bouton Play lance cette sélection.
- La lecture automatique configurée reste disponible après une lecture lancée volontairement.
- Des messages de diagnostic distinguent l'ouverture d'une playlist d'une demande Play.

## Historique : CIA version 2

- Three-page first-use guide on the upper screen. A advances, B goes back, and SELECT held at startup reopens it. Completion is saved in the CIA save data, with an SD marker fallback when save data is unavailable.
- After choosing 128 or 256 kbps, A requests an embedded cover and B selects no cover. X cancels this choice. The separate confirmation screen still follows.
- When requested, the YouTube video thumbnail is downloaded with a 128 KiB cap and embedded as JPEG APIC artwork inside an ID3v2.4 tag in the MP3. The title is embedded too. Missing artwork does not discard a successfully converted MP3; the result screen says whether artwork was included.
- Export prompts take input before the underlying player, so B on the artwork screen cannot also navigate away.
- The artwork request has its own visible state and a 15-second timeout. It can be cancelled with B.
- Output stays in `SD:/download audio`, stereo 44.1 kHz, at the selected MP3 bitrate. The one-hour limit remains.

The CIA compiles with one build job.
