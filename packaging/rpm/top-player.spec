# Builds on Fedora and openSUSE (Tumbleweed and Leap). Built in CI by
# .github/workflows/release.yml through packaging/rpm/build-rpm.sh, which
# replaces Version with the release tag. Local build from a checkout:
#
#   VERSION=1.0.5 packaging/rpm/build-rpm.sh
Name:           top-player
Version:        1.0.5
Release:        1%{?dist}
Summary:        High-performance, lightweight native media player

License:        MIT
URL:            https://github.com/Henok-Enyew/top-player-linux
Source0:        %{name}-%{version}.tar.gz

%if 0%{?suse_version}
# openSUSE's %%cmake, %%cmake_build and %%cmake_install use this generator.
%global __builder ninja
%endif

BuildRequires:  cmake >= 3.16
BuildRequires:  gcc-c++
%if 0%{?suse_version}
BuildRequires:  ninja
%else
BuildRequires:  ninja-build
%endif
BuildRequires:  pkgconfig(mpv)
BuildRequires:  cmake(Qt6Core)
# MPRIS: the media keys and the desktop's media controls.
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Concurrent)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6OpenGL)
BuildRequires:  cmake(Qt6OpenGLWidgets)
BuildRequires:  pkgconfig(gl)
BuildRequires:  pkgconfig(zlib)
BuildRequires:  pkgconfig(xcb)
BuildRequires:  desktop-file-utils
BuildRequires:  /usr/bin/appstreamcli

Requires:       hicolor-icon-theme

%description
Top Player is a high-performance, lightweight native media player for Linux
powered by Qt6 and libmpv: a borderless cyan-on-obsidian skin with an on-screen display, a seekbar with preview thumbnails, a
playlist manager, an audio view with cover art and visualizations, dual
subtitles, a 10-band equalizer and subtitle downloads from OpenSubtitles.com.

%prep
%autosetup -n %{name}-%{version}

%build
%if 0%{?suse_version}
%cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
%else
%cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
%endif
%cmake_build

%install
%cmake_install
# The license is packaged with %%license instead.
rm -rf %{buildroot}%{_datadir}/licenses/%{name}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/org.github.topplayer.desktop
appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/org.github.topplayer.metainfo.xml

%files
%license LICENSE
%doc README.md
%{_bindir}/top-player
%{_datadir}/applications/org.github.topplayer.desktop
%{_datadir}/metainfo/org.github.topplayer.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/org.github.topplayer.svg

%changelog
* Tue Oct 06 2026 Henok Enyew Andargie - 1.0.5-1
- Volume, mute, shuffle and repeat are kept between runs
- Spotify links (songs, albums, playlists) download as tagged MP3s
- Lyrics: scroll through them, click a timed line to play from it
- Lyrics appearance: font, size, spacing, alignment and colors
- Mini player that stays on top and on every workspace (X11)
- Songs always start from the beginning; videos still resume
- Fullscreen controls float over the video instead of moving it

* Mon Oct 05 2026 Henok Enyew Andargie - 1.0.4-1
- Packages for Debian, Ubuntu, openSUSE and Arch Linux
- Builds with Qt 6.2

* Sun Oct 04 2026 Henok Enyew Andargie - 1.0.3-1
- Ask to resume or start over when reopening a file
- Synced lyrics: LRCLIB download, karaoke-style view, load from file
- AI prompt for lyrics no site has, with Paste AI Answer
- Tap-to-sync editor for lyrics and subtitles
- Custom audio artwork shows over visualizers; drop an image to set it

* Sat Oct 03 2026 Henok Enyew Andargie - 1.0.2-1
- Media keys and desktop media controls through MPRIS
- Drag or scroll sideways over the video to seek
- Opened media plays right away; opening a folder replaces the playlist
- Library folders and playlists open with a click
- New window buttons and a PotPlayer-style playlist bar
- Fewer position updates and repaints during playback

* Sat Oct 03 2026 Henok Enyew Andargie - 1.0.1-1
- Live TV plays channels that need a referrer or user agent, and tries a
  channel's other streams when one fails
- Every country, All Countries, a category filter and Hide geo-blocked
- The playlist opens in fullscreen too
- Shuffle, repeat (all / one) and aspect (fit / 16:9 / 100%) buttons
- Opening a playlist keeps its titles, skips missing files and says so

* Sat Oct 03 2026 Henok Enyew Andargie - 1.0.0-1
- Renamed to Top Player, new icon and cyan skin
- About dialog, click to pause, double-click for fullscreen
- Asynchronous folder scanning and batched playlist loading
- 10-band audio equalizer

* Sat Oct 03 2026 Top Player contributors - 0.2.0-1
- Subtitle search and download from OpenSubtitles.com
- Audio view with cover art, metadata and visualizations
- Playlist manager with folders, sorting, M3U and session restore

* Fri Oct 02 2026 Top Player contributors - 0.1.0-1
- Initial package
