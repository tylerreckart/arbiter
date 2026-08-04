#include "tui/confirm_keys.h"

#include "tui/history_sidebar.h"

namespace arbiter {

int read_confirm_key() {
    while (true) {
        char csi = 0;
        std::string params;
        const int key = read_history_sidebar_key(csi, params);
        if (key < 0) return key;
        if (key == 0x1B && (csi == 'M' || csi == 'm')
            && !params.empty() && params[0] == '<') {
            continue;
        }
        return key;
    }
}

}  // namespace arbiter
