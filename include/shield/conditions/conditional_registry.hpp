#pragma once

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/core/application_context.hpp"
#include "shield/di/advanced_container.hpp"

namespace shield::conditions {

/**
 * @brief Base condition interface
 */
class Condition {
public:
    virtual ~Condition() = default;

    /**
     * @brief Evaluate condition
     * @return true if condition is met, false otherwise
     */
    virtual bool matches() const = 0;

    /**
     * @brief Get condition description for debugging
     */
    virtual std::string description() const = 0;
};

/**
 * @brief Property-based condition
 */
class PropertyCondition : public Condition {
public:
    PropertyCondition(const std::string& property_name,
                      const std::string& expected_value = "true",
                      bool match_if_missing = false)
        : property_name_(property_name),
          expected_value_(expected_value),
          match_if_missing_(match_if_missing) {}

    bool matches() const override {
        auto& config_manager = shield::config::ConfigManager::instance();
        const auto& config_tree = config_manager.get_config_tree();

        try {
            auto value = config_tree.get<std::string>(property_name_);
            return value == expected_value_;
        } catch (const boost::property_tree::ptree_bad_path&) {
            return match_if_missing_;
        }
    }

    std::string description() const override {
        return "Property '" + property_name_ + "' equals '" + expected_value_ +
               "'";
    }

private:
    std::string property_name_;
    std::string expected_value_;
    bool match_if_missing_;
};

/**
 * @brief Profile-based condition
 */
class ProfileCondition : public Condition {
public:
    explicit ProfileCondition(const std::vector<std::string>& required_profiles)
        : required_profiles_(required_profiles) {}

    explicit ProfileCondition(const std::string& required_profile)
        : required_profiles_{required_profile} {}

    bool matches() const override {
        // Get active profiles from configuration
        auto active_profiles = get_active_profiles();

        for (const auto& required_profile : required_profiles_) {
            if (active_profiles.count(required_profile) > 0) {
                return true;
            }
        }
        return required_profiles_
            .empty();  // If no profiles required, always match
    }

    std::string description() const override {
        std::string desc = "Active profile matches one of: [";
        for (size_t i = 0; i < required_profiles_.size(); ++i) {
            if (i > 0) desc += ", ";
            desc += required_profiles_[i];
        }
        desc += "]";
        return desc;
    }

private:
    std::vector<std::string> required_profiles_;

    std::unordered_set<std::string> get_active_profiles() const {
        // Implementation would read from configuration
        // For now, return default profile
        return {"default"};
    }
};

/**
 * @brief Bean existence condition
 */
class BeanCondition : public Condition {
public:
    template <typename T>
    static BeanCondition on_bean() {
        return BeanCondition(std::type_index(typeid(T)), true);
    }

    template <typename T>
    static BeanCondition on_missing_bean() {
        return BeanCondition(std::type_index(typeid(T)), false);
    }

    bool matches() const override {
        // This would need to check the actual container/context
        // For now, simplified implementation
        return expect_exists_;  // Placeholder
    }

    std::string description() const override {
        return expect_exists_ ? "Bean of type exists: " + bean_type_.name()
                              : "Bean of type missing: " + bean_type_.name();
    }

private:
    BeanCondition(std::type_index type, bool expect_exists)
        : bean_type_(type), expect_exists_(expect_exists) {}

    std::type_index bean_type_;
    bool expect_exists_;
};

/**
 * @brief Class presence condition
 */
class ClassCondition : public Condition {
public:
    explicit ClassCondition(const std::string& class_name)
        : class_name_(class_name) {}

    bool matches() const override {
        // In C++, this is challenging without proper reflection
        // For now, always return true (assume class is present)
        return true;
    }

    std::string description() const override {
        return "Class is present: " + class_name_;
    }

private:
    std::string class_name_;
};

/**
 * @brief Composite condition for combining multiple conditions
 */
class CompositeCondition : public Condition {
public:
    enum class LogicalOperator { AND, OR };

    CompositeCondition(LogicalOperator op) : operator_(op) {}

    CompositeCondition& add_condition(std::unique_ptr<Condition> condition) {
        conditions_.push_back(std::move(condition));
        return *this;
    }

    bool matches() const override {
        if (conditions_.empty()) {
            return true;
        }

        if (operator_ == LogicalOperator::AND) {
            for (const auto& condition : conditions_) {
                if (!condition->matches()) {
                    return false;
                }
            }
            return true;
        } else {  // OR
            for (const auto& condition : conditions_) {
                if (condition->matches()) {
                    return true;
                }
            }
            return false;
        }
    }

