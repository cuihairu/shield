# 开发环境搭建

### 1. 安装基础工具

#### Ubuntu/Debian
```bash
# 更新包管理器
sudo apt update

# 安装编译工具
sudo apt install -y build-essential cmake git curl zip unzip tar
sudo apt install -y pkg-config autoconf automake libtool

# 安装 GCC 10+ (如果默认版本不够)
sudo apt install -y gcc-10 g++-10
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 100
```

#### CentOS/RHEL
```bash
# 安装开发工具
sudo yum groupinstall -y "Development Tools"
sudo yum install -y cmake git curl zip unzip tar
sudo yum install -y pkgconfig autoconf automake libtool

# 启用 PowerTools 仓库 (CentOS 8)
sudo yum config-manager --set-enabled powertools
```

#### macOS
```bash
# 安装 Xcode 命令行工具
xcode-select --install

# 安装 Homebrew (如果未安装)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 安装必需工具
brew install cmake git curl zip unzip tar pkg-config autoconf automake libtool
```

### 2. 安装 vcpkg 包管理器

```bash
# 克隆 vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg

# 编译 vcpkg
./bootstrap-vcpkg.sh  # Linux/macOS
# ./bootstrap-vcpkg.bat  # Windows

# 设置环境变量 (添加到 ~/.bashrc 或 ~/.zshrc)
export VCPKG_ROOT=/path/to/vcpkg
export PATH=$VCPKG_ROOT:$PATH

# 集成到系统
./vcpkg integrate install
```

### 3. 克隆并构建 Shield

```bash
# 克隆项目
git clone https://github.com/your-repo/shield.git
cd shield

# 创建构建目录
mkdir build && cd build

# 配置项目
cmake -B . -S .. \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug

# 编译项目
cmake --build . --parallel $(nproc)

# 运行测试
ctest -V
```