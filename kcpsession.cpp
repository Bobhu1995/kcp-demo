#include <string.h>
#include "kcpserver.h"
#include "kcpsession.h"

const int kcp_max_package_size = 64 * 1024; //64K
const int kcp_package_len_size = 4; //4B

int32_t kcp_output(const char* buf, int len, ikcpcb* kcp, void* ptr)
{
    assert(ptr != NULL);
    KCPSession* session = static_cast<KCPSession*>(ptr);
    session->Output(buf, len);
    return 0;
}

ikcpcb* NewKCP(int conv, KCPSession* session)
{
    ikcpcb* kcp = ikcp_create(conv, (void*)session);
    assert(NULL != kcp);
    ikcp_setoutput(kcp, kcp_output);
    ikcp_nodelay(kcp, 1, 20, 2, 1);
    ikcp_setmtu(kcp, 128 * 3);
    ikcp_wndsize(kcp, 64, 64);
    return kcp;
}

KCPSession* NewKCPSession(KCPServer* server, const KCPAddr& addr, int conv, IUINT64 current)
{
    KCPSession* session = new KCPSession(server, addr, current);
    ikcpcb* kcp = NewKCP(conv, session);
    session->SetKCP(kcp);
    return session;
}

KCPRingBuffer::KCPRingBuffer()
{
    Clear();
}

KCPRingBuffer::~KCPRingBuffer()
{
}

void KCPRingBuffer::Clear()
{
    read_pos = 0;
    write_pos = 0;
    is_full = false;
    is_empty = true;
}

int KCPRingBuffer::GetUsedSize() const
{
    if (is_empty) return 0;
    if (is_full) return BUFFER_SIZE;

    if (write_pos > read_pos)
    {
        return write_pos - read_pos;
    }

    return BUFFER_SIZE - read_pos + write_pos;
}

int KCPRingBuffer::GetFreeSize() const
{
    return BUFFER_SIZE - GetUsedSize();
}

int KCPRingBuffer::Write(const char* src, int len)
{
    if (len <= 0 || is_full) return 0;
    
    is_empty = false;
    if (write_pos >= read_pos)
    {
        int left_size = BUFFER_SIZE - write_pos;
        if (left_size > len)
        {
            memcpy(ring_buffer + write_pos, src, len);
            write_pos += len;
            return len;
        }
        memcpy(ring_buffer + write_pos, src, left_size);
        write_pos = std::min(read_pos, len - left_size);
        memcpy(ring_buffer, src + left_size, write_pos);
        is_full = (read_pos == write_pos);
        return left_size + write_pos;
    }
    
    int can_write_size = std::min(GetFreeSize(), len);
    memcpy(ring_buffer + write_pos, src, can_write_size);
    write_pos += can_write_size;
    is_full = (read_pos == write_pos);
    return can_write_size;
}

int KCPRingBuffer::Read(char* dst, int len)
{
    if (len <= 0 || is_empty) return 0;

    is_full = false;
    if (read_pos >= write_pos)
    {
        int left_size = BUFFER_SIZE - read_pos;
        if (left_size > len)
        {
            memcpy(dst, ring_buffer + read_pos, len);
            read_pos += len;
            return len;
        }
        memcpy(dst, ring_buffer + read_pos, left_size);
        read_pos = std::min(write_pos, len - left_size);
        memcpy(dst + left_size, ring_buffer, read_pos);
        is_empty = (read_pos == write_pos);
        return left_size + read_pos;
    }

    int can_read_size = std::min(GetFreeSize(), len);
    memcpy(dst, ring_buffer + read_pos, can_read_size);
    read_pos += can_read_size;
    is_empty = (read_pos == write_pos);
    return can_read_size;
}

bool KCPRingBuffer::ReadNoPop(char* dst, int len) const
{
    if (len <= 0 || GetUsedSize() < len) return false;

    if (read_pos >= write_pos)
    {
        int left_size = BUFFER_SIZE - read_pos;
        int first_copy_len = std::min(left_size, len);
        memcpy(dst, ring_buffer + read_pos, first_copy_len);
        if (first_copy_len < len)
        {
            memcpy(dst + first_copy_len, ring_buffer, len - first_copy_len);
        }
    }
    else
    {
        memcpy(dst, ring_buffer + read_pos, len);
    }

    return true;
}

