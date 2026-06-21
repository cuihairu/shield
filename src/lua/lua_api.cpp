// [SHIELD_LUA] Lua API registration
#include "shield/lua/lua_api.hpp"

#include "shield/config/config.hpp"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#endif
#include "shield/data/data.hpp"
#include "shield/plugin/plugin_host.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/http_client.hpp"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <vector>

namespace shield::lua {

// Resource limits per service.
static constexpr size_t kTimerLimit = 10000;
static constexpr size_t kForkLimit = 1000;

nlohmann::json lua_to_json(const sol::object& value);
sol::variadic_results call_with_timeout(sol::this_state state,
                                        LuaServiceManager* manager,
                                        LuaRuntime* runtime,
                                        int timeout_ms,
                                        const std::string& target,
                                        const std::string& method,
                                        const nlohmann::json& args);

sol::table make_error(sol::this_state state,
                      std::string code,
                      std::string message,
                      bool retryable = false,
                      sol::object detail = sol::nil) {
    sol::state_view lua(state);
    sol::table err = lua.create_table();
    err["code"] = std::move(code);
    err["message"] = std::move(message);
    err["retryable"] = retryable;
    if (detail.valid() && detail != sol::nil) {
        err["detail"] = detail;
    }
    return err;
}

sol::object json_to_lua(sol::state_view lua, const nlohmann::json& value) {
    if (value.is_null()) {
        return sol::make_object(lua, sol::nil);
    }
    if (value.is_boolean()) {
        return sol::make_object(lua, value.get<bool>());
    }
    if (value.is_number_integer()) {
        return sol::make_object(lua, value.get<std::int64_t>());
    }
    if (value.is_number_unsigned()) {
        return sol::make_object(lua, value.get<std::uint64_t>());
    }
    if (value.is_number_float()) {
        return sol::make_object(lua, value.get<double>());
    }
    if (value.is_string()) {
        return sol::make_object(lua, value.get<std::string>());
    }
    if (value.is_array()) {
        sol::table table = lua.create_table();
        int index = 1;
        for (const auto& item : value) {
            table[index++] = json_to_lua(lua, item);
        }
        return sol::make_object(lua, table);
    }
    if (value.is_object()) {
        sol::table table = lua.create_table();
        for (const auto& [key, item] : value.items()) {
            table[key] = json_to_lua(lua, item);
        }
        return sol::make_object(lua, table);
    }
    return sol::make_object(lua, sol::nil);
}

nlohmann::json lua_table_to_json(const sol::table& table) {
    bool array_like = true;
    std::size_t max_index = 0;
    std::size_t entry_count = 0;

    for (const auto& [key, _] : table) {
        ++entry_count;
        sol::object key_obj = key;
        if (!key_obj.is<int>()) {
            array_like = false;
            break;
        }

        const int index = key_obj.as<int>();
        if (index <= 0) {
            array_like = false;
            break;
        }
        max_index = std::max(max_index, static_cast<std::size_t>(index));
    }

    if (array_like && max_index == entry_count) {
        nlohmann::json array = nlohmann::json::array();
        for (std::size_t i = 1; i <= max_index; ++i) {
            array.push_back(lua_to_json(table[static_cast<int>(i)]));
        }
        return array;
    }

    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, value] : table) {
        sol::object key_obj = key;
        std::string object_key;
        if (key_obj.is<std::string>()) {
            object_key = key_obj.as<std::string>();
        } else if (key_obj.is<int>()) {
            object_key = std::to_string(key_obj.as<int>());
        } else {
            continue;
        }
        object[object_key] = lua_to_json(value);
    }
    return object;
}

nlohmann::json lua_to_json(const sol::object& value) {
    if (!value.valid() || value == sol::nil) {
        return nullptr;
    }
    if (value.is<bool>()) {
        return value.as<bool>();
    }
    if (value.is<std::int64_t>()) {
        return value.as<std::int64_t>();
    }
    if (value.is<int>()) {
        return value.as<int>();
    }
    if (value.is<double>()) {
        return value.as<double>();
    }
    if (value.is<std::string>()) {
        return value.as<std::string>();
    }
    if (value.is<sol::table>()) {
        return lua_table_to_json(value.as<sol::table>());
    }
    return "<unsupported>";
}

nlohmann::json variadic_to_json_array(sol::variadic_args args) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& arg : args) {
        values.push_back(lua_to_json(arg));
    }
    return values;
}

std::vector<std::string> table_to_string_vector(const sol::table& table) {
    std::vector<std::string> params;
    const auto size = table.size();
    params.reserve(size);
    for (std::size_t i = 1; i <= size; ++i) {
        sol::object item = table[static_cast<int>(i)];
        if (item == sol::nil) {
            params.emplace_back("");
        } else if (item.is<std::string>()) {
            params.push_back(item.as<std::string>());
        } else if (item.is<int>()) {
            params.push_back(std::to_string(item.as<int>()));
        } else if (item.is<std::int64_t>()) {
            params.push_back(std::to_string(item.as<std::int64_t>()));
        } else if (item.is<double>()) {
            params.push_back(std::to_string(item.as<double>()));
        } else if (item.is<bool>()) {
            params.push_back(item.as<bool>() ? "true" : "false");
        } else {
            params.push_back(lua_to_json(item).dump());
        }
    }
    return params;
}

