// libFuzzer target for the .ignispak package reader. Malformed packages must
// fail safely: no crash, no unbounded allocation, consistent errors.
// The input is written to a temp file outside the project tree; the reader's
// in-memory index path is what is being fuzzed.

#include "ignis/core/pak.h"

#include <cstdint>
#include <cstdlib>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 4u * 1024u * 1024u) return 0;
    char path[] = "/tmp/ignis_fuzz_pak_XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return 0;
    size_t off = 0;
    while (off < size) {
        const ssize_t w = write(fd, data + off, size - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
    close(fd);

    ignis::PakReader r;
    if (r.open(path).ok()) {
        for (const auto& name : r.list()) {
            if (name.size() > 1024) continue;
            auto bytes = r.read(name);
            (void)bytes; // expected failures are normal outcomes
        }
    }
    unlink(path);
    return 0;
}
