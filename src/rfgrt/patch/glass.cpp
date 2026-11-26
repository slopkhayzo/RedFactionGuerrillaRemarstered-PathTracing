#include "patch/glass.h"

#include <array>
#include <cctype>
#include <cstring>
#include <map>

#include "patch/dxbc.h"

namespace glass {
namespace {

using dxbc::Instruction;
using dxbc::Operand;

constexpr uint32_t kSaturate = 1u << 13;
constexpr uint32_t kDiv = 14;
constexpr uint32_t kFtoi = 27;
constexpr uint32_t kDclInputPs = 98;
constexpr uint32_t kImm4 = 0x00004002u;  // a 4-component immediate32 operand
// what a temp component's value comes from
constexpr uint32_t kFromDiffuse = 1;  // the diffuse texture
constexpr uint32_t kFromAmbient = 2;  // the ambient colours (cb0[2], cb0[3])
constexpr uint32_t kFromShadow = 4;   // ClbTexture's G: the sun's shadow (a shader that reads it is no glass of this make)

bool is_declaration(uint32_t op) { return (op >= dxbc::kDclResource && op <= dxbc::kDclGlobalFlags) || op == dxbc::kCustomData; }
bool is_sample(uint32_t op) {
    return op == dxbc::kSample || op == dxbc::kSampleC || op == dxbc::kSampleCLz || op == dxbc::kSampleL ||
           op == dxbc::kSampleD || op == dxbc::kSampleB || op == dxbc::kLd;
}
int dot_width(uint32_t op) { return op == dxbc::kDp2 ? 2 : op == dxbc::kDp3 ? 3 : op == dxbc::kDp4 ? 4 : 0; }

uint32_t swz(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a | b << 2 | c << 4 | d << 6; }
uint32_t op_token(uint32_t opcode, uint32_t length) { return opcode | (length << 24); }
uint32_t temp_dst(uint32_t mask) { return 0x00100002u | (mask << 4); }
uint32_t output_dst(uint32_t mask) { return 0x00102002u | (mask << 4); }
uint32_t temp_src(uint32_t s) { return 0x00100006u | (s << 4); }
constexpr uint32_t kNegated = 0x80000000u;  // an operand token with an extended one after it: the modifier below
constexpr uint32_t kNeg = 0x41u;            // (an extended operand token: a modifier, negate)
uint32_t input_src(uint32_t s) { return 0x00101006u | (s << 4); }
uint32_t resource_src(uint32_t s) { return 0x00107006u | (s << 4); }

bool contains_nocase(const std::string& s, const char* what) {
    std::string a = s, b = what;
    for (auto& ch : a) ch = char(tolower(uint8_t(ch)));
    for (auto& ch : b) ch = char(tolower(uint8_t(ch)));
    return a.find(b) != std::string::npos;
}

uint32_t read_component(const Operand& o, int c) {
    if (o.components == 1) return 0;
    if (o.mode == 2) return o.swizzle[0];
    return o.swizzle[c];
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
        case Result::NotGlass: return "not glass";
        case Result::Patched: return "patched";
        default: return "unsupported";
    }
}

Result patch(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report, bool fog) {
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

    // The make: the common and lighting constants at cb0 and cb1, a diffuse map, no terrain constants (the far terrain's
    // unlit shaders are made alike), no ClbTexture's shadow read (below).
    bool common = false, lighting = false, terrain = false;
    uint32_t diffuse = UINT32_MAX;
    for (const dxbc::Binding& b : dxbc::bindings(c)) {
        if (b.type == 0 && b.slot == 0 && contains_nocase(b.name, "commonFragment")) common = true;
        if (b.type == 0 && b.slot == 1 && contains_nocase(b.name, "lightingFragment")) lighting = true;
        if (b.type == 0 && contains_nocase(b.name, "terrain")) terrain = true;
        // exactly the glass shaders' diffuse map (the far terrain's unlit shaders, made alike, name theirs Diffuse_Map_0)
        if (b.type == 2 && b.name.size() == 19 && contains_nocase(b.name, "Diffuse_Map_texture")) diffuse = b.slot;
    }
    if (!common || !lighting || terrain || diffuse == UINT32_MAX) return r.result = Result::NotGlass;
    uint32_t temps = 0, position = UINT32_MAX;
    int temps_at = -1, diffuse_dcl = -1, first_input = -1, first_code = -1;
    bool has_t15 = false;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (!is_declaration(in.opcode)) {
            if (first_code < 0) first_code = int(i);
            continue;
        }
        if (in.opcode == dxbc::kDclResource && !in.operands.empty()) {
            const uint32_t slot = in.operands[0].index[0];
            if (slot == 15) has_t15 = true;  // ClbTexture: only its occlusion may be read (below)
            if (slot == kGlassSlot) return fail("t17 is in use");
            if (slot == kGlassReflSlot) return fail("t19 is in use");
            if (fog && slot == kGlassFogSlot) return fail("t21 is in use");
            if (slot == diffuse) diffuse_dcl = int(i);
        } else if (in.opcode == dxbc::kDclTemps && in.length == 2) {
            temps = p.tokens[in.offset + 1];
            temps_at = int(i);
        } else if (in.opcode == dxbc::kDclInputPsSiv && !in.operands.empty() && p.tokens[in.offset + in.length - 1] == 1) {
            position = in.operands[0].index[0];
        } else if (in.opcode == kDclInputPs && first_input < 0) {
            first_input = int(i);
        }
    }
    if (diffuse_dcl < 0 || first_code < 0) return r.result = Result::NotGlass;

