#include "patch/light_pass.h"

#include <cctype>
#include <cstring>

#include "patch/dxbc.h"

namespace light_pass {
namespace {

using dxbc::Instruction;
using dxbc::Operand;

constexpr uint32_t kDclInputPs = 98, kDclInputPsSgv = 99, kFtoi = 27;

uint32_t swz(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a | b << 2 | c << 4 | d << 6; }
uint32_t op_token(uint32_t opcode, uint32_t length) { return opcode | (length << 24); }
uint32_t temp_dst(uint32_t mask) { return 0x00100002u | (mask << 4); }
uint32_t output_dst(uint32_t mask) { return 0x00102002u | (mask << 4); }
uint32_t temp_src(uint32_t s) { return 0x00100006u | (s << 4); }
uint32_t input_src(uint32_t s) { return 0x00101006u | (s << 4); }
uint32_t resource_src(uint32_t s) { return 0x00107006u | (s << 4); }

bool contains_nocase(const std::string& s, const char* what) {
    std::string a = s, b = what;
    for (auto& ch : a) ch = char(tolower(uint8_t(ch)));
    for (auto& ch : b) ch = char(tolower(uint8_t(ch)));
    return a.find(b) != std::string::npos;
}

uint32_t rd32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    memcpy(&v, d.data() + at, 4);
    return v;
}

bool is_declaration(uint32_t op) { return op >= dxbc::kDclResource && op <= dxbc::kDclGlobalFlags; }

}  // namespace

