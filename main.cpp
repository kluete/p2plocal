
#include <cstdio>
#include <string>
#include <iostream>
#include <vector>
#include <random>
#include <limits>
#include <array>
#include <map>
#include <deque>
#include <chrono>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <sstream>

#include "asio.hpp"

namespace strimrut
{
    
using namespace std;                            // (broad use in cpp/non-header is ok)

using asio::ip::udp;

const int   SERVER_PORT = 3000;

const auto  BROADCAST_INTERVAL_SECS = chrono::seconds(4);

const auto  HEARTBEAT_INTERVAL_MS = chrono::milliseconds(500);
const auto  REAPER_INTERVAL_MS = HEARTBEAT_INTERVAL_MS;
const auto  REAPER_DEADLINE_MS = chrono::duration_cast<chrono::milliseconds>(3 * HEARTBEAT_INTERVAL_MS).count();

enum MSG_T : uint16_t
{
    P2P_ILLEGAL          = 0,
    P2S_PEER_BIRTH       = 0xC0,
    S2P_PEER_DIRECTORY,
    P2P_VANILLA_MESSAGE,
    P2S_KEEP_ALIVE,
};

// PODs

#pragma pack(push)
#pragma pack(1)             // no padding

// (slim) peer directory entry
struct peer_entry
{
    peer_entry(const string &name, const uint16_t port)
        : m_Name(name), m_Port(port)
    {
    }
    
    const string    m_Name;
    const uint16_t  m_Port;
};

// (alive) peer entry
struct live_peer_entry
{
    live_peer_entry(const string &name, const uint16_t port)
        : m_Raw(name, port), m_LastHeartbeat(chrono::steady_clock::now())
    {
    }
    
    void    UpdateBeat(void)
    {
        m_LastHeartbeat = chrono::steady_clock::now();
    }
    
    // millisecs since last heartbeat
    size_t  MsSinceLastBeat(void) const
    {
        const auto	    tp = chrono::steady_clock::now() - m_LastHeartbeat;
        const auto	    elap_ms_no_typ = chrono::duration_cast<chrono::milliseconds>(tp);
        const size_t	elap_ms = elap_ms_no_typ.count();
	
        return elap_ms;
    }
        
    peer_entry                          m_Raw;
    chrono::steady_clock::time_point    m_LastHeartbeat;
};

#pragma pack(pop)

//---- Peer (string) name to Int -----------------------------------------------

uint16_t    PeerStrToInt(const string &s)
{
    if (s.empty())   return 0;
    
    return (uint16_t)0x0000 | s[0];
}

//---- Peer (int) name to String -----------------------------------------------

string      PeerIntToStr(const uint16_t hex_name)
{
    if (!hex_name)  return "";         // should fall-through? assert?
    
    const string  res((size_t)1, (char)(hex_name & 0xff));
    
    return res;
}

//---- UDP SERVER --------------------------------------------------------------

class udp_server
{
public:
    udp_server(asio::io_context& io_context, uint16_t server_port)
        : m_io_context(io_context), m_ServerSocket(io_context, udp::endpoint(udp::v4(), server_port)),
        m_ReaperTimer(io_context)
    {
    }

    void start(void)
    {
        ReadUnknownMessage();
        
        RestartReaperTimer();
    }
    
private:

//---- Read Unknown Message ----------------------------------------------------