constexpr const char* kDbMapperRuntimeScript = R"lua(
do
  local db = shield.db

  local function err(code, message)
    return { code = code, message = message }
  end

  local function is_array(t)
    if type(t) ~= "table" then return false end
    local count = 0
    for k, _ in pairs(t) do
      if type(k) ~= "number" or k < 1 or k % 1 ~= 0 then
        return false
      end
      count = count + 1
    end
    return count == #t
  end

  local function table_keys(t)
    local keys = {}
    for k, _ in pairs(t or {}) do
      keys[#keys + 1] = k
    end
    table.sort(keys)
    return keys
  end

  local function push_value(values, value)
    if value == nil then value = "" end
    values[#values + 1] = value
  end

  local function identifier(name)
    if type(name) ~= "string" or not name:match("^[A-Za-z_][A-Za-z0-9_]*$") then
      error("invalid SQL identifier: " .. tostring(name))
    end
    return name
  end

  local function resolve_path(params, path)
    local current = params
    for part in tostring(path):gmatch("[^.]+") do
      if type(current) ~= "table" then
        return nil
      end
      current = current[part]
    end
    return current
  end

  local function compile_sql(sql, params)
    params = params or {}
    local values = {}
    local compiled = tostring(sql):gsub("#%{([%w_%.]+)%}", function(path)
      push_value(values, resolve_path(params, path))
      return "?"
    end)
    if compiled:find("%$%{", 1, false) then
      return nil, err("mapper_unsafe_sql", "raw SQL substitution is not supported")
    end
    if compiled:find(";", 1, true) then
      return nil, err("mapper_unsafe_sql", "mapper statement must contain one SQL statement")
    end
    return compiled, values
  end

  local function normalize_statement(name, statement)
    if type(statement) == "string" then
      return { type = "select", sql = statement }
    end
    if type(statement) ~= "table" then
      error("mapper statement " .. tostring(name) .. " must be table or string")
    end
    local normalized = {}
    for k, v in pairs(statement) do
      normalized[k] = v
    end
    normalized.type = normalized.type or normalized.kind or "select"
    if type(normalized.sql) ~= "string" then
      error("mapper statement " .. tostring(name) .. " requires sql")
    end
    return normalized
  end

  local function execute_statement(statement, params, tx)
    local sql, values_or_err = compile_sql(statement.sql, params)
    if not sql then return false, values_or_err end

    local executor = tx or db
    local kind = statement.type or "select"
    if kind == "select" then
      if statement.one or statement.result == "one" then
        return executor.query_one(sql, values_or_err)
      end
      return executor.query(sql, values_or_err)
    elseif kind == "insert" or kind == "update" or kind == "delete" or kind == "execute" then
      return executor.execute(sql, values_or_err)
    end
    return false, err("mapper_invalid_statement", "unsupported mapper statement type: " .. tostring(kind))
  end

  local function mapper_call(statement, first, second)
    local tx = nil
    local params = first
    if type(first) == "table" and type(first.query) == "function" and
       type(first.execute) == "function" then
      tx = first
      params = second
    end

    if statement.transaction == "required" and not tx then
      return db.transaction(function(inner_tx)
        return execute_statement(statement, params, inner_tx)
      end)
    end

    return execute_statement(statement, params, tx)
  end

  local function build_mapper(definition)
    if type(definition) ~= "table" then
      error("mapper definition must be table")
    end
    local mapper = {}
    for name, statement in pairs(definition) do
      if name ~= "name" and name ~= "namespace" then
        local normalized = normalize_statement(name, statement)
        mapper[name] = function(self_or_first, first, second)
          if self_or_first == mapper then
            return mapper_call(normalized, first, second)
          end
          return mapper_call(normalized, self_or_first, first)
        end
      end
    end
    return mapper
  end

  function db.mapper(definition)
    return build_mapper(definition)
  end

  function db.register_mapper(name, definition)
    identifier(name)
    local mapper = build_mapper(definition)
    db[name] = mapper
    return mapper
  end

  local function build_where_by_pk(def, row, values)
      local clauses = {}
      for _, field in ipairs(def.primary_key) do
        local column = identifier(def.columns[field] or field)
        clauses[#clauses + 1] = column .. " = ?"
        push_value(values, row[field])
      end
    return table.concat(clauses, " AND ")
  end

  function db.entity(def)
    if type(def) ~= "table" then
      error("entity definition must be table")
    end
    local table_name = identifier(def.table or def.name)
    local fields = def.fields
    if type(fields) ~= "table" or #fields == 0 then
      error("entity requires ordered fields")
    end

    local columns = {}
      if is_array(fields) then
        for _, field in ipairs(fields) do
          columns[field] = field
        end
      else
      for field, column in pairs(fields) do
        columns[field] = column
      end
    end

    local primary_key = def.primary_key or def.id or fields[1]
    if type(primary_key) ~= "table" then
      primary_key = { primary_key }
    end

    local entity = {
      table = table_name,
      fields = fields,
      columns = columns,
      primary_key = primary_key
    }

    function entity:ordered_fields()
      if is_array(self.fields) then
        return self.fields
      end
      return table_keys(self.columns)
    end

    function entity:insert(row, tx)
      local keys = {}
      for _, field in ipairs(self:ordered_fields()) do
        if row[field] ~= nil then keys[#keys + 1] = field end
      end
      if #keys == 0 then
        return false, err("entity_invalid_row", "insert requires at least one field")
      end

      local cols, marks, values = {}, {}, {}
      for _, field in ipairs(keys) do
        cols[#cols + 1] = identifier(self.columns[field] or field)
        marks[#marks + 1] = "?"
        push_value(values, row[field])
      end
      local sql = "INSERT INTO " .. self.table .. " (" .. table.concat(cols, ", ") ..
                  ") VALUES (" .. table.concat(marks, ", ") .. ")"
      return (tx or db).execute(sql, values)
    end

    function entity:update(row, tx)
      local set_parts, values = {}, {}
      local pk_lookup = {}
      for _, field in ipairs(self.primary_key) do pk_lookup[field] = true end
      for _, field in ipairs(self:ordered_fields()) do
        if not pk_lookup[field] and row[field] ~= nil then
          set_parts[#set_parts + 1] = identifier(self.columns[field] or field) .. " = ?"
          push_value(values, row[field])
        end
      end
      if #set_parts == 0 then
        return false, err("entity_invalid_row", "update requires non-primary-key fields")
      end
      local where = build_where_by_pk(self, row, values)
      local sql = "UPDATE " .. self.table .. " SET " .. table.concat(set_parts, ", ") ..
                  " WHERE " .. where
      return (tx or db).execute(sql, values)
    end

    function entity:delete(key, tx)
      local row = type(key) == "table" and key or { [self.primary_key[1]] = key }
      local values = {}
      local where = build_where_by_pk(self, row, values)
      local sql = "DELETE FROM " .. self.table .. " WHERE " .. where
      return (tx or db).execute(sql, values)
    end

    function entity:find(key, tx)
      local row = type(key) == "table" and key or { [self.primary_key[1]] = key }
      local values = {}
      local where = build_where_by_pk(self, row, values)
      local sql = "SELECT * FROM " .. self.table .. " WHERE " .. where
      return (tx or db).query_one(sql, values)
    end

    return entity
  end
end
)lua";

std::string db_error_code(const shield::data::QueryResult& result,
                          std::string_view fallback) {
    return result.error_code.empty() ? std::string(fallback) : result.error_code;
}

sol::table db_error(sol::this_state state,
                    const shield::data::QueryResult& result,
                    std::string_view fallback = "db_query_failed") {
    return make_error(state, db_error_code(result, fallback),
                      result.error_message);
}

sol::table db_row_to_lua(sol::state_view lua, const shield::data::Row& row) {
    sol::table row_table = lua.create_table();
    for (const auto& [column, value] : row) {
        row_table[column] = value;
    }
    return row_table;
}

sol::table db_rows_to_lua(sol::state_view lua,
                          const std::vector<shield::data::Row>& rows) {
    sol::table result = lua.create_table();
    int row_index = 1;
    for (const auto& row : rows) {
        result[row_index++] = db_row_to_lua(lua, row);
    }
    return result;
}

sol::object copy_lua_object(sol::state_view lua, const sol::object& object) {
    return json_to_lua(lua, lua_to_json(object));
}

// Helper to extract service ID from ServiceHandle or string
std::string extract_service_id(const sol::object& target) {
    if (target.is<ServiceHandle>()) {
        return target.as<ServiceHandle>().id();
    }
    if (target.is<std::string>()) {
        return target.as<std::string>();
    }
    return "";
}

void register_service_api(sol::table& shield, LuaServiceManager* manager) {
    shield.set_function("spawn",
        [manager](sol::this_state state,
                  std::string module,
                  sol::optional<sol::table> opts) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Lua service manager is not available"));
                return results;
            }

            nlohmann::json options = opts ? lua_table_to_json(*opts)
                                          : nlohmann::json::object();
            if (!options.is_object()) {
                options = nlohmann::json::object();
            }

            SpawnResult result = manager->spawn(module, options.dump());
            if (!result.success) {
                std::string code = "spawn_failed";
                if (result.error_message.find("timeout") != std::string::npos) {
                    code = "spawn_timeout";
                } else if (result.error_message.find("on_init failed") != std::string::npos) {
                    code = "init_failed";
                }
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, std::move(code),
                                             result.error_message));
                return results;
            }

            // Return ServiceHandle userdata instead of string
            ServiceHandle handle(result.service_id);
            results.push_back(sol::make_object(lua, handle));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    shield.set_function("exit",
        [manager](sol::optional<std::string> reason) {
            if (manager) {
                manager->request_current_exit(reason.value_or("normal"));
            }
        });

    shield.set_function("self",
        [manager](sol::this_state state) -> sol::object {
            sol::state_view lua(state);
            if (!manager) {
                return sol::make_object(lua, sol::nil);
            }
            const auto service_id = manager->current_service_id();
            if (service_id.empty()) {
                return sol::make_object(lua, sol::nil);
            }
            ServiceHandle handle(service_id);
            return sol::make_object(lua, handle);
        });

    shield.set_function("names",
        [manager](sol::this_state state) -> sol::table {
            sol::state_view lua(state);
            sol::table names = lua.create_table();
            if (!manager) {
                return names;
            }

            int index = 1;
            for (const auto& name : manager->list_services()) {
                names[index++] = name;
            }
            return names;
        });

    shield.set_function("query",
        [manager](sol::this_state state,
                  std::string name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Lua service manager is not available"));
                return results;
            }

            const auto service = manager->query_service(name);
            if (!service.empty()) {
                ServiceHandle handle(service);
                results.push_back(sol::make_object(lua, handle));
                results.push_back(sol::make_object(lua, sol::nil));
                return results;
            }

            results.push_back(sol::make_object(lua, sol::nil));
            results.push_back(make_error(state, "service_not_found",
                                         "service not found: " + name));
            return results;
        });

    shield.set_function("register",
        [manager](sol::this_state state, std::string name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Lua service manager is not available"));
                return results;
            }

            std::string error;
            if (!manager->register_name(name, &error)) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "register_failed", error));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    shield.set_function("unregister",
        [manager](sol::this_state state, std::string name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Lua service manager is not available"));
                return results;
            }

            std::string error;
            if (!manager->unregister_name(name, &error)) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "unregister_failed", error));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });
}

