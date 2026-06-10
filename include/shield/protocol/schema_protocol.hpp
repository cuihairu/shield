// [CORE]
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>
#include <thread>

namespace shield::protocol {

enum class MessageDirection { C2S, S2C, BIDIRECTIONAL };
enum class MessageKind { RPC, EVENT, COMMAND, STREAM };
enum class FieldType {
    BOOL,
    INT32,
    INT64,
    UINT32,
    UINT64,
    FLOAT,
    DOUBLE,
    STRING,
    BYTES
};

enum class RpcErrorCode {
    NONE,
    TIMEOUT,
    CANCELED,
    TRANSPORT_ERROR,
    PROTOCOL_ERROR,
    REMOTE_ERROR,
    UNKNOWN
};

struct RpcError {
    RpcErrorCode code = RpcErrorCode::UNKNOWN;
    std::string message;
};

class ProtocolValue {
public:
    using Storage =
        std::variant<bool, int64_t, uint64_t, double, std::string,
                     std::vector<uint8_t>>;

    ProtocolValue() = default;
    ProtocolValue(bool value);
    ProtocolValue(int8_t value);
    ProtocolValue(int16_t value);
    ProtocolValue(int32_t value);
    ProtocolValue(int64_t value);
    ProtocolValue(uint8_t value);
    ProtocolValue(uint16_t value);
    ProtocolValue(uint32_t value);
    ProtocolValue(uint64_t value);
    ProtocolValue(float value);
    ProtocolValue(double value);
    ProtocolValue(const char* value);
    ProtocolValue(std::string value);
    ProtocolValue(std::string_view value);
    ProtocolValue(const std::vector<uint8_t>& value);
    ProtocolValue(std::vector<uint8_t>&& value);

    bool is_null() const;
    FieldType field_type() const;
    const Storage& storage() const;

    template <typename T>
    bool is() const {
        return storage_ && std::holds_alternative<T>(*storage_);
    }

    template <typename T>
    const T& get() const {
        return std::get<T>(storage());
    }

    std::string to_string() const;

private:
    std::optional<Storage> storage_;
};

struct MessageFieldDefinition {
    uint32_t id = 0;
    std::string name;
    FieldType type = FieldType::STRING;
    bool repeated = false;
    bool required = false;
    std::string default_value;
};

struct MessageDefinition {
    std::string name;
    uint32_t id = 0;
    MessageKind kind = MessageKind::RPC;
    MessageDirection direction = MessageDirection::BIDIRECTIONAL;
    uint32_t timeout_ms = 0;
    bool compressed = false;
    std::vector<MessageFieldDefinition> fields;
};

struct MessageField {
    uint32_t field_id = 0;
    std::vector<ProtocolValue> values;
};

struct MessageEnvelope {
    uint32_t message_id = 0;
    uint64_t correlation_id = 0;
    uint64_t stream_id = 0;
    uint32_t sequence = 0;
    bool compressed = false;
    std::vector<MessageField> fields;
};

class SchemaRegistry {
public:
    bool load_from_xml_file(const std::string& path);
    bool load_from_xml_string(const std::string& xml);
    void clear();

    const MessageDefinition* find_message_by_id(uint32_t message_id) const;
    const MessageDefinition* find_message_by_name(
        const std::string& name) const;

    const std::vector<MessageDefinition>& messages() const { return messages_; }
    uint64_t schema_hash() const { return schema_hash_; }
    const std::string& last_error() const { return last_error_; }

private:
    std::vector<MessageDefinition> messages_;
    std::unordered_map<uint32_t, size_t> message_by_id_;
    std::unordered_map<std::string, size_t> message_by_name_;
    uint64_t schema_hash_ = 0;
    std::string last_error_;
};

std::vector<uint8_t> encode_message(const MessageDefinition& definition,
                                    const MessageEnvelope& envelope);
MessageEnvelope decode_message(const SchemaRegistry& registry,
                               const std::vector<uint8_t>& data);

class RpcTaskBase {
public:
    virtual ~RpcTaskBase() = default;
    virtual void reject(const RpcError& error) = 0;
    virtual bool ready() const = 0;
};

template <typename T>
class RpcTask : public RpcTaskBase,
                public std::enable_shared_from_this<RpcTask<T>> {
public:
    using Result = std::variant<T, RpcError>;
    using SuccessHandler = std::function<void(const T&)>;
    using ErrorHandler = std::function<void(const RpcError&)>;
    using Finalizer = std::function<void()>;

    RpcTask();

    std::shared_future<Result> future() const;

    RpcTask& on_ok(SuccessHandler handler);
    RpcTask& on_err(ErrorHandler handler);
    RpcTask& on_complete(Finalizer finalizer);

    void resolve(T value);
    void reject(const RpcError& error) override;
    bool ready() const override;

private:
    struct State {
        std::promise<Result> promise;
        std::shared_future<Result> future;
        std::atomic_bool completed{false};
        std::optional<Result> result;
        std::vector<SuccessHandler> ok_handlers;
        std::vector<ErrorHandler> err_handlers;
        Finalizer finalizer;
        std::mutex mutex;

        State();
    };

    void complete(Result result);
    static bool is_success_result(const Result& result);

    std::shared_ptr<State> state_;
};

class PendingRpcRegistry {
public:
    template <typename T>
    std::shared_ptr<RpcTask<T>> create(
        uint64_t request_id, std::chrono::milliseconds timeout = {},
        std::function<RpcError()> timeout_error_factory = {});

    template <typename T>
    bool resolve(uint64_t request_id, T value);

    bool reject(uint64_t request_id, const RpcError& error);
    bool cancel(uint64_t request_id, std::string reason = "Canceled");
    bool contains(uint64_t request_id) const;
    size_t size() const;

private:
    struct EntryBase {
        virtual ~EntryBase() = default;
        virtual bool ready() const = 0;
        virtual void reject(const RpcError& error) = 0;
    };

