#include "patch/eye_adaption.h"

#include <cstring>

#include "patch/dxbc.h"

namespace eye_adaption {
namespace {

using dxbc::Instruction;
using dxbc::Operand;

constexpr uint32_t kLt = 49;               // D3D10_SB_OPCODE_LT
constexpr uint32_t kImm4 = 0x00004002u;    // a 4-component immediate32 operand
uint32_t temp_dst(uint32_t mask) { return 0x00100002u | (mask << 4); }
uint32_t temp_src(uint32_t c) { return 0x00100006u | ((c | c << 2 | c << 4 | c << 6) << 4); }  // rT.cccc
uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// The one component a destination mask writes, or -1.
int single(uint32_t mask) {
    switch (mask) {
        case 1: return 0;
        case 2: return 1;
        case 4: return 2;
        case 8: return 3;
        default: return -1;
    }
}

// The component a source reads for destination component c.
int read_component(const Operand& o, int c) {
    if (o.components != 4) return -1;
    if (o.mode == 2) return o.swizzle[0];
    if (o.mode == 1) return o.swizzle[c];
    return -1;
}

// A source reading the constant buffer's variable at (slot, byte offset) for destination component c.
bool reads_variable(const Operand& o, int c, uint32_t slot, uint32_t offset) {
    return o.type == dxbc::kConstantBuffer && !o.relative && !o.neg && !o.abs && o.dims >= 2 && o.index[0] == slot &&
           o.index[1] == offset / 16 && read_component(o, c) == int(offset % 16 / 4);
}

}  // namespace

const char* name(Result r) {
    switch (r) {
        case Result::NotPixelShader: return "not a pixel shader";
        case Result::NotAdaptation: return "no eye adaptation";
        case Result::Patched: return "patched";
        default: return "unsupported";
    }
}

Result patch(const void* bytes, size_t size, float dark, std::vector<uint8_t>* out, Report* report) {
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

    bool have_amount = false, have_base = false;
    uint32_t amount_slot = 0, amount_offset = 0, base_slot = 0, base_offset = 0;
    for (const dxbc::Variable& v : dxbc::variables(c)) {
        if (v.name == "Eye_adaption_amount") {
            have_amount = true;
            amount_slot = v.slot;
            amount_offset = v.offset;
        } else if (v.name == "Eye_adaption_base") {
            have_base = true;
            base_slot = v.slot;
            base_offset = v.offset;
        }
    }
    if (!have_amount || !have_base) return r.result = Result::NotAdaptation;

    // the adaptation's mads: the amount times the difference plus the base (the amount first or second)
    struct Site {
        int at;
        int amount;  // its operand index (1 or 2); the difference is the other of 1 and 2
        int c;
    };
    std::vector<Site> sites;
    uint32_t temps = 0;
    int temps_at = -1;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (in.opcode == dxbc::kDclTemps && in.length == 2) {
            temps = p.tokens[in.offset + 1];
            temps_at = int(i);
            continue;
        }
        if (in.opcode != dxbc::kMad || in.operands.size() != 4) continue;
        const Operand& d = in.operands[0];
        const int cc = d.type == dxbc::kTemp && d.mode == 0 ? single(d.mask) : -1;
        if (cc < 0 || !reads_variable(in.operands[3], cc, base_slot, base_offset)) continue;
        for (int a = 1; a <= 2; ++a)
            if (reads_variable(in.operands[size_t(a)], cc, amount_slot, amount_offset) &&
                in.operands[size_t(3 - a)].type == dxbc::kTemp) {
                sites.push_back(Site{int(i), a, cc});
                break;
            }
    }
    if (sites.empty()) return r.result = Result::NotAdaptation;
    if (temps_at < 0) return fail("no temps declared");
    const uint32_t rt = temps;  // the new temp

    std::vector<uint32_t> t;
    t.reserve(p.tokens.size() + sites.size() * 32);
    const uint32_t length = p.tokens[1];
    size_t next = 0;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        const uint32_t* tk = p.tokens.data() + in.offset;
        if (next < sites.size() && sites[next].at == int(i)) {
            const Site& s = sites[next++];
            if (tk[0] >> 31) return fail("an extended opcode computes the adaptation");
            const Operand& amount = in.operands[size_t(s.amount)];
            const Operand& diff = in.operands[size_t(3 - s.amount)];
            const uint32_t mask = 1u << s.c;
            // lt rT.c, <difference>, l(0)
            std::vector<uint32_t> lt = {0, temp_dst(mask), rt};
            lt.insert(lt.end(), tk + diff.first, tk + diff.first + diff.length);
            lt.insert(lt.end(), {kImm4, 0, 0, 0, 0});
            lt[0] = kLt | uint32_t(lt.size()) << 24;
            // movc rT.c, rT.cccc, l(dark), <amount>
            std::vector<uint32_t> mv = {0, temp_dst(mask), rt, temp_src(uint32_t(s.c)), rt, kImm4, bits(dark), bits(dark),
                                        bits(dark), bits(dark)};
            mv.insert(mv.end(), tk + amount.first, tk + amount.first + amount.length);
            mv[0] = dxbc::kMovc | uint32_t(mv.size()) << 24;
            // the mad, its amount read from rT.c
            std::vector<uint32_t> mad(tk, tk + in.operands[0].first);
            for (size_t k = 0; k < in.operands.size(); ++k) {
                const Operand& o = in.operands[k];
                if (int(k) == s.amount) {
                    mad.push_back(temp_src(uint32_t(s.c)));
                    mad.push_back(rt);
                } else {
                    mad.insert(mad.end(), tk + o.first, tk + o.first + o.length);
                }
            }
            mad[0] = (mad[0] & ~(0x7Fu << 24)) | (uint32_t(mad.size()) << 24);
            t.insert(t.end(), lt.begin(), lt.end());
            t.insert(t.end(), mv.begin(), mv.end());
            t.insert(t.end(), mad.begin(), mad.end());
            continue;
        }
        t.insert(t.end(), tk, tk + in.length);
        if (int(i) == temps_at) t[t.size() - 1] = temps + 1;
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
    if (check.code.size() != p.code.size() + 2 * sites.size()) return fail("the rewritten program has the wrong instruction count");
    r.mads = uint32_t(sites.size());
    if (out) *out = dxbc::serialize(c);
    r.result = Result::Patched;
    return r.result;
}

}  // namespace eye_adaption
