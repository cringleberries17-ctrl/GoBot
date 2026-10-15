#include "gobot/opcode_registry.hpp"
#include "gobot/byte_reader.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace gobot {
namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, sep)) {
        cur = trim(cur);
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

struct TypeEntry { const char* name; FieldType type; std::size_t width; };

const TypeEntry kTypes[] = {
    { "i8",   FieldType::I8,   1 }, { "u8",   FieldType::U8,   1 },
    { "i16",  FieldType::I16,  2 }, { "u16",  FieldType::U16,  2 },
    { "i32",  FieldType::I32,  4 }, { "u32",  FieldType::U32,  4 },
    { "i64",  FieldType::I64,  8 }, { "u64",  FieldType::U64,  8 },
    { "f32",  FieldType::F32,  4 }, { "f64",  FieldType::F64,  8 },
    { "str8", FieldType::Str8, 0 }, { "str16", FieldType::Str16, 0 },
    { "str32", FieldType::Str32, 0 },
    { "strz", FieldType::StrZ, 0 },
};

bool lookup_type(const std::string& name, FieldType& out) {
    for (const auto& t : kTypes) {
        if (name == t.name) { out = t.type; return true; }
    }
    return false;
}

const char* type_name(FieldType t) {
    for (const auto& e : kTypes) {
        if (e.type == t) return e.name;
    }
    switch (t) {
        case FieldType::StrFixed: return "strf";
        case FieldType::Bytes:    return "bytes";
        case FieldType::Skip:     return "skip";
        default:                  return "u8";
    }
}

} // namespace

// ------------------------------------------------------------------ Semantic

Semantic semantic_from_string(const std::string& raw) {
    const std::string s = lower(trim(raw));
    if (s == "self_state")      return Semantic::SelfState;
    if (s == "self_hpmp")       return Semantic::SelfHpMp;
    if (s == "self_move")       return Semantic::SelfMove;
    if (s == "self_dead")       return Semantic::SelfDead;
    if (s == "self_revive")     return Semantic::SelfRevive;
    if (s == "self_exp")        return Semantic::SelfExp;
    if (s == "monster_spawn")   return Semantic::MonsterSpawn;
    if (s == "monster_update")  return Semantic::MonsterUpdate;
    if (s == "monster_remove")  return Semantic::MonsterRemove;
    if (s == "monster_selected")return Semantic::MonsterSelected;
    if (s == "player_spawn")    return Semantic::PlayerSpawn;
    if (s == "player_move")     return Semantic::PlayerMove;
    if (s == "player_remove")   return Semantic::PlayerRemove;
    if (s == "item_drop")       return Semantic::ItemDrop;
    if (s == "item_remove")     return Semantic::ItemRemove;
    if (s == "map_warp")        return Semantic::MapWarp;
    if (s == "map_data")        return Semantic::MapData;
    if (s == "party_update")    return Semantic::PartyUpdate;
    if (s == "party_invite")    return Semantic::PartyInvite;
    if (s == "party_leave")     return Semantic::PartyLeave;
    if (s == "party_hp")        return Semantic::PartyHp;
    if (s == "chat")            return Semantic::Chat;
    if (s == "skill_confirm")   return Semantic::SkillConfirm;
    if (s == "attack_confirm")  return Semantic::AttackConfirm;
    if (s == "buff_applied")    return Semantic::BuffApplied;
    if (s == "disconnect")      return Semantic::Disconnect;
    return Semantic::Unknown;
}

