# Shield MP3 Downloader

**Codex was used to generate the code**

Application 3DS de Shield-77 basée sur [FourthTube](https://github.com/erievs/FourthTube), lui-même issu de ThirdTube. Codex a été utilisé pour écrire les modifications C++ de SHIELD. La [release v1.07](https://github.com/Shield-77/Shield-MP3-downloader/releases/tag/v1.07) fournit `MP3Downloader_dev.cia`.

L’application permet d’écouter le son d’une vidéo et d’enregistrer un MP3 en 128 ou 256 kb/s. Pour exporter, mettez la lecture en pause, appuyez sur **Y**, choisissez la qualité, puis indiquez si la miniature doit être intégrée comme pochette. Une confirmation précède le téléchargement. Le fichier est enregistré dans `SD:/download audio`. Les téléchargements de plus d’une heure sont bloqués. La miniature peut différer d’une pochette d’album ; si elle manque, l’audio peut quand même être enregistré et l’écran de résultat le signale.

Au premier lancement, l’application propose la langue et un guide de quatre pages. Maintenir **SELECT** pendant le démarrage rouvre le guide. Voir [les changements de la version actuelle](MP3_DOWNLOADER_CHANGES.md).

Pour préparer plusieurs MP3, sélectionnez une vidéo dans les résultats avec haut/bas, puis appuyez sur **X**. **X** ajoute aussi la vidéo affichée depuis sa fiche, y compris dans une playlist. Touchez **File** (ou **Queue** en anglais) en haut de l’écran inférieur, juste à côté de la barre de recherche : cela ouvre la file même depuis l’accueil. Lancez la file en 128 ou 256 kb/s. **B** ferme la file ; si un téléchargement est en cours, **B** l’arrête d’abord, puis un second appui ferme la file. La pochette est désactivée par défaut dans la file pour éviter une requête réseau par titre ; touchez **Pochette** si vous la souhaitez. La file est conservée dans `SD:/3ds/MP3Downloader/download-queue-v1.tsv`. Une vidéo indisponible est signalée et sautée ; touchez sa ligne pour la remettre en attente. La petite croix retire uniquement la ligne de la file ; **Annuler** permet de la rétablir. Les MP3 déjà enregistrés restent dans `SD:/download audio`.

Si la connexion coupe, la file et les titres en échec restent enregistrés. Relancez la file pour réessayer : le téléchargement reprend son fichier temporaire lorsque la source le permet. Le bouton **B** arrête le traitement en cours. Pour protéger la lecture audio, les miniatures chargent une par une pendant la lecture et plus vite lorsque le lecteur est au repos.

Dans une playlist, toucher une vidéo affiche sa fiche et son bouton Play. **A** lance la vidéo affichée, puis sert à mettre en pause ou reprendre sa lecture. Le module de mise à jour hérité de FourthTube a été retiré des paramètres ; les releases de Shield MP3 Downloader sont publiées ici et consultables avec Shield Updater.

## Code et droits

Le [code de l’application](source/), les [ressources](resource/) et les scripts de construction sont consultables dans ce dépôt. Le [ZIP source](Shield-MP3-downloader-source.zip) contient la même copie publique. Les bibliothèques statiques tierces précompilées en ont été retirées : leurs sources et options exactes doivent être vérifiées avant de les redistribuer. Cette archive seule ne permet donc pas de reproduire à l’identique le CIA déjà publié.

Le code hérité de FourthTube/ThirdTube et les modifications SHIELD sont distribués sous GPL v3 ou ultérieure selon la [licence du dépôt](LICENSE). Les dépendances et ressources tierces conservent leurs propres licences ; consulter [l’état des licences et de la compilation](LICENSING_STATUS.md). La GPL n’accorde pas le droit de faire passer un fork pour une release officielle de Shield-77. Les créations téléchargées par l’utilisateur restent soumises aux droits de leurs auteurs.

## Compiler depuis les sources

Il faut devkitPro/devkitARM, les bibliothèques 3DS requises, et des builds compatibles de FFmpeg, libctru, libcurl, brotli et nghttp2. Leurs archives statiques ne sont pas fournies ici. Les chemins se configurent par `FFMPEG_ROOT`, `LIBCTRU_ROOT`, `LIBCURL_ROOT`, `LIBBROTLI_ROOT` et `NGHTTP2_ROOT`. La bibliothèque Shine est construite depuis les sources incluses. Obtenir `makerom` et `bannertool` séparément ; voir [BUILD-TOOLS.md](BUILD-TOOLS.md).

```sh
make BUILD_JOBS=1 FFMPEG_ROOT=/chemin/FFmpeg LIBCTRU_ROOT=/chemin/libctru LIBCURL_ROOT=/chemin/libcurl LIBBROTLI_ROOT=/chemin/brotli NGHTTP2_ROOT=/chemin/nghttp2
```

La sortie prévue est `MP3Downloader_dev.cia`. Les chemins et étapes d’origine des dépendances sont documentés dans `library/*/build.txt` ; voir [l’état des licences](LICENSING_STATUS.md) pour les limites de provenance des archives statiques.
