#include "patch/shadow_floor.h"

#include <array>
#include <cctype>
#include <cstring>
#include <map>

#include "patch/dxbc.h"

namespace shadow_floor {
namespace {

using dxbc::Instruction;
using dxbc::Operand;

constexpr uint32_t kIrradianceSlot = 14;      // as ambient::kIrradianceSlot
constexpr uint32_t kGameFloor = 0x3D4CCCCDu;  // 0.05f, the game's
constexpr uint32_t kGlowWeight = 0x3F333333u;  // 0.7f and 0.3f: rfg-eosglass's mad <shadow>, 0.7, 0.3
constexpr uint32_t kGlowFloor = 0x3E99999Au;
constexpr uint32_t kOne = 0x3F800000u;

bool is_sample(uint32_t op) {
    return op == dxbc::kSample || op == dxbc::kSampleC || op == dxbc::kSampleCLz || op == dxbc::kSampleL ||
           op == dxbc::kSampleD || op == dxbc::kSampleB || op == dxbc::kLd;
}
bool is_declaration(uint32_t op) { return (op >= dxbc::kDclResource && op <= dxbc::kDclGlobalFlags) || op == dxbc::kCustomData; }

uint32_t swz(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a | b << 2 | c << 4 | d << 6; }
uint32_t op_token(uint32_t opcode, uint32_t length) { return opcode | (length << 24); }
uint32_t temp_dst(uint32_t mask) { return 0x00100002u | (mask << 4); }
uint32_t temp_src(uint32_t s) { return 0x00100006u | (s << 4); }
uint32_t input_src(uint32_t s) { return 0x00101006u | (s << 4); }
uint32_t resource_src(uint32_t s) { return 0x00107006u | (s << 4); }
uint32_t cb_src(uint32_t s) { return 0x00208006u | (s << 4); }  // cb<slot>[<register>], both immediate
constexpr uint32_t kImm4 = 0x00004002u;  // a 4-component immediate32 operand

uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
int single_component(uint32_t mask) {
    switch (mask) {
        case 1: return 0;
        case 2: return 1;
        case 4: return 2;
        case 8: return 3;
        default: return -1;
    }
}
// The component a source operand reads for destination component c.
int dot_width(uint32_t op) { return op == dxbc::kDp2 ? 2 : op == dxbc::kDp3 ? 3 : op == dxbc::kDp4 ? 4 : 0; }
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

}  // namespace

const char* name(Result r) {
    switch (r) {
        case Result::NotPixelShader: return "not a pixel shader";
        case Result::NotLit: return "not lit";
        case Result::NoFloor: return "lit, no floor";
        case Result::Patched: return "patched";
        default: return "unsupported";
    }
}

Result patch(const void* bytes, size_t size, float in_shadow, std::vector<uint8_t>* out, Report* report) {
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
    int temps_at = -1, t15_at = -1;
    bool has_t14 = false;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (in.opcode == dxbc::kDclResource && !in.operands.empty()) {
            if (in.operands[0].index[0] == 15) t15_at = int(i);
            if (in.operands[0].index[0] == kIrradianceSlot) has_t14 = true;
        } else if (in.opcode == dxbc::kDclTemps && in.length == 2) {
            temps = p.tokens[in.offset + 1];
            temps_at = int(i);
        } else if (in.opcode == dxbc::kDclInputPsSiv && !in.operands.empty() && p.tokens[in.offset + in.length - 1] == 1) {
            position = in.operands[0].index[0];  // D3D10_SB_NAME_POSITION
        }
    }
    if (t15_at < 0) return r.result = Result::NotLit;

