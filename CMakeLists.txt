cmake_minimum_required(VERSION 3.16)

project(top-player
    VERSION 1.0.8
    DESCRIPTION "Top Player: a lightweight Qt6 + libmpv media player for Linux"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Concurrent Gui Widgets Network OpenGL OpenGLWidgets)
# MPRIS (media keys, the desktop's media controls) needs D-Bus.
find_package(Qt6 6.2 QUIET OPTIONAL_COMPONENTS DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
# Keeps the mini player on every workspace under X11 (optional).
pkg_check_modules(XCB QUIET IMPORTED_TARGET xcb)
# Unpacks the ZIP archives subtitle sites hand out.
find_package(ZLIB REQUIRED)

# OpenSubtitles.com needs an API key. One built in here is used unless the
# user enters their own in the subtitle download settings. Release builds
# take it from $OPENSUBTITLES_API_KEY or an untracked opensubtitles-api-key.txt.
set(_opensubtitles_key "$ENV{OPENSUBTITLES_API_KEY}")
if(_opensubtitles_key STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/opensubtitles-api-key.txt")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/opensubtitles-api-key.txt" _opensubtitles_key LIMIT_COUNT 1)
endif()
set(TOPPLAYER_OPENSUBTITLES_API_KEY "${_opensubtitles_key}" CACHE STRING "Built-in OpenSubtitles.com API key")

option(TOPPLAYER_BUILD_TESTS "Build the integration tests (need an X or Wayland display)" OFF)

# Everything but main(), so the tests can drive the real window.
add_library(top-player-core OBJECT
    resources/skin.qrc
    src/AboutDialog.cpp
    src/AboutDialog.h
    src/ArtworkExtractor.cpp
    src/ArtworkExtractor.h
    src/AudioArtwork.cpp
    src/AudioArtwork.h
    src/AudioControlDialog.cpp
    src/AudioControlDialog.h
    src/AudioController.cpp
    src/AudioController.h
    src/AudioEffects.cpp
    src/AudioEffects.h
    src/AudioView.cpp
    src/AudioView.h
    src/ControlBar.cpp
    src/ControlBar.h
    src/DownloadQueue.cpp
    src/DownloadQueue.h
    src/EmptyStateWidget.cpp
    src/EmptyStateWidget.h
    src/HostTools.cpp
    src/HostTools.h
    src/Icons.cpp
    src/Icons.h
    src/LibraryPanel.cpp
    src/LibraryPanel.h
    src/LiveStreamDialog.cpp
    src/LiveStreamDialog.h
    src/Lyrics.cpp
    src/Lyrics.h
    src/LyricsClient.cpp
    src/LyricsClient.h
    src/LyricsController.cpp
    src/LyricsController.h
    src/LyricsDialogs.cpp
    src/LyricsDialogs.h
    src/LyricsView.cpp
    src/LyricsView.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MediaCutter.cpp
    src/MediaCutter.h
    src/MediaCutterDialog.cpp
    src/MediaCutterDialog.h
    src/MediaDownloader.cpp
    src/MediaDownloader.h
    src/MediaDownloaderDialog.cpp
    src/MediaDownloaderDialog.h
    src/MediaFiles.cpp
    src/MediaFiles.h
    src/MediaLibrary.cpp
    src/MediaLibrary.h
    src/MediaProber.cpp
    src/MediaProber.h
    src/MpvHelpers.h
    src/MpvWidget.cpp
    src/MpvWidget.h
    src/OpenSubtitlesClient.cpp
    src/OpenSubtitlesClient.h
    src/OsdWidget.cpp
    src/OsdWidget.h
    src/PlayerMenu.cpp
    src/PlayerMenu.h
    src/PlaylistController.cpp
    src/PodnapisiClient.cpp
    src/PodnapisiClient.h
    src/PlaylistController.h
    src/PlaylistDrawer.cpp
    src/PlaylistDrawer.h
    src/PlaylistOps.cpp
    src/PlaylistOps.h
    src/PlaylistSession.cpp
    src/PlaylistSession.h
    src/ResumeManager.cpp
    src/ResumeManager.h
    src/ResumePrompt.cpp
    src/ResumePrompt.h
    src/SeekBar.cpp
    src/SeekBar.h
    src/StreamCatalog.cpp
    src/StreamCatalog.h
    src/SubtitleDownloadDialog.cpp
    src/SubtitleDownloadDialog.h
    src/SubtitleFinder.cpp
    src/SubtitleFinder.h
    src/SubtitleHasher.cpp
    src/SubtitleHasher.h
    src/SubtitleSearch.cpp
    src/SubtitleSearch.h
    src/SyncEditorDialog.cpp
    src/SyncEditorDialog.h
    src/Theme.cpp
    src/Theme.h
    src/ThumbnailGenerator.cpp
    src/ThumbnailGenerator.h
    src/ThumbnailPopup.cpp
    src/ThumbnailPopup.h
    src/TimeFormat.h
    src/TitleBar.cpp
    src/TitleBar.h
    src/WindowPin.cpp
    src/WindowPin.h
    src/ZipArchive.cpp
    src/ZipArchive.h
)

target_include_directories(top-player-core PUBLIC src)

target_compile_definitions(top-player-core PUBLIC
    APP_VERSION="${PROJECT_VERSION}"
    TOPPLAYER_OPENSUBTITLES_API_KEY="${TOPPLAYER_OPENSUBTITLES_API_KEY}"
    QT_NO_KEYWORDS
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(top-player-core PUBLIC -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(top-player-core PUBLIC
    Qt6::Core
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    PkgConfig::MPV
    ZLIB::ZLIB
)

if(TARGET Qt6::DBus)
    target_sources(top-player-core PRIVATE src/MprisService.cpp src/MprisService.h)
    target_compile_definitions(top-player-core PUBLIC TOPPLAYER_HAVE_DBUS)
    target_link_libraries(top-player-core PUBLIC Qt6::DBus)
else()
    message(STATUS "Qt6 DBus not found: building without MPRIS (media keys)")
endif()

if(XCB_FOUND)
    target_compile_definitions(top-player-core PUBLIC TOPPLAYER_HAVE_XCB)
    target_link_libraries(top-player-core PUBLIC PkgConfig::XCB)
else()
    message(STATUS "xcb not found: the mini player can't pin itself to every X11 workspace")
endif()

add_executable(top-player src/main.cpp)
target_link_libraries(top-player PRIVATE top-player-core)

if(TOPPLAYER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS top-player RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/linux/org.github.topplayer.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/linux/org.github.topplayer.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES packaging/linux/org.github.topplayer.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/top-player)