    void    ReadUnknownMessage(void)
    {
        m_ServerSocket.async_wait(asio::ip::udp::socket::wait_read,
            [&](asio::error_code ec)
            {
                // cout << "server socket has readable data ***" << endl;
                
                size_t    avail_sz = 0;
                
                // wait until available data (shouldn't be looping though)
                do
                {   avail_sz = m_ServerSocket.available();
                
                    this_thread::sleep_for(chrono::milliseconds(10));
                
                } while (!avail_sz);
                
                ResizeRcvBuff(avail_sz);
                
                asio::mutable_buffer	                muta_buf(&m_RcvBuffer[0], avail_sz);
                const asio::socket_base::message_flags	flags = 0;
                udp::endpoint		                    ep;
                
                const size_t	rcv_sz = m_ServerSocket.receive_from(asio::buffer(muta_buf), ep/*&*/, flags, ec/*&*/);
                if (ec || (rcv_sz < avail_sz))
                {	cout << "server error: receiving underflowed message size (" << rcv_sz << " vs " << avail_sz << " bytes), ASIO error = \"" << ec.message() << endl;
                }
                else
                {
                    // cout << "server received unknown message of " << rcv_sz << " bytes from " << ep.address() << endl;
                
                    DispatchMessage();
                }
                
                // restart wait for next read
                ReadUnknownMessage();
        });
    }
    
//---- Dispatch Message --------------------------------------------------------

void    DispatchMessage(void)
{            
    switch (*m_HeaderIDPtr)
    {
        case MSG_T::P2S_PEER_BIRTH:
        {
            DecodeBirthBody();
    
            AnnounceDirectory();
        }   break;
        
        case MSG_T::P2S_KEEP_ALIVE:
        {
            UpdatePeerHeartbeat();
        }   break;
        
        default:
        {
            cout << "error in server msg switch" << endl;
        }   break;
    }
    
    
}

//---- Decode Birth Body -------------------------------------------------------

bool    DecodeBirthBody(void)
{
        const uint8_t *raw_p = m_BodyPtr;
        
        const uint16_t  hex_name = *(uint16_t*)raw_p;
        raw_p += sizeof(uint16_t);
        
        const string    peer_name((const char*)&hex_name);
        
        const uint16_t  peer_port = *(uint16_t*)raw_p;
        raw_p += sizeof(uint16_t);
        
        cout << peer_name << " announced self with UDP port " << peer_port << endl;
        // cout << "  (useless ephemeral endpoint peer port " << m_TempPeerEndpoint.port() << ")" << endl;
        
        if (m_PeerDirectory.count(peer_name))
        {   // duplicate name
            cout << "error: duplicate peer \"" << peer_name << "\", silently ignored" << endl;
            return false;
        }
        
        if (m_PeerPortSet.count(peer_port))
        {   // duplicate port
            cout << "error: duplicate peer \"" << peer_name << "\" port " << peer_port << ", silently ignored" << endl;
            return false;
        }
        
        m_PeerDirectory.emplace(peer_name, live_peer_entry(peer_name, peer_port));
        
        DirtyPeerDirectory();
        
        return true;    // ok
}

//--- Update Peer Heartbeat ----------------------------------------------------

void    UpdatePeerHeartbeat(void)
{
    const uint8_t *raw_p = m_BodyPtr;
    
    const uint16_t  hex_name = *(uint16_t*)raw_p;
    raw_p += sizeof(uint16_t);
    
    const string    peer_name((const char*)&hex_name);
        
    // cout << "updating " << peer_name << " heartbeat" << endl;
    
    if (!m_PeerDirectory.count(peer_name))
    {
        cout << "server error updating unknown peer: \"" << peer_name << "\"" << endl;
        return;
    }
    
    m_PeerDirectory.at(peer_name).UpdateBeat();
}

//---- Restart Reaper Timer ----------------------------------------------------

void    RestartReaperTimer()                  // (reloops)
{
    m_ReaperTimer.expires_after(REAPER_INTERVAL_MS);
    m_ReaperTimer.async_wait(
        [this](std::error_code ec)
        {
            if (ec)
            {   // error
                cout << "server reaper timer error " << ec.message() << endl;
                return;
            }
            
            ReapDeadPeers();
            
            RestartReaperTimer();
        });
}

//---- Reap Dead Peers ---------------------------------------------------------

void    ReapDeadPeers(void)
{
    // cout << "ReapDeadPeers START" << endl;
    
    bool                                    some_dead_f = false;
    unordered_map<string, live_peer_entry>  survived_peers;
    
    for (const auto &it : m_PeerDirectory)
    {
        const string            name = it.first;
        const live_peer_entry   live_pe = it.second;
        
        if (live_pe.MsSinceLastBeat() > REAPER_DEADLINE_MS)
        {   // dead
            some_dead_f = true;
            
            cout << "server culling peer \"" << name << "\"" << endl;
            continue;
        }
        
        // still alive, keep in survival list
        survived_peers.emplace(name, live_pe);
    }
    
    if (!some_dead_f)       return;
    
    // replace survived ones
    m_PeerDirectory.swap(survived_peers);
    
    DirtyPeerDirectory();
    
    AnnounceDirectory();
        
    cout << "updated server directory has " << m_PeerDirectory.size() << " entries" << endl;
}
        
//---- Announce Directory ------------------------------------------------------

