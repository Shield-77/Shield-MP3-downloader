# État des licences et des dépendances

Shield MP3 Downloader dérive de FourthTube, lui-même dérivé de ThirdTube. Le code de l’application et les modifications SHIELD suivent la GPL v3 ou ultérieure indiquée par le projet et son fichier `LICENSE`. Les fichiers tiers ne changent pas de licence du fait de cette notice.

Le projet utilise FFmpeg, libctru, libcurl, brotli, nghttp2, Shine, RapidJSON et stb_image. Les notices disponibles dans l’arbre source sont conservées, notamment `library/shine/COPYING`, `library/rapidjson/license.txt` et `library/stb_image/LICENSE`. Les fichiers `library/*/build.txt` et `.gitmodules` indiquent des versions, réglages ou dépôts amont historiques ; ils ne prouvent pas à eux seuls que les bibliothèques statiques du CIA publié ont été construites exactement depuis ces sources.

Les archives précompilées `.a` et `.la` ont été retirées de la copie source publique. Pour reconstruire le programme, il faut fournir des versions compatibles de ces bibliothèques. **La correspondance exacte entre les dépendances, leurs options de compilation, leurs licences effectives et le CIA v1.02 reste à vérifier.** Ne considérez pas cette publication du code comme une validation complète de conformité du binaire.

FFmpeg peut relever de la LGPL ou de la GPL selon les options et composants activés. Ses [consignes officielles de licence](https://ffmpeg.org/legal.html) demandent notamment que les sources correspondent aux bibliothèques distribuées. Les futurs builds et releases doivent documenter cette correspondance avant publication.

Les outils `makerom` et `bannertool` sont externes et ne sont pas distribués avec le source. Voir `BUILD-TOOLS.md`.
