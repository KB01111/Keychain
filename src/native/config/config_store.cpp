#include "config/config_schema.h"
#include <nlohmann/json.hpp>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace keychain {

using json = nlohmann::json;

std::string ActionTypeToString(ActionType type) {
    switch (type) {
        case ActionType::None:                 return "None";
        case ActionType::LaunchApp:            return "LaunchApp";
        case ActionType::OpenFile:             return "OpenFile";
        case ActionType::OpenFolder:           return "OpenFolder";
        case ActionType::OpenUrl:              return "OpenUrl";
        case ActionType::SendShortcut:         return "SendShortcut";
        case ActionType::WindowsAction:        return "WindowsAction";
        case ActionType::OpenKeychainSettings: return "OpenKeychainSettings";
        case ActionType::OpenChildRing:        return "OpenChildRing";
    }
    return "None";
}

std::optional<ActionType> StringToActionType(std::string_view str) {
    if (str == "None")                 return ActionType::None;
    if (str == "LaunchApp")            return ActionType::LaunchApp;
    if (str == "OpenFile")             return ActionType::OpenFile;
    if (str == "OpenFolder")           return ActionType::OpenFolder;
    if (str == "OpenUrl")              return ActionType::OpenUrl;
    if (str == "SendShortcut")         return ActionType::SendShortcut;
    if (str == "WindowsAction")        return ActionType::WindowsAction;
    if (str == "OpenKeychainSettings") return ActionType::OpenKeychainSettings;
    if (str == "OpenChildRing")        return ActionType::OpenChildRing;
    return std::nullopt;
}

const Profile* ConfigDocument::FindActiveProfile() const noexcept {
    for (const auto& p : profiles) {
        if (p.id == activeProfileId) return &p;
    }
    return profiles.empty() ? nullptr : &profiles.front();
}

const RadialRing* ConfigDocument::FindRing(std::string_view ringId) const noexcept {
    for (const auto& r : rings) {
        if (r.id == ringId) return &r;
    }
    return nullptr;
}

const ActionDefinition* ConfigDocument::FindAction(std::string_view actionId) const noexcept {
    if (actionId.empty()) return nullptr;
    for (const auto& a : actions) {
        if (a.id == actionId) return &a;
    }
    return nullptr;
}

std::filesystem::path ConfigStore::GetDefaultStorageDirectory() {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData) {
        std::filesystem::path p(localAppData);
        CoTaskMemFree(localAppData);
        return p / L"Keychain";
    }
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buf) / L"Keychain";
    }
    return std::filesystem::temp_directory_path() / L"Keychain";
}

ConfigStore::ConfigStore(std::filesystem::path storageDir)
    : storageDir_(std::move(storageDir)) {
    std::error_code ec;
    std::filesystem::create_directories(storageDir_, ec);
    snapshot_ = std::make_shared<const ConfigDocument>(CreateDefaultConfig());
}

