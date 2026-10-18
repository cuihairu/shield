[0m
> build · gpt-6-luna
[0m→ [0mSkill "architecture-review" 
> build · gpt-6-luna
[0m$ [0mfind /home/cui/.config/opencode/skills/architecture-review -maxdepth 3 -type f | sort
(no output)
[0m$ [0mgit status --short && git log -1 --oneline
37675b9 fix(global): close rank backend coverage gaps
[0m$ [0mfind . -maxdepth 3 -type f | sort | sed -n '1,240p'
./actor_addr.hpp##62eaf6325b8b8ed69d3a3c125699cf56.gcov
./actor_addr.hpp.gcov
./actor_cast.hpp##0cbf4060b1a5c6f408690225f78d6f47.gcov
./actor_cast.hpp.gcov
./actor_control_block.hpp##682f5709b2fa4a8ee4d6d2f23d4505fa.gcov
./actor_control_block.hpp.gcov
./actor.hpp##c23b74a8aafad07264c78c0e81f130a7.gcov
./actor.hpp.gcov
./actor_storage.hpp##c6093646f07450c078d586c48c85553d.gcov
./actor_storage.hpp.gcov
./actor_system_config.hpp##523b28c068d3efa14a377c5a00a695de.gcov
./actor_system.hpp##793daffc8b64a4ab881e9681f71f4230.gcov
./actor_system.hpp.gcov
./addressof.hpp##b9f15ae3856f4a7c9e48ef5353c90134.gcov
./adl_serializer.hpp##378968870d0d91e496dfac1b18c6724d.gcov
./adl_serializer.hpp.gcov
./after.hpp##93658f932d50a144db7d05acb693cd86.gcov
./after.hpp.gcov
./aligned_buffer.h##6b0bff75715f084a654bf7128235cbee.gcov
./aligned_buffer.h.gcov
./allocated_ptr.h##36d781ba27a390bcaa5f58cf9d4c9379.gcov
./allocated_ptr.h.gcov
./allocator.h##8c52fd9b2fe70b653901964f2063d23e.gcov
./allocator.h.gcov
./alloc_traits.h##5a54c417c0da7ff2c92ecd51dd3afaa0.gcov
./alloc_traits.h##fb47bee2ed37b7e23ffba03b506d3005.gcov
./alloc_traits.h.gcov
./apply_args.hpp##f52fee72f9d043989e6e74d07d94ceca.gcov
./apply_args.hpp.gcov
./array##f195f1d2f9adabd951e4fc7ef269f706.gcov
./array.gcov
./as_mutable_ref.hpp.gcov
./assertion_result.hpp##01171f948179293bb7e9a7f6d24349d5.gcov
./atomic##88133c3ee7634511201a553d678d3807.gcov
./atomic_base.h##94e0d98d1e82d2fde7e9c1da7c8fa6fc.gcov
./atomic_base.h.gcov
./atomic_futex.h##654be61fa1de46fc84f6827c36728763.gcov
./atomic_futex.h.gcov
./atomic.gcov
./atomicity.h##08a11efda281551c4b24c7c9aa6e78e4.gcov
./atomicity.h.gcov
./atomic_ref_counted.hpp##9020fba4c8ad667a23b66eff7a7163ea.gcov
./atomic_ref_counted.hpp.gcov
./atomic_wait.h##ce1acd63b04dc2b713c4e574e3943ac5.gcov
./basic_cstring.hpp##8549bebd2d751d4426f5448f7734d7c0.gcov
./basic_string.h##c4f29f0ffe008a3f6a7745e256e9453f.gcov
./basic_string.h.gcov
./basic_string.tcc##da5c2484d6f02cb21960696bf940a4da.gcov
./basic_string.tcc.gcov
./bcs_char_traits.hpp##68ad74656da207148bb83d089b16de47.gcov
./behavior.hpp##4c0b8503012e3e4db07c8fd4e33c5ed2.gcov
./behavior.hpp.gcov
./behavior_impl.hpp##7c05e43fa28968717a0d1441bc2dff3f.gcov
./behavior_impl.hpp.gcov
./binary_deserializer.hpp.gcov
./binary_serializer.hpp.gcov
./binary_writer.hpp##4b0701a4e4dc3d9d953757e9b873ced3.gcov
./binary_writer.hpp.gcov
./binding.hpp##d74ffcb7ebec13f589ce0cb02da0821c.gcov
./bit##85256e9cb4ae2528209883c60b6c86de.gcov
./bit.gcov
./bitset.gcov
./blocking_actor.hpp##8d05d4f7ffe9d66188c844b7c1a7bd03.gcov
./blocking_actor.hpp.gcov
./blocking_behavior.hpp##ab606f304ba9c6a7ca011d6f4e36118b.gcov
./blocking_behavior.hpp.gcov
./bstree_algorithms.hpp##2849d6f474030fa369c52d58594412a2.gcov
./bstree.hpp##d4e99cb94c544e3aef852a65e86f4ffd.gcov
./build.bat
./build-cov-local/bin/shield
./build-cov-local/bin/shield-console
./build-cov-local/bin/test_client_rpc_e2e
./build-cov-local/bin/test_cluster_manager
./build-cov-local/bin/test_cluster_transport
./build-cov-local/bin/test_console_server
./build-cov-local/bin/test_cov_base_id
./build-cov-local/bin/test_cov_base_types
./build-cov-local/bin/test_cov_binding
./build-cov-local/bin/test_cov_bootstrap
./build-cov-local/bin/test_cov_bootstrap2
./build-cov-local/bin/test_cov_cluster
./build-cov-local/bin/test_cov_config
./build-cov-local/bin/test_cov_console_net
./build-cov-local/bin/test_cov_cpp_lua_api
./build-cov-local/bin/test_cov_gateway_actor
./build-cov-local/bin/test_cov_http_bridge2
./build-cov-local/bin/test_cov_http_client
./build-cov-local/bin/test_cov_http_server
./build-cov-local/bin/test_cov_ip_blocklist
./build-cov-local/bin/test_cov_listener
./build-cov-local/bin/test_cov_logger
./build-cov-local/bin/test_cov_lua_api2
./build-cov-local/bin/test_cov_lua_commands
./build-cov-local/bin/test_cov_lua_crypto
./build-cov-local/bin/test_cov_lua_db_async
./build-cov-local/bin/test_cov_lua_gateway_bridge
./build-cov-local/bin/test_cov_lua_http_bridge
./build-cov-local/bin/test_cov_lua_panic
./build-cov-local/bin/test_cov_lua_runtime
./build-cov-local/bin/test_cov_lua_runtime2
./build-cov-local/bin/test_cov_lua_runtime3
./build-cov-local/bin/test_cov_lua_service
./build-cov-local/bin/test_cov_lua_service2
./build-cov-local/bin/test_cov_lua_service3
./build-cov-local/bin/test_cov_lua_service4
./build-cov-local/bin/test_cov_lua_shutdown
./build-cov-local/bin/test_cov_manifest
./build-cov-local/bin/test_cov_ops_http
./build-cov-local/bin/test_cov_plugin_config
./build-cov-local/bin/test_cov_plugin_host
./build-cov-local/bin/test_cov_protocol
./build-cov-local/bin/test_cov_root_commands
./build-cov-local/bin/test_cov_rpc_descriptor
./build-cov-local/bin/test_cov_schema_validator
./build-cov-local/bin/test_cov_session
./build-cov-local/bin/test_cov_shield
./build-cov-local/bin/test_cov_slow_calls
./build-cov-local/bin/test_cov_starter
./build-cov-local/bin/test_cov_tls
./build-cov-local/bin/test_default_config_boot
./build-cov-local/bin/test_global_data_backend
./build-cov-local/bin/test_global_lock_backend
./build-cov-local/bin/test_global_manager
./build-cov-local/bin/test_global_rank_backend
./build-cov-local/bin/test_hello_world_acceptance
./build-cov-local/bin/test_lua_api_caf_bridge
./build-cov-local/bin/test_lua_api_call
./build-cov-local/bin/test_lua_api_client_rpc
./build-cov-local/bin/test_lua_api_clock
./build-cov-local/bin/test_lua_api_concurrency
./build-cov-local/bin/test_lua_api_config
./build-cov-local/bin/test_lua_api_context
./build-cov-local/bin/test_lua_api_gateway
./build-cov-local/bin/test_lua_api_global
./build-cov-local/bin/test_lua_api_legacy
./build-cov-local/bin/test_lua_api_lifecycle
./build-cov-local/bin/test_lua_api_messaging
./build-cov-local/bin/test_lua_api_player
./build-cov-local/bin/test_lua_api_registry
./build-cov-local/bin/test_lua_api_server
./build-cov-local/bin/test_lua_api_service_messages
./build-cov-local/bin/test_lua_api_spawn
./build-cov-local/bin/test_lua_api_timers
./build-cov-local/bin/test_node_discovery
./build-cov-local/bin/test_ops_profile
./build-cov-local/bin/test_player_manager
./build-cov-local/bin/test_plugin_host
./build-cov-local/bin/test_plugin_manifest
./build-cov-local/bin/test_plugin_schema
./build-cov-local/bin/test_pool_stats
./build-cov-local/bin/test_protocol_routing
./build-cov-local/bin/test_server_manager
./build-cov-local/bin/test_tcp_listener
./build-cov-local/build.ninja
./build-cov-local/CMakeCache.txt
./build-cov-local/CMakeFiles/cmake.check_cache
./build-cov-local/CMakeFiles/CMakeConfigureLog.yaml
./build-cov-local/CMakeFiles/InstallScripts.json
./build-cov-local/CMakeFiles/rules.ninja
./build-cov-local/CMakeFiles/TargetDirectories.txt
./build-cov-local/cmake_install.cmake
./build-cov-local/CTestTestfile.cmake
./build-cov-local/lib/libshield_base.a
./build-cov-local/lib/libshield_bootstrap.a
./build-cov-local/lib/libshield_cluster.a
./build-cov-local/lib/libshield_config.a
./build-cov-local/lib/libshield_core.a
./build-cov-local/lib/libshield_global.a
./build-cov-local/lib/libshield_log.a
./build-cov-local/lib/libshield_lua.a
./build-cov-local/lib/libshield_net.a
./build-cov-local/lib/libshield_player.a
./build-cov-local/lib/libshield_plugin.a
./build-cov-local/lib/libshield_server.a
./build-cov-local/lib/libshield_transport.a
./build-cov-local/.ninja_deps
./build-cov-local/.ninja_log
./build-cov-local/scaffold-smoke/README.md
./build-cov-local/tests/cmake_install.cmake
./build-cov-local/tests/CTestTestfile.cmake
./build-cov-local/vcpkg-manifest-install.log
./build.sh
./byte_container_with_subtype.hpp##7468e937535e82e09ad0d817aa51e1ac.gcov
./byte_container_with_subtype.hpp.gcov
./c++config.h##e58b4817326051d6e564d11bcc709124.gcov
./c++config.h.gcov
./call.hpp.gcov
./charconv.h##16b9caa1e7e1aa3c5d7d9b58d0c9cff5.gcov
./charconv.h.gcov
./char_traits.h##c253020bba15ff12ef431897f484dfc4.gcov
./char_traits.h.gcov
./chrono.h##b9bb1f04661032b5cee095785f9e3622.gcov
./chrono.h.gcov
./circular_list_algorithms.hpp##e7504788b4aa52ebf255db932b82d234.gcov
./.clang-format
./.clang-tidy
./class_properties.hpp##16246a83d11fd06e9fcd5b4595015f24.gcov
./clock.hpp##fefa26ed4b3357ca6c09b46f2efa35ef.gcov
./clock.hpp.gcov
./cluster_messages.hpp.gcov
./cluster_transport.cpp.gcov
./cmake/AssertCommand.cmake
./cmake/CheckDeps.cmake
./cmake/CheckSourceBoundary.cmake
./cmake/crashpad.cmake
./cmake/GitHooks.cmake
./CMakeLists.txt
./cmath##cd192ded68560b6527c3b9756a9d6fa2.gcov
./cmath.gcov
./comparable.hpp##a3098c1cd229120f8a33d07fe22dd34c.gcov
./comparable.hpp.gcov
./compare##870d1482fe02f3876569cb9ca16a7723.gcov
./compare.gcov
./condition_variable##e29b2726a50a539088af43208ab41f27.gcov
./condition_variable.gcov
./config/app-dev.yaml
./config/app-prod.yaml
./config/app-with-redis.yaml
./config/app-with-sqlite.yaml
./config/app.yaml
./const_typed_message_view.hpp##f8f8191a40b2438851569d6583588758.gcov
./const_typed_message_view.hpp.gcov
./counter.hpp.gcov
./cpp_type_traits.h##10d999b0dc8c1cc45b203ce67613db5f.gcov
./cpp_type_traits.h.gcov
./default_header_holder.hpp##f58ab7eb2ecaf584249611134c5ee223.gcov
./demangle.hpp.gcov
./deque.tcc##c4564314dc9b2427101d3d0d46508bb4.gcov
./deque.tcc.gcov
./disposable.hpp##d5c6b76d3a0dc7cd3d1600083110266b.gcov
./disposable.hpp.gcov
./dispose_on_call.hpp.gcov
./Dockerfile
./.dockerignore
./docs/api.md
./docs/architecture-core-concepts.md
./docs/architecture-decisions.md
./docs/architecture.md
./docs/architecture-review.md
./docs/best-practices.md
[0m$ [0mfind .. -name AGENTS.md -o -name CLAUDE.md
../croupier-wt-support/CLAUDE.md
../courier/node_modules/cytoscape/AGENTS.md
../apollo/docs/node_modules/.pnpm/cytoscape@3.34.1/node_modules/cytoscape/AGENTS.md
../falcon/packages/falcon-swarmd/CLAUDE.md
../falcon/packages/libfalcon-core/CLAUDE.md
../falcon/packages/libfalcon-protocols/plugins/ftp/CLAUDE.md
../falcon/packages/libfalcon-protocols/plugins/http/CLAUDE.md
../falcon/packages/libfalcon-protocols/plugins/bittorrent/CLAUDE.md
../falcon/packages/falcon-daemon/CLAUDE.md
../falcon/packages/falcon-cli/CLAUDE.md
../falcon/CLAUDE.md
../falcon/apps/desktop/CLAUDE.md
../awn/node_modules/.pnpm/cytoscape@3.34.3/node_modules/cytoscape/AGENTS.md
../redis-streaming/AGENTS.md
../redis-streaming/CLAUDE.md
../hello-game/orleans/test/AGENTS.md
../hello-game/orleans/docs/AGENTS.md
../hello-game/orleans/samples/AGENTS.md
../hello-game/orleans/AGENTS.md
../cage/node_modules/cytoscape/AGENTS.md
../BigWorld/node_modules/cytoscape/AGENTS.md
../atlas/docs/node_modules/cytoscape/AGENTS.md
../luna/deps/tomlc17/CLAUDE.md
../unidict/AGENTS.md
../spawn/node_modules/expo-modules-jsi/CLAUDE.md
../spawn/apps/mobile-app/AGENTS.md
../alpha/target/accept-cargo/registry/src/index.crates.io-1949cf8c6b5b557f/zerocopy-0.8.59/AGENTS.md
../mistlane/node_modules/.pnpm/cytoscape@3.34.4/node_modules/cytoscape/AGENTS.md
../chirp/docs/node_modules/.pnpm/cytoscape@3.34.3/node_modules/cytoscape/AGENTS.md
find: ‘../chirp/apps/ios/.build/out/ModuleCache.noindex/296VLK6T9AYBW’: Permission denied
find: ‘../chirp/apps/ios/.build/out/ModuleCache.noindex/DQF4JBQ1JU0C’: Permission denied
find: ‘../chirp/apps/ios/.build/out/ModuleCache.noindex/RLD7J61IBCX’: Permission denied
find: ‘../chirp/apps/ios/.build/out/ModuleCache.noindex/6C26W81FE103’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/WB’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/N0’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/0B’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/MI’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/FX’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/MB’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/ZW’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/5T’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/Q9’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/MR’: Permission denied
find: ‘../chirp/apps/ios/.build/out/v5/records/GZ’: Permission denied
../sandtable/node_modules/.pnpm/cytoscape@3.34.3/node_modules/cytoscape/AGENTS.md
../oddsmaker/node_modules/.pnpm/cytoscape@3.34.0/node_modules/cytoscape/AGENTS.md
../wingman/CLAUDE.md
../game-fogs/mistlane/node_modules/.pnpm/cytoscape@3.34.4/node_modules/cytoscape/AGENTS.md
../chorus/node_modules/.pnpm/cytoscape@3.34.4/node_modules/cytoscape/AGENTS.md
../croupier-wt-pages/CLAUDE.md
../servify/docs/node_modules/cytoscape/AGENTS.md
../servify/apps/admin/node_modules/.pnpm/axios@0.34.0/node_modules/axios/AGENTS.md
../servify/apps/admin/node_modules/.pnpm/axios@0.34.0/node_modules/axios/CLAUDE.md
../croupier/web/node_modules/.pnpm/cytoscape@3.34.0/node_modules/cytoscape/AGENTS.md
../croupier/docs/node_modules/.pnpm/cytoscape@3.34.0/node_modules/cytoscape/AGENTS.md
../croupier/CLAUDE.md
../persona/docs/node_modules/cytoscape/AGENTS.md
../croupier-wt-ui/CLAUDE.md
../croupier-wt-api/CLAUDE.md
> build · gpt-6-luna
[0m$ [0mgit ls-files | awk 'BEGIN{FS="/"} NF==1{print;next}{print $1 "/" $2}' | sort | uniq -c | sort -nr | head -80
73 include/shield
     48 tests/lua_api
     45 tests/coverage
     25 tests/fixtures
     16 tests/plugin
     16 docs/plugins
     11 src/lua
     10 src/console
      9 ports-overlay/mongo-c-driver
      8 src/net
      6 src/plugin
      5 tests/net
      5 examples/hello_world
      4 tests/global
      4 src/global
      4 plugins/mongodb
      4 plugins/_shared
      4 examples/kickstart
      3 tests/cluster
      3 templates/minimal_game
      3 src/cluster
      3 plugins/sqlite
      3 plugins/redis.driver
      3 plugins/queue.redis
      3 plugins/protocol.protobuf
      3 plugins/protocol.msgpack
      3 plugins/protocol.json
      3 plugins/protocol.flatbuffers
      3 plugins/postgresql
      3 plugins/mysql
      3 plugins/metric_prometheus
      3 plugins/matchmaking_elo
      3 plugins/leaderboard.redis
      3 plugins/health_http
      3 plugins/cache.redis
      3 plugins/auth_jwt
      3 docs/superpowers
      3 docs/.vitepress
      3 .github/workflows
      2 tools/shield-console
      2 tests/acceptance
      2 src/transport
      2 src/bootstrap
      2 docs/public
      1 vcpkg.json
      1 vcpkg-configuration.json
      1 tools/symbolize-crash.sh
      1 tools/new_plugin.sh
      1 todo.md
      1 third_party/mini_chromium
      1 third_party/lss
      1 third_party/crashpad
      1 tests/transport
      1 tests/server
      1 tests/protocol
      1 tests/player
      1 tests/hello_world_acceptance.cpp
      1 tests/CMakeLists.txt
      1 src/shield_base
      1 src/shield.cpp
      1 src/server
      1 src/player
      1 src/main.cpp
      1 src/log
      1 src/crash
      1 src/core
      1 src/config
      1 src/caf_initializer.cpp
      1 scripts/new_project.sh
      1 scripts/lib
      1 scripts/echo.lua
      1 scripts/client_demo.py
      1 scripts/bootstrap.lua
      1 pnpm-workspace.yaml
      1 pnpm-lock.yaml
      1 plugins/CMakeLists.txt
      1 package.json
      1 examples/CMakeLists.txt
      1 docs/xmldef-unity-generator-spec.md
      1 docs/xmldef-toolchain-design.md
[0m$ [0mgit ls-files | grep -Ei '(^|/)(todo|task|roadmap|plan)|\.md$' | head -300
.vscode/tasks.json
README.md
README.zh.md
docs/api.md
docs/architecture-core-concepts.md
docs/architecture-decisions.md
docs/architecture-review.md
docs/architecture.md
docs/best-practices.md
docs/cluster-implementation-plan.md
docs/cmake-refactor.md
docs/config-system-analysis.md
docs/configuration.md
docs/crash-reporting.md
docs/db-async-design.md
docs/db-discipline.md
docs/deployment.md
docs/development-guide.md
docs/diagnostics-console.md
docs/directory-structure.md
docs/documentation-map.md
docs/engine-sdk-design.md
docs/faq.md
docs/gateway.md
docs/index.md
docs/lua-api-tests.md
docs/lua-api.md
docs/open-decisions.md
docs/ops-lua-console.md
docs/optional-module-tests.md
docs/optional-modules.md
docs/performance.md
docs/plugin-pool-stats.md
docs/plugin-system.md
docs/plugins/cache-redis.md
docs/plugins/database-mongodb.md
docs/plugins/database-mysql.md
docs/plugins/database-postgresql.md
docs/plugins/database-sqlite.md
docs/plugins/health-http.md
docs/plugins/index.md
docs/plugins/leaderboard-redis.md
docs/plugins/matchmaking-elo.md
docs/plugins/metrics-prometheus.md
docs/plugins/protocol-flatbuffers.md
docs/plugins/protocol-json.md
docs/plugins/protocol-msgpack.md
docs/plugins/protocol-protobuf.md
docs/plugins/queue-redis.md
docs/plugins/redis-driver.md
docs/product-gap.md
docs/protocol-codec-plugins.md
docs/protocol-routing-design.md
docs/quickstart.md
docs/roadmap.md
docs/runtime-bootstrap.md
docs/runtime-cluster.md
docs/runtime-config.md
docs/runtime-data.md
docs/runtime-entity.md
docs/runtime-errors.md
docs/runtime-global.md
docs/runtime-log.md
docs/runtime-lua-vm.md
docs/runtime-messaging.md
docs/runtime-network.md
docs/runtime-ops.md
docs/runtime-persistence.md
docs/runtime-player.md
docs/runtime-primitives.md
docs/runtime-security.md
docs/runtime-semantics.md
docs/runtime-server.md
docs/runtime-service.md
docs/runtime-timer.md
docs/schema-design.md
docs/skynet-comparison.md
docs/starter-system.md
docs/superpowers/plans/2026-06-27-binding-align.md
docs/superpowers/plans/2026-06-27-pool-stats-v1.md
docs/superpowers/plans/2026-09-22-ops-profile-v1.md
docs/tls-design.md
docs/tutorial-game-backend.md
docs/udp-protocol-support.md
docs/xmldef-compiler-runtime-mvp.md
docs/xmldef-descriptor-spec.md
docs/xmldef-phase1-implementation.md
docs/xmldef-toolchain-design.md
docs/xmldef-unity-generator-spec.md
examples/kickstart/README.md
templates/minimal_game/README.md
todo.md
[0m$ [0mfind /home/cui/.config/opencode/skills/architecture-review -type f -maxdepth 4 -print
(no output)
[0m$ [0mgit ls-files | grep -vE '(^build|\.gcov$|^third_party/|^vendor/)' | wc -l
494
> build · gpt-6-luna
[0m→ [0mRead todo.md 
[0m→ [0mRead docs/architecture.md 
[0m→ [0mRead docs/architecture-review.md 
[0m→ [0mRead README.md 
[0m→ [0mRead docs/roadmap.md 
[0m→ [0mRead docs/architecture-decisions.md 
[0m→ [0mRead docs/product-gap.md 
> build · gpt-6-luna
[0m→ [0mSkill "code-review" 
> build · gpt-6-luna
我会把 `todo.md` 当作完成项清单而非质量证明；重点核对实现路径、运行边界和文档口径。初步已看到架构评估文档里有些历史结论后来虽标注“已解决”，但需要检查总览是否同步。
[0m→ [0mRead docs/runtime-security.md 
[0m→ [0mRead docs/runtime-cluster.md 
[0m→ [0mRead docs/runtime-global.md 
[0m→ [0mRead docs/runtime-lua-vm.md 
[0m→ [0mRead docs/plugin-system.md 
[0m→ [0mRead docs/deployment.md 
[0m$ [0mgit ls-files '*.cpp' '*.hpp' '*.h' | grep -v '^third_party/' | wc -l && git ls-files 'tests/**' | wc -l && grep -nE '^[-*] \[ \]' todo.md | tail -30 && grep -nE '^[-*] \[[ xX]\]' todo.md | awk 'BEGIN{n=0;u=0} /\[ \]/{u++} /\[[xX]\]/{n++} END{print "checked="n,"unchecked="u}'
245
154
checked=122 unchecked=0
> build · gpt-6-luna
[0m✱ [0mGrep "hello|Hello|handshake|tls|SSL|auth|token|node_id|open|accept" [90min src/cluster · 100 matches[0m
[0m✱ [0mGrep "redis::|client\.|hiredis|redis++|command\(|execute\(|run_lua|Lua" [90min src/global · 100 matches[0m
[0m✱ [0mGrep "redis|timeout|async|worker|query_async|query\(" [90min plugins/redis.driver · 100 matches[0m
[0m✱ [0mGrep "allow_os|allow_io|luaL_openlibs|open_libraries|allow.*io" [90min src/lua · 13 matches[0m
[0m✱ [0mGrep "eval|listen|bind|127.0.0.1|auth|token" [90min src/console · 55 matches[0m
[0m$ [0mfind include/shield src plugins tests docs -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) | wc -l
242
> build · gpt-6-luna
[0m→ [0mRead src/cluster/cluster_transport.cpp [90m[offset=220, limit=190][0m
[0m→ [0mRead plugins/redis.driver/shield_redis_driver.cpp [90m[offset=190, limit=190][0m
[0m→ [0mRead src/global/global_rank_backend.cpp [90m[offset=230, limit=215][0m
[0m→ [0mRead README.md [90m[offset=212, limit=20][0m
[0m→ [0mRead docs/db-async-design.md 
[0m→ [0mRead docs/runtime-cluster.md [90m[offset=7, limit=13][0m
[0m→ [0mRead docs/runtime-security.md [90m[offset=301, limit=16][0m
> build · gpt-6-luna
[0m✱ [0mGrep "rank_top|rank_position|rank_range|rank_update|g:get|GlobalManager::|global.*get" [90min src/lua · 14 matches[0m
[0m✱ [0mGrep "max_memory|max_instructions|instruction_count|lua_sethook|memory_limit|quota" [90min src/lua · 6 matches[0m
[0m✱ [0mGrep "max_memory|max_instructions|sandbox" [90min include/shield · 0 matches[0m
[0m$ [0mnl -ba src/cluster/cluster_transport.cpp | sed -n '274,305p'; nl -ba plugins/redis.driver/shield_redis_driver.cpp | sed -n '300,380p'; nl -ba src/global/global_rank_backend.cpp | sed -n '273,324p;389,422p'; nl -ba src/lua/lua_runtime.cpp | sed -n '190,220p'; nl -ba README.md | sed -n '212,224p'
274	        // -- inbound handshake ---------------------------------------------
   275	        [self](const HelloMsg& hello) {  // GCOVR_EXCL_LINE (gcov attributes no
   276	                                         // code to this line)
   277	            auto& log = shield::log::get_logger("cluster");
   278	            SHIELD_LOG_DEBUG(log, "Hello from " + hello.node_id + " (epoch " +
   279	                                      std::to_string(hello.epoch) + ")");
   280	            if (hello.proto_version != kClusterProtoVersion) {
   281	                SHIELD_LOG_ERROR(log, "Peer protocol version mismatch: got " +
   282	                                          std::to_string(hello.proto_version) +
   283	                                          ", expected " +
   284	                                          std::to_string(kClusterProtoVersion));
   285	                return;
   286	            }
   287	            // Reply over the inbound connection; the dialer adopts our
   288	            // identity from this ack. We adopt nothing here — our own
   289	            // adoption happens via our own dial (both sides dial). The ack
   290	            // must carry our address as sender too: the dialer matches it
   291	            // against the proxy it dialed.
   292	            if (auto sender =
   293	                    self->current_sender()) {  // GCOVR_EXCL_BR_LINE (defensive:
   294	                                               // current_sender only null for
   295	                                               // non-request messages; HelloMsg
   296	                                               // is a request)
   297	                self->send(caf::actor_cast<caf::actor>(sender),
   298	                           HelloAckMsg{// GCOVR_EXCL_BR_LINE (compiler artifact:
   299	                                       // aggregate init-list branches)
   300	                                       self->state().self_node_id,
   301	                                       self->state().self_epoch,
   302	                                       kClusterProtoVersion});
   303	            }
   304	        },
   305	        // -- handshake completion (dialer side) ------------------------------
   300	        // get
   301	        [](void* inst, const char* key, shield_redis_value_v1** out,
   302	           shield_error_v1* err) -> int {
   303	            if (!inst || !key || !out) return -1;
   304	            auto* ri = static_cast<redis_instance*>(inst);
   305	            if (!ri->redis) return -1;
   306	            try {
   307	                auto val = ri->redis->get(std::string(key));
   308	                if (val) {
   309	                    *out = make_string_value(*val);
   310	                } else {
   311	                    *out = make_nil_value();
   312	                }
   313	                return 0;
   314	            } catch (const std::exception& e) {
   315	                if (err) {
   316	                    err->code = "redis.command.failed";
   317	                    err->message = e.what();
   318	                }
   319	                *out = make_nil_value();
   320	                return -1;
   321	            }
   322	        },
   323	
   324	        // set
   325	        [](void* inst, const char* key, const char* val, int ttl_sec,
   326	           shield_error_v1* err) -> int {
   327	            if (!inst || !key || !val) return -1;
   328	            auto* ri = static_cast<redis_instance*>(inst);
   329	            if (!ri->redis) return -1;
   330	            try {
   331	                if (ttl_sec > 0) {
   332	                    ri->redis->set(std::string(key), std::string(val),
   333	                                   std::chrono::seconds(ttl_sec));
   334	                } else {
   335	                    ri->redis->set(std::string(key), std::string(val));
   336	                }
   337	                return 0;
   338	            } catch (const std::exception& e) {
   339	                if (err) {
   340	                    err->code = "redis.command.failed";
   341	                    err->message = e.what();
   342	                }
   343	                return -1;
   344	            }
   345	        },
   346	
   347	        // del
   348	        [](void* inst, const char* key, shield_error_v1* err) -> int {
   349	            if (!inst || !key) return -1;
   350	            auto* ri = static_cast<redis_instance*>(inst);
   351	            if (!ri->redis) return -1;
   352	            try {
   353	                ri->redis->del(std::string(key));
   354	                return 0;
   355	            } catch (const std::exception& e) {
   356	                if (err) {
   357	                    err->code = "redis.command.failed";
   358	                    err->message = e.what();
   359	                }
   360	                return -1;
   361	            }
   362	        },
   363	
   364	        // hget
   365	        [](void* inst, const char* key, const char* field,
   366	           shield_redis_value_v1** out, shield_error_v1* err) -> int {
   367	            if (!inst || !key || !field || !out) return -1;
   368	            auto* ri = static_cast<redis_instance*>(inst);
   369	            if (!ri->redis) return -1;
   370	            try {
   371	                auto val =
   372	                    ri->redis->hget(std::string(key), std::string(field));
   373	                if (val) {
   374	                    *out = make_string_value(*val);
   375	                } else {
   376	                    *out = make_nil_value();
   377	                }
   378	                return 0;
   379	            } catch (const std::exception& e) {
   380	                if (err) {
   273	    std::optional<std::uint64_t> rank_position(
   274	        const std::string& board, const std::string& uid) override {
   275	        auto redis = connect();
   276	        const auto sorted = sorted_board(redis, board);
   277	        for (const auto& entry : sorted) {
   278	            if (entry.uid == uid) {
   279	                return entry.rank;
   280	            }
   281	        }
   282	        return std::nullopt;
   283	    }
   284	
   285	    std::vector<RankEntry> rank_top(const std::string& board,
   286	                                    std::size_t n) override {
   287	        auto redis = connect();
   288	        auto sorted = sorted_board(redis, board);
   289	        if (sorted.size() > n) {
   290	            sorted.resize(n);
   291	        }
   292	        return sorted;
   293	    }
   294	
   295	    std::vector<RankEntry> rank_range(const std::string& board,
   296	                                      std::uint64_t from,
   297	                                      std::uint64_t to) override {
   298	        std::vector<RankEntry> out;
   299	        if (from == 0 || to < from) {
   300	            return out;
   301	        }
   302	        auto redis = connect();
   303	        const auto sorted = sorted_board(redis, board);
   304	        const std::size_t begin = std::min<std::size_t>(
   305	            static_cast<std::size_t>(from - 1), sorted.size());
   306	        const std::size_t end =
   307	            std::min<std::size_t>(static_cast<std::size_t>(to), sorted.size());
   308	        out.assign(sorted.begin() + begin, sorted.begin() + end);
   309	        return out;
   310	    }
   311	
   312	    std::vector<RankEntry> rank_range_by_score(const std::string& board,
   313	                                               double lo, double hi) override {
   314	        std::vector<RankEntry> out;
   315	        if (hi < lo) {
   316	            return out;
   317	        }
   318	        // Pull the whole board in rank order, then filter: ranks stay the
   319	        // global board positions exactly like the in-process implementation.
   320	        auto redis = connect();
   321	        for (const auto& entry : sorted_board(redis, board)) {
   322	            if (entry.score >= lo && entry.score <= hi) {
   323	                out.push_back(entry);
   324	            }
   389	    /// Full board in contract order (score desc, member asc on ties).
   390	    /// No ZSET traversal produces this order: ascending traversal gives
   391	    /// (score asc, member asc) and descending traversal is its exact
   392	    /// reverse, so ties come out member-desc either way. The board is
   393	    /// pulled in one round trip and ordered by the same comparator as the
   394	    /// in-process implementation — the same cost class as its
   395	    /// sort-every-query queries.
   396	    std::vector<RankEntry> sorted_board(
   397	        const std::shared_ptr<sw::redis::Redis>& redis,
   398	        const std::string& board) const {
   399	        const std::string key = prefixed_board(board);
   400	        std::vector<std::pair<std::string, double>> rows;
   401	        redis->zrange(key, 0, -1, std::back_inserter(rows));
   402	        std::vector<RankEntry> out;
   403	        out.reserve(rows.size());
   404	        for (auto& row : rows) {
   405	            out.push_back(RankEntry{// GCOVR_EXCL_BR_LINE (compiler artifact:
   406	                                    // aggregate init-list branches)
   407	                                    std::move(row.first), row.second, 0});
   408	        }
   409	        order_entries(out);
   410	        return out;
   411	    }
   412	
   413	    std::vector<std::string> scan_boards() {
   414	        auto redis = connect();
   415	        std::vector<std::string> keys;
   416	        const std::string pattern = cfg_.prefix + ":rank:*";
   417	        long long cursor = 0;
   418	        do {
   419	            cursor =
   420	                redis->scan(cursor, pattern, 100, std::back_inserter(keys));
   421	        } while (cursor != 0);
   422	        return keys;
   190	// GCOVR_EXCL_START (gcov attribution drift: class header / ctor opening arc
   191	// lands in an uncalled clone on some gcc builds)
   192	class LuaVM {
   193	public:
   194	    LuaVM() : state_(std::make_shared<shd::state>()) {
   195	        // GCOVR_EXCL_STOP
   196	        // Sandbox: lua.sandbox.allow_os / allow_io gate the os and io
   197	        // standard libraries. Unset keys keep the historical behavior
   198	        // (libraries available) so existing deployments that omit the
   199	        // section are not silently changed; the shipped default config
   200	        // declares both false, so a fresh checkout runs sandboxed.
   201	        const bool allow_os =
   202	            shield::config::get_bool("lua.sandbox.allow_os", true);
   203	        const bool allow_io =
   204	            shield::config::get_bool("lua.sandbox.allow_io", true);
   205	
   206	        state_->open_libraries(  // GCOVR_EXCL_LINE (line-continuation
   207	            shd::lib::base,      // GCOVR_EXCL_LINE artifact across args)
   208	            shd::lib::package,   // GCOVR_EXCL_LINE (line-continuation artifact)
   209	            shd::lib::string,    // GCOVR_EXCL_LINE (line-continuation artifact)
   210	            shd::lib::table,     // GCOVR_EXCL_LINE (line-continuation artifact)
   211	            shd::lib::math, shd::lib::coroutine);
   212	        if (allow_io) {
   213	            state_->open_libraries(shd::lib::io);
   214	        }
   215	        if (allow_os) {
   216	            state_->open_libraries(shd::lib::os);
   217	        }
   218	        install_panic_handler(state_);
   219	
   220	        // Set Lua module search path from configuration
   212	## Current Status
   213	
   214	The refactor mainline is landed (roadmap Phases 0-5 and the official optional
   215	modules' P0). The runtime
   216	can load config, start Lua module-table services, validate TCP listener startup,
   217	run local registry and messaging smoke tests, and initialize the plugin host
   218	without linking provider libraries into the core executable.
   219	
   220	Some documented APIs remain target contracts or partial implementations. In
   221	particular, the hello-world TCP gateway verifies listener startup and gateway
   222	event routing; session state (target service, player_id, epoch) lives in the
   223	C++ `SessionBinding` layer and is not exposed to Lua as a userdata.
   224
[0m$ [0mnl -ba docs/runtime-security.md | sed -n '126,136p;301,313p'; nl -ba docs/runtime-global.md | sed -n '755,771p'; nl -ba docs/runtime-cluster.md | sed -n '132,143p;176,197p'; nl -ba docs/architecture-review.md | sed -n '8,28p;130,142p'
126	### 速率限制（网关层）
   127	
   128	网关层限流是连接级防护，在消息进入业务逻辑前拦截。与业务级分布式限流（`shield.rate_limiter()`，见 [全局数据](runtime-global.md#五限流器)）不同：
   129	
   130	| 层级 | 作用 | 存储 | 适用场景 |
   131	|------|------|------|----------|
   132	| 网关层 `rate_limit` | 按连接限流（每连接一个令牌桶） | 内存 | 防刷、防 DDoS |
   133	| 业务层 `shield.rate_limiter()` | 按业务 key 限流 | Redis | 全服限流、API 限流 |
   134	
   135	```yaml
   136	actors:
   301	### 服务间认证
   302	
   303	服务间调用默认信任（同一进程内）。跨节点认证**未实现**：
   304	
   305	```yaml
   306	cluster:
   307	  auth:
   308	    type: token                    # token | mtls（未实现）
   309	    token: ${CLUSTER_TOKEN}        # 未实现
   310	```
   311	
   312	上面这段 `cluster.auth` 配置当前没有任何消费者——不会做 token/mTLS 服务间认证；`${VAR}` 环境变量展开同样未实现（配置加载器不处理占位符，插件会拿到字面量字符串，见 [配置语义](runtime-config.md#环境变量展开未实现)）。实现前的行为：`cluster` 段本身受 optional module 规则约束（未启用 `shield_cluster` 时出现即拒绝启动）；即使启用，`auth` 子键也会被静默忽略（`parse_cluster_config` 只读取 `node_id` / `listen` / `peers` / 心跳与判定超时等键）。
   313	
   755	
   756	## 五、限流器
   757	
   758	业务级限流。与网关层连接级限流（`rate_limit`，见 [安全语义](runtime-security.md#速率限制网关层)）不同：
   759	
   760	> P0 实现状态：限流器完全进程内（token bucket / sliding window 均为 `GlobalManager` 内存结构，零 Redis 调用）。"全服统一配额"（跨进程汇总）依赖 Redis 周期同步，属 Phase 2+；P0 的配额是**本进程**配额。
   761	
   762	| 层级 | 作用 | 存储 | 适用场景 |
   763	|------|------|------|----------|
   764	| 网关层 `rate_limit` | 按客户端 IP/连接限流 | 内存 | 防刷、防 DDoS |
   765	| 业务层 `shield.rate_limiter()` | 按业务 key 限流 | 进程内存（P0；本地+Redis 为 Phase 2+） | 全服限流、API 限流 |
   766	
   767	### 实现策略
   768	
   769	**P0：** 纯进程内实现——`allow()` 内存判断，无任何 Redis 交互，也不存在同步线程；配额以本进程为单位。
   770	
   771	**Phase 2+ 设计意图（未实现）：本地令牌桶 + Redis 周期同步。** 每次 `allow()` 不请求 Redis，使用本地令牌桶纳秒级判断。周期性与 Redis 同步全局配额。
   132	## 节点发现
   133	
   134	节点发现属于 `shield_cluster`，不属于 `shield_core`。Phase 1 只实现静态配置：
   135	
   136	| 方案 | 适用场景 | 外部依赖 | 配置方式 |
   137	|------|----------|----------|----------|
   138	| **静态配置** | 开发/测试/小型部署 | 无 | `cluster.peers` |
   139	| **Redis** | 小型部署/已有 Redis | Redis | `cluster.discovery.type: redis` |
   140	| 广播发现 | Phase 2+ | 无 | `cluster.discovery: broadcast` |
   141	| Kubernetes | Phase 2+ | K8s API | `cluster.discovery: kubernetes` |
   142	| Etcd/Consul | Phase 2+ | Etcd/Consul | `cluster.discovery: etcd` |
   143	
   176	### Redis 服务发现（已实现）
   177	
   178	基于 Redis 的服务发现，适合小型游戏和已有 Redis 的项目。成本低、实现简单、可靠性足够。
   179	存储走 redis++（与 redis.driver 插件同款依赖），连接懒建立——配置了不可达的
   180	Redis 只会在日志里持续告警，不影响进程启动。
   181	
   182	```yaml
   183	cluster:
   184	  node_id: "node-1"
   185	  listen: "0.0.0.0:9000"
   186	  discovery:
   187	    type: redis
   188	    host: "127.0.0.1"
   189	    port: 6379
   190	    password: ""              # 可选
   191	    db: 0
   192	    prefix: "shield:nodes"    # Redis key 前缀
   193	    ttl_seconds: 10           # 节点注册 TTL（秒）
   194	    heartbeat_interval_ms: 3000  # 注册续期间隔（ms）
   195	    scan_interval_ms: 3000       # 节点列表扫描间隔（ms）
   196	```
   197	
     8	总体结论：**架构骨架是健康的，方向正确**——CAF 只作内部 actor 运行时且对业务
     9	完全隐藏、one-VM-per-service 的确定性单线程语义、客户端 RPC 的路由描述符 +
    10	session 单目标绑定、分层可 mock 的时间。主要问题不在骨架，而在**默认值不安全/
    11	不友好、少数声明与实现不符、以及两处对游戏场景真正欠缺的能力（DB 异步路径、
    12	热更新）**。不需要为重构而重构；需要的是把"已经对的设计"补上安全的默认值，
    13	把"声明了但没做"的东西要么做掉要么摘掉声明。
    14	
    15	各维度判断速览：
    16	
    17	| 维度 | 判断 | 主要风险 |
    18	| --- | --- | --- |
    19	| 进程/线程模型 | 合理 | net 单线程默认值是吞吐天花板（低） |
    20	| 网络层与协议 | 合理 | max_frame_size 默认无限（高）、无 TCP_NODELAY（中） |
    21	| 会话与状态管理 | 合理且必须 | — |
    22	| 数据存储与持久化 | 分层合理，执行欠缺 | DB ABI 全同步阻塞（高） |
    23	| 定时器与任务调度 | 合理 | — |
    24	| 热更新与重启策略 | 欠缺（已标注） | blue-green 只有设计稿（中） |
    25	| 扩展点 | 合理 | manifest 仪式感偏重（低） |
    26	| 容错与监控 | 良好 | 进程级 abort 粒度粗（已接受的设计） |
    27	| 横向扩展能力 | P0（口径已声明） | global 仅进程内（已声明，低） |
    28	
   130	## 6. 热更新与重启策略
   131	
   132	**现状**：进程内无热更新命令；设计走 blue-green service replacement（新实例
   133	拉起 → 迁移 → 旧实例退出，runtime-lua-vm.md）；容错层面 per-service
   134	on_error → 连续错误计数（阈值 10）→ on_panic → `exit("panic")`；进程级
   135	`lua_atpanic` = forensics 转储 + abort（lua_panic.hpp）。
   136	
   137	**判断：欠缺**。skynet 的 `clear` 热更新是它的招牌；Shield 用 blue-green
   138	替代原地打补丁，模型上更适合 one-VM-per-service（状态随实例走，无代码/状态
   139	纠缠），**但当前只有设计稿**。对产品化而言这是 P2：新用户 10 分钟不需要它，
   140	运维期一定会要。落地前文档必须保持当前的不承诺口径（现状做到了）。
   141	
   142	容错侧判断**良好**：per-service panic 隔离（坏脚本只退出自己的 service）+
> build · gpt-6-luna
[0m$ [0mnl -ba src/lua/lua_api.cpp | sed -n '4050,4175p'; nl -ba src/lua/lua_runtime.cpp | sed -n '194,218p'; nl -ba include/shield/config/config.hpp | grep -nE 'sandbox|allow_os|allow_io'
4050	        [lua](shd::this_state state,
  4051	              shd::object board_obj) -> shd::variadic_results {
  4052	            shd::state_view s(state);
  4053	            shd::variadic_results results;
  4054	            auto* mgr = shield::global::GlobalManager::global();
  4055	            if (mgr == nullptr) {
  4056	                results.push_back(shd::make_object(s, shd::nil));
  4057	                results.push_back(
  4058	                    make_error(state, "module_unavailable",
  4059	                               "shield_global is not initialized"));
  4060	                return results;
  4061	            }
  4062	            if (!board_obj.is<std::string>()) {
  4063	                results.push_back(shd::make_object(s, shd::nil));
  4064	                results.push_back(make_error(state, "invalid_argument",
  4065	                                             "rank requires a board name"));
  4066	                return results;
  4067	            }
  4068	            const std::string board = board_obj.as<std::string>();
  4069	            shd::table rank = s.create_table();
  4070	            rank.set_function(
  4071	                "update", [mgr, board](shd::object /*self*/, std::string uid,
  4072	                                       double score) {
  4073	                    mgr->rank_update(board, uid, score);
  4074	                    return true;
  4075	                });
  4076	            rank.set_function("mupdate", [mgr, board](shd::object /*self*/,
  4077	                                                      shd::table updates) {
  4078	                std::vector<std::pair<std::string, double>> batch;
  4079	                for (auto&& [uid, score] : updates) {
  4080	                    if (uid.is<std::string>() && score.is<double>()) {
  4081	                        batch.emplace_back(uid.as<std::string>(),
  4082	                                           score.as<double>());
  4083	                    }
  4084	                }
  4085	                mgr->rank_mupdate(board, batch);
  4086	                return true;
  4087	            });
  4088	            rank.set_function(
  4089	                "score",
  4090	                [mgr, board](shd::this_state state, shd::object /*self*/,
  4091	                             shd::object uid) -> shd::object {
  4092	                    shd::state_view s(state);
  4093	                    if (!uid.is<std::string>()) {
  4094	                        return shd::make_object(s, shd::nil);
  4095	                    }
  4096	                    auto value = mgr->rank_score(board, uid.as<std::string>());
  4097	                    return value ? shd::make_object(s, *value)
  4098	                                 : shd::make_object(s, shd::nil);
  4099	                });
  4100	            rank.set_function(
  4101	                "position",
  4102	                [mgr, board](shd::this_state state, shd::object /*self*/,
  4103	                             shd::object uid) -> shd::object {
  4104	                    shd::state_view s(state);
  4105	                    if (!uid.is<std::string>()) {
  4106	                        return shd::make_object(s, shd::nil);
  4107	                    }
  4108	                    auto value =
  4109	                        mgr->rank_position(board, uid.as<std::string>());
  4110	                    return value ? shd::make_object(s, *value)
  4111	                                 : shd::make_object(s, shd::nil);
  4112	                });
  4113	            auto entry_table = [](shd::state_view s,
  4114	                                  const shield::global::RankEntry& entry) {
  4115	                shd::table row = s.create_table();
  4116	                row["uid"] = entry.uid;
  4117	                row["score"] = entry.score;
  4118	                row["rank"] = entry.rank;
  4119	                return row;
  4120	            };  // GCOVR_EXCL_LINE (fn-close artifact)
  4121	            rank.set_function(
  4122	                "top",
  4123	                [mgr, board, entry_table](  // GCOVR_EXCL_BR_LINE (compiler
  4124	                                            // artifact: lambda-entry clone
  4125	                                            // arcs)
  4126	                    shd::this_state
  4127	                        state,  // GCOVR_EXCL_BR_LINE (shd argument-conversion
  4128	                                // arcs at the set_function boundary)
  4129	                    shd::object /*self*/, double n) -> shd::object {
  4130	                    shd::state_view s(state);
  4131	                    shd::table out = s.create_table();
  4132	                    for (const auto& entry :
  4133	                         mgr->rank_top(board, static_cast<std::size_t>(n))) {
  4134	                        out.add(entry_table(s, entry));
  4135	                    }
  4136	                    return out;
  4137	                });
  4138	            rank.set_function(
  4139	                "range",
  4140	                [mgr, board, entry_table](  // GCOVR_EXCL_BR_LINE (compiler
  4141	                                            // artifact: lambda-entry clone
  4142	                                            // arcs)
  4143	                    shd::this_state
  4144	                        state,  // GCOVR_EXCL_BR_LINE (shd argument-conversion
  4145	                                // arcs at the set_function boundary)
  4146	                    shd::object /*self*/, double from,
  4147	                    double to) -> shd::object {
  4148	                    shd::state_view s(state);
  4149	                    shd::table out = s.create_table();
  4150	                    for (const auto& entry : mgr->rank_range(
  4151	                             board, static_cast<std::uint64_t>(from),
  4152	                             static_cast<std::uint64_t>(to))) {
  4153	                        out.add(entry_table(s, entry));
  4154	                    }
  4155	                    return out;
  4156	                });
  4157	            rank.set_function(
  4158	                "range_by_score",
  4159	                [mgr, board, entry_table](  // GCOVR_EXCL_BR_LINE (compiler
  4160	                                            // artifact: lambda-entry clone
  4161	                                            // arcs)
  4162	                    shd::this_state
  4163	                        state,  // GCOVR_EXCL_BR_LINE (shd argument-conversion
  4164	                                // arcs at the set_function boundary)
  4165	                    shd::object /*self*/, double lo, double hi) -> shd::object {
  4166	                    shd::state_view s(state);
  4167	                    shd::table out = s.create_table();
  4168	                    for (const auto& entry :
  4169	                         mgr->rank_range_by_score(board, lo, hi)) {
  4170	                        out.add(entry_table(s, entry));
  4171	                    }
  4172	                    return out;
  4173	                });
  4174	            rank.set_function(
  4175	                "around",
   194	    LuaVM() : state_(std::make_shared<shd::state>()) {
   195	        // GCOVR_EXCL_STOP
   196	        // Sandbox: lua.sandbox.allow_os / allow_io gate the os and io
   197	        // standard libraries. Unset keys keep the historical behavior
   198	        // (libraries available) so existing deployments that omit the
   199	        // section are not silently changed; the shipped default config
   200	        // declares both false, so a fresh checkout runs sandboxed.
   201	        const bool allow_os =
   202	            shield::config::get_bool("lua.sandbox.allow_os", true);
   203	        const bool allow_io =
   204	            shield::config::get_bool("lua.sandbox.allow_io", true);
   205	
   206	        state_->open_libraries(  // GCOVR_EXCL_LINE (line-continuation
   207	            shd::lib::base,      // GCOVR_EXCL_LINE artifact across args)
   208	            shd::lib::package,   // GCOVR_EXCL_LINE (line-continuation artifact)
   209	            shd::lib::string,    // GCOVR_EXCL_LINE (line-continuation artifact)
   210	            shd::lib::table,     // GCOVR_EXCL_LINE (line-continuation artifact)
   211	            shd::lib::math, shd::lib::coroutine);
   212	        if (allow_io) {
   213	            state_->open_libraries(shd::lib::io);
   214	        }
   215	        if (allow_os) {
   216	            state_->open_libraries(shd::lib::os);
   217	        }
   218	        install_panic_handler(state_);
[0m$ [0mnl -ba docs/runtime-cluster.md | sed -n '13,16p;339,357p'; nl -ba docs/runtime-security.md | sed -n '12,16p;303,312p'; nl -ba docs/plugin-system.md | sed -n '883,891p'; nl -ba docs/runtime-global.md | sed -n '1,6p;216,238p;573,589p'
13	实现状态（2026-09，M1 心跳调度 + M2 CAF middleman transport + M3 路由学习 + M4 跨节点投递 + M5 观测收尾已落地，与 [Phase 1 实现范围](#phase-1-实现范围)一致）：
    14	
    15	- **已实现（握手/保活/路由学习/跨节点投递）**：`cluster.*` 配置解析；节点状态数据结构与快照查询；`shield.cluster.nodes()/node_id()/node_epoch()`；`/ops/status` cluster 块与 console 命令；bootstrap 生命周期接线；**心跳调度线程**（`start()` 起内部 `std::jthread` 按 `heartbeat_interval_ms` 驱动 `tick()` 降级，`stop()` 收线）；**CAF middleman transport**（M2）：绑定 `cluster.listen`、主动拨号全部静态 peers、hello/hello_ack 握手（协议版本 v1，身份采纳只在拨号侧完成）、按心跳节拍互发 `HeartbeatMsg`、连接断开即刻 offline 并清路由、带 2s 超时的异步重拨（对端重启可自愈，新 epoch 使旧路由失效）；**route 学习**（M3）：`LuaServiceManager` 名字变更钩子（spawn 发布/register/unregister/退出收回，bootstrap 注入）维护本节点发布表，完整表随每个心跳捎带广播（幂等、自愈，空表自然传播服务下线），握手后立即补发一次加速收敛；接收端要求节点已采纳且 epoch 匹配，整桶替换（stale 实例的表被静默丢弃）；**跨节点投递**（M4）：`EnvelopeMsg`/`EnvelopeReplyMsg` 数据面（详见 [Phase 1 实现范围](#phase-1-实现范围)），`shield.send/call` 对 `node:service` 形态的目标走 envelope 路径。双节点集成测试以两个真实 `caf::actor_system` 走 BASP 验证握手/保活/下线/重启重连/路由收敛与断连清除，并挂真实 Lua 服务管理器验证跨节点 send/call 全链路（`tests/cluster/test_cluster_transport.cpp`）。
    16	- **当前实际行为**：静态 peers 以 `connecting` 占位（临时以拨号地址为节点标识）；握手完成后改名为对端宣布的 `node_id` 并转为 `online`；此后心跳保活，超时按 `online → suspect → offline` 如实降级。对端进程退出会经连接断开事件直接置 `offline`（无 suspect 宽限）并清除其路由缓存；对端重启后自动重连，新 epoch 触发路由失效。`cluster.listen` 绑定失败视为致命错误，启动即失败。`shield.cluster.query(node, name)` 能命中对端真实发布的服务名（本地 `shield.register` 的每一次变更都会传播）；路由命中后 `shield.send`/`shield.call`/`shield.call_timeout` 可直接以 `"node:service"` 为目标完成真实跨节点投递。后续实现路线见 [cluster 实现方案](cluster-implementation-plan.md)。
   339	**已实现，行为可信：**
   340	
   341	- `cluster.node_id/listen/heartbeat_interval_ms/suspect_timeout_ms/offline_timeout_ms/peers` 配置解析。
   342	- 节点状态数据结构、`nodes()` / `find_node()` / `check_node_reachable()` 查询。
   343	- `shield.cluster.nodes()/node_id()/node_epoch()` Lua 查询（`node_id`、`node_epoch` 返回真实本地元数据；`node_epoch` 与节点快照 `epoch` 均为十进制字符串）。
   344	- `/ops/status` 的 cluster 快照、console `root.*` 命令、bootstrap 创建/停止接线。
   345	- **观测指标（M5）**：`ClusterTransport::stats()` 原子计数器——`connections`（活跃连接数，已采纳身份的活连接）、`reconnects`（重连计数，断连后的再拨成功次数）、`tx_messages`/`rx_messages`（数据面 envelope/reply 收发计数，路由表随心跳捎带不计入）、`tx_heartbeats`/`rx_heartbeats`（心跳收发计数）；`ClusterManager::heartbeat_age_ms()` 提供**最近一次心跳时延**（年龄语义：`now - last_heartbeat_ms`，尚未收到心跳为 `-1`；注意 `last_heartbeat_ms` 本身是 steady-clock 值，无墙钟含义，消费方一律读年龄）。`/ops/status` cluster 块与 console `root.status`/`root.cluster` 共享同一 JSON 构建器（`src/console/cluster_status.cpp`），三个出口字段一致：计数器六键（transport 未注册时整组缺席）+ 每节点 `last_heartbeat_ms` 与 `heartbeat_age_ms`（无心跳时为 `null`）。bootstrap 注册/注销 `global_cluster_transport()` 与 manager 全局指针同生命周期。
   346	- **心跳调度线程（M1）**：`start()` 启动内部 `std::jthread`，按 `heartbeat_interval_ms`（下限 50ms）驱动 `tick()` 降级；`stop()` 先收线再清理节点状态。
   347	- **CAF middleman transport（M2，`cluster_transport.cpp`）**：绑定 `cluster.listen`（`0.0.0.0`/`*` 绑全部接口）；按心跳节拍带 2s 超时异步拨号全部静态 peers，断线自动重拨；hello/hello_ack 握手（协议版本 v1，版本不符拒绝完成握手）。身份采纳只发生在拨号侧：ack 的 sender 即本节点拨号所得 proxy，与 peer 条目一一对应，不依赖对端播报的监听地址字符串。握手采纳 `node_id` + `epoch`，`connecting` 占位条目就此改名；epoch 变化的再握手（对端重启）清除该节点路由缓存。心跳按节拍互发，`on_heartbeat` 刷新 liveness 并可把 `suspect`/`offline` 拉回 `online`；连接断开事件直接置 `offline` 并清路由。
   348	- bootstrap 在 CAF actor system 构建后启动 transport（集群 wire 类型必须在任何 `actor_system` 构造前注册，已与 `initialize_caf_types()` 并列处理）；`cluster.listen` 绑定失败为致命错误；两条 teardown 路径均先停 transport 再停 manager。
   349	- `tests/cluster/test_cluster_manager.cpp`（18 例：状态机、握手采纳、epoch 失效、心跳恢复、下线清路由、握手超时降级、本地路由表发布/收回、远端表整桶替换与 epoch 校验、占位键早期路由不泄漏）与 `tests/cluster/test_cluster_transport.cpp`（6 例：双 `caf::actor_system` 真实 BASP 握手/保活/下线/同端口重启重连/路由收敛与断连清除；M4 起挂真实 Lua 服务管理器验证跨节点 call/send 全链路、对端停机 fast-fail `node_offline`、慢 callee 超时语义、路由收回 `service_not_found`）。
   350	- **LuaServiceManager 名字变更钩子（M3）**：`set_name_change_notifier(fn)` 在 registry 锁外回调每次已提交的发布变更（spawn 发布、`shield.register`/`unregister`、服务退出收回；空 `service_id` 表示收回）；bootstrap 将其接到 `ClusterManager::on_local_route_changed()`，本节点发布表由此维护。`LuaServiceManager` 本身零 cluster 依赖。
   351	- **route 学习（M3）**：本节点完整发布表随每个心跳捎带广播（`RoutesMsg`），握手完成后立即补发一次（新节点免等首个心跳即可收敛）；空表自然传播"该节点已无任何服务"。接收端要求节点已采纳且 epoch 匹配后整桶替换，stale 实例（旧 epoch）的表被静默丢弃。
   352	- **跨节点投递（M4，`EnvelopeMsg`/`EnvelopeReplyMsg` 数据面）**：
   353	  - **发送侧**：`RemoteSendFn` 由 bootstrap 注入并桥接到 `ClusterTransport::send_envelope()`（按节点查活连接，无连接即 `node_offline`）。`shield.send`/`shield.call`/`shield.call_timeout` 入口解析目标：**本地名字命中优先**（本地命中的名字绝不会被重解释为 `node:service`）；`node:service` 形态且 node 非本节点 → 可达性预检（`node_not_found`/`node_suspect`/`node_offline`/`node_removed` 即失败，`node_offline`/`node_suspect` 带 retryable 标记）+ route cache 解析（未注册 → `service_not_found`）→ 走 envelope。
   354	  - **信封**：`EnvelopeMsg{source_node, service_id, method, args_json, call_session, timeout_ms}`（`call_session=0` 即单向 send，无回包）；载荷保持 JSON 字符串，wire 类型不依赖 Lua/nlohmann。
   355	  - **接收侧（proxied 会话）**：callee 节点把进来的 call 在本地分配一个 **proxied session**——占用与本地 call 同一张 pending_calls 表、`caller_co=nullptr`，复用完全相同的本地派发、完成与超时机制；完成时经 hook 原路回 `EnvelopeReplyMsg` 到 `source_node`。两边 session 空间独立编号，不存在跨节点撞号。
   356	  - **call 语义与本地同形**：caller 协程 yield → envelope → callee 本地 handler 跑完 → `EnvelopeReplyMsg` → caller 协程以 `(ok, values...)` 恢复；超时由 caller 侧既有 CAF 驱动裁决，callee 侧 proxied 会话的销毁兜底为"caller 剩余超时 + 30s slack"（驱动直达 hook，不依赖 caller 存活）。主线程同步 `call_with_session()` 走同一套 pending_sync_calls 阻塞语义。
   357	  - **降级语义**：peer 非 Online → 预检立即失败（错误码复用 `node_offline`/`node_suspect`，retryable=true，与本地 `service_not_found` 可区分）；对端断连后 route cache 清空 → `service_not_found`。远程失败（对端派发不存在的服务等）以对端错误码经 `EnvelopeReplyMsg` 原样透传。
    12	> **未实现（Phase 2+ 草案）**：下文 per-actor sandbox 资源限制
    13	> （max_instructions/allowed_modules 等）、permissions 权限矩阵、
    14	> mTLS 客户端证书校验——见 `include/shield/config/config.hpp` 与
    15	> [TLS 设计](tls-design.md) 的 M2 清单。若与
    16	> [配置语义](runtime-config.md) 或 [Lua API 契约](lua-api.md) 冲突，
   303	服务间调用默认信任（同一进程内）。跨节点认证**未实现**：
   304	
   305	```yaml
   306	cluster:
   307	  auth:
   308	    type: token                    # token | mtls（未实现）
   309	    token: ${CLUSTER_TOKEN}        # 未实现
   310	```
   311	
   312	上面这段 `cluster.auth` 配置当前没有任何消费者——不会做 token/mTLS 服务间认证；`${VAR}` 环境变量展开同样未实现（配置加载器不处理占位符，插件会拿到字面量字符串，见 [配置语义](runtime-config.md#环境变量展开未实现)）。实现前的行为：`cluster` 段本身受 optional module 规则约束（未启用 `shield_cluster` 时出现即拒绝启动）；即使启用，`auth` 子键也会被静默忽略（`parse_cluster_config` 只读取 `node_id` / `listen` / `peers` / 心跳与判定超时等键）。
   883	以下条目是 v1 收敛项，文档中按目标语义描述，但实现仍需补齐：
   884	
   885	| 项目 | 当前状态 | 目标 |
   886	| --- | --- | --- |
   887	| `config_schema` 校验 | resolve 阶段已应用默认值，并执行类型/必填/范围/enum 校验。 | 继续扩展 JSON Schema 子集和 secret 脱敏输出。 |
   888	| `required: false` | optional 实例在 package/load/create/start 失败时进入 `unavailable`；required 实例失败会中断启动。 | 后续可增加 binding fallback 候选列表。 |
   889	| binding 校验 | resolve 阶段校验 binding 名唯一且目标 instance 存在；访问时仍由 `get_by_binding<T>()` 请求 interface。 | 必要时增加显式 interface binding 格式。 |
   890	| 结构化错误 | C ABI 有 `shield_error_v1`；host 侧错误字符串仍有部分路径是拼接文本。 | 所有阶段统一携带 `code/message/hint/package/instance/phase`。 |
   891	
     1	# 全局能力运行时语义
     2	
     3	> 状态：P0 已落地（`SHIELD_ENABLE_GLOBAL=ON`；测试矩阵 `tests/lua_api/test_lua_api_global.cpp`）。
     4	>
     5	> 本文仍是 `shield_global` 的边界契约；P0 已实现：`shield.global()`（KV + 本地缓存）、互斥/读写/自旋/分布式锁门面、`shield.rank()` 排行榜、普通/延迟/优先级/广播/可靠队列、`shield.scheduler()`（cron/interval/once 与 pause/resume/remove/trigger）、`shield.rate_limiter()`（`allow`/`remaining`/有界 `wait`；token_bucket 与 sliding_window 双算法）。P0 的"分布式"锁、可靠队列与限流器共享进程内 `GlobalManager` 后端（token_bucket 的 Redis 周期同步、sliding_window 的 Redis 计数均为 Phase 2+ 形态；P0 两种算法全程进程内计算，零 Redis 调用，不存在"Redis 不可用"降级语义）；`shield.priority_queue` 与 `shield.broadcast_queue` 已实现（前者进程内多级队列，值越小越优先、同优先级 FIFO；后者进程内为有界 history + 每组 cursor，实时回调分发发生在推送方 VM，离线组在重新 subscribe 时按序补发——跨进程实时广播 Pub/Sub 留 Phase 2+）；KV 数据域、锁域（独占锁+读写锁）与排行域的 Redis 后端已实现（`global.data_backend = redis` / `global.lock_backend = redis` / `global.rank_backend = redis`，见"配置"一节；队列/调度/限流的 Redis 后端仍为 Phase 2+，见文末范围表）。若与 [Lua API 契约](lua-api.md) 或 [配置语义](runtime-config.md) 冲突，以那两份文档为当前主线。
     6	
   216	#### 数据域 Redis 后端（已实现）
   217	
   218	KV 数据域支持把存储切到 Redis：`global.data_backend` 取 `""`（默认，进程内存）或 `"redis"`；选 `redis` 时 `global.redis.host` 必填，其余键可省：
   219	
   220	```yaml
   221	global:
   222	  data_backend: redis
   223	  redis:
   224	    host: 127.0.0.1
   225	    port: 6379          # 默认 6379
   226	    password: ""        # 默认空（不鉴权）
   227	    db: 0               # 默认 0
   228	    prefix: shield:global  # 键前缀，实际存储键为 `<prefix>:<key>`
   229	```
   230	
   231	实现语义（`GlobalDataBackend` 接缝，`src/global/global_data_backend.cpp`）：
   232	
   233	- **懒连接**：构造永不阻塞；首次命令才建立 redis++ 连接。连接失败时命令抛异常，向上传到 Lua 面（无静默降级——跨进程后端静默回退进程内存会造成脑裂）。
   234	- **TTL**：`set(key, value, ttl)` 的 `ttl` 经 PEXPIRE 下发（毫秒精度）；`ttl = 0` 表示永不过期，且会 PERSIST 清掉上次写入留下的 TTL（与进程内语义一致）。
   235	- **incr**：INCRBY；对非整数值 Redis 报错，映射为与进程内相同的错误串（`value of '...' is not an integer`）。
   236	- **mget/mset**：MGET / MSET（mset 再逐键 PEXPIRE）；`size()` 用 SCAN `prefix:*` 计数。
   237	- **缓存一致性**：本地缓存（LRU）留在 `GlobalManager` 委托层——写、删、incr 与惰性过期都会丢弃该键的缓存副本。本地缓存是按 TTL 有界的：Redis 后端下其他进程写入本进程不会立刻感知，缓存副本在自身 TTL 到期前可能短暂陈旧。
   238	- **隔离**：不同部署共用一个 Redis 实例时用不同 `prefix`。
   573	#### 排行域 Redis 后端（已实现）
   574	
   575	排行域（`shield.rank(name)`）支持把榜单切到 Redis：`global.rank_backend` 取 `""`（默认，进程内存）或 `"redis"`；选 `redis` 时与数据域共享 `global.redis.*` 键（`host` 必填）：
   576	
   577	```yaml
   578	global:
   579	  rank_backend: redis
   580	  # redis.* 键与数据域共用一套（host/port/password/db/prefix）
   581	```
   582	
   583	实现语义（`GlobalRankBackend` 接缝，`src/global/global_rank_backend.cpp`）：
   584	
   585	- **懒连接**：与数据域/锁域同规则——构造永不阻塞，首次命令才建连，失败抛异常上抛 Lua 面（无静默降级）。
   586	- **榜单的表示**：每个榜单是 `<prefix>:rank:<board>` 一个 ZSET（member = uid，score 原样存储）；写路径（update/mupdate/remove/clear）与计数（count→ZCARD）是直发 ZSET 命令，无 Lua 脚本。
   587	- **排序**：板内契约是"分数降序、同分 uid 字典序升序"；ZSET 任一方向的遍历都给不出这个序（降序遍历是升序的精确反转，同分会变成 member 降序），因此查询（position/top/range/range_by_score/around）把整榜 ZRANGE 拉回后用与进程内实现相同的比较器排序——`rank` 字段始终是全榜 1 基全局名次，与后端选择无关。
   588	- **一致性**：两种后端的查询语义逐位一致（同分 tie-break、越界钳制、`range_by_score` 双闭区间、`around` 缺席成员返回空报告、无效窗口返回空）。`board_count` 数非空榜单——Redis 侧空 ZSET 自动删除，键数即非空榜数，与进程内"clear 后保留空榜壳（不计数）"的观察口径一致。
   589	- **隔离**：与数据域共用 `prefix`；榜单键为 `<prefix>:rank:*`，与数据键（`<prefix>:<key>`）、锁键（`<prefix>:lock:*`）互不重叠。
[0m$ [0mnl -ba docs/architecture-review.md | sed -n '17,27p;62,74p;103,115p;130,145p;173,182p;209,224p'; nl -ba docs/roadmap.md | sed -n '147,164p'; nl -ba docs/runtime-lua-vm.md | sed -n '67,79p;102,118p'
17	| 维度 | 判断 | 主要风险 |
    18	| --- | --- | --- |
    19	| 进程/线程模型 | 合理 | net 单线程默认值是吞吐天花板（低） |
    20	| 网络层与协议 | 合理 | max_frame_size 默认无限（高）、无 TCP_NODELAY（中） |
    21	| 会话与状态管理 | 合理且必须 | — |
    22	| 数据存储与持久化 | 分层合理，执行欠缺 | DB ABI 全同步阻塞（高） |
    23	| 定时器与任务调度 | 合理 | — |
    24	| 热更新与重启策略 | 欠缺（已标注） | blue-green 只有设计稿（中） |
    25	| 扩展点 | 合理 | manifest 仪式感偏重（低） |
    26	| 容错与监控 | 良好 | 进程级 abort 粒度粗（已接受的设计） |
    27	| 横向扩展能力 | P0（口径已声明） | global 仅进程内（已声明，低） |
    62	**风险**：
    63	- 【高】`max_frame_size` 默认 0 = **不限**（include/shield/net/listener.hpp:88
    64	  `// 0 = unlimited`；session.hpp:153 同）。恶意/异常客户端发一个巨大长度前缀
    65	  即可推动服务端按其分配缓冲。对公网监听这是明确的 DoS 面。**建议：默认给
    66	  16MB 上限（config 校验层兜底），显式配置可覆盖**。这是"安全默认值"问题，
    67	  不是架构问题，修复成本低、产品收益直接。
    68	- 【中】全代码库无 `TCP_NODELAY`（src/net/ 零命中）。Nagle 开着，小包回路
    69	  延迟对回合制/实时游戏是可感知劣化。asio socket 默认即 Nagle on。建议 accept
    70	  后统一 `set_option(tcp::no_delay(true))`。
    71	- ~~【中】无连接级限流/黑名单。~~ **已解决（2026-09-26）**：
    72	  `network.rate_limit`（每连接令牌桶，按解码后消息计费、超限丢帧不断连）
    73	  与 `network.blocklist.deny`（accept 时按地址/CIDR 拒绝、启动期校验条目）
    74	  均已落地，语义见 [安全运行时语义](runtime-security.md)。
   103	- 【高】**ABI 全同步阻塞，无异步/回调路径**（include/shield/plugin/database.h
   104	  全表无 callback 参数）。后果：任何 Lua service 里发起的 DB 调用会**卡住该
   105	  service 的整个 VM**——单玩家 service 卡自己尚可忍，共享 service（world/
   106	  ranking）里一次慢查询 = 全服卡顿。skynet 同样没有内置 DB，但它的惯例
   107	  （agent 挂起 + driver 服务）让阻塞被隔离在独立服务里；Shield 的文档还没有
   108	  把"DB 调用必须放进独立 service / 必须配超时"写成硬规则。**建议两步走：
   109	  (a) 立即在文档写明使用纪律 + 示例（把 DB 封装进专职 service，业务经
   110	  shield.call 访问）；(b) Phase 2 给 ABI 加异步入口（callback 或 future +
   111	  协程恢复）**。这不是重构，是补一个已知缺口。(a) 已完成（
   112	  [DB 使用纪律](db-discipline.md)）；(b) 已落地（
   113	  [DB 异步入口](db-async-design.md)，协程恢复式，M1–M4；纪律文档已按落地
   114	  计划同步降级——Lua 面不再全同步，C vtable 保持同步原语不变）。
   115	
   130	## 6. 热更新与重启策略
   131	
   132	**现状**：进程内无热更新命令；设计走 blue-green service replacement（新实例
   133	拉起 → 迁移 → 旧实例退出，runtime-lua-vm.md）；容错层面 per-service
   134	on_error → 连续错误计数（阈值 10）→ on_panic → `exit("panic")`；进程级
   135	`lua_atpanic` = forensics 转储 + abort（lua_panic.hpp）。
   136	
   137	**判断：欠缺**。skynet 的 `clear` 热更新是它的招牌；Shield 用 blue-green
   138	替代原地打补丁，模型上更适合 one-VM-per-service（状态随实例走，无代码/状态
   139	纠缠），**但当前只有设计稿**。对产品化而言这是 P2：新用户 10 分钟不需要它，
   140	运维期一定会要。落地前文档必须保持当前的不承诺口径（现状做到了）。
   141	
   142	容错侧判断**良好**：per-service panic 隔离（坏脚本只退出自己的 service）+
   143	进程级兜底 abort + forensics，比 skynet 默认（错误一路 printf）更适合产品。
   144	abort 粒度粗（单 VM 不可恢复错误 = 全进程死）是已知取舍，配合 systemd/k8s
   145	重启策略可接受，文档应写明。
   173	**风险**：
   174	- 【中】**声明与实现不符：sandbox**。config/app.yaml 声明
   175	  `lua.sandbox.allow_os/allow_io: false`，但 VM 启动**无条件** open 了
   176	  `shd::lib::io` 与 `shd::lib::os`（src/lua/lua_runtime.cpp:194-202），
   177	  runtime-security.md 也自认未实现。对产品这是诚实性问题：**配置键声明了
   178	  却不生效**比"没有沙箱"更糟（用户以为关了，实际开着）。建议：实现该开关
   179	  （按配置条件 open，成本低）或删键。倾向前者——脚本来自策划/外包的团队
   180	  需要它。**本轮落地**。
   181	- 【低】metrics 口径无文档（有 /ops/metrics 端点，内容语义未成文）。
   182	
   209	**欠缺**（按优先级）：
   210	1. 安全默认值：max_frame_size 默认无限（本轮修）；TCP_NODELAY（本轮修）；
   211	   连接限流 + 地址黑名单（本轮修，见 §2 已解决项）
   212	2. sandbox 开关声明未实现（本轮修——产品诚实性）
   213	3. DB 异步路径（已落地：纪律文档 + [协程恢复式异步入口](db-async-design.md) M1–M4）
   214	4. 热更新 blue-green 落地（P2 另立项）
   215	5. net.threads 默认值与调优文档（本轮文档带出）
   216	
   217	## 改进路线（服务产品化，不重构骨架）
   218	
   219	| 阶段 | 内容 | 取舍理由 |
   220	| --- | --- | --- |
   221	| 本轮 P0 | 安全默认值（frame 上限/NODELAY）+ sandbox 开关实现 + 默认配置可观测 + 模板工程 + 上手路径线性化（详见 product-gap.md） | 全部是小改动大收益的"默认值/诚实性"问题；不动架构骨架 |
   222	| Phase 1 | DB 使用纪律文档与示例；metrics 口径文档；net.threads 调优指引；限流设计 | 文档与低风险改动为主，先把已知坑标出来 |
   223	| Phase 2 | DB ABI 异步入口（已落地，M1–M4）；连接级限流；blue-green 热更新落地；TLS | 各自独立立项，均有既有设计稿或文档位次；按产品反馈排期 |
   224	
   147	## Later
   148	
   149	以下内容不属于当前 refactor core，也不属于已冻结的官方模块契约：
   150	
   151	- 高级数据 mapper。
   152	- Schema 工具链。
   153	- Lua runtime primitives / cosocket 风格出站 I/O（如 `shield.buffer` / `shield.socket` /
   154	  `shield.stream` / `shield.tls`）。
   155	- `shield.crypto` 一期切片已落地（2026-09-28，AD-08：编解码 / SHA-256 /
   156	  HMAC-SHA256 / `random_bytes` / `constant_time_compare`），作为业务层自建认证的原语基础；
   157	  二期四刀（KDF → 对称加密 → 非对称 → JWKS）已全部落地（2026-10-09）：
   158	  PBKDF2/HKDF、AES-256-GCM、Ed25519、JWT EdDSA + JWKS（`scripts/lib/jwt.lua`）。
   159	
   160	说明：
   161	
   162	- 当前主路径不要求用户理解或使用上述 primitives。
   163	- 当前推荐心智模型仍然是：客户端入站走 gateway 单一 target + `ClientContext`/`shield.client_rpc.<name>` helper，服务间走 `shield.send/call`，对外 HTTP 走 `shield.http`。
   164	- primitives / cosocket 方向只保留为后置草案，不作为当前阶段阻塞项。
    67	### 实现 API
    68	
    69	name 所有权转移用 **`shield.claim(name)`**（原子接管，见
    70	[Lua API 契约](lua-api.md#shieldclaimname)）。注意不能用
    71	`unregister + register` 拼——register 只把 **当前** service 注册为 owner，
    72	旧 service 无法替新 service 注册名字，且先注销再注册会让 name 出现解析
    73	空窗。claim 由**新 service 在自己的 handler 里调用**，registry 锁内原子
    74	换 owner，全程无空窗：
    75	
    76	```lua
    77	-- 1. 编排方 spawn 新代码（临时名字，spawn 时自动注册）
    78	local new_handle, err = shield.spawn("player", {
    79	    name = "player.1.new",
   102	-- 3. 新 service 原子接管生产 name（在它自己的 migrate handler 里：
   103	--    function M.migrate(ctx, state)
   104	--        ... 导入状态 ...
   105	--        return shield.claim("player.1")
   106	--    end）
   107	--    claim 成功即生效：后续按 name 的 send/call 全部落到新 service。
   108	--    旧 service 收到 name 变更通知（或由编排方显式通知）进入 drain。
   109	
   110	-- 4. Old service draining
   111	--    actor 模型天然串行：旧 service 把在途消息处理完即可退出；
   112	--    drain 期间不再接新业务（业务侧用 M.draining 挡新增请求）。
   113	M.draining = true
   114	
   115	-- 5. Old service exit（旧 service 自己的 handler 里）
   116	shield.exit("upgraded")   -- 退出清理只收回它仍拥有的名字，"player.1"
   117	                          -- 已归新 service，不受影响
   118	```
[0m$ [0mnl -ba docs/runtime-cluster.md | sed -n '327,366p'; nl -ba src/cluster/cluster_transport.cpp | sed -n '274,303p'; nl -ba src/cluster/cluster_transport.cpp | sed -n '120,175p'
327	## Phase 1 实现范围
   328	
   329	`shield_cluster` 第一版实现范围：
   330	
   331	- 静态 peers 配置。
   332	- 静态 peers 配置解析。
   333	- 节点状态快照和 `online/suspect/offline/removed` 状态模型。
   334	- 远端路由 cache 结构。
   335	- 显式 `(node_id, service_name)` route cache 查询。
   336	
   337	当前源码实现状态（`src/cluster/`，2026-09 实测核对）：
   338	
   339	**已实现，行为可信：**
   340	
   341	- `cluster.node_id/listen/heartbeat_interval_ms/suspect_timeout_ms/offline_timeout_ms/peers` 配置解析。
   342	- 节点状态数据结构、`nodes()` / `find_node()` / `check_node_reachable()` 查询。
   343	- `shield.cluster.nodes()/node_id()/node_epoch()` Lua 查询（`node_id`、`node_epoch` 返回真实本地元数据；`node_epoch` 与节点快照 `epoch` 均为十进制字符串）。
   344	- `/ops/status` 的 cluster 快照、console `root.*` 命令、bootstrap 创建/停止接线。
   345	- **观测指标（M5）**：`ClusterTransport::stats()` 原子计数器——`connections`（活跃连接数，已采纳身份的活连接）、`reconnects`（重连计数，断连后的再拨成功次数）、`tx_messages`/`rx_messages`（数据面 envelope/reply 收发计数，路由表随心跳捎带不计入）、`tx_heartbeats`/`rx_heartbeats`（心跳收发计数）；`ClusterManager::heartbeat_age_ms()` 提供**最近一次心跳时延**（年龄语义：`now - last_heartbeat_ms`，尚未收到心跳为 `-1`；注意 `last_heartbeat_ms` 本身是 steady-clock 值，无墙钟含义，消费方一律读年龄）。`/ops/status` cluster 块与 console `root.status`/`root.cluster` 共享同一 JSON 构建器（`src/console/cluster_status.cpp`），三个出口字段一致：计数器六键（transport 未注册时整组缺席）+ 每节点 `last_heartbeat_ms` 与 `heartbeat_age_ms`（无心跳时为 `null`）。bootstrap 注册/注销 `global_cluster_transport()` 与 manager 全局指针同生命周期。
   346	- **心跳调度线程（M1）**：`start()` 启动内部 `std::jthread`，按 `heartbeat_interval_ms`（下限 50ms）驱动 `tick()` 降级；`stop()` 先收线再清理节点状态。
   347	- **CAF middleman transport（M2，`cluster_transport.cpp`）**：绑定 `cluster.listen`（`0.0.0.0`/`*` 绑全部接口）；按心跳节拍带 2s 超时异步拨号全部静态 peers，断线自动重拨；hello/hello_ack 握手（协议版本 v1，版本不符拒绝完成握手）。身份采纳只发生在拨号侧：ack 的 sender 即本节点拨号所得 proxy，与 peer 条目一一对应，不依赖对端播报的监听地址字符串。握手采纳 `node_id` + `epoch`，`connecting` 占位条目就此改名；epoch 变化的再握手（对端重启）清除该节点路由缓存。心跳按节拍互发，`on_heartbeat` 刷新 liveness 并可把 `suspect`/`offline` 拉回 `online`；连接断开事件直接置 `offline` 并清路由。
   348	- bootstrap 在 CAF actor system 构建后启动 transport（集群 wire 类型必须在任何 `actor_system` 构造前注册，已与 `initialize_caf_types()` 并列处理）；`cluster.listen` 绑定失败为致命错误；两条 teardown 路径均先停 transport 再停 manager。
   349	- `tests/cluster/test_cluster_manager.cpp`（18 例：状态机、握手采纳、epoch 失效、心跳恢复、下线清路由、握手超时降级、本地路由表发布/收回、远端表整桶替换与 epoch 校验、占位键早期路由不泄漏）与 `tests/cluster/test_cluster_transport.cpp`（6 例：双 `caf::actor_system` 真实 BASP 握手/保活/下线/同端口重启重连/路由收敛与断连清除；M4 起挂真实 Lua 服务管理器验证跨节点 call/send 全链路、对端停机 fast-fail `node_offline`、慢 callee 超时语义、路由收回 `service_not_found`）。
   350	- **LuaServiceManager 名字变更钩子（M3）**：`set_name_change_notifier(fn)` 在 registry 锁外回调每次已提交的发布变更（spawn 发布、`shield.register`/`unregister`、服务退出收回；空 `service_id` 表示收回）；bootstrap 将其接到 `ClusterManager::on_local_route_changed()`，本节点发布表由此维护。`LuaServiceManager` 本身零 cluster 依赖。
   351	- **route 学习（M3）**：本节点完整发布表随每个心跳捎带广播（`RoutesMsg`），握手完成后立即补发一次（新节点免等首个心跳即可收敛）；空表自然传播"该节点已无任何服务"。接收端要求节点已采纳且 epoch 匹配后整桶替换，stale 实例（旧 epoch）的表被静默丢弃。
   352	- **跨节点投递（M4，`EnvelopeMsg`/`EnvelopeReplyMsg` 数据面）**：
   353	  - **发送侧**：`RemoteSendFn` 由 bootstrap 注入并桥接到 `ClusterTransport::send_envelope()`（按节点查活连接，无连接即 `node_offline`）。`shield.send`/`shield.call`/`shield.call_timeout` 入口解析目标：**本地名字命中优先**（本地命中的名字绝不会被重解释为 `node:service`）；`node:service` 形态且 node 非本节点 → 可达性预检（`node_not_found`/`node_suspect`/`node_offline`/`node_removed` 即失败，`node_offline`/`node_suspect` 带 retryable 标记）+ route cache 解析（未注册 → `service_not_found`）→ 走 envelope。
   354	  - **信封**：`EnvelopeMsg{source_node, service_id, method, args_json, call_session, timeout_ms}`（`call_session=0` 即单向 send，无回包）；载荷保持 JSON 字符串，wire 类型不依赖 Lua/nlohmann。
   355	  - **接收侧（proxied 会话）**：callee 节点把进来的 call 在本地分配一个 **proxied session**——占用与本地 call 同一张 pending_calls 表、`caller_co=nullptr`，复用完全相同的本地派发、完成与超时机制；完成时经 hook 原路回 `EnvelopeReplyMsg` 到 `source_node`。两边 session 空间独立编号，不存在跨节点撞号。
   356	  - **call 语义与本地同形**：caller 协程 yield → envelope → callee 本地 handler 跑完 → `EnvelopeReplyMsg` → caller 协程以 `(ok, values...)` 恢复；超时由 caller 侧既有 CAF 驱动裁决，callee 侧 proxied 会话的销毁兜底为"caller 剩余超时 + 30s slack"（驱动直达 hook，不依赖 caller 存活）。主线程同步 `call_with_session()` 走同一套 pending_sync_calls 阻塞语义。
   357	  - **降级语义**：peer 非 Online → 预检立即失败（错误码复用 `node_offline`/`node_suspect`，retryable=true，与本地 `service_not_found` 可区分）；对端断连后 route cache 清空 → `service_not_found`。远程失败（对端派发不存在的服务等）以对端错误码经 `EnvelopeReplyMsg` 原样透传。
   358	
   359	**由此产生的当前实际行为（业务与运维须知）：**
   360	
   361	- 静态 peers 以 `connecting` 占位（临时以拨号地址为节点标识）；握手完成转为 `online` 并按真实 `node_id` 可查。对端不可达时，`connecting` 在 `offline_timeout_ms` 后如实降级为 `offline`——配置了不存在的 peer 不会被报成健康节点。
   362	- 在线 peers 由心跳保活；对端进程退出即刻 `offline` 并清其路由（无 suspect 宽限）；对端重启后自动重连，新 epoch 触发路由失效。`suspect_timeout_ms` 后降级 `suspect`，`offline_timeout_ms` 后降级 `offline`，由心跳调度线程自动完成。
   363	- `shield.cluster.query(node, name)` 可命中对端真实发布的服务名（收敛延迟：握手后 ≤ 一个心跳周期）；对端服务注销后同名查询随下一次心跳失效；peer 降级后同一调用先被可达性检查以 `node_suspect`/`node_offline` 拦下，"节点不可达"与"路由未注册"自此可区分。查询命中之后 `shield.send`/`shield.call` 即完成真实跨节点投递（`"node:service"` 目标形态）。
   364	- `node_epoch` 与节点快照 `epoch` 以十进制字符串经 Lua/JSON 暴露（uint64 直接过 double 会丢精度）；stale-handle 比对须按字符串比较。M2 起握手 epoch 具备跨节点语义：peer 重启（新 epoch）即宣告其旧路由全部失效。
   365	
   366	因此：启用 `cluster.node_id` 后，集群会呈现真实的连接/握手/保活/下线视图（健康节点 `online`，故障节点 `offline`），本地服务发布表在集群内自动同步，`shield.cluster.query` 能解析远端服务名，且 `shield.send/call` 可对 `"node:service"` 目标完成真实跨节点投递（超时/错误码语义与本地 call 同形）。未配置 `cluster.node_id` 时整条路径不激活，单节点部署不受影响。远端连接失败的 degrade 契约（[官方可选模块契约](optional-modules.md) 的"必须持续暴露 unhealthy 状态"）已满足：故障节点如实暴露 `offline`。
   274	        // -- inbound handshake ---------------------------------------------
   275	        [self](const HelloMsg& hello) {  // GCOVR_EXCL_LINE (gcov attributes no
   276	                                         // code to this line)
   277	            auto& log = shield::log::get_logger("cluster");
   278	            SHIELD_LOG_DEBUG(log, "Hello from " + hello.node_id + " (epoch " +
   279	                                      std::to_string(hello.epoch) + ")");
   280	            if (hello.proto_version != kClusterProtoVersion) {
   281	                SHIELD_LOG_ERROR(log, "Peer protocol version mismatch: got " +
   282	                                          std::to_string(hello.proto_version) +
   283	                                          ", expected " +
   284	                                          std::to_string(kClusterProtoVersion));
   285	                return;
   286	            }
   287	            // Reply over the inbound connection; the dialer adopts our
   288	            // identity from this ack. We adopt nothing here — our own
   289	            // adoption happens via our own dial (both sides dial). The ack
   290	            // must carry our address as sender too: the dialer matches it
   291	            // against the proxy it dialed.
   292	            if (auto sender =
   293	                    self->current_sender()) {  // GCOVR_EXCL_BR_LINE (defensive:
   294	                                               // current_sender only null for
   295	                                               // non-request messages; HelloMsg
   296	                                               // is a request)
   297	                self->send(caf::actor_cast<caf::actor>(sender),
   298	                           HelloAckMsg{// GCOVR_EXCL_BR_LINE (compiler artifact:
   299	                                       // aggregate init-list branches)
   300	                                       self->state().self_node_id,
   301	                                       self->state().self_epoch,
   302	                                       kClusterProtoVersion});
   303	            }
   120	};
   121	
   122	using transport_actor = caf::stateful_actor<transport_state>;
   123	
   124	caf::behavior transport_loop(transport_actor* self, ClusterManager* manager,
   125	                             const ClusterConfig cfg, const SidePtr& side) {
   126	    auto& st = self->state();
   127	    st.manager = manager;
   128	    st.self_node_id = cfg.node_id;
   129	    st.self_epoch = manager->node_epoch();
   130	    st.heartbeat_interval_ms = cfg.heartbeat_interval_ms;
   131	    for (const auto& address : cfg.peers) {
   132	        // Split "host:port". Invalid entries are skipped and logged; the
   133	        // manager still tracks them so they degrade honestly.
   134	        auto colon = address.rfind(':');
   135	        if (colon == std::string::npos || colon == 0 ||
   136	            colon + 1 == address.size()) {
   137	            auto& log = shield::log::get_logger("cluster");
   138	            SHIELD_LOG_ERROR(log, "Invalid peer address, skipping: " + address);
   139	            continue;
   140	        }
   141	        PeerLink link;
   142	        link.address = address;
   143	        link.host = address.substr(0, colon);
   144	        link.port = static_cast<uint16_t>(std::stoi(address.substr(colon + 1)));
   145	        st.peers.push_back(std::move(link));
   146	    }
   147	
   148	    auto& log = shield::log::get_logger("cluster");
   149	
   150	    self->set_down_handler([self, side](caf::down_msg& dm) {
   151	        // The connection to a dialed peer died: definitive offline for that
   152	        // node, routes invalidated; the connect loop redials.
   153	        auto& st = self->state();
   154	        for (auto& peer :
   155	             st.peers) {  // GCOVR_EXCL_BR_LINE (defensive: down_msg only fires
   156	                          // for monitored handles; loop body guarded by handle
   157	                          // equality)
   158	            if (peer.handle &&  // GCOVR_EXCL_BR_LINE (defensive: down_msg
   159	                                // carries exactly one source handle, so the
   160	                                // != arm is loop continuation only)
   161	                peer.handle == dm.source) {
   162	                st.manager->on_peer_down(peer.address);
   163	                if (!peer.node_id.empty()) {
   164	                    std::unique_lock lock(side->peers_mutex);
   165	                    side->peers_by_node.erase(peer.node_id);
   166	                }
   167	                peer.dropped = true;
   168	                peer.handle = nullptr;
   169	                peer.node_id.clear();
   170	                break;
   171	            }
   172	        }
   173	    });
   174	
   175	    return {
> build · gpt-6-luna
[0m✱ [0mGrep "ClientContext|ClientRef|session_id\(\)|shield.client.bind" [90min docs/lua-api.md · 22 matches[0m
[0m✱ [0mGrep "ClientContext|ClientRef|session_id" [90min src/lua · 83 matches[0m
[0m$ [0mgit ls-files 'tests/acceptance/*' 'tests/cluster/*' | sort
tests/acceptance/test_client_rpc_e2e.cpp
tests/acceptance/test_default_config_boot.cpp
tests/cluster/test_cluster_manager.cpp
tests/cluster/test_cluster_transport.cpp
tests/cluster/test_node_discovery.cpp
[0m$ [0mnl -ba docs/runtime-security.md | sed -n '1,17p'; nl -ba docs/runtime-global.md | sed -n '755,766p'; nl -ba docs/runtime-cluster.md | sed -n '132,143p;176,180p;327,335p'; nl -ba docs/architecture-review.md | sed -n '17,27p;62,73p;103,114p;173,181p'
1	# 安全运行时语义
     2	
     3	> 状态：部分实现 + 部分设计草案。
     4	>
     5	> **已实现**：`lua.sandbox.allow_os` / `lua.sandbox.allow_io`（全局级
     6	> VM 标准库开关，见 [配置语义](runtime-config.md)；未设置时保持历史
     7	> 行为=开放，随仓库分发的默认配置声明两者为 false）；网关层连接级
     8	> `rate_limit`（令牌桶）与 `blocklist.deny`（accept 时按地址/CIDR 拒绝，
     9	> 见下文「速率限制」与「地址黑名单」）；客户端面 TLS
    10	> （`network.tls`，见下文「TLS 支持」与 [TLS 设计](tls-design.md)）。
    11	>
    12	> **未实现（Phase 2+ 草案）**：下文 per-actor sandbox 资源限制
    13	> （max_instructions/allowed_modules 等）、permissions 权限矩阵、
    14	> mTLS 客户端证书校验——见 `include/shield/config/config.hpp` 与
    15	> [TLS 设计](tls-design.md) 的 M2 清单。若与
    16	> [配置语义](runtime-config.md) 或 [Lua API 契约](lua-api.md) 冲突，
    17	> 以那两份文档为准。
   755	
   756	## 五、限流器
   757	
   758	业务级限流。与网关层连接级限流（`rate_limit`，见 [安全语义](runtime-security.md#速率限制网关层)）不同：
   759	
   760	> P0 实现状态：限流器完全进程内（token bucket / sliding window 均为 `GlobalManager` 内存结构，零 Redis 调用）。"全服统一配额"（跨进程汇总）依赖 Redis 周期同步，属 Phase 2+；P0 的配额是**本进程**配额。
   761	
   762	| 层级 | 作用 | 存储 | 适用场景 |
   763	|------|------|------|----------|
   764	| 网关层 `rate_limit` | 按客户端 IP/连接限流 | 内存 | 防刷、防 DDoS |
   765	| 业务层 `shield.rate_limiter()` | 按业务 key 限流 | 进程内存（P0；本地+Redis 为 Phase 2+） | 全服限流、API 限流 |
   766	
   132	## 节点发现
   133	
   134	节点发现属于 `shield_cluster`，不属于 `shield_core`。Phase 1 只实现静态配置：
   135	
   136	| 方案 | 适用场景 | 外部依赖 | 配置方式 |
   137	|------|----------|----------|----------|
   138	| **静态配置** | 开发/测试/小型部署 | 无 | `cluster.peers` |
   139	| **Redis** | 小型部署/已有 Redis | Redis | `cluster.discovery.type: redis` |
   140	| 广播发现 | Phase 2+ | 无 | `cluster.discovery: broadcast` |
   141	| Kubernetes | Phase 2+ | K8s API | `cluster.discovery: kubernetes` |
   142	| Etcd/Consul | Phase 2+ | Etcd/Consul | `cluster.discovery: etcd` |
   143	
   176	### Redis 服务发现（已实现）
   177	
   178	基于 Redis 的服务发现，适合小型游戏和已有 Redis 的项目。成本低、实现简单、可靠性足够。
   179	存储走 redis++（与 redis.driver 插件同款依赖），连接懒建立——配置了不可达的
   180	Redis 只会在日志里持续告警，不影响进程启动。
   327	## Phase 1 实现范围
   328	
   329	`shield_cluster` 第一版实现范围：
   330	
   331	- 静态 peers 配置。
   332	- 静态 peers 配置解析。
   333	- 节点状态快照和 `online/suspect/offline/removed` 状态模型。
   334	- 远端路由 cache 结构。
   335	- 显式 `(node_id, service_name)` route cache 查询。
    17	| 维度 | 判断 | 主要风险 |
    18	| --- | --- | --- |
    19	| 进程/线程模型 | 合理 | net 单线程默认值是吞吐天花板（低） |
    20	| 网络层与协议 | 合理 | max_frame_size 默认无限（高）、无 TCP_NODELAY（中） |
    21	| 会话与状态管理 | 合理且必须 | — |
    22	| 数据存储与持久化 | 分层合理，执行欠缺 | DB ABI 全同步阻塞（高） |
    23	| 定时器与任务调度 | 合理 | — |
    24	| 热更新与重启策略 | 欠缺（已标注） | blue-green 只有设计稿（中） |
    25	| 扩展点 | 合理 | manifest 仪式感偏重（低） |
    26	| 容错与监控 | 良好 | 进程级 abort 粒度粗（已接受的设计） |
    27	| 横向扩展能力 | P0（口径已声明） | global 仅进程内（已声明，低） |
    62	**风险**：
    63	- 【高】`max_frame_size` 默认 0 = **不限**（include/shield/net/listener.hpp:88
    64	  `// 0 = unlimited`；session.hpp:153 同）。恶意/异常客户端发一个巨大长度前缀
    65	  即可推动服务端按其分配缓冲。对公网监听这是明确的 DoS 面。**建议：默认给
    66	  16MB 上限（config 校验层兜底），显式配置可覆盖**。这是"安全默认值"问题，
    67	  不是架构问题，修复成本低、产品收益直接。
    68	- 【中】全代码库无 `TCP_NODELAY`（src/net/ 零命中）。Nagle 开着，小包回路
    69	  延迟对回合制/实时游戏是可感知劣化。asio socket 默认即 Nagle on。建议 accept
    70	  后统一 `set_option(tcp::no_delay(true))`。
    71	- ~~【中】无连接级限流/黑名单。~~ **已解决（2026-09-26）**：
    72	  `network.rate_limit`（每连接令牌桶，按解码后消息计费、超限丢帧不断连）
    73	  与 `network.blocklist.deny`（accept 时按地址/CIDR 拒绝、启动期校验条目）
   103	- 【高】**ABI 全同步阻塞，无异步/回调路径**（include/shield/plugin/database.h
   104	  全表无 callback 参数）。后果：任何 Lua service 里发起的 DB 调用会**卡住该
   105	  service 的整个 VM**——单玩家 service 卡自己尚可忍，共享 service（world/
   106	  ranking）里一次慢查询 = 全服卡顿。skynet 同样没有内置 DB，但它的惯例
   107	  （agent 挂起 + driver 服务）让阻塞被隔离在独立服务里；Shield 的文档还没有
   108	  把"DB 调用必须放进独立 service / 必须配超时"写成硬规则。**建议两步走：
   109	  (a) 立即在文档写明使用纪律 + 示例（把 DB 封装进专职 service，业务经
   110	  shield.call 访问）；(b) Phase 2 给 ABI 加异步入口（callback 或 future +
   111	  协程恢复）**。这不是重构，是补一个已知缺口。(a) 已完成（
   112	  [DB 使用纪律](db-discipline.md)）；(b) 已落地（
   113	  [DB 异步入口](db-async-design.md)，协程恢复式，M1–M4；纪律文档已按落地
   114	  计划同步降级——Lua 面不再全同步，C vtable 保持同步原语不变）。
   173	**风险**：
   174	- 【中】**声明与实现不符：sandbox**。config/app.yaml 声明
   175	  `lua.sandbox.allow_os/allow_io: false`，但 VM 启动**无条件** open 了
   176	  `shd::lib::io` 与 `shd::lib::os`（src/lua/lua_runtime.cpp:194-202），
   177	  runtime-security.md 也自认未实现。对产品这是诚实性问题：**配置键声明了
   178	  却不生效**比"没有沙箱"更糟（用户以为关了，实际开着）。建议：实现该开关
   179	  （按配置条件 open，成本低）或删键。倾向前者——脚本来自策划/外包的团队
   180	  需要它。**本轮落地**。
   181	- 【低】metrics 口径无文档（有 /ops/metrics 端点，内容语义未成文）。
> build · gpt-6-luna
[0m→ [0mRead src/cluster/cluster_transport.cpp [90m[offset=380, limit=210][0m
> build · gpt-6-luna
- `todo.md` 的 122 项均已勾选；但“全部完成”不等于运行时边界已补齐。按风险排序：

1. **集群数据面缺少对端认证，消息来源未绑定已握手节点（高）**：`src/cluster/cluster_transport.cpp:275-303` 的握手只校验协议版本；`436-455、519-523` 收到 `EnvelopeMsg` 后直接派发，没有校验 `source_node` 是否对应已认证连接。能接入集群端口的对端可能伪造服务间调用。配置中的 `cluster.auth` 目前也没有消费者：`docs/runtime-security.md:303-312`。集群监听若暴露在不可信网络，应视为发布阻断项。
2. **外部存储调用仍可能阻塞 service actor（高）**：Redis 插件在 Lua 调用路径中直接同步执行 `redis++` 命令，例：`plugins/redis.driver/shield_redis_driver.cpp:300-320`；global API 同步调用后端，例：`src/lua/lua_api.cpp:4071-4085`。慢 Redis 会阻塞该 VM 的消息、定时器和 RPC。Redis 排行榜查询还会把整榜拉回再排序：`src/global/global_rank_backend.cpp:389-410`，规模增大时延迟和内存开销随榜单线性增长。DB 异步化没有覆盖 Redis、MongoDB 和非协程调用路径。
3. **Lua VM 隔离不是资源隔离（中高）**：`src/lua/lua_runtime.cpp:196-217` 只按全局配置开关 `io/os`，且配置缺省时两者默认开放；每 service 的指令数、内存上限和权限矩阵未实现，文档也明确列为草案：`docs/runtime-security.md:12-16`。脚本死循环或持续分配内存没有 per-service 硬边界，可能耗尽调度资源甚至拖垮进程。

- **最值得补的 3 项新能力**
  1. **集群认证与加密（mTLS 或有轮换机制的节点凭证）**：集群已支持跨节点服务调用，但当前 `cluster.auth` 被忽略。现在没有，是因为当前阶段实现了静态 peer、BASP 通信和路由语义，认证被明确留在后续安全工作；在多节点进入生产前应优先补上。
  2. **持久化队列与任务调度**：让可靠/延迟队列和 scheduler 在重启、故障切换后保留任务，并明确至少一次投递、幂等与重复执行语义。现在没有，是因为 global 的队列和调度器仍以进程内实现为主，Redis 后端被列为 Phase 2+（`docs/runtime-global.md:1-5、698-700、1119-1121`）。
  3. **受控的 blue-green 发布编排**：提供新实例启动、状态迁移、原子切名、旧实例 drain、失败回滚的完整操作路径。现在只有 `shield.claim` 这个原子接管原语；迁移和 drain 仍由业务脚本自行编排（`docs/runtime-lua-vm.md:67-118`）。原因是任意业务状态迁移无法通用化，且 coroutine/closure 不能透明迁移；应做显式编排而非原地热补丁。

- **文档与实现可能不一致**
  - `docs/architecture-review.md:17-24、62-70、103-114、173-180` 的风险汇总仍称默认帧大小无限、无 TCP_NODELAY、DB 全同步、sandbox 开关无效；其中多项已在 `todo.md` 后续记录为修复，DB 异步也已有 M1–M4 落地说明。建议更新摘要和风险等级，避免旧结论继续被当作现状。
  - `docs/runtime-security.md:128-134` 把 `shield.rate_limiter()` 标成 Redis 存储；`docs/runtime-global.md:760-765` 则说明当前是进程内、Redis 同步尚未实现。以后者作为当前实现口径，前者需更正。
  - `docs/runtime-cluster.md:134-142` 称 Phase 1 仅静态 peer；同一文档 `176-195` 又称 Redis 服务发现已实现。`327-335` 的实现范围也仍是静态 peer/cache 查询口径，与前文已落地的跨节点投递描述不一致。
  - `README.md:212-223` 把当前能力描述为 smoke-test 级，并称 session 状态未以 userdata 暴露；但 `docs/lua-api.md:986-1013` 已定义 `ClientContext`/`ClientRef` userdata 与 bind API，且仓库有真实 TCP 闭环测试 `tests/acceptance/test_client_rpc_e2e.cpp`。README 状态段明显滞后。
