#!/usr/bin/env bash
# new_project.sh - 生成一个可运行的最小 Shield 游戏服务端工程。
#
# Usage:
#   scripts/new_project.sh <target-dir> [--force] [--shield-bin <shield>]
#
# 行为：
#   1. 从 templates/minimal_game 拷贝模板（config + scripts + README）；
#   2. 把 <APP_NAME> 占位符替换为目标目录名；
#   3. 若找到 shield 二进制（--shield-bin 或 ./build/bin/shield），
#      对生成的配置执行 --check-config 自检；
#   4. 打印下一步命令。
#
# 退出码：0 成功；1 用法错误；2 目标目录已存在（未 --force）；
#         3 配置自检失败。

set -euo pipefail

FORCE=0
SHIELD_BIN=""
TARGET=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --force) FORCE=1 ;;
        --shield-bin) [[ $# -ge 2 ]] || { echo "--shield-bin needs a path" >&2; exit 1; }
                       SHIELD_BIN="$2"; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) if [[ -n "$TARGET" ]]; then
               echo "unexpected argument: $1" >&2; exit 1
           fi
           TARGET="$1" ;;
    esac
    shift
done

if [[ -z "$TARGET" ]]; then
    echo "usage: $0 <target-dir> [--force] [--shield-bin <shield>]" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEMPLATE="$REPO_ROOT/templates/minimal_game"

if [[ ! -d "$TEMPLATE" ]]; then
    echo "template missing: $TEMPLATE" >&2
    exit 1
fi

if [[ -e "$TARGET" && "$FORCE" -eq 0 ]]; then
    echo "target exists: $TARGET (use --force to replace)" >&2
    exit 2
fi
if [[ -d "$TARGET" ]]; then
    rm -rf "$TARGET"
fi
mkdir -p "$TARGET"

APP_NAME="$(basename "$TARGET")"
cp -R "$TEMPLATE/." "$TARGET/"
# 模板里的占位符 → 工程名
if [[ "$(uname -s)" == "Darwin" ]]; then
    sed -i '' "s/<APP_NAME>/$APP_NAME/g" "$TARGET/config/app.yaml" "$TARGET/README.md"
else
    sed -i "s/<APP_NAME>/$APP_NAME/g" "$TARGET/config/app.yaml" "$TARGET/README.md"
fi

# 配置自检（shield 二进制可用时）
if [[ -z "$SHIELD_BIN" ]]; then
    for CAND in "$REPO_ROOT/build/bin/shield" "$REPO_ROOT/build-release/bin/shield"; do
        [[ -x "$CAND" ]] && SHIELD_BIN="$CAND" && break
    done
fi

CHECK_RESULT="skipped (shield binary not found; run --check-config yourself)"
if [[ -n "$SHIELD_BIN" && -x "$SHIELD_BIN" ]]; then
    if "$SHIELD_BIN" --check-config --config "$TARGET/config/app.yaml"; then
        CHECK_RESULT="ok"
    else
        echo "generated config failed --check-config" >&2
        echo "(common cause: the listener port 8100 is already in use on this" >&2
        echo " machine — the check performs a real bind; edit $TARGET/config/app.yaml)" >&2
        exit 3
    fi
fi

RUN_CMD="${SHIELD_BIN:-$REPO_ROOT/build/bin/shield}"
echo ""
echo "created game project: $TARGET (config check: $CHECK_RESULT)"
echo "next steps:"
echo "  1. run:    $RUN_CMD --config $TARGET/config/app.yaml"
echo "  2. connect (from the shield repo):"
echo "       python3 $REPO_ROOT/scripts/client_demo.py --port 8100"
echo "  3. iterate: edit $TARGET/scripts/echo.lua and $TARGET/config/app.yaml"
