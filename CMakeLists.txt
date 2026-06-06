cmake_minimum_required(VERSION 3.10)
include(cmake/aarch64-toolchain.cmake)
project(ds5_dongle_linux)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
SET(EXECUTABLE_OUTPUT_PATH  ${PROJECT_SOURCE_DIR}/bin)


option(STATIC_LINK "Prefer static linking where possible" ON)
if (STATIC_LINK)
    include(cmake/AlsaStatic.cmake)
else ()
    find_package(ALSA REQUIRED)
endif ()
find_package(Threads REQUIRED)

# WDL
add_library(wdl_resampler STATIC
        libs/WDL/WDL/resample.cpp
)
target_include_directories(wdl_resampler PUBLIC
        libs/WDL/WDL
)
target_compile_options(wdl_resampler PRIVATE -fsigned-char)
target_compile_definitions(wdl_resampler PRIVATE WDL_RESAMPLE_TYPE=float)

set(OPUS_DISABLE_INTRINSICS ON)
add_subdirectory(libs/opus)

add_executable(ds5_dongle_linux
        src/USBGadget.cpp
        src/main.cpp
        src/DebugHIDInput.cpp
        src/ALSARecord.cpp
        src/USBHID.cpp
        src/BTHID.cpp
        src/Utils.cpp
        src/bluetoothPacket.cpp
        src/audio.cpp
        src/config.cpp)

target_include_directories(ds5_dongle_linux PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/inc
)

target_compile_options(ds5_dongle_linux PRIVATE -fsigned-char -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(ds5_dongle_linux PRIVATE WDL_RESAMPLE_TYPE=float)

if (STATIC_LINK)
    target_include_directories(ds5_dongle_linux PRIVATE ${ALSA_STATIC_INCLUDE_DIR})
    target_link_options(ds5_dongle_linux PRIVATE -static)
    target_link_libraries(ds5_dongle_linux PRIVATE
            ALSA::asound_static
            Threads::Threads
            wdl_resampler
            opus
    )
else ()
# Match the Pico speaker path, which builds Opus in its default floating-point mode.

target_link_libraries(ds5_dongle_linux PRIVATE
        ALSA::ALSA
        Threads::Threads
        wdl_resampler
        opus
)
endif ()
