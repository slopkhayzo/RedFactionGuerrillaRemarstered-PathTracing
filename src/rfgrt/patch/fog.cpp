#include "patch/fog.h"

#include <cctype>
#include <cstring>

#include "patch/dxbc.h"

namespace fog {
namespace {

using dxbc::Instruction;
using dxbc::Operand;

constexpr uint32_t kFtoi = 27;
constexpr uint32_t kLt = 49;
constexpr uint32_t kDclInputPs = 98;
constexpr uint32_t kDclInputPsSgv = 99;
constexpr uint32_t kImm1 = 0x00004001u;  // a 1-component immediate32 operand
constexpr uint32_t kImm4 = 0x00004002u;  // a 4-component one
constexpr uint32_t kOne = 0x3F800000u;   // 1.0f

bool is_declaration(uint32_t op) { return (op >= dxbc::kDclResource && op <= dxbc::kDclGlobalFlags) || op == dxbc::kCustomData; }
bool is_sample(uint32_t op) {
    return op == dxbc::kSample || op == dxbc::kSampleC || op == dxbc::kSampleCLz || op == dxbc::kSampleL ||
           op == dxbc::kSampleD || op == dxbc::kSampleB || op == dxbc::kLd;
}
int dot_width(uint32_t op) { return op == dxbc::kDp2 ? 2 : op == dxbc::kDp3 ? 3 : op == dxbc::kDp4 ? 4 : 0; }

uint32_t swz(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a | b << 2 | c << 4 | d << 6; }
uint32_t op_token(uint32_t opcode, uint32_t length) { return opcode | (length << 24); }
uint32_t temp_dst(uint32_t mask) { return 0x00100002u | (mask << 4); }
uint32_t temp_src(uint32_t s) { return 0x00100006u | (s << 4); }
uint32_t input_src(uint32_t s) { return 0x00101006u | (s << 4); }
uint32_t resource_src(uint32_t s) { return 0x00107006u | (s << 4); }

bool contains_nocase(const std::string& s, const char* what) {
    std::string a = s, b = what;
    for (auto& ch : a) ch = char(tolower(uint8_t(ch)));
    for (auto& ch : b) ch = char(tolower(uint8_t(ch)));
    return a.find(b) != std::string::npos;
}

// The components of a source operand an instruction reads (bits xyzw): per component of its destination for the
// component-wise ones, the first n for a dot product, all four it names otherwise (sampling, several destinations).
uint32_t reads_of(const Instruction& in, const Operand& o) {
    if (o.components == 1) return 1u;
    if (o.components != 4) return 0u;
    if (o.mode == 2) return 1u << o.swizzle[0];
    uint32_t m = 0;
    if (const int w = dot_width(in.opcode)) {
        for (int j = 0; j < w; ++j) m |= 1u << o.swizzle[j];
        return m;
    }
    if (dxbc::destinations(in.opcode) == 1 && !is_sample(in.opcode) && !in.operands.empty() && in.operands[0].components == 4 &&
        in.operands[0].mode == 0) {
        for (int c = 0; c < 4; ++c)
            if (in.operands[0].mask & (1u << c)) m |= 1u << o.swizzle[c];
        return m;
    }
    for (int j = 0; j < 4; ++j) m |= 1u << o.swizzle[j];
    return m;
}

// 1.0 in every component the instruction reads of an immediate operand.
bool is_one(const dxbc::Program& p, const Instruction& in, const Operand& o) {
    if (o.type != dxbc::kImmediate32) return false;
    if (o.components == 1) return p.tokens[in.offset + o.first + 1] == kOne;
    if (o.components != 4) return false;
    const uint32_t m = reads_of(in, o);
    for (int c = 0; c < 4; ++c)
        if ((m & (1u << c)) && p.tokens[in.offset + o.first + 1 + uint32_t(c)] != kOne) return false;
    return m != 0;
}

int single(uint32_t m) {
    for (int c = 0; c < 4; ++c)
        if (m == (1u << c)) return c;
    return -1;
}

// The input register the signature gives SV_Position (ISGN: elements of 24 bytes after a count and a field), or none.
uint32_t position_register(const dxbc::Container& c) {
    const dxbc::Chunk* isgn = c.find("ISGN");
    if (!isgn || isgn->data.size() < 8) return UINT32_MAX;
    const uint8_t* d = isgn->data.data();
    uint32_t count = 0;
    memcpy(&count, d, 4);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 8 + size_t(i) * 24;
        if (at + 24 > isgn->data.size()) break;
        uint32_t sv = 0, reg = 0;
        memcpy(&sv, d + at + 8, 4);
        memcpy(&reg, d + at + 16, 4);
        if (sv == 1) return reg;  // D3D_NAME_POSITION
    }
    return UINT32_MAX;
}

}  // namespace

