#include "patch/dxbc.h"

#include <cstdio>
#include <cstring>

namespace dxbc {
namespace {

constexpr uint32_t kMagic = 0x43425844;  // "DXBC"

uint32_t fourcc(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
}

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// --- MD5 (RFC 1321), the block transform the DXBC checksum is built on -----------------------------------------------

uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

void md5_block(uint32_t s[4], const uint8_t block[64]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9, 14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = rd32(block + 4 * i);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = uint32_t(i);
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = uint32_t(5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = uint32_t(3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = uint32_t(7 * i) & 15;
        }
        const uint32_t t = d;
        d = c;
        c = b;
        b = b + rotl(a + f + K[i] + m[g], R[i]);
        a = t;
    }
    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
}

// --- SM4 operands ------------------------------------------------------------------------------------------------------

bool read_operand(const std::vector<uint32_t>& t, uint32_t& pos, uint32_t end, uint32_t base, Operand& o) {
    if (pos >= end) return false;
    o.first = pos - base;
    const uint32_t tok = t[pos++];
    const uint32_t num = tok & 3;
    o.components = num == 0 ? 0 : num == 1 ? 1 : 4;
    if (num == 2) {
        o.mode = (tok >> 2) & 3;
        if (o.mode == 0) {
            o.mask = (tok >> 4) & 0xF;
        } else if (o.mode == 1) {
            for (int i = 0; i < 4; ++i) o.swizzle[i] = uint8_t((tok >> (4 + 2 * i)) & 3);
        } else {
            const uint8_t c = uint8_t((tok >> 4) & 3);
            for (auto& s : o.swizzle) s = c;
        }
    } else if (num == 1) {
        for (auto& s : o.swizzle) s = 0;
    }
    o.type = (tok >> 12) & 0xFF;
    o.dims = (tok >> 20) & 3;
    uint32_t rep[3] = {(tok >> 22) & 7, (tok >> 25) & 7, (tok >> 28) & 7};
    bool extended = (tok >> 31) != 0;
    while (extended) {
        if (pos >= end) return false;
        const uint32_t ext = t[pos++];
        if ((ext & 0x3F) == 1) {
            const uint32_t mod = (ext >> 6) & 0xFF;
            o.neg = mod == 1 || mod == 3;
            o.abs = mod == 2 || mod == 3;
        }
        extended = (ext >> 31) != 0;
    }
    for (uint32_t d = 0; d < o.dims && d < 3; ++d) {
        switch (rep[d]) {
            case 0:  // immediate32
                if (pos >= end) return false;
                o.index[d] = t[pos++];
                break;
            case 1:  // immediate64
                if (pos + 1 >= end) return false;
                o.index[d] = t[pos];
                pos += 2;
                break;
            case 2: {  // relative
                Operand rel;
                o.relative = true;
                if (!read_operand(t, pos, end, base, rel)) return false;
                break;
            }
            case 3: {  // immediate32 + relative
                if (pos >= end) return false;
                o.index[d] = t[pos++];
                Operand rel;
                o.relative = true;
                if (!read_operand(t, pos, end, base, rel)) return false;
                break;
            }
            case 4: {  // immediate64 + relative
                pos += 2;
                Operand rel;
                o.relative = true;
                if (!read_operand(t, pos, end, base, rel)) return false;
                break;
            }
            default: return false;
        }
    }
    if (o.type == kImmediate32) pos += o.components == 4 ? 4 : 1;
    if (o.type == kImmediate64) pos += o.components == 4 ? 8 : 2;
    if (pos > end) return false;
    o.length = pos - base - o.first;
    return true;
}

bool is_declaration(uint32_t op) { return (op >= kDclResource && op <= kDclGlobalFlags) || (op >= 143 && op <= 182); }

const char* opcode_name(uint32_t op) {
    static const char* names[] = {
        "add", "and", "break", "breakc", "call", "callc", "case", "continue", "continuec", "cut", "default", "deriv_rtx",
        "deriv_rty", "discard", "div", "dp2", "dp3", "dp4", "else", "emit", "emitthencut", "endif", "endloop", "endswitch",
        "eq", "exp", "frc", "ftoi", "ftou", "ge", "iadd", "if", "ieq", "ige", "ilt", "imad", "imax", "imin", "imul", "ine",
        "ineg", "ishl", "ishr", "itof", "label", "ld", "ld_ms", "log", "loop", "lt", "mad", "min", "max", "customdata",
        "mov", "movc", "mul", "ne", "nop", "not", "or", "resinfo", "ret", "retc", "round_ne", "round_ni", "round_pi",
        "round_z", "rsq", "sample", "sample_c", "sample_c_lz", "sample_l", "sample_d", "sample_b", "sqrt", "switch",
        "sincos", "udiv", "ult", "uge", "umul", "umad", "umax", "umin", "ushr", "utof", "xor", "dcl_resource",
        "dcl_constantbuffer", "dcl_sampler", "dcl_index_range", "dcl_gs_output_topology", "dcl_gs_input", "dcl_maxout",
        "dcl_input", "dcl_input_sgv", "dcl_input_siv", "dcl_input_ps", "dcl_input_ps_sgv", "dcl_input_ps_siv",
        "dcl_output", "dcl_output_sgv", "dcl_output_siv", "dcl_temps", "dcl_indexable_temp", "dcl_global_flags"};
    return op < sizeof(names) / sizeof(names[0]) ? names[op] : "?";
}

}  // namespace

Chunk* Container::find(const char (&cc)[5]) {
    for (Chunk& c : chunks)
        if (c.fourcc == fourcc(cc)) return &c;
    return nullptr;
}
const Chunk* Container::find(const char (&cc)[5]) const {
    for (const Chunk& c : chunks)
        if (c.fourcc == fourcc(cc)) return &c;
    return nullptr;
}

bool parse(const void* bytes, size_t size, Container* out, std::string* error) {
    const uint8_t* p = static_cast<const uint8_t*>(bytes);
    auto bad = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (size < 32 || rd32(p) != kMagic) return bad("not a DXBC container");
    const uint32_t total = rd32(p + 24), count = rd32(p + 28);
    if (total > size || total < 32 + 4 * count) return bad("container size");
    out->chunks.clear();
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t off = rd32(p + 32 + 4 * i);
        if (off + 8 > total) return bad("chunk offset");
        const uint32_t len = rd32(p + off + 4);
        if (off + 8 + len > total) return bad("chunk size");
        out->chunks.push_back({rd32(p + off), std::vector<uint8_t>(p + off + 8, p + off + 8 + len)});
    }
    return true;
}

