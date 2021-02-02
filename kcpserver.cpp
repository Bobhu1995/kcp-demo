#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>

#include "kcpserver.h"

const IUINT32 KCP_HEAD_LENGTH = 24;
const IUINT32 KCP_CHECK_SUM_LENGTH = 4;

KCPOptions::KCPOptions()
{
    port = 9527;
    keep_session_time = 15 * 1000;
    recv_cb = NULL;
    kick_cb = NULL;
    error_reporter = NULL;
    check_package = NULL;
}

KCPServer::KCPServer() : kcp_fd(0),
    kcp_current_clock(0)
{
}

KCPServer::KCPServer(const KCPOptions& options) : kcp_options(options),
    kcp_fd(0),
    kcp_current_clock(0)

{
}

KCPServer::~KCPServer()
{
    Clear();
}

bool KCPServer::Start()
{
    bool ret = false;
    do 
    {
        if (!UDPBind())
        {
            break;
        }
        ret = true;
    } while (false);

    if (!ret)
    {
        Clear();
    }
    return ret;
}

void KCPServer::Update()
{
    kcp_current_clock = iclock();
    UDPRead();
    SessionUpdate();
}

bool KCPServer::Send(int conv, const char* data, int len)
{
    KCPSession* session = GetSession(conv);
    if (NULL == session)
    {
        DoErrorLog("no session(%d) find", conv);
        return false;
    }

    int ret = 0;
    if (ret = session->Send(data, len) < 0)
    {
        DoErrorLog("session(%d) send data failed, ret(%d)", conv, ret);
        return false;
    }

    return true;
}

void KCPServer::KickSession(int conv)
{
    auto it = kcp_sessions.find(conv);
    if (it == kcp_sessions.end()) return;
    
    delete it->second;
    kcp_sessions.erase(it);
}

bool KCPServer::SessionExist(int conv) const
{
    return kcp_sessions.find(conv) != kcp_sessions.end();
}

void KCPServer::SetOption(const KCPOptions& options)
{
    kcp_options = options;
}

bool KCPServer::UDPBind()
{
    sockaddr_in server_addr;
    kcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (kcp_fd < 0)
    {
        DoErrorLog("call socket error:%s", strerror(errno));
        return false;
    }

    int flag = fcntl(kcp_fd, F_GETFL, 0);
    flag |= O_NONBLOCK;
    if (-1 == fcntl(kcp_fd, F_SETFL, flag))
    {
        DoErrorLog("set socket non block error:%s", strerror(errno));
        return false;
    }

    int opt = 1;
    if (0 != setsockopt(kcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        DoErrorLog("set socket reuse addr error:%s", strerror(errno));
        return false;
    }

    int val = 10 * 1024 * 1024;
    if (0 != setsockopt(kcp_fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)))
    {
        DoErrorLog("set socket recv buf error:%s", strerror(errno));
        return false;
    }

    if (0 != setsockopt(kcp_fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)))
    {
        DoErrorLog("set socket send buf error:%s", strerror(errno));
        return false;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(kcp_options.port);
    if (0 != bind(kcp_fd, (const sockaddr*)&server_addr, sizeof(server_addr)))
    {
        DoErrorLog("call bind error:%s", strerror(errno));
        return false;
    }

    return true;
}

void KCPServer::UDPRead()
{
    assert(kcp_fd > 0);

    static char buf[64 * 1024];
    int offset = (NULL != kcp_options.check_package) ? KCP_CHECK_SUM_LENGTH : 0;
    do
    {
        sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);
        memset(&cliaddr, 0, sizeof(cliaddr));
        ssize_t n = recvfrom(kcp_fd, buf, sizeof(buf), 0, (sockaddr*)&cliaddr, &len);
        if (n < 0)
        {
            if (EAGAIN != errno && EINTR != errno)
            {
                DoErrorLog("call recv from error(%d):%s", errno, strerror(errno));
            }
            break;
        }

        if (n < KCP_HEAD_LENGTH + offset)
        {
            DoErrorLog("kcp package len(%d) invalid", n);
            break;
        }

        int conv = ikcp_getconv(&buf[offset]);
        int ret = 0;
        if (offset > 0 && 0 != (ret = kcp_options.check_package(conv, buf, KCP_HEAD_LENGTH + offset)))
        {
            DoErrorLog("kcp package check sum(%d) invalid", ret);
            break;
        }

        KCPSession* session = GetSession(conv);
        if (NULL == session)
        {
            session = NewKCPSession(this, KCPAddr(cliaddr, len), conv, kcp_current_clock);
            kcp_sessions[conv] = session;
        }
        assert(NULL != session);
        session->KCPInput(cliaddr, len, &buf[offset], n - offset, kcp_current_clock);
    } while (true);
}

void KCPServer::Clear()
{
    kcp_fd = 0;
    for (auto it = kcp_sessions.begin(); it != kcp_sessions.end(); ++it)
    {
        delete it->second;
    }
    kcp_sessions.clear();
}

KCPSession* KCPServer::GetSession(int conv)
{
    auto it = kcp_sessions.find(conv);
    if (it != kcp_sessions.end())
    {
        return it->second;
    }
    return NULL;
}
 
void KCPServer::DoOutput(const KCPAddr& addr, const char* data, int len)
{
    assert(kcp_fd > 0);
    if (-1 == sendto(kcp_fd, data, len, 0, (sockaddr*)&addr.sockaddr, addr.sock_len))
    {
        DoErrorLog("udp send data size(%d) to address(%s) port(%d) error:%s",
            len, inet_ntoa(addr.sockaddr.sin_addr), ntohs(addr.sockaddr.sin_port),
            strerror(errno));
        return;
    }
}

void KCPServer::SessionUpdate()
{
    IUINT32 current = kcp_current_clock & 0xfffffffflu;
    for (auto it = kcp_sessions.begin(); it != kcp_sessions.end();)
    {
        KCPSession* session = it->second;
        if (kcp_options.keep_session_time > 0 && kcp_current_clock > session->LastActiveTime() + kcp_options.keep_session_time)
        {
            DoErrorLog("conv(%d) timeout, kick it", it->first);
            if (NULL != kcp_options.kick_cb)
            {
                kcp_options.kick_cb(it->first);
            }
            delete session;
            kcp_sessions.erase(it++);
            continue;
        }
        it++;
        session->Update(current);
    }
}

void KCPServer::OnKCPRecv(int conv, const char* data, int len)
{
    if (NULL != kcp_options.recv_cb)
    {
        kcp_options.recv_cb(conv, data, len);
    }
}

void KCPServer::DoErrorLog(const char* fmt, ...)
{
    if (NULL == kcp_options.error_reporter) return;

    static char buffer[1024];
    va_list argptr;
    va_start(argptr, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, argptr);
    va_end(argptr);
    kcp_options.error_reporter(buffer);
}