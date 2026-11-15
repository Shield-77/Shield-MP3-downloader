# Shield MP3 Downloader

Application 3DS de Shield-77 basée sur [FourthTube](https://github.com/erievs/FourthTube), lui-même issu de ThirdTube. Codex a été utilisé pour écrire les modifications C++ de SHIELD. La [dernière release v1.02](https://github.com/Shield-77/Shield-MP3-downloader/releases/tag/v1.02) fournit `MP3Downloader_dev.cia`.

L’application permet d’écouter le son d’une vidéo et d’enregistrer un MP3 en 128 ou 256 kb/s. Pour exporter, mettez la lecture en pause, appuyez sur **Y**, choisissez la qualité, puis indiquez si la miniature doit être intégrée comme pochette. Une confirmation précède le téléchargement. Le fichier est enregistré dans `SD:/download audio`. Les téléchargements de plus d’une heure sont bloqués. La miniature peut différer d’une pochette d’album ; si elle manque, l’audio peut quand même être enregistré et l’écran de résultat le signale.

Au premier lancement, l’application propose la langue et un guide de trois pages. Maintenir **SELECT** pendant le démarrage rouvre le guide. Voir [les changements de la version actuelle](MP3_DOWNLOADER_CHANGES.md).

## Code et droits

Le [code de l’application](source/), les [ressources](resource/) et les scripts de construction sont consultables dans ce dépôt. Le [ZIP source](Shield-MP3-downloader-source.zip) contient la même copie publique. Les bibliothèques statiques tierces précompilées en ont été retirées : leurs sources et options exactes doivent être vérifiées avant de les redistribuer. Cette archive seule ne permet donc pas de reproduire à l’identique le CIA déjà publié.

Le code hérité de FourthTube/ThirdTube et les modifications SHIELD sont distribués sous GPL v3 ou ultérieure selon la [licence du dépôt](LICENSE). Les dépendances et ressources tierces conservent leurs propres licences ; consulter [l’état des licences et de la compilation](LICENSING_STATUS.md). La GPL n’accorde pas le droit de faire passer un fork pour une release officielle de Shield-77. Les créations téléchargées par l’utilisateur restent soumises aux droits de leurs auteurs.

## Compiler depuis les sources

Il faut devkitPro/devkitARM, les bibliothèques 3DS requises, et des builds compatibles de FFmpeg, libctru, libcurl, brotli et nghttp2. Leurs archives statiques ne sont pas fournies ici. Les chemins se configurent par `FFMPEG_ROOT`, `LIBCTRU_ROOT`, `LIBCURL_ROOT`, `LIBBROTLI_ROOT` et `NGHTTP2_ROOT`. La bibliothèque Shine est construite depuis les sources incluses. Obtenir `makerom` et `bannertool` séparément ; voir [BUILD-TOOLS.md](BUILD-TOOLS.md).

```sh
make BUILD_JOBS=1 FFMPEG_ROOT=/chemin/FFmpeg LIBCTRU_ROOT=/chemin/libctru LIBCURL_ROOT=/chemin/libcurl LIBBROTLI_ROOT=/chemin/brotli NGHTTP2_ROOT=/chemin/nghttp2
```

La sortie prévue est `MP3Downloader_dev.cia`. Les chemins et étapes d’origine des dépendances sont documentés dans `library/*/build.txt`, mais leur correspondance avec le CIA de la release n’a pas été vérifiée. Aucun nouveau CIA n’a été produit par cette mise à jour du dépôt.
