#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace inputleap {

class MSWindowsDisplayController final {
public:
    MSWindowsDisplayController(std::string clientName, std::string displayName);
    ~MSWindowsDisplayController();

    void clientConnected(const std::string& name);
    void clientDisconnected(const std::string& name);

private:
    enum class Request { None, Disable, Restore, Stop };
    void run();
    void disableDisplay();
    void restoreDisplay();
    bool captureTopology();
    bool loadRecoverySnapshot();
    bool saveRecoverySnapshot() const;
    void clearRecoverySnapshot() const;
    std::wstring recoveryPath() const;

    std::string client_name_;
    std::wstring display_name_;
    std::mutex mutex_;
    std::condition_variable changed_;
    Request request_ = Request::None;
    std::thread worker_;
    bool disabled_ = false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths_;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes_;
};

}