ConfigDocument ConfigStore::CreateDefaultConfig() {
    ConfigDocument doc;
    doc.schemaVersion = kCurrentConfigSchemaVersion;
    doc.revision = 1;
    doc.activeProfileId = "profile_default";

    // Default 6-sector actions per specification:
    // Documents, Downloads, File Explorer, Notepad, Windows Settings, and Keychain Settings.
    doc.actions = {
        ActionDefinition{
            .id = "act_documents",
            .type = ActionType::WindowsAction,
            .label = "Documents",
            .windowsActionName = "Documents",
            .iconGlyph = "DOC",
            .approved = true,
            .enabled = true
        },
        ActionDefinition{
            .id = "act_downloads",
            .type = ActionType::WindowsAction,
            .label = "Downloads",
            .windowsActionName = "Downloads",
            .iconGlyph = "DL",
            .approved = true,
            .enabled = true
        },
        ActionDefinition{
            .id = "act_explorer",
            .type = ActionType::WindowsAction,
            .label = "File Explorer",
            .windowsActionName = "FileExplorer",
            .iconGlyph = "EXP",
            .approved = true,
            .enabled = true
        },
        ActionDefinition{
            .id = "act_notepad",
            .type = ActionType::WindowsAction,
            .label = "Notepad",
            .windowsActionName = "Notepad",
            .iconGlyph = "TXT",
            .approved = true,
            .enabled = true
        },
        ActionDefinition{
            .id = "act_win_settings",
            .type = ActionType::WindowsAction,
            .label = "Windows Settings",
            .windowsActionName = "WindowsSettings",
            .iconGlyph = "WIN",
            .approved = true,
            .enabled = true
        },
        ActionDefinition{
            .id = "act_keychain_settings",
            .type = ActionType::OpenKeychainSettings,
            .label = "Keychain Settings",
            .iconGlyph = "CFG",
            .approved = true,
            .enabled = true
        },
        // Optional child-ring actions available for assignment or child-ring use
        ActionDefinition{
            .id = "act_child_tools",
            .type = ActionType::OpenChildRing,
            .label = "Quick Tools",
            .childRingId = "ring_quick_tools",
            .iconGlyph = "SUB",
            .approved = true,
            .enabled = true
        },
        ActionDefinition{
            .id = "act_task_manager",
            .type = ActionType::WindowsAction,
            .label = "Task Manager",
            .windowsActionName = "TaskManager",
            .iconGlyph = "TSK",
            .approved = true,
            .enabled = true
        },
        ActionDefinition{
            .id = "act_copy_chord",
            .type = ActionType::SendShortcut,
            .label = "Copy Selection",
            .shortcutChord = "Ctrl+C",
            .iconGlyph = "CPY",
            .approved = true,
            .enabled = true
        },
        ActionDefinition{
            .id = "act_paste_chord",
            .type = ActionType::SendShortcut,
            .label = "Paste Clipboard",
            .shortcutChord = "Ctrl+V",
            .iconGlyph = "PST",
            .approved = true,
            .enabled = true
        }
    };

    doc.rings = {
        RadialRing{
            .id = "ring_root_default",
            .name = "Main Ring",
            .sectorCount = 6,
            .isChildRing = false,
            .parentRingId = "",
            .sectorActionIds = {
                "act_documents",
                "act_downloads",
                "act_explorer",
                "act_notepad",
                "act_win_settings",
                "act_keychain_settings"
            }
        },
        RadialRing{
            .id = "ring_quick_tools",
            .name = "Quick Tools",
            .sectorCount = 6,
            .isChildRing = true,
            .parentRingId = "ring_root_default",
            .sectorActionIds = {
                "act_task_manager",
                "act_copy_chord",
                "act_paste_chord",
                "act_documents",
                "act_explorer",
                "act_keychain_settings"
            }
        }
    };

    doc.profiles = {
        Profile{
            .id = "profile_default",
            .name = "Default Profile",
            .rootRingId = "ring_root_default",
            .exclusions = {}
        }
    };

    return doc;
}

