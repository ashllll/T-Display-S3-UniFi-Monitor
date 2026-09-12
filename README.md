# T-Display-S3 UniFi Monitor

基于 **LILYGO T-Display-S3（ESP32-S3）** 的 UniFi Network 小屏监控器。通过局域网 HTTPS 读取官方 Network Integration API，以 ESP-IDF + LVGL 9 显示设备、客户端、系统资源和上联流量。本项目非 Ubiquiti 官方产品。

![五视图与无数据状态，匿名测试数据](designs/unifi-ui/preview.png)

预览由实际 LVGL 代码渲染，使用合成数据，并非开发板照片。

## 硬件与功能

- ESP32-S3R8、16 MB Flash、8 MB OPI PSRAM。
- ST7789 170×320 LCD，8 位 i80 并口，横屏 320×170；不是 SPI 屏。
- Overview / Devices / Clients / Traffic / Console 五个视图。
- CPU、内存、运行时间、设备/客户端数量、选定设备上联 RX/TX；过期或缺失数据以 `--` 表示。
- GPIO0（BOOT）上一页、GPIO14 下一页；60 秒无操作回 Overview。
- Wi-Fi 重连、请求退避与限流处理、夜间背光。

上联流量不等于 WAN 吞吐，API 可达不代表互联网正常。设备和客户端列表各显示最多三项，客户端列表不是流量排行。更多语义见[固件说明](examples/ikuai_widget/README.md)。

## 1. 安装工具并获取源码

安装 Git 和 [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)，或使用 VS Code 的 PlatformIO IDE 终端。首次构建需联网下载工具链、ESP-IDF 和 LVGL。

```bash
git clone https://github.com/ashllll/T-Display-S3-UniFi-Monitor.git
cd T-Display-S3-UniFi-Monitor
pio --version
```

固件目录沿用 `examples/ikuai_widget`，实际运行的是 UniFi 固件；保留路径以兼容构建配置和测试。

## 2. 创建本地配置

在仓库根目录执行，已有文件不会被覆盖：

```bash
python3 - <<'PY'
from pathlib import Path
src = Path('examples/ikuai_widget/src')
for name in ('config', 'unifi_config', 'unifi_cert'):
    target = src / (name + '.h')
    if not target.exists():
        target.write_bytes((src / (name + '.example.h')).read_bytes())
PY
```

在本地 `examples/ikuai_widget/src/config.h` 中填写 Wi-Fi：

```c
#define APP_WIFI_SSID "YOUR_SSID"
#define APP_WIFI_PASS "YOUR_PASSWORD"
```

开发板使用 2.4 GHz Wi-Fi；网络应允许访问控制台 HTTPS、DNS 和授时服务。可在同一文件调整时区、日间/夜间背光。正式构建强制真实数据模式，缺少配置不会产生演示数据。

## 3. 创建 UniFi API Key

