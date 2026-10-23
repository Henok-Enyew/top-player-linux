# Memory audit (1.3.0)

Where Top Player's memory went in 1.2.1, what changed in 1.3.0, and how to
measure it yourself.

## Results

Measured on the same machine and the same files, 1.2.1 against 1.3.0. Each run
starts the player with a fresh configuration, opens the media, moves the
pointer along the seekbar (seek previews), then waits 25 seconds. The figures
are the memory the player allocated itself (`Pss_Anon` in
`/proc/<pid>/smaps_rollup`): the heap, thread stacks and decoded frames,
without the code of the shared libraries. Each figure is the mean of two runs.

| Scenario | Moment | 1.2.1 | 1.3.0 | Change |
| --- | --- | ---: | ---: | ---: |
| 720p video (60 s, H.264) | playing | 174 MB | 149 MB | −14 % |
| | after seek previews | 206 MB | 172 MB | −16 % |
| | 25 s later | 206 MB | 152 MB | **−26 %** |
| Folder of 25 songs with covers | opened | 283 MB | 158 MB | −44 % |
| | 25 s later | 385 MB | 126 MB | **−67 %** |
| Threads | 25 s after a video | 48 | 28 | −20 |
| | 25 s after songs | 38 | 22 | −16 |

The test machine draws with Mesa's software OpenGL (llvmpipe), which itself
accounts for roughly 100 MB of the figures above. On a real GPU that memory sits
in the driver, so the share the player saves there is larger still.

## What used the memory

Measured with [heaptrack](https://github.com/KDE/heaptrack) and
`/proc/self/smaps_rollup`:

1. **Freed memory that was never given back.** Decoding runs on many
   threads (mpv's demuxer, decoders, audio output, and Qt's). glibc gives busy
   threads malloc arenas of their own, up to eight per CPU core, and keeps the
   memory freed in each one for that arena. After a few songs the player held
   two to three times the memory it actually used. Setting
   `MALLOC_ARENA_MAX=2` alone took a folder of songs from 361 MB to 198 MB.
2. **Three helper players running for the whole session.** Besides the
   player itself, three headless libmpv instances were started with the
   window and never stopped: one for seek previews, one reading playlist
   durations, and one extracting embedded covers. Each held its threads and
   buffers, and the duration reader kept the last file it read open, paused,
   for the rest of the run.
3. **Caches counted in entries, not bytes.** Covers were cached 16 at a time
   at up to 1024×1024 (64 MB), the built-in artwork gallery 6 at a time
   (24 MB), and seek previews 300 at a time (44 MB).
4. **Large stream buffers.** The demuxer cache was allowed 150 MB ahead and
   75 MB behind per stream.

## What changed

| Change | Where |
| --- | --- |
| glibc malloc arenas capped at 2 (unless `MALLOC_ARENA_MAX` is set), and freed memory handed back to the system (`malloc_trim`) after each file closes and after a helper player stops | [`src/MemoryUsage.cpp`](../src/MemoryUsage.cpp), `main()` |
| Seek preview decoder starts on the first hover and stops 20 s after the last one, or as soon as nothing can be previewed | [`src/ThumbnailGenerator.cpp`](../src/ThumbnailGenerator.cpp) |
| Duration reader starts when there are files to read and stops 3 s after its queue runs empty | [`src/MediaProber.cpp`](../src/MediaProber.cpp) |
| Cover extractor starts on a song's first cover and stops 15 s after the last one | [`src/ArtworkExtractor.cpp`](../src/ArtworkExtractor.cpp) |
| Helpers stop in a thread of their own, so one stuck on an unresponsive mount can't freeze the window | `destroyMpvDetached()` in [`src/MpvHelpers.h`](../src/MpvHelpers.h) |
| Helper demuxers buffer 4 to 8 MB instead of mpv's default 150 MB | the three helpers above |
| Caches budgeted in bytes: covers 16 MB, seek previews 16 MB, gallery artwork 12 MB | the same files, [`src/ArtworkGallery.cpp`](../src/ArtworkGallery.cpp) |
| Stream cache 64 MB ahead and 32 MB behind (still over a minute of 1080p) | [`src/MpvWidget.cpp`](../src/MpvWidget.cpp) |
| No file lookups on the UI thread when a file starts (a mount that stopped answering froze the window) | [`src/MainWindow.cpp`](../src/MainWindow.cpp) |

Nothing a user sees was given up: previews, durations and covers appear as
before. The first preview after a long pause takes a moment longer while its
decoder starts.

## Checking it yourself

**Help → About Top Player** (`F1`) shows the memory the player uses, updated
every two seconds: first its own memory (the figure in the table above), then
in brackets the total including shared libraries.

From a terminal, with the player running:

```sh
grep -E '^(Rss|Pss_Anon)' /proc/$(pidof top-player)/smaps_rollup
ls /proc/$(pidof top-player)/task | wc -l   # threads
```

To compare with glibc's default arenas, start the player with
`MALLOC_ARENA_MAX=8 top-player`.
