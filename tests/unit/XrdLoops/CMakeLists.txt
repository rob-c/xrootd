add_executable(xrdsys-loop-tests IOEventsCoreTests.cc)
target_link_libraries(xrdsys-loop-tests
  XrdUtils GTest::gtest GTest::gtest_main)

add_executable(xrd-server-loop-tests ServerLoopTests.cc)
target_link_libraries(xrd-server-loop-tests
  XrdUtils GTest::gtest GTest::gtest_main)

add_executable(xrd-server-poll-loop-tests ServerPollPollTests.cc)
target_link_libraries(xrd-server-poll-loop-tests
  XrdUtils GTest::gtest GTest::gtest_main)
set_target_properties(xrd-server-poll-loop-tests PROPERTIES ENABLE_EXPORTS ON)

set(xrdloops_targets
  xrdsys-loop-tests xrd-server-loop-tests xrd-server-poll-loop-tests)
if(ENABLE_XRDCL)
  add_executable(xrdcl-loop-tests ClientTimeoutTests.cc)
  target_link_libraries(xrdcl-loop-tests
    XrdCl XrdUtils GTest::gtest GTest::gtest_main)
  list(APPEND xrdloops_targets xrdcl-loop-tests)
endif()

add_custom_target(xrdloops-unit-tests DEPENDS ${xrdloops_targets})

# The Linux harness pauses the real condition wait at the disputed
# interleaving, without adding hooks to the production libraries.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set_target_properties(xrd-server-loop-tests PROPERTIES ENABLE_EXPORTS ON)

  # XrdUtils uses epoll on Linux. Compile the poller translation unit into a
  # separate executable so the portable server loop receives equal coverage.
  target_sources(xrd-server-poll-loop-tests PRIVATE
    ../../../src/Xrd/XrdPoll.cc)
  target_compile_definitions(xrd-server-poll-loop-tests PRIVATE
    XRD_SERVER_FORCE_POLL)

  # Exercise the portable poll implementation on Linux as well. Reuse the
  # public API tests so backend-specific lock and descriptor state cannot hide.
  add_executable(xrdsys-poll-loop-tests
    IOEventsCoreTests.cc
    ../../../src/XrdSys/XrdSysIOEvents.cc
    ../../../src/XrdSys/XrdSysE2T.cc
    ../../../src/XrdSys/XrdSysError.cc
    ../../../src/XrdSys/XrdSysLogger.cc
    ../../../src/XrdSys/XrdSysLogging.cc
    ../../../src/XrdSys/XrdSysPthread.cc
    ../../../src/XrdSys/XrdSysTimer.cc
    ../../../src/XrdSys/XrdSysUtils.cc)
  target_compile_definitions(xrdsys-poll-loop-tests PRIVATE
    XRD_SYS_IOEVENTS_FORCE_POLL)
  target_link_libraries(xrdsys-poll-loop-tests
    GTest::gtest GTest::gtest_main)
  add_dependencies(xrdloops-unit-tests xrdsys-poll-loop-tests)
  gtest_discover_tests(xrdsys-poll-loop-tests
    TEST_PREFIX "PollFallback." DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 30)
endif()

file(STRINGS ../../../src/XrdSys/XrdSysIOEvents.cc ioevents_port_hook
  REGEX "XRD_SYS_IOEVENTS_FORCE_PORT_REARM" LIMIT_COUNT 1)
if(ioevents_port_hook)
  add_executable(xrdsys-port-loop-tests
    IOEventsCoreTests.cc ../../../src/XrdSys/XrdSysIOEvents.cc)
  target_compile_definitions(xrdsys-port-loop-tests PRIVATE
    XRD_SYS_IOEVENTS_FORCE_PORT_REARM)
  target_link_libraries(xrdsys-port-loop-tests
    XrdUtils GTest::gtest GTest::gtest_main)
  add_dependencies(xrdloops-unit-tests xrdsys-port-loop-tests)
  gtest_discover_tests(xrdsys-port-loop-tests
    TEST_PREFIX "PortContract." TEST_FILTER "IOEventsCore.EventPort*"
    DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 30)
endif()

foreach(target ${xrdloops_targets})
  gtest_discover_tests(${target} DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 30)
endforeach()

# Run the ownership transitions once with tracing enabled. The SetFD case
# covers the isChanged status name; the self-delete case proves tracing never
# reads the channel after the callback destroys it.
add_test(NAME IOEventsCore.CallbackOwnership.Trace
  COMMAND $<TARGET_FILE:xrdsys-loop-tests>
    --gtest_filter=IOEventsCore.SetFDPreservesCallbackOwnershipUntilReturn:IOEventsCore.FatalCallbackCanDeleteItsOwnChannel)
set_tests_properties(
  IOEventsCore.CallbackOwnership.Trace
  PROPERTIES TIMEOUT 30 ENVIRONMENT "XrdSysIOE_TRACE=1")
