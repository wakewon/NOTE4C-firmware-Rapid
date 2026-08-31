# Youn Ink Four Color

这是一个面向 ESP32-S3 墨水屏设备的个人 AI 助手项目。当前主线由三部分组成：ESP32 固件、Python 后端服务、以及图片/待办/设备管理页面。

项目重点不是一个通用 npm 包，而是一套可以真实运行在墨水屏设备上的系统：语音对话、TTS 播放、待办同步、天气/新闻/日历/电子书/相册页面、AP 传图、OTA 固件管理，以及适配四色屏的 RawDraw UI。

## 2BP 四色图像链路

![Youn Ink Four Color 2BP BWRY architecture](README-2bp-architecture.png)

相册图片可由 PC/NAS 管理端或设备 AP 页面进入服务端，转换为 `2BP BWRY`（黑、白、红、黄）后通过 Wi-Fi 推送到 ESP32-S3 四色墨水屏。本仓库的 2BP 四色链路与 NOTE4 的 4BP 黑白灰阶相册独立维护：面板颜色、像素格式和刷新驱动均不同。

## 当前状态

- 后端已经切换为 `server/` 下的 Python 服务，根目录旧 Node `scripts/` 已删除。
- 固件主界面使用 RawDraw 渲染，默认按四色屏设计，同时保留 1bpp 黑白屏兼容。
- 主题暂时只保留一个默认视觉方向：偏任天堂感的四色主题，强调红、黄、黑、白的语义使用。
- 图片传输支持 1bpp 黑白与 2bpp 四色 BWRY 两种格式。
- 根目录 `.gitignore` 已排除构建产物、日志、pid、数据库、本地配置和密钥文件。

## 目录结构

```text
.
├── firmware/        ESP32-IDF 固件，RawDraw UI、页面渲染、屏幕驱动、AP 传图
├── server/          Python 后端，WebSocket 对话、TTS、Discovery、图片推送、OTA API
├── frontend/        管理前端源码，使用独立的 package/pnpm 工作流
├── docs/            历史设计文档和实现记录
├── documents/       项目资料
└── package.json     仅保留仓库级辅助命令，不再作为旧 Node 服务入口
```

注意：`firmware/scripts/` 和 `frontend/scripts/` 仍然有用，分别属于固件工具和前端工具；删除的是根目录历史遗留的 `scripts/`。

## 后端服务

后端入口是 `server/llmserve.py`，推荐通过 `server/start.sh` 管理。服务默认端口：

| 端口 | 协议 | 用途 |
| --- | --- | --- |
| `9001` | WebSocket | ESP32 语音、LLM、TTS、同步消息 |
| `8766` | UDP | 设备发现 |
| `8766` | HTTP | 图片推送、设备图片管理、OTA API |
| `8090` | HTTP | 独立管理服务，可选 |

### 安装依赖

```bash
cd server
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

### 启动服务

```bash
export DASHSCOPE_API_KEY=你的百炼APIKey
cd server
./start.sh start
```

常用命令：

```bash
cd server
./start.sh status
./start.sh logs
./start.sh restart
./start.sh stop
```

也可以从仓库根目录调用：

```bash
npm run server:start
npm run server:status
npm run server:logs
```

### 本地模拟设备

```bash
cd server
python3 mock_client.py --server ws://127.0.0.1:9001
```

## 图片和设备管理

图片 HTTP API 由 `server/push_image.py` 挂到 `8766` 端口。它支持：

- 上传图片文件并转换后推送到设备。
- 选择 `1bpp` 黑白格式或 `2bpp` 四色 BWRY 格式。
- 查询设备图片列表。
- 删除设备图片。
- 上传固件并提供 OTA 下载。

常用接口：

```bash
curl http://localhost:8766/api/status
curl http://localhost:8766/api/images
```

上传图片示例：

```bash
curl -X POST http://localhost:8766/api/upload_image \
  -F "image=@/path/to/photo.jpg" \
  -F "format=bwry2bpp" \
  -F "title=照片标题"
```

设备进入 AP 传图模式后，手机连接设备热点并访问：

```text
http://192.168.4.1
```

## 固件

固件位于 `firmware/`，基于 ESP-IDF。默认面向 ZecTrix ESP32-S3 4.2 寸墨水屏，支持四色 BWRY 屏，也保留 1bpp 黑白屏配置。

### 编译

本机已验证的环境为 macOS（Apple Silicon）、ESP-IDF `v6.0`（即
`6.0.0`）和 Python `3.12`。工程的 `dependencies.lock` 同样锁定 IDF
`6.0.0`；不要直接改用 `v6.0.2`，其 SPI 私有接口与当前锁定的
`espressif/esp_cam_sensor 1.5.2` 不兼容。

首次安装环境：

```bash
brew install python@3.12 cmake ninja ccache dfu-util