const char* name(Result r) {
    switch (r) {
        case Result::NotPixelShader: return "not a pixel shader";
        case Result::NotLightPass: return "not a light pass";
        case Result::ReadsShadow: return "a light pass reading t15";
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

    // A light pass: g_lightingFragmentConstants with more than its colour read (attenuation, hotspot), and none of the
    // ambient of g_commonFragmentConstants (the base passes read the light's colour, and the ambient).
    uint32_t light_cb = UINT32_MAX;
    bool common_cb0 = false;
    for (const dxbc::Binding& b : dxbc::bindings(c)) {
        if (b.type == 0 && contains_nocase(b.name, "lightingFragment")) light_cb = b.slot;
        if (b.type == 0 && b.slot == 0 && contains_nocase(b.name, "commonFragment")) common_cb0 = true;
    }
    if (light_cb == UINT32_MAX) return r.result = Result::NotLightPass;
    bool attenuation = false, ambient = false, t15 = false, retc = false, other_output = false;
    uint32_t temps = 0, position = UINT32_MAX;
    int temps_at = -1, last_resource_at = -1, first_input_at = -1, output_at = -1, last_dcl = -1;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (is_declaration(in.opcode) || in.opcode == dxbc::kCustomData) {
            if (is_declaration(in.opcode)) last_dcl = int(i);
            if (in.opcode == dxbc::kDclResource && !in.operands.empty()) {
                last_resource_at = int(i);
                if (in.operands[0].index[0] == kShadowSlot) t15 = true;
            } else if (in.opcode == dxbc::kDclTemps && in.length == 2) {
                temps = p.tokens[in.offset + 1];
                temps_at = int(i);
            } else if (in.opcode == kDclInputPs || in.opcode == kDclInputPsSgv || in.opcode == dxbc::kDclInputPsSiv) {
                if (first_input_at < 0) first_input_at = int(i);
                if (in.opcode == dxbc::kDclInputPsSiv && !in.operands.empty() && p.tokens[in.offset + in.length - 1] == 1)
                    position = in.operands[0].index[0];  // D3D10_SB_NAME_POSITION
            } else if (in.opcode == dxbc::kDclOutput && !in.operands.empty()) {
                if (output_at < 0) output_at = int(i);
                if (in.operands[0].type != dxbc::kOutput || in.operands[0].index[0] != 0) other_output = true;
            }
            continue;
        }
        if (in.opcode == dxbc::kRetc) retc = true;
        for (const Operand& o : in.operands) {
            if (o.type != dxbc::kConstantBuffer || o.relative) continue;
            if (o.index[0] == light_cb && o.index[1] >= 1) attenuation = true;
            if (common_cb0 && o.index[0] == 0 && (o.index[1] == 2 || o.index[1] == 3)) ambient = true;
        }
    }
    if (!attenuation || ambient) return r.result = Result::NotLightPass;
    if (t15) return r.result = Result::ReadsShadow;
    if (other_output) return fail("an output other than o0");
    if (retc) return fail("a conditional return");
    if (output_at < 0) return fail("no output declared");

    // The pixel position: declared, or found in the input signature (SV_Position) and declared here.
    dxbc::Chunk* isgn = c.find("ISGN");
    std::vector<uint32_t> dcl_position;
    if (position == UINT32_MAX) {
        if (!isgn || isgn->data.size() < 8) return fail("no input signature");
        const uint32_t count = rd32(isgn->data, 0);
        size_t used_at = 0;
        for (uint32_t k = 0; k < count; ++k) {
            const size_t e = 8 + size_t(k) * 24;
            if (e + 24 > isgn->data.size()) return fail("a short input signature");
            if (rd32(isgn->data, e + 8) == 1 /* D3D_NAME_POSITION */) {
                position = rd32(isgn->data, e + 16);
                used_at = e + 21;
            }
        }
        if (position == UINT32_MAX) return fail("no SV_Position in the input signature");
        isgn->data[used_at] |= 0x3;  // read: xy
        // dcl_input_ps_siv linear noperspective v<position>.xy, position
        dcl_position = {100u | (4u << 11) | (4u << 24), 0x00101032u, position, 1u};
    }

    // The rewrite: o0's writes go to rK; before each ret,
    //   ftoi rN.xy, v<pos>.xyxx
    //   mov  rN.zw, l(0, 0, 0, 0)
    //   ld   rN.xyzw, rN.xyzw, t15.xyzw
    //   mul  o0.xyz, rK.xyzx, rN.yyyy
    //   mov  o0.w, rK.wwww
    // with t15 declared (texture2d, float) and two more temps.
    const uint32_t tn = temps, tk = temps + 1;
    const std::vector<uint32_t> dcl_t15 = {88u | (3u << 11) | (4u << 24), 0x00107000u, kShadowSlot, 0x00005555u};
    const std::vector<uint32_t> tail = {
        op_token(kFtoi, 5), temp_dst(0x3), tn, input_src(swz(0, 1, 0, 0)), position,
        op_token(dxbc::kMov, 8), temp_dst(0xC), tn, 0x00004002u, 0, 0, 0, 0,
        op_token(dxbc::kLd, 7), temp_dst(0xF), tn, temp_src(swz(0, 1, 2, 3)), tn, resource_src(swz(0, 1, 2, 3)), kShadowSlot,
        op_token(dxbc::kMul, 7), output_dst(0x7), 0, temp_src(swz(0, 1, 2, 0)), tk, temp_src(swz(1, 1, 1, 1)), tn,
        op_token(dxbc::kMov, 5), output_dst(0x8), 0, temp_src(swz(3, 3, 3, 3)), tk,
    };
    std::vector<uint32_t> t;
    t.reserve(p.tokens.size() + 64);
    const uint32_t length = p.tokens[1];
    int rets = 0, redirected = 0;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (!dcl_position.empty() && int(i) == (first_input_at >= 0 ? first_input_at : output_at))
            t.insert(t.end(), dcl_position.begin(), dcl_position.end());
        if (in.opcode == dxbc::kRet) {
            t.insert(t.end(), tail.begin(), tail.end());
            ++rets;
        }
        const size_t at = t.size();
        t.insert(t.end(), p.tokens.begin() + in.offset, p.tokens.begin() + in.offset + in.length);
        if (!is_declaration(in.opcode) && in.opcode != dxbc::kCustomData) {
            const int nd = dxbc::destinations(in.opcode);
            for (int d = 0; d < nd && d < int(in.operands.size()); ++d) {
                const Operand& o = in.operands[size_t(d)];
                if (o.type != dxbc::kOutput || o.index[0] != 0 || o.dims != 1 || o.relative) continue;
                // the operand token's type field to temp, its index to K (extended operand tokens skipped)
                size_t k = at + o.first;
                t[k] &= ~(0xFFu << 12);
                bool ext = (t[k] >> 31) != 0;
                while (ext) {
                    ++k;
                    ext = (t[k] >> 31) != 0;
                }
                t[k + 1] = tk;
                ++redirected;
            }
        }
        if (int(i) == temps_at) t[t.size() - 1] = temps + 2;
        if (int(i) == last_resource_at) t.insert(t.end(), dcl_t15.begin(), dcl_t15.end());
        if (last_resource_at < 0 && int(i) == last_dcl) t.insert(t.end(), dcl_t15.begin(), dcl_t15.end());
        if (temps_at < 0 && int(i) == last_dcl) {
            t.push_back(op_token(dxbc::kDclTemps, 2));
            t.push_back(2);
        }
    }
    if (!rets) return fail("no ret");
    if (!redirected) return fail("o0 is never written");
    std::vector<uint32_t> program;
    program.push_back(p.tokens[0]);
    program.push_back(uint32_t(2 + t.size()));
    program.insert(program.end(), t.begin(), t.end());
    for (size_t i = length; i < p.tokens.size(); ++i) program.push_back(p.tokens[i]);
    shdr->data.assign(reinterpret_cast<const uint8_t*>(program.data()),
                      reinterpret_cast<const uint8_t*>(program.data()) + program.size() * 4);
    dxbc::Program check;
    if (!dxbc::decode(*shdr, &check, &error)) return fail("the rewritten program does not decode: " + error);
    const size_t added = size_t(rets) * 5 + 1 + (dcl_position.empty() ? 0 : 1) + (temps_at < 0 ? 1 : 0);
    if (check.code.size() != p.code.size() + added) return fail("the rewritten program has the wrong instruction count");
    if (out) *out = dxbc::serialize(c);
    r.result = Result::Patched;
    return r.result;
}

}  // namespace light_pass