    void    AnnounceDirectory(void)
    {
        const size_t    n_entries = m_PeerDirectory.size();
        assert(n_entries <= numeric_limits<uint16_t>::max());
        
        const size_t        body_sz = sizeof(MSG_T)/*msg_id*/ + sizeof(uint16_t)/*# entries*/ + (n_entries * (sizeof(uint16_t)/*name*/ + sizeof(uint16_t)/*port*/));
        
        cout << "server announces directory with " << dec << n_entries << " entries" << endl;
        
        // build one, fat directory
        vector<uint8_t>     buff(body_sz, 0);
        
        uint8_t    *raw_p(&buff[0]);
        
        *(MSG_T*)raw_p = S2P_PEER_DIRECTORY;
        raw_p += sizeof(MSG_T);
        
        *(uint16_t*)raw_p = n_entries;
        raw_p += sizeof(uint16_t);
        
        vector<uint16_t>    peer_ports;
        
        for (auto &it : m_PeerDirectory)
        {
                const string        peer_name(it.first);
                const peer_entry    pe(it.second.m_Raw);
            
                cout << " " << peer_name << " = " << pe.m_Port << endl;
                
                // fill entry
                const uint16_t  name_hex = PeerStrToInt(peer_name);
                
                *(uint16_t*)raw_p = name_hex;
                raw_p += sizeof(uint16_t);
                
                *(uint16_t*)raw_p = pe.m_Port;
                raw_p += sizeof(uint16_t);
                
                // (save peer port)
                peer_ports.push_back(pe.m_Port);                
        }
        
    // send whole directories in one go
        
        for (const uint16_t port : peer_ports)
        {
                asio::ip::udp::endpoint                 ep(asio::ip::udp::v4(), port);
                const asio::socket_base::message_flags	flags = 0;	// asio::socket_base::message_do_not_route;		// defined in asio/socket_base.hpp
                asio::error_code			            ec;
                
                // body
                const size_t    written_sz = m_ServerSocket.send_to(asio::buffer(&buff[0], body_sz/*len*/), ep/*destination*/, flags, ec/*&*/);
                assert(!ec);
                assert(written_sz == body_sz);
                
                // cout << "server wrote directory body of " << written_sz << " bytes " << endl;
        }
    }
    
//---- Dirty Peer Directory ----------------------------------------------------
    
    void    DirtyPeerDirectory(void)
    {
        // rebuild peer ports hashset
        m_PeerPortSet.clear();
        
        for (const auto &it : m_PeerDirectory)
        {
            const peer_entry &pe = it.second.m_Raw;
            
            assert(!m_PeerPortSet.count(pe.m_Port));
            
            m_PeerPortSet.insert(pe.m_Port);
        }
    }
    
//---- Resize Receive Buffer ---------------------------------------------------

    void    ResizeRcvBuff(const size_t &sz)
    {
        m_RcvBuffer.reserve(sz);

        m_HeaderIDPtr = (const MSG_T*)&m_RcvBuffer[0];
        m_BodyPtr = &m_RcvBuffer[2];
    }

    asio::io_context                        &m_io_context;
    udp::socket                             m_ServerSocket;
    
    unordered_map<string, live_peer_entry>  m_PeerDirectory;
    unordered_set<uint16_t>                 m_PeerPortSet;
    
    vector<uint8_t>                         m_RcvBuffer;
    const MSG_T                             *m_HeaderIDPtr;
    const uint8_t                           *m_BodyPtr;
    
