# 编译说明：纯 VS Code 离线方案（arduino-cli）

`AI_KEY_v3/AI_KEY_v3.ino` 已可完整编译通过，**全程无需 Arduino IDE，无需联网**。

```
Sketch uses 320174 bytes (24%) of program storage space. Maximum is 1310720 bytes.
Global variables use 23168 bytes (7%) of dynamic memory, leaving 304512 bytes.
```

产物：`build\out\AI_KEY_v3.ino.bin` / `.elf` / `.map` / `.merged.bin`（均在仓库内）

---

## 1. 目录约定

| 路径 | 作用 |
|---|---|
| `F:\Git\arduino-cli\arduino-cli.exe` | arduino-cli 1.5.1 |
| `F:\Git\Arduino15\` | 离线平台与工具链（esp32 core 3.3.11 + esp-x32/esp-rv32 2601 + esp32s3-libs + esptool_py 5.3.1） |
| `F:\Git\arduino-sketchbook\libraries\PN532-arduino\` | Seeed PN532（旧版，代码所依赖的版本） |
| `F:\Git\arduino-sketchbook\libraries\NDEF\` | Seeed NDEF 1.1.0（旧版） |
| `F:\Git\CrX-AI-assistant\build\` | 编译输出与增量缓存（`build\AI_KEY_v3\` 中间产物+缓存，`build\out\` 最终产物），已 gitignore，可随时删除重建 |
| [`arduino-cli.yaml`](./arduino-cli.yaml) | 工程内配置，指向以上路径 |

> **为什么必须在 `F:\Git` 而不是 `F:\Downloads` 或 OneDrive 里？**
> ESP32 core 3.x 的 `gen_esp32part.exe`、`espota.exe`、`flasher.exe`、`bin_signing.exe` 是 PyInstaller 单文件包，
> 启动时要在自身目录解压临时文件。Windows 会阻止从 **Downloads** 目录运行的这类程序解压，
> 报 `[PYI-xxxx:ERROR] Could not create temporary directory!`。OneDrive 目录则会同步 6.8 GB 编译产物。

## 2. 日常用法

`Ctrl+Shift+B` → 编译 `AI_KEY_v3`（默认任务，见 [`.vscode/tasks.json`](./.vscode/tasks.json)）。

其他任务：编译当前 sketch、清理缓存、上传、串口监视器、列出开发板。

命令行等价：

```powershell
F:\Git\arduino-cli\arduino-cli.exe --config-file arduino-cli.yaml compile `
  --fqbn esp32:esp32:esp32s3:FlashSize=16M `
  --build-path build\AI_KEY_v3 --output-dir build\out AI_KEY_v3
```

上传（先确认串口号）：

```powershell
F:\Git\arduino-cli\arduino-cli.exe --config-file arduino-cli.yaml upload `
  --fqbn esp32:esp32:esp32s3:FlashSize=16M --input-dir build\out COM5
```

> **所有产物只落在仓库内的 `build\`**：`--build-path` 同时是中间产物和增量缓存的位置，`--output-dir` 是最终产物，两者都指到 `build\`。
> 只要每次都带上这两个参数，arduino-cli 就不会在仓库外写任何东西（1.5.1 已无 `build.cache_directory` 配置项）。
> 仓库外只有只读的工具链与库：`F:\Git\arduino-cli`、`F:\Git\Arduino15`、`F:\Git\arduino-sketchbook`。
> （`%TEMP%` 里仍会有 PyInstaller 的瞬时解压文件，那是 Windows 临时目录，不是产物。）

## 3. 仓库迁移到 `F:\Git` 之后

仓库内的 [`arduino-cli.yaml`](./arduino-cli.yaml) 用的是绝对路径，迁移后**不用改**（工具链仍在 `F:\Git\Arduino15`）。
只需把 `arduino-cli.yaml` 里的 `directories.data` / `user` 保持指向 `F:\Git\...` 即可。

若换机器，重建步骤：

1. 把整套 `Arduino15`（或至少 `packages\esp32\hardware\esp32\3.3.11`、`packages\esp32\tools\*`、`packages\arduino\tools\dfu-util`）放到 Downloads **之外**的目录。
2. `arduino-cli.yaml` 的 `directories.data` 指向它，`directories.user` 指向一个 sketchbook。
3. 把 `PN532-arduino` 与 `NDEF` 放进 `<sketchbook>\libraries\`。
4. `arduino-cli core list` 应显示 `esp32:esp32 3.3.11` 且无下载动作。

## 4. 库版本的关键坑

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

## 5. 已放弃的方案：PlatformIO

`platformio.ini` / `src/main.cpp` / `lib/` 是 PlatformIO 路线的遗留物，**当前不可用**：
`dl.registry.platformio.org` 在本机只有 ~12 KB/s，`framework-arduinoespressif32` 始终下不完；
VS Code 的 PlatformIO 扩展还会常驻 `load_build_metadata` 进程抢占 `~/.platformio` 全局锁，触发 `LockFileTimeoutError`。
PlatformIO 路线遗留的 `.tools/`、`.arduino/` 已删除。如确认不再回退，可继续删除 `platformio.ini`、`src/`、`lib/`、`.pio/`。
（注意 `lib/` 里是**新版** Seeed 库，与本文第 4 节描述的旧库不兼容，别混用。）

## 6. 尚未验证

- 只编译过 `AI_KEY_v3`。根目录的 `AI_KEY.ino`、`AI_KEY_v2.ino` 是散文件，arduino-cli 要求 `名字\名字.ino` 结构，需各自放进同名文件夹才能编译。
- 未做过上板烧录与串口实测（当前机器只枚举到 COM1）。
- `SHARED_KEY` 仍是公开测试向量 `00 01 … 0f`，见 [`SECURITY-REVIEW.md`](./SECURITY-REVIEW.md)。
