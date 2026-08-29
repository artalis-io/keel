// F2-A freestanding-umbrella C++ TU: a separate translation unit that includes ONLY the freestanding
// umbrella and calls into the C archive, proving the freestanding umbrella's extern "C" guards and
// multi-TU C++ linkage against libkeel.a.
#include <keel/freestanding.h>

const char *fs_probe(void) {
    (void)kl_version_number();
    return kl_version();
}
