// shield/tests/fs/test_file_watcher.cpp
#define BOOST_TEST_MODULE file_watcher_tests
#include <boost/test/unit_test.hpp>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdio>

#include "shield/fs/file_watcher.hpp"

using namespace shield::fs;

BOOST_AUTO_TEST_SUITE(file_watcher_suite)

// =====================================
// FileEvent 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_file_event_construction) {
    FileEvent event("/tmp/test.txt", FileEventType::Created);

    BOOST_CHECK_EQUAL(event.file_path, "/tmp/test.txt");
    BOOST_CHECK_EQUAL(event.event_type, FileEventType::Created);
    BOOST_CHECK(event.old_path.empty());
}

BOOST_AUTO_TEST_CASE(test_file_event_with_old_path) {
    FileEvent event("/tmp/new.txt", FileEventType::Moved, "/tmp/old.txt");

    BOOST_CHECK_EQUAL(event.file_path, "/tmp/new.txt");
    BOOST_CHECK_EQUAL(event.old_path, "/tmp/old.txt");
    BOOST_CHECK_EQUAL(event.event_type, FileEventType::Moved);
}

// =====================================
// FileWatcherFactory 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_file_watcher_factory_create_polling) {
    auto watcher = FileWatcherFactory::create_polling_watcher(
        std::chrono::milliseconds(100));

    BOOST_CHECK(watcher != nullptr);
    BOOST_CHECK(watcher->is_supported());
}

BOOST_AUTO_TEST_CASE(test_file_watcher_factory_create_best) {
    auto watcher = FileWatcherFactory::create_best_watcher(
        std::chrono::milliseconds(100));

    BOOST_CHECK(watcher != nullptr);
    BOOST_CHECK(watcher->is_supported());
}

// =====================================
// FileEventDispatcher 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_file_event_dispatcher_add_remove) {
    FileEventDispatcher dispatcher;

    int call_count = 0;
    auto handler_id = dispatcher.add_handler([&](const FileEvent&) {
        call_count++;
    });

    FileEvent event("/tmp/test.txt", FileEventType::Modified);
    dispatcher.dispatch(event);

    BOOST_CHECK_EQUAL(call_count, 1);

    dispatcher.remove_handler(handler_id);
    dispatcher.dispatch(event);

    BOOST_CHECK_EQUAL(call_count, 1);  // 不应增加
}

BOOST_AUTO_TEST_CASE(test_file_event_dispatcher_multiple_handlers) {
    FileEventDispatcher dispatcher;

    int count1 = 0, count2 = 0;

    dispatcher.add_handler([&](const FileEvent&) { count1++; });
    dispatcher.add_handler([&](const FileEvent&) { count2++; });

    FileEvent event("/tmp/test.txt", FileEventType::Modified);
    dispatcher.dispatch(event);

    BOOST_CHECK_EQUAL(count1, 1);
    BOOST_CHECK_EQUAL(count2, 1);
}

BOOST_AUTO_TEST_CASE(test_file_event_dispatcher_clear) {
    FileEventDispatcher dispatcher;

    int count = 0;
    dispatcher.add_handler([&](const FileEvent&) { count++; });

    BOOST_CHECK_EQUAL(dispatcher.handler_count(), 1);

    dispatcher.clear();
    BOOST_CHECK_EQUAL(dispatcher.handler_count(), 0);

    FileEvent event("/tmp/test.txt", FileEventType::Modified);
    dispatcher.dispatch(event);

    BOOST_CHECK_EQUAL(count, 0);
}

// =====================================
// FileWatchManager 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_file_watch_manager_singleton) {
    auto& manager1 = FileWatchManager::instance();
    auto& manager2 = FileWatchManager::instance();

    BOOST_CHECK(&manager1 == &manager2);
}

BOOST_AUTO_TEST_CASE(test_file_watch_manager_create_get) {
    auto& manager = FileWatchManager::instance();

    std::string watcher_name = "test_watcher_001";
    manager.remove_watcher(watcher_name);  // 清理可能存在的

    auto watcher = manager.create_watcher(watcher_name);
    BOOST_CHECK(watcher != nullptr);

    auto retrieved = manager.get_watcher(watcher_name);
    BOOST_CHECK(retrieved != nullptr);

    manager.remove_watcher(watcher_name);

    auto after_remove = manager.get_watcher(watcher_name);
    BOOST_CHECK(after_remove == nullptr);
}

BOOST_AUTO_TEST_CASE(test_file_watch_manager_get_names) {
    auto& manager = FileWatchManager::instance();

    std::string watcher_name = "test_watcher_002";
    manager.remove_watcher(watcher_name);

    manager.create_watcher(watcher_name);

    auto names = manager.get_watcher_names();
    bool found = std::find(names.begin(), names.end(), watcher_name) != names.end();

    BOOST_CHECK(found);

    manager.remove_watcher(watcher_name);
}

// =====================================
// FileEventDispatcher 异常处理测试
// =====================================

BOOST_AUTO_TEST_CASE(test_file_event_dispatcher_exception_handling) {
    FileEventDispatcher dispatcher;

    int good_count = 0;

    // 添加会抛出异常的处理器
    dispatcher.add_handler([](const FileEvent&) {
        throw std::runtime_error("Test exception");
    });

    // 添加正常处理器
    dispatcher.add_handler([&](const FileEvent&) {
        good_count++;
    });

    FileEvent event("/tmp/test.txt", FileEventType::Modified);

    // 不应抛出异常，正常处理器应该被调用
    BOOST_CHECK_NO_THROW(dispatcher.dispatch(event));
    BOOST_CHECK_EQUAL(good_count, 1);
}

BOOST_AUTO_TEST_SUITE_END()