打开 **UniFi Network → Integrations**，查看当前版本的本地 API 文档并创建专用 Key。官方入口见 [UniFi API 指南](https://help.ui.com/hc/en-us/articles/30076656117655-Getting-Started-with-the-Official-UniFi-API)。本项目使用本地 Network API，不使用云 Site Manager Key、网页登录密码或 Cookie。

在本地 `examples/ikuai_widget/src/unifi_config.h` 填写：

| 配置项 | 填写方式 |
|---|---|
| `UNIFI_HOST` | 局域网 HTTPS origin，例如 `https://unifi.home.arpa`，不含末尾 `/`、路径或云端管理 URL |
| `UNIFI_API_KEY` | 专用密钥，只放本地配置，不写进模板或 README |
| `UNIFI_SITE_ID` | Integration API 返回的站点 UUID |
| `UNIFI_DEVICE_ID` | 要监控的网关/控制台设备 UUID |
| `UNIFI_TLS_SERVER_NAME` | 默认 `NULL`；按 IP 连接且证书仅含 DNS 名时，填证书名称，如 `"unifi.home.arpa"` |

使用控制台本地 API 文档/调试工具，以 `X-API-Key` 请求头执行只读请求：

1. `GET /proxy/network/integration/v1/sites`：从 `data[].id` 选择站点 UUID。
2. `GET /proxy/network/integration/v1/sites/{siteId}/devices`：根据名称、型号和地址核实目标设备，读取其 `id`；设备多时按文档分页。
3. 将 UUID 填入配置；检查 `/sites/{siteId}/devices/{deviceId}/statistics/latest` 是否返回统计（同一 API 前缀）。

UUID 不是 MAC、IP、云控制台 ID 或 `default`。留空仅在唯一站点、唯一带 gateway 标记设备时自动选择；部分控制台没有 gateway 标记，必须手动填写设备 UUID，不能将任意交换机当网关。

固件只发 GET 请求，但密钥权限由 UniFi 账号/角色决定。使用满足读取需要的最低权限，并设置合适的有效期。

## 4. 配置 TLS 信任

默认使用 ESP-IDF 公共 CA 证书包，始终验证证书名称。私有 CA/自签证书需从可信管理渠道取得并核对，随后写入本地 `examples/ikuai_widget/src/unifi_cert.h`。只放证书，不放私钥：

```c
#pragma once
static const char unifi_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "REPLACE_WITH_VERIFIED_CERTIFICATE_BASE64\n"
    "-----END CERTIFICATE-----\n";
```

主机名应匹配证书 SAN，必要时设置 `UNIFI_TLS_SERVER_NAME`。证书变化后重新核对并构建烧录，不要关闭 TLS 验证。公共 CA 签发且名称匹配时，保留模板中的空证书字符串即可。

## 5. 构建、烧录和串口

在仓库根目录执行：

```bash
pio run -e t-display-s3 -d examples/ikuai_widget
pio device list
# 用实际端口替换 PORT：macOS /dev/cu.usbmodem...，Windows COM...
pio run -e t-display-s3 -d examples/ikuai_widget -t upload --upload-port PORT
pio device monitor -b 115200 -p PORT
```

无法进入下载模式时：按住 BOOT，按一下 RST，再松开 BOOT，重新烧录。LCD 总电源 GPIO15 必须置高，现有驱动已处理。保留 ESP-IDF、16 MB Flash、OPI PSRAM 配置。

启动后依次核对 Wi-Fi、授时、API 状态及数据，再实机检查五页切换、颜色、亮度与流量变化。构建成功不等于 API 连通或实机验收通过。

## 常见问题

| 现象 | 检查项 |
|---|---|
| 等待 Wi-Fi | 2.4 GHz SSID、密码、信号、DHCP |
| 配置/设备发现错误 | HTTPS origin、Integration UUID；多站点或缺少 gateway 标记时手动填写 |
| HTTP 401/403 | Key 有效期、本地 Network Key 类型、账号权限 |
| HTTP 404 | 控制台版本、UUID、API 前缀；独立 Network Server 可能需要修改代码中的 UniFi OS 前缀 |
| TLS/网络错误 | 时间同步、证书链与 SAN、DNS、端口、网络可达性 |
| HTTP 429 | 等待 Retry-After 退避，不要加快轮询 |
| 曲线不变 | 控制台统计更新可能慢于 1 秒轮询；动画不代表新数据 |
| 屏幕暗 | 供电、GPIO15、复位日志、夜间背光 |

## API Key 与发布安全

- `config.h`、`unifi_config.h`、`unifi_cert.h` 被 Git 忽略，仓库仅提供 `.example.h` 模板。
- API Key 和 Wi-Fi 密码会编译进固件。**不要发布个人 `.bin`、`.elf`、构建目录、Flash dump、带凭据的日志或接口响应。**
- 不要在命令行、截图、Issue、PR、聊天或第三方在线 API 调试服务中粘贴密钥；接口响应也可能含家庭设备信息。
- 提交前检查 `git diff --cached` 和 `git ls-files`；不要用 `git add -f` 强行添加私密文件。
- `.gitignore` 不会清理历史。如凭据曾泄露，先撤销/更换密钥，再清理历史；仅删除当前文件不够。
- 本仓库以独立初始提交发布，不继承旧项目历史，不提供含个人凭据的预编译固件。

## 主机验证

先完成一次 PlatformIO 构建以取得 SDK 和 LVGL。主机还需 Python 3、C/C++ 编译器及 CMake：

```bash
python3 tests/test_system_health_parser.py
python3 tests/test_telemetry_timing.py
python3 tests/render_unifi_ui.py
```

解析测试使用 SDK cJSON 与 ASan/UBSan；时序测试模拟网络与时间；UI 测试编译实际 LVGL 代码，检查导航、布局、过期状态和图表采样时间。渲染写入 `/tmp/unifi-console-render`，不读取个人 API Key。

## 目录与许可

- `boards/`：T-Display-S3 板定义。
- `examples/ikuai_widget/`：PlatformIO/ESP-IDF 工程、源码、配置模板。
- `tests/`：解析、时序、LVGL 主机测试。
- `designs/unifi-ui/`：匿名 UI 预览。

基于 LILYGO T-Display-S3 项目及 iKuai 监控分支演进。保留原始 [MIT 许可证](LICENSE) 和版权声明；Lato 字体遵循 [SIL Open Font License](examples/ikuai_widget/src/fonts/Lato-OFL.txt)。UniFi 名称及相关商标属于其权利人。
