#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "caf/all.hpp"
#include "shield/actor/actor_registry.hpp"
#include "shield/actor/actor_system_coordinator.hpp"
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/core/logger.hpp"
#include "shield/discovery/local_discovery.hpp"

using namespace shield::actor;
using namespace shield::discovery;
using namespace std::chrono_literals;

// Simple test actor
class TestActor : public caf::event_based_actor {
public:
    TestActor(caf::actor_config &cfg, const std::string &name)
        : caf::event_based_actor(cfg), name_(name) {}

protected:
    caf::behavior make_behavior() override {
        return {[this](const std::string &msg) {
                    return "TestActor " + name_ + " received: " + msg;
                },
                [this](caf::get_atom) { return name_; }};
    }

private:
    std::string name_;
};

class DistributedActorSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize logging
        shield::core::LogConfig log_config;
        log_config.level = 2;  // Info level
        log_config.console_output = true;
        shield::core::Logger::init(log_config);

        // Create discovery service
        discovery_service_ = make_local_discovery();
        ASSERT_NE(discovery_service_, nullptr);

        // Create CAF actor system
        caf::actor_system_config cfg;
        cfg.set("scheduler.max-threads", 2);
        actor_system_ = std::make_unique<caf::actor_system>(cfg);

        // Create distributed actor system
        DistributedActorConfig config;
        config.node_id = "test_node_1";
        config.heartbeat_interval = 5s;
        config.discovery_interval = 3s;

        distributed_system_ = make_distributed_actor_system(
            *actor_system_, discovery_service_, config);
        ASSERT_NE(distributed_system_, nullptr);
    }

    void TearDown() override {
        if (distributed_system_) {
            distributed_system_->shutdown();
        }
        distributed_system_.reset();
        actor_system_.reset();
        discovery_service_.reset();
        shield::core::Logger::shutdown();
    }

    std::shared_ptr<IServiceDiscovery> discovery_service_;
    std::unique_ptr<caf::actor_system> actor_system_;
    std::unique_ptr<DistributedActorSystem> distributed_system_;
};

TEST_F(DistributedActorSystemTest, Initialization) {
    EXPECT_TRUE(distributed_system_->initialize());
    EXPECT_TRUE(distributed_system_->is_healthy());
    EXPECT_EQ(distributed_system_->get_node_id(), "test_node_1");
}

TEST_F(DistributedActorSystemTest, ActorRegistration) {
    ASSERT_TRUE(distributed_system_->initialize());

    // Create and register a test actor
    auto test_actor = actor_system_->spawn<TestActor>("test_actor_1");
    ASSERT_TRUE(test_actor);

    bool success = distributed_system_->register_actor(
        test_actor, ActorType::LOGIC, "test_actor_1", "test_group");
    EXPECT_TRUE(success);

    // Try to find the actor locally
    auto found_actor = distributed_system_->find_actor("test_actor_1");
    EXPECT_TRUE(found_actor);

    // Test sending a message
    auto response = caf::anon_send_request<std::string>(
        test_actor, std::string("Hello World"));
    // Note: In a real test, you'd use proper CAF testing utilities
}

TEST_F(DistributedActorSystemTest, ActorDiscovery) {
    ASSERT_TRUE(distributed_system_->initialize());

    // Register multiple actors of different types
    auto logic_actor = actor_system_->spawn<TestActor>("logic_1");
    auto gateway_actor = actor_system_->spawn<TestActor>("gateway_1");

    EXPECT_TRUE(distributed_system_->register_actor(
        logic_actor, ActorType::LOGIC, "logic_1"));
    EXPECT_TRUE(distributed_system_->register_actor(
        gateway_actor, ActorType::GATEWAY, "gateway_1"));

    // Find actors by type
    auto logic_actors =
        distributed_system_->find_actors_by_type(ActorType::LOGIC);
    EXPECT_EQ(logic_actors.size(), 1);
    EXPECT_EQ(logic_actors[0].metadata.name, "logic_1");

    auto gateway_actors =
        distributed_system_->find_actors_by_type(ActorType::GATEWAY);
    EXPECT_EQ(gateway_actors.size(), 1);
    EXPECT_EQ(gateway_actors[0].metadata.name, "gateway_1");
}

TEST_F(DistributedActorSystemTest, ServiceGroupDiscovery) {
    ASSERT_TRUE(distributed_system_->initialize());

    // Register actors in the same service group
    auto actor1 = actor_system_->spawn<TestActor>("player_1");
    auto actor2 = actor_system_->spawn<TestActor>("player_2");

    EXPECT_TRUE(distributed_system_->register_actor(
        actor1, ActorType::LOGIC, "player_1", "game_players"));
    EXPECT_TRUE(distributed_system_->register_actor(
        actor2, ActorType::LOGIC, "player_2", "game_players"));

    // Find actors by service group
    auto players = distributed_system_->find_actors_by_group("game_players");
    EXPECT_EQ(players.size(), 2);
}

