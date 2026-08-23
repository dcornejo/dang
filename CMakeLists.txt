# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.24)
project(yang_cpp VERSION 0.1.0 LANGUAGES CXX)

option(YANG_BUILD_TESTS "Build unit tests" ON)
option(YANG_BUILD_TOOLS "Build command-line tools" ON)
option(YANG_BUILD_DOCS "Build Doxygen API documentation" OFF)
option(YANG_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(YANG_BUILD_FUZZERS "Build libFuzzer hardening targets" OFF)
option(YANG_FUZZ_STANDALONE "Use the deterministic fuzz smoke driver" OFF)
option(YANG_BUILD_BENCHMARKS "Build deterministic performance benchmarks" OFF)
option(YANG_BUILD_DANGD "Build the dangd NETCONF server application" ON)

if(YANG_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    message(FATAL_ERROR "YANG_ENABLE_SANITIZERS requires Clang or GCC")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

find_package(fmt CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(LibXml2 REQUIRED)

add_library(yang
  src/config_edit.cc
  src/config_validation.cc
  src/netconf_datastore.cc
  src/netconf_framing.cc
  src/netconf_filter.cc
  src/netconf_notifications.cc
  src/nacm.cc
  src/netconf_persistence.cc
  src/netconf_server.cc
  src/netconf_session_registry.cc
  src/netconf_transport.cc
  src/netconf_with_defaults.cc
  src/compiler.cc
  src/diagnostic.cc
  src/deviation.cc
  src/extension.cc
  src/lexer.cc
  src/parser.cc
  src/resource_limits.cc
  src/schema_registry.cc
  src/schema_value.cc
  src/source_file.cc
  src/string_decoder.cc
  src/syntax_tree.cc
  src/utf8.cc)
target_sources(yang PRIVATE src/xml_schema_regex.cc)
target_sources(yang PRIVATE src/identity_feature.cc src/leafref.cc src/module_resolver.cc src/schema_path.cc src/schema_tree.cc src/semantic_model.cc src/type_system.cc src/validator.cc src/xpath.cc src/yin_document.cc)
add_library(yang::yang ALIAS yang)
target_compile_features(yang PUBLIC cxx_std_20)
target_include_directories(yang PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(yang PUBLIC fmt::fmt pugixml::pugixml
  nlohmann_json::nlohmann_json LibXml2::LibXml2)
target_compile_options(yang PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS yang EXPORT yangTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT yangTargets NAMESPACE yang:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yang)
configure_package_config_file(
  cmake/yangConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/yangConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yang)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/yangConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/yangConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/yangConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yang)
install(FILES LICENSE NOTICE README.md TODO.md CHANGELOG.md docs/STANDARDS.md
  docs/RELEASING.md
  docs/BENCHMARKS.md
  docs/HARDENING.md
  docs/COMPLIANCE.md
  docs/NACM_COMPLIANCE_MATRIX.md
  docs/USER_GUIDE.md
  docs/CONFIGURATION_LIBRARIES_PLAN.md
  docs/CONFIG_VALIDATION.md
  docs/CONFIG_EDIT.md
  docs/NETCONF_DATASTORE.md
  docs/NETCONF_SERVER.md
  docs/NETCONF_FRAMING.md
  docs/NETCONF_NOTIFICATIONS.md
  docs/NETCONF_TRANSPORT.md
  docs/NETCONF_WITH_DEFAULTS.md
  docs/NETCONF_FILTER_NACM_PERSISTENCE.md
  docs/DANGD_PLUGINS.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yang)
install(FILES examples/example.yang examples/library_example.cc
  examples/config_validation_example.cc
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yang/examples)

if(YANG_BUILD_DANGD)
  find_package(OpenSSL REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBSSH REQUIRED IMPORTED_TARGET libssh)
  add_library(dangd_core
    dangd/application.cc
    dangd/configuration_backend.cc
    dangd/hardware_transaction.cc
    dangd/plugin_manager.cc
    dangd/ssh_transport.cc
    dangd/tls_transport.cc)
  target_compile_features(dangd_core PUBLIC cxx_std_20)
  target_include_directories(dangd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(dangd_core PRIVATE
    DANGD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    DANGD_INSTALL_MODEL_DIR="${CMAKE_INSTALL_FULL_DATADIR}/doc/yang/dangd/models")
  target_link_libraries(dangd_core PUBLIC yang::yang OpenSSL::SSL OpenSSL::Crypto
    PkgConfig::LIBSSH ${CMAKE_DL_LIBS})
  target_compile_options(dangd_core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

  add_executable(dangd dangd/main.cc)
  target_compile_features(dangd PRIVATE cxx_std_20)
  target_link_libraries(dangd PRIVATE dangd_core)
  target_compile_options(dangd PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_executable(dangctl dangd/client_main.cc)
  target_compile_features(dangctl PRIVATE cxx_std_20)
  target_link_libraries(dangctl PRIVATE dangd_core)
  target_compile_options(dangctl PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_library(dangd_example_plugin MODULE dangd/plugins/example_plugin.cc)
  target_compile_features(dangd_example_plugin PRIVATE cxx_std_20)
  target_include_directories(dangd_example_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(dangd_example_plugin PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  set_target_properties(dangd_example_plugin PROPERTIES PREFIX "")
  file(READ tests/third_party/ietf/ietf-interfaces@2018-02-20.yang
    DANGD_IETF_INTERFACES_YANG)
  file(READ tests/third_party/ietf/ietf-ip@2018-02-22.yang
    DANGD_IETF_IP_YANG)
  configure_file(dangd/plugins/ip_management_models.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ip_management_models.h @ONLY)
  add_library(dangd_ip_management_plugin MODULE
    dangd/plugins/ip_management_plugin.cc)
  target_compile_features(dangd_ip_management_plugin PRIVATE cxx_std_20)
  target_include_directories(dangd_ip_management_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_link_libraries(dangd_ip_management_plugin PRIVATE
    nlohmann_json::nlohmann_json pugixml::pugixml)
  target_compile_options(dangd_ip_management_plugin PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  set_target_properties(dangd_ip_management_plugin PROPERTIES PREFIX "")
  if(YANG_BUILD_TESTS)
    add_library(dangd_test_plugin_support SHARED
      dangd/test_plugins/plugin_test_support.cc)
    target_compile_features(dangd_test_plugin_support PRIVATE cxx_std_20)
    target_include_directories(dangd_test_plugin_support PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(dangd_test_plugin_support PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    foreach(test_plugin IN ITEMS provider consumer)
      add_library(dangd_test_${test_plugin}_plugin MODULE
        dangd/test_plugins/${test_plugin}_plugin.cc)
      target_compile_features(dangd_test_${test_plugin}_plugin PRIVATE
        cxx_std_20)
      target_include_directories(dangd_test_${test_plugin}_plugin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
      target_link_libraries(dangd_test_${test_plugin}_plugin PRIVATE
        dangd_test_plugin_support)
      target_compile_options(dangd_test_${test_plugin}_plugin PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
      set_target_properties(dangd_test_${test_plugin}_plugin PROPERTIES
        PREFIX "")
    endforeach()
    add_library(dangd_test_broken_discovery_plugin MODULE
      dangd/test_plugins/broken_discovery_plugin.cc)
    target_include_directories(dangd_test_broken_discovery_plugin PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(dangd_test_broken_discovery_plugin PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    set_target_properties(dangd_test_broken_discovery_plugin PROPERTIES
      PREFIX "")
    add_library(dangd_test_deviation_plugin MODULE
      dangd/test_plugins/deviation_plugin.cc)
    target_include_directories(dangd_test_deviation_plugin PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(dangd_test_deviation_plugin PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    set_target_properties(dangd_test_deviation_plugin PROPERTIES PREFIX "")
  endif()
  install(TARGETS dangd dangctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(TARGETS dangd_example_plugin dangd_ip_management_plugin
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/dangd/plugins)
  install(FILES dangd/plugin_api.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dangd)
  install(FILES dangd/README.md dangd/CHANGELOG.md docs/DANGD_PLUGINS.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yang/dangd)
  install(DIRECTORY dangd/examples dangd/models dangd/testdata
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yang/dangd)
endif()

if(YANG_BUILD_TOOLS)
  add_executable(yangc src/yangc.cc)
  target_compile_features(yangc PRIVATE cxx_std_20)
  target_link_libraries(yangc PRIVATE yang::yang)
  target_compile_options(yangc PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  install(TARGETS yangc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  add_executable(yang_library_example examples/library_example.cc)
  target_compile_features(yang_library_example PRIVATE cxx_std_20)
  target_link_libraries(yang_library_example PRIVATE yang::yang)
  target_compile_options(yang_library_example PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_executable(yang_config_validation_example
    examples/config_validation_example.cc)
  target_compile_features(yang_config_validation_example PRIVATE cxx_std_20)
  target_link_libraries(yang_config_validation_example PRIVATE yang::yang)
  target_compile_options(yang_config_validation_example PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(YANG_BUILD_DOCS)
  find_package(Doxygen REQUIRED)
  find_program(YANG_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
  find_program(YANG_PDFLATEX_EXECUTABLE NAMES pdflatex REQUIRED)
  find_program(YANG_MAKEINDEX_EXECUTABLE NAMES makeindex REQUIRED)
  set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/docs)
  set(DOXYGEN_GENERATE_HTML YES)
  set(DOXYGEN_HTML_OUTPUT html)
  set(DOXYGEN_GENERATE_LATEX YES)
  set(DOXYGEN_LATEX_OUTPUT latex)
  set(DOXYGEN_USE_PDFLATEX YES)
  set(DOXYGEN_LATEX_CMD_NAME ${YANG_PDFLATEX_EXECUTABLE})
  set(DOXYGEN_MAKEINDEX_CMD_NAME ${YANG_MAKEINDEX_EXECUTABLE})
  set(DOXYGEN_PDF_HYPERLINKS YES)
  set(DOXYGEN_GENERATE_MAN NO)
  # Generate a complete public API index, including simple value members and
  # accessors whose names are self-describing, while retaining detailed
  # comments for contracts, ownership, failure modes, and host obligations.
  set(DOXYGEN_EXTRACT_ALL YES)
  set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
  set(DOXYGEN_WARN_AS_ERROR FAIL_ON_WARNINGS)
  set(DOXYGEN_WARN_LOGFILE
    ${CMAKE_CURRENT_BINARY_DIR}/docs/doxygen-warnings.log)
  doxygen_add_docs(yang_docs_generate include src dangd README.md TODO.md
    CHANGELOG.md docs)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/docs/latex/refman.pdf
    COMMAND ${YANG_MAKE_EXECUTABLE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/docs/latex
    DEPENDS yang_docs_generate
    COMMENT "Building Doxygen PDF documentation"
    VERBATIM)
  add_custom_target(yang_docs ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/docs/latex/refman.pdf)
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/docs/html/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yang/html)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/docs/latex/refman.pdf
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yang
    RENAME yang-cpp-reference.pdf)
endif()

if(YANG_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  find_package(Threads REQUIRED)
  add_executable(yang_tests
    tests/lexer_test.cc
    tests/parser_test.cc
    tests/resource_limits_test.cc
    tests/schema_registry_test.cc
    tests/source_file_test.cc
    tests/string_decoder_test.cc
    tests/syntax_tree_test.cc
    tests/utf8_test.cc)
  target_sources(yang_tests PRIVATE tests/concurrency_stress_test.cc tests/config_edit_test.cc tests/config_validation_test.cc tests/conformance_test.cc tests/deviation_test.cc tests/extension_test.cc tests/feature_schema_test.cc tests/identity_feature_test.cc tests/leafref_test.cc tests/module_resolver_test.cc tests/nacm_test.cc tests/netconf_datastore_test.cc tests/netconf_filter_test.cc tests/netconf_framing_test.cc tests/netconf_notifications_test.cc tests/netconf_persistence_test.cc tests/netconf_server_test.cc tests/netconf_session_registry_test.cc tests/netconf_transport_test.cc tests/netconf_with_defaults_test.cc tests/schema_path_test.cc tests/schema_tree_test.cc tests/schema_value_test.cc tests/semantic_model_test.cc tests/type_system_test.cc tests/validator_test.cc tests/xpath_test.cc tests/yin_document_test.cc)
  target_compile_definitions(yang_tests PRIVATE
    YANG_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(yang_tests PRIVATE yang::yang GTest::gtest_main Threads::Threads)
  include(GoogleTest)
  if(YANG_ENABLE_SANITIZERS AND APPLE)
    # Homebrew's prebuilt GoogleTest is not ASan-instrumented. Defer discovery
    # so libc++ container annotations can be disabled by the sanitizer preset.
    gtest_discover_tests(yang_tests DISCOVERY_MODE PRE_TEST)
  else()
    gtest_discover_tests(yang_tests)
  endif()

  if(YANG_BUILD_DANGD)
    add_executable(dangd_tests dangd/application_test.cc
      dangd/hardware_transaction_test.cc dangd/plugin_manager_test.cc
      dangd/ssh_transport_test.cc
      dangd/tls_transport_test.cc)
    target_compile_features(dangd_tests PRIVATE cxx_std_20)
    target_link_libraries(dangd_tests PRIVATE dangd_core
      dangd_test_plugin_support GTest::gtest_main Threads::Threads)
    target_compile_definitions(dangd_tests PRIVATE
      DANG_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
      DANG_TEST_PLUGIN_PATH="$<TARGET_FILE:dangd_example_plugin>"
      DANG_TEST_IP_PLUGIN_PATH="$<TARGET_FILE:dangd_ip_management_plugin>"
      DANG_TEST_PROVIDER_PLUGIN_PATH="$<TARGET_FILE:dangd_test_provider_plugin>"
      DANG_TEST_CONSUMER_PLUGIN_PATH="$<TARGET_FILE:dangd_test_consumer_plugin>"
      DANG_TEST_BROKEN_PLUGIN_PATH="$<TARGET_FILE:dangd_test_broken_discovery_plugin>")
    target_compile_definitions(dangd_tests PRIVATE
      DANG_TEST_DEVIATION_PLUGIN_PATH="$<TARGET_FILE:dangd_test_deviation_plugin>")
    add_dependencies(dangd_tests dangd_example_plugin dangd_ip_management_plugin
      dangd_test_provider_plugin dangd_test_consumer_plugin
      dangd_test_broken_discovery_plugin dangd_test_deviation_plugin)
    target_compile_options(dangd_tests PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(YANG_ENABLE_SANITIZERS AND APPLE)
      gtest_discover_tests(dangd_tests DISCOVERY_MODE PRE_TEST)
    else()
      gtest_discover_tests(dangd_tests)
    endif()
  endif()
endif()

if(YANG_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang")
    message(FATAL_ERROR "YANG_BUILD_FUZZERS requires Clang libFuzzer support")
  endif()
  add_executable(yang_frontend_fuzz tests/fuzz/frontend_fuzz.cc)
  add_executable(yang_protocol_fuzz tests/fuzz/protocol_fuzz.cc)
  if(YANG_FUZZ_STANDALONE)
    target_sources(yang_frontend_fuzz PRIVATE tests/fuzz/standalone_fuzz_main.cc)
    target_sources(yang_protocol_fuzz PRIVATE tests/fuzz/standalone_fuzz_main.cc)
  endif()
  foreach(fuzz_target IN ITEMS yang_frontend_fuzz yang_protocol_fuzz)
    target_compile_features(${fuzz_target} PRIVATE cxx_std_20)
    target_link_libraries(${fuzz_target} PRIVATE yang::yang)
  endforeach()
  if(NOT YANG_FUZZ_STANDALONE)
    foreach(fuzz_target IN ITEMS yang_frontend_fuzz yang_protocol_fuzz)
      target_compile_options(${fuzz_target} PRIVATE
        -fsanitize=fuzzer -fno-omit-frame-pointer)
      target_link_options(${fuzz_target} PRIVATE
        -fsanitize=fuzzer -fno-omit-frame-pointer)
    endforeach()
  endif()
endif()

if(YANG_BUILD_BENCHMARKS)
  add_executable(yang_benchmarks benchmarks/benchmark_main.cc)
  target_compile_features(yang_benchmarks PRIVATE cxx_std_20)
  target_link_libraries(yang_benchmarks PRIVATE yang::yang)
  target_compile_options(yang_benchmarks PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