mkdir -p ~/Developer/esp/v6.0
git clone --branch v6.0 --depth 1 --recursive \
  https://github.com/espressif/esp-idf.git \
  ~/Developer/esp/v6.0/esp-idf

export PATH=/opt/homebrew/opt/python@3.12/libexec/bin:$PATH
export IDF_TOOLS_PATH=~/Developer/esp/v6.0/.espressif
cd ~/Developer/esp/v6.0/esp-idf
./install.sh esp32s3
```

每个新终端先激活环境，再编译。**推荐用这一条**，它会自动找到 IDF checkout、旁边的
`.espressif` 工具目录、以及按已安装 venv 名字反推出的正确 Python 版本——不用记
硬编码路径，换机器、换 Python 版本、重装工具链都不用改：

```bash
eval "$(python3 firmware/scripts/espimage.py)"

cd firmware
idf.py build
```

`eval` 是必须的：这条命令打印的是要在当前 shell 里执行的 `export`/`source` 语句，
`eval` 才会真的把它们跑在当前 shell（而不是子进程）里，`idf.py` 之后才能一直留在
PATH 上，可以用于 `idf.py menuconfig` / `monitor` / `size` 等任意手动命令。IDF/工具
目录不在默认位置时可加 `--idf-path` / `--idf-tools-path`。

<details>
<summary>手动激活（不推荐，路径写死、Python 版本不匹配就会失败）</summary>

```bash
export PATH=/opt/homebrew/opt/python@3.12/libexec/bin:$PATH
export IDF_TOOLS_PATH=~/Developer/esp/v6.0/.espressif
source ~/Developer/esp/v6.0/esp-idf/export.sh

cd firmware
idf.py build
```

上面两个 `export` 都是必需的，缺任何一个 `export.sh` 都会「成功」但不把 `idf.py` 放进 PATH：

- 缺 `IDF_TOOLS_PATH` → 回退去找 `~/.espressif`，那里什么都没有
- 缺 `PATH=python@3.12` → venv 目录名按当前 `python3` 版本拼（`idf6.0_py3.12_env`），
  用 3.14 就会去找一个从没创建过的 `idf6.0_py3.14_env`

两种情况报的都是「virtual environment not found」，容易误判成没装。这也是为什么
上面推荐用 `espimage.py`：它自动探测这两项，不需要每次改代码里的硬编码路径。

</details>

编译和刷写本身不需要手动激活——`package.py` / `flash.py` 内部各自处理好了环境：

- `package.py` 需要跑 `idf.py`，靠 `firmware/scripts/espimage.py` 里的激活逻辑
  （自动找 IDF、找旁边的 `.espressif`、按已安装的 venv 名字反推该用哪个 Python）
  在子进程里 `source export.sh`，不依赖当前 shell 是否手动激活过。
- `flash.py` 需要的是 `esptool`/`pyserial`，和 ESP-IDF 环境是两回事——它会在仓库
  根目录自动建一个独立的 `.venv-esptool`（同 `tools/bwry` 用 `.venv-imgtool` 一样
  的思路，不污染系统 Python），首次运行自动创建并装依赖，之后直接复用。

只有在直接手敲 `idf.py ...`（比如 `idf.py menuconfig`）时才需要上面 `eval "$(python3
firmware/scripts/espimage.py)"` 这步手动激活。

### 编译 + 打包：`package.py`

`idf.py build` 只生成 `xiaozhi.bin`（OTA/application 镜像），**不会**顺带生成
`merged-binary.bin`（整包镜像）。只跑 `build` 的话，`xiaozhi.bin` 是新的，
`merged-binary.bin` 还是旧的——刷了旧的合并镜像会得到一个看起来能跑、但缺少
最近改动的固件（这坑已经踩过一次）。

`firmware/scripts/package.py` 把这步锁死：每次都跑 `idf.py build && idf.py
merge-bin`，并且校验两个镜像里的 application 构建时间戳一致，不一致就报错、
不产出「成功」。

```bash
python3 firmware/scripts/package.py                    # 编译 + 生成两个镜像
python3 firmware/scripts/package.py --clean             # 先 fullclean 再全量重建
python3 firmware/scripts/package.py --quiet              # 只在失败时打印构建日志
python3 firmware/scripts/package.py --out firmware/releases   # 额外拷贝一份带版本号/时间戳的副本
python3 firmware/scripts/package.py --idf-path <dir> --idf-tools-path <dir>  # 环境不在默认位置时
```

