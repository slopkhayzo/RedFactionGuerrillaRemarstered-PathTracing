#include "patch/ambient.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

#include "patch/dxbc.h"

namespace ambient {
namespace {

using dxbc::Instruction;
using dxbc::Operand;

// What a temp component's value comes from.
constexpr uint32_t kAmbient = 1;    // the ambient colours (g_commonFragmentConstants ambient, backAmbient: cb0[2], cb0[3])
constexpr uint32_t kShading = 2;    // a texture other than a normal map, or ClbTexture: the rest of the shading
constexpr uint32_t kSelfIllum = 4;  // the material's self-illumination (Self_Illumination, illum_cycle, the lerp to 4)
constexpr uint32_t kBounce = 8;     // the fake bounce's constants: indirectLightParams (cb0[7]), lightColor
constexpr uint32_t kWeighted = 16;  // an ambient value the loose pass let a shading weight into (or one made from it)

bool is_sample(uint32_t op) {
    return op == dxbc::kSample || op == dxbc::kSampleC || op == dxbc::kSampleCLz || op == dxbc::kSampleL ||
           op == dxbc::kSampleD || op == dxbc::kSampleB || op == dxbc::kLd || op == 46 /* ld_ms */ || op == 109 /* gather4 */;
}
bool is_declaration(uint32_t op) { return op >= dxbc::kDclResource && op <= dxbc::kDclGlobalFlags; }

bool contains_nocase(const std::string& s, const char* what) {
    std::string a = s, b = what;
    for (auto& ch : a) ch = char(tolower(uint8_t(ch)));
    for (auto& ch : b) ch = char(tolower(uint8_t(ch)));
    return a.find(b) != std::string::npos;
}

// What shading a value carries, as a vector (a scalar weight carries none): a diffuse one (albedo, decal, ClbTexture)
// or a specular one (specular maps and colours).
constexpr uint32_t kRoleDiffuse = 1, kRoleSpecular = 2;

struct Comp {
    uint32_t taint = 0;
    uint32_t slots = 0;  // the textures it met (t0-t31, a bit each)
    uint32_t roles = 0;  // kRole*
    int def = -1;        // the instruction that wrote it
};

uint32_t swz(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a | b << 2 | c << 4 | d << 6; }
uint32_t op_token(uint32_t opcode, uint32_t length) { return opcode | (length << 24); }
uint32_t temp_dst(uint32_t mask) { return 0x00100002u | (mask << 4); }
uint32_t temp_src(uint32_t s) { return 0x00100006u | (s << 4); }
uint32_t input_src(uint32_t s) { return 0x00101006u | (s << 4); }
uint32_t resource_src(uint32_t s) { return 0x00107006u | (s << 4); }
uint32_t cb_src(uint32_t s) { return 0x00208006u | (s << 4); }  // cb<slot>[<register>], both immediate

}  // namespace

const char* name(Result r) {
    switch (r) {
        case Result::NotPixelShader: return "not a pixel shader";
        case Result::NotLit: return "not lit";
        case Result::NoAmbient: return "lit, no ambient";
        case Result::Patched: return "patched";
        default: return "unsupported";
    }
}

// The diffuse ambient: the game's hemispherical ambient lerp(backAmbient, ambient, N.up) with its fake bounce
// (lightColor * indirectLightParams, weighted toward the horizon; terrain orders the two lerps the other way), before
// the self-illumination's lerp to 4 and the albedo apply to it. The specular ambient (the same sky looked up along the
// reflection vector, times the specular map) shares the first steps and is left as it is.
//
// The dataflow: every instruction whose value comes from the ambient colours, with nothing but scalar weights besides
// (N.up, the horizon factor, an AO, even when a texture gave them), is an ambient node; a consumer is where an ambient
// value meets shading as a vector: a diffuse one with a diffuse, decal, alpha or environment texture or ClbTexture, a
// specular one with specular textures only. The value replaced is the latest ambient node, three components in a temp
// and no self-illumination in it (a Self_Illumination or illum_cycle constant, or the lerp toward 4), that every path
// from the ambient colours to a diffuse consumer passes through, and that no specular consumer reads. An AO multiplied
// in before that is replaced with the rest: the path-traced irradiance holds the occlusion.
namespace {

// The analysis and the rewrite, for the diffuse ambient (into `slot`: the irradiance) or the specular one (the
// reflections).
Result patch_one(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report, bool specular, uint32_t slot) {
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

    // What the shader declares: its textures (by slot), temps, the pixel position input.
    const std::vector<dxbc::Binding> binds = dxbc::bindings(c);
    bool lit = false, ambient_cb = false;
    uint32_t temps = 0, position = UINT32_MAX;
    int temps_at = -1, t15_at = -1, imm4_token = -1;
    std::set<uint32_t> textures;
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (in.opcode == dxbc::kDclResource && !in.operands.empty()) {
            textures.insert(in.operands[0].index[0]);
            if (in.operands[0].index[0] == 15) {
                lit = true;
                t15_at = int(i);
            }
        } else if (in.opcode == dxbc::kDclTemps && in.length == 2) {
            temps = p.tokens[in.offset + 1];
            temps_at = int(i);
        } else if (in.opcode == dxbc::kDclInputPsSiv && !in.operands.empty() && p.tokens[in.offset + in.length - 1] == 1) {
            position = in.operands[0].index[0];  // D3D10_SB_NAME_POSITION
        }
        for (const Operand& o : in.operands)
            if (o.type == dxbc::kImmediate32 && o.components == 4 && imm4_token < 0) imm4_token = int(p.tokens[in.offset + o.first]);
    }
    r.temps_before = temps;
    if (!lit) return r.result = Result::NotLit;
    for (const dxbc::Binding& b : binds)
        if (b.type == 0 && b.slot == 0 && contains_nocase(b.name, "commonFragment")) ambient_cb = true;
    if (textures.count(slot)) return fail("t" + std::to_string(slot) + " is in use");
    // The textures by role: normal maps and alpha maps (what N.up is made of: the terrain and decals blend their normals
    // by alpha maps), specular ones, the rest.
    uint32_t all_slots = 0, normal_slots = 0, alpha_slots = 0, specular_slots = 0;
    for (uint32_t tex : textures) {
        if (tex >= 32) continue;
        all_slots |= 1u << tex;
        for (const dxbc::Binding& b : binds) {
            if (b.type != 2 || b.slot != tex) continue;
            if (contains_nocase(b.name, "Normal")) normal_slots |= 1u << tex;
            if (contains_nocase(b.name, "Alpha_Map")) alpha_slots |= 1u << tex;
            // (Environment_Map is a 2D layer blended into the albedo in the game's materials, not a reflection)
            if (contains_nocase(b.name, "Specular") || contains_nocase(b.name, "reflection") || contains_nocase(b.name, "BRDF_Spec"))
                specular_slots |= 1u << tex;
        }
    }
    uint32_t light_cb = UINT32_MAX;
    for (const dxbc::Binding& b : binds)
        if (b.type == 0 && contains_nocase(b.name, "lightingFragment")) light_cb = b.slot;
    // The self-illumination constants: (cb slot, register, component); the material's colours: (cb slot, register) ->
    // role (a Diffuse_Color multiplies the ambient as an albedo does).
    uint32_t material_cb = UINT32_MAX;
    for (const dxbc::Binding& b : binds)
        if (b.type == 0 && contains_nocase(b.name, "material")) material_cb = b.slot;  // g_material(Fragment)Constants
    std::set<std::array<uint32_t, 3>> self_illum;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> colour_roles;
    for (const dxbc::Variable& v : dxbc::variables(c)) {
        if (contains_nocase(v.name, "Self_Illum") || contains_nocase(v.name, "illum_cycle"))
            self_illum.insert({v.slot, v.offset / 16, (v.offset % 16) / 4});
        if (v.slot == material_cb && contains_nocase(v.name, "Color") && v.offset % 16 == 0)
            colour_roles[{v.slot, v.offset / 16}] = contains_nocase(v.name, "Specular") ? kRoleSpecular : kRoleDiffuse;
    }
    if (position == UINT32_MAX) return fail("no SV_Position input");
    if (temps_at < 0 || t15_at < 0) return fail("no dcl_temps or t15 declaration");

