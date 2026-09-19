cmake_minimum_required(VERSION 3.24)
project(TtpI18n LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "ttp_i18n is a Windows DLL")
endif()

# One DLL serves both player editions, including standalone and Debug builds.
# The player is a separate build; this repository owns its ABI and runtime policy.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xp_runtime.cmake")
# Runtime checks require a modern debug CRT; symbols and /Od still work.
string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
add_library(ttp_i18n SHARED src/catalog.cpp src/dll.cpp src/ttp_i18n.def)
target_compile_features(ttp_i18n PRIVATE cxx_std_20)
target_include_directories(ttp_i18n PRIVATE include src)
target_compile_definitions(ttp_i18n PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(ttp_i18n PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
set(TTP_I18N_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
set_target_properties(ttp_i18n PROPERTIES PREFIX "" OUTPUT_NAME "ttp_i18n"
  RUNTIME_OUTPUT_DIRECTORY "${TTP_I18N_RUNTIME_DIR}/AddIn"
  PDB_OUTPUT_DIRECTORY "${TTP_I18N_RUNTIME_DIR}")
# Discard unused thunk sections even in Debug; otherwise their optional
# dependency imports would become mandatory XP loader dependencies.
target_link_options(ttp_i18n PRIVATE /OPT:REF /INCREMENTAL:NO)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(TTP_I18N_IMPORT_CHECK "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py")
add_custom_command(TARGET ttp_i18n POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${TTP_I18N_IMPORT_CHECK}"
    "$<TARGET_FILE:ttp_i18n>"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
    --report "${TTP_I18N_RUNTIME_DIR}/i18n-legacy-imports.json"
  COMMENT "Auditing the shared i18n DLL for XP and Windows 7"
  VERBATIM)
set_property(TARGET ttp_i18n APPEND PROPERTY LINK_DEPENDS "${TTP_I18N_IMPORT_CHECK}")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/i18n-licenses")
configure_file("${ttplayer_yy_thunks_SOURCE_DIR}/LICENSE"
  "${CMAKE_CURRENT_BINARY_DIR}/i18n-licenses/YY-Thunks-LICENSE.txt" COPYONLY)
foreach(license IN ITEMS VC-LTL-LICENSE.txt legacy-third-party.md)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/docs/licenses/${license}"
    "${CMAKE_CURRENT_BINARY_DIR}/i18n-licenses/${license}" COPYONLY)
endforeach()
add_custom_target(ttp_i18n_licenses
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
    "${CMAKE_CURRENT_BINARY_DIR}/i18n-licenses" "${TTP_I18N_RUNTIME_DIR}/licenses"
  VERBATIM)
add_dependencies(ttp_i18n ttp_i18n_licenses)

# Keep this as a build target so PO-only edits are copied without relinking.
add_custom_target(ttp_i18n_catalogs
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/i18n" "${TTP_I18N_RUNTIME_DIR}/i18n"
  COMMENT "Staging gettext catalogs"
  VERBATIM)
add_dependencies(ttp_i18n ttp_i18n_catalogs)

if(BUILD_TESTING)
  add_executable(ttp_i18n_catalog_tests tests/catalog_tests.cpp src/catalog.cpp)
  target_compile_features(ttp_i18n_catalog_tests PRIVATE cxx_std_20)
  target_include_directories(ttp_i18n_catalog_tests PRIVATE src)
  target_compile_definitions(ttp_i18n_catalog_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(ttp_i18n_catalog_tests PRIVATE /W4 /EHsc /utf-8)
  endif()
  enable_testing()
  add_test(NAME ttp_i18n_catalog_tests COMMAND ttp_i18n_catalog_tests "${CMAKE_CURRENT_SOURCE_DIR}/i18n")
  add_test(NAME ttp_i18n_translation_tests COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/translation_tests.py")
endif()
