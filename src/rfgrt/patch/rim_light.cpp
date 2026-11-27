#include "patch/rim_light.h"

#include <cstring>

#include "patch/dxbc.h"

namespace rim_light {
namespace {

using dxbc::Instruction;
using dxbc::Operand;

constexpr uint32_t kIrradianceSlot = 14;  // as ambient::kIrradianceSlot
constexpr uint32_t kFtoi = 27;
constexpr uint32_t kImm4 = 0x00004002u;

bool is_declaration(uint32_t op) { return (op >= dxbc::kDclResource && op <= dxbc::kDclGlobalFlags) || op == dxbc::kCustomData; }
uint32_t swz(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a | b << 2 | c << 4 | d << 6; }
uint32_t op_token(uint32_t opcode, uint32_t length) { return opcode | (length << 24); }
uint32_t temp_dst(uint32_t mask) { return 0x00100002u | (mask << 4); }
uint32_t temp_src(uint32_t s) { return 0x00100006u | (s << 4); }
uint32_t input_src(uint32_t s) { return 0x00101006u | (s << 4); }
uint32_t resource_src(uint32_t s) { return 0x00107006u | (s << 4); }
uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
float value(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}
uint32_t read_component(const Operand& o, int c) {
    if (o.components == 1) return 0;
    if (o.mode == 2) return o.swizzle[0];
    return o.swizzle[c];
}
int single(uint32_t mask) { return mask == 1 ? 0 : mask == 2 ? 1 : mask == 4 ? 2 : mask == 8 ? 3 : -1; }
// the operand reads temp register `reg`'s component `comp` (for an instruction writing component c)
bool reads(const Operand& o, uint32_t reg, int comp, int c) {
    return o.type == dxbc::kTemp && o.dims == 1 && !o.relative && o.index[0] == reg && int(read_component(o, c)) == comp;
}
// a scalar immediate (or one whose read component is) as a float; false if it is not one
bool immediate(const dxbc::Program& p, const Instruction& in, const Operand& o, int c, float* v, uint32_t* at) {
    if (o.type != dxbc::kImmediate32 || o.neg || o.abs) return false;
    const uint32_t end = in.offset + o.first + o.length;
    const uint32_t k = o.components == 4 ? end - 4 + read_component(o, c) : end - 1;
    *v = value(p.tokens[k]);
    *at = k;
    return true;
}
bool near(float a, float b) { return a > b - 1e-6f && a < b + 1e-6f; }

// The rim's factor: the instruction multiplying the rim by its immediate scale, which operand that is, and the
// immediate's token.
struct Rim {
    int at = -1;
    int operand = -1;
    uint32_t token = 0;
    float factor = 0.0f;
};
bool find_rim(const dxbc::Program& p, Rim* rim) {
    for (size_t i = 0; i + 1 < p.code.size(); ++i) {
        const Instruction& a = p.code[i];
        if (a.opcode != dxbc::kAdd || a.saturate || a.operands.size() != 3 || a.operands[0].type != dxbc::kTemp) continue;
        const int c = single(a.operands[0].mask);
        if (c < 0) continue;
        const uint32_t reg = a.operands[0].index[0];
        // add r.c, -x, l(0.4) (either order)
        float v = 0.0f;
        uint32_t tok = 0;
        const Operand& a1 = a.operands[1];
        const Operand& a2 = a.operands[2];
        const bool form = (a1.neg && a1.type == dxbc::kTemp && immediate(p, a, a2, c, &v, &tok) && near(v, 0.4f)) ||
                          (a2.neg && a2.type == dxbc::kTemp && immediate(p, a, a1, c, &v, &tok) && near(v, 0.4f));
        if (!form) continue;
        // mul_sat r.c, r.c, l(2.5)
        const Instruction& b = p.code[i + 1];
        if (b.opcode != dxbc::kMul || !b.saturate || b.operands.size() != 3 || b.operands[0].type != dxbc::kTemp ||
            b.operands[0].index[0] != reg || b.operands[0].mask != a.operands[0].mask)
            continue;
        const bool scaled = (reads(b.operands[1], reg, c, c) && immediate(p, b, b.operands[2], c, &v, &tok) && near(v, 2.5f)) ||
                            (reads(b.operands[2], reg, c, c) && immediate(p, b, b.operands[1], c, &v, &tok) && near(v, 2.5f));
        if (!scaled) continue;
        // then only products of r.c, up to its product with an immediate
        for (size_t j = i + 2; j < p.code.size(); ++j) {
            const Instruction& m = p.code[j];
            if (is_declaration(m.opcode) || m.operands.empty()) continue;
            const int dsts = dxbc::destinations(m.opcode);
            bool writes = false;
            for (int d = 0; d < dsts && d < int(m.operands.size()); ++d)
                writes |= m.operands[d].type == dxbc::kTemp && m.operands[d].index[0] == reg && (m.operands[d].mask & (1u << c));
            if (!writes) continue;
            if (m.opcode != dxbc::kMul || m.saturate || m.operands.size() != 3 || m.operands[0].mask != (1u << c)) break;
            const int self = reads(m.operands[1], reg, c, c) ? 1 : reads(m.operands[2], reg, c, c) ? 2 : 0;
            if (!self) break;
            const int other = 3 - self;
            if (immediate(p, m, m.operands[other], c, &v, &tok)) {
                if (p.tokens[m.offset] >> 31) return false;  // (an extended opcode)
                rim->at = int(j);
                rim->operand = other;
                rim->token = tok;
                rim->factor = v;
                return true;
            }
        }
    }
    return false;
}

struct Parsed {
    dxbc::Container c;
    dxbc::Chunk* shdr = nullptr;
    dxbc::Program p;
};
Result open(const void* bytes, size_t size, Parsed* s, Report& r) {
    std::string error;
    if (!dxbc::parse(bytes, size, &s->c, &error)) {
        r.reason = error;
        return r.result = Result::Unsupported;
    }
    s->shdr = s->c.find("SHDR");
    if (!s->shdr) s->shdr = s->c.find("SHEX");
    if (!s->shdr) return r.result = Result::NotPixelShader;
    if (!dxbc::decode(*s->shdr, &s->p, &error)) {
        r.reason = "decode: " + error;
        return r.result = Result::Unsupported;
    }
    if ((s->p.version >> 16) != 0) return r.result = Result::NotPixelShader;
    return Result::Patched;  // (opened)
}
void store(Parsed& s, const std::vector<uint32_t>& program) {
    s.shdr->data.assign(reinterpret_cast<const uint8_t*>(program.data()),
                        reinterpret_cast<const uint8_t*>(program.data()) + program.size() * 4);
}

}  // namespace

const char* name(Result r) {
    switch (r) {
        case Result::NotPixelShader: return "not a pixel shader";
        case Result::NoRimLight: return "no rim light";
        case Result::Patched: return "patched";
        default: return "unsupported";
    }
}

Result patch(const void* bytes, size_t size, float scale, std::vector<uint8_t>* out, Report* report) {
    Report local;
    Report& r = report ? *report : local;
    auto fail = [&](const std::string& why) {
        r.result = Result::Unsupported;
        r.reason = why;
        return r.result;
    };
    Parsed s;
    if (open(bytes, size, &s, r) != Result::Patched) return r.result;
    const dxbc::Program& p = s.p;
    Rim rim;
    if (!find_rim(p, &rim)) return r.result = Result::NoRimLight;

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
    if (!has_t14) return fail("no irradiance (t14): not GI-patched");
    if (position == UINT32_MAX) return fail("no pixel position input");
    if (temps_at < 0) return fail("no temps declared");

    // before the rim's product: its factor times `scale` where the core traced the pixel, the game's elsewhere
    const uint32_t tn = temps;
    const uint32_t traced = bits(rim.factor * scale), game = bits(rim.factor);
    const std::vector<uint32_t> block = {
        op_token(kFtoi, 5), temp_dst(0x3), tn, input_src(swz(0, 1, 0, 0)), position,
        op_token(dxbc::kMov, 8), temp_dst(0xC), tn, kImm4, 0, 0, 0, 0,
        op_token(dxbc::kLd, 7), temp_dst(0xF), tn, temp_src(swz(0, 1, 2, 3)), tn, resource_src(swz(0, 1, 2, 3)), kIrradianceSlot,
        op_token(dxbc::kMovc, 15), temp_dst(0x1), tn, temp_src(swz(3, 3, 3, 3)), tn, kImm4, traced, traced, traced, traced, kImm4,
        game, game, game, game,
    };
    const size_t added = 4;
    // the product itself, reading rN.x in place of the immediate
    const Instruction& m = p.code[size_t(rim.at)];
    std::vector<uint32_t> product;
    const uint32_t* tk = p.tokens.data() + m.offset;
    product.push_back(tk[0]);
    for (size_t k = 0; k < m.operands.size(); ++k) {
        const Operand& o = m.operands[k];
        if (int(k) == rim.operand) {
            product.push_back(temp_src(swz(0, 0, 0, 0)));
            product.push_back(tn);
        } else {
            product.insert(product.end(), tk + o.first, tk + o.first + o.length);
        }
    }
    product[0] = (product[0] & ~(0x7Fu << 24)) | (uint32_t(product.size()) << 24);

    std::vector<uint32_t> t;
    t.reserve(p.tokens.size() + block.size() + 4);
    const uint32_t length = p.tokens[1];
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (int(i) == rim.at) {
            t.insert(t.end(), block.begin(), block.end());
            t.insert(t.end(), product.begin(), product.end());
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
    store(s, program);
    dxbc::Program check;
    std::string error;
    if (!dxbc::decode(*s.shdr, &check, &error)) return fail("the rewritten program does not decode: " + error);
    if (check.code.size() != p.code.size() + added) return fail("the rewritten program has the wrong instruction count");
    if (out) *out = dxbc::serialize(s.c);
    r.result = Result::Patched;
    return r.result;
}

Result scaled(const void* bytes, size_t size, float scale, std::vector<uint8_t>* out, Report* report) {
    Report local;
    Report& r = report ? *report : local;
    Parsed s;
    if (open(bytes, size, &s, r) != Result::Patched) return r.result;
    Rim rim;
    if (!find_rim(s.p, &rim)) return r.result = Result::NoRimLight;
    std::vector<uint32_t> program = s.p.tokens;
    program[rim.token] = bits(rim.factor * scale);
    store(s, program);
    if (out) *out = dxbc::serialize(s.c);
    return r.result = Result::Patched;
}

}  // namespace rim_light
