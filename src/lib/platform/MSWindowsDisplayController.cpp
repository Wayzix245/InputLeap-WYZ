#include "platform/MSWindowsDisplayController.h"
#include "base/Log.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace {

constexpr UINT32 kRecoveryMagic = 0x494c4450;
constexpr UINT32 kRecoveryVersion = 1;

bool writeAll(HANDLE file, const void* data, DWORD size)
{
    if (size == 0) return true;
    DWORD written = 0;
    return WriteFile(file, data, size, &written, nullptr) != FALSE && written == size;
}

bool readAll(HANDLE file, void* data, DWORD size)
{
    if (size == 0) return true;
    DWORD read = 0;
    return ReadFile(file, data, size, &read, nullptr) != FALSE && read == size;
}

bool compactDisplayConfig(std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                          const std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
                          std::vector<DISPLAYCONFIG_MODE_INFO>& compactModes)
{
    std::unordered_map<UINT32, UINT32> remappedModes;
    auto remapMode = [&modes, &compactModes, &remappedModes](UINT32& index) {
        if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) return true;
        if (index >= modes.size()) return false;
        const auto existing = remappedModes.find(index);
        if (existing != remappedModes.end()) {
            index = existing->second;
            return true;
        }
        const UINT32 newIndex = static_cast<UINT32>(compactModes.size());
        compactModes.push_back(modes[index]);
        remappedModes.emplace(index, newIndex);
        index = newIndex;
        return true;
    };

    compactModes.clear();
    compactModes.reserve(modes.size());
    for (auto& path : paths) {
        if (!remapMode(path.sourceInfo.modeInfoIdx) ||
            !remapMode(path.targetInfo.modeInfoIdx)) return false;
    }
    return true;
}

}

namespace inputleap {

MSWindowsDisplayController::MSWindowsDisplayController(std::string clientName,
                                                       std::string displayName) :
    client_name_(std::move(clientName)),
    display_name_(displayName.begin(), displayName.end()),
    worker_()
{
    if (loadRecoverySnapshot()) {
        restoreDisplay();
    }
    worker_ = std::thread([this] { run(); });
}

MSWindowsDisplayController::~MSWindowsDisplayController()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_ = Request::Stop;
    }
    changed_.notify_one();
    worker_.join();
}

void MSWindowsDisplayController::clientConnected(const std::string& name)
{
    if (name != client_name_) return;
    { std::lock_guard<std::mutex> lock(mutex_); request_ = Request::Disable; }
    changed_.notify_one();
}

void MSWindowsDisplayController::clientDisconnected(const std::string& name)
{
    if (name != client_name_) return;
    { std::lock_guard<std::mutex> lock(mutex_); request_ = Request::Restore; }
    changed_.notify_one();
}

void MSWindowsDisplayController::run()
{
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return request_ != Request::None; });
            request = request_;
            request_ = Request::None;
        }
        if (request == Request::Disable) disableDisplay();
        else if (request == Request::Restore) restoreDisplay();
        else if (request == Request::Stop) {
            restoreDisplay();
            return;
        }
    }
}

bool MSWindowsDisplayController::captureTopology()
{
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return false;
    paths_.resize(pathCount);
    modes_.resize(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths_.data(), &modeCount,
                           modes_.data(), nullptr) != ERROR_SUCCESS) return false;
    paths_.resize(pathCount);
    modes_.resize(modeCount);
    return true;
}

void MSWindowsDisplayController::disableDisplay()
{
    if (disabled_ || display_name_.empty() || !captureTopology()) return;
    auto active = paths_;
    active.erase(std::remove_if(active.begin(), active.end(), [this](const auto& path) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        return DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
               display_name_ == source.viewGdiDeviceName;
    }), active.end());
    if (active.size() == paths_.size() || active.empty()) return;

    std::vector<DISPLAYCONFIG_MODE_INFO> activeModes;
    if (!compactDisplayConfig(active, modes_, activeModes)) {
        LOG_ERR("saved display topology contains an invalid mode index");
        return;
    }

    if (!saveRecoverySnapshot()) {
        LOG_ERR("failed to save display topology before disabling display");
        return;
    }
    if (SetDisplayConfig(static_cast<UINT32>(active.size()), active.data(),
                         static_cast<UINT32>(activeModes.size()), activeModes.data(),
                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES) == ERROR_SUCCESS) {
        disabled_ = true;
    } else {
        LOG_ERR("failed to disable configured local display");
        clearRecoverySnapshot();
        paths_.clear(); modes_.clear();
    }
}

