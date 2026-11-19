#include "patch/sun_tint.h"

#include <cctype>

#include "patch/dxbc.h"

namespace sun_tint {
namespace {

using dxbc::Instruction;
using dxbc::Operand;

bool is_declaration(uint32_t op) { return (op >= dxbc::kDclResource && op <= dxbc::kDclGlobalFlags) || op == dxbc::kCustomData; }

uint32_t swz(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a | b << 2 | c << 4 | d << 6; }
uint32_t op_token(uint32_t opcode, uint32_t length) { return opcode | (length << 24); }
uint32_t temp_dst(uint32_t mask) { return 0x00100002u | (mask << 4); }
uint32_t temp_src(uint32_t s) { return 0x00100006u | (s << 4); }
uint32_t input_src(uint32_t s) { return 0x00101006u | (s << 4); }
uint32_t resource_src(uint32_t s) { return 0x00107006u | (s << 4); }
uint32_t cb_src(uint32_t s) { return 0x00208006u | (s << 4); }  // cb<slot>[<register>], both immediate
constexpr uint32_t kImm4 = 0x00004002u;                          // a 4-component immediate32 operand

bool contains_nocase(const std::string& s, const char* what) {
    std::string a = s, b = what;
    for (auto& ch : a) ch = char(tolower(uint8_t(ch)));
    for (auto& ch : b) ch = char(tolower(uint8_t(ch)));
    return a.find(b) != std::string::npos;
}

bool reads_sun(const Operand& o) { return o.type == dxbc::kConstantBuffer && o.index[0] == 1 && o.index[1] == 0; }

}  // namespace

const char* name(Result r) {
    switch (r) {
        case Result::NotPixelShader: return "not a pixel shader";
        case Result::NoSun: return "no sun's colour read";
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

    bool lighting_cb1 = false;
    for (const dxbc::Binding& b : dxbc::bindings(c))
        if (b.type == 0 && b.slot == 1 && contains_nocase(b.name, "lightingFragment")) lighting_cb1 = true;
    if (!lighting_cb1) return r.result = Result::NoSun;

    uint32_t temps = 0, position = UINT32_MAX;
    int temps_at = -1, t15_at = -1;
    bool has_t20 = false;
    std::vector<int> reads;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (in.opcode == dxbc::kDclResource && !in.operands.empty()) {
            if (in.operands[0].index[0] == 15) t15_at = int(i);
            if (in.operands[0].index[0] == kSunTintSlot) has_t20 = true;
        } else if (in.opcode == dxbc::kDclTemps && in.length == 2) {
            temps = p.tokens[in.offset + 1];
            temps_at = int(i);
        } else if (in.opcode == dxbc::kDclInputPsSiv && !in.operands.empty() && p.tokens[in.offset + in.length - 1] == 1) {
            position = in.operands[0].index[0];  // D3D10_SB_NAME_POSITION
        }
        if (is_declaration(in.opcode)) continue;
        const int dsts = dxbc::destinations(in.opcode);
        bool sun = false;
        for (size_t k = size_t(dsts > 0 ? dsts : 0); k < in.operands.size(); ++k)
            if (reads_sun(in.operands[k])) {
                if (in.operands[k].relative) return fail("the sun's colour read with a relative index");
                sun = true;
            }
        if (sun) reads.push_back(int(i));
    }
    if (t15_at < 0 || reads.empty()) return r.result = Result::NoSun;
    if (has_t20) return fail("t20 is declared already");
    if (position == UINT32_MAX) return fail("no pixel position input");
    if (temps_at < 0) return fail("no temps declared");
    r.reads = uint32_t(reads.size());

    // the tint at the pixel into rT, the sun's colour as it gets through into rS (the game's where alpha is 0)
    const uint32_t tt = temps, ts = temps + 1;
    const std::vector<uint32_t> load = {
        op_token(27 /* ftoi */, 5), temp_dst(0x3), tt, input_src(swz(0, 1, 0, 0)), position,
        op_token(dxbc::kMov, 8), temp_dst(0xC), tt, kImm4, 0, 0, 0, 0,
        op_token(dxbc::kLd, 7), temp_dst(0xF), tt, temp_src(swz(0, 1, 2, 3)), tt, resource_src(swz(0, 1, 2, 3)), kSunTintSlot,
        op_token(dxbc::kMul, 8), temp_dst(0x7), ts, cb_src(swz(0, 1, 2, 0)), 1, 0, temp_src(swz(0, 1, 2, 0)), tt,
        op_token(dxbc::kMovc, 10), temp_dst(0x7), ts, temp_src(swz(3, 3, 3, 3)), tt, temp_src(swz(0, 1, 2, 0)), ts,
        cb_src(swz(0, 1, 2, 0)), 1, 0,
        op_token(dxbc::kMov, 6), temp_dst(0x8), ts, cb_src(swz(3, 3, 3, 3)), 1, 0,
    };
    constexpr size_t kAdded = 6;
    // a reading instruction with its cb1[0] sources reading rS (same components, same modifiers)
    auto reread = [&](const Instruction& in) -> std::vector<uint32_t> {
        const uint32_t* tk = p.tokens.data() + in.offset;
        if (tk[0] >> 31) return {};  // an extended opcode: not ours to rewrite
        std::vector<uint32_t> o2(tk, tk + in.operands[0].first);
        const int dsts = dxbc::destinations(in.opcode);
        for (size_t k = 0; k < in.operands.size(); ++k) {
            const Operand& o = in.operands[k];
            if (int(k) >= dsts && reads_sun(o)) {
                o2.push_back((tk[o.first] & 0x80000FFFu) | (1u << 20));  // a temp, one immediate index
                uint32_t at = o.first;
                bool ext = (tk[at] >> 31) != 0;
                while (ext) {  // its modifiers
                    ++at;
                    o2.push_back(tk[at]);
                    ext = (tk[at] >> 31) != 0;
                }
                o2.push_back(ts);
            } else {
                o2.insert(o2.end(), tk + o.first, tk + o.first + o.length);
            }
        }
        o2[0] = (o2[0] & ~(0x7Fu << 24)) | (uint32_t(o2.size()) << 24);
        return o2;
    };

    const Instruction& d15 = p.code[size_t(t15_at)];
    std::vector<uint32_t> dcl_t20(p.tokens.begin() + d15.offset, p.tokens.begin() + d15.offset + d15.length);
    dcl_t20[d15.operands[0].first + 1] = kSunTintSlot;  // the operand's index token

    std::vector<uint32_t> t;
    t.reserve(p.tokens.size() + load.size() + dcl_t20.size() + reads.size() * 2);
    const uint32_t length = p.tokens[1];
    size_t next = 0;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (int(i) == reads.front()) t.insert(t.end(), load.begin(), load.end());
        if (next < reads.size() && reads[next] == int(i)) {
            ++next;
            const std::vector<uint32_t> op = reread(in);
            if (op.empty()) return fail("an extended opcode reads the sun's colour");
            t.insert(t.end(), op.begin(), op.end());
            continue;
        }
        t.insert(t.end(), p.tokens.begin() + in.offset, p.tokens.begin() + in.offset + in.length);
        if (int(i) == temps_at) t[t.size() - 1] = temps + 2;
        if (int(i) == t15_at) t.insert(t.end(), dcl_t20.begin(), dcl_t20.end());
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
    if (check.code.size() != p.code.size() + kAdded + 1) return fail("the rewritten program has the wrong instruction count");
    if (out) *out = dxbc::serialize(c);
    r.result = Result::Patched;
    return r.result;
}

}  // namespace sun_tint