典型输出：

```text
ESP-IDF: ~/Developer/esp/v6.0/esp-idf
  tools: ~/Developer/esp/v6.0/.espressif
built in 5s
  xiaozhi.bin            2.72 MB   app built Aug 13 2026 00:19:12
  merged-binary.bin      2.85 MB   app built Aug 13 2026 00:19:12
  both images carry the same application ✓

flash with: python3 firmware/scripts/flash.py
```

两个产物的用途：

| 文件 | 用途 | 刷写偏移 |
| --- | --- | --- |
| `build/merged-binary.bin` | 首次刷写、救砖、网页/GUI 整包刷写 | `0x0` |
| `build/xiaozhi.bin` | OTA，或已具备正确 bootloader/分区表时单独更新应用 | `0x20000`（即 `ota_0`） |

### 刷写：`flash.py`

`firmware/scripts/flash.py` 取代了旧的 `flash_keep_nvs.py`（已 `git mv`，接口不兼容，
下面是新用法）。默认交互式：列出串口、标出像 ESP32 的设备、显示两个镜像里各是
哪次构建、刷写前会问一遍是否继续；每个选项也都能用参数直接给出，可用于脚本化。

```bash
python3 firmware/scripts/flash.py                                  # 交互式，默认保留配网
python3 firmware/scripts/flash.py --list                           # 只列出串口和镜像信息，不刷写
python3 firmware/scripts/flash.py --port /dev/cu.usbmodem14101 --image merged --yes
python3 firmware/scripts/flash.py --image app                      # 只刷应用分区（本就不碰 nvs）
python3 firmware/scripts/flash.py --no-keep-wifi                   # 允许清空配网
python3 firmware/scripts/flash.py --backup-only                    # 只备份 nvs/phy_init，不刷写
python3 firmware/scripts/flash.py --restore firmware/build/nvs_backup/nvs_<时间戳>.bin
```

保留配网的原理：`merged-binary.bin` 从 `0x0` 写入约 3 MB，正好盖住 `nvs @
0x9000`——WiFi 凭据、设备密钥、配网状态都在那里。`--keep-wifi`（默认开启）会在
写入前把 `nvs`（和 `phy_init`）读出来存到 `firmware/build/nvs_backup/`，写完再
写回并读回校验；**备份失败会直接中止，不会带着失败的备份去刷**。若目标镜像是
`--image app`，因为应用分区本就够不到 `nvs`，脚本会自动跳过备份这一步。
`ota_1 @ 0x410000` 和 `assets @ 0x800000` 在合并镜像覆盖范围之外，相册和资源包
不受任一种刷法影响。

若两个镜像的 application 构建时间戳不一致，`flash.py` 会在开刷前警告并建议先跑
`package.py` 重新生成；`--list` 同样会做这个检查，方便刷写前单独确认。

**依赖环境**：`flash.py` 首次运行会在仓库根目录自动创建 `.venv-esptool` 并装好
`esptool`/`pyserial`，然后把自己重新用那个 venv 的 Python 执行一遍（脚本内部
`os.execv` 完成，无感知）。这是刻意和系统 `python3` 分开的——较新的 Homebrew
Python 默认拒绝 `pip install`（externally-managed-environment），把 `esptool`
硬装进系统 Python 既装不进去，装进去了也容易把 Homebrew 自己搞坏。之后每次运行
直接复用这个 venv，不会重复安装。如果这个 venv 出了问题（比如手动改坏了），
删掉重建即可：

```bash
rm -rf .venv-esptool
```

若刷写失败且提示端口连不上（`Could not open ... the port is busy`），先确认没有
别的东西占着串口——常见是浏览器网页刷写页面的 Web Serial 连接还没断开、或者有
`idf.py monitor` / `screen` 挂在后台；网页刷写走的是浏览器自身的 Web Serial，和
这个脚本是完全独立的两条路径，互不冲突，但同一时间只能有一方真正打开设备的串口。
关掉占用方（或刷新/关闭那个网页标签页）后再重试。

若 esptool 能打开端口但连不上芯片：按住 BOOT、点一下 RESET（或按住 BOOT 重新插
USB）、松开 BOOT，再重试。若设备曾把 `xiaozhi.bin` 错写到 `0x0` 并出现 `Invalid
image block`，可执行 `idf.py erase-flash flash monitor` 恢复；该命令会清除原
NVS，需要保留时先用 `--backup-only` 备份，刷完再 `--restore` 写回。

根目录辅助命令：

```bash
npm run firmware:build
```

### 屏幕配置

固件 Kconfig 中有屏幕类型选择：

