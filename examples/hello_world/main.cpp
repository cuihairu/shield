// Shield Hello World - 最终 C++ 入口形态 (spec)
// 唯一需要写 C++ 的地方：启动入口

#include "shield/shield.hpp"

int main(int argc, char** argv) {
    return shield::run(argc, argv);  // 读 config/app.yaml，启动所有 actor，进入事件循环
}
