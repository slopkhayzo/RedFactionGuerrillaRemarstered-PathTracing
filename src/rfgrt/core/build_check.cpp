#include "core/build_check.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "game/rfg.h"

namespace {

struct CodeViewRsds {
    DWORD signature;  // 'RSDS'
    GUID guid;
    DWORD age;
};

#if defined(_WIN64)
constexpr WORD kOwnMachine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr WORD kOwnMachine = IMAGE_FILE_MACHINE_I386;
#endif

}  // namespace

int identify_build(std::string& detail) {
    auto base = reinterpret_cast<const uint8_t*>(rfg::base());
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        detail = "no MZ header";
        return -1;
    }
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);  // native: rfg.exe has our bitness
    const DWORD ts = nt->FileHeader.TimeDateStamp;
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    const WORD machine = nt->FileHeader.Machine;

    const CodeViewRsds* rsds = nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    auto dbg = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    for (DWORD i = 0; dir.VirtualAddress && i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        if (dbg[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW && dbg[i].AddressOfRawData) {
            auto cv = reinterpret_cast<const CodeViewRsds*>(base + dbg[i].AddressOfRawData);
            if (cv->signature == 0x53445352) rsds = cv;  // "RSDS"
        }
    }

    int found = -1;
    for (int i = 0; rsds && i < rfg::kBuildCount; ++i)
        if (rfg::kBuilds[i].machine == machine && machine == kOwnMachine &&
            memcmp(&rsds->guid, rfg::kBuilds[i].pdb_guid, 16) == 0 && rsds->age == rfg::kBuilds[i].pdb_age)
            found = i;
    const rfg::BuildId* b = found >= 0 ? &rfg::kBuilds[found] : nullptr;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "%s build: machine %04X (plugin %04X), pdb guid/age %s (the compiled code; required); timestamp %08lX (%s), "
             "image size %08lX (%s)",
             b ? b->name : "unknown", machine, kOwnMachine, rsds ? (b ? "ok" : "MISMATCH") : "missing", ts,
             b && ts == b->exe_timestamp ? "ok" : "differs", size, b && size == b->exe_size_of_image ? "ok" : "differs");
    detail = buf;
    return found;
}