    template <typename T>
    struct Entry final : EntryBase {
        explicit Entry(std::shared_ptr<RpcTask<T>> task) : task(std::move(task)) {}

        bool ready() const override { return task->ready(); }
        void reject(const RpcError& error) override { task->reject(error); }

        std::shared_ptr<RpcTask<T>> task;
    };

    void erase(uint64_t request_id);

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<EntryBase>> entries_;
};

template <typename T>
RpcTask<T>::State::State() : future(promise.get_future().share()) {}

template <typename T>
RpcTask<T>::RpcTask() : state_(std::make_shared<State>()) {}

template <typename T>
std::shared_future<typename RpcTask<T>::Result> RpcTask<T>::future() const {
    return state_->future;
}

template <typename T>
RpcTask<T>& RpcTask<T>::on_ok(SuccessHandler handler) {
    bool invoke_now = false;
    std::optional<T> value;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->completed.load()) {
            state_->ok_handlers.push_back(std::move(handler));
            return *this;
        }

        if (state_->result && std::holds_alternative<T>(*state_->result)) {
            invoke_now = true;
            value.emplace(std::get<T>(*state_->result));
        }
    }

    if (invoke_now && value) {
        handler(*value);
    }
    return *this;
}

template <typename T>
RpcTask<T>& RpcTask<T>::on_err(ErrorHandler handler) {
    bool invoke_now = false;
    RpcError error;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->completed.load()) {
            state_->err_handlers.push_back(std::move(handler));
            return *this;
        }

        if (state_->result && std::holds_alternative<RpcError>(*state_->result)) {
            invoke_now = true;
            error = std::get<RpcError>(*state_->result);
        }
    }

    if (invoke_now) {
        handler(error);
    }
    return *this;
}

template <typename T>
RpcTask<T>& RpcTask<T>::on_complete(Finalizer finalizer) {
    bool invoke_now = false;
    Finalizer invoke_finalizer;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->finalizer = std::move(finalizer);
        invoke_finalizer = state_->finalizer;
        invoke_now = state_->completed.load();
    }

    if (invoke_now && invoke_finalizer) {
        invoke_finalizer();
    }
    return *this;
}

template <typename T>
void RpcTask<T>::resolve(T value) {
    complete(Result{std::move(value)});
}

template <typename T>
void RpcTask<T>::reject(const RpcError& error) {
    complete(Result{error});
}

template <typename T>
bool RpcTask<T>::ready() const {
    return state_->completed.load();
}

template <typename T>
void RpcTask<T>::complete(Result result) {
    std::vector<SuccessHandler> ok_handlers;
    std::vector<ErrorHandler> err_handlers;
    Finalizer finalizer;
    bool is_success = false;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->completed.exchange(true)) {
            return;
        }

        state_->result = std::move(result);
        ok_handlers = state_->ok_handlers;
        err_handlers = state_->err_handlers;
        finalizer = state_->finalizer;
        is_success = is_success_result(*state_->result);
    }

    state_->promise.set_value(*state_->result);

    if (is_success) {
        const T& value = std::get<T>(*state_->result);
        for (const auto& handler : ok_handlers) {
            if (handler) {
                handler(value);
            }
        }
    } else {
        const RpcError& error = std::get<RpcError>(*state_->result);
        for (const auto& handler : err_handlers) {
            if (handler) {
                handler(error);
            }
        }
    }

    if (finalizer) {
        finalizer();
    }
}

template <typename T>
bool RpcTask<T>::is_success_result(const Result& result) {
    return std::holds_alternative<T>(result);
}

template <typename T>
std::shared_ptr<RpcTask<T>> PendingRpcRegistry::create(
    uint64_t request_id, std::chrono::milliseconds timeout,
    std::function<RpcError()> timeout_error_factory) {
    auto task = std::make_shared<RpcTask<T>>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[request_id] = std::make_shared<Entry<T>>(task);
    }

    task->on_complete([this, request_id]() { erase(request_id); });

    if (timeout.count() > 0) {
        std::weak_ptr<RpcTask<T>> weak_task = task;
        std::thread([weak_task, timeout,
                     timeout_error_factory = std::move(timeout_error_factory)]() mutable {
            std::this_thread::sleep_for(timeout);
            if (auto locked = weak_task.lock(); locked && !locked->ready()) {
                if (timeout_error_factory) {
                    locked->reject(timeout_error_factory());
                } else {
                    locked->reject(
                        RpcError{RpcErrorCode::TIMEOUT, "RPC timeout"});
                }
            }
        }).detach();
    }

    return task;
}

template <typename T>
bool PendingRpcRegistry::resolve(uint64_t request_id, T value) {
    std::shared_ptr<EntryBase> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(request_id);
        if (it == entries_.end()) {
            return false;
        }
        entry = it->second;
    }

    auto typed = std::dynamic_pointer_cast<Entry<T>>(entry);
    if (!typed) {
        return false;
    }

    typed->task->resolve(std::move(value));
    return true;
}

inline bool PendingRpcRegistry::reject(uint64_t request_id,
                                       const RpcError& error) {
    std::shared_ptr<EntryBase> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(request_id);
        if (it == entries_.end()) {
            return false;
        }
        entry = it->second;
    }

    entry->reject(error);
    return true;
}

inline bool PendingRpcRegistry::cancel(uint64_t request_id,
                                       std::string reason) {
    return reject(request_id, RpcError{RpcErrorCode::CANCELED,
                                       reason.empty() ? "Canceled" : reason});
}

inline bool PendingRpcRegistry::contains(uint64_t request_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(request_id) != entries_.end();
}

inline size_t PendingRpcRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace shield::protocol
