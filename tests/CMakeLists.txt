file(GLOB MATA_LAZY_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

add_executable(mata_lazy_tests ${MATA_LAZY_TEST_SOURCES})

target_link_libraries(mata_lazy_tests PRIVATE mata_lazy Catch2::Catch2WithMain)

catch_discover_tests(mata_lazy_tests)
