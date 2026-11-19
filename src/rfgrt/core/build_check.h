#pragma once
#include <string>

// Which known rfg.exe build is running: the index into rfg::kBuilds, or -1. A build counts only if its PDB GUID and
// age (the compiled code) match and its PE machine type is this plugin's architecture. `detail` describes the result.
int identify_build(std::string& detail);
