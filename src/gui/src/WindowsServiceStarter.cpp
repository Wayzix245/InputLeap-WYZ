#include "WindowsServiceStarter.h"

#include <QThread>

#if defined(Q_OS_WIN)
#include <windows.h>

namespace {
struct PendingServiceNotification {
    SERVICE_NOTIFY_2W notification{};
    HANDLE event = nullptr;
};

class ServiceStartThread final : public QThread
{
    Q_OBJECT
public:
    using QThread::QThread;

Q_SIGNALS:
    void serviceReady();
    void serviceFailed(const QString& message);

protected:
    static VOID CALLBACK statusChanged(PVOID parameter)
    {
        auto* notification = static_cast<SERVICE_NOTIFY_2W*>(parameter);
        auto* pending = static_cast<PendingServiceNotification*>(notification->pContext);
        SetEvent(pending->event);
    }

    void run() override
    {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) {
            Q_EMIT serviceFailed(QStringLiteral("cannot open Windows service manager (%1)").arg(GetLastError()));
            return;
        }
        SC_HANDLE service = OpenServiceW(manager, L"InputLeap", SERVICE_QUERY_STATUS | SERVICE_START);
        if (!service) {
            const DWORD error = GetLastError();
            CloseServiceHandle(manager);
            Q_EMIT serviceFailed(QStringLiteral("InputLeap service is unavailable (%1)").arg(error));
            return;
        }

        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
            const DWORD error = GetLastError();
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            Q_EMIT serviceFailed(QStringLiteral("cannot query InputLeap service (%1)").arg(error));
            return;
        }

        if (status.dwCurrentState == SERVICE_STOPPED &&
            !StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            const DWORD error = GetLastError();
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            Q_EMIT serviceFailed(QStringLiteral("cannot start InputLeap service (%1)").arg(error));
            return;
        }

        const ULONGLONG deadline = GetTickCount64() + 15000;
        while (status.dwCurrentState != SERVICE_RUNNING && GetTickCount64() < deadline) {
            auto* pending = new PendingServiceNotification;
            pending->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!pending->event) {
                delete pending;
                break;
            }
            pending->notification.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
            pending->notification.pfnNotifyCallback = &ServiceStartThread::statusChanged;
            pending->notification.pContext = pending;
            const DWORD result = NotifyServiceStatusChangeW(
                service, SERVICE_NOTIFY_RUNNING | SERVICE_NOTIFY_STOPPED,
                &pending->notification);
            if (result != ERROR_SUCCESS) {
                CloseHandle(pending->event);
                delete pending;
                break;
            }
            const DWORD waitResult = WaitForSingleObject(pending->event, 15000);
            if (waitResult != WAIT_OBJECT_0) {
                // Closing the service handle cancels the outstanding notification. The callback
                // is allowed to finish before its event and storage are released.
                CloseServiceHandle(service);
                service = nullptr;
                WaitForSingleObject(pending->event, INFINITE);
                CloseHandle(pending->event);
                delete pending;
                break;
            }
            CloseHandle(pending->event);
            delete pending;
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                      reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
                break;
            }
            if (status.dwCurrentState == SERVICE_STOPPED &&
                !StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                break;
            }
        }

        if (service) CloseServiceHandle(service);
        CloseServiceHandle(manager);
        if (status.dwCurrentState == SERVICE_RUNNING) {
            Q_EMIT serviceReady();
        } else {
            Q_EMIT serviceFailed(QStringLiteral("InputLeap service did not become ready within 15 seconds"));
        }
    }
};
}
#endif

WindowsServiceStarter::WindowsServiceStarter(QObject* parent) : QObject(parent) {}
WindowsServiceStarter::~WindowsServiceStarter()
{
    if (worker_) {
        disconnect(worker_, nullptr, this, nullptr);
        worker_ = nullptr;
    }
}

void WindowsServiceStarter::ensureRunning()
{
#if defined(Q_OS_WIN)
    if (running_) {
        return;
    }
    running_ = true;
    auto* thread = new ServiceStartThread;
    worker_ = thread;
    connect(thread, &ServiceStartThread::serviceReady, this, [this]() {
        running_ = false;
        Q_EMIT ready();
    });
    connect(thread, &ServiceStartThread::serviceFailed, this, [this](const QString& message) {
        running_ = false;
        Q_EMIT failed(message);
    });
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (worker_ == thread) worker_ = nullptr;
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
#else
    Q_EMIT ready();
#endif
}

#include "WindowsServiceStarter.moc"
