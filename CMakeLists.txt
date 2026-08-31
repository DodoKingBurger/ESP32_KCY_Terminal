# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# PROJECT_VER для app_desc: база из version.txt
set(VERSION_FILE "${CMAKE_SOURCE_DIR}/version.txt")
if(NOT EXISTS "${VERSION_FILE}")
    file(WRITE "${VERSION_FILE}" "1.0.0\n")
endif()
file(READ "${VERSION_FILE}" _ver_base)
string(STRIP "${_ver_base}" _ver_base)
string(REPLACE "\r" "" _ver_base "${_ver_base}")
if(NOT _ver_base MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    set(_ver_base "1.0.0")
endif()
set(PROJECT_VER "${_ver_base}")
message(STATUS "esp32_MK3 PROJECT_VER(base from version.txt)=${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32_MK3)

# Инкремент patch при КАЖДОЙ сборке → build/esp_fw_version.h
set(VERSION_STATE "${CMAKE_BINARY_DIR}/version_state.txt")
set(VERSION_HEADER "${CMAKE_BINARY_DIR}/esp_fw_version.h")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    set(Python3_EXECUTABLE "python")
endif()

# Первый header при configure (чтобы compile не падал до custom target)
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/bump_version.py"
            --base "${CMAKE_SOURCE_DIR}/version.txt"
            --state "${VERSION_STATE}"
            --header "${VERSION_HEADER}"
            --no-increment
    RESULT_VARIABLE _bump_rc
)
if(NOT _bump_rc EQUAL 0)
    file(WRITE "${VERSION_HEADER}" "#pragma once\n#define ESP_FW_VERSION_STR \"${PROJECT_VER}\"\n")
endif()

add_custom_target(bump_fw_version ALL
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/bump_version.py"
            --base "${CMAKE_SOURCE_DIR}/version.txt"
            --state "${VERSION_STATE}"
            --header "${VERSION_HEADER}"
    BYPRODUCTS "${VERSION_HEADER}" "${VERSION_STATE}"
    COMMENT "Bumping firmware version (every build)"
    VERBATIM
)

# web_server должен ждать генерации header
idf_component_get_property(_ws_lib web_server COMPONENT_LIB)
if(_ws_lib)
    add_dependencies(${_ws_lib} bump_fw_version)
endif()

# Итоговый файл для OTA: build/esp32_MK3_app_vX.Y.Z.bin (+ esp32_MK3_app_latest.bin)
add_custom_target(copy_versioned_firmware ALL
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/copy_named_firmware.py"
            --src "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
            --state "${VERSION_STATE}"
            --dst-dir "${CMAKE_BINARY_DIR}"
            --prefix "${CMAKE_PROJECT_NAME}"
    DEPENDS gen_project_binary bump_fw_version
    COMMENT "Copy firmware binary with version in filename"
    VERBATIM
)
