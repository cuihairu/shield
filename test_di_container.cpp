#include <cassert>
#include <iostream>
#include <string>

#include "shield/di/di.hpp"

using namespace shield::di;

// 定义服务接口
SHIELD_INTERFACE(IUserRepository)
virtual std::string findUserById(int id) = 0;
virtual void saveUser(int id, const std::string& name) = 0;
SHIELD_END_INTERFACE

SHIELD_INTERFACE(IUserService)
virtual std::string getUserInfo(int id) = 0;
virtual void createUser(int id, const std::string& name) = 0;
SHIELD_END_INTERFACE

// 实现类
SHIELD_REPOSITORY UserRepository SHIELD_IMPLEMENTS(IUserRepository){
    public : std::string findUserById(int id)
        override{return "User_" + std::to_string(id);
}

void saveUser(int id, const std::string& name) override {
    std::cout << "Saving user " << id << ": " << name << std::endl;
}
}
;

SHIELD_SERVICE UserService SHIELD_IMPLEMENTS(IUserService) {
private:
    SHIELD_INJECT(IUserRepository) userRepository_;

public:
    // 声明依赖注入
    SHIELD_CONSTRUCTOR_INJECT(IUserRepository);

    // 构造函数
    UserService(SHIELD_INJECT(IUserRepository) userRepository)
        : userRepository_(userRepository) {
        std::cout << "UserService created with injected repository"
                  << std::endl;
    }

    std::string getUserInfo(int id) override {
        return "Info: " + userRepository_->findUserById(id);
    }

    void createUser(int id, const std::string& name) override {
        userRepository_->saveUser(id, name);
    }
};

SHIELD_CONTROLLER UserController {
private:
    SHIELD_INJECT(IUserService) userService_;

public:
    SHIELD_CONSTRUCTOR_INJECT(IUserService);

    UserController(SHIELD_INJECT(IUserService) userService)
        : userService_(userService) {
        std::cout << "UserController created with injected service"
                  << std::endl;
    }

    void handleGetUser(int id) {
        std::string info = userService_->getUserInfo(id);
        std::cout << "GET /users/" << id << " -> " << info << std::endl;
    }

    void handleCreateUser(int id, const std::string& name) {
        userService_->createUser(id, name);
        std::cout << "POST /users/" << id << " created" << std::endl;
    }
};

// 简单服务（无依赖）
SHIELD_SERVICE SimpleService{public : void doSomething(){
    std::cout << "SimpleService doing something..." << std::endl;
}
}
;

int main() {
    std::cout << "=== Shield DI Container Test ===" << std::endl;

    try {
        // 创建 DI 容器
        auto container = create_container();

        // 注册服务
        std::cout << "\n1. Registering services..." << std::endl;
        SHIELD_REGISTER_SINGLETON(*container, IUserRepository, UserRepository);
        SHIELD_REGISTER_SINGLETON(*container, IUserService, UserService);
        SHIELD_REGISTER_TRANSIENT(*container, UserController, UserController);

        // 注册简单服务（无依赖）
        container->add_singleton<SimpleService, SimpleService>();

        std::cout << "Registered " << container->service_count() << " services"
                  << std::endl;

        // 测试服务解析
        std::cout << "\n2. Resolving services..." << std::endl;

        // 获取单例服务
        auto simpleService = container->get_service<SimpleService>();
        simpleService->doSomething();

        // 获取有依赖的服务
        auto userController1 = container->get_service<UserController>();
        auto userController2 = container->get_service<UserController>();

        std::cout << "\n3. Testing service calls..." << std::endl;
        userController1->handleGetUser(123);
        userController1->handleCreateUser(456, "John Doe");

        // 测试单例 vs 瞬态
        std::cout << "\n4. Testing service lifetimes..." << std::endl;
        auto repo1 = container->get_service<IUserRepository>();
        auto repo2 = container->get_service<IUserRepository>();
        std::cout << "Repository singleton test: "
                  << (repo1 == repo2 ? "PASS" : "FAIL") << std::endl;

        std::cout << "Controller transient test: "
                  << (userController1 != userController2 ? "PASS" : "FAIL")
                  << std::endl;

        // 测试服务注册检查
        std::cout << "\n5. Testing service registration checks..." << std::endl;
        std::cout << "IUserService registered: "
                  << container->is_registered<IUserService>() << std::endl;
        std::cout << "UnknownService registered: "
                  << container->is_registered<int>() << std::endl;

        std::cout << "\n=== All tests completed successfully! ===" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}