ValidationResult ConfigStore::ValidateConfig(const ConfigDocument& doc) {
    ValidationResult res;

    if (doc.schemaVersion != kCurrentConfigSchemaVersion) {
        res.AddError("Unsupported schemaVersion: " + std::to_string(doc.schemaVersion));
    }
    if (doc.profiles.empty()) {
        res.AddError("Configuration must contain at least one profile.");
    }
    if (doc.profiles.size() > kMaxProfiles) {
        res.AddError("Profile count exceeds maximum bound (" + std::to_string(kMaxProfiles) + ").");
    }
    if (doc.rings.empty()) {
        res.AddError("Configuration must contain at least one ring.");
    }
    if (doc.rings.size() > kMaxRings) {
        res.AddError("Ring count exceeds maximum bound (" + std::to_string(kMaxRings) + ").");
    }
    if (doc.actions.size() > kMaxActions) {
        res.AddError("Action count exceeds maximum bound (" + std::to_string(kMaxActions) + ").");
    }
    if (doc.preferences.foregroundExclusions.size() > kMaxExclusions) {
        res.AddError("Foreground exclusion count exceeds maximum bound.");
    }

    auto checkStrLen = [&](std::string_view s, const char* fieldName) {
        if (s.size() > kMaxStringLength) {
            res.AddError(std::string(fieldName) + " exceeds maximum string length.");
        }
    };

    // Validate Preferences
    const auto& pref = doc.preferences;
    if (pref.activationMode != "WinCaps" && pref.activationMode != "CtrlAltSpace") {
        res.AddError("Invalid activationMode: " + pref.activationMode);
    }
    if (pref.interactionMode != "Hold" && pref.interactionMode != "Toggle") {
        res.AddError("Invalid interactionMode: " + pref.interactionMode);
    }
    if (pref.ringDiameterDip < 240.0f || pref.ringDiameterDip > 640.0f) {
        res.AddError("ringDiameterDip out of range [240, 640].");
    }
    if (pref.defaultSectorCount != 6 && pref.defaultSectorCount != 8) {
        res.AddError("defaultSectorCount must be 6 or 8.");
    }
    if (pref.deadZoneRadiusDip < 20.0f || pref.deadZoneRadiusDip > 100.0f) {
        res.AddError("deadZoneRadiusDip out of range [20, 100].");
    }
    if (pref.childRingDwellMs < 80 || pref.childRingDwellMs > 2000) {
        res.AddError("childRingDwellMs out of range [80, 2000].");
    }
    if (pref.glassOpacity < 0.2f || pref.glassOpacity > 1.0f) {
        res.AddError("glassOpacity out of range [0.2, 1.0].");
    }
    if (pref.animationSpeedMultiplier < 0.25f || pref.animationSpeedMultiplier > 4.0f) {
        res.AddError("animationSpeedMultiplier out of range [0.25, 4.0].");
    }

    // Index actions and rings
    std::unordered_map<std::string, const ActionDefinition*> actionMap;
    for (const auto& act : doc.actions) {
        if (act.id.empty()) {
            res.AddError("Action ID cannot be empty.");
            continue;
        }
        checkStrLen(act.id, "action.id");
        checkStrLen(act.label, "action.label");
        checkStrLen(act.target, "action.target");
        checkStrLen(act.arguments, "action.arguments");
        checkStrLen(act.workingDirectory, "action.workingDirectory");
        checkStrLen(act.shortcutChord, "action.shortcutChord");
        if (!actionMap.emplace(act.id, &act).second) {
            res.AddError("Duplicate action ID: " + act.id);
        }
        if (act.type == ActionType::OpenUrl && !act.target.empty()) {
            if (!act.target.starts_with("http://") && !act.target.starts_with("https://")) {
                res.AddError("OpenUrl action '" + act.id + "' must use http:// or https:// scheme.");
            }
        }
    }

    std::unordered_map<std::string, const RadialRing*> ringMap;
    for (const auto& ring : doc.rings) {
        if (ring.id.empty()) {
            res.AddError("Ring ID cannot be empty.");
            continue;
        }
        checkStrLen(ring.id, "ring.id");
        checkStrLen(ring.name, "ring.name");
        if (!ringMap.emplace(ring.id, &ring).second) {
            res.AddError("Duplicate ring ID: " + ring.id);
        }
        if (ring.sectorCount != 6 && ring.sectorCount != 8) {
            res.AddError("Ring '" + ring.id + "' must have 6 or 8 sectors.");
        }
        if (ring.sectorActionIds.size() != ring.sectorCount) {
            res.AddError("Ring '" + ring.id + "' sectorActionIds length must match sectorCount.");
        }
    }

    // Validate action references and child-ring nesting rules (max 1 child level, no cycles)
    for (const auto& ring : doc.rings) {
        for (const auto& actId : ring.sectorActionIds) {
            if (actId.empty()) continue;
            auto it = actionMap.find(actId);
            if (it == actionMap.end()) {
                res.AddError("Ring '" + ring.id + "' references unknown action ID '" + actId + "'.");
                continue;
            }
            const ActionDefinition* act = it->second;
            if (act->type == ActionType::OpenChildRing) {
                if (ring.isChildRing) {
                    res.AddError("Child ring '" + ring.id + "' cannot link to another child ring ('" + act->id + "'); deeper nesting beyond 1 child level is disallowed.");
                }
                if (act->childRingId == ring.id) {
                    res.AddError("Ring '" + ring.id + "' contains self-referencing child ring link.");
                }
                auto childIt = ringMap.find(act->childRingId);
                if (childIt == ringMap.end()) {
                    res.AddError("Action '" + act->id + "' references non-existent childRingId '" + act->childRingId + "'.");
                } else if (!childIt->second->isChildRing) {
                    res.AddError("Action '" + act->id + "' targets root ring '" + act->childRingId + "' as a child ring (cycle/root link disallowed).");
                }
            }
        }
    }

    // Validate profiles
    std::unordered_set<std::string> profileIds;
    bool foundActiveProfile = false;
    for (const auto& prof : doc.profiles) {
        if (prof.id.empty()) {
            res.AddError("Profile ID cannot be empty.");
            continue;
        }
        checkStrLen(prof.id, "profile.id");
        checkStrLen(prof.name, "profile.name");
        if (!profileIds.insert(prof.id).second) {
            res.AddError("Duplicate profile ID: " + prof.id);
        }
        if (prof.id == doc.activeProfileId) {
            foundActiveProfile = true;
        }
        auto rIt = ringMap.find(prof.rootRingId);
        if (rIt == ringMap.end()) {
            res.AddError("Profile '" + prof.id + "' references unknown rootRingId '" + prof.rootRingId + "'.");
        } else if (rIt->second->isChildRing) {
            res.AddError("Profile '" + prof.id + "' rootRingId '" + prof.rootRingId + "' cannot be marked isChildRing.");
        }
    }

    if (!doc.profiles.empty() && !foundActiveProfile) {
        res.AddError("activeProfileId '" + doc.activeProfileId + "' does not match any profile.");
    }

    return res;
}