    // One pass of the analysis. Strict: anything a texture (other than a normal or alpha map) or ClbTexture gave ends
    // the ambient where it meets it, scalar or not (an AO, a rim factor stay outside what is replaced). Loose, for the
    // materials whose N.up is itself a texture's (foliage's alpha) or an AO's: a scalar operand is a weight inside it.
    // Either way an addition of anything but the ambient or the bounce's constants ends it (a rim light added on).
    struct Choice {
        int found = -1;
        uint32_t mask = 0, reg = 0, swz[4] = {0, 0, 0, 0};
        std::vector<int> direct;
        bool any_ambient = false;
        std::string why;
    };
    const size_t n = p.code.size();
    auto analyze = [&](bool strict, Choice* ch) -> bool {
        const uint32_t shading_slots = all_slots & ~normal_slots & (strict ? ~alpha_slots : ~0u);
        std::vector<std::array<Comp, 4>> regs(std::max<uint32_t>(temps, 1));
        std::vector<char> node(n, 0), consumer(n, 0), diffuse(n, 0), reads_const(n, 0);
        std::vector<uint32_t> node_taint(n, 0);
        std::vector<std::set<int>> parents(n);  // the ambient nodes an instruction reads
        std::vector<std::set<int>> addend_of(n);  // a consumer's parents it only adds (not a diffuse use of theirs)
        for (size_t i = 0; i < n; ++i) {
            const Instruction& in = p.code[i];
            if (is_declaration(in.opcode) || in.opcode == dxbc::kCustomData) continue;
            const int nd = dxbc::destinations(in.opcode);
            if (nd == 0 || int(in.operands.size()) <= nd) continue;
            bool constant = false;
            // one component of a source: its taint, its textures, the ambient node that wrote it
            auto read = [&](const Operand& o, uint32_t sel, Comp& acc, std::set<int>& defs) {
                if (o.type == dxbc::kTemp && o.index[0] < regs.size()) {
                    const Comp& cv = regs[o.index[0]][sel];
                    acc.taint |= cv.taint;
                    acc.slots |= cv.slots;
                    acc.roles |= cv.roles;
                    // the dataflow's edges: through components that carry the ambient (a register may hold others too)
                    if (cv.def >= 0 && node[size_t(cv.def)] && (cv.taint & kAmbient)) defs.insert(cv.def);
                } else if (o.type == dxbc::kConstantBuffer && !o.relative) {
                    auto colour = colour_roles.find({o.index[0], o.index[1]});
                    if (colour != colour_roles.end() && sel < 3) {
                        acc.taint |= kShading;
                        acc.roles |= colour->second;
                    }
                    if (ambient_cb && o.index[0] == 0 && (o.index[1] == 2 || o.index[1] == 3)) {
                        acc.taint |= kAmbient;
                        constant = true;
                    }
                    if ((ambient_cb && o.index[0] == 0 && o.index[1] == 7) || (o.index[0] == light_cb && o.index[1] == 0))
                        acc.taint |= kBounce;
                    if (self_illum.count({o.index[0], o.index[1], sel})) acc.taint |= kSelfIllum;
                } else if (o.type == dxbc::kImmediate32 && o.components == 4 && !(p.tokens[in.offset + o.first] >> 31)) {
                    // the self-illumination's lerp toward 4 (some materials drive it by a constant with no telling name)
                    const uint32_t* v = p.tokens.data() + in.offset + o.first + 1;
                    if (v[0] == 0x40800000u && v[1] == 0x40800000u && v[2] == 0x40800000u) acc.taint |= kSelfIllum;
                }
            };
            Comp val[4];
            std::set<int> defs[4];
            Comp src[4];                // per source operand, over the components read for the written ones
            uint32_t src_read[4] = {};  // which of its components were read
            std::set<int> src_defs[4];  // the ambient nodes read through it
            Comp per[4][4];             // component-wise instructions: what each written component reads of each source
            bool componentwise = false;
            std::set<int> unused;
            const int ns = int(in.operands.size()) - nd;
            if (is_sample(in.opcode)) {
                Comp acc;
                std::set<int> d;
                for (size_t k = nd; k < in.operands.size(); ++k) {
                    const Operand& o = in.operands[k];
                    if (o.type == dxbc::kResource && o.index[0] < 32) {
                        acc.slots |= 1u << o.index[0];
                        if (shading_slots & (1u << o.index[0])) {
                            acc.taint |= kShading;
                            acc.roles |= (specular_slots & (1u << o.index[0])) ? kRoleSpecular : kRoleDiffuse;
                        }
                    }
                    if (k == size_t(nd))
                        for (uint32_t comp = 0; comp < 4; ++comp) read(o, o.components == 4 ? o.swizzle[comp] : 0, acc, d);
                }
                for (int comp = 0; comp < 4; ++comp) {
                    val[comp] = acc;
                    defs[comp] = d;
                }
                if (ns > 0) {
                    src[0] = acc;
                    src_read[0] = 0xF;  // a texture's value: a vector
                }
            } else if (in.opcode == dxbc::kDp2 || in.opcode == dxbc::kDp3 || in.opcode == dxbc::kDp4) {
                const uint32_t k = in.opcode == dxbc::kDp2 ? 2 : in.opcode == dxbc::kDp3 ? 3 : 4;
                Comp acc;
                std::set<int> d;
                for (int s = 0; s < ns && s < 4; ++s) {
                    const Operand& o = in.operands[size_t(nd + s)];
                    for (uint32_t comp = 0; comp < k; ++comp) {
                        const uint32_t sel = o.components == 4 ? o.swizzle[comp] : 0;
                        read(o, sel, acc, d);
                        read(o, sel, src[s], src_defs[s]);
                        src_read[s] |= 1u << sel;
                    }
                }
                for (int comp = 0; comp < 4; ++comp) {
                    val[comp] = acc;
                    defs[comp] = d;
                }
            } else {
                componentwise = true;
                for (int dst = 0; dst < nd; ++dst) {
                    const Operand& od = in.operands[size_t(dst)];
                    const uint32_t mask = od.components == 4 ? od.mask : 1;
                    for (uint32_t comp = 0; comp < 4; ++comp) {
                        if (!(mask & (1u << comp))) continue;
                        for (int s = 0; s < ns && s < 4; ++s) {
                            const Operand& o = in.operands[size_t(nd + s)];
                            const uint32_t sel = o.components == 4 ? o.swizzle[comp] : 0;
                            read(o, sel, val[comp], defs[comp]);
                            read(o, sel, src[s], src_defs[s]);
                            read(o, sel, per[comp][s], unused);
                            src_read[s] |= 1u << sel;
                        }
                    }
                }
            }
            // shading met as a vector, or as a scalar; what the vectors met; an addition of something foreign
            // A component-wise instruction is judged by each written component that carries the ambient, from what that
            // component reads (a register may pack unrelated values: the specular ambient beside a horizon weight).
            bool vector_shading = false, scalar_shading = false, foreign_addend = false;
            uint32_t vector_roles = 0, output_roles = 0;
            std::set<int> added;  // ambient nodes read only as something added (a mad's third operand, an add's)
            uint32_t written = 0;
            for (int dst = 0; dst < nd; ++dst) {
                const Operand& od = in.operands[size_t(dst)];
                written |= od.components == 4 ? od.mask : 1;
            }
            for (int s = 0; s < ns && s < 4; ++s) {
                const uint32_t m = src_read[s];
                const bool scalar = m && !(m & (m - 1));
                if (!scalar) output_roles |= src[s].roles;  // a weight carries no role on
                const bool addend = (in.opcode == dxbc::kAdd && s < 2) || (in.opcode == dxbc::kMad && s == 2);
                const bool immediate = in.operands[size_t(nd + s)].type == dxbc::kImmediate32;
                if (componentwise) {
                    for (uint32_t comp = 0; comp < 4; ++comp) {
                        if (!(written & (1u << comp)) || !(val[comp].taint & kAmbient)) continue;
                        const Comp& sc = per[comp][s];
                        if (sc.taint & kShading) {
                            (scalar ? scalar_shading : vector_shading) = true;
                            if (!scalar) vector_roles |= sc.roles;
                        }
                        if (addend && !immediate && !(sc.taint & (kAmbient | kBounce))) foreign_addend = true;
                    }
                } else {
                    if (src[s].taint & kShading) {
                        (scalar ? scalar_shading : vector_shading) = true;
                        if (!scalar) vector_roles |= src[s].roles;
                    }
                    if (addend && !immediate && !(src[s].taint & (kAmbient | kBounce))) foreign_addend = true;
                }
                if (addend) added.insert(src_defs[s].begin(), src_defs[s].end());
            }
            for (int s = 0; s < ns && s < 4; ++s) {
                const bool addend = (in.opcode == dxbc::kAdd && s < 2) || (in.opcode == dxbc::kMad && s == 2);
                if (!addend)
                    for (int q : src_defs[s]) added.erase(q);  // read as a factor too
            }
            // the written components
            bool wrote = false, output = false;
            uint32_t taint = 0;
            std::set<int> read_nodes;
            for (int dst = 0; dst < nd; ++dst) {
                const Operand& o = in.operands[size_t(dst)];
                const uint32_t mask = o.components == 4 ? o.mask : 1;
                for (uint32_t comp = 0; comp < 4; ++comp) {
                    if (!(mask & (1u << comp))) continue;
                    wrote = true;
                    taint |= val[comp].taint;
                    read_nodes.insert(defs[comp].begin(), defs[comp].end());
                    if (o.type == dxbc::kOutput) output = true;
                }
            }
            if (!wrote) continue;
            const bool ambient_in = (taint & kAmbient) != 0;
            if (ambient_in) ch->any_ambient = true;
            reads_const[i] = constant;
            const bool is_node = ambient_in && !vector_shading && !(strict && scalar_shading) && !foreign_addend && !output;
            const uint32_t weighted = scalar_shading ? kWeighted : 0;  // the loose pass only (strict ends a node there)
            if (is_node) {
                node[i] = 1;
                node_taint[i] = (taint & ~kShading) | weighted;
                parents[i] = read_nodes;
            } else if (ambient_in && (!read_nodes.empty() || constant)) {
                consumer[i] = 1;
                parents[i] = read_nodes;
                addend_of[i] = added;
                // diffuse unless what it meets as vectors is specular alone (a scalar meeting, an AO, is diffuse)
                diffuse[i] = vector_roles != kRoleSpecular;
            }
            // the written components' values: an ambient node's is ambient (and self-illumination), without the weights'
            for (int dst = 0; dst < nd; ++dst) {
                const Operand& o = in.operands[size_t(dst)];
                if (o.type != dxbc::kTemp || o.index[0] >= regs.size()) continue;
                const uint32_t mask = o.components == 4 ? o.mask : 1;
                for (uint32_t comp = 0; comp < 4; ++comp) {
                    if (!(mask & (1u << comp))) continue;
                    const Comp& v = val[comp];
                    regs[o.index[0]][comp] =
                        is_node ? Comp{(v.taint & ~kShading) | weighted, 0, 0, int(i)} : Comp{v.taint, v.slots, output_roles, int(i)};
                }
            }
        }
        if (!ch->any_ambient) return false;
        // A use of node x by consumer y, for this target: the diffuse ambient's are diffuse consumers reading it as a
        // factor; the specular ambient's are specular consumers, or any that only adds it on (a specular ambient
        // scaled by scalars alone, added to the lit colour).
        auto is_use = [&](int x, int y) {
            if (!consumer[size_t(y)]) return false;
            const bool added_only = addend_of[size_t(y)].count(x) != 0;
            return specular ? (!diffuse[size_t(y)] || added_only) : (diffuse[size_t(y)] && !added_only);
        };
        auto own_kind = [&](size_t i) { return specular ? !diffuse[i] : diffuse[i]; };
        std::vector<int> uses;
        for (size_t i = 0; i < n; ++i) {
            if (!consumer[i]) continue;
            bool u = parents[i].empty() && own_kind(i);  // reads the constant itself
            for (int q : parents[i])
                if (is_use(q, int(i))) u = true;
            if (u) uses.push_back(int(i));
        }
        if (uses.empty()) {
            ch->why = specular ? "the ambient never meets a specular term" : "the ambient never meets a diffuse term";
            return false;
        }
        // children: the ambient nodes and consumers that read each node
        std::vector<std::vector<int>> children(n);
        for (size_t i = 0; i < n; ++i)
            for (int q : parents[i]) children[size_t(q)].push_back(int(i));
        // Can the ambient colours reach a use without passing `skip`?
        auto reaches_use_without = [&](int skip) {
            std::vector<char> seen(n, 0);
            std::vector<int> stack;
            for (size_t i = 0; i < n; ++i) {
                if (int(i) == skip || !reads_const[i]) continue;
                if (consumer[i] && own_kind(i)) return true;  // reads the constant itself
                if (node[i]) stack.push_back(int(i));
            }
            while (!stack.empty()) {
                const int x = stack.back();
                stack.pop_back();
                if (seen[size_t(x)]) continue;
                seen[size_t(x)] = 1;
                for (int y : children[size_t(x)]) {
                    if (y == skip) continue;
                    if (is_use(x, y)) return true;
                    if (node[size_t(y)]) stack.push_back(y);
                }
            }
            return false;
        };
        // Does `from` reach a consumer that must keep the game's ambient (one that is not a use of it)?
        auto feeds_other = [&](int from) {
            std::vector<char> seen(n, 0);
            std::vector<int> stack{from};
            while (!stack.empty()) {
                const int x = stack.back();
                stack.pop_back();
                if (seen[size_t(x)]) continue;
                seen[size_t(x)] = 1;
                for (int y : children[size_t(x)]) {
                    if (consumer[size_t(y)] && !is_use(x, y)) return true;
                    if (node[size_t(y)]) stack.push_back(y);
                }
            }
            return false;
        };
        for (int i = int(n) - 1; i >= 0 && ch->found < 0; --i) {
            if (!node[size_t(i)] || (node_taint[size_t(i)] & kSelfIllum)) continue;
            // the specular ambient is replaced before any shading weight comes in: the weights (fresnel times a texture's
            // alpha, say) must scale the reflection as they scaled the game's sky
            if (specular && (node_taint[size_t(i)] & kWeighted)) continue;
            const Operand& dst = p.code[size_t(i)].operands[0];
            if (dst.type != dxbc::kTemp || dst.components != 4) continue;
            uint32_t count = 0;
            for (uint32_t comp = 0; comp < 4; ++comp) count += (dst.mask >> comp) & 1;
            if (count != 3) continue;
            if (reaches_use_without(i) || feeds_other(i)) continue;
            ch->found = i;
            uint32_t comps[4], k = 0;
            for (uint32_t comp = 0; comp < 4; ++comp)
                if (dst.mask & (1u << comp)) comps[k++] = comp;
            for (uint32_t j = 0; j < 3; ++j) ch->swz[comps[j]] = j;
            ch->mask = dst.mask;
            ch->reg = dst.index[0];
        }
        if (ch->found >= 0) return true;
        // The operand form (the diffuse ambient only): every use reads the ambient colour itself (no register holds it,
        // nor the back ambient), which then reads the irradiance instead.
        if (specular) {
            ch->why = "no single specular ambient value";
            return false;
        }
        for (int i : uses) {
            bool back = false;
            for (const Operand& o : p.code[size_t(i)].operands)
                if (o.type == dxbc::kConstantBuffer && o.index[0] == 0 && o.index[1] == 3 && !o.relative) back = true;
            if (!reads_const[size_t(i)] || back || !parents[size_t(i)].empty()) {
                ch->direct.clear();
                ch->why = "no single diffuse ambient value";
                return false;
            }
            ch->direct.push_back(i);
        }
        return true;
    };
    Choice choice;
    if (!analyze(true, &choice)) {
        if (!choice.any_ambient) return r.result = Result::NoAmbient;
        Choice loose;
        // (for the reflections, the loose pass finds the specular ambient that is only scaled by such weights and added
        // on, as the stitch pieces' is, and takes the value before the weights: they then scale the reflection)
        if (!analyze(false, &loose)) return fail(loose.why.empty() ? choice.why : loose.why);
        choice = loose;
        r.loose = true;
    }
    const int found = choice.found;
    const uint32_t amb_mask = choice.mask, amb_reg = choice.reg;
    const uint32_t* amb_swz = choice.swz;
    const std::vector<int>& direct = choice.direct;
    if (found >= 0) {
        r.ambient = dxbc::describe(p, p.code[size_t(found)]);
        r.ambient_index = uint32_t(found);
    } else {
        r.ambient = dxbc::describe(p, p.code[size_t(direct.front())]) + " (reads the constant)";
        r.ambient_index = uint32_t(direct.front());
    }

