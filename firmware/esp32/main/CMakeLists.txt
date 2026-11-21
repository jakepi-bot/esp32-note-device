if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/wifi_setup.html" HG_WIFI_SETUP_HTML)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/wifi_setup.html")
    configure_file(wifi_setup_page.hpp.in wifi_setup_page.hpp @ONLY)
endif()

# Which application this component builds.
#   hermes  the library app (the default): hg::App, the core UI, the web link
#   notes   the notes-only app: the hardware ports and the notes stack, and
#           nothing else (docs/notes-only-firmware-plan.md section 3, option B).
#           The PlatformIO env `notes` passes -DHG_APP=notes.
# Two apps in one component because they share the board table, the ports and
# the drivers: the difference is which of both lists is compiled.
set(HG_APP "hermes" CACHE STRING "Which app to build: hermes or notes")
if(NOT HG_APP STREQUAL "hermes" AND NOT HG_APP STREQUAL "notes")
    message(FATAL_ERROR "HG_APP must be 'hermes' or 'notes', not '${HG_APP}'")
endif()
set(HG_NOTES_APP OFF)
if(HG_APP STREQUAL "notes")
    # The notes app replaces main.cpp, which is what reads the board for every
    # other target: without the e-paper board there is no panel, no card slot and
    # no codec to bring up, so the target would link and do nothing.
    if(NOT CONFIG_HG_BOARD_WS_EPAPER_154)
        message(FATAL_ERROR "HG_APP=notes is the waveshare-epaper-154 app; "
                            "select that board in sdkconfig.defaults")
    endif()
    set(HG_NOTES_APP ON)
endif()

set(HG_P4_SOURCES)
set(HG_P4_REQUIRES)
set(HG_RGB_REQUIRES)
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND HG_P4_SOURCES
        "port_dsi.cpp"
        "port_audio_p4.cpp"
        "port_power_p4.cpp"
        "../../drivers/stc8_battery.cpp")
    list(APPEND HG_P4_REQUIRES esp_lcd_ek79007 esp_lcd_st7123)
    if(CONFIG_HG_BOARD_WS_P4_TOUCH_LCD_7B)
        list(APPEND HG_P4_SOURCES "port_camera.cpp")
        list(APPEND HG_P4_REQUIRES esp_video)
    endif()
endif()
# The CoreS3's GC0308 camera rides the S3 DVP backend of esp32-camera; the
# driver's own Kconfig default enables the GC0308 board support. The action is
# registered only for this board, so no other S3 image grows.
set(HG_CORES3_CAMERA_SOURCES)
set(HG_CORES3_CAMERA_REQUIRES)
if(IDF_TARGET STREQUAL "esp32s3")
    if(CONFIG_HG_BOARD_CORES3)
        list(APPEND HG_CORES3_CAMERA_SOURCES "port_camera_cores3.cpp")
        list(APPEND HG_CORES3_CAMERA_REQUIRES esp32-camera)
    endif()
endif()
# The e-paper display driver and the mode foundation (UI state, durable
# config, mode stubs) are only referenced by that board's main.cpp, so they
# compile only for it and no other image grows.
set(HG_EPAPER_SOURCES)
if(CONFIG_HG_BOARD_WS_EPAPER_154)
    list(APPEND HG_EPAPER_SOURCES
        "port_display_epd.cpp"
        "ui_state.cpp"
        "device_config.cpp"
        "modes/live_mode.cpp"
        "modes/live_state.cpp"
        "modes/notes_mode.cpp"
        "modes/notes_screen.cpp"
        "modes/mode_router.cpp"
        "modes/home_keys.cpp"
        "modes/auto_home.cpp"
        "modes/status_screen.cpp"
        "notes/notes_queue.cpp"
        "notes/notes_ble.cpp"
        "notes/notes_store.cpp"
        "notes/notes_retention.cpp"
        "notes/notes_capture.cpp"
        "notes/capture_writer.cpp"
        "notes/notes_uploader.cpp")
endif()
# The RGB panel driver (ST7701 over an I/O expander) only builds for the chips
# with an LCD_CAM RGB scanout: port_rgb.cpp is compiled out on every other
# target, and esp_lcd_st7701 does not publish builds for them either.
if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4")
    list(APPEND HG_RGB_REQUIRES esp_lcd_st7701 esp_lcd_panel_io_additions)
endif()

