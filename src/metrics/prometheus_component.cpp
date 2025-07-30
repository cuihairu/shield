#include "shield/metrics/prometheus_component.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include "shield/config/config.hpp"

#ifdef SHIELD_ENABLE_PROMETHEUS
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#elif __linux__
#include <sys/sysinfo.h>
#include <unistd.h>
#endif
#endif

namespace shield::metrics {

#ifdef SHIELD_ENABLE_PROMETHEUS

// SystemMetricsCollector implementation
SystemMetricsCollector::SystemMetricsCollector(
    std::shared_ptr<prometheus::Registry> registry) {
    // CPU usage gauge
    cpu_usage_family_ = &prometheus::BuildGauge()
                             .Name("shield_cpu_usage_percent")
                             .Help("CPU usage percentage")
                             .Register(*registry);
    cpu_usage_gauge_ = &cpu_usage_family_->Add({});

    // Memory usage gauge
    memory_usage_family_ = &prometheus::BuildGauge()
                                .Name("shield_memory_usage_bytes")
                                .Help("Memory usage in bytes")
                                .Register(*registry);
    memory_usage_gauge_ = &memory_usage_family_->Add({});

    // Total memory gauge
    memory_total_family_ = &prometheus::BuildGauge()
                                .Name("shield_memory_total_bytes")
                                .Help("Total memory in bytes")
                                .Register(*registry);
    memory_total_gauge_ = &memory_total_family_->Add({});
}

void SystemMetricsCollector::collect() {
    // Update CPU usage
    double cpu_usage = get_cpu_usage();
    cpu_usage_gauge_->Set(cpu_usage);

    // Update memory usage
    auto [memory_used, memory_total] = get_memory_usage();
    memory_usage_gauge_->Set(static_cast<double>(memory_used));
    memory_total_gauge_->Set(static_cast<double>(memory_total));
}

double SystemMetricsCollector::get_cpu_usage() {
#ifdef __APPLE__
    host_cpu_load_info_data_t cpuinfo;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;

    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&cpuinfo, &count) == KERN_SUCCESS) {
        unsigned long long totalTicks = 0;
        for (int i = 0; i < CPU_STATE_MAX; i++) {
            totalTicks += cpuinfo.cpu_ticks[i];
        }
        return static_cast<double>(cpuinfo.cpu_ticks[CPU_STATE_USER] +
                                   cpuinfo.cpu_ticks[CPU_STATE_SYSTEM]) /
               totalTicks * 100.0;
    }
#elif __linux__
    std::ifstream file("/proc/stat");
    std::string line;
    if (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string cpu;
        long user, nice, system, idle;
        iss >> cpu >> user >> nice >> system >> idle;
        long total = user + nice + system + idle;
        return static_cast<double>(user + system) / total * 100.0;
    }
#endif
    return 0.0;
}

std::pair<size_t, size_t> SystemMetricsCollector::get_memory_usage() {
#ifdef __APPLE__
    vm_size_t page_size;
    vm_statistics64_data_t vm_stat;
    mach_msg_type_number_t count = sizeof(vm_stat) / sizeof(natural_t);

    if (host_page_size(mach_host_self(), &page_size) == KERN_SUCCESS &&
        host_statistics64(mach_host_self(), HOST_VM_INFO,
                          (host_info64_t)&vm_stat, &count) == KERN_SUCCESS) {
        size_t total_memory = (vm_stat.free_count + vm_stat.active_count +
                               vm_stat.inactive_count + vm_stat.wire_count) *
                              page_size;
        size_t used_memory = (vm_stat.active_count + vm_stat.inactive_count +
                              vm_stat.wire_count) *
                             page_size;
        return {used_memory, total_memory};
    }
#elif __linux__
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        size_t total_memory = info.totalram * info.mem_unit;
        size_t used_memory = (info.totalram - info.freeram) * info.mem_unit;
        return {used_memory, total_memory};
    }
#endif
    return {0, 0};
}

