// Linked only when generated/ is absent (no recompiled U-King.rpx yet). The tables are empty, so the
// process can still start the host window and the Vulkan device. Re-run CMake after tools/recompile.py.
#include "guest_table.h"

const GuestFnEntry g_guest_functions[] = {{0, nullptr}};
const uint32_t g_guest_function_count = 0;
const GuestImportEntry g_guest_imports[] = {{0, "", "", false, nullptr}};
const uint32_t g_guest_import_count = 0;