void register_message_api(sol::table& shield, LuaServiceManager* manager,
                          LuaRuntime* runtime) {
    shield.set_function("send",
        [manager](sol::this_state state,
                  sol::object target,
                  std::string method,
                  sol::variadic_args args) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Lua service manager is not available"));
                return results;
            }

            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "invalid_target",
                                             "target must be ServiceHandle or string"));
                return results;
            }

            std::string error;
            if (!manager->send(target_id, method, variadic_to_json_array(args),
                               &error)) {
                // Map error message to stable error code.
                std::string code = "service_not_found";
                bool retryable = false;
                if (error.find("mailbox full") != std::string::npos) {
                    code = "mailbox_full";
                    retryable = true;
                } else if (error.find("runtime is stopping") != std::string::npos) {
                    code = "runtime_stopping";
                } else if (error.find("message too large") != std::string::npos) {
                    code = "message_too_large";
                } else if (error.find("unsupported") != std::string::npos) {
                    code = "encode_failed";
                } else if (error.find("permission denied") != std::string::npos) {
                    code = "permission_denied";
                } else if (error.find("invalid method") != std::string::npos) {
                    code = "invalid_method";
                } else if (error.find("service dead") != std::string::npos) {
                    code = "service_dead";
                } else if (error.find("coroutine limit") != std::string::npos) {
                    code = "coroutine_limit";
                }
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, std::move(code), error,
                                             retryable));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    shield.set_function("_sync_call",
        [manager, runtime](sol::this_state state,
                   sol::object target,
                   std::string method,
                   sol::variadic_args args) -> sol::variadic_results {
            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                sol::state_view lua(state);
                sol::variadic_results results;
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "invalid_target",
                                             "target must be ServiceHandle or string"));
                return results;
            }
            return call_with_timeout(state, manager, runtime, 5000, target_id, method,
                                     variadic_to_json_array(args));
        });

    shield.set_function("_sync_call_timeout",
        [manager, runtime](sol::this_state state,
                   int timeout_ms,
                   sol::object target,
                   std::string method,
                   sol::variadic_args args) -> sol::variadic_results {
            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                sol::state_view lua(state);
                sol::variadic_results results;
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "invalid_target",
                                             "target must be ServiceHandle or string"));
                return results;
            }
            return call_with_timeout(state, manager, runtime, timeout_ms, target_id, method,
                                     variadic_to_json_array(args));
        });

    // Coroutine-aware call primitive. Suspends the caller's coroutine and
    // sends a call-request message to the target; the caller is resumed with
    // [ok, values...] when the callee completes (or on timeout). Returns the
    // session id (0 if the request could not be queued).
    shield.set_function("_coro_call",
        [manager](sol::this_state state,
                  sol::object target,
                  std::string method,
                  sol::table args,
                  int timeout_ms) -> uint64_t {
            if (!manager) {
                return 0;
            }
            const std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                return 0;
            }
            const std::string service_id = manager->query_service(target_id);
            if (service_id.empty()) {
                return 0;
            }
            lua_State* co = state;
            const uint64_t session = manager->suspend_for_call(co, timeout_ms);

            // Build and queue the call-request message.
            nlohmann::json json_args = nlohmann::json::array();
            for (std::size_t i = 1; i <= args.size(); ++i) {
                json_args.push_back(lua_to_json(args[static_cast<int>(i)]));
            }
            std::string send_error;
            // Send carries call_session so the callee's dispatch can route the
            // response back. call_reply_to is the caller's service id.
            if (!manager->send_call_request(service_id, method, json_args,
                                            session, &send_error)) {
                // Could not queue: cancel the pending wait with an error so
                // the caller resumes immediately instead of hanging.
                nlohmann::json err;
                err = send_error;
                manager->resume_caller(session, false, nlohmann::json::array({err}));
                return 0;
            }
            return session;
        });

    shield.set_function("_is_in_exit",
        [manager]() -> bool {
            return manager && manager->is_in_exit();
        });

    shield.set_function("sender",
        [manager]() -> sol::optional<std::string> {
            if (!manager) {
                return sol::nullopt;
            }
            // Returns nil in timer/fork context (no sender).
            // Returns nil outside any dispatch (module-level code).
            // The distinction between "no sender" and "context_expired" is
            // that in timer/fork context we ARE inside a dispatch scope but
            // the sender is empty; outside any scope the context is expired.
            const auto sender = manager->current_sender_id();
            if (sender.empty()) {
                return sol::nullopt;
            }
            return sender;
        });

    shield.set_function("trace", [manager]() -> sol::optional<std::string> {
        if (!manager) return sol::nullopt;
        const auto trace = manager->current_trace_id();
        if (trace.empty()) return sol::nullopt;
        return trace;
    });

    shield.set_function("deadline", [manager]() -> sol::optional<int64_t> {
        if (!manager) return sol::nullopt;
        const auto dl = manager->current_deadline_ms();
        if (dl <= 0) return sol::nullopt;
        return dl;
    });

    // Override shield.call / shield.call_timeout with Lua wrappers that pick
    // the coroutine-aware path when running inside a handler coroutine (so the
    // caller yields instead of blocking the worker) and fall back to the
    // synchronous path on the main thread.
    sol::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    lua.safe_script(
        "shield.call = function(target, method, ...)\n"
        "  if shield._is_in_exit() then\n"
        "    return false, {code='api_not_allowed_in_exit', message='shield.call is not allowed in on_exit'}\n"
        "  end\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then return shield._sync_call(target, method, ...) end\n"
        "  local session = shield._coro_call(target, method, table.pack(...), 5000)\n"
        "  if session == 0 then return shield._sync_call(target, method, ...) end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then return false, r[2] end\n"
        "  return true, table.unpack(r, 2, r.n)\n"
        "end\n"
        "shield.call_timeout = function(timeout_ms, target, method, ...)\n"
        "  if shield._is_in_exit() then\n"
        "    return false, {code='api_not_allowed_in_exit', message='shield.call_timeout is not allowed in on_exit'}\n"
        "  end\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then return shield._sync_call_timeout(timeout_ms, target, method, ...) end\n"
        "  local session = shield._coro_call(target, method, table.pack(...), timeout_ms)\n"
        "  if session == 0 then return shield._sync_call_timeout(timeout_ms, target, method, ...) end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then return false, r[2] end\n"
        "  return true, table.unpack(r, 2, r.n)\n"
        "end",
        [](lua_State*, sol::protected_function_result pfr) -> sol::protected_function_result {
            return pfr;
        });
}

