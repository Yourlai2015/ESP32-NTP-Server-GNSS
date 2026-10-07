// NTPv4 对称密钥报文认证接口（RFC 5905 / RFC 8573 风格 MAC）。
// 密钥在 ntp_auth.c 的 symmetric_keys[] 中配置，须为 32 位字母数字字符。
// 由原 C++ 实现移植，哈希使用 PSA Crypto（psa/crypto.h）。


#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    NTP_AUTH_UNAUTHENTICATED = 0,
    NTP_AUTH_VALID,
    NTP_AUTH_MALFORMED,
    NTP_AUTH_UNAVAILABLE,
    NTP_AUTH_UNKNOWN_KEY,
    NTP_AUTH_INVALID_MAC,
} ntp_auth_result_t;

// 初始化已配置的密钥（幂等）
bool ntp_auth_initialize(void);
bool ntp_auth_available(void);
size_t ntp_auth_key_count(void);

uint32_t ntp_auth_default_key_id(void);
bool ntp_auth_key_hex(size_t index, uint32_t *key_id, char *output, size_t output_size);
bool ntp_auth_default_key_hex(char *output, size_t output_size);

// 校验请求的 MAC：明文 48 字节请求返回 UNAUTHENTICATED，校验通过返回 VALID，
// 否则返回对应错误码；成功时 *key_id 输出请求携带的密钥 ID。
ntp_auth_result_t ntp_auth_verify_request(const uint8_t *packet, size_t packet_length, uint8_t version, uint32_t *key_id);

// 为 48 字节应答追加密钥 ID 与 MAC，并更新 *packet_length。
bool ntp_auth_append_response(uint8_t *packet, size_t packet_capacity, size_t *packet_length, uint32_t key_id);
