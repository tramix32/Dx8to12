#include "config.h"

#include <fstream>
#include <sstream>

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

// LightingMode values 2+ need a raytracing-capable device; there either isn't
// one yet (config load happens at DLL_PROCESS_ATTACH, long before device
// creation) or the adapter doesn't support DXR. Either way, refuse to hand
// out a mode nothing can render and say why.
bool RaytracingSupported() {
  Device *device = GetCurrentDeviceForModApi();
  return device && device->raytracing_supported();
}

}  // namespace

Config &GetConfig() { return g_config; }

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

  std::ifstream file(ini_path);
  if (!file.is_open()) {
    LOG(AixLog::Severity::info)
        << "LoadConfig: no dx8to12.ini found next to the DLL, using "
           "defaults.\n";
    return;
  }

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

    bool applied = false;
    try {
      if (EqualsIgnoreCase(key, "AnisotropicOverride")) {
        applied = SetConfigValueInt(key, std::stoi(value));
      } else if (EqualsIgnoreCase(key, "MSAASamples")) {
        applied = SetConfigValueInt(key, std::stoi(value));
      } else if (EqualsIgnoreCase(key, "LightingMode")) {
        applied = SetConfigValueInt(key, std::stoi(value));
      } else if (EqualsIgnoreCase(key, "SharpenStrength")) {
        applied = SetConfigValueFloat(key, std::stof(value));
      } else if (EqualsIgnoreCase(key, "HighPrecisionDepth")) {
        applied = SetConfigValueBool(
            key, EqualsIgnoreCase(value, "true") || value == "1");
      } else if (EqualsIgnoreCase(key, "FullTraceLog")) {
        applied = SetConfigValueBool(
            key, EqualsIgnoreCase(value, "true") || value == "1");
      } else {
        LOG(AixLog::Severity::error)
            << "LoadConfig: dx8to12.ini line " << line_number
            << ": unrecognized key '" << key << "', ignoring.\n";
        continue;
      }
    } catch (const std::exception &) {
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
  LOG(AixLog::Severity::info)
      << "LoadConfig: loaded dx8to12.ini. Effective settings: "
      << "AnisotropicOverride=" << g_config.anisotropic_override
      << " MSAASamples=" << g_config.msaa_samples
      << " SharpenStrength=" << g_config.sharpen_strength
      << " HighPrecisionDepth=" << g_config.high_precision_depth
      << " FullTraceLog=" << g_config.full_trace_log
      << " LightingMode=" << g_config.lighting_mode << "\n";
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
  return false;
}

bool SetConfigValueInt(const std::string &key, int value) {
  if (EqualsIgnoreCase(key, "AnisotropicOverride")) {
    if (value != -1 && (value < 1 || value > 16)) return false;
    g_config.anisotropic_override = value;
    return true;
  }
  if (EqualsIgnoreCase(key, "MSAASamples")) {
    if (value != 1 && value != 2 && value != 4 && value != 8) return false;
    g_config.msaa_samples = value;
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
  return false;
}

bool SetConfigValueBool(const std::string &key, bool value) {
  if (EqualsIgnoreCase(key, "HighPrecisionDepth")) {
    g_config.high_precision_depth = value;
    return true;
  }
  if (EqualsIgnoreCase(key, "FullTraceLog")) {
    g_config.full_trace_log = value;
    return true;
  }
  return false;
}

}  // namespace Dx8to12
