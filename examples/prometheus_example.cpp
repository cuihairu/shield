#include <iostream>
#include <thread>
#include <chrono>
#include <random>

#include "shield/core/config.hpp"
#include "shield/metrics/prometheus_component.hpp"
#include "shield/metrics/metrics.hpp"

class GameServer {
public:
    GameServer() : running_(false), rng_(std::random_device{}()) {}
    
    void start() {
        running_ = true;
        
        // Start simulation threads
        player_simulation_thread_ = std::thread(&GameServer::simulate_players, this);
        network_simulation_thread_ = std::thread(&GameServer::simulate_network, this);
        room_simulation_thread_ = std::thread(&GameServer::simulate_rooms, this);
        
        std::cout << "Game server started with Prometheus monitoring" << std::endl;
        std::cout << "Metrics available at: http://localhost:9090/metrics" << std::endl;
    }
    
    void stop() {
        running_ = false;
        
        if (player_simulation_thread_.joinable()) {
            player_simulation_thread_.join();
        }
        if (network_simulation_thread_.joinable()) {
            network_simulation_thread_.join();
        }
        if (room_simulation_thread_.joinable()) {
            room_simulation_thread_.join();
        }
    }

private:
    void simulate_players() {
        int player_count = 0;
        while (running_) {
            // Simulate players joining/leaving
            if (player_count < 100 && std::uniform_int_distribution<>(0, 10)(rng_) < 3) {
                SHIELD_METRIC_INC_PLAYERS();
                player_count++;
                std::cout << "Player joined. Total: " << player_count << std::endl;
            } else if (player_count > 0 && std::uniform_int_distribution<>(0, 10)(rng_) < 2) {
                SHIELD_METRIC_DEC_PLAYERS();
                player_count--;
                std::cout << "Player left. Total: " << player_count << std::endl;
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    
    void simulate_network() {
        while (running_) {
            // Simulate network activity
            size_t bytes_sent = std::uniform_int_distribution<size_t>(100, 2000)(rng_);
            size_t bytes_received = std::uniform_int_distribution<size_t>(50, 1000)(rng_);
            
            SHIELD_METRIC_ADD_BYTES_SENT(bytes_sent);
            SHIELD_METRIC_ADD_BYTES_RECEIVED(bytes_received);
            SHIELD_METRIC_INC_REQUESTS();
            
            // Simulate some connections
            if (std::uniform_int_distribution<>(0, 10)(rng_) < 3) {
                SHIELD_METRIC_INC_CONNECTIONS();
            }
            if (std::uniform_int_distribution<>(0, 10)(rng_) < 2) {
                SHIELD_METRIC_DEC_CONNECTIONS();
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    
    void simulate_rooms() {
        int room_count = 0;
        while (running_) {
            // Simulate rooms being created/destroyed
            if (room_count < 20 && std::uniform_int_distribution<>(0, 10)(rng_) < 2) {
                SHIELD_METRIC_INC_ROOMS();
                room_count++;
                std::cout << "Room created. Total: " << room_count << std::endl;
            } else if (room_count > 0 && std::uniform_int_distribution<>(0, 10)(rng_) < 1) {
                SHIELD_METRIC_DEC_ROOMS();
                room_count--;
                std::cout << "Room destroyed. Total: " << room_count << std::endl;
            }
            
            // Simulate message processing
            int message_count = std::uniform_int_distribution<>(5, 50)(rng_);
            for (int i = 0; i < message_count; ++i) {
                SHIELD_METRIC_INC_MESSAGES();
            }
            
            // Simulate actor lifecycle
            if (std::uniform_int_distribution<>(0, 10)(rng_) < 4) {
                SHIELD_METRIC_INC_ACTORS_CREATED();
            }
            if (std::uniform_int_distribution<>(0, 10)(rng_) < 2) {
                SHIELD_METRIC_INC_ACTORS_DESTROYED();
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
    
    void simulate_request_processing() {
        SHIELD_METRIC_TIME_REQUEST();
        
        // Simulate varying request processing times
        auto processing_time = std::uniform_int_distribution<>(10, 500)(rng_);
        std::this_thread::sleep_for(std::chrono::milliseconds(processing_time));
    }

private:
    bool running_;
    std::thread player_simulation_thread_;
    std::thread network_simulation_thread_;
    std::thread room_simulation_thread_;
    std::mt19937 rng_;
};

int main() {
    try {
        // Load configuration
        auto& config = shield::core::Config::instance();
        
        // Create a simple config for the example
        std::string config_content = R"(
prometheus:
  enabled: true
  enable_exposer: true
  listen_address: "0.0.0.0"
  listen_port: 9090
  collection_interval: 5
  job_name: "shield-example"
  labels:
    service: "shield"
    environment: "example"
)";
        
        config.load_from_string(config_content);
        
        // Initialize and start Prometheus component
        auto& prometheus = shield::metrics::PrometheusComponent::instance();
        prometheus.init();
        prometheus.start();
        
        // Start game server simulation
        GameServer server;
        server.start();
        
        std::cout << "Press Enter to stop the server..." << std::endl;
        std::cin.get();
        
        // Stop everything
        server.stop();
        prometheus.stop();
        
        std::cout << "Server stopped." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}