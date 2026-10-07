# ESP32-S3 GNSS 授时服务器（Stratum 1 NTP Server）

基于 ESP32-S3 的**一级（Stratum 1）NTP 时间服务器**：通过 GNSS 模块获取 UTC 时间，
用 PPS（秒脉冲）驯服系统时钟，再通过 WiFi 为局域网内的设备提供 NTP 校时服务，
精度可达亚毫秒级。

本项目由 C++ 项目 [roblatour / ESP32TimeServer](https://github.com/roblatour/ESP32TimeServer)（MIT 许可）
移植为 C 语言版本，用于适配ESP32-S3，并新增了 SSD1306 OLED 状态显示与翻页按键。

***

## 目录

- [一、功能特性](#一功能特性)
- [二、硬件与接线](#二硬件与接线)
- [三、快速开始](#三快速开始)
- [四、编译与烧录](#四编译与烧录)
- [五、配置说明](#五配置说明)
- [六、使用说明](#六使用说明)
- [七、连接 NTP 客户端](#七连接-ntp-客户端)
- [八、常见问题排查](#八常见问题排查)
- [九、许可证与致谢](#九许可证与致谢)
- [十、关于作者](#十关于作者)

***

## 一、功能特性

- **GNSS 授时**：通过串口接收 NMEA 报文获取 UTC 时间与定位信息，兼容常见 GNSS 模块。
- **PPS 驯服**：秒脉冲由硬件捕获，平滑校正系统时钟，避免时间跳变，实现亚毫秒精度。
- **开机自恢复**：自动探测接收机波特率；接收机无响应或长期不定位时自动重试并重启。
- **标准 NTP 服务**：同时监听 IPv4、IPv6 与 IPv6 链路本地地址，支持 NTPv3 / NTPv4。
- **网页配网**：长按按键开启配网热点，手机连接后用网页设置 WiFi，凭据自动保存。
- **OLED 状态显示**：实时显示定位状态、时间、同步残差、经纬度、IP 与 PPS 状态。
- **可选对称密钥认证**：NTPv4 对称密钥（SHA-256 MAC），默认关闭。
- **可选启动自检**：开机时对自身 NTP 服务做回环测试，默认关闭。

***

## 二、硬件与接线

### 所需硬件

| 部件      | 说明                                         |
| ------- | ------------------------------------------ |
| 主控      | ESP32-S3 开发板                               |
| GNSS 模块 | 支持 NMEA 输出并带 PPS 引脚（如 u-blox M10 / NEO 系列） |
| 显示屏     | SSD1306 128×64 OLED（I2C 接口）                |
| 按键      | 轻触按键 1 个（一端接 GPIO，另一端接 GND）                |

### 接线表

引脚定义见 `main/include/app_config.h`，可按需修改。

| ESP32-S3 引脚 | 连接到            | 说明          |
| ----------- | -------------- | ----------- |
| GPIO41 (TX) | GNSS RXD       | ESP32-S3 发送 |
| GPIO42 (RX) | GNSS TXD       | ESP32-S3 接收 |
| GPIO45      | GNSS PPS       | 秒脉冲输入       |
| GPIO2       | OLED SCL       | I2C 时钟      |
| GPIO1       | OLED SDA       | I2C 数据      |
| GPIO21      | 按键             | 按下时接地       |
| GND         | GNSS / OLED 共地 | 共地          |

***

## 三、快速开始

1. 按上表接好 GNSS、OLED 与按键。
2. 修改 `main/include/app_config.h` 中的 WiFi 名称与密码（见[配置说明](#五配置说明)）。
3. 编译并烧录（见[编译与烧录](#四编译与烧录)）。
4. 上电后等待设备连接 WiFi、等待 GNSS 定位（OLED 上 `SAT` 显示 `FIX`）。
5. 记下 OLED 上显示的 IP 地址，将其配置为电脑/路由器的 NTP 服务器即可。

> 首次定位可能需要几分钟（取决于天线放置与天空视野）。室外或靠近窗户效果最好。

***

## 四、编译与烧录

前置条件：安装 [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.1 或更高版本**，并在终端导入其环境。

```bash
# 1. 设置目标芯片（本项目针对 ESP32-S3）
idf.py set-target esp32s3

# 2.（可选）按需调整分区表、日志级别等
idf.py menuconfig

# 3. 编译
idf.py build

# 4. 烧录并打开串口监视器（把 COMx / /dev/ttyUSBx 换成实际端口）
idf.py -p COMx flash monitor
```

烧录成功后，串口会打印启动信息，正常时会看到类似
`NTP server is now open for business` 的提示。

***

## 五、配置说明

所有可配置项都集中在 [`main/include/app_config.h`](main/include/app_config.h)。

### 将以下选项进行个性化修改

```c
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

### 常用配置项

| 配置项                                    | 默认值          | 说明                              |
| -------------------------------------- | ------------ | ------------------------------- |
| `WIFI_SSID` / `WIFI_PASSWORD`          | 占位符          | 要连接的 WiFi 名称与密码。                |
| `TIME_ZONE_SPEC`                       | `"CST-8"`    | 时区（POSIX TZ 字符串），默认北京时间（UTC+8）。 |
| `GNSS_TX_PIN` / `GNSS_RX_PIN`          | 41 / 42      | GNSS 串口引脚。                      |
| `GNSS_PPS_PIN`                         | 45           | PPS 输入引脚。                       |
| `OLED_SCK_PIN` / `OLED_SDA_PIN`        | 2 / 1        | OLED I2C 引脚。                    |
| `BUTTON_PIN`                           | 21           | 按键引脚。                           |
| `BUTTON_LONG_PRESS_MS`                 | 5000         | 触发配网的长按时长（毫秒）。                  |
| `PROVISION_AP_SSID_PREFIX`             | `"ESPTIME-"` | 配网热点名称前缀。                       |
| `PROVISION_AP_PASSWORD`                | `"12345678"` | 配网热点密码，建议修改；留空为开放热点。            |
| `PERIODIC_GNSS_REFRESH_MINUTES`        | 5            | 与 GNSS 重新对时的周期（分钟）。             |
| `SYMMETRIC_KEY_AUTHENTICATION_ENABLED` | 0            | 是否启用 NTPv4 对称密钥认证。              |
| `STARTUP_HEALTH_TEST_ENABLED`          | 0            | 是否启用开机自检。                       |
| `DEBUG_ENABLED`                        | 0            | 是否输出调试日志。                       |

> **关于认证密钥**：启用对称密钥认证时，密钥在 [`main/ntp/ntp_auth.c`](main/ntp/ntp_auth.c)
> 的 `symmetric_keys[]` 中配置，每个密钥必须为**恰好 32 位字母数字字符**，
> 且 `key_id` 唯一、非零。请务必把示例占位符替换成你自己的密钥。

> **关于时区**：`TIME_ZONE_SPEC` 只影响本地时间的显示与格式化；NTP 对外提供的始终是 UTC 时间。

***

## 六、使用说明

### 上电后的自动流程

设备上电后会依次：初始化显示与存储 → 连接 WiFi → 等待 GNSS 定位与 PPS 稳定 →
首次对时成功 → 启动 NTP 服务。全部就绪后，OLED 会显示正常状态页，NTP 开始对外服务。

### OLED 显示页面（短按按键切换）

**第 1 页 — 状态页**

| 行                     | 内容       | 说明              |
| --------------------- | -------- | --------------- |
| `SAT:xx FIX / NO FIX` | 卫星数与定位状态 | `FIX` 表示已定位     |
| 日期 / 时间               | 当前本地时间   | 受时区设置影响         |
| `DELTA:...`           | 上次同步残差   | 校正前本地时钟的偏差，越小越好 |

**第 2 页 — 位置页**

| 行                   | 内容         | 说明                |
| ------------------- | ---------- | ----------------- |
| `LAT` / `LON`       | 纬度 / 经度    | 定位有效时显示           |
| `IP:...`            | 设备 IPv4 地址 | 用于配置 NTP 客户端      |
| `PPS:ACTIVE / LOST` | 秒脉冲状态      | `ACTIVE` 表示正在驯服时钟 |

此外还有两个临时页面：进入配网模式时显示 `CONFIG MODE`（热点信息），
保存 WiFi 后显示 `WIFI SAVED`（正在重连）。

### 按键操作

- **短按**：切换 OLED 页面。
- **长按约 5 秒**：启动 WiFi 配网热点。

### WiFi 配网流程

当需要更换 WiFi，或首次使用时 WiFi 信息尚未配置：

1. **长按按键约 5 秒**，OLED 显示 `CONFIG MODE` 以及热点名称、密码、配置地址。
2. 用手机连接该热点，名称形如 `ESPTIME-1A2B`（后四位为设备 MAC 末两字节）。
3. 打开浏览器访问 **`http://192.168.4.1/`**。
4. 在网页中选择要连接的 WiFi（会自动列出附近网络），输入密码，点击“保存”。
5. 保存成功后热点自动关闭，设备用新凭据重连，OLED 显示 `WIFI SAVED`。

> 配网保存的凭证会写入设备存储，**优先级高于** **`app_config.h`** **中的编译期值**，
> 下次开机自动使用，无需重新配置。

***

## 七、连接 NTP 客户端

1. **获取设备 IP**：查看 OLED 第 2 页的 `IP:`，或串口日志中的 `IPv4 link is up (...)`。
2. 在客户端把该 IP 配置为 NTP 服务器，端口为标准的 **123**。

### Windows

以管理员身份打开 PowerShell：

```powershell
# 配置单一 NTP 服务器（0x9 表示客户端模式）
w32tm /config /manualpeerlist:"<设备IP>,0x9" /syncfromflags:manual /update

# 立即校时
w32tm /resync

# 查看状态
w32tm /query /status
```

### Linux（chrony）

编辑 `/etc/chrony/chrony.conf`，添加：

```
server <设备IP> iburst
```

然后重启服务：`sudo systemctl restart chrony`。

### Linux（systemd-timesyncd）

编辑 `/etc/systemd/timesyncd.conf`：

```
[Time]
NTP=<设备IP>
```

然后执行 `sudo systemctl restart systemd-timesyncd`。

### 使用对称密钥认证时

若已启用对称密钥认证，设备启动日志会分别打印适用于 **Meinberg**（`ntp.keys`）
和 **Chrony**（`chrony.keys`）的密钥行，可直接复制到客户端配置中。

***

## 八、常见问题排查

| 现象                      | 可能原因与处理                                    |
| ----------------------- | ------------------------------------------ |
| OLED 上 `SAT:-- no data` | 未收到 GNSS 数据。检查 TX/RX 是否接反、供电与共地、模块波特率。     |
| `SAT:xx NO FIX` 长时间不定位  | 天线视野不佳。移到室外或靠近窗户，等待数分钟；确认天线连接正常。           |
| `PPS:LOST`              | PPS 未接入或电平不对。确认 PPS 接 3.3V、走线短、与 ESP32 共地。 |
| `DELTA` 数值很大或非零偏大       | 首次对时或刚恢复，属正常现象，稳定后会快速收敛到毫秒级。               |
| 无法连接 WiFi               | 用长按按键进入配网模式重新设置 WiFi；确认密码至少 8 位（或留空为开放网络）。 |
| 找不到设备 IP                | 设备尚未连上 WiFi。查看串口日志，必要时重新配网。                |
| 设备反复重启                  | 多为 GNSS 始终无法就绪。检查接收机接线、供电与模块兼容性；串口日志会提示原因。 |

调试建议：将 `app_config.h` 中的 `DEBUG_ENABLED` 置为 `1`，可输出更详细的串口日志
（会略微增加高负载下的开销）。

***

## 九、许可证与致谢

- 本项目整体以 **Apache License 2.0** 发布，全文见 [LICENSE](LICENSE)。
- 本项目为 [roblatour / ESP32TimeServer](https://github.com/roblatour/ESP32TimeServer) 的 C 语言移植版本，
  其中移植自上游的部分仍遵循其原始 **MIT 许可**（Copyright (c) 2026 Rob Latour），版权归原作者所有，全文见 [LICENSES/MIT.txt](LICENSES/MIT.txt)。
- 完整的第三方归属说明见 [NOTICE](NOTICE)。

> 说明：本项目遵循 MIT → Apache-2.0 的兼容方式——自身代码以 Apache-2.0 授权，
> 第三方 MIT 代码保留其原始 MIT 声明，两者可共存于同一仓库。


***

## 十、关于作者

- 作者：yourlai
- 网站：[yourlai.com](https://yourlai.com)
- 邮箱：<yourlai@yourlai.icu>