sol::variadic_results call_with_timeout(sol::this_state state,
                                        LuaServiceManager* manager,
                                        LuaRuntime* runtime,
                                        int timeout_ms,
                                        const std::string& target,
                                        const std::string& method,
                                        const nlohmann::json& args) {
    sol::state_view lua(state);
    sol::variadic_results results;

    // Helper to map call error message to stable error code.
    auto call_error_code = [](const std::string& msg) -> std::string {
        if (msg.find("service not found") != std::string::npos) return "service_not_found";
        if (msg.find("service dead") != std::string::npos) return "service_dead";
        if (msg.find("method not found") != std::string::npos) return "method_not_found";
        if (msg.find("runtime is stopping") != std::string::npos) return "runtime_stopping";
        if (msg.find("invalid method") != std::string::npos) return "invalid_method";
        if (msg.find("coroutine limit") != std::string::npos) return "coroutine_limit";
        return "handler_error";
    };

    if (!manager) {
        results.push_back(sol::make_object(lua, false));
        results.push_back(make_error(state, "runtime_unavailable",
                                     "Lua service manager is not available"));
        return results;
    }

    CallResult result = manager->call(target, method, args, timeout_ms);
    if (!result.success) {
        results.push_back(sol::make_object(lua, false));
        results.push_back(make_error(state, call_error_code(result.error_message),
                                     result.error_message));
        return results;
    }

    results.push_back(sol::make_object(lua, true));
    for (const auto& value : result.values) {
        results.push_back(json_to_lua(lua, value));
    }
    return results;
}

void register_timer_api(sol::table& shield, LuaServiceManager* manager,
                         LuaRuntime* runtime) {
    shield.set_function("now", []() -> int64_t {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    });

    shield.set_function("timer_once",
        [manager, runtime](int delay_ms, sol::function callback) -> sol::variadic_results {
            sol::variadic_results results;
            sol::state_view lua(callback.lua_state());

            if (!runtime) {
                // Fallback to thread-based implementation
                static std::atomic<uint64_t> timer_id{1};
                const uint64_t id = timer_id.fetch_add(1);
                std::thread([delay_ms, callback]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                    callback();
                }).detach();
                results.push_back(sol::make_object(lua, id));
                return results;
            }

            // Get current service ID
            std::string service_id;
            if (manager) {
                service_id = manager->current_service_id();
            }

            // Check timer limit.
            if (runtime->timer_manager().active_count() >= kTimerLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(callback.lua_state());
                results.push_back(make_error(ts, "timer_limit",
                                             "timer limit reached"));
                return results;
            }

            const uint64_t id = runtime->timer_manager().schedule_once(
                delay_ms, callback, service_id);
            results.push_back(sol::make_object(lua, id));
            return results;
        });

    shield.set_function("timer",
        [manager, runtime](int interval_ms, sol::function callback) -> sol::variadic_results {
            sol::variadic_results results;
            sol::state_view lua(callback.lua_state());

            if (!runtime) {
                // Fallback to thread-based implementation
                static std::atomic<uint64_t> timer_id{1};
                const uint64_t id = timer_id.fetch_add(1);
                std::thread([interval_ms, callback]() {
                    while (true) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(interval_ms));
                        callback();
                    }
                }).detach();
                results.push_back(sol::make_object(lua, id));
                return results;
            }

            // Get current service ID
            std::string service_id;
            if (manager) {
                service_id = manager->current_service_id();
            }

            // Check timer limit.
            if (runtime->timer_manager().active_count() >= kTimerLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(callback.lua_state());
                results.push_back(make_error(ts, "timer_limit",
                                             "timer limit reached"));
                return results;
            }

            const uint64_t id = runtime->timer_manager().schedule_fixed_delay(
                interval_ms, callback, service_id);
            results.push_back(sol::make_object(lua, id));
            return results;
        });

    shield.set_function("cancel_timer",
        [runtime](sol::this_state state, uint64_t id) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            if (!runtime) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "runtime_unavailable",
                                             "Timer manager is not available"));
                return results;
            }

            const bool cancelled = runtime->timer_manager().cancel(id);
            results.push_back(sol::make_object(lua, cancelled));
            if (!cancelled) {
                results.push_back(make_error(state, "timer_not_found",
                                             "Timer not found or already completed"));
            } else {
                results.push_back(sol::make_object(lua, sol::nil));
            }
            return results;
        });

    // shield.sleep is implemented as a Lua wrapper that schedules a native
    // timer to resume the current coroutine and then yields. The C primitive
    // _resume_after anchors the running coroutine against GC and arms the
    // timer; coroutine.yield suspends until the timer fires and resumes us.
    shield.set_function("_resume_after",
        [manager, runtime](sol::this_state state, int delay_ms) {
            if (!runtime || delay_ms <= 0) {
                return;
            }
            lua_State* co = state;  // current coroutine thread
            // Anchor the thread so it survives GC while suspended.
            lua_pushthread(co);
            const int ref = luaL_ref(co, LUA_REGISTRYINDEX);
            std::string service_id;
            if (manager) {
                service_id = manager->current_service_id();
            }
            auto& timer_mgr = runtime->timer_manager();
            timer_mgr.schedule_once_fn(delay_ms,
                [co, ref, manager]() {
                    int nres = 0;
                    const int status = lua_resume(co, nullptr, 0, &nres);
                    if (status == LUA_YIELD) {
                        // Yielded again (e.g. another sleep): another
                        // _resume_after has re-anchored it. Keep this ref so
                        // the next resume stays covered until final completion.
                        return;
                    }
                    // If this coroutine was servicing a call request that
                    // yielded (e.g. the callee slept), route the response now
                    // that it has completed. No-op for plain handlers.
                    if (status == LUA_OK && manager) {
                        nlohmann::json returns = nlohmann::json::array();
                        for (int i = 0; i < nres; ++i) {
                            sol::stack_object so(sol::state_view(co), i + 1);
                            returns.push_back(lua_to_json(so));
                        }
                        manager->on_handler_completed(co, returns);
                    }
                    // LUA_OK (completed) or an error: release the anchor.
                    luaL_unref(co, LUA_REGISTRYINDEX, ref);
                },
                service_id);
        });

    // Blocking fallback used when shield.sleep is invoked outside any
    // coroutine (e.g. from the synchronous manager.call path). The
    // coroutine-aware branch below handles the yieldable case.
    shield.set_function("_block_sleep", [](int delay_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    });

    sol::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    // Define shield.sleep in Lua so it can use coroutine.yield natively. When
    // not running inside a coroutine (synchronous dispatch) fall back to a
    // blocking sleep so the call still completes.
    lua.safe_script(
        "shield.sleep = function(ms)\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    shield._block_sleep(ms)\n"
        "  else\n"
        "    shield._resume_after(ms); coroutine.yield()\n"
        "  end\n"
        "end",
        [](lua_State*, sol::protected_function_result pfr) -> sol::protected_function_result {
            return pfr;
        });
}

void register_task_api(sol::table& shield, LuaServiceManager* manager,
                        LuaRuntime* runtime) {
    (void)runtime;

    shield.set_function("fork",
        [manager](sol::this_state state, sol::function fn) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            if (!manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                return results;
            }
            const std::string service_id = manager->current_service_id();

            // Check fork limit.
            if (manager->pending_task_count(service_id) >= kForkLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(fn.lua_state());
                results.push_back(make_error(ts, "fork_limit",
                                             "fork limit reached"));
                return results;
            }
            // Capture the Lua function with its owning state_view. Execution
            // happens on the worker thread; since the worker is the only thread
            // touching Lua VMs once running, this is race-free.
            uint64_t task_id = manager->enqueue_forked_task(
                service_id,
                [fn]() {
                    try {
                        fn();
                    } catch (const std::exception& e) {
                        auto& log = shield::log::get_logger("lua");
                        SHIELD_LOG_ERROR(log, std::string("task error: ") + e.what());
                    }
                },
                fn);  // raw_fn for coroutine wrapping
            results.push_back(sol::make_object(lua, task_id));
            return results;
        });
}