const char* to_string(Semantic s) {
    switch (s) {
        case Semantic::SelfState:       return "self_state";
        case Semantic::SelfHpMp:        return "self_hpmp";
        case Semantic::SelfMove:        return "self_move";
        case Semantic::SelfDead:        return "self_dead";
        case Semantic::SelfRevive:      return "self_revive";
        case Semantic::SelfExp:         return "self_exp";
        case Semantic::MonsterSpawn:    return "monster_spawn";
        case Semantic::MonsterUpdate:   return "monster_update";
        case Semantic::MonsterRemove:   return "monster_remove";
        case Semantic::MonsterSelected: return "monster_selected";
        case Semantic::PlayerSpawn:     return "player_spawn";
        case Semantic::PlayerMove:      return "player_move";
        case Semantic::PlayerRemove:    return "player_remove";
        case Semantic::ItemDrop:        return "item_drop";
        case Semantic::ItemRemove:      return "item_remove";
        case Semantic::MapWarp:         return "map_warp";
        case Semantic::MapData:         return "map_data";
        case Semantic::PartyUpdate:     return "party_update";
        case Semantic::PartyInvite:     return "party_invite";
        case Semantic::PartyLeave:      return "party_leave";
        case Semantic::PartyHp:         return "party_hp";
        case Semantic::Chat:            return "chat";
        case Semantic::SkillConfirm:    return "skill_confirm";
        case Semantic::AttackConfirm:   return "attack_confirm";
        case Semantic::BuffApplied:     return "buff_applied";
        case Semantic::Disconnect:      return "disconnect";
        case Semantic::Unknown:         break;
    }
    return "unknown";
}

// ------------------------------------------------------------ DecodedPacket

std::int64_t DecodedPacket::get_int(const std::string& k, std::int64_t def) const {
    auto it = fields.find(k);
    if (it == fields.end()) return def;
    if (auto* v = std::get_if<std::int64_t>(&it->second)) return *v;
    if (auto* v = std::get_if<double>(&it->second))       return static_cast<std::int64_t>(*v);
    return def;
}

double DecodedPacket::get_float(const std::string& k, double def) const {
    auto it = fields.find(k);
    if (it == fields.end()) return def;
    if (auto* v = std::get_if<double>(&it->second))       return *v;
    if (auto* v = std::get_if<std::int64_t>(&it->second)) return static_cast<double>(*v);
    return def;
}

std::string DecodedPacket::get_string(const std::string& k, const std::string& def) const {
    auto it = fields.find(k);
    if (it == fields.end()) return def;
    if (auto* v = std::get_if<std::string>(&it->second)) return *v;
    return def;
}

std::int64_t DecodedPacket::group_int(const std::unordered_map<std::string, FieldValue>& g,
                                      const std::string& k, std::int64_t def) {
    auto it = g.find(k);
    if (it == g.end()) return def;
    if (auto* v = std::get_if<std::int64_t>(&it->second)) return *v;
    if (auto* v = std::get_if<double>(&it->second))       return static_cast<std::int64_t>(*v);
    return def;
}

double DecodedPacket::group_float(const std::unordered_map<std::string, FieldValue>& g,
                                  const std::string& k, double def) {
    auto it = g.find(k);
    if (it == g.end()) return def;
    if (auto* v = std::get_if<double>(&it->second))       return *v;
    if (auto* v = std::get_if<std::int64_t>(&it->second)) return static_cast<double>(*v);
    return def;
}

std::string DecodedPacket::group_string(const std::unordered_map<std::string, FieldValue>& g,
                                        const std::string& k, const std::string& def) {
    auto it = g.find(k);
    if (it == g.end()) return def;
    if (auto* v = std::get_if<std::string>(&it->second)) return *v;
    return def;
}

std::string DecodedPacket::describe() const {
    std::ostringstream os;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%04X", opcode);
    os << buf << " " << (name.empty() ? "?" : name)
       << " [" << to_string(semantic) << "]";

    auto dump = [&os](const std::unordered_map<std::string, FieldValue>& m) {
        for (const auto& [k, v] : m) {
            os << " " << k << "=";
            if (auto* i = std::get_if<std::int64_t>(&v))      os << *i;
            else if (auto* d = std::get_if<double>(&v))       os << *d;
            else if (auto* s = std::get_if<std::string>(&v))  os << "\"" << *s << "\"";
            else if (auto* b = std::get_if<Bytes>(&v))        os << "<" << b->size() << "B>";
            else                                             os << "-";
        }
    };

    dump(fields);
    for (std::size_t i = 0; i < groups.size(); ++i) {
        os << " |" << i << ":";
        dump(groups[i]);
    }
    return os.str();
}

// ------------------------------------------------------------ field parsing

