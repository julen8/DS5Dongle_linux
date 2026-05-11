cmake_minimum_required(VERSION 3.10)
project(ds5_dongle_linux)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")
SET(EXECUTABLE_OUTPUT_PATH  ${PROJECT_SOURCE_DIR}/bin)

# Core Dependency
set(ALSA_LIBRARY /usr/lib/aarch64-linux-gnu/libasound.so)
set(ALSA_INCLUDE_DIR /usr/include)
find_package(ALSA REQUIRED)
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

add_executable(ds5_dongle_linux
        src/USBGadget.cpp
        src/USBGadget.h
        src/main.cpp
        src/DebugHIDInput.cpp
        src/DebugHIDInput.h
        src/ALSARecord.cpp
        src/ALSARecord.h
        src/USBHID.cpp
        src/USBHID.h
        src/BTHID.cpp
        src/BTHID.h
        src/Utils.cpp
        src/Utils.h)

target_compile_options(ds5_dongle_linux PRIVATE -fsigned-char)
target_compile_definitions(ds5_dongle_linux PRIVATE WDL_RESAMPLE_TYPE=float)

# Match the Pico speaker path, which builds Opus in its default floating-point mode.
set(OPUS_DISABLE_INTRINSICS ON)

add_subdirectory(libs/opus)

target_link_libraries(ds5_dongle_linux PRIVATE
        ALSA::ALSA
        Threads::Threads
        wdl_resampler
        opus
)

#add_custom_command(TARGET ds5_dongle_linux POST_BUILD
#        COMMAND /bin/scp ${PROJECT_SOURCE_DIR}/bin/ds5_dongle_linux root@192.168.6.142:/tmp
##        COMMAND /bin/scp -P 2222 ${PROJECT_SOURCE_DIR}/bin/ds5_dongle_linux root@192.168.6.168:/tmp
#        COMMENT "scp to server"
#)
