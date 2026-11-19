#pragma once
// DXBC shader containers and their Shader Model 4 bytecode: enough to read the game's pixel shaders (ps_4_0), find
// values in them by dataflow, insert instructions and declarations, and write them back with a valid checksum
// (research/docs/04 section 6.1). The token layout is the one of the D3D10/11 tokenized program format.
#include <cstdint>
#include <string>
#include <vector>

namespace dxbc {

// A container: its chunks, by four-character code.
struct Chunk {
    uint32_t fourcc;
    std::vector<uint8_t> data;
};
struct Container {
    std::vector<Chunk> chunks;
    Chunk* find(const char (&fourcc)[5]);
    const Chunk* find(const char (&fourcc)[5]) const;
};
bool parse(const void* bytes, size_t size, Container* out, std::string* error);
std::vector<uint8_t> serialize(const Container& c);  // with its checksum
// The container's checksum (a variant of MD5 over everything after the hash field), as stored at bytes 4..19.
void checksum(const uint8_t* container, size_t size, uint32_t out[4]);

// --- Shader Model 4 bytecode ---------------------------------------------------------------------------------------

enum Opcode : uint32_t {
    kAdd = 0, kDiscard = 13, kDp2 = 15, kDp3 = 16, kDp4 = 17, kIf = 31, kLd = 45, kMad = 50, kMin = 51, kMax = 52,
    kCustomData = 53, kMov = 54, kMovc = 55, kMul = 56, kRet = 62, kRetc = 63, kSample = 69, kSampleC = 70,
    kSampleCLz = 71, kSampleL = 72, kSampleD = 73, kSampleB = 74, kSincos = 77,
    kDclResource = 88, kDclConstantBuffer = 89, kDclSampler = 90, kDclInputPsSiv = 100, kDclOutput = 101,
    kDclTemps = 104, kDclGlobalFlags = 106,
};
enum OperandType : uint32_t {
    kTemp = 0, kInput = 1, kOutput = 2, kIndexableTemp = 3, kImmediate32 = 4, kImmediate64 = 5, kSampler = 6,
    kResource = 7, kConstantBuffer = 8, kImmediateConstantBuffer = 9,
};

// One operand as the instruction reads or writes it.
struct Operand {
    uint32_t type = 0;
    uint32_t components = 0;   // 0, 1 or 4
    uint32_t mode = 0;         // with 4 components: 0 mask, 1 swizzle, 2 select one
    uint32_t mask = 0;         // write mask (mode 0), xyzw bits
    uint8_t swizzle[4] = {0, 1, 2, 3};  // the source component read for each of x, y, z, w (modes 1 and 2)
    uint32_t dims = 0;         // indices
    uint32_t index[3] = {};    // immediate indices (relative indexing is noted, not followed)
    bool relative = false;
    bool neg = false, abs = false;
    uint32_t first = 0, length = 0;  // its tokens in the program (offset from the instruction's start)
};
struct Instruction {
    uint32_t opcode = 0;
    uint32_t offset = 0;   // in the program, DWORDs from its start (the version token)
    uint32_t length = 0;   // DWORDs
    bool saturate = false;
    std::vector<Operand> operands;  // for instructions (not declarations): destination(s) first, then sources
};
struct Program {
    uint32_t version = 0;           // the version token (program type and model)
    std::vector<uint32_t> tokens;   // the whole program, version and length tokens included
    std::vector<Instruction> code;  // every instruction and declaration, in order
};
bool decode(const Chunk& shdr, Program* out, std::string* error);
// The number of destination operands of an instruction (most have one; a few none or two).
int destinations(uint32_t opcode);
// A readable form of one instruction (opcode number and operands), for reports.
std::string describe(const Program& p, const Instruction& i);

// The resource bindings declared in the RDEF chunk: name -> (type, bind point).
struct Binding {
    std::string name;
    uint32_t type = 0;  // D3D_SIT_*: 0 cbuffer, 2 texture, 3 sampler
    uint32_t slot = 0;
};
std::vector<Binding> bindings(const Container& c);

// A constant buffer's variables as the RDEF chunk describes them, struct members flattened (the game's buffers hold one
// struct each): name, the buffer's bind point, the byte offset in it (register offset / 16, component (offset % 16) / 4).
struct Variable {
    std::string name;
    uint32_t slot = 0;
    uint32_t offset = 0;
};
std::vector<Variable> variables(const Container& c);

}  // namespace dxbc
