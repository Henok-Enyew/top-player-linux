find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(tst_transport tst_transport.cpp)
target_link_libraries(tst_transport PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_transport COMMAND tst_transport)

add_executable(tst_emptystate tst_emptystate.cpp)
target_link_libraries(tst_emptystate PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_emptystate COMMAND tst_emptystate)

add_executable(tst_playlist tst_playlist.cpp)
target_link_libraries(tst_playlist PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_playlist COMMAND tst_playlist)

add_executable(tst_audio tst_audio.cpp)
target_link_libraries(tst_audio PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_audio COMMAND tst_audio)

add_executable(tst_subtitles tst_subtitles.cpp)
target_link_libraries(tst_subtitles PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_subtitles COMMAND tst_subtitles)

add_executable(tst_library tst_library.cpp)
target_link_libraries(tst_library PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_library COMMAND tst_library)

add_executable(tst_livestreams tst_livestreams.cpp)
target_link_libraries(tst_livestreams PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_livestreams COMMAND tst_livestreams)

add_executable(tst_equalizer tst_equalizer.cpp)
target_link_libraries(tst_equalizer PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_equalizer COMMAND tst_equalizer)

add_executable(tst_tools tst_tools.cpp)
target_link_libraries(tst_tools PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_tools COMMAND tst_tools)

add_executable(tst_lyrics tst_lyrics.cpp)
target_link_libraries(tst_lyrics PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_lyrics COMMAND tst_lyrics)

add_executable(tst_resume tst_resume.cpp)
target_link_libraries(tst_resume PRIVATE top-player-core Qt6::Test)
add_test(NAME tst_resume COMMAND tst_resume)
