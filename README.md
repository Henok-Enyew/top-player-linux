<a id="top"></a>

<div align="center">

<img src="packaging/linux/org.github.topplayer.svg" alt="Top Player logo" width="112">

# Top Player

**A fast, lightweight native media player for Linux, built on Qt 6 and libmpv.**

Video, music, live TV and online radio in one dark, focused player, with subtitle downloads,
synced lyrics, a 10-band equalizer, a playlist manager and a media library.

[![Latest release](https://img.shields.io/github/v/release/Henok-Enyew/top-player-linux?label=release&color=00D2FF)](https://github.com/Henok-Enyew/top-player-linux/releases/latest)
[![CI](https://github.com/Henok-Enyew/top-player-linux/actions/workflows/ci.yml/badge.svg)](https://github.com/Henok-Enyew/top-player-linux/actions/workflows/ci.yml)
[![Release build](https://github.com/Henok-Enyew/top-player-linux/actions/workflows/release.yml/badge.svg)](https://github.com/Henok-Enyew/top-player-linux/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-121316)](LICENSE)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-1A1C22)
![Qt 6](https://img.shields.io/badge/Qt-6-1A1C22)
![libmpv](https://img.shields.io/badge/powered%20by-libmpv-1A1C22)

**[⬇ Download](https://github.com/Henok-Enyew/top-player-linux/releases/latest)** ·
[Features](#features) ·
[Screenshots](#screenshots) ·
[Install](#install) ·
[Shortcuts](#keyboard-and-mouse) ·
[Build](#building-from-source) ·
[Releases](#releases) ·
[License](#license)

</div>

---

## Screenshots

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/start-screen.png" alt="Start screen with open actions"></td>
    <td width="50%"><img src="docs/screenshots/audio-playlist.png" alt="Audio view with album art and the playlist drawer"></td>
  </tr>
  <tr>
    <td align="center"><sub>Start screen</sub></td>
    <td align="center"><sub>Audio view with album art, and the playlist drawer</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/screenshots/live-tv.png" alt="Live TV & Radio browser"></td>
    <td width="50%" align="center"><img src="docs/screenshots/equalizer.png" alt="Audio Control & Equalizer" width="300"></td>
  </tr>
  <tr>
    <td align="center"><sub>Live TV &amp; Radio browser</sub></td>
    <td align="center"><sub>Audio Control &amp; 10-band equalizer</sub></td>
  </tr>
</table>

## Highlights

|  |  |
| --- | --- |
| 🎬 **Plays everything mpv plays** | Every common video and audio format, streams and playlists, with hardware decoding. |
| 📺 **Live TV & Radio** | Free channels and stations from every country, with categories, search and automatic fallback streams. |
| 💬 **Subtitles without an account** | Exact-match and by-name search, one-click download, dual subtitles, tap-to-sync editor. |
| 🎤 **Synced lyrics** | Free LRC downloads, a karaoke-style view, a tap-to-sync editor, and a ready-made AI prompt for songs no site has. |
| ⏯️ **Pick up where you left off** | Reopen any file and choose **Resume** or **Start Over**. |
| 🎚️ **Studio-style sound** | Preamp, bass, treble, a 10-band equalizer with presets, and night mode. |
| 📂 **Playlist & Library** | Sorting, search, shuffle and repeat, M3U save/open, and saved folders and playlists. |
| ✂️ **Built-in tools** | Lossless cutting, audio extraction, and downloads from YouTube and 1000+ sites. |
| ⚡ **Fast and light** | Native Qt 6 and libmpv, background scanning, no freezes on huge folders. |

---

## Features

<details open>
<summary><b>Contents</b></summary>

- [Playback](#playback)
- [Interface](#interface)
- [Audio view](#audio-view)
- [Lyrics](#lyrics)
- [Audio Control & Equalizer](#audio-control--equalizer)
- [Playlist](#playlist)
- [Library](#library)
- [Live TV & Radio](#live-tv--radio)
- [Subtitles](#subtitles)
- [Tools](#tools)
- [Settings and files](#settings-and-files)

</details>

### Playback

- Opens **files, folders, URLs and streams** (http, https, rtsp, rtmp, ...) and
  playlists (`.m3u`, `.m3u8`, `.pls`): from the start screen, the right-click
  menu, the command line (`top-player video.mkv [more files...]`) or by
  **drag and drop** (the first file plays, the rest are queued).
- **Folders** are scanned with all their subfolders in a background thread, in
  natural order ("Episode 2" before "Episode 10"), and handed to mpv as one
  batch, so thousands of files never freeze the window.
- **Hardware decoding**: Auto (Safe), Auto (Copy-back), VA-API, NVDEC, Vulkan
  or Off (Software).
- **Speed** from 0.25x to 2.0x, in 0.1 steps (`C` / `X` / `Z`), **Loop File**,
  **Shuffle** and **Repeat** (Off → All → One).
- **Video**: track selection, **aspect ratio** (Default, 4:3, 16:9, 1.85:1,
  2.35:1), quick **Fit to Window / 16:9 / 100%** button, **rotate** (0°, 90°,
  180°, 270°), **deinterlace**, and **screenshots** (`Ctrl+E`, saved to
  `~/Pictures`).
- **Audio**: track selection, mute, volume up to mpv's maximum, and **audio
  delay** in 0.1 s steps.
- **Stop keeps the playlist**: Play, Previous and Next continue from the entry
  that was playing.
- **Media keys** (play/pause, stop, previous, next) work wherever the focus
  is: the player speaks **MPRIS**, so the desktop hands it the keyboard's
  media keys and its sound applet, lock screen and `playerctl` control it too.
- **Swipe to seek**, as in VLC: hold the video and drag sideways (the full
  width spans 3 minutes, or the whole file if shorter); the OSD shows the jump
  and the target time, `Esc` cancels. Sideways touchpad or tilt-wheel
  scrolling seeks 5 s per step.
- **Resume or start over**: every file remembers where it was left. Opening
  it again shows a small card, *Continue watching?* (or *listening?*), with
  **Resume from 12:34** and **Start Over** (`Enter` / `Esc`); it resumes by
  itself after 10 seconds. **Playback → When Reopening a File** switches
  between asking, **Always Resume** and **Always Start Over**.
- **Opened media plays right away**, even if the last file was paused or
  ran to its end. Opening a folder or playlist **replaces** the playlist;
  a slower scan that finishes after a newer open is dropped.

### Interface

A flat **electric cyan on obsidian** skin (accent `#00D2FF`, surface `#121316`,
panels `#1A1C22`) defined in [`resources/skin/top-player.qss`](resources/skin/top-player.qss)
on top of Qt's Fusion style; self-painted widgets share the palette in
[`src/Theme.h`](src/Theme.h). All icons are drawn as vectors in code.

- **Start screen** while nothing is loaded, with **Open File**, **Open
  Folder**, **Open URL / Stream**, **Open Playlist** and a drop zone. It fades
  out as playback starts and adapts to small windows, down to icon-only buttons.
- **Title bar** with the logo, the media title and PotPlayer's corner
  buttons: **always on top** (pin), minimize, maximize, **fullscreen** and
  close, with soft animated highlights (red for close). Drag it to move the
  window, double-click to maximize.
- **Control bar** with open, previous, play/pause, stop and next; **shuffle**
  (lit cyan while on) and **repeat** (the icon shows a "1" in Repeat One); the
  time; volume; an **aspect** button (Fit to Window → 16:9 → 100%); and
  playlist and fullscreen toggles. The mode buttons follow the menu (e.g. Loop
  File) and step aside in narrow windows.
- **Seekbar** that thickens on hover, marks chapters and the cut range, and
  shows a **thumbnail preview** of the hovered moment. Previews come from a
  second, headless libmpv instance that only opens the file on the first hover.
- **On-screen display** for volume, seeking, speed, delays, modes and messages.
- **Click** the video to pause, **double-click** for fullscreen.
- **Fullscreen** hides the title and control bars; the control bar comes back
  when the pointer nears the bottom edge, and controls and cursor hide again
  after two seconds. The playlist drawer stays as you left it and opens and
  closes with `F6` in fullscreen too.
- **Window size** follows the video (100%, shrunk to fit the screen), or pick
  50% / 100% / 150% / 200% (`Alt+1`..`Alt+4`); **Always on Top** (`Ctrl+T`).
- **About Top Player** (`F1`): version, credits, links, license, and the Qt,
  libmpv and video acceleration details.

### Audio view

Audio files (no video, or only cover art) get their own view instead of a
black screen; seeking, volume and track switching work as for video.

- The **cover** is shown with a soft drop shadow above the **title, artist and
  album**. It comes from, in order: an image you assign (**Audio → Set Custom
  Audio Artwork...**, or **drop an image** on the window while the song
  plays; undone with **Clear Custom Audio Artwork**), art embedded
  in the file (ID3 `APIC`, FLAC / Vorbis `METADATA_BLOCK_PICTURE`, MP4 cover
  atoms), or a `cover` / `folder` / `front` / `album` image next to the track.
- **Audio → Visualizations**: **Album Art Mode** (falls back to the spectrum
  without a cover), **Waveform Visualizer**, **Frequency Spectrum** (mpv's
  `showwaves` / `showfreqs` through `lavfi-complex`), or **Off**, a minimal
  canvas with the track's metadata. A custom cover you set for a track is
  shown even while a visualizer is selected, until you pick a visualization
  again.

### Lyrics

Right-click → **Lyrics**. Lyrics show over the song karaoke style: the line
being sung sits in the middle, large and bold, the lines around it fade with
their distance, and the view scrolls smoothly from line to line over a
blurred tint of the cover. Plain (unsynced) lyrics scroll along with the song.

- **Found automatically**: a `.lrc` (or `.txt`) next to the track with the
  same name, or lyrics downloaded, loaded or synced for it before. They show
  by themselves for songs (**Show Lyrics Automatically for Songs**); `Y`
  shows or hides them, also over videos.
- **Download Lyrics...** (`Alt+Y`) searches [LRCLIB](https://lrclib.net), a
  free, open database of time-synced lyrics that needs no account or key,
  with [lyrics.ovh](https://lyrics.ovh) as a fallback for plain lyrics. The
  title and artist come from the tags, or from an "Artist - Title" file name;
  synced results and the closest length come first, with a preview.
- **Generate Lyrics with AI (Copy Prompt)...**: for songs no site has, a
  structured prompt with the title, artist, album and exact length that asks
  any chat AI (ChatGPT, Claude, Gemini, ...) for a ready `.lrc` file. Load the
  file it gives you, or copy its whole answer and press **Paste AI Answer**.
- **Load Lyrics File...**, or drop a `.lrc` on the window (`.lrc`, `.txt`,
  `.srt`, `.vtt`; UTF-8, UTF-16 or Latin-1).
- **Lyrics Sync Editor...** (`Ctrl+Y`): paste the words (**Edit Text...**),
  play the song and press `Space` as each line starts. `Backspace` undoes,
  `←` / `→` seek 3 s, `[` / `]` nudge a line by 0.1 s (optionally with every
  line after it), `P` plays or pauses, playback can slow to 0.5x, and the
  lyrics view follows every change live. **Save Lyrics** keeps them for the
  track; **Export As...** writes `.lrc` or `.srt`.
- **Lyrics Earlier / Later** (`Alt+[` / `Alt+]`) shift synced lyrics by 0.1 s
  and save the offset.

### Audio Control & Equalizer

**Audio → Audio Control & Equalizer...** (`F7`) changes the sound live:

- **Preamp** (−10 to +10 dB), **Bass** (110 Hz) and **Treble** (3 kHz), −15 to +20 dB.
- A **10-band equalizer** (31 Hz to 16 kHz, ±12 dB) with presets: **Flat**,
  **Bass Boost**, **Club**, **Rock**, **Vocal Clear** and **Cinema/Action**.
- **Loudness Normalization (Night Mode)** evens out loud and quiet passages.
- **Reset to Default** turns everything off.

The effects are one FFmpeg filter graph in mpv's `af` property (`volume`,
`bass`, `treble`, `equalizer`, `dynaudnorm`), so they apply to video and audio
alike, and they are restored on the next start.

### Playlist

The drawer (`F6` or the playlist button) slides in from the right and mirrors
mpv's playlist. Each entry shows its duration, read in the background.

- A PotPlayer-style **bar along the bottom**: move the selected entries to
  the top, up, down or to the bottom, and **ADD** (files, folder, URL, or
  Open Folder to replace the playlist), **DEL** (selected, missing,
  duplicates, clear) and **SORT** menus.
- **Reorder** by dragging, **drop** files from a file manager at any position,
  **double-click** to play, `Del` to remove.
- **Add Files...**, **Add Folder...** ("Added 24 items from Season 1") and
  **Add URL...**.
- **Sort** by name, duration, file path or file size, **Reverse Order**, and
  **Shuffle**; the playing file keeps playing.
- **Remove Missing/Inaccessible Files**, **Remove Duplicates** (keeps the
  playing copy) and **Clear Playlist**.
- A **search field** that filters as you type, matching every word against
  names and paths, without changing the playlist.
- **Save Playlist...** (`Ctrl+S`): extended M3U in UTF-8 with titles and
  durations, as `.m3u8` or `.m3u`.
- **Open Playlist...**: reads `.m3u` / `.m3u8` / `.pls` in the background,
  keeps the titles, skips files that no longer exist and reports what was
  opened ("12 items from Road Trip.m3u8 · 1 missing skipped").
- **Remember Playlist on Exit** and **Resume Playback Position** (both on by
  default): the queue comes back on the next start, reopening the last entry
  paused where you left it.
- **Resizable and expandable**: drag the drawer's edge (the width is
  remembered), or click the expand button (or double-click the edge) to spread
  it over the video.
- While the list has the keyboard, `Up` / `Down`, `Home` / `End` and `Enter`
  move through and play entries; click the video to give the keys back.

### Library

The drawer's second tab keeps folders and playlists inside the player. The
**Folders** and **Playlists** sections fold and unfold with a click, and so
do the folders and playlists in them; an empty section offers to add one.

- **Add Folder to Library...**: browse its subfolders and media files.
  Double-click a folder to play all of it, or a file to play its folder from
  that file on.
- **Save Current Playlist to Library...** stores the queue as a named playlist;
  **Add Playlist File to Library...** references an existing `.m3u` / `.m3u8` /
  `.pls`. Expand a playlist to see its entries and start from any of them.
- Right-click for **Add to Playlist**, **Rename...**, **Replace with Current
  Playlist**, **Refresh** and **Remove from Library** (**Delete Playlist** for
  playlists saved in the library, after confirming). Items can be dragged
  onto the video or the playlist.

### Live TV & Radio

**Live TV & Radio...** (`Ctrl+L`) browses free live streams in two tabs:

- **Live TV**: channels of a country from the [iptv-org](https://github.com/iptv-org/iptv)
  playlists, or **All Countries** at once, each with its country, category and
  resolution.
- **Online Radio**: stations of a country from the community
  [Radio-Browser](https://www.radio-browser.info) directory, most popular
  first, with their tags and bitrate.

Ethiopia is pinned at the top of the country list, followed by **All
Countries** and **every other country** alphabetically. Then:

- A **category** filter (News, Sports, Movies, Music, Kids, ...; tags for radio).
- A **search** box that matches name, country, language and genre as you type.
- **Hide geo-blocked** (on by default) hides channels that only play in their
  own country.
- Channel **logos** load in the background for the rows on screen.
- **Play** (or double-click) to watch; radio opens in the audio view. Right-click
  → **Add to Current Playlist** or **Copy Stream URL**.
- **Reliable playback**: each channel is requested with the referrer and user
  agent its playlist asks for (`#EXTVLCOPT`), or a browser's, since many TV
  servers refuse media players with 403 Forbidden. If a stream fails, the
  channel's **other streams are tried in turn**; a channel with none left is
  greyed out with a clear "offline or not available in your region" message.
- Lists and logos are **cached for 24 hours**, and the cached copy is used
  offline. **Refresh** reloads from the server.

### Subtitles

- Subtitles next to the video, or in a `sub` / `subs` / `subtitles` folder,
  **load automatically** when their names match.
- **Drop** a subtitle file onto the window, or **Load Subtitle File...**.
- **Dual subtitles**: a **Secondary Subtitle Track** shows a second language
  at the top while the primary stays at the bottom.
- Adjust **delay** (0.5 s and 0.1 s steps), **size** and **position**, or hide
  either track.
- **Download Subtitles** (`D`), with no account or API key needed. The title,
  year, season and episode are read from the file name
  ("The.Matrix.1999.1080p.BluRay.mkv" → *The Matrix*, 1999), in your language.
  - **Search by Hash (Exact Match)** finds subtitles made for this exact file
    through OpenSubtitles' [movie hash](https://trac.opensubtitles.org/projects/opensubtitles/wiki/HashSourceCodes)
    (needs the build's key or your own, under **Subtitle Download Settings...**).
  - **Search by Name** uses [podnapisi.net](https://www.podnapisi.net), then
    OpenSubtitles if set up; subtitles named like your file are marked
    **Release match**.
  - **Download & Play** saves to `~/.cache/top-player/subtitles/` (or next to
    the video) and shows it right away. `Esc` cancels a search.
- **Subtitle Sync Editor...** (`Ctrl+Shift+Y`) fixes out-of-sync `.srt` /
  `.vtt` subtitles by ear: select a line, play, and press `Space` when it is
  spoken. Every following line moves with it and keeps its length, so one
  tap often fixes a whole file; single lines can be tapped or nudged too.
  **Save & Load Subtitles** writes `<name>.synced.srt` next to the original
  (or to the cache) and switches to it.

### Tools

- **Cut / Extract Media** (`Ctrl+X`): mark the range while playing with
  `Ctrl+[` (A) and `Ctrl+]` (B); cyan brackets show it on the seekbar. Save it
  as a **lossless stream copy** (instant) or **audio only** (MP3, AAC or FLAC)
  with `ffmpeg`, with progress, cancel, **Open in Player** and **Show in File
  Manager**. **Clear In/Out Points** resets the range.
- **Download from URL** (`Ctrl+Shift+D`): YouTube, TikTok, Instagram, X, Vimeo
  and every site [yt-dlp](https://github.com/yt-dlp/yt-dlp) supports. Pick
  **Best Video + Audio**, **4K**, **1080p**, **720p** or **Audio Only (.mp3)**
  and a folder (`~/Videos` by default), with progress, speed and ETA.
  **Direct Stream** plays the link without saving it.

> [!NOTE]
> The tools use the system's `ffmpeg` and `yt-dlp`, so they are available in
> the AppImage, the distro packages and source builds, not in the Flatpak.

### Settings and files

| What | Where |
| --- | --- |
| Settings | `~/.config/top-player/settings.ini` |
| Last playlist and position | `~/.config/top-player/last_playlist.json` |
| Library | `~/.config/top-player/library.json` |
| Library playlists | `~/.config/top-player/playlists/<name>.m3u8` |
| Audio effects | `~/.config/top-player/audio_settings.json` |
| Live TV & Radio lists and logos | `~/.cache/top-player/streams/` |
| Downloaded subtitles | `~/.cache/top-player/subtitles/` |
| Lyrics (downloaded, synced, assigned) | `~/.config/top-player/lyrics/` |
| Resume positions | `~/.config/top-player/resume.ini` |

Settings from versions before 1.0 are copied over from
`~/.config/potplayer-linux` on the first start.

<p align="right"><a href="#top">↑ Back to top</a></p>

---

## Install

Download the package for your system from the
**[latest release](https://github.com/Henok-Enyew/top-player-linux/releases/latest)**:

| Distro | Download | Install |
| --- | --- | --- |
| **Ubuntu 22.04 / 24.04 / 26.04** | `top-player_*+ubuntu<version>_amd64.deb` | `sudo apt install ./top-player_*.deb` |
| **Debian 13** | `top-player_*+debian13_amd64.deb` | `sudo apt install ./top-player_*.deb` |
| **Fedora** | `top-player-*.fc<version>.x86_64.rpm` | `sudo dnf install ./top-player-*.rpm` |
| **openSUSE Tumbleweed** | `top-player-*.tw.x86_64.rpm` | `sudo zypper install ./top-player-*.rpm` |
| **openSUSE Leap 16.0** | `top-player-*.lp160.x86_64.rpm` | `sudo zypper install ./top-player-*.rpm` |
| **Arch Linux** (and Manjaro, EndeavourOS) | `top-player-*-x86_64.pkg.tar.zst` | `sudo pacman -U ./top-player-*.pkg.tar.zst` |
| **Any distro** | `Top_Player-*-x86_64.AppImage` | `chmod +x Top_Player-*.AppImage && ./Top_Player-*.AppImage` |
| **Any distro** | `Top_Player-*-x86_64.flatpak` | `flatpak install --user Top_Player-*.flatpak` (needs the Flathub remote for the KDE runtime) |

Pick the `.deb` built for your exact release: each one links against that
release's Qt and libmpv. Linux Mint 21 / 22 and Pop!_OS use the Ubuntu 22.04 /
24.04 package. The package manager pulls in Qt and libmpv.

Each release also includes a `SHA256SUMS` file to verify the downloads.

> [!TIP]
> The AppImage is built on Ubuntu 24.04 and needs glibc 2.39 or newer
> (Ubuntu 24.04+, Fedora 40+, Debian 13+). On older systems, use your
> distro's package or the Flatpak.
>
> On Ubuntu 22.04, FFmpeg 4.4 has no Spectrum visualization; the other
> visualizations work.

**Optional tools:** `ffmpeg` for Cut / Extract Media (`sudo dnf install ffmpeg`
from RPM Fusion, `sudo apt install ffmpeg`, `sudo zypper install ffmpeg` or
`sudo pacman -S ffmpeg`) and `yt-dlp` for Download from URL
(`sudo dnf install yt-dlp`, `sudo apt install yt-dlp`, `sudo pacman -S yt-dlp`
or `pip install --user yt-dlp`). The `.deb` and Arch packages suggest both.

<p align="right"><a href="#top">↑ Back to top</a></p>

---

## Keyboard and mouse

Right-click anywhere for the full menu: **Playback**, **Video**, **Audio**,
**Subtitles**, **Lyrics**, **Tools**, **Window** and **Help**. Every item is bound to an
mpv property or command, and check marks reflect mpv's live state.

<table>
<tr><td valign="top">

**Playback**

| Input | Action |
| --- | --- |
| `Space`, click video | Play / pause |
| `Left` / `Right` | Seek −5 s / +5 s |
| `Ctrl+Left` / `Ctrl+Right` | Seek −30 s / +30 s |
| `Shift+Left` / `Shift+Right` | Seek −60 s / +60 s |
| `PgUp` / `PgDn` | Previous / next file |
| `Up` / `Down` | Volume ±2 |
| Mouse wheel | Volume ±5 |
| Drag the video sideways | Seek (swipe), `Esc` cancels |
| Scroll sideways (touchpad, tilt wheel) | Seek ±5 s |
| `M` | Mute |
| `C` / `X` / `Z` | Speed +0.1 / −0.1 / reset |
| `Ctrl+Shift+L` | Loop file |
| `Ctrl+.` / `Ctrl+,` | Audio delay ±0.1 s |
| Media keys | Play, pause, stop, previous, next (also through MPRIS, from the desktop) |

**Window**

| Input | Action |
| --- | --- |
| Double-click video, `Enter` | Toggle fullscreen |
| `Esc` | Leave fullscreen |
| `F6` | Show / hide the playlist |
| `Ctrl+T` | Always on top |
| `Alt+1`..`Alt+4` | Window size 50–200% |
| Drag video or title bar | Move window |
| Drag window edge | Resize window |
| Right-click | Context menu |
| `F1` | About Top Player |
| `Q` | Quit |

</td><td valign="top">

**Subtitles**

| Input | Action |
| --- | --- |
| `D` | Download subtitles |
| `]` / `[` | Delay ±0.5 s |
| `.` / `,` | Delay ±0.1 s |
| `Alt+L` | Next subtitle |
| `Alt+Shift+L` | Next secondary subtitle |
| `Alt+H` | Show / hide subtitles |
| `Alt+Shift+H` | Show / hide secondary |
| `Alt+Up` / `Alt+Down` | Move up / down |
| `Alt+PgUp` / `Alt+PgDn` | Larger / smaller |
| `Ctrl+Shift+Y` | Subtitle sync editor |

**Lyrics**

| Input | Action |
| --- | --- |
| `Y` | Show / hide lyrics |
| `Alt+Y` | Download lyrics |
| `Ctrl+Y` | Lyrics sync editor |
| `Alt+[` / `Alt+]` | Lyrics earlier / later 0.1 s |
| `Enter` / `Esc` | Resume / start over (resume card) |

**Tools and dialogs**

| Input | Action |
| --- | --- |
| `Ctrl+O` | Open files |
| `Ctrl+S` | Save playlist |
| `Ctrl+L` | Live TV & Radio |
| `F7` | Audio Control & Equalizer |
| `Ctrl+[` / `Ctrl+]` | Set cut In / Out point |
| `Ctrl+X` | Cut / extract media |
| `Ctrl+Shift+D` | Download from URL |
| `Ctrl+E` | Screenshot |
| `Ctrl+D` | Deinterlace |

</td></tr>
</table>

<p align="right"><a href="#top">↑ Back to top</a></p>

---

## Building from source

### Dependencies

| Distro | Packages |
| --- | --- |
| Ubuntu / Debian | `build-essential cmake ninja-build pkg-config qt6-base-dev libqt6opengl6-dev libgl-dev libmpv-dev zlib1g-dev` |
| Fedora | `gcc-c++ cmake ninja-build pkgconf-pkg-config qt6-qtbase-devel mesa-libGL-devel mpv-devel zlib-devel` |
| openSUSE | `gcc-c++ cmake ninja pkgconf qt6-base-devel Mesa-libGL-devel mpv-devel zlib-devel` |
| Arch Linux | `base-devel cmake ninja qt6-base mpv zlib` |

### Compile and run

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/top-player /path/to/video.mkv [more files to queue...]
```

`cmake --install build` installs the binary with its desktop entry, icon and
AppStream metadata (app ID `org.github.topplayer`).

### Tests

The integration tests drive the real window through its buttons, hotkeys,
dialogs and drop zone. They need Qt Test (part of `qt6-base-dev` /
`qt6-qtbase-devel`), `ffmpeg` to generate test media, and a display such as Xvfb:

```sh
cmake -S . -B build -G Ninja -DTOPPLAYER_BUILD_TESTS=ON
cmake --build build
xvfb-run -a ctest --test-dir build --output-on-failure
```

### Packaging

| Format | Recipe | Build locally |
| --- | --- | --- |
| AppImage | [`packaging/appimage/build-appimage.sh`](packaging/appimage/build-appimage.sh) | `VERSION=1.0.3 packaging/appimage/build-appimage.sh` (also needs `qmake6`, optionally `qt6-wayland`) |
| Flatpak | [`org.github.topplayer.yaml`](org.github.topplayer.yaml) (KDE 6.11 runtime, builds libmpv) | `flatpak-builder --user --install --force-clean build-flatpak org.github.topplayer.yaml` |
| `.deb` (Debian, Ubuntu) | [`packaging/debian/`](packaging/debian) | `VERSION=1.0.3 packaging/debian/build-deb.sh` (needs `devscripts` and `equivs`; builds for the release it runs on) |
| RPM (Fedora, openSUSE) | [`packaging/rpm/top-player.spec`](packaging/rpm/top-player.spec) | `VERSION=1.0.3 packaging/rpm/build-rpm.sh` (needs `rpm-build`) |
| Arch Linux | [`packaging/arch/PKGBUILD`](packaging/arch/PKGBUILD) | `VERSION=1.0.3 packaging/arch/build-arch.sh` (needs `base-devel`) |

Packages land in `dist/`.

<details>
<summary><b>OpenSubtitles API key (optional)</b></summary>

To build in a default key for exact (hash) subtitle matches, add it as the
repository secret `OPENSUBTITLES_API_KEY`; the release workflow passes it to
every build. Locally, set `$OPENSUBTITLES_API_KEY` or put the key in an
untracked `opensubtitles-api-key.txt` before running CMake. Without one,
search by name still works through podnapisi.net, and users can enter a key
of their own.

</details>

<p align="right"><a href="#top">↑ Back to top</a></p>

---

## Releases

**[⬇ Latest release](https://github.com/Henok-Enyew/top-player-linux/releases/latest)** ·
[All releases](https://github.com/Henok-Enyew/top-player-linux/releases)

| Version | Date | Highlights |
| --- | --- | --- |
| [**1.0.3**](https://github.com/Henok-Enyew/top-player-linux/releases/tag/v1.0.3) | 2026-10-04 | Resume or start over when reopening a file, synced lyrics (LRCLIB download, karaoke-style view, AI prompt, load from file), tap-to-sync editor for lyrics and subtitles, custom audio artwork shows over visualizers and can be dropped on the window |
| [1.0.2](https://github.com/Henok-Enyew/top-player-linux/releases/tag/v1.0.2) | 2026-10-03 | Media keys over MPRIS, swipe / sideways-scroll seeking, autoplay on open, opening a folder replaces the playlist, clickable library sections, new window buttons and a PotPlayer-style playlist bar, lighter playback |
| [1.0.1](https://github.com/Henok-Enyew/top-player-linux/releases/tag/v1.0.1) | 2026-10-03 | Live TV fix (referrer / user agent, fallback streams), every country with categories and Hide geo-blocked, playlist in fullscreen, shuffle / repeat / aspect buttons, better Open Playlist |
| [**1.0.0**](https://github.com/Henok-Enyew/top-player-linux/releases/tag/v1.0.0) | 2026-10-03 | Renamed to Top Player with a new icon and cyan skin, About dialog, click to pause, double-click for fullscreen, fast folder loading, 10-band equalizer |
| [0.2.0](https://github.com/Henok-Enyew/top-player-linux/releases/tag/v0.2.0) | 2026-10-03 | Subtitle download, audio view with cover art and visualizations, playlist manager with sorting, M3U and session restore |
| [0.1.0](https://github.com/Henok-Enyew/top-player-linux/releases/tag/v0.1.0) | 2026-10-02 | First release |

<details>
<summary><b>Publishing a release</b> (maintainers)</summary>

The [Release workflow](.github/workflows/release.yml) builds the AppImage,
Flatpak, the `.deb` packages (Ubuntu 22.04, 24.04, 26.04, Debian 13), the
RPMs (Fedora, openSUSE Tumbleweed, Leap 16.0) and the Arch package, and checks
that each one installs and starts. It runs on pull requests that touch packaging and can be
started from the Actions tab; both only build. Pushing a `v*` tag also
publishes a GitHub Release with the packages and `SHA256SUMS`:

1. Set `VERSION` in `project()` in `CMakeLists.txt` (the tag must match it),
   add a `<release>` entry to
   [`packaging/linux/org.github.topplayer.metainfo.xml`](packaging/linux/org.github.topplayer.metainfo.xml),
   and commit.
2. `git tag v1.0.3 && git push origin v1.0.3`

A tag with a suffix such as `v1.0.3-rc1` is published as a pre-release.

</details>

<p align="right"><a href="#top">↑ Back to top</a></p>

---

## License

Released under the [MIT License](LICENSE).
Copyright © 2026 Henok Enyew Andargie and Top Player contributors.

<div align="center">

**[GitHub](https://github.com/Henok-Enyew/top-player-linux)** ·
**[Telegram](https://t.me/enoch90s)** ·
**[Report an issue](https://github.com/Henok-Enyew/top-player-linux/issues)**

<sub>Live TV channels come from <a href="https://github.com/iptv-org/iptv">iptv-org</a> and radio stations from
<a href="https://www.radio-browser.info">Radio-Browser</a>; Top Player hosts no streams.</sub>

</div>