# Built into both apps: the board table, the notes stack and the ports the
# notes device keeps (display, codec, card, clock, power, console, radio,
# Wi-Fi setup, OTA). A port that needs hg::App only does so in the callbacks
# main.cpp wires up; the notes app leaves those sections unlinked.
set(HG_SHARED_SOURCES
    "board.cpp"
    "port_ble.cpp"
    "port_buttons.cpp"
    "port_codec.cpp"
    # The codec calls hg::speaker_now_ms(), which lives here beside the I2S
    # speaker that shares its clock. The I2S classes the file also defines cost
    # the notes image nothing: nothing references them, so they are dropped.
    "port_audio.cpp"
    "port_console.cpp"
    "port_diag.cpp"
    "port_events.cpp"
    "port_latch_power.cpp"
    "port_ota.cpp"
    "port_power.cpp"
    "port_sdlog.cpp"
    # port_sdlog.cpp is built for every board and calls these, whether or not
    # the board has a card slot (a board with none leaves sd.enabled false and
    # the mount declines), so the implementation cannot follow the notes
    # modules behind CONFIG_HG_BOARD_WS_EPAPER_154: that made every ESP32-P4
    # image fail to link.
    "notes/sd_mount.cpp"
    "port_storage.cpp"
    "port_system.cpp"
    "port_time.cpp"
    "port_wifi.cpp"
    "port_wifi_setup.cpp"
    "../../drivers/axp2101.cpp"
    "../../drivers/cores3.cpp"
    "../../drivers/sticks3.cpp"
    "../../drivers/stopwatch.cpp"
    "../../drivers/bmi270.cpp"
    "../../drivers/pcf85063.cpp"
    "../../drivers/rx8130.cpp"
    "../../drivers/qmi8658.cpp")

# The notes app's own source: the entry point (notes_app/main.cpp), the screen
# and key rules, and the notes half of the e-paper board's sources. The mode
# modules, ui_state.cpp and the core's UI are not here: the notes app has its
# own screens.
set(HG_NOTES_SOURCES
    "../notes_app/main.cpp"
    "notes_app/ui_keys.cpp"
    "port_display_epd.cpp"
    "device_config.cpp"
    "notes/notes_queue.cpp"
    "notes/notes_ble.cpp"
    "notes/notes_store.cpp"
    "notes/notes_retention.cpp"
    "notes/notes_capture.cpp"
    "notes/capture_writer.cpp"
    "notes/notes_uploader.cpp")

# The library app: everything the core's UI drives, on every board it supports.
set(HG_HERMES_SOURCES
    "main.cpp"
    "port_amoled.cpp"
    "port_audio.cpp"
    ${HG_P4_SOURCES}
    ${HG_CORES3_CAMERA_SOURCES}
    ${HG_EPAPER_SOURCES}
    "port_cardputer.cpp"
    "port_cores3.cpp"
    "port_sticks3.cpp"
    "port_stopwatch.cpp"
    "port_tca9554.cpp"
    "port_dac.cpp"
    "port_display.cpp"
    "port_motion.cpp"
    "port_opus.cpp"
    "port_parallel.cpp"
    "port_rgb.cpp"
    "port_tailnet.cpp"
    "port_touch.cpp"
    "port_ws.cpp")

if(HG_NOTES_APP)
    set(HG_APP_SOURCES ${HG_SHARED_SOURCES} ${HG_NOTES_SOURCES})
else()
    set(HG_APP_SOURCES ${HG_SHARED_SOURCES} ${HG_HERMES_SOURCES})
endif()

# "driver" pulls in GPIO, I2S, LEDC and SPI on every ESP-IDF 5.x release.
# ESP-IDF 6.x dropped I2S, LEDC and the DAC from that catch-all, so name them
# too; all three components exist in 5.x as well, where this is a no-op.
set(HG_APP_PRIV_REQUIRES
    app_update
    bt
    driver
    esp_driver_i2s
    esp_driver_ledc
    esp_driver_dac
    esp_app_format
    esp_adc
    esp_event
    esp_http_client
    esp_partition
    espcoredump
    esp_lcd
    esp_netif
    esp_http_server
    esp_psram
    esp_timer
    esp_wifi
    sdmmc
    fatfs
    console
    nvs_flash
    mbedtls)
# The Hermes app's dependencies only: the live WebSocket client, the tailnet
# tunnel and the panel drivers no notes device carries. A component that is not
# required is still built by ESP-IDF but never linked, so the notes image holds
# no symbol from any of them.
if(NOT HG_NOTES_APP)
    list(APPEND HG_APP_PRIV_REQUIRES
        esp_websocket_client
        ${HG_RGB_REQUIRES}
        ${HG_P4_REQUIRES}
        ${HG_CORES3_CAMERA_REQUIRES}
        # Empty unless CONFIG_HG_TAILNET (components/tailtiny).
        tailtiny)
endif()

idf_component_register(
    SRCS
        ${HG_APP_SOURCES}
    INCLUDE_DIRS "." "../../drivers" "${CMAKE_CURRENT_BINARY_DIR}"
                 # The notes app's entry point lives beside main/ rather than in
                 # it, so its own headers resolve from the top of the app dir.
                 "${CMAKE_CURRENT_LIST_DIR}/../notes_app"
    REQUIRES core
    PRIV_REQUIRES
        ${HG_APP_PRIV_REQUIRES})

# The firmware's own debug lines stay in the build, so the console's `log <tag> debug`
# can turn them on without a rebuild. A line below its tag's level is skipped at run
# time. Only this component: ESP-IDF's and the managed components' debug lines stay out.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_DEBUG)

# Which app this component is, as a number, so a shared port can leave out the
# module the other app alone carries. The only one today is port_diag.cpp's
# touch report: the notes app keeps no port_touch.cpp, so asking a touch input
# for its statistics would ask for a symbol nothing defines.
if(HG_NOTES_APP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HG_NOTES_APP=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HG_NOTES_APP=0)
endif()