const char* name(Result r) {
    switch (r) {
        case Result::NotPixelShader: return "not a pixel shader";
        case Result::NoFog: return "no fog";
        case Result::Patched: return "patched";
        default: return "unsupported";
    }
}

Result patch(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report) {
    Report local;
    Report& r = report ? *report : local;
    auto fail = [&](const std::string& why) {
        r.result = Result::Unsupported;
        r.reason = why;
        return r.result;
    };
    dxbc::Container c;
    std::string error;
    if (!dxbc::parse(bytes, size, &c, &error)) return fail(error);
    dxbc::Chunk* shdr = c.find("SHDR");
    if (!shdr) shdr = c.find("SHEX");
    if (!shdr) return r.result = Result::NotPixelShader;
    dxbc::Program p;
    if (!dxbc::decode(*shdr, &p, &error)) return fail("decode: " + error);
    if ((p.version >> 16) != 0) return r.result = Result::NotPixelShader;

    bool common = false;  // cb0 is the common fragment constants (fogColor at cb0[4])
    for (const dxbc::Binding& b : dxbc::bindings(c))
        if (b.type == 0 && b.slot == 0 && contains_nocase(b.name, "commonFragment")) common = true;

    uint32_t temps = 0, position = UINT32_MAX;
    int temps_at = -1, first_input = -1, last_resource = -1, first_other_dcl = -1, first_code = -1;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (!is_declaration(in.opcode)) {
            if (first_code < 0) first_code = int(i);
            continue;
        }
        if (in.opcode == dxbc::kDclResource && !in.operands.empty()) {
            if (in.operands[0].index[0] == kFogSlot) return fail("t18 is in use");
            last_resource = int(i);
        } else if (in.opcode == dxbc::kDclTemps && in.length == 2) {
            temps = p.tokens[in.offset + 1];
            temps_at = int(i);
        } else if (in.opcode == dxbc::kDclInputPsSiv && !in.operands.empty() && p.tokens[in.offset + in.length - 1] == 1) {
            if ((in.operands[0].mask & 0x3u) != 0x3u) return fail("the position declared without its xy");
            position = in.operands[0].index[0];
        }
        if (in.opcode == kDclInputPs && first_input < 0) first_input = int(i);
        if ((in.opcode == kDclInputPs || in.opcode == kDclInputPsSgv || in.opcode == dxbc::kDclInputPsSiv ||
             in.opcode == dxbc::kDclOutput || in.opcode == dxbc::kDclTemps) &&
            first_other_dcl < 0)
            first_other_dcl = int(i);
    }
    if (first_code < 0) return r.result = Result::NoFog;

    // The fog: `add d, -v.c, l(1)` (either order), one input component.
    struct Use {
        int at;
        size_t k;
    };
    std::vector<Use> uses;
    int input = -1, comp = -1;
    for (size_t i = size_t(first_code); i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (in.opcode != dxbc::kAdd || in.operands.size() != 3 || in.operands[0].type != dxbc::kTemp) continue;
        for (size_t k = 1; k < 3; ++k) {
            const Operand& v = in.operands[k];
            if (v.type != dxbc::kInput || !v.neg || v.abs || v.dims != 1 || v.relative) continue;
            const int cc = single(reads_of(in, v));
            if (cc < 0 || !is_one(p, in, in.operands[3 - k])) continue;
            if (input >= 0 && (input != int(v.index[0]) || comp != cc)) return fail("two inputs read as 1 - fog");
            input = int(v.index[0]);
            comp = cc;
            uses.push_back({int(i), k});
        }
    }
    if (input < 0) return r.result = Result::NoFog;

    // Every other read of that component: a `mad` with the fog's colour (cb0[4]) only.
    for (size_t i = size_t(first_code); i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        const int dsts = dxbc::destinations(in.opcode);
        for (size_t k = size_t(dsts < 0 ? 0 : dsts); k < in.operands.size(); ++k) {
            const Operand& o = in.operands[k];
            if (o.type != dxbc::kInput || int(o.index[0]) != input) continue;
            const uint32_t m = reads_of(in, o);
            if (!(m & (1u << comp))) continue;
            bool known = false;
            for (const Use& u : uses) known |= u.at == int(i) && u.k == k;
            if (known) continue;
            if (m != (1u << comp)) return fail("the fog's component read with others: " + dxbc::describe(p, in));
            if (o.dims != 1 || o.relative) return fail("the fog read with relative indexing");
            bool fog_colour = false;
            if (in.opcode == dxbc::kMad && in.operands.size() == 4 && (k == 1 || k == 2)) {
                const Operand& f = in.operands[k == 1 ? 2 : 1];
                fog_colour = f.type == dxbc::kConstantBuffer && f.index[0] == 0 && f.index[1] == 4 && !f.relative;
            }
            if (!fog_colour) return fail("the fog's component read elsewhere: " + dxbc::describe(p, in));
            if (!common) return r.result = Result::NoFog;  // cb0[4] is something else's
            r.colour = true;
            uses.push_back({int(i), k});
        }
    }
    r.input = uint32_t(input);
    r.component = uint32_t(comp);
    r.reads = uint32_t(uses.size());

    if (position == UINT32_MAX) {
        position = position_register(c);
        if (position == UINT32_MAX) return fail("no pixel position in the input signature");
        if (first_input < 0) return fail("no input declarations to put the position's before");
    } else {
        first_input = -1;  // declared already
    }
    if (temps_at < 0) return fail("no temps declared");
    const int resource_at = last_resource >= 0 ? last_resource : -1;  // the dcl after it (or before the first other's)
    if (resource_at < 0 && first_other_dcl < 0) return fail("no place for t18's declaration");

    const uint32_t tn = temps;
    // dcl_resource_texture2d (float,float,float,float) t18
    const std::vector<uint32_t> dcl_t18 = {op_token(dxbc::kDclResource, 4) | (3u << 11), 0x00107000u, kFogSlot, 0x00005555u};
    // dcl_input_ps_siv linear noperspective v<position>.xy, position
    const std::vector<uint32_t> dcl_position = {op_token(dxbc::kDclInputPsSiv, 4) | (4u << 11), 0x00101032u, position, 1u};
    const std::vector<uint32_t> load = {
        op_token(kFtoi, 5), temp_dst(0x3), tn, input_src(swz(0, 1, 0, 0)), position,
        op_token(dxbc::kMov, 8), temp_dst(0xC), tn, kImm4, 0, 0, 0, 0,
        op_token(dxbc::kLd, 7), temp_dst(0x8), tn, temp_src(swz(0, 1, 2, 3)), tn, resource_src(swz(0, 1, 2, 3)), kFogSlot,
        op_token(kLt, 7), temp_dst(0x8), tn, kImm1, 0u, temp_src(swz(3, 3, 3, 3)), tn,
        op_token(dxbc::kMovc, 9), temp_dst(0x1), tn, temp_src(swz(3, 3, 3, 3)), tn, kImm1, 0u,
        input_src(swz(uint32_t(comp), uint32_t(comp), uint32_t(comp), uint32_t(comp))), uint32_t(input),
    };

    std::vector<uint32_t> t;
    t.reserve(p.tokens.size() + 64);
    const uint32_t length = p.tokens[1];
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (resource_at < 0 && int(i) == first_other_dcl) t.insert(t.end(), dcl_t18.begin(), dcl_t18.end());
        if (int(i) == first_input) t.insert(t.end(), dcl_position.begin(), dcl_position.end());
        if (int(i) == first_code) t.insert(t.end(), load.begin(), load.end());
        const size_t base = t.size();
        t.insert(t.end(), p.tokens.begin() + in.offset, p.tokens.begin() + in.offset + in.length);
        if (int(i) == temps_at) t[t.size() - 1] = temps + 1;
        if (int(i) == resource_at) t.insert(t.end(), dcl_t18.begin(), dcl_t18.end());
        for (const Use& u : uses) {
            if (u.at != int(i)) continue;
            const Operand& o = in.operands[u.k];
            // the same selection, modifiers and component count, of rN.x: a temp, one immediate index
            t[base + o.first] = ((t[base + o.first] & 0x80000FFFu) & ~0xFF0u) | (1u << 20);
            t[base + o.first + o.length - 1] = tn;
        }
    }
    std::vector<uint32_t> program;
    program.push_back(p.tokens[0]);
    program.push_back(uint32_t(2 + t.size()));
    program.insert(program.end(), t.begin(), t.end());
    for (size_t i = length; i < p.tokens.size(); ++i) program.push_back(p.tokens[i]);
    shdr->data.assign(reinterpret_cast<const uint8_t*>(program.data()),
                      reinterpret_cast<const uint8_t*>(program.data()) + program.size() * 4);
    dxbc::Program check;
    if (!dxbc::decode(*shdr, &check, &error)) return fail("the rewritten program does not decode: " + error);
    const size_t added = 5 + 1 + (first_input >= 0 ? 1 : 0);
    if (check.code.size() != p.code.size() + added) return fail("the rewritten program has the wrong instruction count");
    if (out) *out = dxbc::serialize(c);
    r.result = Result::Patched;
    return r.result;
}

}  // namespace fog
