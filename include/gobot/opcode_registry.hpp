#pragma once

#include "gobot/common.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace gobot {

/// What a decoded packet *means* to the bot.
///
/// Opcode numbers are per-game and unknown until you observe them, but the set
/// of things a side-scroller bot needs to know is small and stable. You map
/// numbers to these in configs/protocol.ini; everything above this layer is
/// written against the semantics, never against raw opcodes.
enum class Semantic {
    Unknown,
    SelfState,          ///< our position / hp / mp, whichever fields are present
    SelfHpMp,
    SelfMove,
    SelfDead,
    SelfRevive,
    SelfExp,
    MonsterSpawn,
    MonsterUpdate,      ///< position and/or hp of an existing monster
    MonsterRemove,
    MonsterSelected,
    PlayerSpawn,
    PlayerMove,
    PlayerRemove,
    ItemDrop,
    ItemRemove,
    MapWarp,
    MapData,
    PartyUpdate,
    PartyInvite,
    PartyLeave,
    PartyHp,
    Chat,
    SkillConfirm,       ///< server acknowledged our skill use
    AttackConfirm,
    BuffApplied,
    Disconnect,
};

Semantic    semantic_from_string(const std::string& s);
const char* to_string(Semantic s);

/// Field primitive types available in a packet layout.
enum class FieldType {
    I8, U8, I16, U16, I32, U32, I64, U64,
    F32, F64,
    Str8, Str16, Str32,     ///< length-prefixed
    StrZ,                   ///< NUL-terminated
    StrFixed,               ///< fixed width, `count` bytes
    Bytes,                  ///< raw run of `count` bytes
    Skip,                   ///< advance `count` bytes, emit nothing
};

struct FieldSpec {
    std::string name;
    FieldType   type = FieldType::U8;
    std::size_t count = 0;              ///< for StrFixed / Bytes / Skip
    bool        little_endian = true;
    bool        has_fixed_offset = false;
    std::size_t fixed_offset = 0;       ///< seek here before reading
    double      scale = 1.0;            ///< numeric fields are multiplied by this
};

/// One packet layout: opcode -> named fields -> meaning.
struct PacketSpec {
    std::uint16_t          opcode = 0;
    std::string            name;
    Semantic               semantic = Semantic::Unknown;
    std::vector<FieldSpec> fields;
    bool                   from_server = true;

    /// Repeat the field list this many times, or read the count from a field
    /// named by `repeat_count_field` (party/monster list packets do this).
    std::size_t repeat = 1;
    std::string repeat_count_field;
};

using FieldValue = std::variant<std::monostate, std::int64_t, double, std::string, Bytes>;

/// A decoded packet: named values, plus the repeat groups if any.
class DecodedPacket {
public:
    std::uint16_t opcode = 0;
    Semantic      semantic = Semantic::Unknown;
    std::string   name;

    /// Top-level scalar fields.
    std::unordered_map<std::string, FieldValue> fields;

    /// One entry per repeat iteration when `repeat` > 1.
    std::vector<std::unordered_map<std::string, FieldValue>> groups;

    bool has(const std::string& k) const { return fields.count(k) != 0; }

    std::int64_t  get_int(const std::string& k, std::int64_t def = 0) const;
    double        get_float(const std::string& k, double def = 0.0) const;
    std::string   get_string(const std::string& k, const std::string& def = {}) const;

    static std::int64_t group_int(const std::unordered_map<std::string, FieldValue>& g,
                                  const std::string& k, std::int64_t def = 0);
    static double group_float(const std::unordered_map<std::string, FieldValue>& g,
                              const std::string& k, double def = 0.0);
    static std::string group_string(const std::unordered_map<std::string, FieldValue>& g,
                                    const std::string& k, const std::string& def = {});

    std::string describe() const;
};

/// Opcode -> layout table, loaded from configs/protocol.ini.
///
/// Hand-editable on purpose: discovering the protocol is an iterative loop of
/// capture, guess a layout, watch the decode, correct it. Making that loop
/// require a recompile would be the wrong trade.
class OpcodeRegistry {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    void add(const PacketSpec& spec);
    void clear();

    const PacketSpec* find(std::uint16_t opcode) const;
    bool              known(std::uint16_t opcode) const { return find(opcode) != nullptr; }

    /// Decode using the registered layout. Returns nullopt for unmapped
    /// opcodes, or when the payload is too short for the layout (which is the
    /// usual sign that a hand-written layout is wrong).
    std::optional<DecodedPacket> decode(const GamePacket& pkt) const;

    /// Opcodes with a semantic attached — the natural capture filter.
    std::vector<std::uint16_t> mapped_opcodes() const;

    /// Every opcode carrying this semantic.
    std::vector<std::uint16_t> opcodes_for(Semantic s) const;

    std::size_t size() const { return specs_.size(); }

    /// Parse one `fields=` line, e.g. "id:u32,x:f32,y:f32,hp:i32@12".
    static std::vector<FieldSpec> parse_field_list(const std::string& text,
                                                   std::string* error = nullptr);
    static std::string field_list_to_string(const std::vector<FieldSpec>& fields);

private:
    std::map<std::uint16_t, PacketSpec> specs_;
};

} // namespace gobot
