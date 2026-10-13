add_library(libSceCdlgPlayerReview SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceCdlgPlayerReview PRIVATE ${LIBS_INCLUDE_DIR})
configure_windows_unwind(libSceCdlgPlayerReview)

if(BUILD_TESTING AND NOT WIN32)
    add_test_executable(player_review_diagnostic_tests tests/Diagnostic.cpp)
    target_include_directories(player_review_diagnostic_tests PRIVATE ${LIBS_INCLUDE_DIR})
    # Load the actual patched provider and its patched dependencies together;
    # linking native libc here would preempt its NID-patched counterpart.
    target_link_libraries(player_review_diagnostic_tests PRIVATE ${CMAKE_DL_LIBS})
    add_custom_target(player_review_diagnostic_patched
        DEPENDS ${PATCHED_DIR}/libSceCdlgPlayerReview.prx)
    add_dependencies(player_review_diagnostic_tests player_review_diagnostic_patched)
    add_test(NAME player_review_diagnostic COMMAND player_review_diagnostic_tests
        ${PATCHED_DIR}/libSceCdlgPlayerReview.prx)
    set_tests_properties(player_review_diagnostic PROPERTIES TIMEOUT 15)
endif()
