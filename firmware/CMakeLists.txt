# Host build: the portable core, the simulator library and the unit tests.
# The ESP32 firmware has its own ESP-IDF project in esp32/.
cmake_minimum_required(VERSION 3.16)
project(hermes_gadget_firmware CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(core)

# Shared library loaded by the Python simulator host (hermes-gadget sim).
add_library(hgsim SHARED sim/src/hgsim.cpp)
target_include_directories(hgsim PUBLIC sim/include)
target_link_libraries(hgsim PRIVATE hg_core)
set_target_properties(hgsim PROPERTIES CXX_VISIBILITY_PRESET hidden)
if(MSVC)
  target_compile_options(hgsim PRIVATE /utf-8)
  # Keep the DLL next to the build root regardless of configuration.
  foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set_target_properties(hgsim PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/bin)
  endforeach()
endif()

option(HG_BUILD_TESTS "Build the core unit tests" ON)
if(HG_BUILD_TESTS)
  enable_testing()
  add_executable(hg_core_tests tests/main.cpp tests/test_basics.cpp tests/test_app.cpp tests/test_timer.cpp tests/test_face.cpp tests/test_ring.cpp tests/test_clock_face.cpp tests/test_widget_face.cpp tests/test_qr.cpp tests/test_i18n.cpp tests/test_power.cpp tests/test_ws185.cpp tests/test_axs5106.cpp tests/test_speaker_pa.cpp tests/test_speaker_queue.cpp tests/test_speaker_underruns.cpp tests/test_band_flush.cpp tests/test_frame_stats.cpp tests/test_shared_reply.cpp tests/test_tag_scanner.cpp tests/test_battery_curve.cpp tests/test_mic_power.cpp tests/test_wifi_retry.cpp tests/test_wifi_power.cpp tests/test_wifi_pick.cpp tests/test_wifi_fast.cpp tests/test_wifi_networks.cpp tests/test_ws_link.cpp tests/test_link_diag.cpp tests/test_log_ring.cpp tests/test_tailnet_watch.cpp tests/test_i2c_scan.cpp tests/test_qmi8658.cpp tests/test_bmi270.cpp tests/test_pcf85063.cpp tests/test_rx8130.cpp tests/test_camera_rgb565.cpp tests/test_cores3_control.cpp tests/test_camera_hal.cpp drivers/axp2101.cpp drivers/cores3.cpp drivers/sticks3.cpp drivers/stopwatch.cpp drivers/stc8_battery.cpp drivers/qmi8658.cpp drivers/bmi270.cpp drivers/pcf85063.cpp drivers/rx8130.cpp)
  target_include_directories(hg_core_tests PRIVATE drivers)
  target_sources(hg_core_tests PRIVATE tests/test_cardputer.cpp tests/test_cores3_panel.cpp)
  target_link_libraries(hg_core_tests PRIVATE hg_core)
  if(MSVC)
    target_compile_options(hg_core_tests PRIVATE /utf-8)
  endif()
  add_test(NAME hg_core_tests COMMAND hg_core_tests)

  # Compile the real board factory: source-pattern checks cannot catch a
  # missing color-order setting inherited from LcdConfig's default.
  add_executable(hg_cores3_board_tests tests/main.cpp tests/test_cores3_board.cpp esp32/main/board.cpp)
  target_include_directories(hg_cores3_board_tests PRIVATE tests/cores3 esp32/main drivers)
  if(MSVC)
    target_compile_options(hg_cores3_board_tests PRIVATE /utf-8)
  endif()
  add_test(NAME hg_cores3_board_tests COMMAND hg_cores3_board_tests)

  # The CJK font's tests, against a core built with it (docs/cjk.md), whatever HG_CJK_FONT says.
  hg_core_library(hg_core_cjk ON)
  add_executable(hg_cjk_tests tests/main.cpp tests/test_cjk.cpp)
  target_link_libraries(hg_cjk_tests PRIVATE hg_core_cjk)
  if(MSVC)
    target_compile_options(hg_cjk_tests PRIVATE /utf-8)
  endif()
  add_test(NAME hg_cjk_tests COMMAND hg_cjk_tests)

  # The e-paper port's mode logic: LiveState (what the notice line shows for a
  # turn and a lost connection) and TalkHold (BOOT's long press) are
  # hardware-free by design, so they test here rather than on a board.
  add_executable(hg_epaper_mode_tests tests/main.cpp tests/test_epaper_live_mode.cpp)
  target_include_directories(hg_epaper_mode_tests PRIVATE esp32/main)
  target_link_libraries(hg_epaper_mode_tests PRIVATE hg_core)
  if(MSVC)
    target_compile_options(hg_epaper_mode_tests PRIVATE /utf-8)
  endif()
  target_sources(hg_epaper_mode_tests PRIVATE esp32/main/modes/live_state.cpp)
  add_test(NAME hg_epaper_mode_tests COMMAND hg_epaper_mode_tests)

  # The offline notes' portable half: note ids, note.json, the multipart
  # request, the response rules, and the card tree against a temp directory.
  # notes_store and notes_capture do their file work with POSIX open/rename and
  # dirent, which the device and every Unix host have and MSVC does not: the
  # card-tree tests are skipped on Windows rather than faked.
  if(NOT MSVC)
    add_executable(hg_epaper_notes_tests tests/main.cpp tests/test_epaper_notes.cpp)
    target_include_directories(hg_epaper_notes_tests PRIVATE esp32/main)
    target_link_libraries(hg_epaper_notes_tests PRIVATE hg_core)
    target_sources(hg_epaper_notes_tests PRIVATE esp32/main/notes/notes_queue.cpp esp32/main/notes/notes_store.cpp
                                                     esp32/main/notes/notes_capture.cpp)
    add_test(NAME hg_epaper_notes_tests COMMAND hg_epaper_notes_tests)
  endif()

  # A dev tool, not a test: POSTs the exact bytes the firmware uploads to a
  # live endpoint (see the header comment), so the device's half of the
  # contract can be checked against plugin/notes_ingest.py from any host.
  # It speaks BSD sockets (arpa/inet.h), so like the card-tree tests it is a
  # Unix-host tool: Windows builds skip it.
  if(NOT MSVC)
    add_executable(hg_notes_request tools/notes_request.cpp)
    target_include_directories(hg_notes_request PRIVATE esp32/main)
    target_link_libraries(hg_notes_request PRIVATE hg_core)
    target_sources(hg_notes_request PRIVATE esp32/main/notes/notes_queue.cpp)
  endif()

  # TailTiny's Noise channel AEAD (esp32/components/TAILTINY.md) against RFC 8439. The vendored
  # C uses GCC builtins, so not on MSVC.
  if(NOT MSVC)
    enable_language(C)
    set(HG_WG_REFC esp32/components/wireguard_lwip/src/crypto/refc)
    add_executable(hg_tailtiny_tests tests/main.cpp tests/test_tailtiny_aead.cpp
      ${HG_WG_REFC}/chacha20poly1305.c ${HG_WG_REFC}/chacha20.c ${HG_WG_REFC}/poly1305-donna.c
      esp32/components/wireguard_lwip/src/crypto.c)
    target_include_directories(hg_tailtiny_tests PRIVATE ${HG_WG_REFC} esp32/components/tailtiny/src)
    add_test(NAME hg_tailtiny_tests COMMAND hg_tailtiny_tests)
  endif()
endif()
