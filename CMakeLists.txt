cmake_minimum_required(VERSION 3.15)

# Apply this to both SDK components (including mm.c) and GPMon application code.
add_compile_definitions(CONFIG_MM_ENABLE_MIN_FREE_TRACKING=1)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(.)
sdk_add_include_directories("${CMAKE_CURRENT_SOURCE_DIR}/src")
sdk_add_include_directories(
    "${BL_SDK_BASE}/components/net/lwip/lwip/lwip-port/config"
)

sdk_set_main_file(src/main.c)
project(GPMon_v1 C CXX ASM)

target_sources(app PRIVATE
    src/capture_queue.c
    src/packet_framer.c
    src/gpmon_core.c
    src/uart_capture.c
    src/wifi_provision.c
    src/gpmon_server_config.c
    src/gpmon_tcp_sender.c
    src/gpmon_flash_queue.c
    src/web_config.c
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(GPMon_v1_bl602.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)