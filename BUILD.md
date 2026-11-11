# 编译说明：纯 VS Code 离线方案（arduino-cli）

`AI_KEY_v3/AI_KEY_v3.ino` 已可完整编译通过，**全程无需 Arduino IDE，无需联网**。

```
Sketch uses 320174 bytes (24%) of program storage space. Maximum is 1310720 bytes.
Global variables use 23168 bytes (7%) of dynamic memory, leaving 304512 bytes.
```

产物：`build\AI_KEY_v3\AI_KEY_v3.ino.bin` / `.elf` / `.map` / `.merged.bin`（均在仓库内）

---

## 1. 目录约定

| 路径 | 作用 |
|---|---|
| `F:\Git\arduino-cli\arduino-cli.exe` | arduino-cli 1.5.1 |
| `F:\Git\Arduino15\` | 离线平台与工具链（esp32 core 3.3.11 + esp-x32/esp-rv32 2601 + esp32s3-libs + esptool_py 5.3.1） |
| `CrX-AI-assistant\libraries\PN532\` | Seeed PN532 1.0.0（旧版，代码所依赖的版本），**git 子模块**（`Seeed-Studio/PN532`，钉到 commit `2c47f5c`） |
| `CrX-AI-assistant\libraries\NDEF\` | Seeed NDEF 1.1.0（旧版），**git 子模块**（`don/NDEF`，tag `1.1.0`） |
| `CrX-AI-assistant\build\` | 编译输出与增量缓存（`build\AI_KEY_v3\` 中间产物 + 最终产物），已 gitignore，可随时删除重建 |
| [`arduino-cli.yaml`](./arduino-cli.yaml) | 工程内配置，指向以上路径 |
| [`.vscode/gen-intellisense.ps1`](./.vscode/gen-intellisense.ps1) | 生成 IntelliSense 配置，见第 3 节 |

> **库为什么都是 git 子模块？**
> - 两个库都钉到**具体 commit / tag**，仓库本身不存内容、不占体积（符合"子模块不占 git 空间"的诉求）。
> - `libraries\NDEF`：`don/NDEF` @ tag `1.1.0`。
> - `libraries\PN532`：`Seeed-Studio/PN532` @ commit `2c47f5c`（`arduino` 分支上**最后一个 1.0.0 提交**，`src/` 平铺布局、带 `#ifdef NFC_INTERFACE_SPI` 守卫，与本项目已验证的 1.0.0 内容逐字节一致）。
>   - **不要钉到 tag `v1.0`**（那是 2020 年老提交，还是"按接口分目录"的旧 Seeed 布局，arduino-cli 解析不了 `#include "PN532_SPI.h"`）；
>   - **不要钉到 1.1.0**（`e44820e` 起移除了 `#ifdef` 守卫，sketch 里的 `#define NFC_INTERFACE_SPI` + `#include .cpp` 会变重复定义，见第 5 节）。
> - 全新克隆后必须执行 `git submodule update --init`（或 `git clone --recurse-submodules`）拉取两个子模块（见第 4 节）。

## 2. 日常用法

`Ctrl+Shift+B` → 编译 `AI_KEY_v3`（默认任务，见 [`.vscode/tasks.json`](./.vscode/tasks.json)）。

其他任务：编译当前 sketch、刷新 IntelliSense 配置、清理缓存、上传、串口监视器、列出开发板。

命令行等价：

```powershell
F:\Git\arduino-cli\arduino-cli.exe --config-file arduino-cli.yaml compile `
  --fqbn esp32:esp32:esp32s3:FlashSize=16M `
  --build-path build\AI_KEY_v3 AI_KEY_v3
```

上传（先确认串口号）：

```powershell
F:\Git\arduino-cli\arduino-cli.exe --config-file arduino-cli.yaml upload `
  --fqbn esp32:esp32:esp32s3:FlashSize=16M --build-path build\AI_KEY_v3 COM5
