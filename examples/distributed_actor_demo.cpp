#include "shield/actor/actor_system_coordinator.hpp"
#include "shield/core/logger.hpp"
#include "caf/all.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace std::chrono_literals;
using namespace shield::actor;

// Message types for game actors
struct player_join {
    std::string player_name;
    int level;
};

struct player_move {
    std::string player_name;
    int x, y;
};

struct room_status_request {};

struct room_status_response {
    std::string room_name;
    std::vector<std::string> players;
    int max_players;
};

// Template message inspection functions (must come before type ID definitions)
template <class Inspector>
bool inspect(Inspector& f, player_join& x) {
    return f.object(x).fields(f.field("player_name", x.player_name),
                             f.field("level", x.level));
}

template <class Inspector>
bool inspect(Inspector& f, player_move& x) {
    return f.object(x).fields(f.field("player_name", x.player_name),
                             f.field("x", x.x),
                             f.field("y", x.y));
}

template <class Inspector>
bool inspect(Inspector& f, room_status_request& x) {
    return f.object(x).fields();
}

template <class Inspector>
bool inspect(Inspector& f, room_status_response& x) {
    return f.object(x).fields(f.field("room_name", x.room_name),
                             f.field("players", x.players),
                             f.field("max_players", x.max_players));
}

// Type ID definitions (must come after struct and inspect function definitions)
CAF_BEGIN_TYPE_ID_BLOCK(game_actors, caf::first_custom_type_id)
  CAF_ADD_TYPE_ID(game_actors, (player_join))
  CAF_ADD_TYPE_ID(game_actors, (player_move))
  CAF_ADD_TYPE_ID(game_actors, (room_status_request))
  CAF_ADD_TYPE_ID(game_actors, (room_status_response))
CAF_END_TYPE_ID_BLOCK(game_actors)

// Player actor implementation
class PlayerActor : public caf::event_based_actor {
public:
    PlayerActor(caf::actor_config& cfg, const std::string& name, int level)
        : caf::event_based_actor(cfg), player_name_(name), level_(level) {
        
        SHIELD_LOG_INFO << "Player actor created: " << player_name_ << " (level " << level_ << ")";
    }

protected:
    caf::behavior make_behavior() override {
        return {
            [this](player_move& move) {
                SHIELD_LOG_INFO << "Player " << player_name_ << " moved to (" 
                               << move.x << ", " << move.y << ")";
                position_x_ = move.x;
                position_y_ = move.y;
            },
            [this](const std::string& message) {
                SHIELD_LOG_INFO << "Player " << player_name_ << " received message: " << message;
                return "Player " + player_name_ + " says: Received your message!";
            },
            [this](caf::get_atom) {
                return std::make_tuple(player_name_, level_, position_x_, position_y_);
            }
        };
    }

private:
    std::string player_name_;
    int level_;
    int position_x_ = 0;
    int position_y_ = 0;
};

// Room actor implementation
class RoomActor : public caf::event_based_actor {
public:
    RoomActor(caf::actor_config& cfg, const std::string& name, int max_players)
        : caf::event_based_actor(cfg), room_name_(name), max_players_(max_players) {
        
        SHIELD_LOG_INFO << "Room actor created: " << room_name_ << " (max players: " << max_players_ << ")";
    }

protected:
    caf::behavior make_behavior() override {
        return {
            [this](player_join& join) {
                if (players_.size() < static_cast<size_t>(max_players_)) {
                    players_.push_back(join.player_name);
                    SHIELD_LOG_INFO << "Player " << join.player_name << " joined room " << room_name_;
                    return std::string("Welcome to room " + room_name_ + ", " + join.player_name + "!");
                } else {
                    SHIELD_LOG_WARNING << "Room " << room_name_ << " is full, rejected " << join.player_name;
                    return std::string("Room " + room_name_ + " is full!");
                }
            },
            [this](room_status_request&) {
                room_status_response response;
                response.room_name = room_name_;
                response.players = players_;
                response.max_players = max_players_;
                return response;
            },
            [this](const std::string& broadcast_msg) {
                SHIELD_LOG_INFO << "Broadcasting in room " << room_name_ << ": " << broadcast_msg;
                return "Broadcast sent to " + std::to_string(players_.size()) + " players in " + room_name_;
            }
        };
    }

private:
    std::string room_name_;
    int max_players_;
    std::vector<std::string> players_;
};

// Gateway actor implementation
class GatewayActor : public caf::event_based_actor {
public:
    GatewayActor(caf::actor_config& cfg, ActorSystemCoordinator* coordinator)
        : caf::event_based_actor(cfg), coordinator_(coordinator) {
        
        SHIELD_LOG_INFO << "Gateway actor created";
    }

protected:
    caf::behavior make_behavior() override {
        return {
            [this](const std::string& command) {
                SHIELD_LOG_INFO << "Gateway received command: " << command;
                
                if (command == "list_players") {
                    auto players = coordinator_->find_actors_by_type(ActorType::LOGIC);
                    std::string result = "Found " + std::to_string(players.size()) + " player actors: ";
                    for (const auto& player : players) {
                        result += player.metadata.name + " ";
                    }
                    return result;
                } else if (command == "cluster_status") {
                    auto status = coordinator_->get_cluster_status();
                    std::string result = "Cluster Status: ";
                    for (const auto& [key, value] : status) {
                        result += key + "=" + value + " ";
                    }
                    return result;
                }
                
                return std::string("Unknown command: " + command);
            }
        };
    }

private:
    ActorSystemCoordinator* coordinator_;
};

