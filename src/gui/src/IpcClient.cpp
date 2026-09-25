/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "IpcClient.h"
#include <QTcpSocket>
#include <QHostAddress>
#include <iostream>
#include <QTimer>
#if defined(Q_OS_WIN)
#include "WindowsServiceStarter.h"
#endif
#include "IpcReader.h"
#include "Ipc.h"
#include <QDataStream>

IpcClient::IpcClient() :
m_ReaderStarted(false),
m_Enabled(false),
m_RetryAttempt(0),
m_PendingElevate(ElevateAsNeeded)
#if defined(Q_OS_WIN)
, m_ServiceStarter(new WindowsServiceStarter(this))
#endif
{
    m_Socket = new QTcpSocket(this);
    connect(m_Socket, &QTcpSocket::connected, this, &IpcClient::connected);
    connect(m_Socket, &QTcpSocket::errorOccurred, this, &IpcClient::error);
    connect(m_Socket, &QTcpSocket::disconnected, this, &IpcClient::disconnected);
    m_RetryTimer.setSingleShot(true);
    connect(&m_RetryTimer, &QTimer::timeout, this, &IpcClient::retryConnect);
#if defined(Q_OS_WIN)
    connect(m_ServiceStarter, &WindowsServiceStarter::ready, this, &IpcClient::retryConnect);
    connect(m_ServiceStarter, &WindowsServiceStarter::failed, this, [this](const QString& message) {
        Q_EMIT errorMessage(message);
    });
#endif

    m_Reader = new IpcReader(m_Socket);
    connect(m_Reader, &IpcReader::readLogLine, this, &IpcClient::handleReadLogLine);
}

IpcClient::~IpcClient()
{
}

void IpcClient::connected()
{
    m_RetryTimer.stop();
    m_RetryAttempt = 0;
    sendHello();
    Q_EMIT infoMessage("connection established");
    if (!m_PendingCommand.isNull()) {
        const QString command = m_PendingCommand;
        m_PendingCommand = QString();
        sendCommand(command, m_PendingElevate);
    }
}

void IpcClient::connectToHost()
{
    m_Enabled = true;
#if defined(Q_OS_WIN)
    m_ServiceStarter->ensureRunning();
#else
    retryConnect();
#endif

    if (!m_ReaderStarted) {
        m_Reader->start();
        m_ReaderStarted = true;
    }
}

void IpcClient::disconnectFromHost()
{
    Q_EMIT infoMessage("service disconnect");
    m_Reader->stop();
    m_RetryTimer.stop();
    m_Enabled = false;
    m_Socket->close();
}

void IpcClient::error(QAbstractSocket::SocketError error)
{
    QString text;
    switch (error) {
        case 0: text = "connection refused"; break;
        case 1: text = "remote host closed"; break;
        default: text = QString("code=%1").arg(error); break;
    }

    if (m_RetryAttempt == 0) {
        Q_EMIT infoMessage(QString("waiting for InputLeap service (%1)").arg(text));
    }
    static const int delays[] = {100, 250, 500, 1000, 2000, 5000};
    if (m_RetryAttempt >= 10) {
        m_Enabled = false;
        Q_EMIT errorMessage(QString("ipc connection failed after bounded startup retries (%1)").arg(text));
        return;
    }
    const int index = qMin(m_RetryAttempt, static_cast<int>(sizeof(delays) / sizeof(delays[0])) - 1);
    ++m_RetryAttempt;
    if (m_Enabled && !m_RetryTimer.isActive()) {
        m_RetryTimer.start(delays[index]);
    }
}

void IpcClient::retryConnect()
{
    if (m_Enabled) {
        if (m_Socket->state() == QAbstractSocket::UnconnectedState) {
            m_Socket->connectToHost(QHostAddress(QHostAddress::LocalHost), IPC_PORT);
        }
    }
}

void IpcClient::disconnected()
{
    if (!m_Enabled) return;
#if defined(Q_OS_WIN)
    m_ServiceStarter->ensureRunning();
#else
    if (!m_RetryTimer.isActive()) m_RetryTimer.start(250);
#endif
}

void IpcClient::sendHello()
{
    QDataStream stream(m_Socket);
    stream.writeRawData(kIpcMsgHello, 4);

    char typeBuf[1];
    typeBuf[0] = kIpcClientGui;
    stream.writeRawData(typeBuf, 1);
}

void IpcClient::sendCommand(const QString& command, ElevateMode const elevate)
{
    if (m_Socket->state() != QAbstractSocket::ConnectedState) {
        m_PendingCommand = command;
        m_PendingElevate = elevate;
        return;
    }
    QDataStream stream(m_Socket);

    stream.writeRawData(kIpcMsgCommand, 4);

    std::string stdStringCommand = command.toStdString();
    const char* charCommand = stdStringCommand.c_str();
    int length = static_cast<int>(strlen(charCommand));

    char lenBuf[4];
    intToBytes(length, lenBuf, 4);
    stream.writeRawData(lenBuf, 4);
    stream.writeRawData(charCommand, length);

    char elevateBuf[1];
    // Refer to enum ElevateMode documentation for why this flag is mapped this way
    elevateBuf[0] = (elevate == ElevateAlways) ? 1 : 0;
    stream.writeRawData(elevateBuf, 1);
}

void IpcClient::handleReadLogLine(const QString& text)
{
    Q_EMIT readLogLine(text);
}

// TODO: qt must have a built in way of converting int to bytes.
void IpcClient::intToBytes(int value, char *buffer, int size)
{
    if (size == 1) {
        buffer[0] = value & 0xff;
    }
    else if (size == 2) {
        buffer[0] = (value >> 8) & 0xff;
        buffer[1] = value & 0xff;
    }
    else if (size == 4) {
        buffer[0] = (value >> 24) & 0xff;
        buffer[1] = (value >> 16) & 0xff;
        buffer[2] = (value >> 8) & 0xff;
        buffer[3] = value & 0xff;
    }
    else {
        // TODO: other sizes, if needed.
    }
}
