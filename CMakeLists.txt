cmake_minimum_required(VERSION 3.18)

# Use the devkitPro Wii toolchain unless the caller passed another
if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/Wii.cmake")
endif()

project(wii-network-tools LANGUAGES C VERSION 0.2.0)

# Set the game ID, whose fourth letter picks the region
set(GAME_ID "NETE01" CACHE STRING "Six character game ID of the disc")

# Find wit, which only the disc target needs
find_program(WIT_EXE wit)

# Build the Unity test framework for the SO and WD test suites from its submodule
add_subdirectory(libs/unity)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_CONFIG_H)
target_include_directories(unity PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/advanced>)

# Build the application from the shared code and the code of each menu
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.c)
add_executable(wii-network-tools ${SOURCES})
target_include_directories(wii-network-tools PRIVATE src src/advanced src/connections src/network)
target_compile_options(wii-network-tools PRIVATE -Wall -Wextra)
target_link_libraries(wii-network-tools PRIVATE unity::framework)
ogc_create_dol(wii-network-tools)

# Lay out the app for the Homebrew Channel
set(HOMEBREW_DIR ${CMAKE_CURRENT_BINARY_DIR}/homebrew/apps/wii-network-tools)
configure_file(meta.xml ${HOMEBREW_DIR}/meta.xml @ONLY)
configure_file(icon.png ${HOMEBREW_DIR}/icon.png COPYONLY)
add_custom_target(
  homebrew
  ALL
  COMMAND ${CMAKE_COMMAND} -E copy wii-network-tools.dol ${HOMEBREW_DIR}/boot.dol
  DEPENDS wii-network-tools
  VERBATIM
)

# Build the apploader straight into the disc's sys folder, freestanding and linked at its load address
set(DISC_DIR ${CMAKE_CURRENT_BINARY_DIR}/disc)
add_custom_command(
  OUTPUT ${DISC_DIR}/sys/apploader.img
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DISC_DIR}/sys ${DISC_DIR}/files
  COMMAND
    ${CMAKE_C_COMPILER} -Os -Wall -Wextra -mcpu=750 -meabi -mhard-float -G0 -ffreestanding -nostdlib
    -nostartfiles -T ${CMAKE_CURRENT_SOURCE_DIR}/apploader/apploader.ld -o apploader.elf
    ${CMAKE_CURRENT_SOURCE_DIR}/apploader/apploader.c
  COMMAND ${CMAKE_OBJCOPY} -O binary apploader.elf ${DISC_DIR}/sys/apploader.img
  DEPENDS apploader/apploader.c apploader/apploader.ld
  VERBATIM
)

# Compose the disc image, with wit filling in the ID, title, region and bi2.bin around an empty boot.bin
set(DISC_WBFS ${CMAKE_CURRENT_BINARY_DIR}/${GAME_ID}.wbfs)
add_custom_command(
  OUTPUT ${DISC_WBFS}
  COMMAND head -c 1088 /dev/zero > ${DISC_DIR}/sys/boot.bin
  COMMAND ${CMAKE_COMMAND} -E copy wii-network-tools.dol ${DISC_DIR}/sys/main.dol
  COMMAND
    ${WIT_EXE} copy ${DISC_DIR} ${DISC_WBFS} --overwrite --id ${GAME_ID} --name "Wii Network Tools"
    --ios 58
  DEPENDS wii-network-tools ${DISC_DIR}/sys/apploader.img
  VERBATIM
)
if(WIT_EXE)
  add_custom_target(disc ALL DEPENDS ${DISC_WBFS})
else()
  add_custom_target(
    disc
    COMMAND ${CMAKE_COMMAND} -E echo "The disc needs wit from Wiimms ISO Tools on the path"
    COMMAND ${CMAKE_COMMAND} -E false
  )
endif()