    // The highlight: the one `mul` by the sun's colour (cb1[0]).
    int highlight = -1, muls = 0;
    for (size_t i = size_t(first_code); i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (in.opcode != dxbc::kMul || in.operands.size() != 3 || in.operands[0].type != dxbc::kTemp) continue;
        for (size_t k = 1; k < 3; ++k) {
            const Operand& o = in.operands[k];
            if (o.type == dxbc::kConstantBuffer && o.index[0] == 1 && o.index[1] == 0 && !o.relative) {
                ++muls;
                highlight = int(i);
                break;
            }
        }
    }
    if (muls != 1) return r.result = Result::NotGlass;
    const Operand& hd = p.code[size_t(highlight)].operands[0];

    // What each temp component comes from, in program order; the combination: the first `mad` after the highlight that
    // adds the highlight to a product of the light and the diffuse colour.
    std::map<uint32_t, std::array<uint32_t, 4>> from;
    auto read = [&](const Operand& o, uint32_t comp) -> uint32_t {
        if (o.type == dxbc::kTemp) {
            auto it = from.find(o.index[0]);
            return it == from.end() ? 0u : it->second[comp & 3];
        }
        if (o.type == dxbc::kConstantBuffer && o.index[0] == 0 && (o.index[1] == 2 || o.index[1] == 3)) return kFromAmbient;
        return 0u;
    };
    int combine = -1, light_k = -1;
    bool shadow_used = false;
    for (size_t i = size_t(first_code); i < p.code.size() && combine < 0; ++i) {
        const Instruction& in = p.code[i];
        if (int(i) == highlight) {  // the highlight's other factor: not shadowed already
            const Instruction& h = in;
            for (size_t k = 1; k < 3; ++k) {
                const Operand& o = h.operands[k];
                for (int cc = 0; cc < 4; ++cc)
                    if ((h.operands[0].mask & (1u << cc)) && (read(o, read_component(o, cc)) & kFromShadow)) shadow_used = true;
            }
        }
        if (int(i) > highlight && in.opcode == dxbc::kMad && in.operands.size() == 4 && in.operands[3].type == dxbc::kTemp &&
            in.operands[3].index[0] == hd.index[0] && in.operands[0].type == dxbc::kTemp) {
            const Operand& d = in.operands[0];
            uint32_t f1 = 0, f2 = 0;
            for (int cc = 0; cc < 4; ++cc) {
                if (!(d.mask & (1u << cc))) continue;
                f1 |= read(in.operands[1], read_component(in.operands[1], cc));
                f2 |= read(in.operands[2], read_component(in.operands[2], cc));
            }
            if ((f2 & kFromDiffuse) && !(f1 & kFromDiffuse) && (f1 & kFromAmbient) && in.operands[1].type == dxbc::kTemp) {
                combine = int(i);
                light_k = 1;
            } else if ((f1 & kFromDiffuse) && !(f2 & kFromDiffuse) && (f2 & kFromAmbient) && in.operands[2].type == dxbc::kTemp) {
                combine = int(i);
                light_k = 2;
            }
            if (combine >= 0) break;
        }
        const int dsts = dxbc::destinations(in.opcode);
        if (dsts < 1 || in.operands.empty() || in.operands[0].type != dxbc::kTemp) continue;
        const Operand& d = in.operands[0];
        std::array<uint32_t, 4> value{};
        const Operand* res = nullptr;
        if (is_sample(in.opcode))
            for (const Operand& o : in.operands)
                if (o.type == dxbc::kResource) res = &o;
        for (int cc = 0; cc < 4; ++cc) {
            if (!(d.mask & (1u << cc))) continue;
            uint32_t f = 0;
            if (res) {
                f = res->index[0] == diffuse ? kFromDiffuse : 0u;
                if (res->index[0] == 15 && res->swizzle[cc] == 1) f |= kFromShadow;
            } else {
                const int w = dot_width(in.opcode);
                for (size_t k = size_t(dsts); k < in.operands.size(); ++k) {
                    const Operand& o = in.operands[k];
                    if (w)
                        for (int j = 0; j < w; ++j) f |= read(o, o.components == 1 ? 0u : o.swizzle[j]);
                    else
                        f |= read(o, read_component(o, cc));
                }
            }
            value[size_t(cc)] = f;
            if (!res && (f & kFromShadow)) shadow_used = true;  // the shadow read into something
        }
        auto& slot = from.try_emplace(d.index[0], std::array<uint32_t, 4>{}).first->second;
        for (int cc = 0; cc < 4; ++cc)
            if (d.mask & (1u << cc)) slot[size_t(cc)] = value[size_t(cc)];
    }
    if (combine < 0 || shadow_used) return r.result = Result::NotGlass;
    (void)has_t15;

