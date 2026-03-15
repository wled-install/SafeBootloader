#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"

#include "esp_flash_partitions.h"
#include "esp_rom_crc.h"
#include "esp32/rom/spi_flash.h"
#include "esp_image_format.h"
#include "bootloader_sha.h"
#include "bootloader_flash.h"

#include "soc/timer_group_reg.h"
#include "soc/timer_group_struct.h"

static const char *TAG = "safe_boot";

#define COPY_BLOCK 4096
#define CRC_MAGIC 0x53504653

#define PART_TYPE_APP  0x00
#define PART_TYPE_DATA 0x01

#define PART_SUBTYPE_APP_OTA_0 0x10
#define PART_SUBTYPE_APP_OTA_1 0x11

#define PART_SUBTYPE_DATA_SPIFFS 0x82
#define PART_SUBTYPE_SPIFFS_BACKUP 0x40
#define PART_SUBTYPE_DATA_CRC_0 0x41
#define PART_SUBTYPE_DATA_CRC_1 0x42


typedef struct {
    uint32_t magic;
    uint32_t crc_spiffs;    // CRC SPIFFS / Backup
    uint32_t size_spiffs;   // Größe SPIFFS / Backup
} crc_group_t;

crc_group_t crc_data_0;
crc_group_t crc_data_1;

const esp_partition_info_t *crc_0_part = NULL;
const esp_partition_info_t *crc_1_part = NULL;

static esp_partition_info_t partition_table[ESP_PARTITION_TABLE_MAX_ENTRIES];

/* -----------------------------------------------------------
   Partition table
----------------------------------------------------------- */

static void load_partition_table()
{
    ESP_LOGI(TAG, "Loading partition table from flash @0x%X",
             ESP_PARTITION_TABLE_OFFSET);

    esp_rom_spiflash_read(
        ESP_PARTITION_TABLE_OFFSET,
        (uint32_t *)partition_table,
        sizeof(partition_table)
    );

    for (int i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; i++) {

        esp_partition_info_t *p = &partition_table[i];
        if (p->magic != ESP_PARTITION_MAGIC) break;

        //ESP_LOGI(TAG,"Partition %d: type=0x%02X subtype=0x%02X offset=0x%08X size=0x%08X",i, p->type, p->subtype, p->pos.offset, p->pos.size);

    }
}

static const esp_partition_info_t* find_partition(uint8_t type, uint8_t subtype)
{
    for (int i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; i++) {

        esp_partition_info_t *p = &partition_table[i];
        if (p->magic != ESP_PARTITION_MAGIC) break;
        if (p->type == type && p->subtype == subtype) {

            //ESP_LOGI(TAG,"Found partition type=0x%02X subtype=0x%02X @0x%08X size=0x%08X",type, subtype, p->pos.offset, p->pos.size);
            return p;
        }
    }
    ESP_LOGW(TAG, "Partition type=0x%02X subtype=0x%02X not found", type, subtype);

    return NULL;
}

/* -----------------------------------------------------------
   Flash helpers
----------------------------------------------------------- */
bool crc_initialized()
{
    return (crc_data_0.magic == CRC_MAGIC) && (crc_data_1.magic == CRC_MAGIC);
}

