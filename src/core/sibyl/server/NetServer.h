/*
   Copyright 2017 Hosang Yoon

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef SIBYL_SERVER_NETSERVER_H_
#define SIBYL_SERVER_NETSERVER_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cerrno>

#include "../NetAgent.h"
#include "Broker.h"
#include "../util/DispPrefix.h"

namespace sibyl
{

template <class TOrder, class TItem>
class NetServer : public NetAgent
{
public:
    void Launch(CSTR &port, bool autoStart, bool reconnectable);
    void StartMainLoop() { start_ab = true; start_cv.notify_one(); }
    
    NetServer(Broker<TOrder, TItem> *pBroker_)
        : pBroker(pBroker_), start_ab(false), sock_serv(sock_fail), sock_conn(sock_fail) {} 
private:
    int  Initialize   (CSTR &port); // returns non-0 to signal error
    int  AcceptConn   ();           // returns non-0 to signal error
    void SendMsgOut   ();
    int  RecvMsgIn    ();           // returns non-0 to signal client disconnection
    int  DisplayString(CSTR &disp, bool isError = false, int errno_ = 0);
    
    Broker<TOrder, TItem>  *pBroker;
    std::condition_variable start_cv;
    std::mutex              start_mutex;
    std::atomic_bool        start_ab;
    
    int sock_serv;
    int sock_conn;
};

template <class TOrder, class TItem>
void NetServer<TOrder, TItem>::Launch(CSTR &port, bool autoStart, bool reconnectable)
{
    start_ab = autoStart;
    
    if (verbose == true) DisplayString("Using port " + port);
    
    if (0 != Initialize(port)) return;
    
    do {
        if (0 == AcceptConn())
        {
            {
                std::unique_lock<std::mutex> lock(start_mutex);
                start_cv.wait(lock, [&]{ return start_ab == true; });
            } // mutex unlocked here
            
            while (true)
            {
                if (0 != pBroker->AdvanceTick()) {
                    reconnectable = false;
                    break;
                }
                if (false == pBroker->IsSkipping())
                {
                    // debug_msg("[NetServer] New tick");
                    SendMsgOut();
                }
                if (false == pBroker->IsSkipping() && 0 != RecvMsgIn())
                    break; 
            }
        }
    } while (reconnectable == true);
    
    if (verbose) DisplayString("Exiting main loop");
    
    pBroker->OnExit();
    
#ifdef _WIN32
    WSACleanup();
#endif /* _WIN32 */
}

template <class TOrder, class TItem>
int NetServer<TOrder, TItem>::Initialize(CSTR &port)
{
#ifdef _WIN32
    WSADATA wsaData;
    if (0 != WSAStartup(MAKEWORD(2, 2), &wsaData) ||
        LOBYTE(wsaData.wVersion) != 2             ||
        HIBYTE(wsaData.wVersion) != 2             )
        return   DisplayString("[Fail] Winsock DLL startup", true);
    // if (verbose) DisplayString("[Done] Winsock DLL startup");
#endif /* _WIN32 */
    
    // Create server socket
    if (sock_fail == (sock_serv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)))
        return   DisplayString("[Fail] Create socket", true, errno);
    // if (verbose) DisplayString("[Done] Create socket");
    
    // Prevent socket's address being blocked after app terminates
    int optval = 1;
    if (sock_fail == setsockopt(sock_serv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optval), sizeof(optval)))
        return   DisplayString("[Fail] Change socket option", true, errno); 
    // if (verbose) DisplayString("[Done] Change socket option");

    // Bind socket to address and listen
    struct sockaddr_in addr_serv;    
    memset(&addr_serv, 0, sizeof(addr_serv));
    addr_serv.sin_family      = AF_INET;
    addr_serv.sin_addr.s_addr = INADDR_ANY;
    addr_serv.sin_port        = htons(std::stoi(port));

    if (sock_fail == bind(sock_serv, (struct sockaddr *)&addr_serv, sizeof(addr_serv)))
        return   DisplayString("[Fail] Bind address to socket", true, errno);
    // if (verbose) DisplayString("[Done] Bind address to socket");
        
    if (sock_fail == listen(sock_serv, kTCPBacklog))
        return   DisplayString("[Fail] Listen on socket", true, errno);
    // if (verbose) DisplayString("[Done] Listen on socket");
    
    return 0; 
}