void checksum(const uint8_t* c, size_t size, uint32_t out[4]) {
    const uint8_t* data = c + 20;
    const uint32_t n = uint32_t(size - 20);
    const uint32_t bits = n * 8;
    const uint32_t full = n & ~63u, last = n - full;
    uint32_t s[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (uint32_t i = 0; i < full; i += 64) md5_block(s, data + i);
    uint8_t block[64];
    if (last >= 56) {
        memset(block, 0, sizeof(block));
        memcpy(block, data + full, last);
        block[last] = 0x80;
        md5_block(s, block);
        memset(block, 0, sizeof(block));
        memcpy(block, &bits, 4);
        const uint32_t tail = (bits >> 2) | 1;
        memcpy(block + 60, &tail, 4);
        md5_block(s, block);
    } else {
        memset(block, 0, sizeof(block));
        memcpy(block, &bits, 4);
        memcpy(block + 4, data + full, last);
        block[4 + last] = 0x80;
        const uint32_t tail = (bits >> 2) | 1;
        memcpy(block + 60, &tail, 4);
        md5_block(s, block);
    }
    memcpy(out, s, 16);
}

std::vector<uint8_t> serialize(const Container& c) {
    const uint32_t count = uint32_t(c.chunks.size());
    uint32_t size = 32 + 4 * count;
    for (const Chunk& ch : c.chunks) size += 8 + uint32_t(ch.data.size());
    std::vector<uint8_t> out(size, 0);
    auto wr = [&](uint32_t at, uint32_t v) { memcpy(out.data() + at, &v, 4); };
    wr(0, kMagic);
    wr(20, 1);
    wr(24, size);
    wr(28, count);
    uint32_t at = 32 + 4 * count;
    for (uint32_t i = 0; i < count; ++i) {
        wr(32 + 4 * i, at);
        wr(at, c.chunks[i].fourcc);
        wr(at + 4, uint32_t(c.chunks[i].data.size()));
        if (!c.chunks[i].data.empty()) memcpy(out.data() + at + 8, c.chunks[i].data.data(), c.chunks[i].data.size());
        at += 8 + uint32_t(c.chunks[i].data.size());
    }
    uint32_t hash[4];
    checksum(out.data(), out.size(), hash);
    memcpy(out.data() + 4, hash, 16);
    return out;
}

int destinations(uint32_t op) {
    switch (op) {
        case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case kDiscard: case 18: case 19: case 20:
        case 21: case 22: case 23: case kIf: case 44: case 48: case 58: case kRet: case kRetc: case 76: case kCustomData:
            return 0;
        case kSincos: case 38: case 78: case 81: return 2;
        default: return 1;
    }
}

bool decode(const Chunk& shdr, Program* out, std::string* error) {
    auto bad = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (shdr.data.size() < 8 || shdr.data.size() % 4) return bad("program size");
    out->tokens.assign(shdr.data.size() / 4, 0);
    memcpy(out->tokens.data(), shdr.data.data(), shdr.data.size());
    const std::vector<uint32_t>& t = out->tokens;
    out->version = t[0];
    const uint32_t length = t[1];
    if (length > t.size() || length < 2) return bad("program length");
    out->code.clear();
    uint32_t pos = 2;
    while (pos < length) {
        Instruction in;
        in.offset = pos;
        const uint32_t tok = t[pos];
        in.opcode = tok & 0x7FF;
        in.length = in.opcode == kCustomData ? (pos + 1 < length ? t[pos + 1] : 0) : (tok >> 24) & 0x7F;
        if (!in.length || pos + in.length > length) return bad("instruction length");
        in.saturate = (tok & 0x2000) != 0 && !is_declaration(in.opcode);
        if (in.opcode != kCustomData && !is_declaration(in.opcode)) {
            uint32_t p = pos + 1;
            bool ext = (tok >> 31) != 0;
            while (ext && p < pos + in.length) ext = (t[p++] >> 31) != 0;
            while (p < pos + in.length) {
                Operand o;
                if (!read_operand(t, p, pos + in.length, pos, o)) return bad("operand");
                in.operands.push_back(o);
            }
        } else if (in.opcode == kDclResource || in.opcode == kDclSampler || in.opcode == kDclInputPsSiv ||
                   in.opcode == kDclOutput || in.opcode == kDclConstantBuffer) {
            uint32_t p = pos + 1;
            Operand o;
            if (read_operand(t, p, pos + in.length, pos, o)) in.operands.push_back(o);
        }
        out->code.push_back(in);
        pos += in.length;
    }
    return true;
}

std::string describe(const Program& p, const Instruction& in) {
    (void)p;
    std::string s = opcode_name(in.opcode);
    if (in.saturate) s += "_sat";
    static const char* comp = "xyzw";
    for (size_t k = 0; k < in.operands.size(); ++k) {
        const Operand& o = in.operands[k];
        char buf[96];
        const char* prefix = o.type == kTemp ? "r" : o.type == kInput ? "v" : o.type == kOutput ? "o" : o.type == kSampler ? "s"
                           : o.type == kResource ? "t" : o.type == kConstantBuffer ? "cb" : o.type == kImmediate32 ? "l" : "?";
        if (o.type == kConstantBuffer)
            snprintf(buf, sizeof(buf), "%scb%u[%u]", o.neg ? "-" : "", o.index[0], o.index[1]);
        else if (o.type == kImmediate32)
            snprintf(buf, sizeof(buf), "l(...)");
        else
            snprintf(buf, sizeof(buf), "%s%s%u", o.neg ? "-" : "", prefix, o.index[0]);
        std::string op = buf;
        if (o.components == 4 && o.type != kImmediate32) {
            op += '.';
            if (o.mode == 0) {
                for (int c = 0; c < 4; ++c)
                    if (o.mask & (1u << c)) op += comp[c];
            } else {
                for (int c = 0; c < 4; ++c) op += comp[o.swizzle[c]];
            }
        }
        if (o.abs) op = "|" + op + "|";
        s += (k ? ", " : " ") + op;
    }
    return s;
}

std::vector<Binding> bindings(const Container& c) {
    std::vector<Binding> out;
    const Chunk* rdef = c.find("RDEF");
    if (!rdef || rdef->data.size() < 28) return out;
    const uint8_t* d = rdef->data.data();
    const size_t n = rdef->data.size();
    const uint32_t count = rd32(d + 8), offset = rd32(d + 12);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = size_t(offset) + 32 * i;
        if (at + 32 > n) break;
        Binding b;
        const uint32_t name = rd32(d + at);
        for (size_t k = name; k < n && d[k]; ++k) b.name += char(d[k]);
        b.type = rd32(d + at + 4);
        b.slot = rd32(d + at + 20);
        out.push_back(b);
    }
    return out;
}