void demonstrate_distributed_actors() {
    SHIELD_LOG_INFO << "=== Distributed Actor System Demo ===";

    // Create two coordinator instances to simulate different nodes
    auto coordinator1 = make_default_coordinator("game_node_1");
    auto coordinator2 = make_default_coordinator("game_node_2");

    // Initialize and start both coordinators
    if (!coordinator1->initialize() || !coordinator1->start()) {
        SHIELD_LOG_ERROR << "Failed to start coordinator1";
        return;
    }

    if (!coordinator2->initialize() || !coordinator2->start()) {
        SHIELD_LOG_ERROR << "Failed to start coordinator2";
        return;
    }

    SHIELD_LOG_INFO << "Both coordinators started successfully";

    // Create and register actors on node 1
    auto player1 = coordinator1->spawn_and_register<PlayerActor>(
        ActorType::LOGIC, "player_alice", "game_players", 
        {{"role", "warrior"}, {"guild", "dragons"}}, "Alice", 25);

    auto room1 = coordinator1->spawn_and_register<RoomActor>(
        ActorType::LOGIC, "room_tavern", "game_rooms",
        {{"type", "social"}, {"capacity", "10"}}, "The Tavern", 10);

    auto gateway1 = coordinator1->spawn_and_register<GatewayActor>(
        ActorType::GATEWAY, "gateway_main", "",
        {{"port", "8080"}}, coordinator1.get());

    // Create and register actors on node 2
    auto player2 = coordinator2->spawn_and_register<PlayerActor>(
        ActorType::LOGIC, "player_bob", "game_players",
        {{"role", "mage"}, {"guild", "wizards"}}, "Bob", 30);

    auto room2 = coordinator2->spawn_and_register<RoomActor>(
        ActorType::LOGIC, "room_dungeon", "game_rooms",
        {{"type", "combat"}, {"capacity", "4"}}, "Dark Dungeon", 4);

    if (!player1 || !room1 || !gateway1 || !player2 || !room2) {
        SHIELD_LOG_ERROR << "Failed to create some actors";
        return;
    }

    SHIELD_LOG_INFO << "All actors created and registered";

    // Wait a bit for discovery to work
    std::this_thread::sleep_for(2s);

    // Demonstrate cross-node communication
    SHIELD_LOG_INFO << "\n=== Testing Cross-Node Communication ===";

    // Node 1 tries to find actors from node 2
    auto found_player2 = coordinator1->find_actor("player_bob");
    if (found_player2) {
        SHIELD_LOG_INFO << "Node 1 successfully discovered player_bob from node 2";
        
        // Send message to remote actor
        coordinator1->send_to_actor("player_bob", std::string("Hello from Alice on node 1!"));
    } else {
        SHIELD_LOG_WARNING << "Node 1 could not find player_bob from node 2";
    }

    // Node 2 tries to find actors from node 1
    auto found_room1 = coordinator2->find_actor("room_tavern");
    if (found_room1) {
        SHIELD_LOG_INFO << "Node 2 successfully discovered room_tavern from node 1";
        
        // Send join message to remote room
        player_join join_msg{"Bob", 30};
        coordinator2->send_to_actor("room_tavern", join_msg);
    } else {
        SHIELD_LOG_WARNING << "Node 2 could not find room_tavern from node 1";
    }

    // Demonstrate broadcasting
    SHIELD_LOG_INFO << "\n=== Testing Broadcasting ===";
    
    size_t players_notified = coordinator1->broadcast_to_type(
        ActorType::LOGIC, std::string("Server announcement: Maintenance in 5 minutes!"));
    SHIELD_LOG_INFO << "Broadcast sent to " << players_notified << " logic actors";

    // Show cluster status
    SHIELD_LOG_INFO << "\n=== Cluster Status ===";
    auto status1 = coordinator1->get_cluster_status();
    auto status2 = coordinator2->get_cluster_status();
    
    SHIELD_LOG_INFO << "Node 1 status:";
    for (const auto& [key, value] : status1) {
        SHIELD_LOG_INFO << "  " << key << ": " << value;
    }
    
    SHIELD_LOG_INFO << "Node 2 status:";
    for (const auto& [key, value] : status2) {
        SHIELD_LOG_INFO << "  " << key << ": " << value;
    }

    // Demonstrate service group discovery
    SHIELD_LOG_INFO << "\n=== Service Group Discovery ===";
    auto game_players = coordinator1->get_distributed_system().find_actors_by_group("game_players");
    SHIELD_LOG_INFO << "Found " << game_players.size() << " actors in 'game_players' group:";
    for (const auto& actor : game_players) {
        SHIELD_LOG_INFO << "  - " << actor.metadata.name << " on node " << actor.metadata.node_id;
    }

    auto game_rooms = coordinator2->get_distributed_system().find_actors_by_group("game_rooms");
    SHIELD_LOG_INFO << "Found " << game_rooms.size() << " actors in 'game_rooms' group:";
    for (const auto& actor : game_rooms) {
        SHIELD_LOG_INFO << "  - " << actor.metadata.name << " on node " << actor.metadata.node_id;
    }

    // Let the system run for a bit to observe heartbeats and discovery
    SHIELD_LOG_INFO << "\n=== Running system for 10 seconds to observe heartbeats ===";
    std::this_thread::sleep_for(10s);

    // Graceful shutdown
    SHIELD_LOG_INFO << "\n=== Shutting down ===";
    coordinator1->stop();
    coordinator2->stop();
    
    SHIELD_LOG_INFO << "Demo completed successfully!";
}

int main() {
    // Initialize logging
    shield::core::LogConfig log_config;
    log_config.level = 1;  // Debug level
    log_config.console_output = true;
    shield::core::Logger::init(log_config);

    try {
        demonstrate_distributed_actors();
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Exception in demo: " << e.what();
        return 1;
    }

    shield::core::Logger::shutdown();
    return 0;
}