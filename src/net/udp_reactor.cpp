#include "shield/net/udp_reactor.hpp"

#include "shield/core/logger.hpp"

namespace shield::net {

UdpReactor::UdpReactor(uint16_t port, size_t num_worker_threads)
    : m_port(port), m_num_worker_threads(num_worker_threads) {
    SHIELD_LOG_INFO << "UdpReactor created for port " << port << " with "
                    << num_worker_threads << " worker threads";
}

UdpReactor::~UdpReactor() { stop(); }

void UdpReactor::start() {
    if (m_running.load()) {
        SHIELD_LOG_WARN << "UdpReactor already running";
        return;
    }

    try {
        // Create the UDP protocol handler
        if (m_handler_creator) {
            m_handler = m_handler_creator(m_io_context, m_port);
        } else {
            m_handler = std::make_unique<shield::protocol::UdpProtocolHandler>(
                m_io_context, m_port);
        }

        // Start the handler
        m_handler->start();

        // Start worker threads
        m_running.store(true);
        for (size_t i = 0; i < m_num_worker_threads; ++i) {
            m_worker_threads.emplace_back([this, i]() {
                SHIELD_LOG_DEBUG << "UDP reactor worker thread " << i
                                 << " starting";

                while (m_running.load()) {
                    try {
                        m_io_context.run();

                        // If run() exits and we're still supposed to be
                        // running, restart it
                        if (m_running.load() && m_io_context.stopped()) {
                            m_io_context.restart();
                        }
                    } catch (const std::exception& e) {
                        SHIELD_LOG_ERROR << "UDP reactor worker " << i
                                         << " exception: " << e.what();
                    }
                }

                SHIELD_LOG_DEBUG << "UDP reactor worker thread " << i
                                 << " stopping";
            });
        }

        SHIELD_LOG_INFO << "UdpReactor started on port " << m_port << " with "
                        << m_num_worker_threads << " worker threads";

    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to start UdpReactor: " << e.what();
        m_running.store(false);
        throw;
    }
}

void UdpReactor::stop() {
    if (!m_running.load()) {
        return;
    }

    SHIELD_LOG_INFO << "Stopping UdpReactor...";

    // Signal shutdown
    m_running.store(false);

    // Stop the handler
    if (m_handler) {
        m_handler->stop();
    }

    // Stop the io_context
    m_io_context.stop();

    // Wait for all worker threads to finish
    for (auto& thread : m_worker_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    m_worker_threads.clear();

    // Reset the handler
    m_handler.reset();

    SHIELD_LOG_INFO << "UdpReactor stopped";
}

}  // namespace shield::net