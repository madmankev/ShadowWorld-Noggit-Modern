#include "ModernMapRegistrations.h"

#include <noggit/DBC.h>
#include <noggit/DBCFile.h>
#include <noggit/Log.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace Noggit::Project
{
  namespace
  {
    // ---------------------------------------------------------------------
    // Minimal JSON codec for the fixed registration schema. Dependency-free
    // on purpose: this file must stay unit-testable outside the application.
    // ---------------------------------------------------------------------

    struct JsonValue;
    using JsonArray = std::vector<JsonValue>;
    using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

    struct JsonValue
    {
      enum class Type { Null, Bool, Number, String, Array, Object };

      Type type = Type::Null;
      bool boolean = false;
      std::string text;   // String: value. Number: raw token.
      JsonArray array;
      std::shared_ptr<JsonObject> object;

      static JsonValue make_string(std::string value)
      {
        JsonValue result;
        result.type = Type::String;
        result.text = std::move(value);
        return result;
      }
      static JsonValue make_number(std::string raw_token)
      {
        JsonValue result;
        result.type = Type::Number;
        result.text = std::move(raw_token);
        return result;
      }
      static JsonValue make_object(JsonObject members)
      {
        JsonValue result;
        result.type = Type::Object;
        result.object = std::make_shared<JsonObject>(std::move(members));
        return result;
      }
      static JsonValue make_array(JsonArray elements)
      {
        JsonValue result;
        result.type = Type::Array;
        result.array = std::move(elements);
        return result;
      }
    };

    class JsonParseError : public std::runtime_error
    {
    public:
      using std::runtime_error::runtime_error;
    };

    class JsonParser
    {
    public:
      explicit JsonParser(std::string input) : _input(std::move(input)) {}

      JsonValue parse_document()
      {
        skip_whitespace();
        JsonValue value = parse_value();
        skip_whitespace();
        if (_pos != _input.size())
          throw JsonParseError("trailing characters at offset " + std::to_string(_pos));
        return value;
      }

    private:
      void skip_whitespace()
      {
        while (_pos < _input.size())
        {
          char const c = _input[_pos];
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ++_pos;
          else
            break;
        }
      }

      char peek()
      {
        if (_pos >= _input.size())
          throw JsonParseError("unexpected end of input");
        return _input[_pos];
      }

      void expect(char c)
      {
        if (peek() != c)
          throw JsonParseError(std::string("expected '") + c + "' at offset " + std::to_string(_pos));
        ++_pos;
      }

      JsonValue parse_value()
      {
        skip_whitespace();
        switch (peek())
        {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return JsonValue::make_string(parse_string());
        case 't':
        case 'f': return parse_bool();
        case 'n': return parse_null();
        default:  return parse_number();
        }
      }

      JsonValue parse_object()
      {
        expect('{');
        JsonObject members;
        skip_whitespace();
        if (peek() == '}')
        {
          ++_pos;
          return JsonValue::make_object(std::move(members));
        }
        while (true)
        {
          skip_whitespace();
          std::string key = parse_string();
          skip_whitespace();
          expect(':');
          members.emplace_back(std::move(key), parse_value());
          skip_whitespace();
          char const c = peek();
          ++_pos;
          if (c == '}')
            break;
          if (c != ',')
            throw JsonParseError("expected ',' or '}' at offset " + std::to_string(_pos - 1));
        }
        return JsonValue::make_object(std::move(members));
      }

      JsonValue parse_array()
      {
        expect('[');
        JsonArray elements;
        skip_whitespace();
        if (peek() == ']')
        {
          ++_pos;
          return JsonValue::make_array(std::move(elements));
        }
        while (true)
        {
          elements.push_back(parse_value());
          skip_whitespace();
          char const c = peek();
          ++_pos;
          if (c == ']')
            break;
          if (c != ',')
            throw JsonParseError("expected ',' or ']' at offset " + std::to_string(_pos - 1));
        }
        return JsonValue::make_array(std::move(elements));
      }

      std::string parse_string()
      {
        expect('"');
        std::string result;
        while (true)
        {
          if (_pos >= _input.size())
            throw JsonParseError("unterminated string");
          char const c = _input[_pos++];
          if (c == '"')
            break;
          if (c != '\\')
          {
            result.push_back(c);
            continue;
          }
          if (_pos >= _input.size())
            throw JsonParseError("unterminated escape");
          char const escape = _input[_pos++];
          switch (escape)
          {
          case '"':  result.push_back('"');  break;
          case '\\': result.push_back('\\'); break;
          case '/':  result.push_back('/');  break;
          case 'b':  result.push_back('\b'); break;
          case 'f':  result.push_back('\f'); break;
          case 'n':  result.push_back('\n'); break;
          case 'r':  result.push_back('\r'); break;
          case 't':  result.push_back('\t'); break;
          case 'u':
          {
            if (_pos + 4 > _input.size())
              throw JsonParseError("truncated \\u escape");
            unsigned int code = 0;
            for (int i = 0; i < 4; ++i)
            {
              char const digit = _input[_pos++];
              code <<= 4;
              if (digit >= '0' && digit <= '9')      code |= static_cast<unsigned>(digit - '0');
              else if (digit >= 'a' && digit <= 'f') code |= static_cast<unsigned>(digit - 'a' + 10);
              else if (digit >= 'A' && digit <= 'F') code |= static_cast<unsigned>(digit - 'A' + 10);
              else throw JsonParseError("invalid \\u escape");
            }
            // Best-effort UTF-8 encoding; surrogate pairs are passed through as
            // their code units (registration values are UI text, not protocol).
            if (code < 0x80)
              result.push_back(static_cast<char>(code));
            else if (code < 0x800)
            {
              result.push_back(static_cast<char>(0xC0 | (code >> 6)));
              result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else
            {
              result.push_back(static_cast<char>(0xE0 | (code >> 12)));
              result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
              result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            break;
          }
          default:
            throw JsonParseError(std::string("invalid escape '\\") + escape + "'");
          }
        }
        return result;
      }

      JsonValue parse_bool()
      {
        if (_input.compare(_pos, 4, "true") == 0)
        {
          _pos += 4;
          JsonValue value;
          value.type = JsonValue::Type::Bool;
          value.boolean = true;
          return value;
        }
        if (_input.compare(_pos, 5, "false") == 0)
        {
          _pos += 5;
          JsonValue value;
          value.type = JsonValue::Type::Bool;
          value.boolean = false;
          return value;
        }
        throw JsonParseError("invalid literal at offset " + std::to_string(_pos));
      }

      JsonValue parse_null()
      {
        if (_input.compare(_pos, 4, "null") == 0)
        {
          _pos += 4;
          return JsonValue{};
        }
        throw JsonParseError("invalid literal at offset " + std::to_string(_pos));
      }

      JsonValue parse_number()
      {
        std::size_t const start = _pos;
        if (_pos < _input.size() && (_input[_pos] == '-' || _input[_pos] == '+'))
          ++_pos;
        bool digits = false;
        while (_pos < _input.size() && std::isdigit(static_cast<unsigned char>(_input[_pos])))
        {
          ++_pos;
          digits = true;
        }
        if (_pos < _input.size() && _input[_pos] == '.')
        {
          ++_pos;
          while (_pos < _input.size() && std::isdigit(static_cast<unsigned char>(_input[_pos])))
          {
            ++_pos;
            digits = true;
          }
        }
        if (digits && _pos < _input.size() && (_input[_pos] == 'e' || _input[_pos] == 'E'))
        {
          ++_pos;
          if (_pos < _input.size() && (_input[_pos] == '-' || _input[_pos] == '+'))
            ++_pos;
          while (_pos < _input.size() && std::isdigit(static_cast<unsigned char>(_input[_pos])))
            ++_pos;
        }
        if (!digits)
          throw JsonParseError("invalid value at offset " + std::to_string(start));
        return JsonValue::make_number(_input.substr(start, _pos - start));
      }

      std::string _input;
      std::size_t _pos = 0;
    };

    void write_json_string(std::ostream& out, std::string const& text)
    {
      out << '"';
      for (unsigned char c : text)
      {
        switch (c)
        {
        case '"':  out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b";  break;
        case '\f': out << "\\f";  break;
        case '\n': out << "\\n";  break;
        case '\r': out << "\\r";  break;
        case '\t': out << "\\t";  break;
        default:
          if (c < 0x20)
          {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out << buffer;
          }
          else
            out << static_cast<char>(c);
        }
      }
      out << '"';
    }

    void write_json_value(std::ostream& out, JsonValue const& value, int depth)
    {
      auto indent = [&](int level)
      {
        out << '\n';
        for (int i = 0; i < level * 2; ++i)
          out << ' ';
      };

      switch (value.type)
      {
      case JsonValue::Type::Null:   out << "null"; break;
      case JsonValue::Type::Bool:   out << (value.boolean ? "true" : "false"); break;
      case JsonValue::Type::Number: out << value.text; break;
      case JsonValue::Type::String: write_json_string(out, value.text); break;
      case JsonValue::Type::Array:
      {
        out << '[';
        bool first = true;
        for (auto const& element : value.array)
        {
          if (!first) out << ',';
          first = false;
          indent(depth + 1);
          write_json_value(out, element, depth + 1);
        }
        if (!first) indent(depth);
        out << ']';
        break;
      }
      case JsonValue::Type::Object:
      {
        out << '{';
        bool first = true;
        for (auto const& [key, member] : *value.object)
        {
          if (!first) out << ',';
          first = false;
          indent(depth + 1);
          write_json_string(out, key);
          out << ": ";
          write_json_value(out, member, depth + 1);
        }
        if (!first) indent(depth);
        out << '}';
        break;
      }
      }
    }

    // ---------------------------------------------------------------------
    // Schema helpers
    // ---------------------------------------------------------------------

    JsonValue const* find_member(JsonValue const& value, std::string const& key)
    {
      if (value.type != JsonValue::Type::Object || !value.object)
        return nullptr;
      for (auto const& [member_key, member] : *value.object)
        if (member_key == key)
          return &member;
      return nullptr;
    }

    // Accepts JSON strings and numbers (numbers keep their raw token) so both
    // hand-authored and wizard-written files work.
    std::string value_as_text(JsonValue const& value)
    {
      if (value.type == JsonValue::Type::String || value.type == JsonValue::Type::Number)
        return value.text;
      if (value.type == JsonValue::Type::Bool)
        return value.boolean ? "1" : "0";
      return {};
    }

    std::uint32_t parse_u32(std::string const& raw, std::uint32_t fallback)
    {
      if (raw.empty())
        return fallback;
      try
      {
        // Accept float-looking values ("1.0") by truncating, matching the
        // leniency of the bridge converters.
        return static_cast<std::uint32_t>(std::stoull(raw));
      }
      catch (std::exception const&)
      {
        try { return static_cast<std::uint32_t>(std::stod(raw)); }
        catch (std::exception const&) { return fallback; }
      }
    }

    int parse_i32(std::string const& raw, int fallback)
    {
      if (raw.empty())
        return fallback;
      try { return static_cast<int>(std::stoll(raw)); }
      catch (std::exception const&)
      {
        try { return static_cast<int>(std::stod(raw)); }
        catch (std::exception const&) { return fallback; }
      }
    }

    float parse_f32(std::string const& raw, float fallback)
    {
      if (raw.empty())
        return fallback;
      try { return std::stof(raw); }
      catch (std::exception const&) { return fallback; }
    }

    // Strict "the whole string is a number of this kind" checks. The sto*
    // family silently accepts trailing garbage ("1.5x" -> 1) and stoull even
    // wraps negatives, which would let malformed hand-authored registrations
    // through validation.
    bool parses_as_u32(std::string const& raw)
    {
      if (raw.empty() || raw[0] == '-')
        return false;
      try
      {
        std::size_t consumed = 0;
        std::stoull(raw, &consumed);
        return consumed == raw.size();
      }
      catch (std::exception const&) { return false; }
    }

    bool parses_as_i32(std::string const& raw)
    {
      if (raw.empty())
        return false;
      try
      {
        std::size_t consumed = 0;
        std::stoll(raw, &consumed);
        return consumed == raw.size();
      }
      catch (std::exception const&) { return false; }
    }

    bool parses_as_f32(std::string const& raw)
    {
      if (raw.empty())
        return false;
      try
      {
        std::size_t consumed = 0;
        std::stof(raw, &consumed);
        return consumed == raw.size();
      }
      catch (std::exception const&) { return false; }
    }

    ModernMapRegistration registration_from_json(JsonValue const& value)
    {
      ModernMapRegistration registration;
      if (auto const* id = find_member(value, "id"))
        registration.id = parse_i32(value_as_text(*id), -1);
      if (auto const* build = find_member(value, "target_build"))
        registration.target_build = value_as_text(*build);
      if (auto const* fields = find_member(value, "fields"))
      {
        if (fields->type == JsonValue::Type::Object && fields->object)
          for (auto const& [key, member] : *fields->object)
            registration.fields[key] = value_as_text(member);
      }
      if (auto const* arrays = find_member(value, "array_fields"))
      {
        if (arrays->type == JsonValue::Type::Object && arrays->object)
          for (auto const& [key, member] : *arrays->object)
          {
            std::vector<std::string> elements;
            if (member.type == JsonValue::Type::Array)
              for (auto const& element : member.array)
                elements.push_back(value_as_text(element));
            registration.array_fields[key] = std::move(elements);
          }
      }
      return registration;
    }

    JsonValue registration_to_json(ModernMapRegistration const& registration)
    {
      JsonObject members;
      members.emplace_back("id", JsonValue::make_number(std::to_string(registration.id)));
      members.emplace_back("target_build", JsonValue::make_string(registration.target_build));

      JsonObject fields;
      for (auto const& [key, value] : registration.fields)
        fields.emplace_back(key, JsonValue::make_string(value));
      members.emplace_back("fields", JsonValue::make_object(std::move(fields)));

      JsonObject arrays;
      for (auto const& [key, elements] : registration.array_fields)
      {
        JsonArray values;
        for (auto const& element : elements)
          values.push_back(JsonValue::make_string(element));
        arrays.emplace_back(key, JsonValue::make_array(std::move(values)));
      }
      members.emplace_back("array_fields", JsonValue::make_object(std::move(arrays)));

      return JsonValue::make_object(std::move(members));
    }
  }

  std::filesystem::path ModernMapRegistrations::storage_path(std::filesystem::path const& project_path)
  {
    return project_path / "extraData" / "modernMapRegistrations.json";
  }

  std::vector<ModernMapRegistration> ModernMapRegistrations::load(std::filesystem::path const& project_path)
  {
    std::vector<ModernMapRegistration> registrations;
    auto const path = storage_path(project_path);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
      return registrations;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
      LogError << "[MapRegistration] Could not open \"" << path.string() << "\"" << std::endl;
      return registrations;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();

    try
    {
      JsonValue document = JsonParser(buffer.str()).parse_document();
      if (auto const* version = find_member(document, "version"))
      {
        int const format = parse_i32(value_as_text(*version), storage_format_version);
        if (format > storage_format_version)
          LogError << "[MapRegistration] \"" << path.string() << "\" uses format version " << format
                   << ", this build understands up to " << storage_format_version
                   << "; registrations may be ignored." << std::endl;
      }
      if (auto const* maps = find_member(document, "maps"))
      {
        if (maps->type != JsonValue::Type::Array)
          throw JsonParseError("\"maps\" must be an array");
        for (auto const& element : maps->array)
        {
          ModernMapRegistration registration = registration_from_json(element);
          if (registration.id > 0)
            registrations.push_back(std::move(registration));
          else
            LogError << "[MapRegistration] Ignoring registration without a usable map id in \""
                     << path.string() << "\"" << std::endl;
        }
      }
    }
    catch (std::exception const& e)
    {
      LogError << "[MapRegistration] Could not parse \"" << path.string() << "\": " << e.what()
               << " - no registered maps were loaded." << std::endl;
      registrations.clear();
    }

    return registrations;
  }

  bool ModernMapRegistrations::save(std::filesystem::path const& project_path,
                                    std::vector<ModernMapRegistration> const& registrations)
  {
    auto const path = storage_path(project_path);

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
      LogError << "[MapRegistration] Could not create \"" << path.parent_path().string() << "\": "
               << ec.message() << std::endl;
      return false;
    }

    JsonArray maps;
    for (auto const& registration : registrations)
      maps.push_back(registration_to_json(registration));

    JsonObject document;
    document.emplace_back("version", JsonValue::make_number(std::to_string(storage_format_version)));
    document.emplace_back("maps", JsonValue::make_array(std::move(maps)));

    std::ostringstream content;
    write_json_value(content, JsonValue::make_object(std::move(document)), 0);
    content << '\n';

    auto const temporary = path.string() + ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      if (!file.is_open())
      {
        LogError << "[MapRegistration] Could not open \"" << temporary << "\" for writing." << std::endl;
        return false;
      }
      file << content.str();
      file.flush();
      if (!file.good())
      {
        LogError << "[MapRegistration] Write error while saving \"" << path.string() << "\"" << std::endl;
        file.close();
        std::remove(temporary.c_str());
        return false;
      }
    }

    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
      LogError << "[MapRegistration] Could not replace \"" << path.string() << "\": " << ec.message()
               << std::endl;
      std::remove(temporary.c_str());
      return false;
    }
    return true;
  }

  void ModernMapRegistrations::upsert(std::vector<ModernMapRegistration>& registrations,
                                      ModernMapRegistration registration)
  {
    auto existing = std::find_if(registrations.begin(), registrations.end(),
                                 [&](ModernMapRegistration const& other) { return other.id == registration.id; });
    if (existing != registrations.end())
      *existing = std::move(registration);
    else
      registrations.push_back(std::move(registration));
  }

  bool ModernMapRegistrations::remove(std::vector<ModernMapRegistration>& registrations, int id)
  {
    auto const previous = registrations.size();
    registrations.erase(std::remove_if(registrations.begin(), registrations.end(),
                                       [&](ModernMapRegistration const& other) { return other.id == id; }),
                        registrations.end());
    return registrations.size() != previous;
  }

  std::string ModernMapRegistrations::validate(ModernMapRegistration const& registration)
  {
    if (registration.id <= 0)
      return "map id must be a positive integer";

    auto const field = [&](char const* name) -> std::string
    {
      auto const it = registration.fields.find(name);
      return it == registration.fields.end() ? std::string{} : it->second;
    };

    // Required set = the columns a map cannot function without in the first
    // targeted modern layout (Map.dbd LAYOUT AAA1F94E, Shadowlands 9.2.7).
    // Everything else in that layout (Corpse, CorpseMapID, TimeOfDayOverride,
    // MapType, ParentMapID, WindSettingsID, Zmp/WdtFileDataID, ...) is
    // optional and falls back to converter defaults at merge time.
    static char const* required[] = {
      "Directory", "InstanceType", "Flags", "MapName_lang", "AreaTableID",
      "ExpansionID", "MaxPlayers", "LoadingScreenID", "MinimapIconScale",
    };
    for (char const* name : required)
      if (registration.fields.find(name) == registration.fields.end())
        return std::string("missing required Map.db2 field \"") + name + "\"";

    std::string const directory = field("Directory");
    if (directory.empty())
      return "Directory must not be empty";
    if (directory.size() > 64)
      return "Directory must not exceed 64 characters";
    for (char c : directory)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
        return "Directory may only contain letters, digits and underscores";

    std::string const map_name = field("MapName_lang");
    if (map_name.empty())
      return "MapName_lang must not be empty";

    if (!parses_as_u32(field("InstanceType")))
      return "InstanceType must be a non-negative integer";
    if (parse_u32(field("InstanceType"), 99) > 4)
      return "InstanceType must be between 0 (none) and 4 (arena)";

    if (!parses_as_u32(field("Flags")))
      return "Flags must be a non-negative integer";
    if (!parses_as_u32(field("AreaTableID")))
      return "AreaTableID must be a non-negative integer";
    if (!parses_as_u32(field("LoadingScreenID")))
      return "LoadingScreenID must be a non-negative integer";
    if (!parses_as_u32(field("MaxPlayers")))
      return "MaxPlayers must be a non-negative integer";
    if (!parses_as_u32(field("ExpansionID")))
      return "ExpansionID must be a non-negative integer";
    if (parse_u32(field("ExpansionID"), 999) > 20)
      return "ExpansionID is implausibly large";
    if (!parses_as_f32(field("MinimapIconScale")))
      return "MinimapIconScale must be a number";

    for (char const* optional : {"CorpseMapID", "TimeOfDayOverride", "RaidOffset"})
    {
      auto const value = field(optional);
      if (!value.empty() && !parses_as_i32(value))
        return std::string(optional) + " must be an integer";
    }
    for (char const* optional : {"MapDescription1_lang", "MapDescription0_lang"})
      field(optional); // presence optional, any text accepted

    auto const corpse = registration.array_fields.find("Corpse");
    if (corpse != registration.array_fields.end())
    {
      if (corpse->second.size() > 2)
        return "Corpse must hold at most two coordinates";
      for (auto const& coordinate : corpse->second)
        if (!parses_as_f32(coordinate))
          return "Corpse coordinates must be numbers";
    }

    if (!registration.target_build.empty())
    {
      int parts = 1;
      for (char c : registration.target_build)
      {
        if (c == '.')
          ++parts;
        else if (!std::isdigit(static_cast<unsigned char>(c)))
          return "target_build must look like a client build (digits and dots)";
      }
      if (parts < 3 || parts > 4)
        return "target_build must have three or four dot-separated components";
    }

    return {};
  }

  bool ModernMapRegistrations::apply_to_map_dbc(DBCFile& map_dbc,
                                                ModernMapRegistration const& registration)
  {
    try
    {
      map_dbc.getByID(static_cast<unsigned int>(registration.id));
      return false; // client data (or an earlier merge) already provides this id
    }
    catch (DBCFile::NotFound const&)
    {
    }

    auto const field = [&](char const* name) -> std::string
    {
      auto const it = registration.fields.find(name);
      return it == registration.fields.end() ? std::string{} : it->second;
    };
    auto const array_field = [&](char const* name) -> std::vector<std::string>
    {
      auto const it = registration.array_fields.find(name);
      return it == registration.array_fields.end() ? std::vector<std::string>{} : it->second;
    };

    DBCFile::Record record = map_dbc.addRecord(static_cast<std::size_t>(registration.id));

    // Same legacy column mapping the DB2 bridge uses for CASC rows, so
    // registered maps are indistinguishable from client maps for consumers.
    record.writeString(MapDB::InternalName, field("Directory"));
    record.write(MapDB::AreaType, parse_u32(field("InstanceType"), 0));
    record.write(MapDB::Flags, parse_u32(field("Flags"), 0));
    record.writeLocalizedString(MapDB::Name, field("MapName_lang"), 0);
    record.write(MapDB::AreaTableID, parse_u32(field("AreaTableID"), 0));
    record.writeLocalizedString(MapDB::MapDescriptionAlliance, field("MapDescription1_lang"), 0);
    record.writeLocalizedString(MapDB::MapDescriptionHorde, field("MapDescription0_lang"), 0);
    record.write(MapDB::LoadingScreen, parse_u32(field("LoadingScreenID"), 0));
    record.write(MapDB::minimapIconScale, parse_f32(field("MinimapIconScale"), 1.0f));
    record.write(MapDB::corpseMapID, parse_i32(field("CorpseMapID"), -1));
    auto const corpse = array_field("Corpse");
    record.write(MapDB::corpseX, corpse.size() > 0 ? parse_f32(corpse[0], 0.0f) : 0.0f);
    record.write(MapDB::corpseY, corpse.size() > 1 ? parse_f32(corpse[1], 0.0f) : 0.0f);
    record.write(MapDB::TimeOfDayOverride, parse_i32(field("TimeOfDayOverride"), -1));
    record.write(MapDB::ExpansionID, parse_u32(field("ExpansionID"), 0));
    record.write(MapDB::RaidOffset, parse_u32(field("RaidOffset"), 0));
    record.write(MapDB::NumberOfPlayers, parse_u32(field("MaxPlayers"), 0));

    return true;
  }
}
