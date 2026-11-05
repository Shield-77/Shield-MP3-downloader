## Shield MP3 Downloader

**Codex was used to write the C++ code**

Shield MP3 Downloader is a homebrew application for 3DS, based on the open-source project FourthTube.

This modified version focuses on music playback, MP3 downloading and audio conversion, with support for 128 kbps and 256 kbps output.

Je travaille actuellement sur la maintenance de la stabilité de l'app ainsi qu'à ses fonctionnalité, je m'excuse du retard des majs ect.

### Credits

- **FourthTube** — Original open-source project by Erie Valley Software and contributors.
- **ThirdTube** — Original project on which FourthTube is based.
- **SHIELD** — MP3 Downloader modifications, features and adaptations.

Original project: https://github.com/erievs/FourthTube

### License

This project is distributed under the GNU General Public License version 3.0 or later (GPL-3.0-or-later), following the license of FourthTube.

Original copyright notices and applicable third-party licenses are retained.

### Source and build

Download and extract [Shield-MP3-downloader-source.zip](Shield-MP3-downloader-source.zip) for the MP3 Downloader source tree, RomFS assets and build scripts. It contains no CIA or other SHIELD applications. The upstream FourthTube and ThirdTube projects retain their own credits and licenses. FFmpeg and libctru source locations are listed in `.gitmodules`; the headers and static libraries used by the local development build are included under `library/`.

Install devkitPro's 3DS toolchain and set `DEVKITPRO` and `DEVKITARM`. Obtain `makerom` and `bannertool` separately from their projects and place binaries compatible with your PC in `resource/tools/`. They are not redistributed in this source archive. See `BUILD-TOOLS.md`. From the extracted archive, run `make BUILD_JOBS=1`. The only target is MP3 Downloader and the resulting installable file is `MP3Downloader_dev.cia`.

The player browses YouTube with audio-only playback. Pause a video, press Y, choose 128 or 256 kbps, then choose whether to embed the video's thumbnail as MP3 cover art. A final confirmation starts the download and conversion. MP3s are saved under `SD:/download audio`; downloads longer than one hour are blocked. A language choice and three-page guide appear on first launch; the guide can be reopened by holding SELECT during startup.

The cover is the video's thumbnail, which may differ from official album artwork. If it cannot be retrieved, the audio still saves and the result screen reports that the cover is unavailable. See [MP3_DOWNLOADER_CHANGES.md](MP3_DOWNLOADER_CHANGES.md) for the latest changes.

I am not responsible for the user's actions; please respect licenses and the copyrights of those who deserve them.
