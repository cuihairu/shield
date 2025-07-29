// tests/discovery/test_in_memory_discovery.cpp
#define BOOST_TEST_MODULE LocalDiscoveryTest

#include "shield/discovery/local_discovery.hpp"
#include <boost/test/unit_test.hpp>
#include <set>
#include <thread>
#include <vector>

using namespace shield::discovery;

BOOST_AUTO_TEST_SUITE(LocalDiscoveryTestSuite)

BOOST_AUTO_TEST_CASE(test_register_and_query_single_service) {
  auto discovery = make_local_discovery();
  ServiceInstance instance = {"auth-service", "instance-1",
                              "tcp://127.0.0.1:9001", ServiceMetadata{},
                              std::chrono::steady_clock::time_point::max()};

  BOOST_CHECK(discovery->register_service(instance));

  auto result = discovery->query_service("auth-service");
  BOOST_CHECK(result.has_value());
  BOOST_CHECK_EQUAL(result->instance_id, "instance-1");
  BOOST_CHECK_EQUAL(result->address, "tcp://127.0.0.1:9001");
}

BOOST_AUTO_TEST_CASE(test_query_non_existent_service) {
  auto discovery = make_local_discovery();
  auto result = discovery->query_service("non-existent-service");
  BOOST_CHECK(!result.has_value());
}

BOOST_AUTO_TEST_CASE(test_register_multiple_instances_and_query_all) {
  auto discovery = make_local_discovery();
  ServiceInstance instance1 = {"auth-service", "instance-1",
                               "tcp://127.0.0.1:9001", ServiceMetadata{},
                               std::chrono::steady_clock::time_point::max()};
  ServiceInstance instance2 = {"auth-service", "instance-2",
                               "tcp://127.0.0.1:9002", ServiceMetadata{},
                               std::chrono::steady_clock::time_point::max()};

  discovery->register_service(instance1);
  discovery->register_service(instance2);

  auto all_instances = discovery->query_all_services("auth-service");
  BOOST_CHECK_EQUAL(all_instances.size(), 2);

  std::set<std::string> ids;
  for (const auto &inst : all_instances) {
    ids.insert(inst.instance_id);
  }
  BOOST_CHECK(ids.count("instance-1"));
  BOOST_CHECK(ids.count("instance-2"));
}

BOOST_AUTO_TEST_CASE(test_deregister_service) {
  auto discovery = make_local_discovery();
  ServiceInstance instance1 = {"auth-service", "instance-1",
                               "tcp://127.0.0.1:9001", ServiceMetadata{},
                               std::chrono::steady_clock::time_point::max()};
  ServiceInstance instance2 = {"auth-service", "instance-2",
                               "tcp://127.0.0.1:9002", ServiceMetadata{},
                               std::chrono::steady_clock::time_point::max()};

  discovery->register_service(instance1);
  discovery->register_service(instance2);

  BOOST_CHECK_EQUAL(discovery->query_all_services("auth-service").size(), 2);

  BOOST_CHECK(discovery->deregister_service("auth-service", "instance-1"));

  auto remaining_instances = discovery->query_all_services("auth-service");
  BOOST_CHECK_EQUAL(remaining_instances.size(), 1);
  BOOST_CHECK_EQUAL(remaining_instances[0].instance_id, "instance-2");

  // Deregister the last one
  BOOST_CHECK(discovery->deregister_service("auth-service", "instance-2"));
  BOOST_CHECK(discovery->query_all_services("auth-service").empty());
  BOOST_CHECK(!discovery->query_service("auth-service").has_value());
}