void register_config_api(sol::table& shield) {
    shield.set_function("config",
        [](sol::this_state state,
           std::string key,
           sol::optional<sol::object> default_value) -> sol::object {
            sol::state_view lua(state);
            auto& config = shield::config::global_config();
            if (!config.has(key)) {
                if (default_value) {
                    return *default_value;
                }
                return sol::make_object(lua, sol::nil);
            }

            const auto value = config.get_string(key, "");
            if (value == "true") {
                return sol::make_object(lua, true);
            }
            if (value == "false") {
                return sol::make_object(lua, false);
            }

            // Require the whole string to be consumed so that values like
            // "3.14" don't get truncated to integer 3 and "12abc" doesn't get
            // partially parsed as 12 by std::stoll / std::stod.
            try {
                size_t pos = 0;
                const long long parsed = std::stoll(value, &pos);
                if (pos == value.size()) {
                    return sol::make_object(lua, parsed);
                }
            } catch (const std::exception&) {}

            try {
                size_t pos = 0;
                const double parsed = std::stod(value, &pos);
                if (pos == value.size()) {
                    return sol::make_object(lua, parsed);
                }
            } catch (const std::exception&) {}

            return sol::make_object(lua, value);
        });
}

void register_log_api(sol::table& shield, LuaServiceManager* manager) {
    auto& log = shield::log::get_logger("lua");
    sol::state_view lua(shield.lua_state());
    auto log_table = lua.create_table();

    // Helper: build log message with service context prefix.
    auto build_msg = [manager](sol::object value) -> std::string {
        std::string msg = lua_to_json(value).dump();
        if (!manager) return msg;
        const std::string sid = manager->current_service_id();
        if (sid.empty()) return msg;
        return "[" + sid + "] " + msg;
    };

    log_table.set_function("debug", [&log, build_msg](sol::object value) {
        SHIELD_LOG_DEBUG(log, build_msg(value));
    });
    log_table.set_function("info", [&log, build_msg](sol::object value) {
        SHIELD_LOG_INFO(log, build_msg(value));
    });
    log_table.set_function("warn", [&log, build_msg](sol::object value) {
        SHIELD_LOG_WARNING(log, build_msg(value));
    });
    log_table.set_function("error", [&log, build_msg](sol::object value) {
        SHIELD_LOG_ERROR(log, build_msg(value));
    });

    shield["log"] = log_table;
}