template <class TOrder, class TItem>
int NetServer<TOrder, TItem>::AcceptConn()
{    
    if (verbose) DisplayString("Waiting for client connection");
    
    // Establish a connection and query password
    struct sockaddr_in addr_clnt;
    socklen_t szAddr = sizeof(addr_clnt);
    
    if(sock_fail == (sock_conn = accept(sock_serv, (struct sockaddr *)&addr_clnt, &szAddr)))
        return DisplayString("[Fail] Accept client", true, errno);
    else
    {
        if (verbose) DisplayString("Querying password");
        memset(bufTCP, 0, kTCPBufSize);
        if (recv(sock_conn, bufTCP, kTCPBufSize, 0) > 0)
        {
            char *pc = strpbrk(bufTCP, "\r\n");
            if (pc != NULL) *pc = '\0';
            if (kTCPPassword == STR(bufTCP))
            {
                // Set timeout for recv calls
                #ifndef _WIN32
                constexpr struct timeval sock_timeout = { .tv_sec  = kTimeRates::secPerTick,
                                                          .tv_usec = 0 };
                #else
                constexpr DWORD sock_timeout = kTimeRates::secPerTick * 1000;
                #endif /* !_WIN32 */
                
                if (sock_fail == setsockopt(sock_conn, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&sock_timeout), sizeof(sock_timeout)))
                    return   DisplayString("[Fail] Change socket recv timeout", true, errno);
            }
            else
            {
                close_socket(sock_conn);
                sock_conn = sock_fail;
                return DisplayString("[Fail] Invalid password", true);
            }
        }
        else
        {
            close_socket(sock_conn);
            sock_conn = sock_fail;
            return DisplayString("[Fail] Receive password", true);
        }
    }
    if (verbose == true)
    {
        DisplayString("Client connection established");
        pBroker->SetVerbose(true);
    }
    
    return 0;
}

template <class TOrder, class TItem>
void NetServer<TOrder, TItem>::SendMsgOut()
{
    verify((sock_serv != sock_fail) && (sock_conn != sock_fail));
    const auto &msg = pBroker->BuildMsgOut();
    send(sock_conn, msg.c_str(), msg.size(), 0);
}

template <class TOrder, class TItem>
int NetServer<TOrder, TItem>::RecvMsgIn()
{
    verify((sock_serv != sock_fail) && (sock_conn != sock_fail));
    bufMsg[0] = '\0';
    while (true)
    {
        memset(bufTCP, 0, kTCPBufSize);
        if (recv(sock_conn, bufTCP, kTCPBufSize, 0) > 0)
        {
            std::size_t lencat = strlen(bufMsg) + strlen(bufTCP); 
            if (lencat >= kTCPBufSize) return DisplayString("[Warn] TCP input buffer overflow", true);
            strcat(bufMsg, bufTCP);
            if (bufMsg[lencat - 1] == '\n')
                break;
        }
        else
        {
            #ifndef _WIN32
            if (errno == EAGAIN || errno == EWOULDBLOCK) // timeout
            #else
            if (WSAGetLastError() == WSAETIMEDOUT)
            #endif
            {
                if (verbose) DisplayString("Timeout during recv");
                break;
            }
            else
            {
                if (verbose) DisplayString("Client disconnected");
                return -1;
            }
        }
    }
    pBroker->ApplyMsgIn(bufMsg);
    return 0;
}

template <class TOrder, class TItem>
int NetServer<TOrder, TItem>::DisplayString(CSTR &disp, bool isError, int errno_)
{
    // cerr used by Agents
    std::ostream& stdNormal = (dispPrefix.IsNull() == true ? std::cout : std::cerr);
    
    if (isError == false)
        stdNormal << dispPrefix << "NetServer: " << disp << std::endl;    
    else
    {
        std::cerr << dispPrefix << "NetServer: " << disp;
        if (errno_ != 0) std::cerr << "\n" << strerror(errno_); 
        std::cerr << std::endl;
    }
    return (isError == false ? 0 : -1);
}

}

#endif /* SIBYL_SERVER_NETSERVER_H_ */