// NetworkMetricsCollector implementation
NetworkMetricsCollector::NetworkMetricsCollector(
    std::shared_ptr<prometheus::Registry> registry) {
    // Active connections gauge
    active_connections_family_ = &prometheus::BuildGauge()
                                      .Name("shield_active_connections")
                                      .Help("Number of active connections")
                                      .Register(*registry);
    active_connections_gauge_ = &active_connections_family_->Add({});

    // Bytes sent counter
    bytes_sent_family_ = &prometheus::BuildCounter()
                              .Name("shield_bytes_sent_total")
                              .Help("Total bytes sent")
                              .Register(*registry);
    bytes_sent_counter_ = &bytes_sent_family_->Add({});

    // Bytes received counter
    bytes_received_family_ = &prometheus::BuildCounter()
                                  .Name("shield_bytes_received_total")
                                  .Help("Total bytes received")
                                  .Register(*registry);
    bytes_received_counter_ = &bytes_received_family_->Add({});

    // Total requests counter
    total_requests_family_ = &prometheus::BuildCounter()
                                  .Name("shield_requests_total")
                                  .Help("Total number of requests")
                                  .Register(*registry);
    total_requests_counter_ = &total_requests_family_->Add({});

    // Request duration histogram
    request_duration_family_ = &prometheus::BuildHistogram()
                                    .Name("shield_request_duration_seconds")
                                    .Help("Request duration in seconds")
                                    .Register(*registry);
    request_duration_histogram_ = &request_duration_family_->Add(
        {},
        prometheus::Histogram::BucketBoundaries{0.001, 0.01, 0.1, 1.0, 10.0});

    // UDP-specific metrics
    active_udp_sessions_family_ = &prometheus::BuildGauge()
                                       .Name("shield_active_udp_sessions")
                                       .Help("Number of active UDP sessions")
                                       .Register(*registry);
    active_udp_sessions_gauge_ = &active_udp_sessions_family_->Add({});

    udp_packets_sent_family_ = &prometheus::BuildCounter()
                                    .Name("shield_udp_packets_sent_total")
                                    .Help("Total UDP packets sent")
                                    .Register(*registry);
    udp_packets_sent_counter_ = &udp_packets_sent_family_->Add({});

    udp_packets_received_family_ =
        &prometheus::BuildCounter()
             .Name("shield_udp_packets_received_total")
             .Help("Total UDP packets received")
             .Register(*registry);
    udp_packets_received_counter_ = &udp_packets_received_family_->Add({});

    udp_bytes_sent_family_ = &prometheus::BuildCounter()
                                  .Name("shield_udp_bytes_sent_total")
                                  .Help("Total UDP bytes sent")
                                  .Register(*registry);
    udp_bytes_sent_counter_ = &udp_bytes_sent_family_->Add({});

    udp_bytes_received_family_ = &prometheus::BuildCounter()
                                      .Name("shield_udp_bytes_received_total")
                                      .Help("Total UDP bytes received")
                                      .Register(*registry);
    udp_bytes_received_counter_ = &udp_bytes_received_family_->Add({});

    udp_timeouts_family_ = &prometheus::BuildCounter()
                                .Name("shield_udp_timeouts_total")
                                .Help("Total UDP session timeouts")
                                .Register(*registry);
    udp_timeouts_counter_ = &udp_timeouts_family_->Add({});
}

void NetworkMetricsCollector::collect() {
    // Network metrics are updated via method calls, no periodic collection
    // needed
}

void NetworkMetricsCollector::increment_connections() {
    active_connections_gauge_->Increment();
}

void NetworkMetricsCollector::decrement_connections() {
    active_connections_gauge_->Decrement();
}

void NetworkMetricsCollector::add_bytes_sent(size_t bytes) {
    bytes_sent_counter_->Increment(static_cast<double>(bytes));
}

void NetworkMetricsCollector::add_bytes_received(size_t bytes) {
    bytes_received_counter_->Increment(static_cast<double>(bytes));
}

void NetworkMetricsCollector::increment_requests() {
    total_requests_counter_->Increment();
}

void NetworkMetricsCollector::record_request_duration(double seconds) {
    request_duration_histogram_->Observe(seconds);
}

// UDP-specific metric methods
void NetworkMetricsCollector::increment_udp_sessions() {
    active_udp_sessions_gauge_->Increment();
}

void NetworkMetricsCollector::decrement_udp_sessions() {
    active_udp_sessions_gauge_->Decrement();
}

void NetworkMetricsCollector::increment_udp_packets_sent() {
    udp_packets_sent_counter_->Increment();
}