std::vector<FieldSpec> OpcodeRegistry::parse_field_list(const std::string& text,
                                                        std::string* error) {
    std::vector<FieldSpec> out;

    for (const std::string& raw : split(text, ',')) {
        FieldSpec f;
        std::string item = raw;

        // Optional trailing "*scale", e.g. "x:i16*0.125" for fixed-point.
        if (const auto star = item.find('*'); star != std::string::npos) {
            try {
                f.scale = std::stod(item.substr(star + 1));
            } catch (...) {
                if (error) *error = "bad scale in '" + raw + "'";
                return {};
            }
            item = item.substr(0, star);
        }

        // Optional "@offset" pins the field to an absolute payload offset.
        if (const auto at = item.find('@'); at != std::string::npos) {
            try {
                f.fixed_offset = static_cast<std::size_t>(std::stoul(item.substr(at + 1), nullptr, 0));
                f.has_fixed_offset = true;
            } catch (...) {
                if (error) *error = "bad offset in '" + raw + "'";
                return {};
            }
            item = item.substr(0, at);
        }

        const auto colon = item.find(':');
        if (colon == std::string::npos) {
            if (error) *error = "missing ':' in '" + raw + "'";
            return {};
        }

        f.name = trim(item.substr(0, colon));
        std::string type = lower(trim(item.substr(colon + 1)));

        // Big-endian suffix: "u16be".
        if (type.size() > 2 && type.compare(type.size() - 2, 2, "be") == 0) {
            f.little_endian = false;
            type.resize(type.size() - 2);
        } else if (type.size() > 2 && type.compare(type.size() - 2, 2, "le") == 0) {
            type.resize(type.size() - 2);
        }

        // Sized types: "strf8", "bytes16", "skip4".
        auto sized = [&](const char* prefix, FieldType ft) -> bool {
            const std::size_t plen = std::strlen(prefix);
            if (type.compare(0, plen, prefix) != 0) return false;
            f.type = ft;
            f.count = type.size() > plen
                    ? static_cast<std::size_t>(std::strtoul(type.c_str() + plen, nullptr, 10))
                    : 0;
            return true;
        };

        if (sized("strf", FieldType::StrFixed)) {
            // ok
        } else if (sized("bytes", FieldType::Bytes)) {
            // ok
        } else if (sized("skip", FieldType::Skip)) {
            // ok
        } else if (!lookup_type(type, f.type)) {
            if (error) *error = "unknown type '" + type + "' in '" + raw + "'";
            return {};
        }

        if ((f.type == FieldType::StrFixed || f.type == FieldType::Bytes ||
             f.type == FieldType::Skip) && f.count == 0) {
            if (error) *error = "'" + raw + "' needs a byte count, e.g. strf12";
            return {};
        }

        if (f.name.empty() && f.type != FieldType::Skip) {
            if (error) *error = "empty field name in '" + raw + "'";
            return {};
        }

        out.push_back(std::move(f));
    }
    return out;
}

std::string OpcodeRegistry::field_list_to_string(const std::vector<FieldSpec>& fields) {
    std::ostringstream os;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) os << ",";
        const auto& f = fields[i];
        os << f.name << ":" << type_name(f.type);
        if (f.type == FieldType::StrFixed || f.type == FieldType::Bytes || f.type == FieldType::Skip)
            os << f.count;
        if (!f.little_endian) os << "be";
        if (f.has_fixed_offset) os << "@" << f.fixed_offset;
        if (f.scale != 1.0) os << "*" << f.scale;
    }
    return os.str();
}

// ------------------------------------------------------------- registry I/O

void OpcodeRegistry::add(const PacketSpec& spec) { specs_[spec.opcode] = spec; }
void OpcodeRegistry::clear() { specs_.clear(); }

const PacketSpec* OpcodeRegistry::find(std::uint16_t opcode) const {
    auto it = specs_.find(opcode);
    return it == specs_.end() ? nullptr : &it->second;
}

