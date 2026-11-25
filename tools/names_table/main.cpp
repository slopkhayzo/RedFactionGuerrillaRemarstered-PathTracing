// rfgrt's names table for an OpenRFG64 build (src/shared/openrfg64_column.h), a post-build step of a game that builds
// the plugin (OpenRFG64 VOM): every value of game/rfg_symbols_names.generated.h looked up in the program's PDB once, so
// that the plugin needs neither the PDB nor DbgHelp at start-up (game/openrfg64.cpp reads the table).
//   rfgrt_names <OpenRFG.exe> <rfgrt.names, beside the plugin>
#include <iterator>

#include "game/rfg_symbols_names.generated.h"
#include "openrfg64_column.h"

int wmain(int argc, wchar_t** argv) {
    const openrfg64::List lists[] = {{rfg::kOpenRFG64Names, std::size(rfg::kOpenRFG64Names)}};
    return openrfg64::write_table_main(argc, argv, lists, std::size(lists));
}