void register_data_api(sol::table& shield, LuaServiceManager* manager) {
    sol::state_view lua(shield.lua_state());

    auto db_table = lua.create_table();
    db_table.set_function("query",
        [](sol::this_state state,
           std::string sql,
           sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& db = shield::data::database();
            if (!db.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "database is not initialized"));
                return results;
            }

            auto query_result = db.query(sql, params ? table_to_string_vector(*params)
                                                     : std::vector<std::string>{});
            results.push_back(sol::make_object(lua, query_result.success));
            if (!query_result.success) {
                results.push_back(make_error(
                    state,
                    query_result.error_code.empty() ? "db_query_failed"
                                                    : query_result.error_code,
                    query_result.error_message));
                return results;
            }

            results.push_back(db_rows_to_lua(lua, query_result.rows));
            return results;
        });

    db_table.set_function("query_one",
        [](sol::this_state state,
           std::string sql,
           sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& db = shield::data::database();
            if (!db.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "database is not initialized"));
                return results;
            }

            auto query_result =
                db.query_one(sql, params ? table_to_string_vector(*params)
                                         : std::vector<std::string>{});
            results.push_back(sol::make_object(lua, query_result.success));
            if (!query_result.success) {
                results.push_back(make_error(
                    state,
                    query_result.error_code.empty() ? "db_query_failed"
                                                    : query_result.error_code,
                    query_result.error_message));
                return results;
            }
            if (query_result.rows.empty()) {
                results.push_back(sol::make_object(lua, sol::nil));
                return results;
            }

            results.push_back(db_row_to_lua(lua, query_result.rows.front()));
            return results;
        });

    db_table.set_function("execute",
        [](sol::this_state state,
           std::string sql,
           sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& db = shield::data::database();
            if (!db.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "database is not initialized"));
                return results;
            }

            auto exec_result =
                db.execute(sql, params ? table_to_string_vector(*params)
                                       : std::vector<std::string>{});
            results.push_back(sol::make_object(lua, exec_result.success));
            if (!exec_result.success) {
                results.push_back(make_error(
                    state,
                    exec_result.error_code.empty() ? "db_query_failed"
                                                   : exec_result.error_code,
                    exec_result.error_message));
                return results;
            }
            sol::table result_table = lua.create_table_with(
                "affected", exec_result.affected_rows,
                "last_insert_id", exec_result.last_insert_id);
            results.push_back(result_table);
            return results;
        });

    db_table.set_function("transaction",
        [](sol::this_state state,
           sol::protected_function callback) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& db = shield::data::database();
            if (!db.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "database is not initialized"));
                return results;
            }

            auto conn = db.acquire();
            if (!conn) {
                const auto code = db.last_error_code().empty()
                                      ? "pool_exhausted"
                                      : db.last_error_code();
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, code,
                                             "No database connection available"));
                return results;
            }

            auto begin = conn->begin_transaction();
            if (!begin.success) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(db_error(state, begin, "transaction_aborted"));
                return results;
            }

            auto active = std::make_shared<bool>(true);
            auto tx = lua.create_table();

            tx.set_function("query",
                [conn, active](sol::this_state tx_state,
                               std::string sql,
                               sol::optional<sol::table> params)
                    -> sol::variadic_results {
                    sol::state_view tx_lua(tx_state);
                    sol::variadic_results tx_results;
                    if (!*active) {
                        tx_results.push_back(sol::make_object(tx_lua, false));
                        tx_results.push_back(make_error(
                            tx_state, "transaction_closed",
                            "transaction handle is closed"));
                        return tx_results;
                    }
                    auto query_result = conn->query(
                        sql, params ? table_to_string_vector(*params)
                                    : std::vector<std::string>{});
                    tx_results.push_back(sol::make_object(tx_lua,
                                                          query_result.success));
                    if (!query_result.success) {
                        tx_results.push_back(db_error(tx_state, query_result));
                        return tx_results;
                    }
                    tx_results.push_back(db_rows_to_lua(tx_lua, query_result.rows));
                    return tx_results;
                });

            tx.set_function("query_one",
                [conn, active](sol::this_state tx_state,
                               std::string sql,
                               sol::optional<sol::table> params)
                    -> sol::variadic_results {
                    sol::state_view tx_lua(tx_state);
                    sol::variadic_results tx_results;
                    if (!*active) {
                        tx_results.push_back(sol::make_object(tx_lua, false));
                        tx_results.push_back(make_error(
                            tx_state, "transaction_closed",
                            "transaction handle is closed"));
                        return tx_results;
                    }
                    auto query_result = conn->query_one(
                        sql, params ? table_to_string_vector(*params)
                                    : std::vector<std::string>{});
                    tx_results.push_back(sol::make_object(tx_lua,
                                                          query_result.success));
                    if (!query_result.success) {
                        tx_results.push_back(db_error(tx_state, query_result));
                        return tx_results;
                    }
                    if (query_result.rows.empty()) {
                        tx_results.push_back(sol::make_object(tx_lua, sol::nil));
                        return tx_results;
                    }
                    tx_results.push_back(db_row_to_lua(tx_lua,
                                                       query_result.rows.front()));
                    return tx_results;
                });

            tx.set_function("execute",
                [conn, active](sol::this_state tx_state,
                               std::string sql,
                               sol::optional<sol::table> params)
                    -> sol::variadic_results {
                    sol::state_view tx_lua(tx_state);
                    sol::variadic_results tx_results;
                    if (!*active) {
                        tx_results.push_back(sol::make_object(tx_lua, false));
                        tx_results.push_back(make_error(
                            tx_state, "transaction_closed",
                            "transaction handle is closed"));
                        return tx_results;
                    }
                    auto exec_result = conn->execute(
                        sql, params ? table_to_string_vector(*params)
                                    : std::vector<std::string>{});
                    tx_results.push_back(sol::make_object(tx_lua,
                                                          exec_result.success));
                    if (!exec_result.success) {
                        tx_results.push_back(db_error(tx_state, exec_result));
                        return tx_results;
                    }
                    sol::table result_table = tx_lua.create_table_with(
                        "affected", exec_result.affected_rows,
                        "last_insert_id", exec_result.last_insert_id);
                    tx_results.push_back(result_table);
                    return tx_results;
                });

            sol::protected_function_result callback_result = callback(tx);
            *active = false;
            if (!callback_result.valid()) {
                auto rollback = conn->rollback_transaction();
                (void)rollback;
                sol::error err = callback_result;
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "transaction_aborted",
                                             err.what()));
                return results;
            }

            const int return_count = callback_result.return_count();
            sol::object ok_object =
                return_count > 0 ? callback_result.get<sol::object>(0)
                                 : sol::make_object(lua, sol::nil);
            if (ok_object.is<bool>() && !ok_object.as<bool>()) {
                auto rollback = conn->rollback_transaction();
                if (!rollback.success) {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(db_error(state, rollback,
                                               "transaction_aborted"));
                    return results;
                }
                results.push_back(sol::make_object(lua, false));
                if (callback_result.return_count() > 1) {
                    auto reason = callback_result.get<sol::object>(1);
                    results.push_back(copy_lua_object(lua, reason));
                } else {
                    results.push_back(make_error(state, "transaction_aborted",
                                                 "transaction callback returned false"));
                }
                return results;
            }

            auto commit = conn->commit_transaction();
            if (!commit.success) {
                auto rollback = conn->rollback_transaction();
                (void)rollback;
                results.push_back(sol::make_object(lua, false));
                results.push_back(db_error(state, commit, "transaction_aborted"));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            const int first_payload_index =
                ok_object.is<bool>() && ok_object.as<bool>() ? 1 : 0;
            if (return_count == 0 || first_payload_index >= return_count) {
                results.push_back(sol::make_object(lua, sol::nil));
                return results;
            }
            for (int i = first_payload_index; i < return_count; ++i) {
                results.push_back(copy_lua_object(
                    lua, callback_result.get<sol::object>(i)));
            }
            return results;
        });
    shield["db"] = db_table;
    lua["shield"] = shield;
    lua.safe_script(
        kDbMapperRuntimeScript,
        [](lua_State*, sol::protected_function_result pfr)
            -> sol::protected_function_result {
            return pfr;
        });

    auto redis_table = lua.create_table();
    redis_table.set_function("get",
        [](sol::this_state state, std::string key) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            auto [ok, value] = redis.get(key);
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? sol::make_object(lua, value)
                                 : sol::make_object(lua, sol::nil));
            return results;
        });

    redis_table.set_function("set",
        [](sol::this_state state,
           std::string key,
           std::string value,
           sol::optional<int> ttl) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            const bool ok = redis.set(key, value, ttl.value_or(0));
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? sol::make_object(lua, sol::nil)
                                 : make_error(state, redis.last_error_code(),
                                              "redis set failed"));
            return results;
        });

    redis_table.set_function("publish",
        [](sol::this_state state,
           std::string channel,
           sol::object message) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            int receivers = redis.publish(channel, lua_to_json(message).dump());
            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, receivers));
            return results;
        });

    redis_table.set_function("del",
        [](sol::this_state state, std::string key) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            const int removed = redis.del(key);
            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, removed));
            return results;
        });

    redis_table.set_function("exists",
        [](sol::this_state state, std::string key) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, redis.exists(key)));
            return results;
        });

    redis_table.set_function("subscribe",
        [manager](sol::this_state state, std::string channel,
           sol::function callback) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            auto& redis = shield::data::redis();
            if (!redis.is_initialized()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "module_unavailable",
                                             "redis is not initialized"));
                return results;
            }

            // Wrap the Lua callback in a C++ callback.
            // The callback is stored by the Redis pool; when a message
            // arrives, it will be invoked on the Redis thread. Since we
            // can't safely call Lua from a non-worker thread, we store
            // the callback for now and return success. Full pub/sub
            // integration requires a message queue back to the worker.
            auto cpp_callback =
                [callback](std::string_view ch, std::string_view msg) mutable {
                    if (callback.valid()) {
                        sol::protected_function_result r =
                            callback(std::string(ch), std::string(msg));
                        (void)r;
                    }
                };

            const bool ok = redis.subscribe(channel, std::move(cpp_callback));
            if (ok && manager) {
                manager->register_redis_subscription(channel);
            }
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? sol::make_object(lua, sol::nil)
                                 : make_error(state, "redis_command_failed",
                                              "subscribe failed"));
            return results;
        });

    shield["redis"] = redis_table;
}

void register_shield_api(LuaRuntime& runtime) {
    (void)runtime;
}

namespace api {

using LuaRuntime = shield::lua::LuaRuntime;
using LuaServiceManager = shield::lua::LuaServiceManager;

void register_service_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_message_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_timer_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_timer_api(sol::table& shield, LuaServiceManager* manager,
                         LuaRuntime* runtime) {
    (void)shield;
    (void)manager;
    (void)runtime;
}

void register_task_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_config_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_log_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_data_api(LuaRuntime& runtime) {
    (void)runtime;
}

void register_gateway_api(LuaRuntime& runtime) {
    (void)runtime;
}

}  // namespace api

// SessionHandle: a Lua userdata representing a network session.
// This is the base registration; actual session objects are created by
// shield_net and passed to gateway handlers.
struct SessionHandle {
    std::string id;
    std::string remote_address;
    bool is_closed = false;
    std::vector<std::string> send_queue;
    size_t max_queue_size = 10000;

    SessionHandle() = default;
    SessionHandle(std::string sid, std::string addr)
        : id(std::move(sid)), remote_address(std::move(addr)) {}
};

static void register_session_handle(sol::state& lua) {
    lua.new_usertype<SessionHandle>("SessionHandle",
        sol::no_constructor,
        "id", [](const SessionHandle& s) { return s.id; },
        "remote_addr", [](const SessionHandle& s) { return s.remote_address; },
        "send", [](SessionHandle& s, sol::object payload) -> sol::variadic_results {
            sol::variadic_results results;
            sol::state_view sv(payload.lua_state());
            if (s.is_closed) {
                results.push_back(sol::make_object(sv, false));
                sol::table err = sv.create_table();
                err["code"] = "session_closed";
                err["message"] = "session is closed";
                results.push_back(sol::make_object(sv, err));
                return results;
            }
            if (s.send_queue.size() >= s.max_queue_size) {
                results.push_back(sol::make_object(sv, false));
                sol::table err = sv.create_table();
                err["code"] = "session_send_queue_full";
                err["message"] = "send queue full";
                results.push_back(sol::make_object(sv, err));
                return results;
            }
            if (payload.is<std::string>()) {
                s.send_queue.push_back(payload.as<std::string>());
            }
            results.push_back(sol::make_object(sv, true));
            return results;
        },
        "close", [](SessionHandle& s, sol::optional<std::string> reason) {
            s.is_closed = true;
        }
    );
}

