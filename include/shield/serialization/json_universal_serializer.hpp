#pragma once

#include <nlohmann/json.hpp>

#include "universal_serializer.hpp"

namespace shield::serialization {

// JSON serializer implementation
class JsonUniversalSerializer
    : public UniversalSerializer<SerializationFormat::JSON> {
public:
    std::vector<uint8_t> serialize_bytes(const void *object,
                                         const std::type_info &type) override;
    void deserialize_bytes(const std::vector<uint8_t> &data, void *object,
                           const std::type_info &type) override;

    std::string get_name() const override { return "JSON"; }

protected:
    template <typename T>
    std::string serialize_impl(const T &object) {
        if constexpr (JsonSerializable<T>) {
            return serialize_json(object);
        } else {
            throw SerializationException("Type is not JSON serializable");
        }
    }

    template <typename T>
    T deserialize_impl(const std::string &json_str) {
        if constexpr (JsonSerializable<T>) {
            return deserialize_json<T>(json_str);
        } else {
            throw SerializationException("Type is not JSON deserializable");
        }
    }

private:
    // JSON serialization implementation
    template <typename T>
    std::string serialize_json(const T &object) {
        try {
            if constexpr (requires {
                              to_json(std::declval<nlohmann::json &>(), object);
                          }) {
                // Use ADL to_json
                nlohmann::json j;
                to_json(j, object);
                return j.dump();
            } else if constexpr (requires { object.to_json(); }) {
                // Use member function
                return object.to_json();
            } else if constexpr (requires { nlohmann::json{object}; }) {
                // Use implicit conversion
                nlohmann::json j = object;
                return j.dump();
            } else {
                throw SerializationException(
                    "No JSON serialization method available for type: " +
                    std::string(typeid(T).name()));
            }
        } catch (const nlohmann::json::exception &e) {
            throw SerializationException("JSON serialization failed: " +
                                         std::string(e.what()));
        }
    }

    template <typename T>
    T deserialize_json(const std::string &json_str) {
        try {
            nlohmann::json j = nlohmann::json::parse(json_str);

            if constexpr (requires(T &obj) { from_json(j, obj); }) {
                // Use ADL from_json
                T object;
                from_json(j, object);
                return object;
            } else if constexpr (requires { T::from_json(json_str); }) {
                // Use static member function
                return T::from_json(json_str);
            } else if constexpr (requires { j.template get<T>(); }) {
                // Use nlohmann::json's get method
                return j.template get<T>();
            } else {
                throw SerializationException(
                    "No JSON deserialization method available for type: " +
                    std::string(typeid(T).name()));
            }
        } catch (const nlohmann::json::exception &e) {
            throw SerializationException("JSON deserialization failed: " +
                                         std::string(e.what()));
        }
    }
};

// Factory function
std::unique_ptr<JsonUniversalSerializer> create_json_universal_serializer();

// Convenience functions
template <typename T>
std::string to_json_string(const T &object)
    requires JsonSerializable<T>
{
    return serialize_as<SerializationFormat::JSON>(object);
}

template <typename T>
T from_json_string(const std::string &json_str)
    requires JsonSerializable<T>
{
    return deserialize_as<SerializationFormat::JSON, T>(json_str);
}

}  // namespace shield::serialization