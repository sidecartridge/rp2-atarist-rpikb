# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.26)

# Board / platform selection:
# Use env var PICO_PLATFORM as selector: pico_w | pico2_w (also accepts rp2040 | rp2350).
# Default is pico2_w -> rp2350.
set(PICO_PLATFORM_SELECTOR "$ENV{PICO_PLATFORM}")
string(STRIP "${PICO_PLATFORM_SELECTOR}" PICO_PLATFORM_SELECTOR)

if("${PICO_PLATFORM_SELECTOR}" STREQUAL "")
    set(PICO_BOARD "pico2_w")
    set(PICO_PLATFORM "rp2350")
    message(STATUS "PICO_PLATFORM not set, defaulting to PICO_BOARD=${PICO_BOARD}, PICO_PLATFORM=${PICO_PLATFORM}")
elseif("${PICO_PLATFORM_SELECTOR}" STREQUAL "pico2_w" OR "${PICO_PLATFORM_SELECTOR}" STREQUAL "rp2350")
    set(PICO_BOARD "pico2_w")
    set(PICO_PLATFORM "rp2350")
elseif("${PICO_PLATFORM_SELECTOR}" STREQUAL "pico_w" OR "${PICO_PLATFORM_SELECTOR}" STREQUAL "rp2040")
    set(PICO_BOARD "pico_w")
    set(PICO_PLATFORM "rp2040")
else()
    message(FATAL_ERROR "Unsupported PICO_PLATFORM='${PICO_PLATFORM_SELECTOR}'. Supported values: pico2_w, pico_w (also rp2350, rp2040).")
endif()

# Set this before creating the target or finding the Pico SDK
set(PICO_BTSTACK_ENABLED 1)
set(PICO_BTSTACK_CLASSIC 1)

# if PICOTOOL_OVERRIDE_DIR system environment variable is set,
# then use that as the folder for the picotool executable
if (DEFINED ENV{PICOTOOL_OVERRIDE_DIR})
    message(STATUS "PICOTOOL_OVERRIDE_DIR env var is set to '$ENV{PICOTOOL_OVERRIDE_DIR}'")
    add_executable(picotool IMPORTED GLOBAL)
    set_property(TARGET picotool PROPERTY IMPORTED_LOCATION $ENV{PICOTOOL_OVERRIDE_DIR}/picotool)
    # check the picotool version:
    execute_process(COMMAND $ENV{PICOTOOL_OVERRIDE_DIR}/picotool version
                    OUTPUT_VARIABLE PICOTOOL_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX MATCH "^picotool v${picotool_VERSION_REQUIRED}" PICOTOOL_VERSION_MATCH ${PICOTOOL_VERSION})
    if (NOT PICOTOOL_VERSION_MATCH)
        message(STATUS "picotool version response was: ${PICOTOOL_VERSION}")
        message(FATAL_ERROR "PICOTOOL_OVERRIDE_DIR is set to '$ENV{PICOTOOL_OVERRIDE_DIR}', but the version of picotool found is not ${picotool_VERSION_REQUIRED}")
    else()
        message(STATUS "picotool version response was: ${PICOTOOL_VERSION}")
    endif()
endif ()

# Keep BOARD_TYPE aligned with resolved PICO_BOARD for downstream compile definitions.
set(BOARD_TYPE ${PICO_BOARD})

# Determine board type macros
if("${BOARD_TYPE}" STREQUAL "pico_w")
    add_compile_definitions(BOARD_TYPE_PICO_W=1 BOARD_TYPE_PICO=0 BOARD_TYPE_CUSTOM16MB=0)
elseif("${BOARD_TYPE}" STREQUAL "pico2_w")
    add_compile_definitions(BOARD_TYPE_PICO_W=1 BOARD_TYPE_PICO=0 BOARD_TYPE_CUSTOM16MB=0)
elseif("${BOARD_TYPE}" STREQUAL "pico")
    add_compile_definitions(BOARD_TYPE_PICO_W=0 BOARD_TYPE_PICO=1 BOARD_TYPE_CUSTOM16MB=0)
