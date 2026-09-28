// Shield Kickstart - 一键体验入口
//
// 与 hello_world 相同的唯一 C++ 形态：业务全部在 Lua，C++ 只负责启动。

#include "shield/shield.hpp"

int main(int argc, char** argv) { return shield::run(argc, argv); }