    // The floor: max <one component>, <a t15 sample's .y>, l(0.05); the glow (rfg-eosglass): mad <one component>, <a
    // t15 sample's .y>, l(0.7), l(0.3). Which ClbTexture channel each temp component holds, in program order (a t15
    // sample's destination gets its channels; any other write forgets them).
    std::map<uint32_t, std::array<int, 4>> clb;
    int floor_at = -1, floors = 0, glow_at = -1, glows = 0;
    // The sun's terms that never meet the shadow: what each temp component owes to ClbTexture's G (any computation from
    // it), the `mul`s by the sun's colour (cb1[0], g_lightingFragmentConstants.lightColor) with an unshadowed other factor,
    // followed while their result lives: one multiplied by a shadowed value later is shadowed after all.
    bool lighting_cb1 = false;
    for (const dxbc::Binding& b : dxbc::bindings(c))
        if (b.type == 0 && b.slot == 1 && contains_nocase(b.name, "lightingFragment")) lighting_cb1 = true;
    std::map<uint32_t, std::array<bool, 4>> shade;
    auto shaded = [&](const Operand& o, int cc, int dot) {  // a source operand's components read for result component cc
        if (o.type != dxbc::kTemp) return false;
        auto it = shade.find(o.index[0]);
        if (it == shade.end()) return false;
        if (dot)
            for (int j = 0; j < dot; ++j) {
                if (it->second[o.components == 1 ? 0u : o.swizzle[j]]) return true;
            }
        return !dot && it->second[read_component(o, cc)];
    };
    struct SunTerm {
        int at;
        uint32_t reg, live;
        bool shadowed;
    };
    std::vector<SunTerm> terms;
    bool shadow_used = false;
    auto shadow_read = [&](const Instruction& in, int* comp) {  // dst one temp component, src0 a t15 sample's .y
        const Operand& d = in.operands[0];
        const Operand& a = in.operands[1];
        *comp = d.type == dxbc::kTemp ? single_component(d.mask) : -1;
        if (*comp < 0 || a.type != dxbc::kTemp || a.neg || a.abs || a.relative) return false;
        auto it = clb.find(a.index[0]);
        return it != clb.end() && it->second[read_component(a, *comp)] == 1;
    };
    auto immediate = [&](const Instruction& in, size_t k, int comp, uint32_t* value) {
        const Operand& b = in.operands[k];
        if (b.type != dxbc::kImmediate32 || b.neg || b.abs || b.components == 0) return false;
        *value = p.tokens[in.offset + b.first + 1 + (b.components == 4 ? uint32_t(comp) : 0u)];
        return true;
    };
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (is_declaration(in.opcode)) continue;
        int comp = -1;
        uint32_t v1 = 0, v2 = 0;
        if (in.opcode == dxbc::kMax && in.operands.size() == 3 && shadow_read(in, &comp) && immediate(in, 2, comp, &v1) &&
            v1 == kGameFloor) {
            ++floors;
            if (floor_at < 0) floor_at = int(i);
        }
        if (in.opcode == dxbc::kMad && in.operands.size() == 4 && shadow_read(in, &comp) && immediate(in, 2, comp, &v1) &&
            immediate(in, 3, comp, &v2) && v1 == kGlowWeight && v2 == kGlowFloor) {
            ++glows;
            if (glow_at < 0) glow_at = int(i);
        }
        const int dsts = dxbc::destinations(in.opcode);
        const Operand* res = nullptr;
        if (is_sample(in.opcode))
            for (const Operand& o : in.operands)
                if (o.type == dxbc::kResource) res = &o;
        const bool from_t15 = res && res->index[0] == 15;
        const int dot = dot_width(in.opcode);
        const uint32_t wmask = dsts >= 1 && !in.operands.empty() && in.operands[0].type == dxbc::kTemp ? in.operands[0].mask : 0u;
        // the open sun terms: read here as a factor of a product with a shadowed value?
        if ((in.opcode == dxbc::kMul || in.opcode == dxbc::kMad) && in.operands.size() >= 3)
            for (SunTerm& st : terms) {
                if (st.shadowed || !st.live) continue;
                for (size_t k = 1; k < 3; ++k) {
                    const Operand& o = in.operands[k];
                    bool reads = false;
                    for (int cc = 0; cc < 4; ++cc)
                        if ((wmask & (1u << cc)) && o.type == dxbc::kTemp && o.index[0] == st.reg &&
                            (st.live & (1u << read_component(o, cc))))
                            reads = true;
                    if (!reads) continue;
                    const Operand& other = in.operands[k == 1 ? 2 : 1];
                    for (int cc = 0; cc < 4; ++cc)
                        if ((wmask & (1u << cc)) && shaded(other, cc, 0)) st.shadowed = true;
                }
            }
        // a new one: mul <temp>, <unshadowed>, cb1[0] (either order)
        if (lighting_cb1 && in.opcode == dxbc::kMul && in.operands.size() == 3 && wmask) {
            for (size_t k = 1; k < 3; ++k) {
                const Operand& o = in.operands[k];
                if (o.type != dxbc::kConstantBuffer || o.index[0] != 1 || o.index[1] != 0 || o.relative) continue;
                const Operand& other = in.operands[k == 1 ? 2 : 1];
                bool sh = false;
                for (int cc = 0; cc < 4; ++cc)
                    if ((wmask & (1u << cc)) && shaded(other, cc, 0)) sh = true;
                if (!sh) terms.push_back({int(i), in.operands[0].index[0], 0u, false});  // live from after its write (below)
                break;
            }
        }
        // the writes: ClbTexture's channels held, the shadow's taint, the sun terms' results overwritten
        std::array<bool, 4> taint{};
        for (int cc = 0; cc < 4; ++cc) {
            if (!(wmask & (1u << cc))) continue;
            bool t = false;
            if (res) {
                t = from_t15 && res->swizzle[cc] == 1;
            } else {
                for (size_t k = size_t(dsts); k < in.operands.size(); ++k) t |= shaded(in.operands[k], cc, dot);
                if (t) shadow_used = true;
            }
            taint[size_t(cc)] = t;
        }
        for (int k = 0; k < dsts && k < int(in.operands.size()); ++k) {
            const Operand& d = in.operands[size_t(k)];
            if (d.type != dxbc::kTemp) continue;
            auto& held = clb.try_emplace(d.index[0], std::array<int, 4>{-1, -1, -1, -1}).first->second;
            auto& sh = shade.try_emplace(d.index[0], std::array<bool, 4>{}).first->second;
            for (int cc = 0; cc < 4; ++cc) {
                if (!(d.mask & (1u << cc))) continue;
                held[size_t(cc)] = from_t15 ? int(res->swizzle[cc]) : -1;
                sh[size_t(cc)] = k == 0 ? taint[size_t(cc)] : false;
            }
            for (SunTerm& st : terms) {
                if (st.reg != d.index[0]) continue;
                if (st.at == int(i))
                    st.live = d.mask;  // its own write: live from here
                else
                    st.live &= ~d.mask;
            }
        }
    }
    std::vector<int> suns;
    if (shadow_used || floors || glows)  // (a shader with a shadow to apply)
        for (const SunTerm& st : terms)
            if (!st.shadowed) suns.push_back(st.at);
    if (!floors && !glows && suns.empty()) return r.result = Result::NoFloor;
    if (floors > 1 || glows > 1) return fail("more than one floor on the shadow channel");
    if (position == UINT32_MAX) return fail("no pixel position input");
    if (temps_at < 0) return fail("no temps declared");
    int first_use = INT32_MAX;
    for (int at : {floor_at, glow_at, suns.empty() ? -1 : suns.front()})
        if (at >= 0 && at < first_use) first_use = at;
    r.index = uint32_t(first_use);
    r.sun_terms = uint32_t(suns.size());

    const uint32_t tn = temps;
    std::vector<uint32_t> dcl_t14;
    if (!has_t14) {
        const Instruction& d15 = p.code[size_t(t15_at)];
        dcl_t14.assign(p.tokens.begin() + d15.offset, p.tokens.begin() + d15.offset + d15.length);
        dcl_t14[d15.operands[0].first + 1] = kIrradianceSlot;  // the operand's index token
    }
    // rN.w: the irradiance's alpha (the core traced the pixel); then rN.x the floor, rN.y and rN.z the glow's weight and
    // floor, the game's where alpha is 0
    const uint32_t keep = bits(in_shadow);
    std::vector<uint32_t> load = {
        op_token(27 /* ftoi */, 5), temp_dst(0x3), tn, input_src(swz(0, 1, 0, 0)), position,
        op_token(dxbc::kMov, 8), temp_dst(0xC), tn, kImm4, 0, 0, 0, 0,
        op_token(dxbc::kLd, 7), temp_dst(0xF), tn, temp_src(swz(0, 1, 2, 3)), tn, resource_src(swz(0, 1, 2, 3)), kIrradianceSlot,
    };
    auto choose = [&](uint32_t mask, uint32_t traced, uint32_t game) {
        const std::vector<uint32_t> m = {op_token(dxbc::kMovc, 15), temp_dst(mask), tn, temp_src(swz(3, 3, 3, 3)), tn,
                                         kImm4, traced, traced, traced, traced, kImm4, game, game, game, game};
        load.insert(load.end(), m.begin(), m.end());
    };
    size_t added = 3;
    // The sun's terms that never met the shadow: rW.xyz, the light that reaches the surface where traced, the sun's
    // colour times the shadow (ClbTexture's G at the pixel, rM.y) plus the traced irradiance (rN.xyz: the bounce, the
    // sky), else the sun's colour as the game has it; their cb1[0] reads it. (Before the floors' values take rN.xyz.)
    const uint32_t tm = temps + 1, tw = temps + 2;
    if (!suns.empty()) {
        const std::vector<uint32_t> m = {
            op_token(27 /* ftoi */, 5), temp_dst(0x3), tm, input_src(swz(0, 1, 0, 0)), position,
            op_token(dxbc::kMov, 8), temp_dst(0xC), tm, kImm4, 0, 0, 0, 0,
            op_token(dxbc::kLd, 7), temp_dst(0xF), tm, temp_src(swz(0, 1, 2, 3)), tm, resource_src(swz(0, 1, 2, 3)), 15,
            op_token(dxbc::kMad, 10), temp_dst(0x7), tw, cb_src(swz(0, 1, 2, 0)), 1, 0, temp_src(swz(1, 1, 1, 1)), tm,
            temp_src(swz(0, 1, 2, 0)), tn,
            op_token(dxbc::kMovc, 10), temp_dst(0x7), tw, temp_src(swz(3, 3, 3, 3)), tn, temp_src(swz(0, 1, 2, 0)), tw,
            cb_src(swz(0, 1, 2, 0)), 1, 0,
        };
        load.insert(load.end(), m.begin(), m.end());
        added += 5;
    }
    if (floors) choose(0x1, keep, kGameFloor), ++added;
    if (glows) choose(0x2, kOne, kGlowWeight), choose(0x4, 0u, kGlowFloor), added += 2;
    // a sun term with its cb1[0] operand reading rW (same components, same modifiers)
    auto sun_op = [&](const Instruction& in) -> std::vector<uint32_t> {
        const uint32_t* tk = p.tokens.data() + in.offset;
        if (tk[0] >> 31) return {};
        std::vector<uint32_t> o2(tk, tk + in.operands[0].first);
        for (size_t k = 0; k < in.operands.size(); ++k) {
            const Operand& o = in.operands[k];
            if (k > 0 && o.type == dxbc::kConstantBuffer && o.index[0] == 1 && o.index[1] == 0 && !o.relative) {
                o2.push_back((tk[o.first] & 0x80000FFFu) | (1u << 20));  // a temp, one immediate index
                uint32_t at = o.first;
                bool ext = (tk[at] >> 31) != 0;
                while (ext) {
                    ++at;
                    o2.push_back(tk[at]);
                    ext = (tk[at] >> 31) != 0;
                }
                o2.push_back(tw);
            } else {
                o2.insert(o2.end(), tk + o.first, tk + o.first + o.length);
            }
        }
        o2[0] = (o2[0] & ~(0x7Fu << 24)) | (uint32_t(o2.size()) << 24);
        return o2;
    };
    // an instruction with its immediates from the first one on replaced by rN.<components>
    auto reread = [&](int at, std::initializer_list<uint32_t> comps) -> std::vector<uint32_t> {
        const Instruction& in = p.code[size_t(at)];
        const uint32_t* tk = p.tokens.data() + in.offset;
        std::vector<uint32_t> op = {tk[0]};
        if (tk[0] >> 31) return {};
        for (int k = 0; k < 2; ++k) {
            const Operand& o = in.operands[size_t(k)];
            op.insert(op.end(), tk + o.first, tk + o.first + o.length);
        }
        for (uint32_t c : comps) {
            op.push_back(temp_src(swz(c, c, c, c)));
            op.push_back(tn);
        }
        op[0] = (op[0] & ~(0x7Fu << 24)) | (uint32_t(op.size()) << 24);
        return op;
    };
    const std::vector<uint32_t> floor_op = floors ? reread(floor_at, {0}) : std::vector<uint32_t>{};
    const std::vector<uint32_t> glow_op = glows ? reread(glow_at, {1, 2}) : std::vector<uint32_t>{};
    if ((floors && floor_op.empty()) || (glows && glow_op.empty())) return fail("an extended opcode");

    std::vector<uint32_t> t;
    t.reserve(p.tokens.size() + dcl_t14.size() + load.size() + 4);
    const uint32_t length = p.tokens[1];
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (int(i) == first_use) t.insert(t.end(), load.begin(), load.end());
        bool sun = false;
        for (int at : suns) sun |= at == int(i);
        if (sun) {  // the sun's term lit by what reaches the surface
            const std::vector<uint32_t> op = sun_op(in);
            if (op.empty()) return fail("an extended opcode");
            t.insert(t.end(), op.begin(), op.end());
            continue;
        }
        if (int(i) == floor_at || int(i) == glow_at) {
            const std::vector<uint32_t>& op = int(i) == floor_at ? floor_op : glow_op;
            t.insert(t.end(), op.begin(), op.end());
            continue;
        }
        t.insert(t.end(), p.tokens.begin() + in.offset, p.tokens.begin() + in.offset + in.length);
        if (int(i) == temps_at) t[t.size() - 1] = tn + (suns.empty() ? 1 : 3);
        if (int(i) == t15_at) t.insert(t.end(), dcl_t14.begin(), dcl_t14.end());
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
    if (check.code.size() != p.code.size() + added + (has_t14 ? 0 : 1)) return fail("the rewritten program has the wrong instruction count");
    if (out) *out = dxbc::serialize(c);
    r.result = Result::Patched;
    return r.result;
}

}  // namespace shadow_floor
