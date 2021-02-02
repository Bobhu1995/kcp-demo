#pragma once
#include <arpa/inet.h>
#include "ikcp.h"

struct KCPAddr
{
    KCPAddr(const sockaddr_in& sockaddr, socklen_t sock_len) : sockaddr(sockaddr),
        sock_len(sock_len){}

    sockaddr_in sockaddr;
    socklen_t sock_len;
};

class KCPServer;
class KCPSession;

KCPSession* NewKCPSession(KCPServer* server, const KCPAddr& addr, int conv, IUINT64 current);

class KCPRingBuffer
{
public:
    static const int BUFFER_SIZE = 1 * 64 * 1024;

public:
	KCPRingBuffer();
	~KCPRingBuffer();

    void Clear();
    int GetUsedSize() const;
    int GetFreeSize() const;
    int Write(const char* src, int len);
    int Read(char* dst, int len);
    bool ReadNoPop(char* dst, int len) const;
    int GetBufferSize() const;

private:
    int read_pos;
    int write_pos;
    bool is_empty;
    bool is_full;
    char ring_buffer[BUFFER_SIZE];
};


class KCPSession
{
public:
    KCPSession(KCPServer* server, const KCPAddr& addr, IUINT64 current);
    ~KCPSession();

    void Update(IUINT32 current);
    int Send(const char* data, int len);
    IUINT64 LastActiveTime() const;
    void SetKCP(ikcpcb* kcp);
public:
    void KCPInput(const sockaddr_in& sockaddr, const socklen_t socklen, const char* data, long sz,
        IUINT64 current);
    void Output(const char* buf, int len);

private:
    void Clear();

    ikcpcb* kcp_cb;
    KCPServer* kcp_server;
    KCPAddr kcp_addr;
    IUINT64 last_active_time;
    KCPRingBuffer recv_buffer;
};