int KCPRingBuffer::GetBufferSize() const
{
    return BUFFER_SIZE;
}

KCPSession::KCPSession(KCPServer* server, const KCPAddr& addr, IUINT64 current) :
    kcp_server(server), kcp_addr(addr), last_active_time(current)
{
}

KCPSession::~KCPSession()
{
    if (NULL != kcp_cb)
    {
        ikcp_release(kcp_cb);
    }
}

void KCPSession::Update(IUINT32 current)
{
    assert(NULL != kcp_cb);
    if (current >= ikcp_check(kcp_cb, current))
    {
        ikcp_update(kcp_cb, current);
    }

    static char buffer[kcp_max_package_size];

    do //revc kcp package 
    {
        int peek_size = ikcp_peeksize(kcp_cb);
        if (peek_size < 0) break;
        if (peek_size > kcp_max_package_size)
        {
            kcp_server->DoErrorLog("kcp peek size(%d) too large", peek_size);
            break;
        }
        if (peek_size > recv_buffer.GetBufferSize())
        {
            kcp_server->DoErrorLog("recv buffer remain size(%d) not enough for peek size(%d)",
                recv_buffer.GetBufferSize(), peek_size);
            break;
        }

        int len = ikcp_recv(kcp_cb, buffer, sizeof(buffer));
        if (len <= 0)
        {
            kcp_server->DoErrorLog("kcp recv error, peek_size:(%d) kcp_recv_len:%d,", peek_size, len);
            break;
        }

        assert(len = recv_buffer.Write(buffer, len));
    } while (true);

    do 
    {
        if (!recv_buffer.ReadNoPop(buffer, 4))
        {
            break;
        }

        IUINT32 tmp_length = *((IUINT32*)(&buffer[0]));
        if (tmp_length == 0xffffffffu)//kcp heart
        {
            assert(4 == recv_buffer.Read(buffer, 4));
            continue;
        }

        int package_len = (int)ntohl((u_long)tmp_length);

        if (package_len <= 0)
        {
            kcp_server->DoErrorLog("package size(%d) invalid", package_len);
            break;
        }

        if (package_len > kcp_max_package_size || package_len > recv_buffer.GetBufferSize())
        {
            kcp_server->DoErrorLog("package size(% d) too large", package_len);
            break;
        }
        if (package_len > recv_buffer.GetUsedSize())
        {
            break;
        }
        assert(package_len == recv_buffer.Read(buffer, package_len));
        kcp_server->OnKCPRecv(kcp_cb->conv, buffer, package_len);
    } while (true);
}

int KCPSession::Send(const char* data, int len)
{
    assert(NULL != kcp_cb);
    return ikcp_send(kcp_cb, data, len);
}

IUINT64 KCPSession::LastActiveTime() const
{
    return last_active_time;
}

void KCPSession::SetKCP(ikcpcb* kcp)
{
    kcp_cb = kcp;
}

void KCPSession::KCPInput(const sockaddr_in& sockaddr, const socklen_t socklen, const char* data, long sz, IUINT64 current)
{
    assert(NULL != kcp_cb);
    assert(NULL != data);

    if (0 != memcmp(&kcp_addr, &sockaddr, sizeof(sockaddr_in)))
    {
        kcp_server->DoErrorLog("conv(%d) switch address(%s) port(%d) to address(%s) port(%d)",
            kcp_cb->conv, inet_ntoa(kcp_addr.sockaddr.sin_addr), ntohs(kcp_addr.sockaddr.sin_port),
            inet_ntoa(sockaddr.sin_addr), ntohs(sockaddr.sin_port));

        int conv = kcp_cb->conv;
        Clear();
        kcp_cb = NewKCP(conv, this);
        kcp_addr = KCPAddr(sockaddr, socklen);
    }

    ikcp_input(kcp_cb, data, sz);
    last_active_time = current;
}

void KCPSession::Output(const char* buf, int len)
{
    kcp_server->DoOutput(kcp_addr, buf, len);
}

void KCPSession::Clear()
{
    if (NULL != kcp_cb)
    {
        ikcp_release(kcp_cb);
        kcp_cb = NULL;
    }
    recv_buffer.Clear();
}