# État des licences et des dépendances

Shield MP3 Downloader dérive de FourthTube, lui-même dérivé de ThirdTube. Le code de l’application et les modifications SHIELD suivent la GPL v3 ou ultérieure indiquée par le projet et son fichier `LICENSE`. Les fichiers tiers ne changent pas de licence du fait de cette notice.

Le projet utilise FFmpeg, libctru, libcurl, brotli, nghttp2, Shine, RapidJSON et stb_image. Les notices disponibles dans l’arbre source sont conservées, notamment `library/shine/COPYING`, `library/rapidjson/license.txt` et `library/stb_image/LICENSE`. Les fichiers `library/*/build.txt` et `.gitmodules` indiquent des versions, réglages ou dépôts amont historiques ; ils ne prouvent pas à eux seuls que les bibliothèques statiques du CIA publié ont été construites exactement depuis ces sources.

Les archives précompilées `.a` et `.la` ont été retirées de la copie source publique. Pour reconstruire le programme, il faut fournir des versions compatibles de ces bibliothèques. La construction locale de v1.08 pointe vers les six archives FFmpeg dont les empreintes figurent ci-dessous ; ces empreintes ont été vérifiées le 16 novembre 2026. **Leur correspondance exacte avec le code source et les options de compilation historiques reste à établir**. Ne considérez pas cette publication du code comme une validation complète de conformité du binaire.

FFmpeg peut relever de la LGPL ou de la GPL selon les options et composants activés. Ses [consignes officielles de licence](https://ffmpeg.org/legal.html) demandent notamment que les sources correspondent aux bibliothèques distribuées. Cette correspondance et les autres dépendances statiques doivent encore être documentées complètement.

Les images privées `resource/mp3downloader_banner.png` et `resource/mp3downloader_icon.png` sont les visuels SHIELD présents dans le projet local v1.01 ; v1.08 les reprend dans le CIA sans les inclure dans la source publique. Le son `resource/banner.wav` est conservé tel qu’il était dans ce projet local, sans nouvelle revendication de propriété.

Les outils `makerom` et `bannertool` sont externes et ne sont pas distribués avec le source. Voir `BUILD-TOOLS.md`.

## Relevé des bibliothèques FFmpeg locales (15 novembre 2026)

Les trois dossiers de travail locaux utilisés pour MP3 Downloader, MP4 Player et MP4 Converter contiennent **six archives FFmpeg strictement identiques octet pour octet**. Leurs SHA-256 sont :

| Archive | SHA-256 |
| --- | --- |
| `libavcodec.a` | `b0ec02def82c028f744b5c8b7b6247961c81ae747f2c38577317d49e58f766b8` |
| `libavfilter.a` | `759610858238955549997a1ad375efb562dd279c326cb0402387a9456eed0d27` |
| `libavformat.a` | `0d9b3a708ea0438d84f9eebeb008f9a8635b5f160c6c236ca426cd7cc56d6ab4` |
| `libavutil.a` | `0686e42596f39bb5be60c2b666d9e383487c6458cabb1334fd674e23b2736075` |
| `libswresample.a` | `0a58c02cb6b283674954bda2778ebf99d2d5b5c9a8a4719febe34b33bbac4e03` |
| `libswscale.a` | `1f89104514f677ab29efe4f8b158b08e2f64eb4352a38001914f50f12bf11e3f` |

Le fichier hérité `library/FFmpeg/build.txt` indique le commit source [`aea5ffd1d2d6d67f2557e38e9e7746b850f8f4bc`](https://github.com/windows-server-2003/FFmpeg/commit/aea5ffd1d2d6d67f2557e38e9e7746b850f8f4bc), devkitARM 68, une chaîne de version `git-2026-07-25-75a6143` et une ligne `configure`. L’existence du commit a été confirmée. **Ce relevé ne prouve pas** que les archives locales ont été construites avec ce commit, ni que ces mêmes octets ont été liés dans les trois CIA déjà publiés. Les options de licence effectives et les sources exactes correspondant aux CIA restent donc à établir par des journaux de build ou une reconstruction contrôlée.

### Recherche complémentaire du 28 novembre 2026

Un ancien dossier de travail MP4 local contient une copie complète de l'arbre source FFmpeg (avec `configure`, `libavcodec/`, `libavformat/` et les textes `COPYING.GPLv2`, `COPYING.GPLv3`, `COPYING.LGPLv2.1`, `COPYING.LGPLv3`). Il contient aussi les six archives `.a` dont les empreintes correspondent au tableau ci-dessus. La copie source ne conserve pas ses métadonnées Git : sa révision exacte n'a donc pas été vérifiée. Aucun `config.log` ni `config.mak` de cette construction n'a été retrouvé dans cet arbre. Le fichier `build.txt` reste une indication historique, pas une preuve de correspondance entre ce source, les archives et les CIA. Cette copie locale n'a pas été substituée aux dépendances et aucune reconstruction n'a été effectuée pendant cette maintenance.