elseif("${BOARD_TYPE}" STREQUAL "sidecartos_16mb")
    add_compile_definitions(BOARD_TYPE_PICO_W=0 BOARD_TYPE_PICO=1 BOARD_TYPE_CUSTOM16MB=0)
else()
    message(FATAL_ERROR "Unknown BOARD_TYPE: ${BOARD_TYPE}")
endif()

# Board target (0=Unknown, 1=Croissant Revision 2, 2=Souffle Revision 2)
if(DEFINED ENV{BOARD_TARGET})
    set(BOARD_TARGET $ENV{BOARD_TARGET})
else()
    set(BOARD_TARGET 0)
endif()

if(NOT BOARD_TARGET MATCHES "^[0-2]$")
    message(FATAL_ERROR "Invalid BOARD_TARGET='${BOARD_TARGET}'. Supported values: 0 (Unknown), 1 (Croissant Revision 2), 2 (Souffle Revision 2).")
endif()

# Add / normalize PICO_SDK_PATH
if(NOT DEFINED ENV{PICO_SDK_PATH})
    set(ENV{PICO_SDK_PATH} "${CMAKE_CURRENT_LIST_DIR}/../pico-sdk")
endif()
set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
message(STATUS "PICO_SDK_PATH: ${PICO_SDK_PATH}")

# Add / normalize PICO_EXTRAS_PATH
if(NOT DEFINED ENV{PICO_EXTRAS_PATH})
    set(ENV{PICO_EXTRAS_PATH} "${CMAKE_CURRENT_LIST_DIR}/../pico-extras")
endif()
set(PICO_EXTRAS_PATH "$ENV{PICO_EXTRAS_PATH}")
message(STATUS "PICO_EXTRAS_PATH: ${PICO_EXTRAS_PATH}")

# Add / normalize BLUEPAD32_ROOT
if(NOT DEFINED ENV{BLUEPAD32_ROOT})
    set(ENV{BLUEPAD32_ROOT} "${CMAKE_CURRENT_LIST_DIR}/../bluepad32")
endif()
set(BLUEPAD32_ROOT "$ENV{BLUEPAD32_ROOT}")
message(STATUS "BLUEPAD32_ROOT: ${BLUEPAD32_ROOT}")

# To use BTstack from Pico SDK do
set(BTSTACK_ROOT "${PICO_SDK_PATH}/lib/btstack")

# If you want to use latest BTstack (included in Bluepad32 repo),
# uncomment the following lines:
# -- uncomment from here:
#set(BTSTACK_ROOT ${BLUEPAD32_ROOT}/external/btstack)   
#set(PICO_BTSTACK_PATH ${BTSTACK_ROOT})
# Add missing files here. See:
# https://github.com/bluekitchen/btstack/issues/649
# https://github.com/raspberrypi/pico-sdk/issues/2142
#set(BTSTACK_MISSING_SOURCES
#    ${BTSTACK_ROOT}/src/hci_event_builder.c
#)
# -- uncomment until here
# And you need to apply this patch in Pico SDK
# https://github.com/raspberrypi/pico-sdk/pull/2157

# Resolve debug flag from environment early so it can drive TinyUSB logging.
set(_DEBUG "$ENV{DEBUG_MODE}")
if("${_DEBUG}" STREQUAL "")
    set(_DEBUG 0)
endif()

# Include build functions from Pico SDK
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

# TinyUSB RP2040 family.cmake maps LOG -> CFG_TUSB_DEBUG.
# Use level 2 for Debug builds (level 3 is broken with this TinyUSB snapshot).
if("${_DEBUG}" STREQUAL "1" AND NOT DEFINED LOG)
    set(LOG 0)
endif()


# Set name of project (as PROJECT_NAME) and C/C   standards
project(rp2-ikbd C CXX)
set(CMAKE_C_STANDARD 11)
# Creates a pico-sdk subdirectory in our project for the libraries
pico_sdk_init()

# Add the settings library sources from settings
add_subdirectory(settings)