void NetworkMetricsCollector::increment_udp_packets_received() {
    udp_packets_received_counter_->Increment();
}

void NetworkMetricsCollector::add_udp_bytes_sent(size_t bytes) {
    udp_bytes_sent_counter_->Increment(static_cast<double>(bytes));
}

void NetworkMetricsCollector::add_udp_bytes_received(size_t bytes) {
    udp_bytes_received_counter_->Increment(static_cast<double>(bytes));
}

void NetworkMetricsCollector::increment_udp_timeouts() {
    udp_timeouts_counter_->Increment();
}

// GameMetricsCollector implementation
GameMetricsCollector::GameMetricsCollector(
    std::shared_ptr<prometheus::Registry> registry) {
    // Active players gauge
    active_players_family_ = &prometheus::BuildGauge()
                                  .Name("shield_active_players")
                                  .Help("Number of active players")
                                  .Register(*registry);
    active_players_gauge_ = &active_players_family_->Add({});

    // Active rooms gauge
    active_rooms_family_ = &prometheus::BuildGauge()
                                .Name("shield_active_rooms")
                                .Help("Number of active rooms")
                                .Register(*registry);
    active_rooms_gauge_ = &active_rooms_family_->Add({});

    // Messages processed counter
    messages_processed_family_ =
        &prometheus::BuildCounter()
             .Name("shield_messages_processed_total")
             .Help("Total number of messages processed")
             .Register(*registry);
    messages_processed_counter_ = &messages_processed_family_->Add({});

    // Actors created counter
    actors_created_family_ = &prometheus::BuildCounter()
                                  .Name("shield_actors_created_total")
                                  .Help("Total number of actors created")
                                  .Register(*registry);
    actors_created_counter_ = &actors_created_family_->Add({});

    // Actors destroyed counter
    actors_destroyed_family_ = &prometheus::BuildCounter()
                                    .Name("shield_actors_destroyed_total")
                                    .Help("Total number of actors destroyed")
                                    .Register(*registry);
    actors_destroyed_counter_ = &actors_destroyed_family_->Add({});
}

void GameMetricsCollector::collect() {
    // Game metrics are updated via method calls, no periodic collection needed
}

void GameMetricsCollector::increment_active_players() {
    active_players_gauge_->Increment();
}

void GameMetricsCollector::decrement_active_players() {
    active_players_gauge_->Decrement();
}

void GameMetricsCollector::increment_active_rooms() {
    active_rooms_gauge_->Increment();
}

void GameMetricsCollector::decrement_active_rooms() {
    active_rooms_gauge_->Decrement();
}

void GameMetricsCollector::increment_messages_processed() {
    messages_processed_counter_->Increment();
}

void GameMetricsCollector::increment_actor_created() {
    actors_created_counter_->Increment();
}

void GameMetricsCollector::increment_actor_destroyed() {
    actors_destroyed_counter_->Increment();
}

#endif  // SHIELD_ENABLE_PROMETHEUS

// PrometheusComponent implementation
PrometheusComponent::PrometheusComponent()
    : Component("prometheus"),
      running_(false),
      collection_interval_(10)  // 10 seconds default
      ,
      listen_address_("0.0.0.0"),
      listen_port_(9090),
      job_name_("shield"),
      enable_pushgateway_(false),
      enable_exposer_(true) {}

PrometheusComponent::~PrometheusComponent() {
    if (running_) {
        stop();
    }
}

PrometheusComponent& PrometheusComponent::instance() {
    static PrometheusComponent instance;
    return instance;
}