    std::string description() const override {
        std::string desc = "(";
        std::string op_str =
            (operator_ == LogicalOperator::AND) ? " AND " : " OR ";

        for (size_t i = 0; i < conditions_.size(); ++i) {
            if (i > 0) desc += op_str;
            desc += conditions_[i]->description();
        }
        desc += ")";
        return desc;
    }

private:
    LogicalOperator operator_;
    std::vector<std::unique_ptr<Condition>> conditions_;
};

/**
 * @brief Conditional bean registration
 */
class ConditionalBeanRegistry {
public:
    /**
     * @brief Conditional bean registration info
     */
    struct ConditionalBeanInfo {
        std::type_index bean_type;
        std::function<std::shared_ptr<void>()> factory;
        std::unique_ptr<Condition> condition;
        std::string name;
        di::ServiceLifetime lifetime;

        ConditionalBeanInfo(
            std::type_index type, std::function<std::shared_ptr<void>()> fact,
            std::unique_ptr<Condition> cond, const std::string& bean_name = "",
            di::ServiceLifetime life = di::ServiceLifetime::SINGLETON)
            : bean_type(type),
              factory(std::move(fact)),
              condition(std::move(cond)),
              name(bean_name),
              lifetime(life) {}
    };

    static ConditionalBeanRegistry& instance() {
        static ConditionalBeanRegistry registry;
        return registry;
    }

    /**
     * @brief Register conditional bean
     */
    template <typename T>
    void register_conditional_bean(
        std::unique_ptr<Condition> condition,
        std::function<std::shared_ptr<T>()> factory = nullptr,
        const std::string& name = "",
        di::ServiceLifetime lifetime = di::ServiceLifetime::SINGLETON) {
        if (!factory) {
            factory = []() { return std::make_shared<T>(); };
        }

        auto wrapper_factory = [factory]() -> std::shared_ptr<void> {
            return std::static_pointer_cast<void>(factory());
        };

        conditional_beans_.emplace_back(std::type_index(typeid(T)),
                                        std::move(wrapper_factory),
                                        std::move(condition), name, lifetime);
    }

    /**
     * @brief Process all conditional registrations and register matching beans
     */
    void process_conditional_registrations(di::AdvancedContainer& container);
    void process_conditional_registrations(core::ApplicationContext& context);

    /**
     * @brief Get all conditional beans (for debugging)
     */
    const std::vector<ConditionalBeanInfo>& get_conditional_beans() const {
        return conditional_beans_;
    }

    /**
     * @brief Clear all conditional registrations
     */
    void clear() { conditional_beans_.clear(); }

private:
    ConditionalBeanRegistry() = default;
    std::vector<ConditionalBeanInfo> conditional_beans_;
};

}  // namespace shield::conditions

/**
 * @brief Conditional registration macros (Spring Boot style)
 */

// Register bean conditional on property
#define SHIELD_CONDITIONAL_ON_PROPERTY(BeanType, property, value)             \
    static inline auto _shield_conditional_property_##BeanType##__COUNTER__ = \
        []() {                                                                \
            auto condition =                                                  \
                std::make_unique<shield::conditions::PropertyCondition>(      \
                    property, value);                                         \
            shield::conditions::ConditionalBeanRegistry::instance()           \
                .register_conditional_bean<BeanType>(std::move(condition));   \
            return 0;                                                         \
        }();

// Register bean conditional on missing bean
#define SHIELD_CONDITIONAL_ON_MISSING_BEAN(BeanType, MissingBeanType)        \
    static inline auto _shield_conditional_missing_##BeanType##__COUNTER__ = \
        []() {                                                               \
            auto condition =                                                 \
                shield::conditions::BeanCondition::on_missing_bean<          \
                    MissingBeanType>();                                      \
            shield::conditions::ConditionalBeanRegistry::instance()          \
                .register_conditional_bean<BeanType>(                        \
                    std::make_unique<shield::conditions::BeanCondition>(     \
                        std::move(condition)));                              \
            return 0;                                                        \
        }();

// Register bean conditional on bean existence
#define SHIELD_CONDITIONAL_ON_BEAN(BeanType, RequiredBeanType)            \
    static inline auto _shield_conditional_bean_##BeanType##__COUNTER__ = \
        []() {                                                            \
            auto condition = shield::conditions::BeanCondition::on_bean<  \
                RequiredBeanType>();                                      \
            shield::conditions::ConditionalBeanRegistry::instance()       \
                .register_conditional_bean<BeanType>(                     \
                    std::make_unique<shield::conditions::BeanCondition>(  \
                        std::move(condition)));                           \
            return 0;                                                     \
        }();

// Register bean conditional on profile
#define SHIELD_CONDITIONAL_ON_PROFILE(BeanType, profile)                     \
    static inline auto _shield_conditional_profile_##BeanType##__COUNTER__ = \
        []() {                                                               \
            auto condition =                                                 \
                std::make_unique<shield::conditions::ProfileCondition>(      \
                    profile);                                                \
            shield::conditions::ConditionalBeanRegistry::instance()          \
                .register_conditional_bean<BeanType>(std::move(condition));  \
            return 0;                                                        \
        }();