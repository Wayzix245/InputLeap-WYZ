#pragma once

#include <QObject>
#include <QString>

class QThread;

class WindowsServiceStarter : public QObject
{
    Q_OBJECT
public:
    explicit WindowsServiceStarter(QObject* parent = nullptr);
    ~WindowsServiceStarter() override;

    void ensureRunning();

Q_SIGNALS:
    void ready();
    void failed(const QString& message);

private:
    bool running_ = false;
    QThread* worker_ = nullptr;
};