bool OpcodeRegistry::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) {
        GB_WARN("OpcodeRegistry: cannot read %s", path.c_str());
        return false;
    }

    clear();
    std::size_t errors = 0;

    for (const std::string& section : ini.sections()) {
        // Only [Packet.<opcode>] sections describe layouts; [Framing] and
        // friends live in the same file and are read elsewhere.
        static constexpr const char* kPrefix = "Packet.";
        if (section.compare(0, std::strlen(kPrefix), kPrefix) != 0) continue;

        const std::string op_text = section.substr(std::strlen(kPrefix));
        char* end = nullptr;
        const unsigned long op = std::strtoul(op_text.c_str(), &end, 0);
        if (end == op_text.c_str() || op > 0xFFFF) {
            GB_WARN("OpcodeRegistry: bad opcode in [%s]", section.c_str());
            ++errors;
            continue;
        }

        PacketSpec spec;
        spec.opcode      = static_cast<std::uint16_t>(op);
        spec.name        = ini.get(section, "name", op_text);
        spec.semantic    = semantic_from_string(ini.get(section, "semantic", "unknown"));
        spec.from_server = ini.get_bool(section, "from_server", true);
        spec.repeat      = static_cast<std::size_t>(ini.get_int(section, "repeat", 1));
        spec.repeat_count_field = ini.get(section, "repeat_count_field", "");

        const std::string fields = ini.get(section, "fields", "");
        if (!fields.empty()) {
            std::string err;
            spec.fields = parse_field_list(fields, &err);
            if (spec.fields.empty() && !err.empty()) {
                GB_ERROR("OpcodeRegistry [%s]: %s", section.c_str(), err.c_str());
                ++errors;
                continue;
            }
        }

        specs_[spec.opcode] = std::move(spec);
    }

    GB_INFO("OpcodeRegistry: %zu packet layouts from %s%s",
            specs_.size(), path.c_str(),
            errors ? (" (" + std::to_string(errors) + " rejected)").c_str() : "");
    return true;
}

bool OpcodeRegistry::save(const std::string& path) const {
    IniFile ini;
    for (const auto& [op, spec] : specs_) {
        char sec[32];
        std::snprintf(sec, sizeof(sec), "Packet.0x%04X", op);
        ini.set     (sec, "name", spec.name);
        ini.set     (sec, "semantic", to_string(spec.semantic));
        ini.set_bool(sec, "from_server", spec.from_server);
        ini.set     (sec, "fields", field_list_to_string(spec.fields));
        if (spec.repeat != 1) ini.set_int(sec, "repeat", static_cast<int>(spec.repeat));
        if (!spec.repeat_count_field.empty())
            ini.set(sec, "repeat_count_field", spec.repeat_count_field);
    }
    return ini.save(path, "GoBot protocol map");
}

std::vector<std::uint16_t> OpcodeRegistry::mapped_opcodes() const {
    std::vector<std::uint16_t> out;
    for (const auto& [op, spec] : specs_) {
        if (spec.semantic != Semantic::Unknown) out.push_back(op);
    }
    return out;
}

std::vector<std::uint16_t> OpcodeRegistry::opcodes_for(Semantic s) const {
    std::vector<std::uint16_t> out;
    for (const auto& [op, spec] : specs_) {
        if (spec.semantic == s) out.push_back(op);
    }
    return out;
}

// ---------------------------------------------------------------- decoding

namespace {

/// Read one field. Returns false when the payload ran out, which is the
/// signal that a hand-written layout doesn't match reality.
bool read_field(ByteReader& r, const FieldSpec& f, FieldValue& out) {
    if (f.has_fixed_offset) r.seek(f.fixed_offset);

    const bool le = f.little_endian;

    switch (f.type) {
        case FieldType::I8:  out = static_cast<std::int64_t>(r.i8());  break;
        case FieldType::U8:  out = static_cast<std::int64_t>(r.u8());  break;
        case FieldType::I16: out = static_cast<std::int64_t>(r.i16(le)); break;
        case FieldType::U16: out = static_cast<std::int64_t>(r.u16(le)); break;
        case FieldType::I32: out = static_cast<std::int64_t>(r.i32(le)); break;
        case FieldType::U32: out = static_cast<std::int64_t>(r.u32(le)); break;
        case FieldType::I64: out = r.i64(le); break;
        case FieldType::U64: out = static_cast<std::int64_t>(r.u64(le)); break;
        case FieldType::F32: out = static_cast<double>(r.f32(le)); break;
        case FieldType::F64: out = r.f64(le); break;
        case FieldType::Str8:  out = r.prefixed_string(1, le); break;
        case FieldType::Str16: out = r.prefixed_string(2, le); break;
        case FieldType::Str32: out = r.prefixed_string(4, le); break;
        case FieldType::StrZ:  out = r.cstring(); break;
        case FieldType::StrFixed: out = r.fixed_string(f.count); break;
        case FieldType::Bytes:    out = r.bytes(f.count); break;
        case FieldType::Skip:     r.skip(f.count); out = std::monostate{}; break;
    }

    if (!r.ok()) return false;

    // Fixed-point coordinates are common; apply the declared scale.
    if (f.scale != 1.0) {
        if (auto* i = std::get_if<std::int64_t>(&out)) {
            out = static_cast<double>(*i) * f.scale;
        } else if (auto* d = std::get_if<double>(&out)) {
            out = *d * f.scale;
        }
    }
    return true;
}

} // namespace

