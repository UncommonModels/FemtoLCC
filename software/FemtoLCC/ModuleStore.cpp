#include "ModuleStore.h"
#include <Arduino.h>
#include <string.h>
#include <esp_rom_crc.h>
#include "Notice.h"

bool ModuleStore::begin() {
    partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!partition_ || partition_->size < sizeof(Header) + CAPACITY) {
        partition_ = nullptr;
        return false;
    }
    data_ = (uint8_t*)calloc(CAPACITY + 1, 1);
    if (!data_) {
        return false;
    }
    Header h;
    if (esp_partition_read(partition_, 0, &h, sizeof(h)) != ESP_OK || h.magic != MAGIC || h.length > CAPACITY) {
        return true;    // never written, or not ours: start empty
    }
    if (esp_partition_read(partition_, sizeof(h), data_, h.length) != ESP_OK ||
        esp_rom_crc32_le(0, data_, h.length) != h.crc) {
        memset(data_, 0, CAPACITY);
        Serial.println(F("module description damaged - starting empty"));
    }
    return true;
}

uint32_t ModuleStore::length() const {
    return data_ ? (uint32_t)strnlen((const char*)data_, CAPACITY) : 0;
}

uint32_t ModuleStore::crc() const {
    return data_ ? esp_rom_crc32_le(0, data_, length()) : 0;
}

size_t ModuleStore::read(uint32_t address, uint8_t* out, size_t len) {
    memcpy(out, data_ + address, len);
    return len;
}

uint16_t ModuleStore::write(uint32_t address, const uint8_t* data, size_t len) {
    memcpy(data_ + address, data, len);
    dirty_ = true;
    lastWrite_ = millis();
    return 0;
}

void ModuleStore::poll() {
    if (dirty_ && millis() - lastWrite_ >= COMMIT_DELAY_MS) {
        commit();
    }
}

// Header first with the new length and CRC, then the text; only the sectors
// the text needs are erased.
void ModuleStore::commit() {
    if (!dirty_ || !partition_) {
        return;
    }
    dirty_ = false;
    const uint32_t len = length();
    const Header h = { MAGIC, len, esp_rom_crc32_le(0, data_, len), 0 };
    const uint32_t sector = partition_->erase_size;
    const uint32_t span = (sizeof(h) + len + sector - 1) / sector * sector;
    if (esp_partition_erase_range(partition_, 0, span) != ESP_OK ||
        esp_partition_write(partition_, 0, &h, sizeof(h)) != ESP_OK ||
        (len && esp_partition_write(partition_, sizeof(h), data_, len) != ESP_OK)) {
        notice("module description: flash write failed\n");
        return;
    }
    notice("module description saved, %lu bytes\n", (unsigned long)len);
}

void ModuleStore::clear() {
    if (!data_) {
        return;
    }
    memset(data_, 0, CAPACITY);
    dirty_ = true;
    commit();
}