```

> **所有产物只落在仓库内的 `build\`**：`--build-path` 同时是中间产物、增量缓存和最终产物的位置。
> **不要加 `--output-dir` 或 `-e`**：esp32 core 的 `savehex` 钩子（`platform.txt` 第 213-239 行）会按 `{sketch_path}\build\<vendor.arch.board>\` 再复制一份 bin/elf/map/sdkconfig，产物就散成两处了。
> 只要每次都带上 `--build-path`，arduino-cli 就不会在仓库外写任何东西（1.5.1 已无 `build.cache_directory` 配置项）。
> 仓库外只有只读的工具链与库：`F:\Git\arduino-cli`、`F:\Git\Arduino15`、`F:\Git\arduino-sketchbook`。
> （`%TEMP%` 里仍会有 PyInstaller 的瞬时解压文件，那是 Windows 临时目录，不是产物。）

## 3. IntelliSense（`.ino` 报「检测到 #include 错误」）

`.vscode/c_cpp_properties.json` 是**生成物，不要手改**，由 [`.vscode/gen-intellisense.ps1`](./.vscode/gen-intellisense.ps1) 生成。

为什么需要生成器：arduino-cli 产出的 `compile_commands.json` cpptools 用不了——

* 它只登记 `build\AI_KEY_v3\sketch\AI_KEY_v3.ino.cpp`，没有 `AI_KEY_v3.ino`，所以总提示「未找到 AI_KEY_v3.ino，改用 includePath」；
* 编译参数里的 include/define 全藏在 `@flags/includes`、`@flags/defines` 这类 **GCC 响应文件**里，cpptools 不展开；
* 370 个 IDF 头目录写成 `-iprefix <根>` + `-iwithprefixbefore <相对路径>`，cpptools 也不处理。

生成器读取真实编译参数，展开响应文件、还原 `-iwithprefixbefore`、补上交叉编译器的系统头目录，最后写出 385 个 include 目录 + 55 个宏。

配置里还有两项不是从 include 参数来的，但同样关键：

* **`compilerPath`** 指向 `esp-x32\2601\bin\xtensa-esp32s3-elf-g++.exe`。不写它，cpptools 会退回 `C_Cpp.default.compilerPath`（本机是宿主 MinGW g++），预定义宏就变成 `__x86_64__`、`_WIN64`、`__SIZEOF_POINTER__=8`。IDF / mbedTLS / TLSF 的头文件大量按这些宏选分支，IntelliSense 会去解析目标上根本不存在的 64 位路径。
* **`cppStandard`** 由生成器从编译参数里取**最后一个** `-std=` 推导（GCC 语义：后者覆盖前者）。esp32 core 会传两次，当前是 `-std=gnu++2b … -std=gnu++2a`，所以实际是 C++20，配置写 `gnu++20`。cpptools 只认 `c++98`…`c++26` / `gnu++98`…`gnu++26`，GCC 的 `2a`/`2b` 别名要先映射过去。

另外必须保留 `__XTENSA__` 这组目标端宏（生成器自动注入）。否则 `xtensa/config/core.h` 会走 `#else` 分支去 `#include "../hal.h"`，`SPI.h` 那行就会报 1696「无法打开源文件 "../hal.h"」——这是假错，真编译器走的是 `#ifdef __XTENSA__` 分支。设了 `compilerPath` 后 cpptools 本来也能问到这些宏，显式注入是双保险。

