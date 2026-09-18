// Module storage: memory space 0xE1.
//
// A layout built from modules keeps each module's part of the track plan on the
// module's own board, so a dispatcher such as olcbweb can put the layout
// together from whatever modules are on the bus. This space holds that
// description: up to 16 KB of text — olcbweb writes JSON — ending at the first
// NUL. The board stores it and hands it back; it never reads it.
//
// Like space 0xE0 this is FemtoLCC's own, reached with the standard Memory
// Configuration commands.
//
// The text is kept in a file on the SPIFFS partition already mounted at /fs for
// the configuration and the generated cdi.xml, rather than in a raw flash
// partition of its own: taking a partition raw would corrupt the file system
// sitting in it. The trade is that there is no CRC of our own — SPIFFS does its
// own checking — and that a factory reset of the file system takes the
// description with it.
//
// Writes collect in RAM and reach flash two seconds after the last one, so a
// dispatcher streaming a description in many datagrams causes one file write,
// not hundreds.
//
//   Uncommon Models — https://uncommonmodels.com

#ifndef _FEMTOLCC_MODULESTORE_HXX_
#define _FEMTOLCC_MODULESTORE_HXX_

#include <stdint.h>

#include "openlcb/MemoryConfig.hxx"
#include "os/OS.hxx"

class ModuleStore : public openlcb::MemorySpace
{
public:
    /// The memory space number a dispatcher addresses.
    static const uint8_t SPACE = 0xE1;

    /// How much text the space holds.
    static const uint32_t CAPACITY = 16384;

    /// @param lock is FemtoController's hardware lock. read() and write() are
    /// served on the stack's executor while poll() and commit() run on the
    /// hardware thread, so the buffer is shared and every entry point takes it.
    explicit ModuleStore(OSMutex *lock);
    ~ModuleStore();

    /// Allocates the buffer and loads whatever was saved. Call after the file
    /// system is mounted. False, and the space reports itself empty, if there
    /// is no memory for the buffer.
    bool begin();

    // --- openlcb::MemorySpace ---------------------------------------------

    address_t max_address() override
    {
        return CAPACITY - 1;
    }

    bool read_only() override
    {
        return false;
    }

    size_t read(address_t source, uint8_t *dst, size_t len, errorcode_t *error,
        Notifiable *again) override;

    size_t write(address_t destination, const uint8_t *data, size_t len,
        errorcode_t *error, Notifiable *again) override;

    // ----------------------------------------------------------------------

    /// Saves the text once it has settled. Called from FemtoController's poll.
    void poll();

    /// Writes the text out now.
    ///
    /// Beware: this writes flash, which disables the flash cache while each
    /// sector is erased and written. The DCC timer interrupt is not resident in
    /// RAM, so calling this while the DCC source is running is a crash — and
    /// one that presents as a random hang rather than an obvious fault.
    /// FemtoController's poll is the only caller and it holds the write back
    /// until the source stops; anything else added later (a console command,
    /// say) must do the same. clear() below has the same hazard.
    void commit();

    /// Empties the description and saves that.
    void clear();

    /// How much text there is: up to the first NUL.
    uint32_t length() const;

private:
    /// Two seconds after the last write.
    static const uint32_t COMMIT_DELAY_MS = 2000;

    uint8_t *data_;
    bool dirty_;
    uint32_t lastWrite_;

    /// Guards data_, dirty_ and lastWrite_; owned by FemtoController.
    OSMutex *lock_;
};

#endif // _FEMTOLCC_MODULESTORE_HXX_
