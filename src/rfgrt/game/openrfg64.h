#pragma once
// OpenRFG64 (a 64-bit OpenRFG: gamerscruffy/OpenRFG64, with OpenRFG64-memtest's changes) as a build of rfg.exe: no
// table of its addresses is kept, because every rebuild moves them. The program's own PDB (OpenRFG.pdb, next to it)
// is read at start-up through DbgHelp and every rfg::rva / rfg::off / rfg::size value is looked up by the same name
// as in rfg.pdb (OpenRFG keeps rfg.pdb's names). Added by OpenRFG64-memtest; not in the upstream plugin.
#include <cstdint>
#include <string>

namespace rfg::openrfg64 {

// The build index select_build() takes for OpenRFG64 (one past the table's builds).
int build_index();

// The running program is OpenRFG64: an x64 image whose CodeView record names OpenRFG.pdb.
bool detected(const char* pdb_path, uint16_t machine);

// Fills the values from the program's PDB. Returns how many names were not found or were ambiguous (their values
// stay 0: rfg::fn/addr callers must treat a 0 RVA as missing); report gets one line per problem and a summary.
int load(std::string& report);

}  // namespace rfg::openrfg64