void PrometheusComponent::on_init() {
    try {
        auto& config = shield::config::Config::instance();

        // Load configuration
        try {
            listen_address_ =
                config.get<std::string>("prometheus.listen_address");
        } catch (...) {
            // Use default
        }

        try {
            listen_port_ = config.get<uint16_t>("prometheus.listen_port");
        } catch (...) {
            // Use default
        }

        try {
            collection_interval_ = std::chrono::seconds(
                config.get<int>("prometheus.collection_interval"));
        } catch (...) {
            // Use default
        }

        try {
            job_name_ = config.get<std::string>("prometheus.job_name");
        } catch (...) {
            // Use default
        }

        try {
            pushgateway_url_ =
                config.get<std::string>("prometheus.pushgateway_url");
            enable_pushgateway_ = !pushgateway_url_.empty();
        } catch (...) {
            // Use default
        }

        try {
            enable_exposer_ = config.get<bool>("prometheus.enable_exposer");
        } catch (...) {
            // Use default
        }

#ifdef SHIELD_ENABLE_PROMETHEUS
        // Create registry
        registry_ = std::make_shared<prometheus::Registry>();

        // Create built-in collectors
        system_collector_ = std::make_shared<SystemMetricsCollector>(registry_);
        network_collector_ =
            std::make_shared<NetworkMetricsCollector>(registry_);
        game_collector_ = std::make_shared<GameMetricsCollector>(registry_);

        std::cout << "Prometheus component initialized with address: "
                  << listen_address_ << ":" << listen_port_ << std::endl;
#else
        // Create stub collectors
        system_collector_ = std::make_shared<SystemMetricsCollector>();
        network_collector_ = std::make_shared<NetworkMetricsCollector>();
        game_collector_ = std::make_shared<GameMetricsCollector>();

        std::cout << "Prometheus component initialized (metrics disabled - "
                     "prometheus-cpp not available)"
                  << std::endl;
#endif

    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize Prometheus component: " << e.what()
                  << std::endl;
        throw;
    }
}

void PrometheusComponent::on_start() {
    try {
#ifdef SHIELD_ENABLE_PROMETHEUS
        // Start HTTP exposer if enabled
        if (enable_exposer_) {
            exposer_ = std::make_unique<prometheus::Exposer>(
                listen_address_ + ":" + std::to_string(listen_port_));
            exposer_->RegisterCollectable(registry_);
            std::cout << "Prometheus metrics exposed on http://"
                      << listen_address_ << ":" << listen_port_ << "/metrics"
                      << std::endl;
        }

        // Setup push gateway if enabled
        if (enable_pushgateway_ && !pushgateway_url_.empty()) {
            gateway_ = std::make_unique<prometheus::Gateway>(pushgateway_url_,
                                                             job_name_);
            gateway_->RegisterCollectable(registry_);
            std::cout << "Prometheus push gateway configured: "
                      << pushgateway_url_ << std::endl;
        }
#endif

        // Start collection thread
        running_ = true;
        collection_thread_ =
            std::thread(&PrometheusComponent::collection_loop, this);

        std::cout << "Prometheus component started" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Failed to start Prometheus component: " << e.what()
                  << std::endl;
        throw;
    }
}

void PrometheusComponent::on_stop() {
    running_ = false;

    if (collection_thread_.joinable()) {
        collection_thread_.join();
    }

#ifdef SHIELD_ENABLE_PROMETHEUS
    if (gateway_) {
        try {
            // Push final metrics
            gateway_->Push();
        } catch (const std::exception& e) {
            std::cerr << "Failed to push final metrics: " << e.what()
                      << std::endl;
        }
        gateway_.reset();
    }

    exposer_.reset();
#endif

    std::cout << "Prometheus component stopped" << std::endl;
}

void PrometheusComponent::add_collector(
    std::shared_ptr<MetricsCollector> collector) {
    custom_collectors_.push_back(collector);
}

void PrometheusComponent::collection_loop() {
    while (running_) {
        try {
            collect_all_metrics();

#ifdef SHIELD_ENABLE_PROMETHEUS
            // Push to gateway if enabled
            if (gateway_) {
                gateway_->Push();
            }
#endif

        } catch (const std::exception& e) {
            std::cerr << "Error during metrics collection: " << e.what()
                      << std::endl;
        }

        // Sleep for collection interval
        auto start = std::chrono::steady_clock::now();
        while (running_ && std::chrono::steady_clock::now() - start <
                               collection_interval_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void PrometheusComponent::collect_all_metrics() {
    // Collect built-in metrics
    system_collector_->collect();
    network_collector_->collect();
    game_collector_->collect();

    // Collect custom metrics
    for (auto& collector : custom_collectors_) {
        try {
            collector->collect();
        } catch (const std::exception& e) {
            std::cerr << "Error collecting metrics from " << collector->name()
                      << ": " << e.what() << std::endl;
        }
    }
}

}  // namespace shield::metrics