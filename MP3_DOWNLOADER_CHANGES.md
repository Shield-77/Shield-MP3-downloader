# MP3 Downloader — CIA version 2

- Three-page first-use guide on the upper screen. A advances, B goes back, and SELECT held at startup reopens it. Completion is saved in the CIA save data, with an SD marker fallback when save data is unavailable.
- After choosing 128 or 256 kbps, A requests an embedded cover and B selects no cover. X cancels this choice. The separate confirmation screen still follows.
- When requested, the YouTube video thumbnail is downloaded with a 128 KiB cap and embedded as JPEG APIC artwork inside an ID3v2.4 tag in the MP3. The title is embedded too. Missing artwork does not discard a successfully converted MP3; the result screen says whether artwork was included.
- Export prompts take input before the underlying player, so B on the artwork screen cannot also navigate away.
- The artwork request has its own visible state and a 15-second timeout. It can be cancelled with B.
- Output stays in `SD:/download audio`, stereo 44.1 kHz, at the selected MP3 bitrate. The one-hour limit remains.

The CIA compiles with one build job. Playback and artwork display still need a real 3DS check.
