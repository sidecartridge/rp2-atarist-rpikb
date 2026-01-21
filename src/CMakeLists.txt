# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.26)

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

# If not specified, use pico_w as default board type
set(BOARD_TYPE $ENV{BOARD_TYPE})
if(NOT DEFINED BOARD_TYPE)
    message(STATUS "BOARD_TYPE not set, using default pico_w")
    set(BOARD_TYPE "pico_w")
endif()
set(PICO_BOARD ${BOARD_TYPE})

# Determine board type macros
if("${BOARD_TYPE}" STREQUAL "pico_w")
    add_compile_definitions(BOARD_TYPE_PICO_W=1 BOARD_TYPE_PICO=0 BOARD_TYPE_CUSTOM16MB=0)
elseif("${BOARD_TYPE}" STREQUAL "pico")
    add_compile_definitions(BOARD_TYPE_PICO_W=0 BOARD_TYPE_PICO=1 BOARD_TYPE_CUSTOM16MB=0)
elseif("${BOARD_TYPE}" STREQUAL "sidecartos_16mb")
    add_compile_definitions(BOARD_TYPE_PICO_W=0 BOARD_TYPE_PICO=1 BOARD_TYPE_CUSTOM16MB=0)
else()
    message(FATAL_ERROR "Unknown BOARD_TYPE: ${BOARD_TYPE}")
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

# Include build functions from Pico SDK
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)


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
    nativeloop.c
    btstack_config.h
    sdkconfig.h
    ${BTSTACK_MISSING_SOURCES}
)

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
set_target_properties(${PROJECT_NAME} PROPERTIES
#       PICO_TARGET_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/memmap_rp2-ikbd_copy_to_ram.ld
       PICO_TARGET_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/memmap_rp2-ikbd_default.ld
)

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
set(_DEBUG $ENV{DEBUG_MODE})

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

if (NOT DEFINED _DEBUG)
        set(_DEBUG 0)
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
else()
    set(COMPUTER_TARGET 5)  # Default: Native + Bluetooth
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