std::string ConfigStore::SerializeToJson(const ConfigDocument& doc, bool pretty) {
    json j;
    j["schemaVersion"] = doc.schemaVersion;
    j["revision"] = doc.revision;
    j["activeProfileId"] = doc.activeProfileId;

    const auto& p = doc.preferences;
    j["preferences"] = {
        {"enabled", p.enabled},
        {"launchAtStartup", p.launchAtStartup},
        {"showTrayIcon", p.showTrayIcon},
        {"completedTutorial", p.completedTutorial},
        {"activationMode", p.activationMode},
        {"interactionMode", p.interactionMode},
        {"ringDiameterDip", p.ringDiameterDip},
        {"defaultSectorCount", p.defaultSectorCount},
        {"deadZoneRadiusDip", p.deadZoneRadiusDip},
        {"childRingDwellMs", p.childRingDwellMs},
        {"sectorGapDip", p.sectorGapDip},
        {"theme", p.theme},
        {"accentColorHex", p.accentColorHex},
        {"glassOpacity", p.glassOpacity},
        {"iconSizeDip", p.iconSizeDip},
        {"motionPreset", p.motionPreset},
        {"animationSpeedMultiplier", p.animationSpeedMultiplier},
        {"reducedMotionOverride", p.reducedMotionOverride},
        {"reducedTransparencyOverride", p.reducedTransparencyOverride},
        {"qualityPreset", p.qualityPreset},
        {"efficiencyFrameCapHz", p.efficiencyFrameCapHz},
        {"preferredAdapterLuid", p.preferredAdapterLuid},
        {"redactPathsInDiagnostics", p.redactPathsInDiagnostics},
        {"foregroundExclusions", p.foregroundExclusions}
    };

    json profilesArr = json::array();
    for (const auto& prof : doc.profiles) {
        profilesArr.push_back({
            {"id", prof.id},
            {"name", prof.name},
            {"rootRingId", prof.rootRingId},
            {"exclusions", prof.exclusions}
        });
    }
    j["profiles"] = std::move(profilesArr);

    json ringsArr = json::array();
    for (const auto& ring : doc.rings) {
        ringsArr.push_back({
            {"id", ring.id},
            {"name", ring.name},
            {"sectorCount", ring.sectorCount},
            {"isChildRing", ring.isChildRing},
            {"parentRingId", ring.parentRingId},
            {"sectorActionIds", ring.sectorActionIds}
        });
    }
    j["rings"] = std::move(ringsArr);

    json actionsArr = json::array();
    for (const auto& act : doc.actions) {
        actionsArr.push_back({
            {"id", act.id},
            {"type", ActionTypeToString(act.type)},
            {"label", act.label},
            {"target", act.target},
            {"arguments", act.arguments},
            {"workingDirectory", act.workingDirectory},
            {"shortcutChord", act.shortcutChord},
            {"windowsActionName", act.windowsActionName},
            {"childRingId", act.childRingId},
            {"iconGlyph", act.iconGlyph},
            {"approved", act.approved},
            {"enabled", act.enabled}
        });
    }
    j["actions"] = std::move(actionsArr);

    return pretty ? j.dump(2) : j.dump();
}

