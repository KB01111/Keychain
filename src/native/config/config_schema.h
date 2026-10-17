#pragma once

#include "common/types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <filesystem>

namespace keychain {

constexpr int32_t kCurrentConfigSchemaVersion = 1;
constexpr size_t  kMaxConfigFileBytes         = 512 * 1024; // 512 KiB
constexpr size_t  kMaxProfiles                = 32;
constexpr size_t  kMaxRings                   = 128;
constexpr size_t  kMaxActions                 = 512;
constexpr size_t  kMaxExclusions              = 128;
constexpr size_t  kMaxStringLength            = 1024;

enum class ActionType {
    None,
    LaunchApp,            // Launch .exe or .lnk
    OpenFile,             // Open document/file with associated app
    OpenFolder,           // Open directory in File Explorer
    OpenUrl,              // Open http:// or https:// URL
    SendShortcut,         // Inject configured key chord after modifiers release
    WindowsAction,        // Allowlisted system facility (Documents, Downloads, FileExplorer, Notepad, WindowsSettings, etc.)
    OpenKeychainSettings, // Launch Keychain.Settings.exe
    OpenChildRing         // Navigate into 1-level child ring
};

std::string ActionTypeToString(ActionType type);
std::optional<ActionType> StringToActionType(std::string_view str);

struct ActionDefinition {
    std::string id;
    ActionType  type = ActionType::None;
    std::string label;
    std::string target;            // Path to .exe/.lnk/file/folder, or http(s) URL
    std::string arguments;         // Separate argument string (never passed to cmd.exe)
    std::string workingDirectory;
    std::string shortcutChord;     // e.g. "Ctrl+Shift+Esc" or "Ctrl+C"
    std::string windowsActionName; // "Documents", "Downloads", "FileExplorer", "Notepad", "WindowsSettings", "LockWorkstation", "TaskManager"
    std::string childRingId;       // Target ring ID when type == OpenChildRing
    std::string iconGlyph;         // Optional Segoe Fluent Icons glyph or label initials
    bool        approved = true;   // Imported executable/shortcut actions start false until user reviews/enables
    bool        enabled  = true;
};

struct RadialRing {
    std::string              id;
    std::string              name;
    uint32_t                 sectorCount = 6; // Must be 6 or 8
    bool                     isChildRing = false;
    std::string              parentRingId;
    std::vector<std::string> sectorActionIds; // Exactly sectorCount elements (empty string = None)
};

struct Profile {
    std::string              id;
    std::string              name;
    std::string              rootRingId;
    std::vector<std::string> exclusions; // Per-profile foreground exe exclusions
};

struct Preferences {
    // General
    bool        enabled                      = true;
    bool        launchAtStartup              = false;
    bool        showTrayIcon                 = true;
    bool        completedTutorial            = false;

    // Hotring & Input
    std::string activationMode               = "WinCaps"; // "WinCaps" or "CtrlAltSpace"
    std::string interactionMode              = "Hold";    // "Hold" or "Toggle"
    float       ringDiameterDip              = 380.0f;    // 260..540 DIPs
    uint32_t    defaultSectorCount           = 6;         // 6 or 8
    float       deadZoneRadiusDip            = 44.0f;     // 24..80 DIPs
    uint32_t    childRingDwellMs             = 250;       // 100..1000 ms
    float       sectorGapDip                 = 3.5f;      // 1..10 DIPs

    // Appearance & Motion
    std::string theme                        = "Dark";     // "Dark", "Light", "System"
    std::string accentColorHex               = "#5CC8FF";  // Midnight Glass ice-blue accent
    float       glassOpacity                 = 0.86f;      // 0.35..1.0
    float       iconSizeDip                  = 24.0f;      // 16..40 DIPs
    std::string motionPreset                 = "Standard"; // "Standard", "Snappy", "Gentle"
    float       animationSpeedMultiplier     = 1.0f;       // 0.5..2.0
    std::string reducedMotionOverride        = "System";   // "System", "On", "Off"
    std::string reducedTransparencyOverride  = "System";   // "System", "On", "Off"

    // Performance & Diagnostics
    std::string qualityPreset                = "Auto";     // "Auto", "Performance", "Efficiency"
    uint32_t    efficiencyFrameCapHz         = 60;         // 30..240 Hz when qualityPreset == "Efficiency"
    std::string preferredAdapterLuid         = "";         // "" = Auto (display-driving/efficient GPU)
    bool        redactPathsInDiagnostics     = true;
    std::vector<std::string> foregroundExclusions;         // Global foreground exe exclusions
};

struct ConfigDocument {
    int32_t                       schemaVersion   = kCurrentConfigSchemaVersion;
    uint64_t                      revision        = 1;
    std::string                   activeProfileId = "profile_default";
    Preferences                   preferences;
    std::vector<Profile>          profiles;
    std::vector<RadialRing>       rings;
    std::vector<ActionDefinition> actions;

    const Profile* FindActiveProfile() const noexcept;
    const RadialRing* FindRing(std::string_view ringId) const noexcept;
    const ActionDefinition* FindAction(std::string_view actionId) const noexcept;
};

struct ValidationResult {
    bool                     valid = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    void AddError(std::string msg) {
        valid = false;
        errors.push_back(std::move(msg));
    }
    void AddWarning(std::string msg) {
        warnings.push_back(std::move(msg));
    }
};

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path storageDir = GetDefaultStorageDirectory());

    // Creates the canonical default configuration with Documents, Downloads,
    // File Explorer, Notepad, Windows Settings, Keychain Settings, and a sample child ring.
    static ConfigDocument CreateDefaultConfig();

    // Validates bounds, IDs, sector counts (6 or 8), and max child-ring depth <= 1 (no cycles).
    static ValidationResult ValidateConfig(const ConfigDocument& doc);

    // Serializes/deserializes UTF-8 JSON.
    static std::string SerializeToJson(const ConfigDocument& doc, bool pretty = true);
    static bool DeserializeFromJson(std::string_view jsonText, ConfigDocument& outDoc, ValidationResult& outValidation);

    // Marks imported LaunchApp / OpenFile / .lnk actions as unapproved (approved = false) until user review.
    static bool ImportConfigFromJson(std::string_view jsonText, ConfigDocument& outDoc, ValidationResult& outValidation);

    // Loads from config.json, recovering automatically from config.last_good.json or defaults on corruption.
    ConfigDocument LoadOrRecover(ValidationResult* outLoadReport = nullptr);

    // Atomically validates and saves config.json and updates config.last_good.json.
    ValidationResult SaveAtomic(ConfigDocument newDoc);

    // Returns thread-safe immutable snapshot of the active configuration.
    std::shared_ptr<const ConfigDocument> GetSnapshot() const noexcept;

    std::filesystem::path GetConfigFilePath() const { return storageDir_ / L"config.json"; }
    std::filesystem::path GetBackupFilePath() const { return storageDir_ / L"config.last_good.json"; }
    std::filesystem::path GetStorageDirectory() const { return storageDir_; }

    static std::filesystem::path GetDefaultStorageDirectory();

private:
    std::filesystem::path storageDir_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::shared_ptr<const ConfigDocument> snapshot_;
};

} // namespace keychain