    if (position == UINT32_MAX) {
        position = position_register(c);
        if (position == UINT32_MAX) return fail("no pixel position in the input signature");
        if (first_input < 0) return fail("no input declarations to put the position's before");
    } else {
        first_input = -1;  // declared already
    }
    if (temps_at < 0) return fail("no temps declared");

    // The light at the combination: which components of which register it reads for each of the result's.
    const Instruction& cm = p.code[size_t(combine)];
    const Operand& lo = cm.operands[size_t(light_k)];
    uint32_t light_mask = 0, light_swz[4] = {0, 0, 0, 0};
    for (int cc = 0; cc < 4; ++cc) {
        if (!(cm.operands[0].mask & (1u << cc))) continue;
        const uint32_t s = read_component(lo, cc);
        if (light_mask & (1u << s)) return fail("the light read twice in one component");
        light_mask |= 1u << s;
        light_swz[s] = uint32_t(cc) < 3 ? uint32_t(cc) : 0u;  // its colour channel: the result's component (rgb)
    }

    // The output: which of o0's components the shader writes (all of its writes go to rO, written to o0 before each ret)
    uint32_t out_mask = 0;
    size_t rets = 0;
    for (size_t i = size_t(first_code); i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (in.opcode == dxbc::kRetc) return fail("a conditional return");
        if (in.opcode == dxbc::kRet) ++rets;
        const int nd = dxbc::destinations(in.opcode);
        for (int d = 0; d < nd && d < int(in.operands.size()); ++d) {
            const Operand& o = in.operands[size_t(d)];
            if (o.type != dxbc::kOutput) continue;
            if (o.index[0] != 0 || o.dims != 1 || o.relative) return fail("an output other than o0");
            out_mask |= o.mask;
        }
    }
    if (!out_mask) return fail("o0 is never written");
    if (!rets) return fail("no ret");