namespace {

std::string cstr(const uint8_t* d, size_t n, uint32_t at) {
    std::string s;
    for (size_t k = at; k < n && d[k]; ++k) s += char(d[k]);
    return s;
}

// A type's members (D3D11_SHADER_TYPE_DESC as RDEF stores it: class, type, rows, columns, elements, members (u16 each),
// then the members' offset; each member: name, type, offset), flattened with their full byte offsets.
void flatten(const uint8_t* d, size_t n, uint32_t type_at, uint32_t base, const std::string& name, uint32_t slot,
             std::vector<Variable>* out, int depth) {
    if (depth > 8 || size_t(type_at) + 16 > n) return;
    const uint16_t members = uint16_t(d[type_at + 10] | (d[type_at + 11] << 8));
    const uint32_t member_at = rd32(d + type_at + 12);
    if (!members) {
        out->push_back({name, slot, base});
        return;
    }
    for (uint16_t m = 0; m < members; ++m) {
        const size_t at = size_t(member_at) + 12 * m;
        if (at + 12 > n) return;
        flatten(d, n, rd32(d + at + 4), base + rd32(d + at + 8), cstr(d, n, rd32(d + at)), slot, out, depth + 1);
    }
}

}  // namespace

std::vector<Variable> variables(const Container& c) {
    std::vector<Variable> out;
    const Chunk* rdef = c.find("RDEF");
    if (!rdef || rdef->data.size() < 28) return out;
    const uint8_t* d = rdef->data.data();
    const size_t n = rdef->data.size();
    const uint32_t cb_count = rd32(d), cb_at = rd32(d + 4);
    const uint32_t major = d[17];
    const size_t var_size = major >= 5 ? 40 : 24;  // SM5 adds texture and sampler ranges to each variable
    const std::vector<Binding> binds = bindings(c);
    for (uint32_t i = 0; i < cb_count; ++i) {
        const size_t at = size_t(cb_at) + 24 * i;
        if (at + 24 > n) break;
        const std::string cb_name = cstr(d, n, rd32(d + at));
        uint32_t slot = UINT32_MAX;
        for (const Binding& b : binds)
            if (b.type == 0 && b.name == cb_name) slot = b.slot;
        if (slot == UINT32_MAX) continue;
        const uint32_t vars = rd32(d + at + 4), var_at = rd32(d + at + 8);
        for (uint32_t v = 0; v < vars; ++v) {
            const size_t va = size_t(var_at) + var_size * v;
            if (va + 24 > n) break;
            flatten(d, n, rd32(d + va + 16), rd32(d + va + 4), cstr(d, n, rd32(d + va)), slot, &out, 0);
        }
    }
    return out;
}

}  // namespace dxbc
