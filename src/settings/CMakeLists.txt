# Create a library for the settings files
add_library(settings
    settings.c
)

# Add the include directory (for settings.h)
target_include_directories(settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Get the _DEBUG value from parent scope and add as compile definition
if (NOT DEFINED _DEBUG)
    set(_DEBUG $ENV{DEBUG_MODE})
endif()
if (NOT DEFINED _DEBUG)
    set(_DEBUG 1)
endif()
target_compile_definitions(settings PRIVATE _DEBUG=${_DEBUG})

# Link necessary libraries
target_link_libraries(settings
    pico_stdlib    # Core Pico SDK library
    hardware_flash # Specific hardware flash library
)