    const uint32_t tn = temps, tm = temps + 1, to = temps + 2, tr = temps + 3, ts = temps + 4;
    std::vector<uint32_t> dcl_t17, dcl_t19, dcl_t21;
    {
        const Instruction& dd = p.code[size_t(diffuse_dcl)];
        dcl_t17.assign(p.tokens.begin() + dd.offset, p.tokens.begin() + dd.offset + dd.length);
        dcl_t17[dd.operands[0].first + 1] = kGlassSlot;
        dcl_t19 = dcl_t17;
        dcl_t19[dd.operands[0].first + 1] = kGlassReflSlot;
        if (fog) {
            dcl_t21 = dcl_t17;
            dcl_t21[dd.operands[0].first + 1] = kGlassFogSlot;
        }
    }
    // dcl_input_ps_siv linear noperspective v<position>.xy, position
    const std::vector<uint32_t> dcl_position = {op_token(dxbc::kDclInputPsSiv, 4) | (4u << 11), 0x00101032u, position, 1u};
    const std::vector<uint32_t> load = {
        op_token(kFtoi, 5), temp_dst(0x3), tn, input_src(swz(0, 1, 0, 0)), position,
        op_token(dxbc::kMov, 8), temp_dst(0xC), tn, kImm4, 0, 0, 0, 0,
        op_token(dxbc::kLd, 7), temp_dst(0xF), tn, temp_src(swz(0, 1, 2, 3)), tn, resource_src(swz(0, 1, 2, 3)), kGlassSlot,
        op_token(dxbc::kAdd, 10) | kSaturate, temp_dst(0x1), tm, temp_src(swz(3, 3, 3, 3)), tn, kImm4, 0xBF800000u, 0xBF800000u,
        0xBF800000u, 0xBF800000u,
        op_token(dxbc::kMovc, 12), temp_dst(0x1), tm, temp_src(swz(3, 3, 3, 3)), tn, temp_src(swz(0, 0, 0, 0)), tm, kImm4, 0x3F800000u,
        0x3F800000u, 0x3F800000u, 0x3F800000u,
    };
    const std::vector<uint32_t> shade = {  // the highlight as much as the sun reaches the glass
        op_token(dxbc::kMul, 7), temp_dst(hd.mask), hd.index[0], temp_src(swz(0, 1, 2, 3)), hd.index[0], temp_src(swz(0, 0, 0, 0)), tm,
    };
    const std::vector<uint32_t> relight = {  // the traced light where there is glass
        op_token(dxbc::kMovc, 9), temp_dst(light_mask), lo.index[0], temp_src(swz(3, 3, 3, 3)), tn,
        temp_src(swz(light_swz[0], light_swz[1], light_swz[2], light_swz[3])), tn, temp_src(swz(0, 1, 2, 3)), lo.index[0],
    };
    const std::vector<uint32_t> reflect = {  // before each ret: what the glass reflects, over the output (glass.h)
        op_token(kFtoi, 5), temp_dst(0x3), tr, input_src(swz(0, 1, 0, 0)), position,
        op_token(dxbc::kMov, 8), temp_dst(0xC), tr, kImm4, 0, 0, 0, 0,
        op_token(dxbc::kLd, 7), temp_dst(0xF), tr, temp_src(swz(0, 1, 2, 3)), tr, resource_src(swz(0, 1, 2, 3)), kGlassReflSlot,
        op_token(dxbc::kMad, 10), temp_dst(0x1), ts, temp_src(swz(3, 3, 3, 3)) | kNegated, kNeg, to, temp_src(swz(3, 3, 3, 3)), tr,
        temp_src(swz(3, 3, 3, 3)), tr,
        op_token(dxbc::kAdd, 7), temp_dst(0x1), ts, temp_src(swz(0, 0, 0, 0)), ts, temp_src(swz(3, 3, 3, 3)), to,
        op_token(dxbc::kMax, 10), temp_dst(0x2), ts, temp_src(swz(0, 0, 0, 0)), ts, kImm4, 0x358637BDu, 0x358637BDu, 0x358637BDu,
        0x358637BDu,
        op_token(dxbc::kMad, 9), temp_dst(0x7), tr, temp_src(swz(0, 1, 2, 0)), to, temp_src(swz(3, 3, 3, 3)), to,
        temp_src(swz(0, 1, 2, 0)), tr,
        op_token(kDiv, 7), temp_dst(0x7), tr, temp_src(swz(0, 1, 2, 0)), tr, temp_src(swz(1, 1, 1, 1)), ts,
        op_token(dxbc::kMovc, 9), temp_dst(0x7), to, temp_src(swz(3, 3, 3, 3)), tr, temp_src(swz(0, 1, 2, 0)), tr,
        temp_src(swz(0, 1, 2, 0)), to,
        op_token(dxbc::kMovc, 9), temp_dst(0x8), to, temp_src(swz(3, 3, 3, 3)), tr, temp_src(swz(0, 0, 0, 0)), ts,
        temp_src(swz(3, 3, 3, 3)), to,
    };
    constexpr size_t kReflectInstructions = 10;
    const std::vector<uint32_t> fogged = {  // then the core's fog at the glass, where it is there (glass.h)
        op_token(kFtoi, 5), temp_dst(0x3), tr, input_src(swz(0, 1, 0, 0)), position,
        op_token(dxbc::kMov, 8), temp_dst(0xC), tr, kImm4, 0, 0, 0, 0,
        op_token(dxbc::kLd, 7), temp_dst(0xF), tr, temp_src(swz(0, 1, 2, 3)), tr, resource_src(swz(0, 1, 2, 3)), kGlassFogSlot,
        op_token(dxbc::kMad, 9), temp_dst(0x7), ts, temp_src(swz(0, 1, 2, 0)), to, temp_src(swz(3, 3, 3, 3)), tr,
        temp_src(swz(0, 1, 2, 0)), tr,
        op_token(dxbc::kMovc, 9), temp_dst(0x7), to, temp_src(swz(3, 3, 3, 3)), tr, temp_src(swz(0, 1, 2, 0)), ts,
        temp_src(swz(0, 1, 2, 0)), to,
    };
    constexpr size_t kFogInstructions = 5;
    const std::vector<uint32_t> write = {op_token(dxbc::kMov, 5), output_dst(out_mask), 0, temp_src(swz(0, 1, 2, 3)), to};