bool ConfigStore::DeserializeFromJson(std::string_view jsonText, ConfigDocument& outDoc, ValidationResult& outValidation) {
    outValidation = ValidationResult{};
    if (jsonText.empty()) {
        outValidation.AddError("Configuration JSON is empty.");
        return false;
    }
    if (jsonText.size() > kMaxConfigFileBytes) {
        outValidation.AddError("Configuration JSON exceeds 512 KiB size limit.");
        return false;
    }

    json j;
    try {
        j = json::parse(jsonText.begin(), jsonText.end());
    } catch (const std::exception& ex) {
        outValidation.AddError(std::string("Malformed JSON: ") + ex.what());
        return false;
    }

    if (!j.is_object()) {
        outValidation.AddError("Root JSON element must be an object.");
        return false;
    }

    try {
        ConfigDocument doc = CreateDefaultConfig();
        doc.schemaVersion = j.value("schemaVersion", kCurrentConfigSchemaVersion);
        doc.revision = j.value("revision", uint64_t{1});
        doc.activeProfileId = j.value("activeProfileId", std::string("profile_default"));

        if (j.contains("preferences") && j["preferences"].is_object()) {
            const auto& pj = j["preferences"];
            auto& p = doc.preferences;
            p.enabled                     = pj.value("enabled", p.enabled);
            p.launchAtStartup             = pj.value("launchAtStartup", p.launchAtStartup);
            p.showTrayIcon                = pj.value("showTrayIcon", p.showTrayIcon);
            p.completedTutorial           = pj.value("completedTutorial", p.completedTutorial);
            p.activationMode              = pj.value("activationMode", p.activationMode);
            p.interactionMode             = pj.value("interactionMode", p.interactionMode);
            p.ringDiameterDip             = pj.value("ringDiameterDip", p.ringDiameterDip);
            p.defaultSectorCount          = pj.value("defaultSectorCount", p.defaultSectorCount);
            p.deadZoneRadiusDip           = pj.value("deadZoneRadiusDip", p.deadZoneRadiusDip);
            p.childRingDwellMs            = pj.value("childRingDwellMs", p.childRingDwellMs);
            p.sectorGapDip                = pj.value("sectorGapDip", p.sectorGapDip);
            p.theme                       = pj.value("theme", p.theme);
            p.accentColorHex              = pj.value("accentColorHex", p.accentColorHex);
            p.glassOpacity                = pj.value("glassOpacity", p.glassOpacity);
            p.iconSizeDip                 = pj.value("iconSizeDip", p.iconSizeDip);
            p.motionPreset                = pj.value("motionPreset", p.motionPreset);
            p.animationSpeedMultiplier    = pj.value("animationSpeedMultiplier", p.animationSpeedMultiplier);
            p.reducedMotionOverride       = pj.value("reducedMotionOverride", p.reducedMotionOverride);
            p.reducedTransparencyOverride = pj.value("reducedTransparencyOverride", p.reducedTransparencyOverride);
            p.qualityPreset               = pj.value("qualityPreset", p.qualityPreset);
            p.efficiencyFrameCapHz        = pj.value("efficiencyFrameCapHz", p.efficiencyFrameCapHz);
            p.preferredAdapterLuid        = pj.value("preferredAdapterLuid", p.preferredAdapterLuid);
            p.redactPathsInDiagnostics    = pj.value("redactPathsInDiagnostics", p.redactPathsInDiagnostics);
            if (pj.contains("foregroundExclusions") && pj["foregroundExclusions"].is_array()) {
                p.foregroundExclusions = pj["foregroundExclusions"].get<std::vector<std::string>>();
            }
        }

        if (j.contains("profiles") && j["profiles"].is_array()) {
            doc.profiles.clear();
            for (const auto& item : j["profiles"]) {
                Profile prof;
                prof.id = item.value("id", "");
                prof.name = item.value("name", "");
                prof.rootRingId = item.value("rootRingId", "");
                if (item.contains("exclusions") && item["exclusions"].is_array()) {
                    prof.exclusions = item["exclusions"].get<std::vector<std::string>>();
                }
                doc.profiles.push_back(std::move(prof));
            }
        }

        if (j.contains("rings") && j["rings"].is_array()) {
            doc.rings.clear();
            for (const auto& item : j["rings"]) {
                RadialRing ring;
                ring.id = item.value("id", "");
                ring.name = item.value("name", "");
                ring.sectorCount = item.value("sectorCount", 6u);
                ring.isChildRing = item.value("isChildRing", false);
                ring.parentRingId = item.value("parentRingId", "");
                if (item.contains("sectorActionIds") && item["sectorActionIds"].is_array()) {
                    ring.sectorActionIds = item["sectorActionIds"].get<std::vector<std::string>>();
                }
                doc.rings.push_back(std::move(ring));
            }
        }

        if (j.contains("actions") && j["actions"].is_array()) {
            doc.actions.clear();
            for (const auto& item : j["actions"]) {
                ActionDefinition act;
                act.id = item.value("id", "");
                std::string typeStr = item.value("type", "None");
                auto parsedType = StringToActionType(typeStr);
                if (!parsedType.has_value()) {
                    outValidation.AddError("Unknown action type '" + typeStr + "' on action '" + act.id + "'.");
                    act.type = ActionType::None;
                } else {
                    act.type = *parsedType;
                }
                act.label = item.value("label", "");
                act.target = item.value("target", "");
                act.arguments = item.value("arguments", "");
                act.workingDirectory = item.value("workingDirectory", "");
                act.shortcutChord = item.value("shortcutChord", "");
                act.windowsActionName = item.value("windowsActionName", "");
                act.childRingId = item.value("childRingId", "");
                act.iconGlyph = item.value("iconGlyph", "");
                act.approved = item.value("approved", true);
                act.enabled = item.value("enabled", true);
                doc.actions.push_back(std::move(act));
            }
        }

        ValidationResult structVal = ValidateConfig(doc);
        for (auto& err : structVal.errors) outValidation.AddError(std::move(err));
        for (auto& warn : structVal.warnings) outValidation.AddWarning(std::move(warn));

        if (!outValidation.valid) {
            return false;
        }
        outDoc = std::move(doc);
        return true;
    } catch (const std::exception& ex) {
        outValidation.AddError(std::string("Configuration parse exception: ") + ex.what());
        return false;
    }
}

