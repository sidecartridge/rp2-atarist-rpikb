# Writes build_id.h (RELEASE_BUILD_ID) and build_id.c (the same string as the
# release_build_id symbol in flash, which the SWD developer tools read) with
# the git commit the firmware was built from. Run on every build, not only at
# configure time, so an incremental build never reports a stale ID.
#
#   <sha7>                     clean tree
#   <sha7>-dirty.<diff7>       uncommitted changes under src/; <diff7> hashes
#                              the diff, so different changes give different
#                              IDs and the same tree always gives the same ID.
#                              Changes elsewhere (docs, root scripts) do not
#                              reach the binary and do not count
#   $ENV{RELEASE_BUILD_ID}     when set (a source copy outside git); cut to 21
#                              characters
#   nogit                      not a git checkout
#
# A debug build appends +debug, so the builds of one tree cannot be taken for
# each other on the device.
#
# Adapted from the sibling projects' build_id.cmake (sidecartos,
# rp2-booster-bootloader, rp2-atarist-rpikb-booster).
#
# Inputs: -DREPO_DIR=<repository root> -DOUT_DIR=<folder for the two files>
#         -DDEBUG_BUILD=<0|1>

if(DEFINED ENV{RELEASE_BUILD_ID} AND NOT "$ENV{RELEASE_BUILD_ID}" STREQUAL "")
  set(BUILD_ID "$ENV{RELEASE_BUILD_ID}")
else()
  execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${REPO_DIR}"
    OUTPUT_VARIABLE SHA
    RESULT_VARIABLE SHA_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT SHA_RESULT EQUAL 0)
    set(BUILD_ID "nogit")
  else()
    # Submodules are excluded: the build scripts check them out at pinned refs.
    execute_process(
      COMMAND git diff HEAD --binary --ignore-submodules -- src
      WORKING_DIRECTORY "${REPO_DIR}"
      OUTPUT_VARIABLE DIFF
      ERROR_QUIET)
    if("${DIFF}" STREQUAL "")
      set(BUILD_ID "${SHA}")
    else()
      string(SHA1 DIFF_HASH "${DIFF}")
      string(SUBSTRING "${DIFF_HASH}" 0 7 DIFF_HASH)
      set(BUILD_ID "${SHA}-dirty.${DIFF_HASH}")
    endif()
  endif()
endif()

# Longest shape is 21 characters (<sha7>-dirty.<diff7>), 27 with +debug.
string(SUBSTRING "${BUILD_ID}" 0 21 BUILD_ID)
if(DEBUG_BUILD)
  set(BUILD_ID "${BUILD_ID}+debug")
endif()

function(write_if_changed path content)
  if(EXISTS "${path}")
    file(READ "${path}" old)
  else()
    set(old "")
  endif()
  if(NOT "${old}" STREQUAL "${content}")
    file(WRITE "${path}" "${content}")
  endif()
endfunction()

write_if_changed("${OUT_DIR}/build_id.h"
  "#pragma once\n#define RELEASE_BUILD_ID \"${BUILD_ID}\"\nextern const char release_build_id[];\n")
write_if_changed("${OUT_DIR}/build_id.c"
  "#include \"build_id.h\"\nconst char release_build_id[] = RELEASE_BUILD_ID;\n")