    asio::steady_timer                      m_ReaperTimer;
};

//---- Peer --------------------------------------------------------------------

class peer
{
public:
    // ctor
    peer(asio::io_context &io_context, const string &name, const uint16_t peer_port)
        : m_OwnPort(peer_port), m_PeerPureName(name), m_PeerName(string("peer").append(name)),
        m_io_context(io_context), m_SocketToServer(m_io_context),
        m_OwnSocket(io_context, udp::endpoint(udp::v4(), peer_port)),
        m_HeartbeatTimer(m_io_context), m_BroadcastTimer(m_io_context), m_BroadcastCnt(0)
    {
        cout << m_PeerName << " ctor" << endl;
        
        m_SocketToServer.open(udp::v4());
        
        AnnounceOwnBirth(name);        
    }

//---- Start -------------------------------------------------------------------

void    start(void)
{
    cout << m_PeerName << " start()" << endl;
    
    RestartHeartbeat();
    RestartBroadcast();

    ReadUnknownMessage();
}

private:

//---- Announce Own Bith (to server) -------------------------------------------
    
void    AnnounceOwnBirth(const string &pure_name)
{
    cout << m_PeerName << " announcing own birth" << endl;

    // msg_id + name + udp port
    const size_t        birth_body_sz = sizeof(MSG_T) + sizeof(uint16_t) + sizeof(uint16_t);
    
    vector<uint16_t>    birth_body{{MSG_T::P2S_PEER_BIRTH, PeerStrToInt(pure_name), m_OwnPort}};
    
    const asio::socket_base::message_flags	flags = 0;	// asio::socket_base::message_do_not_route;		// defined in asio/socket_base.hpp
    asio::error_code			            ec;
    udp::endpoint                           ep(asio::ip::udp::v4(), SERVER_PORT);
    
    // loop until can send
    size_t	written_sz;

    do
    {        
        // try to send body
        written_sz = m_SocketToServer.send_to(asio::buffer(&birth_body[0], birth_body_sz/*len*/), ep/*destination*/, flags, ec/*&*/);
        
        if (!ec)     break;
        
        assert(birth_body_sz == written_sz);
        
        // (wait for server up)
        this_thread::sleep_for(std::chrono::milliseconds(100));
        
    } while (!ec);
    
    cout << m_PeerName << " wrote " << written_sz << " bytes " << endl;
}

//---- Read Unknown Message ----------------------------------------------------

void    ReadUnknownMessage(void)
{
    // wait for socket to have something to read
    m_OwnSocket.async_wait(asio::ip::udp::socket::wait_read,
        [&](asio::error_code ec)
        {
            // cout << m_PeerName << " socket has readable data ***" << endl;

            size_t    avail_sz = 0;
            
            do
            {   avail_sz = m_OwnSocket.available();                 // shouldn't be zero (nor loop)
                
                this_thread::sleep_for(chrono::milliseconds(10));
            
            } while (!avail_sz);
            
            ResizeRcvBuff(avail_sz);
            
            asio::mutable_buffer	                muta_buf(&m_RcvBuffer[0], avail_sz);
            const asio::socket_base::message_flags	flags = 0;
            udp::endpoint		                    ep;
            
            const size_t	rcv_sz = m_OwnSocket.receive_from(asio::buffer(muta_buf), ep/*&*/, flags, ec/*&*/);
            if (ec || (rcv_sz != avail_sz))
            {	cout << m_PeerName << " error receiving message of " << dec << rcv_sz << "bytes (ASIO error \"" << ec.message() << "\")" << endl;
            }
            else
            {
                // cout << m_PeerName << " received unknown message of " << rcv_sz << " bytes from " << ep.address() << endl;
            
                DispatchMessage();
            }
            
            // reloop to wait for next message
            ReadUnknownMessage();
        });
}

//---- Dispatch Message --------------------------------------------------------
           
void    DispatchMessage(void)
{
    // dispatch
    switch (*m_HeaderIDPtr)
    {
        case MSG_T::S2P_PEER_DIRECTORY:
        {
                cout << m_PeerName << " got S2P_PEER_DIRECTORY msg" << endl;
                DecodePeerDirectory(m_BodyPtr);                
        }       break;
        
        case MSG_T::P2P_VANILLA_MESSAGE:
        {
                // cout << m_PeerName << " got P2P_VANILLA_MESSAGE msg" << endl;
                ReadVanillaMessage(m_BodyPtr);
        }       break;
        
        default:
        {
                cout << "illegal message header id 0x" << hex << (int) (*m_HeaderIDPtr) << endl;
        }       break;
    }
}

//---- Restart Heartbeat -------------------------------------------------------
    
void    RestartHeartbeat(void)                  // (reloops)
{
    m_HeartbeatTimer.expires_after(HEARTBEAT_INTERVAL_MS);
    m_HeartbeatTimer.async_wait(
        [this](std::error_code ec)
        {
            if (ec)
            {   // error
                cout << m_PeerName << " heartbeat timer error " << ec.message() << endl;
                return;
            }
            
            SendHeartbeat();
            
            RestartHeartbeat();
        });
}

//---- Send Heartbeat (to server) ----------------------------------------------

void    SendHeartbeat(void)
{
    // msg_id + name
    const size_t        birth_body_sz = sizeof(MSG_T) + sizeof(uint16_t);
    
    vector<uint16_t>    birth_body{{MSG_T::P2S_KEEP_ALIVE, PeerStrToInt(m_PeerPureName)}};
    
    const asio::socket_base::message_flags	flags = 0;	// asio::socket_base::message_do_not_route;		// defined in asio/socket_base.hpp
    asio::error_code			            ec;
    udp::endpoint                           ep(asio::ip::udp::v4(), SERVER_PORT);
    
    const size_t	written_sz = m_SocketToServer.send_to(asio::buffer(&birth_body[0], birth_body_sz/*len*/), ep/*destination*/, flags, ec/*&*/);
    if (ec || (birth_body_sz != written_sz))
    {
        cout << m_PeerName << " error sending keepalive: " << ec.message() << endl;
        return;
    }
}

//---- Restart Broadcast -------------------------------------------------------
    
void    RestartBroadcast(void)                  // (reloops)
{
    m_BroadcastTimer.expires_after(BROADCAST_INTERVAL_SECS);
    m_BroadcastTimer.async_wait(
        [this](std::error_code ec)
        {
            if (ec)
            {   // error
                cout << m_PeerName << " broadcast timer error " << ec.message() << endl;
                return;
            }
            
            // cout << m_PeerName << " broadcast timer triggered!!!" << endl;
            
            ostringstream   oss;
            
            oss << m_PeerName << "_broadcast_" << hex << m_BroadcastCnt++;
            
            BroadcastVanillaMessages(oss.str());
            
            RestartBroadcast();
        });
}

//---- Broadcast Vanilla Messages ----------------------------------------------
    
bool    BroadcastVanillaMessages(const string &msg_body)
{       
    for (auto &it : m_PeerDirectory)
    {
        const peer_entry    &pe = it.second;
        
        const string    dest_peer_name = pe.m_Name;
            
        const bool    ok = SendVanillaMessage(dest_peer_name, msg_body);
        if (!ok)        return false;       // error
     }
     
     return true;       // ok
}

//---- Decode Peer Durectory (sent by server) ----------------------------------

void    DecodePeerDirectory(const uint8_t *raw_p)
{
    m_PeerDirectory.clear();
    
    const uint16_t  n_entries = *(uint16_t*)raw_p;
    raw_p += sizeof(uint16_t);
    
    cout << m_PeerName << " directory has " << n_entries << " entries" << endl;
    
    for (int i = 0; i < n_entries; i++)
    {
        const uint16_t    name_hex = *(uint16_t*)raw_p;
        raw_p += sizeof(uint16_t);
        
        const string    peer_name = PeerIntToStr(name_hex);
            
        const uint16_t  peer_port = *(uint16_t*)raw_p;
        raw_p += sizeof(uint16_t);
        
        if (peer_port == m_OwnPort)             continue;           // skip self
        
        assert(!m_PeerDirectory.count(peer_name));
        
        m_PeerDirectory.emplace(peer_name, peer_entry(peer_name, peer_port));
    }
    
   // cout << m_PeerName << " dir has decoded " << dec << n_entries << " entries" << endl;
}
    
//---- Read Vanilla Message (sent by peer) -------------------------------------
    
