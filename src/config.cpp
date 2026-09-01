#include "config.h"

#include <fstream>
#include <sstream>
#include <vector>

#include "aixlog.hpp"
#include "device.h"

namespace Dx8to12 {

namespace {

std::string Trim(const std::string &s) {
  size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

// Case-insensitive compare -- INI keys and section names shouldn't be
// case-sensitive, matching every other INI-reading tool a modder is likely
// to have used before.
bool EqualsIgnoreCase(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (tolower(static_cast<unsigned char>(a[i])) !=
        tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

Config g_config;

// Where LoadConfig found (or would have found) the INI, so SaveConfig can
// write back to the same place. Empty until LoadConfig has run.
std::wstring g_ini_path;
bool g_config_dirty = false;
// True while LoadConfig is applying the file. Without this, reading the INI
// would mark every value it just read as needing to be written back.
bool g_loading_config = false;
ULONGLONG g_last_save_tick = 0;
// A mod flipping settings in a menu should see them land promptly; a mod
// animating a value should not cost a file write per frame.
constexpr ULONGLONG kMinSaveIntervalMs = 1000;

// The single list of settings. Everything that needs to enumerate keys --
// the INI parser, the INI writer -- walks this instead of repeating them.
//
// `persist` is false for diagnostics: FullTraceLog produces enormous logs and
// MotionVectorDebug paints over half the screen. If a mod turns either on for
// a session, writing that back would leave the player with a broken-looking
// game and no idea why, having never edited the file themselves.
constexpr ConfigField kFields[] = {
    {"AnisotropicOverride", ConfigFieldType::Int, true},
    {"MSAASamples", ConfigFieldType::Int, true},
    {"LightingMode", ConfigFieldType::Int, true},
    {"SharpenStrength", ConfigFieldType::Float, true},
    {"HighPrecisionDepth", ConfigFieldType::Bool, true},
    {"FullTraceLog", ConfigFieldType::Bool, false},
    {"TemporalAA", ConfigFieldType::Int, true},
    {"TemporalJitter", ConfigFieldType::Bool, true},
    {"MotionVectors", ConfigFieldType::Bool, true},
    {"MotionVectorDebug", ConfigFieldType::Bool, false},
};

// LightingMode values 2+ need a raytracing-capable device; there either isn't
// one yet (config load happens at DLL_PROCESS_ATTACH, long before device
// creation) or the adapter doesn't support DXR. Either way, refuse to hand
// out a mode nothing can render and say why.
bool RaytracingSupported() {
  Device *device = GetCurrentDeviceForModApi();
  return device && device->raytracing_supported();
}

const ConfigField *FindField(const std::string &key) {
  for (const ConfigField &field : kFields) {
    if (EqualsIgnoreCase(key, field.name)) return &field;
  }
  return nullptr;
}

// Applies a raw INI/string value to `field`. Returns false if it doesn't
// parse or the setter rejects it as out of range -- the two are reported
// differently by the caller, so they're distinguished by `out_parsed`.
bool ApplyStringValue(const ConfigField &field, const std::string &value,
                      bool *out_parsed) {
  *out_parsed = true;
  try {
    switch (field.type) {
      case ConfigFieldType::Int:
        return SetConfigValueInt(field.name, std::stoi(value));
      case ConfigFieldType::Float:
        return SetConfigValueFloat(field.name, std::stof(value));
      case ConfigFieldType::Bool:
        return SetConfigValueBool(
            field.name, EqualsIgnoreCase(value, "true") || value == "1");
    }
  } catch (const std::exception &) {
    *out_parsed = false;
    return false;
  }
  return false;
}

// Current value of `field`, formatted the way the INI wants it.
std::string CurrentValueAsString(const ConfigField &field) {
  switch (field.type) {
    case ConfigFieldType::Int: {
      int value = 0;
      GetConfigValueInt(field.name, &value);
      return std::to_string(value);
    }
    case ConfigFieldType::Float: {
      float value = 0.f;
      GetConfigValueFloat(field.name, &value);
      std::ostringstream out;
      out << value;
      return out.str();
    }
    case ConfigFieldType::Bool: {
      bool value = false;
      GetConfigValueBool(field.name, &value);
      return value ? "true" : "false";
    }
  }
  return "";
}

}  // namespace

const ConfigField *ConfigFields(size_t *out_count) {
  if (out_count) *out_count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

Config &GetConfig() { return g_config; }

void MarkConfigDirty() {
  if (g_loading_config) return;
  g_config_dirty = true;
}

void FlushConfigIfDirty(bool force) {
  if (!g_config_dirty) return;
  const ULONGLONG now = GetTickCount64();
  if (!force && now - g_last_save_tick < kMinSaveIntervalMs) return;
  g_last_save_tick = now;
  g_config_dirty = false;
  SaveConfig();
}

void SaveConfig() {
  if (g_ini_path.empty()) return;

  // Read the file as it stands. Rewriting only the values keeps the modder's
  // own comments, ordering and section headers intact; regenerating the file
  // from kFields would throw all of that away every time a mod nudged a
  // setting.
  std::vector<std::string> lines;
  {
    std::ifstream existing(g_ini_path);
    std::string line;
    while (std::getline(existing, line)) {
      // getline leaves the CR of a CRLF file on the string.
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }

  std::vector<bool> written(sizeof(kFields) / sizeof(kFields[0]), false);
  for (std::string &line : lines) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(trimmed.substr(0, eq));
    const ConfigField *field = FindField(key);
    if (!field || !field->persist) continue;
    const size_t index = static_cast<size_t>(field - kFields);
    written[index] = true;
    // Preserve any trailing comment on the line -- it is usually the
    // explanation of what the setting does.
    const std::string value_part = trimmed.substr(eq + 1);
    const size_t comment = value_part.find_first_of(";#");
    const std::string trailing =
        comment == std::string::npos ? "" : " " + Trim(value_part.substr(comment));
    // Keep the key exactly as the file spells it, not as kFields does.
    line = key + " = " + CurrentValueAsString(*field) + trailing;
  }

  // Anything the file didn't mention yet gets appended, so a setting a mod
  // turned on for the first time is actually recorded.
  std::vector<std::string> appended;
  for (size_t i = 0; i < written.size(); ++i) {
    if (written[i] || !kFields[i].persist) continue;
    appended.push_back(std::string(kFields[i].name) + " = " +
                       CurrentValueAsString(kFields[i]));
  }
  if (!appended.empty()) {
    if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
    lines.push_back("; Written by Dx8to12.");
    for (const std::string &line : appended) lines.push_back(line);
  }

  std::ofstream out(g_ini_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    LOG(AixLog::Severity::error)
        << "SaveConfig: could not open dx8to12.ini for writing.\n";
    return;
  }
  for (const std::string &line : lines) out << line << "\r\n";
}

// Every setting this DLL exposes, both to the INI file and to the runtime
// Dx8to12_* mod API (dx8to12_api.cpp) -- add a new field here (and to
// Config, config.h) rather than hand-rolling a new parse/lookup path per
// setting. Deliberately hand-rolled instead of pulling in an INI library:
// this file format is tiny (flat key=value, one array of typed fields, no
// nesting) and not worth a third-party dependency for.
void LoadConfig(HMODULE dll_module) {
  wchar_t path[MAX_PATH];
  DWORD len = GetModuleFileNameW(dll_module, path, MAX_PATH);
  if (len == 0 || len == MAX_PATH) {
    LOG(AixLog::Severity::error)
        << "LoadConfig: GetModuleFileNameW failed, using defaults.\n";
    return;
  }
  std::wstring ini_path(path);
  size_t slash = ini_path.find_last_of(L"\\/");
  ini_path = (slash == std::wstring::npos ? L"" : ini_path.substr(0, slash + 1)) +
            L"dx8to12.ini";
  // Remembered even when the file doesn't exist yet: a mod changing a setting
  // in a fresh install should still produce an INI rather than silently lose
  // the change.
  g_ini_path = ini_path;

  std::ifstream file(ini_path);
  if (!file.is_open()) {
    LOG(AixLog::Severity::info)
        << "LoadConfig: no dx8to12.ini found next to the DLL, using "
           "defaults.\n";
    return;
  }

  g_loading_config = true;
  std::string line;
  int line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
    if (trimmed.front() == '[' && trimmed.back() == ']') {
      // Sections are accepted (for readability/organization in the INI) but
      // not required or checked -- every key is globally unique regardless
      // of which section it's written under.
      continue;
    }
    size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      LOG(AixLog::Severity::error)
          << "LoadConfig: dx8to12.ini line " << line_number
          << ": expected key=value, got: " << trimmed << "\n";
      continue;
    }
    std::string key = Trim(trimmed.substr(0, eq));
    std::string value = Trim(trimmed.substr(eq + 1));
    // Strip an inline `; comment` or `# comment` trailing the value.
    size_t comment = value.find_first_of(";#");
    if (comment != std::string::npos) value = Trim(value.substr(0, comment));

    const ConfigField *field = FindField(key);
    if (!field) {
      LOG(AixLog::Severity::error)
          << "LoadConfig: dx8to12.ini line " << line_number
          << ": unrecognized key '" << key << "', ignoring.\n";
      continue;
    }
    bool parsed = true;
    const bool applied = ApplyStringValue(*field, value, &parsed);
    if (!parsed) {
      LOG(AixLog::Severity::error)
          << "LoadConfig: dx8to12.ini line " << line_number
          << ": couldn't parse value for '" << key << "': " << value << "\n";
      continue;
    }
    if (!applied) {
      LOG(AixLog::Severity::error)
          << "LoadConfig: dx8to12.ini line " << line_number << ": key '"
          << key << "' rejected an out-of-range value: " << value << "\n";
    }
  }
  g_loading_config = false;
  // Enumerated rather than hand-listed, so a new setting shows up here
  // automatically instead of being silently missing from the one place
  // someone looks to check what actually took effect.
  std::ostringstream summary;
  for (const ConfigField &field : kFields) {
    summary << " " << field.name << "=" << CurrentValueAsString(field);
  }
  LOG(AixLog::Severity::info)
      << "LoadConfig: loaded dx8to12.ini. Effective settings:" << summary.str()
      << "\n";
}

bool GetConfigValueInt(const std::string &key, int *out_value) {
  if (EqualsIgnoreCase(key, "AnisotropicOverride")) {
    *out_value = g_config.anisotropic_override;
    return true;
  }
  if (EqualsIgnoreCase(key, "MSAASamples")) {
    *out_value = g_config.msaa_samples;
    return true;
  }
  if (EqualsIgnoreCase(key, "LightingMode")) {
    *out_value = g_config.lighting_mode;
    return true;
  }
  if (EqualsIgnoreCase(key, "TemporalAA")) {
    *out_value = g_config.temporal_aa;
    return true;
  }
  return false;
}

bool SetConfigValueInt(const std::string &key, int value) {
  if (EqualsIgnoreCase(key, "AnisotropicOverride")) {
    if (value != -1 && (value < 1 || value > 16)) return false;
    if (value != g_config.anisotropic_override) MarkConfigDirty();
    g_config.anisotropic_override = value;
    return true;
  }
  if (EqualsIgnoreCase(key, "MSAASamples")) {
    if (value != 1 && value != 2 && value != 4 && value != 8) return false;
    if (value != g_config.msaa_samples) MarkConfigDirty();
    g_config.msaa_samples = value;
    return true;
  }
  if (EqualsIgnoreCase(key, "TemporalAA")) {
    if (value < 0 || value > 2) return false;
    if (value != g_config.temporal_aa) {
      g_config.temporal_aa = value;
      MarkConfigDirty();
    }
    // A temporal upscaler without jitter and motion vectors does not degrade
    // gracefully -- it produces a blurred, ghosting image that looks like a
    // bug in the upscaler. Rather than let that combination exist, turning
    // TemporalAA on turns its inputs on.
    if (value != 0) {
      SetConfigValueBool("TemporalJitter", true);
      SetConfigValueBool("MotionVectors", true);
    }
    return true;
  }
  if (EqualsIgnoreCase(key, "LightingMode")) {
    if (value < 0 || value > 4) return false;
    if (value >= 2 && !RaytracingSupported()) {
      LOG(AixLog::Severity::error)
          << "LightingMode " << value
          << " requires raytracing support that isn't available yet (no "
             "device, or the adapter doesn't support DXR) -- staying on "
             "PerPixel (1) instead.\n";
      value = 1;
    }
    if (value != g_config.lighting_mode) {
      g_config.lighting_mode = value;
      MarkConfigDirty();
      // Fixed-function vertex/pixel shader generation bakes lighting_mode in
      // at compile time (vertex_shader.cpp, ff_pixel_shader.cpp) -- anything
      // already compiled under the old mode has to be thrown away, or the
      // game would keep drawing with stale-mode shaders until whichever
      // FVF/texture-stage combination they were cached under happened to
      // never recur. No-op before device creation (LoadConfig at
      // DLL_PROCESS_ATTACH): there's nothing compiled yet to invalidate, and
      // RaytracingSupported() above already refused any device-less request
      // for a raytraced mode anyway.
      if (Device *device = GetCurrentDeviceForModApi()) {
        device->OnLightingModeChanged();
      }
    }
    return true;
  }
  return false;
}

bool GetConfigValueFloat(const std::string &key, float *out_value) {
  if (EqualsIgnoreCase(key, "SharpenStrength")) {
    *out_value = g_config.sharpen_strength;
    return true;
  }
  return false;
}

bool SetConfigValueFloat(const std::string &key, float value) {
  if (EqualsIgnoreCase(key, "SharpenStrength")) {
    if (value < 0.0f || value > 1.0f) return false;
    if (value != g_config.sharpen_strength) MarkConfigDirty();
    g_config.sharpen_strength = value;
    return true;
  }
  return false;
}

bool GetConfigValueBool(const std::string &key, bool *out_value) {
  if (EqualsIgnoreCase(key, "HighPrecisionDepth")) {
    *out_value = g_config.high_precision_depth;
    return true;
  }
  if (EqualsIgnoreCase(key, "FullTraceLog")) {
    *out_value = g_config.full_trace_log;
    return true;
  }
  if (EqualsIgnoreCase(key, "TemporalJitter")) {
    *out_value = g_config.temporal_jitter;
    return true;
  }
  if (EqualsIgnoreCase(key, "MotionVectors")) {
    *out_value = g_config.motion_vectors;
    return true;
  }
  if (EqualsIgnoreCase(key, "MotionVectorDebug")) {
    *out_value = g_config.motion_vector_debug;
    return true;
  }
  return false;
}

bool SetConfigValueBool(const std::string &key, bool value) {
  if (EqualsIgnoreCase(key, "HighPrecisionDepth")) {
    if (value != g_config.high_precision_depth) MarkConfigDirty();
    g_config.high_precision_depth = value;
    return true;
  }
  if (EqualsIgnoreCase(key, "FullTraceLog")) {
    g_config.full_trace_log = value;
    return true;
  }
  if (EqualsIgnoreCase(key, "TemporalJitter")) {
    if (value != g_config.temporal_jitter) MarkConfigDirty();
    g_config.temporal_jitter = value;
    return true;
  }
  if (EqualsIgnoreCase(key, "MotionVectors")) {
    if (value != g_config.motion_vectors) MarkConfigDirty();
    g_config.motion_vectors = value;
    // The debug overlay draws the motion vectors; with them off there is
    // nothing for it to draw, so it cannot stay on by itself.
    if (!value) g_config.motion_vector_debug = false;
    return true;
  }
  if (EqualsIgnoreCase(key, "MotionVectorDebug")) {
    g_config.motion_vector_debug = value;
    // Asking to see the vectors is asking for them to exist.
    if (value) SetConfigValueBool("MotionVectors", true);
    return true;
  }
  return false;
}

}  // namespace Dx8to12