> **改生成器时注意**：`gen-intellisense.ps1` 必须保持**纯 ASCII**。Windows PowerShell 5.1 读无 BOM 的 `.ps1` 按系统 ANSI（GBK）解码，UTF-8 中文注释会被解成乱码，其中某些字节恰好凑成 `"` 或 `\`，把注释行变成字符串边界，整个脚本语法崩掉（报「语句块或类型定义中缺少右 `}`」）。同理，输出 JSON 是手写序列化而不是 `ConvertTo-Json`，因为 PS 5.1 和 7 的缩进、冒号后空格不同，会导致整文件 diff。

配置里全是本机绝对路径，换机器或升级工具链后必须重新生成。

改完配置后如果红线没消失，执行命令面板的 **C/C++: Reset IntelliSense Database**（或重新加载窗口）。

## 4. 仓库迁移到 `F:\Git` 之后

仓库内的 [`arduino-cli.yaml`](./arduino-cli.yaml) 用的是绝对路径，迁移后**不用改**（工具链仍在 `F:\Git\Arduino15`）。
只需把 `arduino-cli.yaml` 里的 `directories.data` / `user` 保持指向 `F:\Git\...` 即可。

若换机器，重建步骤：

1. 把整套 `Arduino15`（或至少 `packages\esp32\hardware\esp32\3.3.11`、`packages\esp32\tools\*`、`packages\arduino\tools\dfu-util`）放到 Downloads **之外**的目录。
2. 克隆仓库并初始化子模块：`git clone --recurse-submodules <repo>`（或克隆后 `git submodule update --init`）。`libraries\PN532` 与 `libraries\NDEF` 都是子模块，需联网拉取。
3. `arduino-cli.yaml` 的 `directories.data` 指向 `Arduino15`，`directories.user` 指向**本仓库根**（`libraries\` 所在目录）——见当前仓库内 yaml。
4. `arduino-cli core list` 应显示 `esp32:esp32 3.3.11` 且无下载动作。
5. 重新生成 IntelliSense（`Ctrl+Shift+B` 编译后自动跑 `gen-intellisense.ps1`，或命令面板「刷新 IntelliSense 配置」）。

> 子模块注意：`libraries\PN532` 与 `libraries\NDEF` 的内容**不要直接改动**（改动会在 `git status` 里显示 submodule dirty）；要升级版本 = 改 `.gitmodules` 钉的 tag/commit 再 `git submodule update`。PN532 钉的是 commit `2c47f5c`（最后 1.0.0），NDEF 钉的是 tag `1.1.0`。

## 5. 库版本的关键坑

工程用的是 **旧版** Seeed 库，`PN532Interface.h` 里**没有** `NFC_INTERFACE_*` 默认宏，
`PN532_SPI.cpp` 整个文件包在 `#ifdef NFC_INTERFACE_SPI` 内，库自带的 `.o` 是空的。
所以 sketch 里必须保留（见 [`AI_KEY_v3.ino`](./AI_KEY_v3/AI_KEY_v3.ino) 第 52 行附近）：

```cpp
#define NFC_INTERFACE_SPI          // 必须在包含 PN532_SPI.cpp 之前
#include "PN532_SPI.h"
#include "PN532_SPI.cpp"           // 否则链接期找不到 PN532_SPI 的实现
```

GitHub 上的**新版** Seeed 库已在头文件里默认这些宏，那种情况下再 `#include .cpp` 会造成重复定义。
**换库版本时必须同步改这段。**

## 6. 已放弃的方案：PlatformIO

`platformio.ini`、`src/main.cpp`、`.pio/`、`.tools/`、`.arduino/` 均已删除，残留的 `lib/` 目录也已移除。

放弃原因：`dl.registry.platformio.org` 在本机只有 ~12 KB/s，`framework-arduinoespressif32` 始终下不完；
VS Code 的 PlatformIO 扩展还会常驻 `load_build_metadata` 进程抢占 `~/.platformio` 全局锁，触发 `LockFileTimeoutError`。

旧 `lib/` 里是**新版** Seeed 库，与第 5 节描述的旧库不兼容，故已删除。当前库布局（全部为 git 子模块）：

| 位置 | 库名（`library.properties`） | 版本 | 形式 |
|---|---|---|---|
| `libraries/PN532` | `PN532-Arduino` | **1.0.0** | git 子模块（`Seeed-Studio/PN532` @ commit `2c47f5c`） |
| `libraries/NDEF` | `NDEF` | 1.1.0 | git 子模块（`don/NDEF` @ tag `1.1.0`） |

当前编译读取的是仓库内 `libraries/`（`arduino-cli.yaml` 的 `directories.user` 指向仓库根），
`F:\Git\arduino-sketchbook` 不再被引用。

## 7. 尚未验证

- 只编译过 `AI_KEY_v3`。根目录的 `AI_KEY.ino`、`AI_KEY_v2.ino` 是散文件，arduino-cli 要求 `名字\名字.ino` 结构，需各自放进同名文件夹才能编译。
- 未做过上板烧录与串口实测（当前机器只枚举到 COM1）。
- `SHARED_KEY` 仍是公开测试向量 `00 01 … 0f`，见 [`SECURITY-REVIEW.md`](./SECURITY-REVIEW.md)。
