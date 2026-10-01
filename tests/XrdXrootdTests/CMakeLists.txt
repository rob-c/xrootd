# XrdXrootd helper-class unit tests.  XrdXrootdRedirHelper is compiled into
# the XrdServer shared library, so the tests are only built when XrdServer is
# being built (i.e. not in client-only configurations).
if(NOT TARGET XrdServer)
    return()
endif()

add_executable(xrdxrootd-redir-helper-tests XrdXrootdRedirHelperTests.cc)

target_link_libraries(xrdxrootd-redir-helper-tests
    XrdServer
    XrdUtils
    GTest::gtest
    GTest::gtest_main)

gtest_discover_tests(xrdxrootd-redir-helper-tests
    PROPERTIES DISCOVERY_TIMEOUT 10)

add_executable(xrdxrootd-parallel-close-tests XrdXrootdParallelCloseTests.cc)

target_link_libraries(xrdxrootd-parallel-close-tests
    XrdServer
    XrdUtils
    GTest::gtest
    GTest::gtest_main)

gtest_discover_tests(xrdxrootd-parallel-close-tests
    PROPERTIES DISCOVERY_TIMEOUT 10 TIMEOUT 10)
