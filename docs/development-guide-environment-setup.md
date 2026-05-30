# 开发环境搭建

### 1. 安装基础工具

#### Ubuntu/Debian

```bash
sudo apt update
sudo apt install -y build-essential cmake git curl zip unzip tar pkg-config
```

#### macOS

```bash
xcode-select --install
brew install cmake git curl zip unzip tar pkg-config
```

#### Windows

安装 Visual Studio 2019+（含 C++ CMake 工具），或使用 MinGW。

### 2. 安装 vcpkg 包管理器

```bash
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg

./bootstrap-vcpkg.sh        # Linux/macOS
# bootstrap-vcpkg.bat       # Windows

export VCPKG_ROOT=/path/to/vcpkg
```

### 3. 克隆并构建 Shield

```bash
git clone https://github.com/cuihairu/shield.git
cd shield

# 一键构建
./build.sh debug            # Linux/macOS
# build.bat debug            # Windows
```

或手动构建：

```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build --parallel $(nproc)
```