#ifdef SHIELD_ENABLE_CLUSTER
void register_cluster_api(sol::table& shield, LuaServiceManager* manager) {
    sol::state_view lua(shield.lua_state());
    auto cluster = lua.create_table();

    // shield.cluster.query(node_id, service_name) -> service_id or nil, error
    cluster.set_function("query",
        [](sol::this_state state, std::string node_id,
                  std::string service_name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            auto* cluster_manager = shield::cluster::global_cluster_manager();
            if (!cluster_manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, "module_unavailable",
                                             "shield_cluster is not enabled"));
                return results;
            }

            const auto reachable = cluster_manager->check_node_reachable(node_id);
            if (!reachable.empty()) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, reachable,
                                             "cluster node is not reachable: " +
                                                 node_id));
                return results;
            }

            auto service_id = cluster_manager->query_remote(node_id, service_name);
            if (service_id.empty()) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, "service_not_found",
                                             "remote service not found: " +
                                                 service_name));
                return results;
            }

            results.push_back(sol::make_object(lua, service_id));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    // shield.cluster.nodes() -> table of node info
    cluster.set_function("nodes",
        [](sol::this_state state) -> sol::table {
            sol::state_view lua(state);
            sol::table nodes = lua.create_table();
            auto* cluster_manager = shield::cluster::global_cluster_manager();
            if (!cluster_manager) {
                return nodes;
            }
            int index = 1;
            for (const auto& node : cluster_manager->nodes()) {
                sol::table entry = lua.create_table();
                entry["node_id"] = node.node_id;
                entry["address"] = node.address;
                entry["state"] = shield::cluster::node_state_name(node.state);
                entry["last_heartbeat_ms"] = node.last_heartbeat_ms;
                entry["connected_at_ms"] = node.connected_at_ms;
                entry["epoch"] = node.epoch;
                nodes[index++] = entry;
            }
            return nodes;
        });

    // shield.cluster.node_id() -> this node's ID
    cluster.set_function("node_id",
        [](sol::this_state state) -> sol::optional<std::string> {
            auto* cluster_manager = shield::cluster::global_cluster_manager();
            if (!cluster_manager || cluster_manager->node_id().empty()) {
                return sol::nullopt;
            }
            return cluster_manager->node_id();
        });

    cluster.set_function("node_epoch", []() -> uint64_t {
        auto* cluster_manager = shield::cluster::global_cluster_manager();
        return cluster_manager ? cluster_manager->node_epoch() : 0;
    });

    shield["cluster"] = cluster;
}
#endif