TEST_F(DistributedActorSystemTest, ClusterStats) {
    ASSERT_TRUE(distributed_system_->initialize());

    // Register some actors
    auto actor1 = actor_system_->spawn<TestActor>("test_1");
    auto actor2 = actor_system_->spawn<TestActor>("test_2");

    distributed_system_->register_actor(actor1, ActorType::LOGIC, "test_1");
    distributed_system_->register_actor(actor2, ActorType::GATEWAY, "test_2");

    auto stats = distributed_system_->get_cluster_stats();
    EXPECT_EQ(stats.total_nodes, 1);
    EXPECT_EQ(stats.local_actors, 2);
    EXPECT_EQ(stats.remote_actors, 0);
    EXPECT_GE(stats.actors_by_type["logic"], 1);
    EXPECT_GE(stats.actors_by_type["gateway"], 1);
}

class ActorSystemCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        shield::core::LogConfig log_config;
        log_config.level = 2;
        log_config.console_output = true;
        shield::core::Logger::init(log_config);

        CoordinatorConfig config;
        config.node_id = "coordinator_test_node";
        config.discovery_type = "in-memory";
        config.worker_threads = 2;

        coordinator_ = std::make_unique<ActorSystemCoordinator>(config);
    }

    void TearDown() override {
        if (coordinator_) {
            coordinator_->stop();
        }
        coordinator_.reset();
        shield::core::Logger::shutdown();
    }

    std::unique_ptr<ActorSystemCoordinator> coordinator_;
};

TEST_F(ActorSystemCoordinatorTest, InitializationAndStart) {
    EXPECT_TRUE(coordinator_->initialize());
    EXPECT_TRUE(coordinator_->start());
    EXPECT_TRUE(coordinator_->is_running());

    coordinator_->stop();
    EXPECT_FALSE(coordinator_->is_running());
}

TEST_F(ActorSystemCoordinatorTest, ActorSpawnAndRegister) {
    ASSERT_TRUE(coordinator_->initialize());
    ASSERT_TRUE(coordinator_->start());

    // Spawn and register an actor
    auto actor = coordinator_->spawn_and_register<TestActor>(
        ActorType::LOGIC, "coord_test_actor", "test_group", {}, "TestName");
    EXPECT_TRUE(actor);

    // Find the actor
    auto found_actor = coordinator_->find_actor("coord_test_actor");
    EXPECT_TRUE(found_actor);
}

TEST_F(ActorSystemCoordinatorTest, ClusterStatus) {
    ASSERT_TRUE(coordinator_->initialize());
    ASSERT_TRUE(coordinator_->start());

    auto status = coordinator_->get_cluster_status();
    EXPECT_EQ(status["node_id"], "coordinator_test_node");
    EXPECT_EQ(status["initialized"], "true");
    EXPECT_EQ(status["running"], "true");
    EXPECT_EQ(status["discovery_type"], "in-memory");
}

// Integration test for multi-node scenario
class MultiNodeIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        shield::core::LogConfig log_config;
        log_config.level = 2;
        log_config.console_output = true;
        shield::core::Logger::init(log_config);

        // Create two coordinators for different nodes
        CoordinatorConfig config1;
        config1.node_id = "node_1";
        config1.discovery_type = "in-memory";
        coordinator1_ = std::make_unique<ActorSystemCoordinator>(config1);

        CoordinatorConfig config2;
        config2.node_id = "node_2";
        config2.discovery_type = "in-memory";
        coordinator2_ = std::make_unique<ActorSystemCoordinator>(config2);
    }

    void TearDown() override {
        if (coordinator1_) coordinator1_->stop();
        if (coordinator2_) coordinator2_->stop();
        coordinator1_.reset();
        coordinator2_.reset();
        shield::core::Logger::shutdown();
    }

    std::unique_ptr<ActorSystemCoordinator> coordinator1_;
    std::unique_ptr<ActorSystemCoordinator> coordinator2_;
};

TEST_F(MultiNodeIntegrationTest, DISABLED_CrossNodeDiscovery) {
    // This test is disabled because it requires proper network setup
    // In a real environment, you would:
    // 1. Set up shared discovery service (like etcd)
    // 2. Configure proper network endpoints
    // 3. Test cross-node actor discovery and communication

    ASSERT_TRUE(coordinator1_->initialize());
    ASSERT_TRUE(coordinator1_->start());

    ASSERT_TRUE(coordinator2_->initialize());
    ASSERT_TRUE(coordinator2_->start());

    // Register actor on node 1
    auto actor1 = coordinator1_->spawn_and_register<TestActor>(
        ActorType::LOGIC, "cross_node_actor", "shared_group", {}, "Node1Actor");
    EXPECT_TRUE(actor1);

    // Wait for discovery
    std::this_thread::sleep_for(2s);

    // Try to find actor from node 2
    auto found_actor = coordinator2_->find_actor("cross_node_actor");
    // This would work with proper network discovery service
    // EXPECT_TRUE(found_actor);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}