static esp_err_t flash_copy(uint32_t src, uint32_t dst, uint32_t size)
{
    uint8_t buf[COPY_BLOCK];
    ESP_LOGI(TAG, "Copy flash: src=0x%08X -> dst=0x%08X size=%u bytes", src, dst, size);
    // Watchdog Timeout setzen
    #define RTC_WDT_TIMEOUT_SEC 5
    #define RTC_CLK_FREQ 150000
    uint32_t timeout_cycles = RTC_WDT_TIMEOUT_SEC * RTC_CLK_FREQ;
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, RTC_CNTL_WDT_WKEY_VALUE);
    REG_WRITE(RTC_CNTL_WDTCONFIG0_REG, timeout_cycles);
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0);

    for (uint32_t off = 0; off < size; off += COPY_BLOCK) {
        uint32_t chunk = (size - off) > COPY_BLOCK ? COPY_BLOCK : (size - off);
	
        if (esp_rom_spiflash_read(src + off, (uint32_t *)buf, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "Flash read failed @0x%08X", src + off);
            return ESP_FAIL;
        }

        uint32_t start_sector = (dst + off) / 4096;
        uint32_t end_sector   = (dst + off + chunk - 1) / 4096;
        for (uint32_t s = start_sector; s <= end_sector; s++) {
            if (esp_rom_spiflash_erase_sector(s) != ESP_OK) {
                ESP_LOGE(TAG, "Flash erase failed sector %u", s);
                return ESP_FAIL;
            }
        }

        uint32_t write_size = (chunk + 3) & ~0x3;
        if (esp_rom_spiflash_write(dst + off, (uint32_t *)buf, write_size) != ESP_OK) {
            ESP_LOGE(TAG, "Flash write failed @0x%08X", dst + off);
            return ESP_FAIL;
        }

        ets_delay_us(50);
        // Feed Watchdog
        REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, RTC_CNTL_WDT_WKEY_VALUE);
        REG_WRITE(RTC_CNTL_WDTCONFIG0_REG, timeout_cycles);
        REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0);
    }

    ESP_LOGI(TAG, "Flash copy finished");
    return ESP_OK;
}


static uint32_t flash_crc(uint32_t addr, uint32_t size)
{
    uint8_t buf[1024];
    uint32_t crc = 0;

    uint32_t remaining = size;
    uint32_t offset = 0;

    while (remaining > 0) {
        uint32_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        uint32_t read_size = (chunk + 3) & ~0x3;

        esp_rom_spiflash_read(addr + offset, (uint32_t *)buf, read_size);
        crc = esp_rom_crc32_le(crc, buf, chunk);

        remaining -= chunk;
        offset += chunk;
    }

    return crc;
}

