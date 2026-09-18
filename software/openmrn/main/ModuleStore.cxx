// Module storage, memory space 0xE1. See ModuleStore.hxx.
//
//   Uncommon Models — https://uncommonmodels.com

#include "ModuleStore.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduino.h"

/// Where the description is kept, on the SPIFFS mounted at /fs.
static const char *MODULE_FILENAME = "/fs/module.txt";

ModuleStore::ModuleStore(OSMutex *lock)
    : data_(nullptr)
    , dirty_(false)
    , lastWrite_(0)
    , lock_(lock)
{
}

ModuleStore::~ModuleStore()
{
    free(data_);
}

bool ModuleStore::begin()
{
    // One spare byte so the text is always NUL-terminated however full it is.
    data_ = (uint8_t *)calloc(CAPACITY + 1, 1);
    if (!data_)
    {
        return false;
    }

    FILE *f = fopen(MODULE_FILENAME, "rb");
    if (!f)
    {
        return true;        // never written: start empty
    }
    const size_t got = fread(data_, 1, CAPACITY, f);
    fclose(f);
    if (got > 0)
    {
        printf("module description: %u bytes loaded\n", (unsigned)got);
    }
    return true;
}

uint32_t ModuleStore::length() const
{
    return data_ ? (uint32_t)strnlen((const char *)data_, CAPACITY) : 0;
}

size_t ModuleStore::read(address_t source, uint8_t *dst, size_t len,
    errorcode_t *error, Notifiable *again)
{
    OSMutexLock l(lock_);
    if (!data_ || source >= CAPACITY)
    {
        *error = openlcb::Defs::ERROR_PERMANENT;
        return 0;
    }
    if (source + len > CAPACITY)
    {
        len = CAPACITY - source;
    }
    memcpy(dst, data_ + source, len);
    *error = 0;
    return len;
}

size_t ModuleStore::write(address_t destination, const uint8_t *data,
    size_t len, errorcode_t *error, Notifiable *again)
{
    OSMutexLock l(lock_);
    if (!data_ || destination >= CAPACITY)
    {
        *error = openlcb::Defs::ERROR_PERMANENT;
        return 0;
    }
    if (destination + len > CAPACITY)
    {
        len = CAPACITY - destination;
    }
    memcpy(data_ + destination, data, len);
    dirty_ = true;
    lastWrite_ = millis();
    *error = 0;
    return len;
}

void ModuleStore::poll()
{
    OSMutexLock l(lock_);
    if (dirty_ && millis() - lastWrite_ >= COMMIT_DELAY_MS)
    {
        commit();
    }
}

void ModuleStore::commit()
{
    // Recursive: poll() and clear() already hold it.
    OSMutexLock l(lock_);
    if (!dirty_ || !data_)
    {
        return;
    }
    dirty_ = false;

    const uint32_t len = length();
    FILE *f = fopen(MODULE_FILENAME, "wb");
    if (!f)
    {
        printf("module description: cannot open %s for writing\n",
            MODULE_FILENAME);
        return;
    }
    const size_t put = len ? fwrite(data_, 1, len, f) : 0;
    fclose(f);
    if (put != len)
    {
        printf("module description: write failed\n");
        return;
    }
    printf("module description saved, %u bytes\n", (unsigned)len);
}

void ModuleStore::clear()
{
    OSMutexLock l(lock_);
    if (!data_)
    {
        return;
    }
    memset(data_, 0, CAPACITY);
    dirty_ = true;
    commit();
}
