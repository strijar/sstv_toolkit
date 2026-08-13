cmake_minimum_required(VERSION 3.16)
project(sstv_toolkit VERSION 1.0.0 LANGUAGES C)

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIQUID QUIET IMPORTED_TARGET liquid)
endif()
if(NOT TARGET PkgConfig::LIQUID)
    find_path(LIQUID_INCLUDE_DIR liquid/liquid.h REQUIRED)
    find_library(LIQUID_LIBRARY NAMES liquid REQUIRED)
    add_library(liquid_external UNKNOWN IMPORTED)
    set_target_properties(liquid_external PROPERTIES
        IMPORTED_LOCATION "${LIQUID_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIQUID_INCLUDE_DIR}"
    )
    add_library(PkgConfig::LIQUID ALIAS liquid_external)
endif()

add_library(sstv
    src/color.c
    src/decoder.c
    src/encoder.c
    src/goertzel.c
    src/iq_file.c
    src/modes.c
)
add_library(SSTV::sstv ALIAS sstv)
target_compile_features(sstv PUBLIC c_std_99)
target_include_directories(sstv
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(sstv PUBLIC PkgConfig::LIQUID m)
set_target_properties(sstv PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

add_executable(sstv_decoder tools/sstv_decode.c)
target_link_libraries(sstv_decoder PRIVATE SSTV::sstv)
add_executable(sstv_encoder tools/sstv_encode.c)
target_link_libraries(sstv_encoder PRIVATE SSTV::sstv)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sstv PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(sstv_decoder PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(sstv_encoder PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

if(BUILD_TESTING)
    add_executable(test_stream tests/test_stream.c)
    target_link_libraries(test_stream PRIVATE SSTV::sstv)
    add_executable(test_roundtrip tests/test_roundtrip.c)
    target_link_libraries(test_roundtrip PRIVATE SSTV::sstv)
    add_test(NAME stream_two_frames
             COMMAND test_stream double ${CMAKE_CURRENT_SOURCE_DIR}/demo/scene_M1.iq)
    add_test(NAME stream_truncated_frame
             COMMAND test_stream truncated ${CMAKE_CURRENT_SOURCE_DIR}/demo/scene_M1.iq)
    add_test(NAME first_line_alignment
             COMMAND test_stream first-line ${CMAKE_CURRENT_SOURCE_DIR}/demo/colorbars_M1.iq)
    add_test(NAME encoder_decoder_roundtrip COMMAND test_roundtrip)
endif()

install(TARGETS sstv sstv_decoder sstv_encoder
    EXPORT SSTVTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT SSTVTargets
    FILE SSTVTargets.cmake
    NAMESPACE SSTV::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SSTV
)
configure_package_config_file(
    cmake/SSTVConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/SSTVConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SSTV
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/SSTVConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/SSTVConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/SSTVConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SSTV
)
