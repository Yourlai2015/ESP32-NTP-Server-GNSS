// NTPv4 对称密钥认证实现：配置并校验密钥，基于 SHA-256 计算报文 MAC，
// 完成请求校验与应答认证追加。

#include "ntp_auth.h"

#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"

// 仅用于配置校验；禁用对称密钥认证时该部分会被编译剔除。
#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
static const char *TAG = "ntp_auth";
#endif

// ---------------------------------------------------------------------------
// 已配置的对称密钥
// ---------------------------------------------------------------------------
// key_value 必须为恰好 32 位字母数字字符；最多支持 NTP_AUTH_MAX_KEYS 个密钥，
// key_id 必须唯一且非零（参见上游项目 misc/symmetric_key_authentication_setup.md）。
// ---------------------------------------------------------------------------
typedef struct
{
    uint32_t key_id;
    char key_value[33];
} symmetric_key_t;

#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
static const symmetric_key_t symmetric_keys[] = {
    {1, "CHANGEMECHANGEMECHANGEMECHANGEME"},
};
#define NTP_AUTH_CONFIGURED_KEY_COUNT (sizeof(symmetric_keys) / sizeof(symmetric_keys[0]))

static const uint8_t NTP_AUTH_ALGORITHM_SHA256_160 = 1;
static const size_t SYMMETRIC_KEY_LENGTH = 32;
#endif

static const uint8_t NTP_AUTH_KEY_ACTIVE = 1;

typedef struct
{
    uint32_t key_id;
    uint8_t algorithm;
    uint8_t key_length;
    uint8_t lifecycle;
    uint8_t key[NTP_AUTH_MAX_KEY_SIZE];
} ntp_auth_key_internal_t;

static ntp_auth_key_internal_t s_keys[NTP_AUTH_MAX_KEYS];
static size_t s_key_count = 0;
static bool s_initialized = false;
static bool s_available = false;

// 按大端序读取 32 位整数
static uint32_t read_u32_be(const uint8_t *value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | (uint32_t)value[3];
}

// 按大端序写入 32 位整数
static void write_u32_be(uint8_t *value, uint32_t number)
{
    value[0] = (uint8_t)(number >> 24);
    value[1] = (uint8_t)(number >> 16);
    value[2] = (uint8_t)(number >> 8);
    value[3] = (uint8_t)(number);
}

// 恒定时间比较，避免时序侧信道
static bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    uint8_t difference = 0;
    for (size_t index = 0; index < length; ++index)
        difference |= left[index] ^ right[index];
    return difference == 0;
}

// 按 key_id 查找处于活动状态的密钥
static const ntp_auth_key_internal_t *find_key(uint32_t key_id)
{
    for (size_t index = 0; index < s_key_count; ++index)
    {
        if (s_keys[index].key_id == key_id && s_keys[index].lifecycle == NTP_AUTH_KEY_ACTIVE)
            return &s_keys[index];
    }
    return NULL;
}

// 以密钥为前缀计算输入的 SHA-256 摘要，并截取为 MAC 长度（使用 PSA Crypto API）
static bool calculate_digest(const ntp_auth_key_internal_t *key, const uint8_t *input, size_t input_length, uint8_t digest[NTP_AUTH_DIGEST_SIZE])
{
    uint8_t full_digest[32] = {0};
    size_t digest_length = 0;

    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_hash_setup(&operation, PSA_ALG_SHA_256);

    if (status == PSA_SUCCESS)
        status = psa_hash_update(&operation, key->key, key->key_length);
    if (status == PSA_SUCCESS)
        status = psa_hash_update(&operation, input, input_length);
    if (status == PSA_SUCCESS)
        status = psa_hash_finish(&operation, full_digest, sizeof(full_digest), &digest_length);

    if (status != PSA_SUCCESS)
    {
        psa_hash_abort(&operation);
        return false;
    }

    memcpy(digest, full_digest, NTP_AUTH_DIGEST_SIZE);
    return true;
}