BOOST_AUTO_TEST_CASE(test_thread_safety) {
  auto discovery = make_local_discovery();
  std::vector<std::thread> threads;
  int num_threads = 10;
  int services_per_thread = 100;

  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&, i]() {
      for (int j = 0; j < services_per_thread; ++j) {
        std::string service_name = "service-" + std::to_string(j);
        std::string instance_id =
            "instance-" + std::to_string(i) + "-" + std::to_string(j);
        ServiceInstance instance = {
            service_name, instance_id, "addr", ServiceMetadata{},
            std::chrono::steady_clock::time_point::max()};
        discovery->register_service(instance);
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  // Check if all services were registered correctly
  for (int j = 0; j < services_per_thread; ++j) {
    std::string service_name = "service-" + std::to_string(j);
    auto instances = discovery->query_all_services(service_name);
    BOOST_CHECK_EQUAL(instances.size(), num_threads);
  }
}

BOOST_AUTO_TEST_CASE(test_ttl_expiration) {
  auto discovery = make_local_discovery();
  ServiceInstance instance1 = {"test-service", "instance-ttl-1",
                               "tcp://127.0.0.1:9001", ServiceMetadata{},
                               std::chrono::steady_clock::time_point::max()};
  ServiceInstance instance2 = {"test-service", "instance-ttl-2",
                               "tcp://127.0.0.1:9002", ServiceMetadata{},
                               std::chrono::steady_clock::time_point::max()};

  // Register instance1 with a short TTL (e.g., 1 second)
  BOOST_CHECK(discovery->register_service(instance1, std::chrono::seconds(1)));
  // Register instance2 without TTL (should be permanent)
  BOOST_CHECK(discovery->register_service(instance2));

  // Initially, both should be present
  BOOST_CHECK_EQUAL(discovery->query_all_services("test-service").size(), 2);

  // Wait for TTL to expire (e.g., 2 seconds to be safe)
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // After expiration, only instance2 should remain
  auto remaining_instances = discovery->query_all_services("test-service");
  BOOST_CHECK_EQUAL(remaining_instances.size(), 1);
  BOOST_CHECK_EQUAL(remaining_instances[0].instance_id, "instance-ttl-2");

  // Register another instance with a short TTL and check if it expires
  ServiceInstance instance3 = {"test-service", "instance-ttl-3",
                               "tcp://127.0.0.1:9003", ServiceMetadata{},
                               std::chrono::steady_clock::time_point::max()};
  BOOST_CHECK(discovery->register_service(instance3, std::chrono::seconds(1)));
  BOOST_CHECK_EQUAL(discovery->query_all_services("test-service").size(), 2);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  BOOST_CHECK_EQUAL(discovery->query_all_services("test-service").size(), 1);
  BOOST_CHECK_EQUAL(
      discovery->query_all_services("test-service")[0].instance_id,
      "instance-ttl-2");
}

BOOST_AUTO_TEST_CASE(test_persistence) {
  // Define a temporary file path for persistence
  std::string test_file_path = "test_local_discovery_persistence.json";

  // Create a discovery instance with persistence enabled
  auto discovery1 =
      make_local_discovery(std::chrono::seconds(1), test_file_path);

  ServiceInstance instance1 = {"persisted-service", "p-instance-1",
                               "tcp://127.0.0.1:9001", ServiceMetadata{},
                               std::chrono::steady_clock::time_point::max()};
  ServiceInstance instance2 = {"persisted-service", "p-instance-2",
                               "tcp://127.0.0.1:9002", ServiceMetadata{},
                               std::chrono::steady_clock::time_point::max()};

  BOOST_CHECK(discovery1->register_service(instance1));
  BOOST_CHECK(discovery1->register_service(instance2));

  BOOST_CHECK_EQUAL(discovery1->query_all_services("persisted-service").size(),
                    2);

  // Allow time for persistence to write to file
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // Destroy the first discovery instance (data should be saved)
  discovery1.reset();

  // Create a new discovery instance, loading from the same file
  auto discovery2 =
      make_local_discovery(std::chrono::seconds(1), test_file_path);

  // Check if services are loaded correctly
  auto loaded_instances = discovery2->query_all_services("persisted-service");
  BOOST_CHECK_EQUAL(loaded_instances.size(), 2);
  std::set<std::string> ids;
  for (const auto &inst : loaded_instances) {
    ids.insert(inst.instance_id);
  }
  BOOST_CHECK(ids.count("p-instance-1"));
  BOOST_CHECK(ids.count("p-instance-2"));

  // Clean up the test file
  std::remove(test_file_path.c_str());
}

BOOST_AUTO_TEST_SUITE_END()