    std::vector<uint32_t> t;
    t.reserve(p.tokens.size() + 64);
    const uint32_t length = p.tokens[1];
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (int(i) == first_input) t.insert(t.end(), dcl_position.begin(), dcl_position.end());
        if (int(i) == first_code) t.insert(t.end(), load.begin(), load.end());
        if (int(i) == combine) t.insert(t.end(), relight.begin(), relight.end());
        if (in.opcode == dxbc::kRet) {
            t.insert(t.end(), reflect.begin(), reflect.end());
            if (fog) t.insert(t.end(), fogged.begin(), fogged.end());
            t.insert(t.end(), write.begin(), write.end());
        }
        const size_t at = t.size();
        t.insert(t.end(), p.tokens.begin() + in.offset, p.tokens.begin() + in.offset + in.length);
        if (!is_declaration(in.opcode)) {  // o0's writes to rO (its type field to temp, its index to rO's)
            const int nd = dxbc::destinations(in.opcode);
            for (int d = 0; d < nd && d < int(in.operands.size()); ++d) {
                const Operand& o = in.operands[size_t(d)];
                if (o.type != dxbc::kOutput) continue;
                size_t k = at + o.first;
                t[k] &= ~(0xFFu << 12);
                bool ext = (t[k] >> 31) != 0;
                while (ext) {
                    ++k;
                    ext = (t[k] >> 31) != 0;
                }
                t[k + 1] = to;
            }
        }
        if (int(i) == temps_at) t[t.size() - 1] = temps + 5;
        if (int(i) == diffuse_dcl) {
            t.insert(t.end(), dcl_t17.begin(), dcl_t17.end());
            t.insert(t.end(), dcl_t19.begin(), dcl_t19.end());
            if (fog) t.insert(t.end(), dcl_t21.begin(), dcl_t21.end());
        }
        if (int(i) == highlight) t.insert(t.end(), shade.begin(), shade.end());
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
    const size_t added = 5 + 1 + 1 + 2 + (fog ? 1 : 0) + (first_input >= 0 ? 1 : 0) + rets * (kReflectInstructions + 1 + (fog ? kFogInstructions : 0));
    if (check.code.size() != p.code.size() + added) return fail("the rewritten program has the wrong instruction count");
    if (out) *out = dxbc::serialize(c);
    r.result = Result::Patched;
    return r.result;
}

}  // namespace glass