// 解析并校验配置的密钥（幂等）
bool ntp_auth_initialize(void)
{
    if (s_initialized)
        return s_available;

    s_initialized = true;
    s_key_count = 0;

#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
    if (NTP_AUTH_CONFIGURED_KEY_COUNT < 1 || NTP_AUTH_CONFIGURED_KEY_COUNT > NTP_AUTH_MAX_KEYS)
    {
        ESP_LOGE(TAG, "Invalid number of symmetric keys: %u", (unsigned)NTP_AUTH_CONFIGURED_KEY_COUNT);
        s_available = false;
        return false;
    }

    for (size_t index = 0; index < NTP_AUTH_CONFIGURED_KEY_COUNT; ++index)
    {
        const symmetric_key_t *source = &symmetric_keys[index];
        bool valid = true;

        if (source->key_id < 1 || source->key_id > 65535)
        {
            ESP_LOGE(TAG, "Symmetric key %u: key_id must be between 1 and 65535 (got %lu)", (unsigned)(index + 1), (unsigned long)source->key_id);
            valid = false;
        }

        size_t actual_length = 0;
        bool alphanumeric = true;
        while (actual_length < sizeof(source->key_value) && source->key_value[actual_length] != '\0')
        {
            const char character = source->key_value[actual_length++];
            if (!((character >= '0' && character <= '9') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= 'a' && character <= 'z')))
                alphanumeric = false;
        }
        if (!alphanumeric)
        {
            ESP_LOGE(TAG, "Symmetric key %u: key_value must contain only alphanumeric characters", (unsigned)(index + 1));
            valid = false;
        }
        if (actual_length != SYMMETRIC_KEY_LENGTH || source->key_value[SYMMETRIC_KEY_LENGTH] != '\0')
        {
            ESP_LOGE(TAG, "Symmetric key %u: key_value must contain exactly %u characters (got %u)",
                     (unsigned)(index + 1), (unsigned)SYMMETRIC_KEY_LENGTH, (unsigned)actual_length);
            valid = false;
        }

        for (size_t other = 0; other < s_key_count; ++other)
        {
            if (s_keys[other].key_id == source->key_id)
            {
                ESP_LOGE(TAG, "Symmetric key %u: duplicate key_id %lu", (unsigned)(index + 1), (unsigned long)source->key_id);
                valid = false;
                break;
            }
        }

        if (!valid)
            continue;

        memset(&s_keys[s_key_count], 0, sizeof(s_keys[s_key_count]));
        s_keys[s_key_count].key_id = source->key_id;
        s_keys[s_key_count].algorithm = NTP_AUTH_ALGORITHM_SHA256_160;
        s_keys[s_key_count].key_length = (uint8_t)SYMMETRIC_KEY_LENGTH;
        s_keys[s_key_count].lifecycle = NTP_AUTH_KEY_ACTIVE;
        memcpy(s_keys[s_key_count].key, source->key_value, SYMMETRIC_KEY_LENGTH);
        s_key_count++;
    }
#endif

    s_available = s_key_count != 0;
    return s_available;
}

bool ntp_auth_available(void)
{
    return s_available;
}

size_t ntp_auth_key_count(void)
{
    return s_available ? s_key_count : 0;
}

uint32_t ntp_auth_default_key_id(void)
{
    return s_available ? s_keys[0].key_id : 0;
}

// 以十六进制字符串输出指定密钥
bool ntp_auth_key_hex(size_t index, uint32_t *key_id, char *output, size_t output_size)
{
    static const char hex_digits[] = "0123456789ABCDEF";

    if (!s_available || index >= s_key_count || key_id == NULL || output == NULL ||
        output_size < (size_t)s_keys[index].key_length * 2 + 1)
        return false;

    const ntp_auth_key_internal_t *key = &s_keys[index];
    for (size_t character_index = 0; character_index < key->key_length; ++character_index)
    {
        output[character_index * 2] = hex_digits[key->key[character_index] >> 4];
        output[character_index * 2 + 1] = hex_digits[key->key[character_index] & 0x0F];
    }
    output[key->key_length * 2] = '\0';
    *key_id = key->key_id;
    return true;
}

bool ntp_auth_default_key_hex(char *output, size_t output_size)
{
    uint32_t key_id = 0;
    return ntp_auth_key_hex(0, &key_id, output, output_size);
}

// 校验请求报文的 MAC
ntp_auth_result_t ntp_auth_verify_request(const uint8_t *packet, size_t packet_length, uint8_t version, uint32_t *key_id)
{
    if (key_id != NULL)
        *key_id = 0;
    if (packet == NULL || packet_length < NTP_PACKET_SIZE)
        return NTP_AUTH_MALFORMED;
    if (packet_length == NTP_PACKET_SIZE)
        return NTP_AUTH_UNAUTHENTICATED;
    if (version != 4 || packet_length != (size_t)NTP_PACKET_SIZE + NTP_AUTH_TRAILER_SIZE)
        return NTP_AUTH_MALFORMED;
    if (!s_available)
        return NTP_AUTH_UNAVAILABLE;

    const size_t key_id_offset = packet_length - NTP_AUTH_TRAILER_SIZE;
    const uint32_t requested_key_id = read_u32_be(packet + key_id_offset);
    const ntp_auth_key_internal_t *key = find_key(requested_key_id);
    if (key == NULL)
        return NTP_AUTH_UNKNOWN_KEY;

    uint8_t digest[NTP_AUTH_DIGEST_SIZE] = {0};
    if (!calculate_digest(key, packet, key_id_offset, digest) ||
        !constant_time_equal(digest, packet + key_id_offset + NTP_AUTH_KEY_ID_SIZE, NTP_AUTH_DIGEST_SIZE))
        return NTP_AUTH_INVALID_MAC;

    if (key_id != NULL)
        *key_id = requested_key_id;
    return NTP_AUTH_VALID;
}

// 为应答追加密钥 ID 与 MAC
bool ntp_auth_append_response(uint8_t *packet, size_t packet_capacity, size_t *packet_length, uint32_t key_id)
{
    if (packet == NULL || packet_length == NULL || *packet_length != NTP_PACKET_SIZE ||
        packet_capacity < (size_t)NTP_PACKET_SIZE + NTP_AUTH_TRAILER_SIZE || !s_available)
        return false;

    const ntp_auth_key_internal_t *key = find_key(key_id);
    if (key == NULL)
        return false;

    write_u32_be(packet + NTP_PACKET_SIZE, key_id);
    if (!calculate_digest(key, packet, NTP_PACKET_SIZE, packet + NTP_PACKET_SIZE + NTP_AUTH_KEY_ID_SIZE))
        return false;

    *packet_length = NTP_PACKET_SIZE + NTP_AUTH_TRAILER_SIZE;
    return true;
}