bool ConfigStore::ImportConfigFromJson(std::string_view jsonText, ConfigDocument& outDoc, ValidationResult& outValidation) {
    if (!DeserializeFromJson(jsonText, outDoc, outValidation)) {
        return false;
    }
    // Security policy: imported executable/shortcut/file actions must remain unapproved
    // until the user explicitly reviews and enables them.
    for (auto& act : outDoc.actions) {
        if (act.type == ActionType::LaunchApp || act.type == ActionType::OpenFile) {
            act.approved = false;
            outValidation.AddWarning("Imported action '" + act.label + "' requires explicit user approval before execution.");
        }
    }
    return true;
}

static bool ReadFileBounded(const std::filesystem::path& path, std::string& outText, std::string& outError) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        outError = "File does not exist.";
        return false;
    }
    auto fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize > kMaxConfigFileBytes) {
        outError = "File exceeds maximum size bound.";
        return false;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        outError = "Unable to open file for reading.";
        return false;
    }
    outText.resize(static_cast<size_t>(fileSize));
    if (fileSize > 0) {
        ifs.read(outText.data(), static_cast<std::streamsize>(fileSize));
        if (!ifs) {
            outError = "Failed reading file bytes.";
            return false;
        }
    }
    return true;
}

static bool WriteFileAtomic(const std::filesystem::path& targetPath, std::string_view content) {
    std::filesystem::path tmpPath = targetPath;
    tmpPath += L".tmp";

    {
        HANDLE hTmp = CreateFileW(
            tmpPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (hTmp == INVALID_HANDLE_VALUE) {
            return false;
        }
        UniqueHandle guard(hTmp);
        DWORD written = 0;
        const DWORD toWrite = static_cast<DWORD>(content.size());
        if (toWrite > 0 && (!WriteFile(hTmp, content.data(), toWrite, &written, nullptr) || written != toWrite)) {
            return false;
        }
        FlushFileBuffers(hTmp);
    }

    if (std::filesystem::exists(targetPath)) {
        if (ReplaceFileW(targetPath.c_str(), tmpPath.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            return true;
        }
    }
    return MoveFileExW(tmpPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

ConfigDocument ConfigStore::LoadOrRecover(ValidationResult* outLoadReport) {
    ValidationResult report;
    std::error_code ec;
    std::filesystem::create_directories(storageDir_, ec);

    const auto primaryPath = GetConfigFilePath();
    const auto backupPath  = GetBackupFilePath();

    std::string rawPrimary;
    std::string readErr;
    if (ReadFileBounded(primaryPath, rawPrimary, readErr)) {
        ConfigDocument doc;
        ValidationResult val;
        if (DeserializeFromJson(rawPrimary, doc, val)) {
            // Primary config is valid; ensure last-known-good backup exists
            if (!std::filesystem::exists(backupPath, ec)) {
                WriteFileAtomic(backupPath, rawPrimary);
            }
            AcquireSRWLockExclusive(&lock_);
            snapshot_ = std::make_shared<const ConfigDocument>(doc);
            ReleaseSRWLockExclusive(&lock_);
            if (outLoadReport) *outLoadReport = std::move(val);
            return doc;
        }
        report.AddWarning("Primary config.json was malformed or invalid (" +
            (val.errors.empty() ? std::string("unknown") : val.errors.front()) +
            "); attempting recovery from config.last_good.json.");
    }

    // Attempt recovery from last-known-good backup without clobbering it
    std::string rawBackup;
    if (ReadFileBounded(backupPath, rawBackup, readErr)) {
        ConfigDocument backupDoc;
        ValidationResult backupVal;
        if (DeserializeFromJson(rawBackup, backupDoc, backupVal)) {
            WriteFileAtomic(primaryPath, rawBackup);
            report.AddWarning("Recovered configuration from config.last_good.json.");
            AcquireSRWLockExclusive(&lock_);
            snapshot_ = std::make_shared<const ConfigDocument>(backupDoc);
            ReleaseSRWLockExclusive(&lock_);
            if (outLoadReport) *outLoadReport = std::move(report);
            return backupDoc;
        }
    }

    // Fallback to clean default configuration and persist atomically
    ConfigDocument defaultDoc = CreateDefaultConfig();
    std::string serialized = SerializeToJson(defaultDoc, true);
    WriteFileAtomic(primaryPath, serialized);
    if (!std::filesystem::exists(backupPath, ec)) {
        WriteFileAtomic(backupPath, serialized);
    }
    AcquireSRWLockExclusive(&lock_);
    snapshot_ = std::make_shared<const ConfigDocument>(defaultDoc);
    ReleaseSRWLockExclusive(&lock_);
    if (outLoadReport) *outLoadReport = std::move(report);
    return defaultDoc;
}

ValidationResult ConfigStore::SaveAtomic(ConfigDocument newDoc) {
    ValidationResult val = ValidateConfig(newDoc);
    if (!val.valid) {
        return val;
    }

    AcquireSRWLockShared(&lock_);
    uint64_t currentRev = snapshot_ ? snapshot_->revision : 0;
    ReleaseSRWLockShared(&lock_);
    newDoc.revision = std::max(newDoc.revision, currentRev + 1);

    std::string serialized = SerializeToJson(newDoc, true);
    if (serialized.size() > kMaxConfigFileBytes) {
        val.AddError("Serialized configuration exceeds 512 KiB size limit.");
        return val;
    }

    std::error_code ec;
    std::filesystem::create_directories(storageDir_, ec);

    if (!WriteFileAtomic(GetConfigFilePath(), serialized)) {
        val.AddError("Failed to atomically write config.json.");
        return val;
    }
    // Update last-known-good backup after verified write
    WriteFileAtomic(GetBackupFilePath(), serialized);

    AcquireSRWLockExclusive(&lock_);
    snapshot_ = std::make_shared<const ConfigDocument>(newDoc);
    ReleaseSRWLockExclusive(&lock_);
    return val;
}

std::shared_ptr<const ConfigDocument> ConfigStore::GetSnapshot() const noexcept {
    AcquireSRWLockShared(&lock_);
    auto copy = snapshot_;
    ReleaseSRWLockShared(&lock_);
    return copy;
}

} // namespace keychain
