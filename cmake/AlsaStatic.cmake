# AlsaStatic.cmake
# Handles standalone static alsa-lib build via ExternalProject
#
# Cache variables:
#   ALSA_VERSION (default: 1.2.11) - alsa-lib version for static build
#   ALSA_STATIC_PREFIX (default: ${CMAKE_SOURCE_DIR}/alsa-static)
#   BUILD_ALSA_STATIC (default: ON) - enable/disable static ALSA build

include_guard(GLOBAL)

include(ExternalProject)
include(ProcessorCount)

# Default cache values
if (NOT ALSA_VERSION)
    set(ALSA_VERSION "1.2.11" CACHE STRING "alsa-lib version used for static build")
endif ()

if (NOT ALSA_STATIC_PREFIX)
    set(ALSA_STATIC_PREFIX "${CMAKE_SOURCE_DIR}/alsa-static" CACHE PATH "Install prefix for CMake-built static alsa-lib")
endif ()

if (NOT DEFINED BUILD_ALSA_STATIC)
    option(BUILD_ALSA_STATIC "Build static alsa-lib with CMake when STATIC_LINK=ON" ON)
endif ()

# Determine parallel build jobs
processorcount(ALSA_MAKE_JOBS)
if (NOT ALSA_MAKE_JOBS)
    set(ALSA_MAKE_JOBS 1)
endif ()

# Setup ALSA static target
function(setup_alsa_static_target)
    if (NOT BUILD_ALSA_STATIC)
        return()
    endif ()

    if(TARGET ALSA::asound_static)
        return()
    endif()

    # Ensure generated include/lib dirs exist before imported target validation.
    file(MAKE_DIRECTORY "${ALSA_STATIC_PREFIX}/include")
    file(MAKE_DIRECTORY "${ALSA_STATIC_PREFIX}/lib")

    # Resolve source URL (prefer local tarball if present, else fetch upstream)
    set(ALSA_TARBALL "${ALSA_STATIC_PREFIX}/src/alsa-lib-${ALSA_VERSION}.tar.bz2")
    set(ALSA_SOURCE_URL "https://www.alsa-project.org/files/pub/lib/alsa-lib-${ALSA_VERSION}.tar.bz2")
    if (EXISTS "${ALSA_TARBALL}")
        set(ALSA_SOURCE_URL "file://${ALSA_TARBALL}")
    endif ()

    # Cross-compilation host flag
    set(ALSA_HOST_FLAG "")
    if (CMAKE_CROSSCOMPILING)
        set(ALSA_HOST_FLAG "--host=${CMAKE_C_COMPILER_TARGET}")
        if (NOT CMAKE_C_COMPILER_TARGET)
            # Fallback for ARM64 cross-compilation
            set(ALSA_HOST_FLAG "--host=aarch64-linux-gnu")
        endif ()
    endif ()

    # Define external project for building alsa-lib from source
    ExternalProject_Add(alsa_static_build
            URL "${ALSA_SOURCE_URL}"
            DOWNLOAD_DIR "${ALSA_STATIC_PREFIX}/src"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_DIR "${ALSA_STATIC_PREFIX}/src/alsa-lib-${ALSA_VERSION}"
            BUILD_IN_SOURCE 1
            CONFIGURE_COMMAND
            ${CMAKE_COMMAND} -E env
            CC=${CMAKE_C_COMPILER}
            CXX=${CMAKE_CXX_COMPILER}
            <SOURCE_DIR>/configure
            ${ALSA_HOST_FLAG}
            --enable-static
            --disable-shared
            --disable-python
            --prefix=${ALSA_STATIC_PREFIX}
            --exec-prefix=${ALSA_STATIC_PREFIX}
            --with-configdir=${ALSA_STATIC_PREFIX}/share/alsa
            --with-plugindir=${ALSA_STATIC_PREFIX}/lib/alsa-lib
            BUILD_COMMAND make -j${ALSA_MAKE_JOBS}
            INSTALL_COMMAND make install
            BUILD_BYPRODUCTS "${ALSA_STATIC_PREFIX}/lib/libasound.a"
            LOG_CONFIGURE 1
            LOG_BUILD 1
            LOG_INSTALL 1
    )

    # Create imported static library target
    add_library(ALSA::asound_static STATIC IMPORTED GLOBAL)
    set_target_properties(ALSA::asound_static PROPERTIES
            IMPORTED_LOCATION "${ALSA_STATIC_PREFIX}/lib/libasound.a"
            INTERFACE_INCLUDE_DIRECTORIES "${ALSA_STATIC_PREFIX}/include"
    )

    # Ensure external project runs before attempting to link
    add_dependencies(ALSA::asound_static alsa_static_build)

    # Export target properties for parent scope
    set(ALSA_STATIC_TARGET "ALSA::asound_static" CACHE INTERNAL "Static ALSA target")
    set(ALSA_STATIC_LIB_PATH "${ALSA_STATIC_PREFIX}/lib/libasound.a" CACHE INTERNAL "Static ALSA library path")
    set(ALSA_STATIC_INCLUDE_DIR "${ALSA_STATIC_PREFIX}/include" CACHE INTERNAL "Static ALSA include directory")
endfunction()

# Call setup function if this module is included
setup_alsa_static_target()

