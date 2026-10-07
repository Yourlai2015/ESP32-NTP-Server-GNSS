// ESP32-S3 NTP 时间服务器
// GNSS UART 驱动与 NMEA 语句解析。
// 直接通过 UART 驱动接收机并解析 NMEA（RMC 取日期时间，GGA 取定位质量），不依赖具体型号，便于更换不同 GNSS 模块。



#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
} nmea_time_t;

typedef struct
{
    int satellites;    // 正在使用的卫星数量（来自 GGA）
    int fix_quality;   // 0 = 未定位，1 = GPS，2 = DGPS，...
    bool valid;        // 至少解析到一条 GGA 后为 true
    bool position_valid; // 经纬度已填充时为 true
    float latitude;      // 绝对值为十进制度
    char latitude_hemisphere;  // 'N' 或 'S'
    float longitude;     // 绝对值为十进制度
    char longitude_hemisphere; // 'E' 或 'W'
} gnss_fix_info_t;

// 安装并配置 GNSS UART（幂等）
void gnss_uart_init(void);

// 切换 UART 波特率并清空输入缓冲
void gnss_uart_set_baud(uint32_t baud);
uint32_t gnss_uart_current_baud(void);

// 读取并解析下一条 NMEA 语句，超时返回 false；解析到有效 RMC 时会内部缓存。
bool gnss_uart_read_sentence(char *out, size_t out_size, uint32_t timeout_ms);

// 等待收到有效的 RMC 语句（状态 'A' 且日期合理）。
bool gnss_uart_wait_for_time(nmea_time_t *out, uint32_t timeout_ms);

// 最近一条 GGA 中的卫星数与定位质量；尚未解析到 GGA 时返回 false。
bool gnss_get_fix_info(gnss_fix_info_t *out);

// 最近收到接收机字节的时间戳（esp_timer_get_time），从未收到为 0。
// 未定位的接收机仍会输出 NMEA，可借此区分“暂无定位”与“链路已断”。
int64_t gnss_uart_last_rx_us(void);

// 依次扫描候选波特率，返回首个能产生有效 NMEA 数据的值；
// preferred_baud 非零时优先尝试，检测结果会保留在 UART 上。
bool gnss_uart_detect_baud(uint32_t preferred_baud, uint32_t *detected_baud);

// 解析辅助函数（供单元式调用）
bool gnss_parse_rmc_sentence(const char *sentence, nmea_time_t *out);
bool gnss_nmea_checksum_valid(const char *sentence);