# Tell CMake where to find the executable source file
add_executable(${PROJECT_NAME} 
    main.c
    gconfig.c
    hidinput.c
    joystick.c
    mouse.c
    serialp.c
    stkeys.c
    6301/6301.c
    usbloop.c
    btloop.c
    btstack_config.h
    sdkconfig.h
    ${BTSTACK_MISSING_SOURCES}
)

if("${BOARD_TARGET}" STREQUAL "1")
    target_sources(${PROJECT_NAME} PRIVATE nativeloop.c)
endif()

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(${PROJECT_NAME})

# Link libraries required for the project
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${LINK_LIBRARIES}        # External or additional libraries passed as variables
    pico_multicore
    pico_stdlib
    pico_stdio_uart
    pico_btstack_cyw43
    pico_btstack_classic
    pico_cyw43_arch_none
    tinyusb_board
    tinyusb_device
    tinyusb_host
    settings                 # Custom settings library
    bluepad32
)

# Link custom memmap with reserved memory for ROMs
if(PICO_PLATFORM STREQUAL "rp2350" OR PICO_PLATFORM STREQUAL "rp2350-arm-s" OR PICO_PLATFORM STREQUAL "rp2350-riscv")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        PICO_TARGET_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/memmap_rp2-ikbd_rp2350.ld
    )
else()
    set_target_properties(${PROJECT_NAME} PROPERTIES
#       PICO_TARGET_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/memmap_rp2-ikbd_copy_to_ram.ld
        PICO_TARGET_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/memmap_rp2-ikbd_default.ld
    )
endif()

# Needed to include lwipopts.h properly
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/6301
)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${BLUEPAD32_ROOT}/src/components/bluepad32/include)

# Needed when using BTstack from our branch
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include)
# Need for certain IDEs, like CLion. Otherwise it won't find btstack
include_directories(${BTSTACK_ROOT}/src)

# Make local btstack_config.h and sdkconfig.h visible to libbluepad32
include_directories(${CMAKE_CURRENT_LIST_DIR})

add_subdirectory(${BLUEPAD32_ROOT}/src/components/bluepad32 libbluepad32)


# Fetch the values from the environment variables
set(RELEASE_TYPE $ENV{RELEASE_TYPE})
set(RELEASE_VERSION $ENV{RELEASE_VERSION})
set(RELEASE_DATE $ENV{RELEASE_DATE})

# If the environment variables are not set, use default values
if(NOT DEFINED RELEASE_VERSION)

    # Check if RELEASE_TYPE is set and not empty, otherwise use default "FINAL"
    if(NOT DEFINED RELEASE_TYPE OR RELEASE_TYPE STREQUAL "")
        set(RELEASE_TYPE "final")
    endif()

    # Determine the file name based on RELEASE_TYPE
    if(RELEASE_TYPE STREQUAL "final")
        set(VERSION_FILE_NAME "version.txt")
    else()
        set(VERSION_FILE_NAME "version-${RELEASE_TYPE}.txt")
    endif()
    
    # Remove newline character if present
    string(STRIP "${RELEASE_VERSION}" RELEASE_VERSION)

    # In case version.txt is empty or does not exist, use a default version
    if("${RELEASE_VERSION}" STREQUAL "")
        set(RELEASE_VERSION "v0.0.1dev")
    endif()
endif()

if(NOT DEFINED RELEASE_DATE)
        string(TIMESTAMP CURRENT_DATE_TIME "%Y-%m-%d %H:%M:%S")
        set(RELEASE_DATE ${CURRENT_DATE_TIME})
endif()

# Debug outputs
pico_enable_stdio_usb(${PROJECT_NAME} 0)
# Workaround to disable USB output in release builds
if(${_DEBUG} STREQUAL "0")
    pico_enable_stdio_uart(${PROJECT_NAME} 0)
    target_link_options(${PROJECT_NAME} PRIVATE
    "-Wl,--gc-sections"
    "-Wl,--strip-all"
    )

else()
    pico_enable_stdio_uart(${PROJECT_NAME} 1)
endif()