void MSWindowsDisplayController::restoreDisplay()
{
    if (!disabled_) return;
    auto restoredPaths = paths_;
    std::vector<DISPLAYCONFIG_MODE_INFO> restoredModes;
    if (!compactDisplayConfig(restoredPaths, modes_, restoredModes)) {
        LOG_ERR("saved display topology contains an invalid mode index");
        clearRecoverySnapshot();
        paths_.clear();
        modes_.clear();
        disabled_ = false;
        return;
    }
    if (SetDisplayConfig(static_cast<UINT32>(restoredPaths.size()), restoredPaths.data(),
                         static_cast<UINT32>(restoredModes.size()), restoredModes.data(),
                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES) == ERROR_SUCCESS) {
        disabled_ = false;
        clearRecoverySnapshot();
        paths_.clear(); modes_.clear();
    } else {
        LOG_ERR("failed to restore saved display topology");
    }
}

std::wstring MSWindowsDisplayController::recoveryPath() const
{
    wchar_t programData[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", programData, MAX_PATH);
    if (!length) length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    const std::wstring root = length ? std::wstring(programData, length) : L"C:\\ProgramData";
    return root + L"\\InputLeap-display-topology.bin";
}

bool MSWindowsDisplayController::saveRecoverySnapshot() const
{
    const std::wstring path = recoveryPath();
    const std::wstring temporaryPath = path + L".tmp";
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const UINT32 pathCount = static_cast<UINT32>(paths_.size());
    const UINT32 modeCount = static_cast<UINT32>(modes_.size());
    const DWORD pathBytes = static_cast<DWORD>(paths_.size() * sizeof(paths_[0]));
    const DWORD modeBytes = static_cast<DWORD>(modes_.size() * sizeof(modes_[0]));
    bool okay = writeAll(file, &kRecoveryMagic, sizeof(kRecoveryMagic)) &&
                writeAll(file, &kRecoveryVersion, sizeof(kRecoveryVersion)) &&
                writeAll(file, &pathCount, sizeof(pathCount)) &&
                writeAll(file, &modeCount, sizeof(modeCount)) &&
                writeAll(file, paths_.data(), pathBytes) &&
                writeAll(file, modes_.data(), modeBytes) &&
                FlushFileBuffers(file) != FALSE;
    if (CloseHandle(file) == FALSE) okay = false;
    if (okay) {
        okay = MoveFileExW(temporaryPath.c_str(), path.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!okay) DeleteFileW(temporaryPath.c_str());
    return okay;
}

bool MSWindowsDisplayController::loadRecoverySnapshot()
{
    HANDLE file = CreateFileW(recoveryPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    UINT32 magic = 0, version = 0, pathCount = 0, modeCount = 0;
    bool okay = readAll(file, &magic, sizeof(magic)) && magic == kRecoveryMagic &&
                readAll(file, &version, sizeof(version)) && version == kRecoveryVersion &&
                readAll(file, &pathCount, sizeof(pathCount)) &&
                readAll(file, &modeCount, sizeof(modeCount)) &&
                pathCount > 0 && pathCount <= 64 && modeCount <= 256;
    if (!okay) {
        CloseHandle(file);
        paths_.clear();
        modes_.clear();
        clearRecoverySnapshot();
        return false;
    }
    paths_.resize(pathCount);
    modes_.resize(modeCount);
    const DWORD pathBytes = static_cast<DWORD>(paths_.size() * sizeof(paths_[0]));
    const DWORD modeBytes = static_cast<DWORD>(modes_.size() * sizeof(modes_[0]));
    okay = readAll(file, paths_.data(), pathBytes) &&
           readAll(file, modes_.data(), modeBytes);
    CloseHandle(file);
    if (!okay) {
        paths_.clear();
        modes_.clear();
        clearRecoverySnapshot();
        return false;
    }
    disabled_ = true;
    return true;
}

void MSWindowsDisplayController::clearRecoverySnapshot() const
{
    const std::wstring path = recoveryPath();
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".tmp").c_str());
}

}
