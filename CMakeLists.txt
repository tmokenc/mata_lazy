cmake_minimum_required(VERSION 3.15)
project(
    mata_lazy
    DESCRIPTION "Lazy on-the-fly emptiness checking for NFA / NFT relations — extension of libmata"
    LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(MATA_LAZY_BUILD_EXAMPLES "Build mata_lazy examples" ON)
option(MATA_LAZY_BUILD_TESTS "Build mata_lazy tests" ON)
option(MATA_LAZY_USE_SUBDIR_MATA "Build mata from a sibling source tree instead of find_package(mata)" OFF)
option(MATA_LAZY_BUNDLE "Build fat archive (.a) and shared library (.so) with libmata embedded — for non-CMake consumers that want a single artifact to link against" OFF)
set(MATA_LAZY_MATA_SUBDIR "../mata" CACHE PATH "Path to the mata source tree (used only when MATA_LAZY_USE_SUBDIR_MATA=ON)")
set(MATA_LAZY_MATA_FETCH_REPO "https://github.com/tmokenc/mata.git" CACHE STRING "Git URL used by the FetchContent fallback for mata")
set(MATA_LAZY_MATA_FETCH_TAG  "nft-per-level-alphabet"               CACHE STRING "Git ref (branch/tag/sha) for the FetchContent fallback for mata")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# -----------------------------------------------------------------------------
# Locate libmata. Resolution order:
#   1. MATA_LAZY_USE_SUBDIR_MATA=ON           → add_subdirectory on a local tree
#   2. find_package(mata) finds an install    → use it
#   3. fall back to FetchContent (clones the configured fork+ref)
# -----------------------------------------------------------------------------
set(MATA_LAZY_MATA_TARGET libmata)

if(MATA_LAZY_USE_SUBDIR_MATA)
    add_subdirectory(${MATA_LAZY_MATA_SUBDIR} mata_build EXCLUDE_FROM_ALL)
else()
    find_package(mata QUIET)
    if(NOT mata_FOUND)
        message(STATUS
            "mata_lazy: libmata not found locally; fetching "
            "${MATA_LAZY_MATA_FETCH_REPO}@${MATA_LAZY_MATA_FETCH_TAG} via FetchContent")
        include(FetchContent)
        FetchContent_Declare(
            mata
            GIT_REPOSITORY ${MATA_LAZY_MATA_FETCH_REPO}
            GIT_TAG        ${MATA_LAZY_MATA_FETCH_TAG}
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(mata)
    endif()
endif()

# -----------------------------------------------------------------------------
# Library
# -----------------------------------------------------------------------------
file(GLOB_RECURSE MATA_LAZY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")
file(GLOB_RECURSE MATA_LAZY_PRIVATE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hh")

add_library(mata_lazy STATIC
    ${MATA_LAZY_SOURCES}
    ${MATA_LAZY_PRIVATE_HEADERS}
)

target_compile_features(mata_lazy PUBLIC cxx_std_20)

target_include_directories(mata_lazy
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mata_lazy PUBLIC ${MATA_LAZY_MATA_TARGET})

# -----------------------------------------------------------------------------
# Examples
# -----------------------------------------------------------------------------
if(MATA_LAZY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
if((CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME) AND MATA_LAZY_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        include(FetchContent)
        if(FETCHCONTENT_FULLY_DISCONNECTED)
            find_package(Catch2 REQUIRED)
        else()
            FetchContent_Declare(
                Catch2
                GIT_REPOSITORY https://github.com/catchorg/Catch2.git
                GIT_TAG v3.11.0
            )
            FetchContent_MakeAvailable(Catch2)
            list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/contrib)
            get_target_property(catch2_includes Catch2 INTERFACE_INCLUDE_DIRECTORIES)
            target_include_directories(Catch2 SYSTEM INTERFACE ${catch2_includes})
        endif()
        include(Catch)

        add_subdirectory(tests)
    endif()
endif()

# -----------------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS mata_lazy
    EXPORT mata_lazyTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT mata_lazyTargets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mata_lazy
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/mata_lazyConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/mata_lazyConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mata_lazy
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/mata_lazyConfig.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mata_lazy
)

# -----------------------------------------------------------------------------
# Bundled artifacts: a single .a and .so containing mata_lazy + libmata + 3rdparty.
# Intended for non-CMake consumers that want one file to drop in and link.
# -----------------------------------------------------------------------------
if(MATA_LAZY_BUNDLE)
    if(NOT UNIX OR APPLE)
        message(FATAL_ERROR
            "MATA_LAZY_BUNDLE is implemented for Linux (GNU ar / GNU ld). "
            "macOS would need libtool -static and -Wl,-force_load instead.")
    endif()

    # --- Fat static archive: libmata_lazy_bundled.a -------------------------
    set(MATA_LAZY_BUNDLED_AR "${CMAKE_CURRENT_BINARY_DIR}/libmata_lazy_bundled.a")
    set(MATA_LAZY_BUNDLE_MRI "${CMAKE_CURRENT_BINARY_DIR}/bundle.mri")

    # GNU ar MRI script: create a fresh archive, addlib from each constituent
    # (which expands to all of that archive's members), save, end.
    file(GENERATE OUTPUT ${MATA_LAZY_BUNDLE_MRI} CONTENT
"create ${MATA_LAZY_BUNDLED_AR}
addlib $<TARGET_FILE:mata_lazy>
addlib $<TARGET_FILE:${MATA_LAZY_MATA_TARGET}>
save
end
")

    add_custom_command(
        OUTPUT ${MATA_LAZY_BUNDLED_AR}
        COMMAND ${CMAKE_AR} -M < ${MATA_LAZY_BUNDLE_MRI}
        DEPENDS mata_lazy ${MATA_LAZY_MATA_TARGET} ${MATA_LAZY_BUNDLE_MRI}
        COMMENT "Bundling libmata_lazy_bundled.a (mata_lazy + libmata + 3rdparty)"
        VERBATIM
    )
    add_custom_target(mata_lazy_bundled ALL DEPENDS ${MATA_LAZY_BUNDLED_AR})

    install(FILES ${MATA_LAZY_BUNDLED_AR}
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    # --- Fat shared library: libmata_lazy.so --------------------------------
    # Compile the same sources -fPIC (already on via CMAKE_POSITION_INDEPENDENT_CODE)
    # and pull libmata.a in whole-archive so its symbols are baked in.
    add_library(mata_lazy_shared SHARED
        ${MATA_LAZY_SOURCES}
        ${MATA_LAZY_PRIVATE_HEADERS}
    )
    target_compile_features(mata_lazy_shared PUBLIC cxx_std_20)
    target_include_directories(mata_lazy_shared
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    set_target_properties(mata_lazy_shared PROPERTIES
        OUTPUT_NAME mata_lazy
        POSITION_INDEPENDENT_CODE ON
    )

    # Force the entire libmata.a into the .so. Order matters: --whole-archive,
    # then the archive path, then --no-whole-archive (so any system libs that
    # follow are not whole-archived).
    target_link_options(mata_lazy_shared PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:${MATA_LAZY_MATA_TARGET}>,--no-whole-archive"
    )
    add_dependencies(mata_lazy_shared ${MATA_LAZY_MATA_TARGET})

    # Threads is libmata's transitive system dep.
    find_package(Threads REQUIRED)
    target_link_libraries(mata_lazy_shared PRIVATE Threads::Threads)

    install(TARGETS mata_lazy_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
