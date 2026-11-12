// [SHIELD_LUA] shield.http/httpd client routes and shield.plugin
// package listing. Split out of lua_api.cpp.
#include "lua_api_detail.hpp"
#include "shield/config/config.hpp"
#include "shield/lua/lua_api.hpp"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_PLAYER
#include "shield/player/player_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_SERVER
#include "shield/server/server_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_GLOBAL
#include "shield/global/global_manager.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <caf/actor.hpp>
#include <caf/send.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/binding.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_crypto.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/lua/player_ref_box.hpp"
#include "shield/net/http_client.hpp"
#include "shield/net/session.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace shield::lua {

void register_http_api(shd::table& shield, LuaServiceManager* manager,
                       LuaRuntime* runtime) {
    shd::state_view lua(shield.lua_state());

    // =========================================================================
    // shield.http — HTTP 客户端（发请求）
    // =========================================================================
    auto http = lua.create_table();

    // Helper: convert HttpClientResponse to Lua table.
    // Auto-parses JSON body into `data` field when Content-Type is JSON.
    auto to_table =
        [](shd::state_view lua,
           const shield::net::HttpClientResponse& res) -> shd::table {
        shd::table result = lua.create_table();
        result["status"] = res.status_code;
        result["body"] = res.body;
        result["ok"] = res.ok();
        result["error"] = res.error;
        shd::table headers = lua.create_table();
        for (const auto& [k, v] : res.headers) {
            headers[k] = v;
        }
        result["headers"] = headers;

        // Auto-parse JSON response body into `data` field.
        if (!res.body.empty()) {
            auto ct_it = res.headers.find("Content-Type");
            if (ct_it == res.headers.end())
                ct_it = res.headers.find("content-type");
            bool is_json = false;
            if (ct_it != res.headers.end()) {
                is_json =
                    ct_it->second.find("application/json") != std::string::npos;
            }
            // Also try parsing if body starts with { or [
            if (!is_json &&
                !res.body  // GCOVR_EXCL_BR_LINE (integration: no json ctype)
                     .empty() &&  // GCOVR_EXCL_BR_LINE (integration: suites
                                  // never receive a JSON content-type response)
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
    auto parse_opts = [](shd::table opts,
                         shield::net::HttpClientOptions& options) {
        if (opts["method"].valid()) {
            options.method = opts["method"].get<std::string>();
        }
        if (opts["body"].valid()) {
            options.body = opts["body"].get<std::string>();
        }
        if (opts["timeout"].valid()) {
            // Clamp the timeout to >= 1s: CURLOPT_TIMEOUT treats 0 as "no
            // timeout", which would let a script block its actor thread
            // forever.
            options.timeout_seconds = std::max(opts["timeout"].get<int>(), 1);
        }
        if (opts["headers"].valid()) {
            shd::table hdrs = opts["headers"];
            for (auto&& [k, v] : hdrs) {
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
            shd::table basic = opts["auth_basic"];
            if (basic["user"].valid()) {
                options.auth_basic_user = basic["user"].get<std::string>();
            }
            if (basic["password"].valid()) {
                options.auth_basic_password =
                    basic["password"].get<std::string>();
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
    http.set_function(
        "request",
        [to_table, parse_opts](shd::this_state state, std::string url,
                               std::optional<shd::table> opts) -> shd::table {
            shd::state_view lua(state);

            shield::net::HttpClientOptions options;
            options.url = url;

            if (opts) {
                parse_opts(*opts, options);
            }

            auto res = shield::net::HttpClient::request(options);
            return to_table(lua, res);
        });

    // Convenience: shield.http.get(url [, options]) -> response_table
    http.set_function(
        "get",
        [to_table, parse_opts](shd::this_state state, std::string url,
                               std::optional<shd::table> opts) -> shd::table {
            shd::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "GET";
            options.url = url;
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.post(url [, body] [, options]) -> response_table
    http.set_function(
        "post",
        [to_table, parse_opts](shd::this_state state, std::string url,
                               std::optional<std::string> body,
                               std::optional<shd::table> opts) -> shd::table {
            shd::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.put(url [, body] [, options]) -> response_table
    http.set_function(
        "put",
        [to_table, parse_opts](shd::this_state state, std::string url,
                               std::optional<std::string> body,
                               std::optional<shd::table> opts) -> shd::table {
            shd::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PUT";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.delete(url [, options]) -> response_table
    http.set_function(
        "delete",
        [to_table, parse_opts](shd::this_state state, std::string url,
                               std::optional<shd::table> opts) -> shd::table {
            shd::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "DELETE";
            options.url = url;
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.patch(url [, body] [, options]) ->
    // response_table
    http.set_function(
        "patch",
        [to_table, parse_opts](shd::this_state state, std::string url,
                               std::optional<std::string> body,
                               std::optional<shd::table> opts) -> shd::table {
            shd::state_view lua(state);
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
    http.set_function(
        "json",
        [to_table, parse_opts](shd::this_state state, std::string url,
                               shd::object data,
                               std::optional<shd::table> opts) -> shd::table {
            shd::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            // Serialize Lua table/object to JSON string.
            options.body = lua_table_to_json(data.as<shd::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_post(url, data [, options]) -> response_table
    http.set_function(
        "json_post",
        [to_table, parse_opts](shd::this_state state, std::string url,
                               shd::object data,
                               std::optional<shd::table> opts) -> shd::table {
            shd::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<shd::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_put(url, data [, options]) -> response_table
    http.set_function(
        "json_put",
        [to_table, parse_opts](shd::this_state state, std::string url,
                               shd::object data,
                               std::optional<shd::table> opts) -> shd::table {
            shd::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PUT";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<shd::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_patch(url, data [, options]) -> response_table
    http.set_function(
        "json_patch",
        [to_table, parse_opts](shd::this_state state, std::string url,
                               shd::object data,
                               std::optional<shd::table> opts) -> shd::table {
            shd::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PATCH";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<shd::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.upload(url, files [, fields] [, timeout]) -> response_table
    // files: array of {field_name, file_path, content_type}
    // fields: table of form field key-value pairs
    http.set_function(
        "upload",
        [to_table](shd::this_state state, std::string url, shd::table files,
                   std::optional<shd::table> fields,
                   std::optional<int> timeout) -> shd::table {
            shd::state_view lua(state);

            std::vector<shield::net::HttpFileField> file_list;
            for (auto&& [i, entry] : files) {
                if (entry.is<shd::table>()) {
                    shd::table f = entry.as<shd::table>();
                    shield::net::HttpFileField field;
                    field.field_name =
                        f.get_or<std::string>("field_name", "file");
                    field.file_path = f.get_or<std::string>("file_path", "");
                    field.content_type =
                        f.get_or<std::string>("content_type", "");
                    file_list.push_back(std::move(field));
                }
            }

            std::unordered_map<std::string, std::string> field_map;
            if (fields) {
                for (auto&& [k, v] : *fields) {
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
                      [to_table](shd::this_state state, std::string url,
                                 std::string output_path,
                                 std::optional<int> timeout) -> shd::table {
                          shd::state_view lua(state);
                          auto res = shield::net::HttpClient::download(
                              url, output_path, timeout.value_or(60));
                          return to_table(lua, res);
                      });

    // shield.http.post_form(url, fields [, timeout]) -> response_table
    // fields: table of key-value pairs for application/x-www-form-urlencoded
    http.set_function(
        "post_form",
        [to_table](shd::this_state state, std::string url, shd::table fields,
                   std::optional<int> timeout) -> shd::table {
            shd::state_view lua(state);

            std::unordered_map<std::string, std::string> field_map;
            for (auto&& [k, v] : fields) {
                if (k.is<std::string>() && v.is<std::string>()) {
                    field_map[k.as<std::string>()] = v.as<std::string>();
                }
            }

            auto res = shield::net::HttpClient::post_form(url, field_map,
                                                          timeout.value_or(10));
            return to_table(lua, res);
        });

    shield["http"] = http;

    // =========================================================================
    // shield.httpd — HTTP 服务端（注册路由处理 incoming 请求）
    // =========================================================================
    auto httpd = lua.create_table();

    // Routes are stored in the runtime's registration table. The bootstrap
    // HTTP bridge (LuaHttpBridge) mirrors them into the HttpServer and
    // dispatches requests back onto the registering service's actor thread.
    auto register_route = [manager, runtime](
                              shd::this_state state, std::string method,
                              std::string path, shd::function handler) {
        if (!manager || !runtime) {
            throw shd::error("shield.httpd is not available in this context");
        }
        const std::string service_id = manager->current_service_id();
        if (service_id.empty()) {
            throw shd::error(
                "shield.httpd routes must be registered from a running "
                "service");
        }
        auto vm = runtime->vm_for_state(state);
        if (!vm) {  // GCOVR_EXCL_LINE (vm_for_state never returns null for a
            throw shd::error(  // registered VM; defensive)
                "registering VM is not managed by the runtime");  // GCOVR_EXCL_LINE
                                                                  // (continuation)
        }
        std::string error;
        if (!runtime->register_http_route(vm, service_id, method, path, handler,
                                          &error)) {
            throw shd::error("shield.httpd registration failed: " + error);
        }
        shd::state_view lua(state);
        return shd::make_object(lua, true);
    };

    httpd.set_function("get", [register_route](shd::this_state s, std::string p,
                                               shd::function h) {
        return register_route(s, "GET", std::move(p), std::move(h));
    });
    httpd.set_function(
        "post",
        [register_route](shd::this_state s, std::string p, shd::function h) {
            return register_route(s, "POST", std::move(p), std::move(h));
        });
    httpd.set_function("put", [register_route](shd::this_state s, std::string p,
                                               shd::function h) {
        return register_route(s, "PUT", std::move(p), std::move(h));
    });
    httpd.set_function(
        "delete",
        [register_route](shd::this_state s, std::string p, shd::function h) {
            return register_route(s, "DELETE", std::move(p), std::move(h));
        });
    httpd.set_function(
        "patch",
        [register_route](shd::this_state s, std::string p, shd::function h) {
            return register_route(s, "PATCH", std::move(p), std::move(h));
        });

    shield["httpd"] = httpd;
}

void register_plugin_api(shd::table& shield) {
    shd::state_view lua(shield.lua_state());
    auto plugin = lua.create_table();

    // shield.plugin.packages() -> array of {id, version, kind, provides}
    plugin.set_function("packages", [](shd::this_state state) -> shd::table {
        shd::state_view lua(state);
        auto t = lua.create_table();
        for (const auto& p : shield::plugin::global_host().list_packages()) {
            shd::table row = lua.create_table();
            row["id"] = p.id;
            row["version"] = p.version;
            row["kind"] = p.kind;
            row["docs_url"] = p.docs_url;
            row["docs_description"] = p.docs_description;
            shd::table prov = lua.create_table();
            for (size_t i = 0; i < p.provides.size(); ++i)
                prov[i + 1] = p.provides[i];
            row["provides"] = prov;
            t[t.size() + 1] = row;
        }
        return t;
    });  // GCOVR_EXCL_LINE (function-exit arc artifact of the plugin
         // packages lambda)

    // shield.plugin.instances() -> array of {id, package, state, required}
    plugin.set_function("instances", [](shd::this_state state) -> shd::table {
        shd::state_view lua(state);
        auto t = lua.create_table();
        for (const auto& in : shield::plugin::global_host().list_instances()) {
            shd::table row = lua.create_table();
            row["id"] = in.id;
            row["package"] = in.package;
            row["state"] = in.state;
            row["required"] = in.required;
            t[t.size() + 1] = row;
        }
        return t;
    });  // GCOVR_EXCL_LINE (function-exit arc artifact of the plugin
         // instances lambda)

    // shield.plugin.instance(id) -> table or nil
    plugin.set_function(  // GCOVR_EXCL_LINE (gcov continuation artifact)
        "instance",
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string id)         // GCOVR_EXCL_LINE (lambda entry artifact)
        -> shd::object {           // GCOVR_EXCL_LINE (lambda entry artifact)
            shd::state_view lua(state);
            for (const auto& in :
                 shield::plugin::global_host().list_instances()) {
                if (in.id == id) {
                    shd::table row = lua.create_table();
                    row["id"] = in.id;
                    row["package"] = in.package;
                    row["state"] = in.state;
                    row["required"] = in.required;
                    return row;
                }
            }
            return shd::nil;
        });  // GCOVR_EXCL_LINE (function-exit arc artifact of the plugin
             // instance lambda)

    // shield.plugin.binding(name) -> {instance_id, interface} or nil
    plugin.set_function(  // GCOVR_EXCL_LINE (gcov continuation artifact)
        "binding",
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string name)       // GCOVR_EXCL_LINE (lambda entry artifact)
        -> shd::object {           // GCOVR_EXCL_LINE (lambda entry artifact)
            shd::state_view lua(state);
            auto b = shield::plugin::global_host().get_binding(name);
            if (!b) return shd::nil;
            shd::table row = lua.create_table();
            row["instance_id"] = b->instance_id;
            row["interface"] = b->interface_name;
            return row;
        });

    shield["plugin"] = plugin;
}

}  // namespace shield::lua
