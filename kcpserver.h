#pragma once
#include <sys/time.h>
#include <string>
#include <map>

#include "kcpsession.h"

inline IUINT64 iclock()
{
    struct timeval time;
    gettimeofday(&time, NULL);
    IUINT64 value = ((IUINT64)time.tv_sec) * 1000 + (time.tv_usec / 1000);
    return value;
}

typedef void (*package_recv_cb_func)(int, const char*, int);
typedef void (*session_kick_cb_func)(int);
typedef void (*error_log_reporter)(const char*);
typedef int (*check_package_valid_cb_func)(int, const char*, int);

struct KCPOptions
{
    int port;
    int keep_session_time;
    package_recv_cb_func recv_cb;
    session_kick_cb_func kick_cb;
    error_log_reporter error_reporter;
    check_package_valid_cb_func check_package;

    KCPOptions();
};

class KCPServer
{
public:
    friend class KCPSession;

	KCPServer();
    KCPServer(const KCPOptions& options);
	~KCPServer();

    bool Start();
    void Update();
    bool Send(int conv, const char* data, int len);
    void KickSession(int conv);
    bool SessionExist(int conv) const;
    void SetOption(const KCPOptions& options);
private:
    bool UDPBind();
    void Clear();
    KCPSession* GetSession(int conv);
    void DoOutput(const KCPAddr& addr, const char* data, int len);
    void UDPRead();
    void SessionUpdate();
    void OnKCPRecv(int conv, const char* data, int len);
    void DoErrorLog(const char* fmt, ...);

    KCPOptions kcp_options;
    int kcp_fd;
    std::map<int, KCPSession*> kcp_sessions;
    IUINT64 kcp_current_clock;
};

