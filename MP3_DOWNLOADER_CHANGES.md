# MP3 Downloader — v1.05, CIA version 6

- File de téléchargement enregistrée sur la SD : X ajoute la vidéo sélectionnée, l’écran File lance les MP3 en série, et la petite croix retire une seule entrée avec possibilité d’annuler.
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