void register_http_api(sol::table& shield) {
    sol::state_view lua(shield.lua_state());

    // =========================================================================
    // shield.http — HTTP 客户端（发请求）
    // =========================================================================
    auto http = lua.create_table();

    // Helper: convert HttpClientResponse to Lua table.
    // Auto-parses JSON body into `data` field when Content-Type is JSON.
    auto to_table = [](sol::state_view lua,
                       const shield::net::HttpClientResponse& res) -> sol::table {
        sol::table result = lua.create_table();
        result["status"] = res.status_code;
        result["body"] = res.body;
        result["ok"] = res.ok();
        result["error"] = res.error;
        sol::table headers = lua.create_table();
        for (const auto& [k, v] : res.headers) {
            headers[k] = v;
        }
        result["headers"] = headers;

        // Auto-parse JSON response body into `data` field.
        if (!res.body.empty()) {
            auto ct_it = res.headers.find("Content-Type");
            if (ct_it == res.headers.end()) ct_it = res.headers.find("content-type");
            bool is_json = false;
            if (ct_it != res.headers.end()) {
                is_json = ct_it->second.find("application/json") != std::string::npos;
            }
            // Also try parsing if body starts with { or [
            if (!is_json && !res.body.empty() &&
                (res.body[0] == '{' || res.body[0] == '[')) {
                is_json = true;
            }
            if (is_json) {
                try {
                    auto json = nlohmann::json::parse(res.body);
                    result["data"] = json_to_lua(lua, json);
                } catch (...) {
                    // Not valid JSON, leave data as nil.
                }
            }
        }
        return result;
    };

    // Helper: parse options table into HttpClientOptions.
    auto parse_opts = [](sol::table opts,
                         shield::net::HttpClientOptions& options) {
        if (opts["method"].valid()) {
            options.method = opts["method"].get<std::string>();
        }
        if (opts["body"].valid()) {
            options.body = opts["body"].get<std::string>();
        }
        if (opts["timeout"].valid()) {
            options.timeout_seconds = opts["timeout"].get<int>();
        }
        if (opts["headers"].valid()) {
            sol::table hdrs = opts["headers"];
            for (auto& [k, v] : hdrs) {
                if (k.is<std::string>() && v.is<std::string>()) {
                    options.headers[k.as<std::string>()] = v.as<std::string>();
                }
            }
        }
        if (opts["auth_bearer"].valid()) {
            options.auth_bearer = opts["auth_bearer"].get<std::string>();
            options.headers["Authorization"] = "Bearer " + options.auth_bearer;
        }
        if (opts["auth_basic"].valid()) {
            sol::table basic = opts["auth_basic"];
            if (basic["user"].valid()) {
                options.auth_basic_user = basic["user"].get<std::string>();
            }
            if (basic["password"].valid()) {
                options.auth_basic_password = basic["password"].get<std::string>();
            }
        }
        if (opts["proxy"].valid()) {
            options.proxy = opts["proxy"].get<std::string>();
        }
        if (opts["verify_ssl"].valid()) {
            options.verify_ssl = opts["verify_ssl"].get<bool>();
        }
        if (opts["ca_cert_path"].valid()) {
            options.ca_cert_path = opts["ca_cert_path"].get<std::string>();
        }
        if (opts["retry"].valid()) {
            options.retry_count = opts["retry"].get<int>();
        }
        if (opts["retry_delay"].valid()) {
            options.retry_delay_ms = opts["retry_delay"].get<int>();
        }
        if (opts["follow_redirects"].valid()) {
            options.follow_redirects = opts["follow_redirects"].get<bool>();
        }
        if (opts["max_redirects"].valid()) {
            options.max_redirects = opts["max_redirects"].get<int>();
        }
    };

    // shield.http.request(url, options) -> response_table
    // Full options: method, body, headers, timeout, auth_bearer,
    //   auth_basic={user,password}, proxy, verify_ssl, retry, retry_delay,
    //   follow_redirects, max_redirects
    http.set_function("request",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
           sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);

            shield::net::HttpClientOptions options;
            options.url = url;

            if (opts) {
                parse_opts(*opts, options);
            }

            auto res = shield::net::HttpClient::request(options);
            return to_table(lua, res);
        });

    // Convenience: shield.http.get(url [, options]) -> response_table
    http.set_function("get",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
           sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "GET";
            options.url = url;
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.post(url [, body] [, options]) -> response_table
    http.set_function("post",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
           sol::optional<std::string> body,
           sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.put(url [, body] [, options]) -> response_table
    http.set_function("put",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
           sol::optional<std::string> body,
           sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PUT";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.delete(url [, options]) -> response_table
    http.set_function("delete",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
           sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "DELETE";
            options.url = url;
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.patch(url [, body] [, options]) -> response_table
    http.set_function("patch",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
           sol::optional<std::string> body,
           sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PATCH";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // =========================================================================
    // JSON 便捷方法：自动将 Lua table 序列化为 JSON，自动设置 Content-Type
    // 响应自动解析 JSON 到 result.data 字段
    // =========================================================================

    // shield.http.json(url, data [, options]) -> response_table
    // 通用 JSON POST（最常用场景）
    http.set_function("json",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
           sol::object data, sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            // Serialize Lua table/object to JSON string.
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_post(url, data [, options]) -> response_table
    http.set_function("json_post",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
           sol::object data, sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_put(url, data [, options]) -> response_table
    http.set_function("json_put",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
           sol::object data, sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PUT";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_patch(url, data [, options]) -> response_table
    http.set_function("json_patch",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
           sol::object data, sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PATCH";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.upload(url, files [, fields] [, timeout]) -> response_table
    // files: array of {field_name, file_path, content_type}
    // fields: table of form field key-value pairs
    http.set_function("upload",
        [&to_table](sol::this_state state, std::string url,
           sol::table files, sol::optional<sol::table> fields,
           sol::optional<int> timeout) -> sol::table {
            sol::state_view lua(state);

            std::vector<shield::net::HttpFileField> file_list;
            for (auto& [i, entry] : files) {
                if (entry.is<sol::table>()) {
                    sol::table f = entry.as<sol::table>();
                    shield::net::HttpFileField field;
                    field.field_name = f.get_or<std::string>("field_name", "file");
                    field.file_path = f.get_or<std::string>("file_path", "");
                    field.content_type = f.get_or<std::string>("content_type", "");
                    file_list.push_back(std::move(field));
                }
            }

            std::unordered_map<std::string, std::string> field_map;
            if (fields) {
                for (auto& [k, v] : *fields) {
                    if (k.is<std::string>() && v.is<std::string>()) {
                        field_map[k.as<std::string>()] = v.as<std::string>();
                    }
                }
            }

            auto res = shield::net::HttpClient::upload(
                url, file_list, field_map, timeout.value_or(60));
            return to_table(lua, res);
        });

    // shield.http.download(url, output_path [, timeout]) -> response_table
    http.set_function("download",
        [&to_table](sol::this_state state, std::string url,
           std::string output_path, sol::optional<int> timeout) -> sol::table {
            sol::state_view lua(state);
            auto res = shield::net::HttpClient::download(
                url, output_path, timeout.value_or(60));
            return to_table(lua, res);
        });

    // shield.http.post_form(url, fields [, timeout]) -> response_table
    // fields: table of key-value pairs for application/x-www-form-urlencoded
    http.set_function("post_form",
        [&to_table](sol::this_state state, std::string url,
           sol::table fields, sol::optional<int> timeout) -> sol::table {
            sol::state_view lua(state);

            std::unordered_map<std::string, std::string> field_map;
            for (auto& [k, v] : fields) {
                if (k.is<std::string>() && v.is<std::string>()) {
                    field_map[k.as<std::string>()] = v.as<std::string>();
                }
            }

            auto res = shield::net::HttpClient::post_form(
                url, field_map, timeout.value_or(10));
            return to_table(lua, res);
        });

    shield["http"] = http;

    // =========================================================================
    // shield.httpd — HTTP 服务端（注册路由处理 incoming 请求）
    // =========================================================================
    auto httpd = lua.create_table();

    // Route registration stubs. Full integration passes the HttpServer
    // instance; these return true to indicate the call was accepted.
    auto register_route = [](sol::this_state state, std::string method,
                              std::string path, sol::function handler) {
        sol::state_view lua(state);
        // TODO: store route in HttpServer instance when integrated with bootstrap
        return sol::make_object(lua, true);
    };

    httpd.set_function("get", [register_route](sol::this_state s, std::string p,
                                                sol::function h) {
        return register_route(s, "GET", std::move(p), std::move(h));
    });
    httpd.set_function("post", [register_route](sol::this_state s, std::string p,
                                                 sol::function h) {
        return register_route(s, "POST", std::move(p), std::move(h));
    });
    httpd.set_function("put", [register_route](sol::this_state s, std::string p,
                                                sol::function h) {
        return register_route(s, "PUT", std::move(p), std::move(h));
    });
    httpd.set_function("delete", [register_route](sol::this_state s, std::string p,
                                                   sol::function h) {
        return register_route(s, "DELETE", std::move(p), std::move(h));
    });
    httpd.set_function("patch", [register_route](sol::this_state s, std::string p,
                                                  sol::function h) {
        return register_route(s, "PATCH", std::move(p), std::move(h));
    });

    shield["httpd"] = httpd;
}

void register_plugin_api(sol::table& shield) {
    sol::state_view lua(shield.lua_state());
    auto plugin = lua.create_table();

    // shield.plugin.packages() -> array of {id, version, kind, provides}
    plugin.set_function("packages", [](sol::this_state state) -> sol::table {
        sol::state_view lua(state);
        auto t = lua.create_table();
        for (const auto& p : shield::plugin::global_host().list_packages()) {
            sol::table row = lua.create_table();
            row["id"] = p.id;
            row["version"] = p.version;
            row["kind"] = p.kind;
            sol::table prov = lua.create_table();
            for (size_t i = 0; i < p.provides.size(); ++i) prov[i + 1] = p.provides[i];
            row["provides"] = prov;
            t[t.size() + 1] = row;
        }
        return t;
    });

    // shield.plugin.instances() -> array of {id, package, state, required}
    plugin.set_function("instances", [](sol::this_state state) -> sol::table {
        sol::state_view lua(state);
        auto t = lua.create_table();
        for (const auto& in : shield::plugin::global_host().list_instances()) {
            sol::table row = lua.create_table();
            row["id"] = in.id;
            row["package"] = in.package;
            row["state"] = in.state;
            row["required"] = in.required;
            t[t.size() + 1] = row;
        }
        return t;
    });

    // shield.plugin.instance(id) -> table or nil
    plugin.set_function("instance", [](sol::this_state state, std::string id) -> sol::object {
        sol::state_view lua(state);
        for (const auto& in : shield::plugin::global_host().list_instances()) {
            if (in.id == id) {
                sol::table row = lua.create_table();
                row["id"] = in.id;
                row["package"] = in.package;
                row["state"] = in.state;
                row["required"] = in.required;
                return row;
            }
        }
        return sol::nil;
    });

    // shield.plugin.binding(name) -> {instance_id, interface} or nil
    plugin.set_function("binding", [](sol::this_state state, std::string name) -> sol::object {
        sol::state_view lua(state);
        auto b = shield::plugin::global_host().get_binding(name);
        if (!b) return sol::nil;
        sol::table row = lua.create_table();
        row["instance_id"] = b->instance_id;
        row["interface"] = b->interface;
        return row;
    });

    shield["plugin"] = plugin;
}

void register_full_shield_api(sol::state& lua, LuaServiceManager* manager,
                               LuaRuntime* runtime) {
    // Initialize HTTP client (libcurl global state).
    shield::net::HttpClient::initialize();

    // Register usertypes
    ServiceHandle::register_usertype(lua);
    register_session_handle(lua);

    auto shield = lua.create_table();

    register_service_api(shield, manager);
    register_message_api(shield, manager, runtime);
    register_timer_api(shield, manager, runtime);
    register_task_api(shield, manager, runtime);
    register_config_api(shield);
    register_log_api(shield, manager);
    register_data_api(shield, manager);
    register_http_api(shield);
    register_plugin_api(shield);

#ifdef SHIELD_ENABLE_CLUSTER
    register_cluster_api(shield, manager);
#endif

    lua["shield"] = shield;

    // Coroutine dispatch helper used by LuaRuntime::call_service_method_coroutine.
    // It wraps a handler + args table into a coroutine whose body returns the
    // handler's results, so a yield inside the handler (e.g. shield.sleep)
    // suspends the whole coroutine and a later resume continues transparently.
    // Defined as a global function directly so a script error can't take down
    // register_api (call_service_method_coroutine falls back to sync dispatch
    // if the helper is absent).
    lua.safe_script(
        "function __shield_run_handler(handler, args)\n"
        "  return coroutine.create(function()\n"
        "    return handler(table.unpack(args))\n"
        "  end)\n"
        "end",
        [](lua_State*, sol::protected_function_result pfr) -> sol::protected_function_result {
            return pfr;
        });
}

}  // namespace shield::lua