std::optional<DecodedPacket> OpcodeRegistry::decode(const GamePacket& pkt) const {
    const PacketSpec* spec = find(pkt.opcode);
    if (!spec) return std::nullopt;

    DecodedPacket out;
    out.opcode   = pkt.opcode;
    out.semantic = spec->semantic;
    out.name     = spec->name;

    ByteReader r(pkt.payload.data(), pkt.payload.size());

    // A repeat group driven by a count field needs that field read first, so
    // the leading fields are decoded into the top level either way.
    std::size_t repeat = spec->repeat;

    if (!spec->repeat_count_field.empty()) {
        // Layout: scalar prologue (including the count) then N groups. The
        // prologue is whatever precedes the count field in the list.
        std::size_t count_index = spec->fields.size();
        for (std::size_t i = 0; i < spec->fields.size(); ++i) {
            if (spec->fields[i].name == spec->repeat_count_field) { count_index = i; break; }
        }
        if (count_index == spec->fields.size()) {
            GB_WARN("Packet 0x%04X: repeat_count_field '%s' not in field list",
                    pkt.opcode, spec->repeat_count_field.c_str());
            return std::nullopt;
        }

        for (std::size_t i = 0; i <= count_index; ++i) {
            FieldValue v;
            if (!read_field(r, spec->fields[i], v)) return std::nullopt;
            if (spec->fields[i].type != FieldType::Skip)
                out.fields[spec->fields[i].name] = std::move(v);
        }

        const std::int64_t n = out.get_int(spec->repeat_count_field, 0);
        if (n < 0 || n > 4096) {
            GB_WARN("Packet 0x%04X: implausible repeat count %lld", pkt.opcode,
                    static_cast<long long>(n));
            return std::nullopt;
        }

        for (std::int64_t g = 0; g < n; ++g) {
            std::unordered_map<std::string, FieldValue> group;
            for (std::size_t i = count_index + 1; i < spec->fields.size(); ++i) {
                FieldValue v;
                if (!read_field(r, spec->fields[i], v)) {
                    // Partial trailing group: keep what we parsed rather than
                    // dropping the whole packet.
                    if (!group.empty()) out.groups.push_back(std::move(group));
                    return out;
                }
                if (spec->fields[i].type != FieldType::Skip)
                    group[spec->fields[i].name] = std::move(v);
            }
            out.groups.push_back(std::move(group));
        }
        return out;
    }

    if (repeat <= 1) {
        for (const auto& f : spec->fields) {
            FieldValue v;
            if (!read_field(r, f, v)) return std::nullopt;
            if (f.type != FieldType::Skip) out.fields[f.name] = std::move(v);
        }
        return out;
    }

    for (std::size_t g = 0; g < repeat; ++g) {
        std::unordered_map<std::string, FieldValue> group;
        for (const auto& f : spec->fields) {
            FieldValue v;
            if (!read_field(r, f, v)) {
                if (!group.empty()) out.groups.push_back(std::move(group));
                return out;
            }
            if (f.type != FieldType::Skip) group[f.name] = std::move(v);
        }
        out.groups.push_back(std::move(group));
    }
    return out;
}

} // namespace gobot