```text
ZECTRIX_EPD_PANEL_4COLOR_SSD2683  四色 BWRY 屏
ZECTRIX_EPD_PANEL_1BPP            黑白 1bpp 屏
```

如果要刷回旧黑白屏，先在 `idf.py menuconfig` 中切到 `1bpp black/white EPD`，再重新构建烧录。RawDraw 主题层会把红/黄语义色降级成黑白可读样式。

### SSD2683 ULTRA_BW 交互预览

Note4C 四色屏默认启用独立的 `ULTRA_BW` 路径。菜单切换、按钮、光标和
连续 UI 操作都会将语义 framebuffer 临时映射为黑白（红/黄映射为黑），
并调用 SSD2683/同类面板厂商示例中的 OTP fast-waveform 选择序列。针对实测
仍需约 12 秒的问题，默认实验时序把厂商的动态 12.5 Hz/20 ms blanking 改为
芯片手册范围内的固定 120 Hz/2 ms blanking，以画质、对比度和残影换取秒级
响应；原厂 12 秒时序以及 120 Hz/20 ms 中间档均可在 menuconfig 中回退。连续操作
期间不会按刷新次数插入四色全刷；最后一次交互后 60 秒无新操作，才执行一次
原有 `FULL_COLOR` 全局刷新来恢复颜色和清理残影。新的交互会取消并重新计时。

相关配置为 `CONFIG_ZECTRIX_EPD_FAST_BW` 和
`CONFIG_ZECTRIX_EPD_FAST_BW_TIMING_*`、
`CONFIG_ZECTRIX_EPD_FAST_BW_IDLE_FULL_SECONDS`。标准四色路径保持独立，若某批次
面板与极速时序不兼容，可先切回 `TIMING_VENDOR`，或关闭 FAST_BW。驱动还提供
默认关闭的只读 `CONFIG_ZECTRIX_EPD_SSD2683_MTP_DUMP`，可导出 3840-byte OTP/MTP
内容用于继续逆向；固件绝不会调用不可逆的 MTP 编程命令。控制器、
waveform 证据、像素映射、限制与硬件验证步骤见
[`docs/SSD2683_FAST_BW_RESEARCH.md`](docs/SSD2683_FAST_BW_RESEARCH.md)。120 Hz/2 ms
按相同 LUT 帧数理论上可把扫描阶段缩短约 9.7 倍，但 1–3 秒仍须在 Note4C 真机
上以 `[ULTRA_BW] waveform BUSY=` 日志验证，README 不把估算当成实测指标。

## UI 说明

固件 UI 目前走 RawDraw 组件体系，重点页面包括：

- 对话：显示用户语音、识别状态、AI 回复。
- 待办：本地展示、服务端同步、完成/删除/编辑。
- 设置：音量、亮度、主题、网络、同步、OTA 等。
- 相册：缩略图列表、大图展示、AP 传图入口。
- 天气/天气详情、新闻、黄历、年度进度、日历、电子书、日志。
- 快速切换 Overlay：用于页面间快速跳转。

四色屏主题层通过语义样式绘制组件，不建议在业务页面里继续新增裸 `RED/YELLOW/BLACK/WHITE`。新增 UI 时优先使用 RawDraw 组件和 theme token。

## 环境变量

常用后端环境变量：

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `DASHSCOPE_API_KEY` | 无 | 百炼 API Key，启动后端必需 |
| `LISTEN_HOST` | `0.0.0.0` | WebSocket 监听地址 |
| `LISTEN_PORT` | `9001` | WebSocket 端口 |
| `DISCOVERY_PORT` | `8766` | UDP 发现端口 |
| `PUSH_IMAGE_PORT` | `8766` | 图片/OTA HTTP API 端口 |
| `TTS_WS_CHUNK_BYTES` | `8000` | TTS 推送分片大小 |
| `TTS_WS_CHUNK_GAP_SEC` | `0.01` | TTS 分片发送间隔 |

不要提交 `.env`、数据库、日志、pid、构建目录和固件产物。

## Git 提交范围

建议提交：

- `firmware/main/`、`firmware/components/`、`firmware/partitions/` 等固件源码。
- `server/*.py`、`server/static/`、`server/requirements.txt`、`server/DEPLOY.md`。
- `frontend/src/`、`frontend/package.json`、`frontend/pnpm-lock.yaml` 等前端源码。
- 根目录 README、文档、配置模板。

不要提交：

- `firmware/build/`
- `firmware/managed_components/`
- `firmware/sdkconfig`
- `firmware/releases/`
- `server/.env`
- `server/todo.db`
- `server/*.pid`
- `server/*.log`
- `frontend/.env*`
- `frontend/dist/`
- `node_modules/`
