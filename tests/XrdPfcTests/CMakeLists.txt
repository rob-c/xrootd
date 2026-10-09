add_executable(xrdpfc-unit-tests XrdPfcTests.cc)

target_link_libraries(xrdpfc-unit-tests GTest::gtest GTest::gtest_main)

gtest_discover_tests(xrdpfc-unit-tests
  PROPERTIES DISCOVERY_TIMEOUT 10)

add_executable(xrdpfc-read-pending-test XrdPfcReadPendingTest.cc)
target_link_libraries(xrdpfc-read-pending-test XrdUtils ${CMAKE_DL_LIBS})
add_dependencies(xrdpfc-read-pending-test XrdPfc-${PLUGIN_VERSION})

add_test(NAME XrdPfcReadPendingTest
  COMMAND xrdpfc-read-pending-test $<TARGET_FILE:XrdPfc-${PLUGIN_VERSION}>)

# Stock XRootD waits forever, so make a regression fail instead of hanging CTest.
set_tests_properties(XrdPfcReadPendingTest PROPERTIES TIMEOUT 5)