    void    ReadVanillaMessage(const uint8_t *raw_p)
    {
        // cout << m_PeerName << " reading vanilla msg" << endl;
        
        const uint16_t  body_sz = *(uint16_t*)raw_p;
        raw_p += sizeof(uint16_t);
        
        const string    vanilla_body((const char*)raw_p, (size_t)body_sz);
        
        cout << m_PeerName << " received vanilla message: \"" << vanilla_body << "\"" << endl;
    }
    
//---- Send Vanilla Message (to peer) ------------------------------------------
    
    bool    SendVanillaMessage(const string &dest_peer_name, const string &body_s)         // returns [ok]
    {
        if (!m_PeerDirectory.count(dest_peer_name))
        {
            cout << "peer name \"" << dest_peer_name << "\" currently unregistered" << endl;
            return true;        // ok
        }
        
        const size_t    body_sz = body_s.length();
        
        const size_t    msg_sz = sizeof(MSG_T)/*msg_id*/ + sizeof(uint16_t)/*len*/ + body_sz + 1;
        assert(msg_sz < numeric_limits<uint16_t>::max());       // check overflow
        
        vector<uint8_t> buff(msg_sz, 0);
        
        uint8_t *raw_p = &buff[0];
        
        *(MSG_T*)raw_p = MSG_T::P2P_VANILLA_MESSAGE;
        raw_p += sizeof(MSG_T);
        
        *(uint16_t*)raw_p = body_s.length();
        raw_p += sizeof(uint16_t);
        
        std::copy(body_s.c_str(), body_s.c_str() + body_sz, raw_p);
        
        const uint16_t                          dest_port = m_PeerDirectory.at(dest_peer_name).m_Port;
        
        asio::ip::udp::endpoint                 ep(asio::ip::udp::v4(), dest_port);
        const asio::socket_base::message_flags	flags = 0;	// asio::socket_base::message_do_not_route;		// defined in asio/socket_base.hpp
        asio::error_code			            ec;
        
        // body (is zero terminated?)
        const size_t    written_sz = m_OwnSocket.send_to(asio::buffer(&buff[0], buff.size()), ep/*destination*/, flags, ec/*&*/);
        assert(!ec);
        assert(written_sz == msg_sz);
        
        // cout << m_PeerName << " wrote vanilla body of " << written_sz << " bytes " << endl;
        
        return true;
    }
    
//---- Resize Receive Buffer ---------------------------------------------------
    