    // The rewrite: t14 declared next to t15, one more temp (rN) and the irradiance loaded into it,
    //   ftoi rN.xy, v<pos>.xyxx
    //   mov  rN.zw, l(0, 0, 0, 0)
    //   ld   rN.xyzw, rN.xyzw, t14.xyzw
    // register form: after the ambient's instruction, then
    //   movc r<amb>.<its 3 components>, rN.wwww, rN.xyz (in order), r<amb>
    // operand form: before the first diffuse term that reads the constant, then
    //   movc rN.xyzw, rN.wwww, rN.xyzw, cb0[2].xyzw
    // and that term's cb0[2] operands read rN. Where the texture's alpha is 0 (nothing traced: the core not answering,
    // the sky's and the landmarks' pixels) the shader keeps the game's own value, as the original computes it.
    const uint32_t tn = temps;
    std::vector<uint32_t> dcl_t14;
    {
        const Instruction& d15 = p.code[size_t(t15_at)];
        dcl_t14.assign(p.tokens.begin() + d15.offset, p.tokens.begin() + d15.offset + d15.length);
        const Operand& o = d15.operands[0];
        dcl_t14[o.first + 1] = slot;  // the operand's index token
    }
    const uint32_t imm4 = imm4_token >= 0 ? uint32_t(imm4_token) : 0x00004002u;
    const std::vector<uint32_t> load = {
        op_token(27 /* ftoi */, 5), temp_dst(0x3), tn, input_src(swz(0, 1, 0, 0)), position,
        op_token(dxbc::kMov, 8), temp_dst(0xC), tn, imm4, 0, 0, 0, 0,
        op_token(dxbc::kLd, 7), temp_dst(0xF), tn, temp_src(swz(0, 1, 2, 3)), tn, resource_src(swz(0, 1, 2, 3)), slot,
    };
    const std::vector<uint32_t> assign = {
        op_token(dxbc::kMovc, 9), temp_dst(amb_mask), amb_reg, temp_src(swz(3, 3, 3, 3)), tn,
        temp_src(swz(amb_swz[0], amb_swz[1], amb_swz[2], amb_swz[3])), tn, temp_src(swz(0, 1, 2, 3)), amb_reg,
    };
    const std::vector<uint32_t> keep = {
        op_token(dxbc::kMovc, 10), temp_dst(0xF), tn, temp_src(swz(3, 3, 3, 3)), tn, temp_src(swz(0, 1, 2, 3)), tn,
        cb_src(swz(0, 1, 2, 3)), 0, 2,
    };
    // an instruction with its cb0[2] operands reading rN (same components, same modifiers)
    auto reread = [&](const Instruction& in) {
        const uint32_t* tk = p.tokens.data() + in.offset;
        std::vector<uint32_t> o2(tk, tk + (in.operands.empty() ? in.length : in.operands[0].first));
        for (const Operand& o : in.operands) {
            if (o.type == dxbc::kConstantBuffer && o.index[0] == 0 && o.index[1] == 2 && !o.relative) {
                o2.push_back((tk[o.first] & 0x80000FFFu) | (1u << 20));  // temp, 1D immediate index
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
    const std::set<int> rereads(direct.begin(), direct.end());
    const int first_reread = found < 0 && !direct.empty() ? *rereads.begin() : -1;
    std::vector<uint32_t> t;
    t.reserve(p.tokens.size() + dcl_t14.size() + load.size() + assign.size());
    const uint32_t length = p.tokens[1];
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instruction& in = p.code[i];
        if (int(i) == first_reread) {
            t.insert(t.end(), load.begin(), load.end());
            t.insert(t.end(), keep.begin(), keep.end());
        }
        if (found < 0 && rereads.count(int(i))) {
            const std::vector<uint32_t> o2 = reread(in);
            t.insert(t.end(), o2.begin(), o2.end());
        } else {
            t.insert(t.end(), p.tokens.begin() + in.offset, p.tokens.begin() + in.offset + in.length);
        }
        if (int(i) == temps_at) t[t.size() - 1] = tn + 1;
        if (int(i) == t15_at) t.insert(t.end(), dcl_t14.begin(), dcl_t14.end());
        if (int(i) == found) {
            t.insert(t.end(), load.begin(), load.end());
            t.insert(t.end(), assign.begin(), assign.end());
        }
    }
    std::vector<uint32_t> program;
    program.push_back(p.tokens[0]);
    program.push_back(uint32_t(2 + t.size()));
    program.insert(program.end(), t.begin(), t.end());
    // anything after the declared length (none in practice) is kept as it was
    for (size_t i = length; i < p.tokens.size(); ++i) program.push_back(p.tokens[i]);
    shdr->data.assign(reinterpret_cast<const uint8_t*>(program.data()),
                      reinterpret_cast<const uint8_t*>(program.data()) + program.size() * 4);
    // check: the new program decodes, with the instructions added
    dxbc::Program check;
    if (!dxbc::decode(*shdr, &check, &error)) return fail("the rewritten program does not decode: " + error);
    if (check.code.size() != p.code.size() + 5) return fail("the rewritten program has the wrong instruction count");
    if (out) *out = dxbc::serialize(c);
    r.result = Result::Patched;
    return r.result;
}

}  // namespace

Result patch(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report) {
    return patch_one(bytes, size, out, report, false, kIrradianceSlot);
}

Result patch_reflection(const void* bytes, size_t size, std::vector<uint8_t>* out, Report* report) {
    return patch_one(bytes, size, out, report, true, kReflectionSlot);
}

}  // namespace ambient
