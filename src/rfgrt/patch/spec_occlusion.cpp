#include "patch/spec_occlusion.h"

#include <cstring>

#include "patch/dxbc.h"

namespace spec_occlusion {
namespace {

using dxbc::Instruction;
using dxbc::Operand;

constexpr uint32_t kIrradianceSlot = 14;  // as ambient::kIrradianceSlot
constexpr uint32_t kDiv = 14, kFtoi = 27, kSaturate = 1u << 13;
constexpr uint32_t kImm4 = 0x00004002u;

bool is_declaration(uint32_t op) { return (op >= dxbc::kDclResource && op <= dxbc::kDclGlobalFlags) || op == dxbc::kCustomData; }
uint32_t swz(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a | b << 2 | c << 4 | d << 6; }
uint32_t op_token(uint32_t opcode, uint32_t length) { return opcode | (length << 24); }
uint32_t temp_dst(uint32_t mask) { return 0x00100002u | (mask << 4); }
uint32_t temp_src(uint32_t s) { return 0x00100006u | (s << 4); }
uint32_t input_src(uint32_t s) { return 0x00101006u | (s << 4); }
uint32_t resource_src(uint32_t s) { return 0x00107006u | (s << 4); }
uint32_t cb_src(uint32_t s) { return 0x00208006u | (s << 4); }  // cb<slot>[<register>], both immediate
uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
uint32_t read_component(const Operand& o, int c) {
    if (o.components == 1) return 0;
    if (o.mode == 2) return o.swizzle[0];
    return o.swizzle[c];
}
// cb0[7] read as .z for every component the instruction writes (indirectLightParams.z)
bool reads_spec_scale(const Instruction& in, const Operand& o) {
    if (o.type != dxbc::kConstantBuffer || o.index[0] != 0 || o.index[1] != 7 || o.relative || o.dims != 2) return false;
    const uint32_t mask = in.operands.empty() ? 0u : in.operands[0].mask;
    if (!mask) return false;
    for (int c = 0; c < 4; ++c)
        if ((mask & (1u << c)) && read_component(o, c) != 2) return false;
    return true;
}

}  // namespace

const char* name(Result r) {
    switch (r) {
        case Result::NotPixelShader: return "not a pixel shader";
        case Result::NoSpecularAmbient: return "no specular ambient";
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

    uint32_t temps = 0, position = UINT32_MAX;
    int temps_at = -1;
    bool has_t14 = false;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (in.opcode == dxbc::kDclResource && !in.operands.empty() && in.operands[0].index[0] == kIrradianceSlot) has_t14 = true;
        if (in.opcode == dxbc::kDclTemps && in.length == 2) {
            temps = p.tokens[in.offset + 1];
            temps_at = int(i);
        }
        if (in.opcode == dxbc::kDclInputPsSiv && !in.operands.empty() && p.tokens[in.offset + in.length - 1] == 1)
            position = in.operands[0].index[0];
    }
    // the reads of the specular ambient's scale (an arithmetic instruction's source, destination a temp)
    std::vector<int> uses;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (is_declaration(in.opcode) || in.operands.size() < 2 || in.operands[0].type != dxbc::kTemp) continue;
        for (size_t k = 1; k < in.operands.size(); ++k)
            if (reads_spec_scale(in, in.operands[k])) {
                uses.push_back(int(i));
                break;
            }
    }
    if (uses.empty()) return r.result = Result::NoSpecularAmbient;
    if (!has_t14) return fail("no irradiance (t14): not GI-patched");
    if (position == UINT32_MAX) return fail("no pixel position input");
    if (temps_at < 0) return fail("no temps declared");

