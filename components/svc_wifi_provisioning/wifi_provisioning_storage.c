#include "wifi_provisioning_internal.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

/** NVS 命名空间长度受 ESP-IDF 限制，因此使用简短且固定的名称。 */
#define WIFI_PROVISIONING_NVS_NAMESPACE "wifi_cfg"

/** 所有配置以单个二进制对象保存，保证列表更新具备原子提交语义。 */
#define WIFI_PROVISIONING_NVS_KEY "profiles"

/** 用于识别存储内容是否属于本组件的固定魔数。 */
#define WIFI_PROVISIONING_STORAGE_MAGIC 0x57465052UL

/** 当前持久化结构版本，后续变更结构时必须递增。 */
#define WIFI_PROVISIONING_STORAGE_VERSION 1U

static const char *TAG = "wifi_storage";

/**
 * @brief NVS 中保存的完整数据布局。
 *
 * profiles[0] 始终是最高优先级，连接任务按照数组顺序轮询。保存成功或
 * 手动新增的配置会移动到首位，从而缩短下次开机连接常用网络的时间。
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t profile_count;
    wifi_provisioning_profile_t profiles[WIFI_PROVISIONING_MAX_PROFILES];
} wifi_provisioning_storage_blob_t;

static nvs_handle_t s_nvs_handle;
static bool s_storage_initialized;

/**
 * @brief 校验配置字段，阻止无效字符串写入 NVS 或传递给 Wi-Fi 驱动。
 */
static bool profile_is_valid(const wifi_provisioning_profile_t *profile)
{
    if (profile == NULL) {
        return false;
    }

    size_t ssid_length = strnlen(profile->ssid, sizeof(profile->ssid));
    size_t password_length = strnlen(profile->password, sizeof(profile->password));
    return ssid_length > 0U && ssid_length < sizeof(profile->ssid) &&
           password_length < sizeof(profile->password);
}

/**
 * @brief 创建一份具有正确魔数和版本号的空存储对象。
 */
static void initialize_empty_blob(wifi_provisioning_storage_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->magic = WIFI_PROVISIONING_STORAGE_MAGIC;
    blob->version = WIFI_PROVISIONING_STORAGE_VERSION;
}

/**
 * @brief 从 NVS 读取并校验完整配置对象。
 *
 * NVS 中不存在数据时返回一个空列表；发现版本、长度或字段异常时同样使用
 * 空列表并覆盖无效内容，避免损坏数据导致后续越界访问。
 */
static esp_err_t load_blob(wifi_provisioning_storage_blob_t *blob)
{
    initialize_empty_blob(blob);

    size_t required_size = sizeof(*blob);
    esp_err_t error = nvs_get_blob(s_nvs_handle, WIFI_PROVISIONING_NVS_KEY, blob, &required_size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }

    bool valid = required_size == sizeof(*blob) &&
                 blob->magic == WIFI_PROVISIONING_STORAGE_MAGIC &&
                 blob->version == WIFI_PROVISIONING_STORAGE_VERSION &&
                 blob->profile_count <= WIFI_PROVISIONING_MAX_PROFILES;

    for (size_t index = 0; valid && index < blob->profile_count; ++index) {
        valid = profile_is_valid(&blob->profiles[index]);
    }

    if (valid) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "检测到无效的 Wi-Fi 配置存储，已重建为空列表");
    initialize_empty_blob(blob);
    error = nvs_set_blob(s_nvs_handle, WIFI_PROVISIONING_NVS_KEY, blob, sizeof(*blob));
    if (error == ESP_OK) {
        error = nvs_commit(s_nvs_handle);
    }
    return error;
}

/**
 * @brief 将完整配置对象原子写入 NVS 并提交。
 */
static esp_err_t save_blob(const wifi_provisioning_storage_blob_t *blob)
{
    esp_err_t error = nvs_set_blob(s_nvs_handle, WIFI_PROVISIONING_NVS_KEY, blob, sizeof(*blob));
    if (error == ESP_OK) {
        error = nvs_commit(s_nvs_handle);
    }
    return error;
}

esp_err_t wifi_provisioning_storage_init(void)
{
    if (s_storage_initialized) {
        return ESP_OK;
    }

    esp_err_t error = nvs_open(WIFI_PROVISIONING_NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (error != ESP_OK) {
        return error;
    }

    s_storage_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_provisioning_storage_get_profiles(wifi_provisioning_profile_t *profiles,
                                                  size_t max_profiles,
                                                  size_t *profile_count)
{
    if (profiles == NULL || profile_count == NULL || max_profiles == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_storage_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_provisioning_storage_blob_t blob;
    esp_err_t error = load_blob(&blob);
    if (error != ESP_OK) {
        return error;
    }

    size_t copy_count = blob.profile_count;
    if (copy_count > max_profiles) {
        copy_count = max_profiles;
    }
    memcpy(profiles, blob.profiles, copy_count * sizeof(profiles[0]));
    *profile_count = copy_count;
    return ESP_OK;
}

esp_err_t wifi_provisioning_storage_save_profile(const wifi_provisioning_profile_t *profile)
{
    if (!profile_is_valid(profile)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_storage_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_provisioning_storage_blob_t blob;
    esp_err_t error = load_blob(&blob);
    if (error != ESP_OK) {
        return error;
    }

    size_t existing_index = blob.profile_count;
    for (size_t index = 0; index < blob.profile_count; ++index) {
        if (strcmp(blob.profiles[index].ssid, profile->ssid) == 0) {
            existing_index = index;
            break;
        }
    }

    size_t move_count;
    if (existing_index < blob.profile_count) {
        move_count = existing_index;
    } else if (blob.profile_count < WIFI_PROVISIONING_MAX_PROFILES) {
        move_count = blob.profile_count;
        blob.profile_count++;
    } else {
        move_count = WIFI_PROVISIONING_MAX_PROFILES - 1U;
    }

    if (move_count > 0U) {
        memmove(&blob.profiles[1],
                &blob.profiles[0],
                move_count * sizeof(blob.profiles[0]));
    }
    blob.profiles[0] = *profile;

    return save_blob(&blob);
}

esp_err_t wifi_provisioning_storage_delete_profile(const char *ssid)
{
    if (ssid == NULL || strnlen(ssid, sizeof(((wifi_provisioning_profile_t *)0)->ssid)) == 0U ||
        strnlen(ssid, sizeof(((wifi_provisioning_profile_t *)0)->ssid)) >=
            sizeof(((wifi_provisioning_profile_t *)0)->ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_storage_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_provisioning_storage_blob_t blob;
    esp_err_t error = load_blob(&blob);
    if (error != ESP_OK) {
        return error;
    }

    for (size_t index = 0; index < blob.profile_count; ++index) {
        if (strcmp(blob.profiles[index].ssid, ssid) != 0) {
            continue;
        }

        size_t remaining_count = blob.profile_count - index - 1U;
        if (remaining_count > 0U) {
            memmove(&blob.profiles[index],
                    &blob.profiles[index + 1U],
                    remaining_count * sizeof(blob.profiles[0]));
        }
        blob.profile_count--;
        memset(&blob.profiles[blob.profile_count], 0, sizeof(blob.profiles[0]));
        return save_blob(&blob);
    }

    return ESP_OK;
}
