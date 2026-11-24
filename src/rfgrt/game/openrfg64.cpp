// OpenRFG64-memtest: OpenRFG64 as a build of rfg.exe. The lookups themselves are mods/common/openrfg64_column (shared
// with the converted rfgphys); this file only hands it rfgrt's names.
#include "game/openrfg64.h"

#include <iterator>

#include "game/rfg.h"
#include "game/rfg_symbols_names.generated.h"
#include "openrfg64_column.h"

namespace rfg::openrfg64 {

int build_index() { return kBuildCount; }

bool detected(const char* pdb_path, uint16_t machine) { return ::openrfg64::detected(pdb_path, machine); }

int load(const std::wstring& plugin_dir, std::string& report) {
    const ::openrfg64::List lists[] = {{kOpenRFG64Names, std::size(kOpenRFG64Names)}};
    const std::wstring table = plugin_dir + L"\\rfgrt.names";
    const int missing = ::openrfg64::load_table(lists, std::size(lists), rfg::base(), table.c_str(), nullptr, report);
    if (missing >= 0) return missing;
    return ::openrfg64::load(kOpenRFG64Names, std::size(kOpenRFG64Names), rfg::base(), report);
}

const ::openrfg64::Name* symbol_of(uint32_t rva) {
    if (rfg::build() != build_index() || rva == 0) return nullptr;
    for (const ::openrfg64::Name& n : kOpenRFG64Names)
        if (n.kind == 'f' && n.value && *n.value == rva) return &n;
    return nullptr;
}

}  // namespace rfg::openrfg64