static esp_err_t write_crc_partition(const esp_partition_info_t *part, crc_group_t *crc)
{
    crc_group_t tmp = *crc;
    uint32_t sector = part->pos.offset / 4096;

    if (esp_rom_spiflash_erase_sector(sector) != ESP_OK) {
        ESP_LOGE(TAG, "Erase CRC sector failed");
        return ESP_FAIL;
    }
    if (esp_rom_spiflash_write(part->pos.offset, (uint32_t *)&tmp, sizeof(tmp)) != ESP_OK) {
        ESP_LOGE(TAG, "Write CRC failed");
        return ESP_FAIL;
    }

    crc_group_t verify;
    esp_rom_spiflash_read(part->pos.offset, (uint32_t *)&verify, sizeof(verify));
    if (memcmp(&verify, &tmp, sizeof(tmp)) != 0) {
        ESP_LOGE(TAG, "CRC verify failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

#define BUF 1024

static bool compute_image_sha(const esp_partition_pos_t *pos, uint8_t sha[32])
{
    esp_image_metadata_t meta;

    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, pos, &meta) != ESP_OK)
        return false;

    bootloader_sha256_handle_t ctx = bootloader_sha256_start();

    uint8_t buf[BUF];
    uint32_t remaining = meta.image_len;
    uint32_t addr = pos->offset;

    while (remaining) {

        uint32_t len = remaining > BUF ? BUF : remaining;

        if (esp_rom_spiflash_read(addr, buf, len) != ESP_OK)
            return false;

        bootloader_sha256_data(ctx, buf, len);

        addr += len;
        remaining -= len;
    }

    bootloader_sha256_finish(ctx, sha);

    return true;
}

/* -----------------------------------------------------------
   OTA Sync
----------------------------------------------------------- */

static void handle_ota_sync(int boot_index)
{
    ESP_LOGI(TAG, "Checking OTA partitions");

    const esp_partition_info_t *ota0 = find_partition(PART_TYPE_APP, PART_SUBTYPE_APP_OTA_0);
    const esp_partition_info_t *ota1 = find_partition(PART_TYPE_APP, PART_SUBTYPE_APP_OTA_1);
    if (!ota0 || !ota1) return;

	esp_partition_pos_t pos0 = {
		.offset = ota0->pos.offset,
		.size   = ota0->pos.size
	};

	esp_partition_pos_t pos1 = {
		.offset = ota1->pos.offset,
		.size   = ota1->pos.size
	};

	uint8_t sha0[32];
    uint8_t sha1[32];

    bool valid0 = compute_image_sha(&pos0, sha0);
    bool valid1 = compute_image_sha(&pos1, sha1);
	
	ESP_LOG_BUFFER_HEX("BOOT", sha0, 32);
    ESP_LOG_BUFFER_HEX("BOOT", sha1, 32);
	
	bool running_is_ota0 = (boot_index == 0);
	bool running_is_ota1 = (boot_index == 1);
	
	bool identical = (memcmp(sha0, sha1, 32) == 0);

	if (valid0 && valid1) {
		if (running_is_ota0 && !identical) {
            ESP_LOGI(TAG, "OTA0 is current FW: sync OTA0 -> OTA1");
            flash_copy(ota0->pos.offset, ota1->pos.offset, ota0->pos.size);
        } else if (running_is_ota1 && !identical) {
            ESP_LOGI(TAG, "OTA1 is current FW: sync OTA1 -> OTA0");
            flash_copy(ota1->pos.offset, ota0->pos.offset, ota1->pos.size);
        }
	
    } else if (valid0) {
		ESP_LOGW(TAG, "OTA0 valid, but OTA1 not: sync OTA0 -> OTA1");
		flash_copy(ota0->pos.offset, ota1->pos.offset, ota0->pos.size);
    } else if (valid1) {
        ESP_LOGW(TAG, "OTA1 valid, but OTA0 not: sync OTA1 -> OTA0");
        flash_copy(ota1->pos.offset, ota0->pos.offset, ota1->pos.size);
    } else {
        ESP_LOGE(TAG, "Both OTA partitions invalid!");
    }
}

/* -----------------------------------------------------------
   CRC Read
----------------------------------------------------------- */
static void crc_read()
{
    crc_0_part = find_partition(PART_TYPE_DATA, PART_SUBTYPE_DATA_CRC_0);
    crc_1_part = find_partition(PART_TYPE_DATA, PART_SUBTYPE_DATA_CRC_1);
 
    if (!crc_0_part || !crc_1_part) {
		ESP_LOGE(TAG, "CRC partitions missing");
		return;
	}
    esp_rom_spiflash_read(crc_0_part->pos.offset, (uint32_t *)&crc_data_0, sizeof(crc_data_0));
    esp_rom_spiflash_read(crc_1_part->pos.offset, (uint32_t *)&crc_data_1, sizeof(crc_data_1));
 
}

/* -----------------------------------------------------------
   SPIFFS Handling
----------------------------------------------------------- */

static void handle_spiffs()
{
    const esp_partition_info_t *spiffs = find_partition(PART_TYPE_DATA, PART_SUBTYPE_DATA_SPIFFS);
    const esp_partition_info_t *backup = find_partition(PART_TYPE_DATA, PART_SUBTYPE_SPIFFS_BACKUP);
    if (!spiffs || !backup || !crc_0_part || !crc_1_part) return;

    uint32_t crc_current = flash_crc(spiffs->pos.offset, spiffs->pos.size);
    uint32_t crc_backup  = flash_crc(backup->pos.offset, backup->pos.size);

    bool current_valid = (crc_data_0.magic == CRC_MAGIC) &&
                         (crc_data_0.crc_spiffs == crc_current) &&
                         (crc_data_0.size_spiffs == spiffs->pos.size);
					
    bool backup_valid = (crc_data_1.magic == CRC_MAGIC) &&
                        (crc_data_1.crc_spiffs == crc_backup) &&
                        (crc_data_1.size_spiffs == backup->pos.size);
	
	//ESP_LOGI(TAG,"SPIFFS CRC calc / read: 0x%08X/0x%08X; Magic: 0x%08X; Size: 0x%08X",crc_current, crc_data_0.crc_spiffs, crc_data_0.magic, crc_data_0.size_spiffs);
	//ESP_LOGI(TAG,"BACKUP CRC calc / read: 0x%08X/0x%08X; Magic: 0x%08X; Size: 0x%08X",crc_backup, crc_data_1.crc_spiffs, crc_data_1.magic, crc_data_1.size_spiffs);

    if (current_valid) {
  
        if (crc_backup != crc_current) {
            ESP_LOGI(TAG, "SPIFFS valid -> creating backup");
            flash_copy(spiffs->pos.offset, backup->pos.offset, spiffs->pos.size);
   
            crc_data_1.magic = CRC_MAGIC;
            crc_data_1.crc_spiffs = crc_current;
            crc_data_1.size_spiffs = spiffs->pos.size;
            write_crc_partition(crc_1_part, &crc_data_1);
        }
    } else if (backup_valid) {
        ESP_LOGW(TAG, "Restoring SPIFFS from backup");
        flash_copy(backup->pos.offset, spiffs->pos.offset, spiffs->pos.size);
        crc_data_0.magic = CRC_MAGIC;
        crc_data_0.crc_spiffs = crc_backup;
        crc_data_0.size_spiffs = spiffs->pos.size;
        write_crc_partition(crc_0_part, &crc_data_0);
    } else {
        ESP_LOGI(TAG, "Generating new SPIFFS CRC");
        crc_data_0.magic = CRC_MAGIC;
        crc_data_0.crc_spiffs = crc_current;
        crc_data_0.size_spiffs = spiffs->pos.size;
        write_crc_partition(crc_0_part, &crc_data_0);
    }
}

static void crc_init_if_empty()
{
    bool init_needed = !crc_initialized();

    if (!init_needed) return;

    ESP_LOGW(TAG, "Initializing CRC partitions...");

    // SPIFFS CRC initialisieren
    const esp_partition_info_t *spiffs = find_partition(PART_TYPE_DATA, PART_SUBTYPE_DATA_SPIFFS);
    if (spiffs) {
        uint32_t crc_spiffs = flash_crc(spiffs->pos.offset, spiffs->pos.size);
        crc_data_0.crc_spiffs = crc_spiffs;
        crc_data_0.size_spiffs = spiffs->pos.size;
    }

    // crc_data_1 identisch setzen
    crc_data_1 = crc_data_0;

    // Schreibe beide Partitionen
    if (crc_0_part) write_crc_partition(crc_0_part, &crc_data_0);
    if (crc_1_part) write_crc_partition(crc_1_part, &crc_data_1);
}



/* -----------------------------------------------------------
   Bootloader
----------------------------------------------------------- */

static int select_partition_number(bootloader_state_t *bs)
{
    if (!bootloader_utility_load_partition_table(bs)) return INVALID_INDEX;
    return bootloader_utility_get_selected_boot_partition(bs);
}


void __attribute__((noreturn)) call_start_cpu0(void)
{
    // Watchdog Timeout setzen
    #define RTC_WDT_TIMEOUT_SEC 5
    #define RTC_CLK_FREQ 150000
    uint32_t timeout_cycles = RTC_WDT_TIMEOUT_SEC * RTC_CLK_FREQ;
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, RTC_CNTL_WDT_WKEY_VALUE);
    REG_WRITE(RTC_CNTL_WDTCONFIG0_REG, timeout_cycles);
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0);
	ESP_LOGI(TAG, "Safe Bootloader start: delay ...");
	
	// delay for tests only
	/*
	for (uint32_t i = 0; i < 600000; i++) {

        // RTC WDT feed
        REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, RTC_CNTL_WDT_WKEY_VALUE);
        REG_WRITE(RTC_CNTL_WDTFEED_REG, 1);
        REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0);

        // TIMG WDT feed (Bootloader nutzt auch diesen)
        REG_WRITE(TIMG_WDTWPROTECT_REG(0), TIMG_WDT_WKEY_VALUE);
        REG_WRITE(TIMG_WDTFEED_REG(0), 1);

        ets_delay_us(100);
    }
	*/

    if (bootloader_init() != ESP_OK) bootloader_reset();
    bootloader_state_t bs = {0};

    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) bootloader_reset();

    load_partition_table();
    crc_read();
	
	crc_init_if_empty();
 
    handle_ota_sync(boot_index);
    handle_spiffs();

    ESP_LOGI(TAG, "Loading app..");

    bootloader_utility_load_boot_image(&bs, boot_index);
}


struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}