# Pass these values to the C compiler
add_definitions(-DRELEASE_TYPE="${RELEASE_TYPE}")
add_definitions(-DRELEASE_VERSION="${RELEASE_VERSION}")
add_definitions(-DRELEASE_DATE="${RELEASE_DATE}")
add_definitions(-DLATEST_RELEASE_URL="${LATEST_RELEASE_URL}")

# Pass the _DEBUG flag to the C compiler
add_definitions(-D_DEBUG=${_DEBUG})

# PICO_USE_FASTEST_SUPPORTED_CLOCK to the maximum frequency
if (NOT DEFINED ENV{PICO_USE_FASTEST_SUPPORTED_CLOCK})
    add_definitions(-DPICO_USE_FASTEST_SUPPORTED_CLOCK=1)
endif()

# Print the variables on screen
message(STATUS "RELEASE_TYPE: " ${RELEASE_TYPE})
message(STATUS "RELEASE_VERSION: " ${RELEASE_VERSION})
message(STATUS "RELEASE_DATE: " ${RELEASE_DATE})
message(STATUS "DEBUG_MODE: " ${_DEBUG})
message(STATUS "LATEST_RELEASE_URL: " ${LATEST_RELEASE_URL})

# Handle custom build type by checking if it's set
if(CMAKE_BUILD_TYPE STREQUAL "CustomBuild")
    # Define custom build type flags
    set(CMAKE_C_FLAGS_CUSTOMBUILD "-ffunction-sections -fdata-sections -O0 -mcpu=cortex-m0 -mthumb")
    set(CMAKE_CXX_FLAGS_CUSTOMBUILD "-ffunction-sections -fdata-sections -O0 -mcpu=cortex-m0 -mthumb")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_CUSTOMBUILD}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_CUSTOMBUILD}")
endif()

target_compile_options(${PROJECT_NAME} PRIVATE -Wno-implicit-int)

set(PICO_DEFAULT_BINARY_TYPE copy_to_ram)
set(PICO_COPY_TO_RAM 1)

#set(PICO_FLASH_ASSUME_CORE1_SAFE 1)

target_compile_definitions(${PROJECT_NAME}  PRIVATE
 HCI_CONTROLLER_CHIPSET_PICOW
)

# Computer target mask (1=Native, 2=USB, 4=Bluetooth)
if(DEFINED ENV{COMPUTER_TARGET})
    set(COMPUTER_TARGET $ENV{COMPUTER_TARGET})
elseif("${BOARD_TARGET}" STREQUAL "1")
    set(COMPUTER_TARGET 5)  # Croissant: Native + Bluetooth
elseif("${BOARD_TARGET}" STREQUAL "2")
    set(COMPUTER_TARGET 6)  # Souffle: USB + Bluetooth
else()
    set(COMPUTER_TARGET 5)  # Unknown board target fallback
endif()
math(EXPR COMPUTER_TARGET_NATIVE "(${COMPUTER_TARGET} & 1)")
math(EXPR COMPUTER_TARGET_USB "(${COMPUTER_TARGET} & 2)")
math(EXPR COMPUTER_TARGET_BT "(${COMPUTER_TARGET} & 4)")
message(STATUS "COMPUTER_TARGET_NATIVE: ${COMPUTER_TARGET_NATIVE}")
message(STATUS "COMPUTER_TARGET_USB: ${COMPUTER_TARGET_USB}")
message(STATUS "COMPUTER_TARGET_BT: ${COMPUTER_TARGET_BT}")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    COMPUTER_TARGET_NATIVE=${COMPUTER_TARGET_NATIVE}
    COMPUTER_TARGET_USB=${COMPUTER_TARGET_USB}
    COMPUTER_TARGET_BT=${COMPUTER_TARGET_BT}
)
message(STATUS "COMPUTER_TARGET: ${COMPUTER_TARGET}")

target_compile_definitions(${PROJECT_NAME} PRIVATE
    BOARD_TARGET=${BOARD_TARGET}
)
message(STATUS "BOARD_TARGET: ${BOARD_TARGET}")
