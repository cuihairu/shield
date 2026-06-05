#define BOOST_TEST_MODULE ServiceHandleTest
#include <boost/test/unit_test.hpp>

#include "shield/service/service_handle.hpp"

using namespace shield::service;

BOOST_AUTO_TEST_SUITE(ServiceHandleTests)

BOOST_AUTO_TEST_CASE(TestDefaultConstructor) {
    ServiceHandle handle;
    BOOST_CHECK(!handle);
    BOOST_CHECK(!handle.valid());
    BOOST_CHECK(handle.is_local());
    BOOST_CHECK(handle.name().empty());
}

BOOST_AUTO_TEST_CASE(TestFromStringInvalidPrefix) {
    auto handle = ServiceHandle::from_string("http://invalid");
    BOOST_CHECK(!handle);
    BOOST_CHECK(!handle.valid());
}

BOOST_AUTO_TEST_CASE(TestFromStringWithHash) {
    auto handle = ServiceHandle::from_string("shield://node1/123#my_svc");
    BOOST_CHECK_EQUAL(handle.name(), "my_svc");
}

BOOST_AUTO_TEST_CASE(TestFromStringWithoutHash) {
    auto handle = ServiceHandle::from_string("shield://node1/123");
    BOOST_CHECK(handle.name().empty());
}

BOOST_AUTO_TEST_CASE(TestFromStringEmpty) {
    auto handle = ServiceHandle::from_string("");
    BOOST_CHECK(!handle);
}

BOOST_AUTO_TEST_CASE(TestToStringInvalid) {
    ServiceHandle handle;
    BOOST_CHECK_EQUAL(handle.to_string(), "shield://invalid");
}

BOOST_AUTO_TEST_CASE(TestToStringWithEmptyActor) {
    // Default-constructed handle has empty actor
    ServiceHandle handle(caf::actor{}, "test_svc", true);
    // to_string returns "shield://invalid" because handle_ is empty
    BOOST_CHECK_EQUAL(handle.to_string(), "shield://invalid");
}

BOOST_AUTO_TEST_CASE(TestIsLocal) {
    ServiceHandle handle(caf::actor{}, "svc", true);
    BOOST_CHECK(handle.is_local());

    ServiceHandle remote(caf::actor{}, "svc", false);
    BOOST_CHECK(!remote.is_local());
}

BOOST_AUTO_TEST_CASE(TestNameAccessor) {
    ServiceHandle handle(caf::actor{}, "my_service", true);
    BOOST_CHECK_EQUAL(handle.name(), "my_service");
}

BOOST_AUTO_TEST_CASE(TestCafHandleAccessor) {
    caf::actor empty_actor;
    ServiceHandle handle(empty_actor, "svc", true);
    // The caf_handle should be the same empty actor
    BOOST_CHECK(!handle.caf_handle());
}

BOOST_AUTO_TEST_CASE(TestOperatorCafActor) {
    ServiceHandle handle(caf::actor{}, "svc", true);
    caf::actor converted = handle;
    BOOST_CHECK(!converted);
}

BOOST_AUTO_TEST_SUITE_END()
