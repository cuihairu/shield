// Shared Lua helpers for SQL-style database plugins.
//
// Every database plugin that exposes a per-instance proxy with
// query / query_one / execute / transaction methods should call
// apply_db_mapper_api(lua, proxy) on each freshly-built proxy. This
// attaches the mapper / register_mapper / entity helpers — a pure-Lua
// DSL over parameterized SQL — without forcing each plugin to duplicate
// the runtime script.
//
// This header is header-only by design: each plugin MODULE compiles a
// private copy of the constexpr script into its own binary. The script
// is small (~6KB) and the cost of duplication is dwarfed by avoiding
// a cross-DLL singleton / shared Lua state.
//
// Why a C++ header instead of a .lua file shipped next to plugin.json?
//   - Plugin autonomy: each plugin must work with *only* its own
//     directory present. A shared `lua/` directory would need to be
//     installed separately and tracked at deploy time.
//   - No path injection needed: the host's inject_lua_paths only adds
//     paths declared in *this* plugin's manifest, not arbitrary
//     shared locations.
//   - Single source of truth: the Lua source lives next to the C++
//     call site that uses it, which is easier to review than a Lua
//     file referenced from three different plugin.json manifests.
#pragma once

#include <sol/sol.hpp>
#include <string>
#include <string_view>

namespace shield::plugins {

// Pure-Lua script that returns a function(db_proxy) -> db_proxy.
// The function adds `mapper`, `register_mapper`, and `entity` methods
// to the given proxy table. The proxy must expose:
//   query(sql, params?)        -> (ok, rows|err)
//   query_one(sql, params?)    -> (ok, row|nil|err)
//   execute(sql, params?)      -> (ok, result|err)
//   transaction(fn)            -> (ok, ...)
//
// The script is self-contained: no globals are modified. All helpers
// (is_array, identifier, compile_sql, ...) are local closures. The
// returned function closes over them and only touches the passed proxy.
inline constexpr std::string_view kDbMapperLuaScript = R"lua(
return function(db)
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
      local pklookup = {}
      for _, field in ipairs(self.primary_key) do pklookup[field] = true end
      for _, field in ipairs(self:ordered_fields()) do
        if not pklookup[field] and row[field] ~= nil then
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

  return db
end
)lua";

// Attach mapper/register_mapper/entity methods to a freshly-built proxy.
//
// Idempotent at the lua_State level: the apply function is compiled once
// per VM and cached in the Lua registry. Subsequent calls reuse the cached
// function. Returns true on success; failure to load the script is non-fatal
// — query/query_one/execute/transaction still work, only the DSL helpers
// are missing.
//
// Thread-safety: each lua_State is single-threaded by Shield's contract
// (one VM per service), so no lock is needed around the registry access.
inline bool apply_db_mapper_api(sol::state_view lua, sol::table proxy) {
    if (!lua.valid() || !proxy.valid()) return false;

    sol::table reg = lua.registry();
    sol::optional<sol::protected_function> cached =
        reg.get<sol::optional<sol::protected_function>>(
            "__shield_db_mapper_apply");

    sol::protected_function apply_fn;
    if (cached && cached->valid()) {
        apply_fn = *cached;
    } else {
        // Compile the chunk once. lua.load() is unambiguous compared to
        // safe_script's many overloads. The chunk returns a function
        // (closure over local helpers) that we then invoke with the proxy.
        sol::load_result load_res = lua.load(
            std::string(kDbMapperLuaScript),
            "shield_db_mapper",
            sol::load_mode::text);
        if (!load_res.valid() ||
            load_res.get_type() != sol::type::function) {
            return false;
        }
        apply_fn = load_res.get<sol::protected_function>();
        // Cache for subsequent proxies on the same VM.
        reg["__shield_db_mapper_apply"] = apply_fn;
    }

    auto call_res = apply_fn(proxy);
    return call_res.valid();
}

}  // namespace shield::plugins