    const uint32_t tn = temps;
    const uint32_t l0 = bits(0.2126f), l1 = bits(0.7152f), l2 = bits(0.0722f), tiny = bits(1e-4f), one = bits(1.0f);
    const std::vector<uint32_t> block = {
        // the irradiance and its alpha
        op_token(kFtoi, 5), temp_dst(0x3), tn, input_src(swz(0, 1, 0, 0)), position,
        op_token(dxbc::kMov, 8), temp_dst(0xC), tn, kImm4, 0, 0, 0, 0,
        op_token(dxbc::kLd, 7), temp_dst(0xF), tn, temp_src(swz(0, 1, 2, 3)), tn, resource_src(swz(0, 1, 2, 3)), kIrradianceSlot,
        // k = saturate(luminance(irradiance) / luminance(ambient)), 1 where nothing was traced, times cb0[7].z
        op_token(dxbc::kDp3, 10), temp_dst(0x1), tn, temp_src(swz(0, 1, 2, 0)), tn, kImm4, l0, l1, l2, 0,
        op_token(dxbc::kDp3, 11), temp_dst(0x2), tn, cb_src(swz(0, 1, 2, 0)), 0, 2, kImm4, l0, l1, l2, 0,
        op_token(dxbc::kMax, 10), temp_dst(0x2), tn, temp_src(swz(1, 1, 1, 1)), tn, kImm4, tiny, tiny, tiny, tiny,
        op_token(kDiv, 7) | kSaturate, temp_dst(0x1), tn, temp_src(swz(0, 0, 0, 0)), tn, temp_src(swz(1, 1, 1, 1)), tn,
        op_token(dxbc::kMovc, 12), temp_dst(0x1), tn, temp_src(swz(3, 3, 3, 3)), tn, temp_src(swz(0, 0, 0, 0)), tn, kImm4, one, one,
        one, one,
        op_token(dxbc::kMul, 8), temp_dst(0x1), tn, temp_src(swz(0, 0, 0, 0)), tn, cb_src(swz(2, 2, 2, 2)), 0, 7,
    };
    const size_t added = 9;
    // an instruction with its cb0[7].z operands reading rN.x (modifiers kept)
    auto reread = [&](const Instruction& in) {
        const uint32_t* tk = p.tokens.data() + in.offset;
        std::vector<uint32_t> o2(tk, tk + in.operands[0].first);
        for (const Operand& o : in.operands) {
            if (&o != &in.operands[0] && reads_spec_scale(in, o)) {
                o2.push_back((tk[o.first] & 0x80000000u) | temp_src(swz(0, 0, 0, 0)));
                uint32_t at = o.first;
                bool ext = (tk[at] >> 31) != 0;
                while (ext) {
                    ++at;
                    o2.push_back(tk[at]);
                    ext = (tk[at] >> 31) != 0;
                }
                o2.push_back(tn);
            } else {
                o2.insert(o2.end(), tk + o.first, tk + o.first + o.length);
            }
        }
        o2[0] = (o2[0] & ~(0x7Fu << 24)) | (uint32_t(o2.size()) << 24);
        return o2;
    };
    if (p.tokens[p.code[size_t(uses.front())].offset] >> 31) return fail("an extended opcode");
    std::vector<uint32_t> t;
    t.reserve(p.tokens.size() + block.size() + 8 * uses.size());
    const uint32_t length = p.tokens[1];
    size_t next = 0;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (int(i) == uses.front()) t.insert(t.end(), block.begin(), block.end());
        if (next < uses.size() && int(i) == uses[next]) {
            ++next;
            if (p.tokens[in.offset] >> 31) return fail("an extended opcode");
            const std::vector<uint32_t> o2 = reread(in);
            t.insert(t.end(), o2.begin(), o2.end());
            continue;
        }
        t.insert(t.end(), p.tokens.begin() + in.offset, p.tokens.begin() + in.offset + in.length);
        if (int(i) == temps_at) t[t.size() - 1] = tn + 1;
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
    if (check.code.size() != p.code.size() + added) return fail("the rewritten program has the wrong instruction count");
    if (out) *out = dxbc::serialize(c);
    r.result = Result::Patched;
    return r.result;
}

}  // namespace spec_occlusion
