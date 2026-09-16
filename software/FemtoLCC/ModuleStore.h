// Module storage: memory space 0xE1.
//
// A layout built from modules keeps each module's part of the track plan on
// the module's own board, so a dispatcher such as olcbweb can put the layout
// together from whatever modules are on the bus. This space holds that
// description: up to 16 KB of text - olcbweb writes JSON - ending at the
// first NUL. The board stores it and hands it back; it never reads it.
//
// Like space 0xE0 it is FemtoLCC's own, reached with the standard Memory
// Configuration commands. Writes collect in RAM and reach flash on Update
// Complete, before a reboot, or two seconds after the last write. The bytes
// live in the flash partition the default partition table sets aside for a
// file system, which the firmware does not otherwise use. A factory reset
// leaves them alone; the console's `module clear` erases them.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <AOLCB.h>
#include <esp_partition.h>

class ModuleStore : public AOLCB::MemorySpace {
public:
    static const uint8_t SPACE = 0xE1;
    static const uint32_t CAPACITY = 16384;

    // Find the partition and load what it holds. False, and the space is
    // absent, if there is no partition or no memory for the buffer.
    bool begin();

    uint32_t size() const override { return data_ ? CAPACITY : 0; }
    size_t read(uint32_t address, uint8_t* out, size_t len) override;
    uint16_t write(uint32_t address, const uint8_t* data, size_t len) override;
    void commit() override;

    // Commits a while after the last write. Call every loop.
    void poll();

    // Erase the stored description.
    void clear();

    uint32_t length() const;
    uint32_t crc() const;
    const char* text() const { return data_ ? (const char*)data_ : ""; }
    bool dirty() const { return dirty_; }

private:
    struct Header {
        uint32_t magic;
        uint32_t length;
        uint32_t crc;
        uint32_t reserved;
    };
    static const uint32_t MAGIC = 0x444D4C4F;          // "OLMD"
    static const uint32_t COMMIT_DELAY_MS = 2000;

    const esp_partition_t* partition_ = nullptr;
    uint8_t* data_ = nullptr;                          // CAPACITY + 1, always NUL-terminated
    bool dirty_ = false;
    uint32_t lastWrite_ = 0;
};