	void    ResizeRcvBuff(const size_t &sz)
    {
        m_RcvBuffer.reserve(sz);

        m_HeaderIDPtr = (const MSG_T*)&m_RcvBuffer[0];
        m_BodyPtr = &m_RcvBuffer[2];
    }

    const uint16_t                      m_OwnPort;
    const string                        m_PeerPureName, m_PeerName;

    asio::io_context                    &m_io_context;
    udp::socket		                    m_SocketToServer;
    udp::socket		                    m_OwnSocket;

    unordered_map<string, peer_entry>   m_PeerDirectory;            // replica updated by server
    
    vector<uint8_t>                     m_RcvBuffer;
    const MSG_T                         *m_HeaderIDPtr;
    const uint8_t                       *m_BodyPtr;
    
    asio::steady_timer                  m_HeartbeatTimer, m_BroadcastTimer;
    uint16_t                            m_BroadcastCnt;
};

//---- Run Server --------------------------------------------------------------

void    RunServer(asio::io_context &io_context)
{
    udp_server  serv(io_context, SERVER_PORT);

    serv.start();
    
    // start server event loop
    io_context.run();
}

//---- Run Peer ----------------------------------------------------------------

void RunPeer(asio::io_context &io_context, const string peer_name, const uint16_t port)
{
    peer    peer_n(io_context, peer_name, port);
    
    peer_n.start();
    
    // start peer event loop
    io_context.run();
}

} // namespace strimrut

using namespace strimrut;

//---- MAIN --------------------------------------------------------------------

int main(int argc, char **argv)
{
	cout << "strimrut test has " << argc << " args" << endl;
    
    asio::io_context    io_context;
    
    /*
    // won't stop?
    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait(
        [&](auto, auto)
        {
            cout << "forcibly quit by user" << endl;
            
            io_context.stop();
            
            abort();
        });
    */
    
    try
    {
        switch (argc)
        {
            case 1:
            {   // server
                cout << "instantiating server at port " << SERVER_PORT << endl;
                
                RunServer(io_context);
            }   break;
                
            case 3:
            {   // peer
                cout << "requesting peer with name \"" << argv[1] << "\", port " << argv[2] << endl;
                
                // sanity checks
                const string    name(argv[1]);
                if (name.size() != 1)
                {
                    cout << "illegal name length, expects 1 char" << endl;
                    cout << "aborting..." << endl;
                    return -1;
                }
                if (!isprint(name[0]))
                {
                    cout << "illegal unprintable name" << endl;
                    cout << "aborting..." << endl;
                    return -1;
                }
                
                const int   port = stoi(argv[2]);       // (throwns on failure)
                if (port == SERVER_PORT)
                {
                    cout << "illegal port, is reserved for server" << endl;
                    cout << "aborting..." << endl;
                    return -1;
                }
                if (port > numeric_limits<int16_t>::max())
                {
                    cout << "illegal port, overflow" << endl;
                    cout << "aborting..." << endl;
                    return -1;
                }
                
                RunPeer(io_context, name, port);
            }   break;

            default:
            {
                // illegal nargs
                cout << "illegal number of arguments:" << endl;
                cout << "  server expects no arguments" << endl;
                cout << "  peer expects <name> <port> arguments" << endl;
                cout << "aborting..." << endl;
                return -1;
            }   break;
        }
        
        return 0;
    }
    catch (std::exception &e)
    {
        cerr << "Exception: " << e.what() << endl;
    }
    
    return 0;
}

// nada mas
