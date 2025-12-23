# Shield Game Server Framework

[![CI/CD Pipeline](https://img.shields.io/github/actions/workflow/status/cuihairu/shield/ci.yml?branch=main&label=CI/CD)](https://github.com/cuihairu/shield/actions/workflows/ci.yml)
[![Docs](https://img.shields.io/github/actions/workflow/status/cuihairu/shield/docs.yml?branch=main&label=Docs)](https://github.com/cuihairu/shield/actions/workflows/docs.yml)

Shield is a modern C++ game server framework that combines Pitaya's distributed architecture with Skynet's high-performance concurrency model, designed specifically for building large-scale multiplayer online games.

## ✨ Core Features

- **🌐 Distributed Architecture**: Microservices architecture with horizontal scaling support
- **🚀 High-Performance Concurrency**: Multi-threaded processing based on Actor model
- **🔧 Lua Script Integration**: C++ engine + Lua business logic with hot reload support
- **🌍 Multi-Protocol Support**: Unified handling of TCP/UDP/HTTP/WebSocket protocols
- **🔄 Service Discovery**: Support for multiple registries: etcd/consul/nacos/redis
- **📊 Monitoring & Metrics**: Built-in Prometheus metrics collection and monitoring
- **⚡ Performance Optimized**: Reactor pattern with multi-threaded I/O processing

## 🚀 Quick Start

```bash
# Clone the project
git clone https://github.com/cuihairu/shield.git
cd shield

# Setup development environment (including git hooks)
./setup-hooks.sh

# Build and run
cmake -B build -S .
cmake --build build
./build/bin/shield --config config/app.yaml
```

## 🌍 Protocol Support

Shield supports multiple network protocols out of the box:

### TCP Protocol
- Persistent connections with session management
- High-throughput binary protocol support
- Connection pooling and load balancing

### UDP Protocol
- Connectionless packet processing with session tracking
- Automatic session timeout and cleanup
- Multi-threaded packet handling
- Real-time metrics collection

### HTTP/WebSocket
- RESTful API endpoints
- WebSocket for real-time communication
- HTTP/2 support for modern web clients

## 📊 Monitoring & Metrics

Built-in Prometheus integration provides comprehensive metrics:

**System Metrics**:
- CPU and memory usage
- Network I/O statistics
- Thread pool utilization

**Protocol-Specific Metrics**:
- Active connections/sessions (TCP/UDP)
- Packet/request throughput
- Response times and error rates
- Protocol-specific counters

**Game Metrics**:
- Active players and rooms
- Message processing rates
- Actor lifecycle events

## 📚 Documentation

- **[Development Guide](docs/development-guide.md)** - Environment setup, dependencies, development configuration
- **[Architecture Design](docs/architecture.md)** - System architecture, framework comparison, design philosophy
- **[API Documentation](docs/api/)** - Detailed interface documentation for all modules
- **[Configuration Guide](docs/configuration.md)** - Configuration file reference and best practices
- **[UDP Protocol Support](docs/udp-protocol-support.md)** - UDP implementation details and usage
- **[Prometheus Integration](docs/prometheus-integration.md)** - Monitoring setup and metrics reference
- **[Roadmap](docs/roadmap.md)** - Feature planning and version roadmap

### 📖 Building Documentation Locally

This project uses [mdBook](https://rust-lang.github.io/mdBook/) to build documentation. You can build and view the documentation locally by following these steps:

#### Install mdBook

**Method 1: Install using Cargo (Recommended)**
```bash
# Make sure Rust and Cargo are installed
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env

# Install mdBook
cargo install mdbook
```

**Method 2: Install from pre-compiled binaries**
```bash
# macOS (using Homebrew)
brew install mdbook

# Or download pre-compiled binaries
# Visit https://github.com/rust-lang/mdBook/releases
# Download the version suitable for your system and extract to PATH
```

#### Build and View Documentation

```bash
# Build documentation in project root directory
mdbook build

# Start local server and automatically open browser to view documentation
mdbook serve --open

# Or manually open browser and visit http://localhost:3000
```

After building, static documentation will be generated in the `book/` directory. You can also directly open the `book/index.html` file to view it.

## 🏗️ Architecture

Shield is built on a modular, component-based architecture:

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   Gateway       │    │  Game Logic     │    │  Data Layer     │
│                 │    │                 │    │                 │
│ ┌─────────────┐ │    │ ┌─────────────┐ │    │ ┌─────────────┐ │
│ │TCP/UDP/HTTP │ │    │ │ Lua Scripts │ │    │ │  Database   │ │
│ │ Handlers    │ │    │ │             │ │    │ │  Cache      │ │
│ └─────────────┘ │    │ └─────────────┘ │    │ └─────────────┘ │
│ ┌─────────────┐ │    │ ┌─────────────┐ │    │ ┌─────────────┐ │
│ │ Load        │ │    │ │ Actor       │ │    │ │ Service     │ │
│ │ Balancer    │ │    │ │ System      │ │    │ │ Discovery   │ │
│ └─────────────┘ │    │ └─────────────┘ │    │ └─────────────┘ │
└─────────────────┘    └─────────────────┘    └─────────────────┘
```

## 🧪 Testing

```bash
# Run all tests
cmake --build build --target test

# Run specific test suites
./build/bin/test_lua_actor
./build/bin/test_udp_server
./build/bin/test_integration
```

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](docs/contributing.md) for details on:

- Code style and standards
- Pull request process
- Issue reporting
- Development workflow

## 📄 License

This project is licensed under the [Apache License 2.0](LICENSE).

---

**Note**: Shield is under active development. APIs may change between versions.
