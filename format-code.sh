#!/bin/bash

# 代码格式化脚本
# 使用项目统一的格式化工具和配置

set -e

echo "🔧 Shield 代码格式化工具"
echo "========================"

# 检查是否在项目根目录
if [ ! -f ".clang-format" ]; then
    echo "❌ 错误：请在项目根目录运行此脚本"
    exit 1
fi

# 查找可用的 clang-format
CLANG_FORMAT=""
for cmd in clang-format-18 clang-format-17 clang-format-16 clang-format-15 clang-format-14 clang-format; do
    if command -v "$cmd" &> /dev/null; then
        CLANG_FORMAT="$cmd"
        break
    fi
done

if [ -z "$CLANG_FORMAT" ]; then
    echo "❌ 错误：未找到 clang-format 工具"
    echo "请安装 clang-format："
    echo "  macOS: brew install clang-format"
    echo "  Ubuntu: sudo apt-get install clang-format"
    exit 1
fi

echo "✅ 使用格式化工具: $CLANG_FORMAT"
$CLANG_FORMAT --version

# 格式化选项
FORMAT_CHECK_ONLY=false
if [ "$1" = "--check" ] || [ "$1" = "-c" ]; then
    FORMAT_CHECK_ONLY=true
    echo "🔍 仅检查格式，不修改文件"
else
    echo "🔄 格式化并修改文件"
fi

# 查找所有 C++ 文件
echo "📁 查找 C++ 文件..."
CPP_FILES=$(find include src -name "*.hpp" -o -name "*.cpp" 2>/dev/null | sort)

if [ -z "$CPP_FILES" ]; then
    echo "⚠️  未找到 C++ 文件"
    exit 0
fi

echo "📝 找到 $(echo "$CPP_FILES" | wc -l) 个文件"

# 格式化或检查文件
failed_files=""
for file in $CPP_FILES; do
    if [ "$FORMAT_CHECK_ONLY" = true ]; then
        # 仅检查格式
        if ! $CLANG_FORMAT --dry-run --Werror "$file" > /dev/null 2>&1; then
            echo "❌ 格式错误: $file"
            failed_files="$failed_files $file"
        else
            echo "✅ 格式正确: $file"
        fi
    else
        # 格式化文件
        echo "🔄 格式化: $file"
        $CLANG_FORMAT -i "$file"
    fi
done

# 结果报告
if [ "$FORMAT_CHECK_ONLY" = true ]; then
    if [ -n "$failed_files" ]; then
        echo ""
        echo "❌ 发现格式问题的文件："
        for file in $failed_files; do
            echo "   $file"
        done
        echo ""
        echo "💡 修复建议："
        echo "   ./format-code.sh  # 自动格式化所有文件"
        exit 1
    else
        echo ""
        echo "✅ 所有文件格式正确"
    fi
else
    echo ""
    echo "✅ 代码格式化完成"
    echo "💡 建议运行测试确保代码正常工作"
fi