#define BOOST_TEST_MODULE ConfigReloadBinderTest
#include <boost/test/unit_test.hpp>

#include "shield/core/config_reload_binder.hpp"
#include "shield/core/service.hpp"

using namespace shield::core;

namespace {
class MockReloadableService : public ReloadableService {
public:
    std::string name() const override { return "mock_reloadable"; }
    void on_config_reloaded() override { reload_count_++; }
    int reload_count() const { return reload_count_; }

protected:
    void on_init(ApplicationContext&) override {}
    void on_start() override {}
    void on_stop() override {}

private:
    int reload_count_ = 0;
};

class NonReloadableService : public Service {
public:
    std::string name() const override { return "non_reloadable"; }

protected:
    void on_init(ApplicationContext&) override {}
    void on_start() override {}
    void on_stop() override {}
};
}  // namespace

BOOST_AUTO_TEST_SUITE(ConfigReloadBinderTests)

BOOST_AUTO_TEST_CASE(TestBindNonReloadableThrows) {
    ConfigReloadBinder binder;
    auto service = std::make_shared<NonReloadableService>();
    BOOST_CHECK_THROW(binder.bind<int>(service), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
