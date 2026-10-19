add_executable(xrdsecgsi-tests-unit rtag_test.cc eckey_test.cc)

target_link_libraries(xrdsecgsi-tests-unit
        XrdCrypto
        XrdUtils
        OpenSSL::Crypto
        GTest::gtest
        GTest::gtest_main
        )

gtest_discover_tests(xrdsecgsi-tests-unit
        TEST_PREFIX XrdSecgsi::
        PROPERTIES DISCOVERY_TIMEOUT 10)
