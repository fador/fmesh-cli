#include "keybinds.h"

namespace meshcli {

int decode_alt(int second_ch) {
    if (second_ch <= 0) return 0;
    return second_ch | 0x80;
}

} // namespace meshcli
