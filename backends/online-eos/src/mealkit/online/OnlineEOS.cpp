// EOS backend (game builds with the EOS SDK and third_party/eos/credentials.txt, MEALKIT_EOS=1).
// Login is EOS Connect (no Epic account). Builds that also have the Steam SDK (MEALKIT_EOS_STEAM=1) log in with a Steam web API ticket and
// friend invites carry the lobby code through the Steam overlay (+connect_lobby <code>). Without Steam or without a Steam ID provider in the portal, device ID.
// One OS user has one device ID, so two processes on the same PC become the same user (two-player tests need two PCs).
#include "mealkit/online/Online.h"

#include "mealkit/core/Log.h"
#include "mealkit/platform/System.h"

#include "EosCredentials.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <unordered_map>

#pragma warning(push, 0)
#include <eos_connect.h>
#include <eos_lobby.h>
#include <eos_logging.h>
#include <eos_p2p.h>
#include <eos_sdk.h>
#if MEALKIT_EOS_STEAM
#include <steam/steam_api.h>
#endif
#pragma warning(pop)

namespace mealkit::online {
namespace {

constexpr const char* kSocketName = "mealkit";
constexpr const char* kCodeKey = "MKCODE";  // lobby attribute: numeric code chosen by the host (lobbyId of joinLobby)
constexpr const char* kBucket = "mealkit";
constexpr f64 kLoginTimeoutSeconds = 15.0;
constexpr const char* kSteamIdentity = "epiconlineservices"; // identity of the Steam web API ticket EOS accepts
constexpr const char* kConnectPrefix = "+connect_lobby ";     // Steam invite string (GameOptions reads the same argument)

// Product User ID string → 64 bits (FNV-1a). Lockstep only handles u64 addresses; the real id is looked up in Impl's table
u64 hashUserId(EOS_ProductUserId id) {
    if (!id) return 0;
    char text[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
    int32_t length = sizeof(text);
    if (EOS_ProductUserId_ToString(id, text, &length) != EOS_EResult::EOS_Success) return 0;
    u64 h = 1469598103934665603ull;
    for (const char* c = text; *c; ++c) h = (h ^ static_cast<u8>(*c)) * 1099511628211ull;
    return h ? h : 1;
}

EOS_P2P_SocketId socketId() {
    EOS_P2P_SocketId s = {};
    s.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
    std::memcpy(s.SocketName, kSocketName, std::strlen(kSocketName)); // the rest is 0
    return s;
}

void EOS_CALL onLog(const EOS_LogMessage* message) {
    // An error saying the device ID already exists is the normal path (login accepts EOS_DuplicateNotAllowed as is)
    if (std::strstr(message->Message, "DeviceId access credentials already exist")) return;
    if (message->Level <= EOS_ELogLevel::EOS_LOG_Error) MEALKIT_ERROR("EOS %s: %s", message->Category, message->Message);
    else if (message->Level <= EOS_ELogLevel::EOS_LOG_Warning) MEALKIT_WARN("EOS %s: %s", message->Category, message->Message);
}

} // namespace

struct Service::Impl {
    EOS_HPlatform platform = nullptr;
    EOS_HConnect connect = nullptr;
    EOS_HLobby lobbies = nullptr;
    EOS_HP2P p2p = nullptr;
    EOS_ProductUserId localUser = nullptr;
    bool initialized = false; // EOS_Initialize (once per process)
    bool available = false;
    bool loginDone = false;
    bool deviceLogin = false; // Steam login failed, so device ID was used
    std::string name;

    LobbyState state = LobbyState::None;
    std::string lobby; // EOS lobby id
    u64 code = 0;      // numeric code chosen by the host
    u64 owner = 0;
    u32 members = 0;
    u64 pendingJoin = 0;
    EOS_HLobbySearch search = nullptr;
    EOS_NotificationId connectionRequest = EOS_INVALID_NOTIFICATIONID;

    std::unordered_map<u64, EOS_ProductUserId> users; // hash → Product User ID (from lobby members and received packets)

#if MEALKIT_EOS_STEAM
    bool steam = false; // SteamAPI_Init succeeded
    bool ticketDone = false;
    HAuthTicket ticket = k_HAuthTicketInvalid;
    std::string ticketHex;
    CCallback<Impl, GetTicketForWebApiResponse_t> ticketResponse;
    CCallback<Impl, GameRichPresenceJoinRequested_t> richJoin;

    Impl() : ticketResponse(this, &Impl::onTicket), richJoin(this, &Impl::onRichJoin) {}

    void onTicket(GetTicketForWebApiResponse_t* r) {
        if (r->m_hAuthTicket != ticket) return;
        if (r->m_eResult == k_EResultOK) {
            static const char hex[] = "0123456789ABCDEF";
            ticketHex.clear();
            for (int i = 0; i < r->m_cubTicket; ++i) {
                ticketHex += hex[r->m_rgubTicket[i] >> 4];
                ticketHex += hex[r->m_rgubTicket[i] & 15];
            }
        } else {
            MEALKIT_WARN("EOS: no Steam web API ticket (result %d)", static_cast<int>(r->m_eResult));
        }
        ticketDone = true;
    }

    // Accepting a Steam friend invite (when the game is already running. If not running, Steam launches it with a +connect_lobby argument)
    void onRichJoin(GameRichPresenceJoinRequested_t* r) {
        const size_t prefix = std::strlen(kConnectPrefix);
        if (std::strncmp(r->m_rgchConnect, kConnectPrefix, prefix) == 0)
            pendingJoin = std::strtoull(r->m_rgchConnect + prefix, nullptr, 10);
    }

    // So [Join Game] in the friends list comes to this lobby
    void setPresence() {
        if (!steam) return;
        if (state == LobbyState::InLobby) {
            const std::string connectString = kConnectPrefix + std::to_string(code);
            SteamFriends()->SetRichPresence("connect", connectString.c_str());
        } else {
            SteamFriends()->SetRichPresence("connect", nullptr);
        }
    }
#else
    void setPresence() {}
#endif

    u64 remember(EOS_ProductUserId id) {
        const u64 h = hashUserId(id);
        if (h) users[h] = id;
        return h;
    }
    EOS_ProductUserId find(u64 h) const {
        const auto it = users.find(h);
        return it == users.end() ? nullptr : it->second;
    }

    // Rereads lobby owner/members from the SDK's local copy (every frame, no network)
    void refreshLobby() {
        owner = 0;
        members = 0;
        if (state != LobbyState::InLobby) return;
        EOS_Lobby_CopyLobbyDetailsHandleOptions o = {};
        o.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST;
        o.LobbyId = lobby.c_str();
        o.LocalUserId = localUser;
        EOS_HLobbyDetails details = nullptr;
        if (EOS_Lobby_CopyLobbyDetailsHandle(lobbies, &o, &details) != EOS_EResult::EOS_Success) return;
        EOS_LobbyDetails_GetLobbyOwnerOptions ownerOptions = {};
        ownerOptions.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
        owner = remember(EOS_LobbyDetails_GetLobbyOwner(details, &ownerOptions));
        EOS_LobbyDetails_GetMemberCountOptions countOptions = {};
        countOptions.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST;
        members = EOS_LobbyDetails_GetMemberCount(details, &countOptions);
        for (u32 i = 0; i < members; ++i) {
            EOS_LobbyDetails_GetMemberByIndexOptions member = {};
            member.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERBYINDEX_API_LATEST;
            member.MemberIndex = i;
            remember(EOS_LobbyDetails_GetMemberByIndex(details, &member));
        }
        EOS_LobbyDetails_Release(details);
    }

    void releaseSearch() {
        if (search) EOS_LobbySearch_Release(search);
        search = nullptr;
    }

    // --- Login: create device ID (keep if it exists) → Connect login → create user the first time ---
    void login() {
        EOS_Connect_CreateDeviceIdOptions o = {};
        o.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
        o.DeviceModel = "PC Windows";
        EOS_Connect_CreateDeviceId(connect, &o, this, [](const EOS_Connect_CreateDeviceIdCallbackInfo* info) {
            Impl* self = static_cast<Impl*>(info->ClientData);
            if (info->ResultCode != EOS_EResult::EOS_Success && info->ResultCode != EOS_EResult::EOS_DuplicateNotAllowed) {
                MEALKIT_ERROR("EOS: could not create a device id (%s)", EOS_EResult_ToString(info->ResultCode));
                self->loginDone = true;
                return;
            }
            self->connectLogin(EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN, nullptr);
        });
    }

    void deviceIdLogin() {
        deviceLogin = true;
        login();
    }

    void connectLogin(EOS_EExternalCredentialType type, const char* token) {
        EOS_Connect_Credentials credentials = {};
        credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
        credentials.Type = type;
        credentials.Token = token;
        EOS_Connect_UserLoginInfo info = {};
        info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
        info.DisplayName = name.c_str(); // only used for device ID login (Steam uses the Steam name)
        EOS_Connect_LoginOptions o = {};
        o.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
        o.Credentials = &credentials;
        o.UserLoginInfo = type == EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN ? &info : nullptr;
        EOS_Connect_Login(connect, &o, this, [](const EOS_Connect_LoginCallbackInfo* result) {
            Impl* self = static_cast<Impl*>(result->ClientData);
            if (result->ResultCode == EOS_EResult::EOS_Success) {
                self->localUser = result->LocalUserId;
                self->loginDone = true;
            } else if (result->ResultCode == EOS_EResult::EOS_InvalidUser && result->ContinuanceToken) {
                EOS_Connect_CreateUserOptions c = {};
                c.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
                c.ContinuanceToken = result->ContinuanceToken;
                EOS_Connect_CreateUser(self->connect, &c, self, [](const EOS_Connect_CreateUserCallbackInfo* created) {
                    Impl* s = static_cast<Impl*>(created->ClientData);
                    if (created->ResultCode == EOS_EResult::EOS_Success) s->localUser = created->LocalUserId;
                    else MEALKIT_ERROR("EOS: could not create the user (%s)", EOS_EResult_ToString(created->ResultCode));
                    s->loginDone = true;
                });
            } else if (!self->deviceLogin) { // Steam login failed: Steam isn't in the portal's Identity Providers or the ticket is wrong
                MEALKIT_WARN("EOS: Steam login failed (%s) - add Steam under Identity Providers in the Epic developer "
                             "portal. Using the device id instead",
                             EOS_EResult_ToString(result->ResultCode));
                self->deviceIdLogin();
            } else {
                MEALKIT_ERROR("EOS: Connect login failed (%s)", EOS_EResult_ToString(result->ResultCode));
                self->loginDone = true;
            }
        });
    }

    // --- Lobby ---
    void setCodeAttribute() {
        EOS_Lobby_UpdateLobbyModificationOptions o = {};
        o.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
        o.LocalUserId = localUser;
        o.LobbyId = lobby.c_str();
        EOS_HLobbyModification modification = nullptr;
        if (EOS_Lobby_UpdateLobbyModification(lobbies, &o, &modification) != EOS_EResult::EOS_Success) {
            state = LobbyState::Failed;
            return;
        }
        EOS_Lobby_AttributeData data = {};
        data.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        data.Key = kCodeKey;
        data.Value.AsInt64 = static_cast<int64_t>(code);
        data.ValueType = EOS_EAttributeType::EOS_AT_INT64;
        EOS_LobbyModification_AddAttributeOptions a = {};
        a.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
        a.Attribute = &data;
        a.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
        EOS_LobbyModification_AddAttribute(modification, &a);
        EOS_Lobby_UpdateLobbyOptions u = {};
        u.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
        u.LobbyModificationHandle = modification;
        EOS_Lobby_UpdateLobby(lobbies, &u, this, [](const EOS_Lobby_UpdateLobbyCallbackInfo* info) {
            Impl* self = static_cast<Impl*>(info->ClientData);
            if (info->ResultCode != EOS_EResult::EOS_Success) {
                MEALKIT_ERROR("EOS: could not set the lobby code (%s)", EOS_EResult_ToString(info->ResultCode));
                self->state = LobbyState::Failed;
                return;
            }
            self->state = LobbyState::InLobby;
            self->refreshLobby();
            self->setPresence();
            MEALKIT_INFO("EOS: lobby created - lobby code %llu (others join with --steam-join %llu)",
                         static_cast<unsigned long long>(self->code), static_cast<unsigned long long>(self->code));
        });
        EOS_LobbyModification_Release(modification);
    }

    void joinFound(const EOS_LobbySearch_FindCallbackInfo* info) {
        EOS_LobbySearch_GetSearchResultCountOptions countOptions = {};
        countOptions.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
        if (info->ResultCode != EOS_EResult::EOS_Success || EOS_LobbySearch_GetSearchResultCount(search, &countOptions) == 0) {
            MEALKIT_ERROR("EOS: no lobby with code %llu (%s)", static_cast<unsigned long long>(code),
                          EOS_EResult_ToString(info->ResultCode));
            releaseSearch();
            state = LobbyState::Failed;
            return;
        }
        EOS_LobbySearch_CopySearchResultByIndexOptions c = {};
        c.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
        c.LobbyIndex = 0;
        EOS_HLobbyDetails details = nullptr;
        const EOS_EResult copied = EOS_LobbySearch_CopySearchResultByIndex(search, &c, &details);
        releaseSearch();
        if (copied != EOS_EResult::EOS_Success) {
            state = LobbyState::Failed;
            return;
        }
        EOS_Lobby_JoinLobbyOptions j = {};
        j.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
        j.LobbyDetailsHandle = details;
        j.LocalUserId = localUser;
        j.bPresenceEnabled = EOS_FALSE;
        EOS_Lobby_JoinLobby(lobbies, &j, this, [](const EOS_Lobby_JoinLobbyCallbackInfo* joined) {
            Impl* self = static_cast<Impl*>(joined->ClientData);
            if (joined->ResultCode != EOS_EResult::EOS_Success) {
                MEALKIT_ERROR("EOS: could not join the lobby (%s)", EOS_EResult_ToString(joined->ResultCode));
                self->state = LobbyState::Failed;
                return;
            }
            self->lobby = joined->LobbyId;
            self->state = LobbyState::InLobby;
            self->refreshLobby();
            self->setPresence();
            MEALKIT_INFO("EOS: joined lobby %llu (%u members)", static_cast<unsigned long long>(self->code), self->members);
        });
        EOS_LobbyDetails_Release(details);
    }

    class Transport; // EOS P2P transport (below)
};

// EOS P2P (NAT traversal, Epic relay if that fails). All connection requests are accepted; actual connection permission is checked by the lockstep session
class Service::Impl::Transport final : public net::Transport {
public:
    explicit Transport(Impl* m) : m_impl(m) {}

    bool send(const net::Address& to, const void* data, u32 size) override {
        const EOS_ProductUserId remote = m_impl->find(to.userId);
        if (!remote || size > EOS_P2P_MAX_PACKET_SIZE) return false;
        const EOS_P2P_SocketId socket = socketId();
        EOS_P2P_SendPacketOptions o = {};
        o.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
        o.LocalUserId = m_impl->localUser;
        o.RemoteUserId = remote;
        o.SocketId = &socket;
        o.Channel = 0;
        o.DataLengthBytes = size;
        o.Data = data;
        o.bAllowDelayedDelivery = EOS_TRUE; // don't drop the first packets while the connection is opening
        o.Reliability = EOS_EPacketReliability::EOS_PR_UnreliableUnordered;
        return EOS_P2P_SendPacket(m_impl->p2p, &o) == EOS_EResult::EOS_Success;
    }

    i32 receive(net::Address& from, void* buffer, u32 capacity) override {
        EOS_P2P_ReceivePacketOptions o = {};
        o.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
        o.LocalUserId = m_impl->localUser;
        o.MaxDataSizeBytes = capacity;
        EOS_ProductUserId peer = nullptr;
        EOS_P2P_SocketId socket = {};
        u8 channel = 0;
        uint32_t written = 0;
        if (EOS_P2P_ReceivePacket(m_impl->p2p, &o, &peer, &socket, &channel, buffer, &written) != EOS_EResult::EOS_Success)
            return -1;
        from = {};
        from.userId = m_impl->remember(peer);
        return static_cast<i32>(written);
    }

private:
    Impl* m_impl;
};

Service::Service() : m(new Impl()) {}
Service::~Service() {
    shutdown();
    delete m;
}

Backend Service::backend() { return Backend::EOS; }
const char* Service::backendName() { return "EOS"; }

bool Service::init() {
    if (m->available) return true;
    if (!m->initialized) {
        EOS_InitializeOptions o = {};
        o.ApiVersion = EOS_INITIALIZE_API_LATEST;
        o.ProductName = "MealKit";
        o.ProductVersion = "1";
        const EOS_EResult r = EOS_Initialize(&o);
        if (r != EOS_EResult::EOS_Success && r != EOS_EResult::EOS_AlreadyConfigured) {
            MEALKIT_ERROR("EOS: EOS_Initialize failed (%s)", EOS_EResult_ToString(r));
            return false;
        }
        EOS_Logging_SetCallback(&onLog);
        EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);
        m->initialized = true;
    }
    EOS_Platform_Options p = {};
    p.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
    p.ProductId = MEALKIT_EOS_PRODUCT_ID;
    p.SandboxId = MEALKIT_EOS_SANDBOX_ID;
    p.DeploymentId = MEALKIT_EOS_DEPLOYMENT_ID;
    p.ClientCredentials.ClientId = MEALKIT_EOS_CLIENT_ID;
    p.ClientCredentials.ClientSecret = MEALKIT_EOS_CLIENT_SECRET;
    p.Flags = EOS_PF_DISABLE_OVERLAY; // no Epic account/overlay
    m->platform = EOS_Platform_Create(&p);
    if (!m->platform) {
        MEALKIT_ERROR("EOS: EOS_Platform_Create failed - check third_party/eos/credentials.txt");
        return false;
    }
    m->connect = EOS_Platform_GetConnectInterface(m->platform);
    m->lobbies = EOS_Platform_GetLobbyInterface(m->platform);
    m->p2p = EOS_Platform_GetP2PInterface(m->platform);
    m->name = userName();
    if (m->name.empty()) m->name = "Player";
    m->loginDone = false;
    m->deviceLogin = false;
    const auto start = std::chrono::steady_clock::now();
    const auto waiting = [&] {
        return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count() < kLoginTimeoutSeconds;
    };

#if MEALKIT_EOS_STEAM
    // Steam account: web API ticket → EOS Connect (EOS_ECT_STEAM_SESSION_TICKET)
    if (!m->steam) m->steam = SteamAPI_Init();
    if (m->steam) {
        m->name = SteamFriends()->GetPersonaName();
        m->ticketDone = false;
        m->ticketHex.clear();
        m->ticket = SteamUser()->GetAuthTicketForWebApi(kSteamIdentity);
        while (!m->ticketDone && waiting()) {
            SteamAPI_RunCallbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } else {
        MEALKIT_WARN("EOS: SteamAPI_Init failed (is Steam running?) - logging in with the device id");
    }
    if (!m->ticketHex.empty())
        m->connectLogin(EOS_EExternalCredentialType::EOS_ECT_STEAM_SESSION_TICKET, m->ticketHex.c_str());
    else m->deviceIdLogin();
#else
    m->deviceIdLogin();
#endif

    // Wait for login (usually under 1 second; without internet, fails after kLoginTimeoutSeconds)
    while (!m->loginDone && waiting()) {
        EOS_Platform_Tick(m->platform);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!m->localUser) {
        if (!m->loginDone) MEALKIT_ERROR("EOS: login timed out (no internet?)");
        EOS_Platform_Release(m->platform);
        m->platform = nullptr;
        return false;
    }

    const EOS_P2P_SocketId socket = socketId();
    EOS_P2P_AddNotifyPeerConnectionRequestOptions n = {};
    n.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
    n.LocalUserId = m->localUser;
    n.SocketId = &socket;
    m->connectionRequest = EOS_P2P_AddNotifyPeerConnectionRequest(
        m->p2p, &n, m, [](const EOS_P2P_OnIncomingConnectionRequestInfo* info) {
            Impl* self = static_cast<Impl*>(info->ClientData);
            self->remember(info->RemoteUserId);
            EOS_P2P_AcceptConnectionOptions a = {};
            a.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
            a.LocalUserId = info->LocalUserId;
            a.RemoteUserId = info->RemoteUserId;
            a.SocketId = info->SocketId;
            EOS_P2P_AcceptConnection(self->p2p, &a);
        });
    m->remember(m->localUser);
    m->available = true;
    MEALKIT_INFO("EOS: logged in with %s as %s (user %llu)", m->deviceLogin ? "the device id" : "Steam",
                 m->name.c_str(),
                 static_cast<unsigned long long>(hashUserId(m->localUser)));
    return true;
}

void Service::shutdown() {
    if (m->platform) {
        leaveLobby();
        m->releaseSearch();
        if (m->connectionRequest != EOS_INVALID_NOTIFICATIONID)
            EOS_P2P_RemoveNotifyPeerConnectionRequest(m->p2p, m->connectionRequest);
        m->connectionRequest = EOS_INVALID_NOTIFICATIONID;
        for (int i = 0; i < 20; ++i) EOS_Platform_Tick(m->platform); // sends the leave lobby request
        EOS_Platform_Release(m->platform);
        m->platform = nullptr;
    }
    m->available = false;
    m->localUser = nullptr;
    m->users.clear();
#if MEALKIT_EOS_STEAM
    if (m->steam) {
        if (m->ticket != k_HAuthTicketInvalid) SteamUser()->CancelAuthTicket(m->ticket);
        m->ticket = k_HAuthTicketInvalid;
        SteamFriends()->ClearRichPresence();
        SteamAPI_Shutdown();
        m->steam = false;
    }
#endif
    if (m->initialized) {
        EOS_Shutdown();
        m->initialized = false; // after EOS_Shutdown it can't be reinitialized in this process
    }
}

void Service::update() {
    if (!m->platform) return;
    EOS_Platform_Tick(m->platform);
#if MEALKIT_EOS_STEAM
    if (m->steam) SteamAPI_RunCallbacks();
#endif
    m->refreshLobby();
}

bool Service::available() const { return m->available; }
u64 Service::localUserId() const { return m->available ? hashUserId(m->localUser) : 0; }
std::string Service::displayName() const { return m->available ? m->name : std::string(); }

void Service::createLobby(u32 maxMembers) {
    if (!m->available) {
        m->state = LobbyState::Failed;
        return;
    }
    m->state = LobbyState::Creating;
    std::random_device device; // lobby code only (unrelated to the simulation)
    m->code = 100000 + (static_cast<u64>(device()) % 900000); // six digits a person can read out
    EOS_Lobby_CreateLobbyOptions o = {};
    o.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
    o.LocalUserId = m->localUser;
    o.MaxLobbyMembers = maxMembers;
    o.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED; // must be visible in searches to be found by code
    o.bPresenceEnabled = EOS_FALSE;
    o.bAllowInvites = EOS_TRUE;
    o.BucketId = kBucket;
    EOS_Lobby_CreateLobby(m->lobbies, &o, m, [](const EOS_Lobby_CreateLobbyCallbackInfo* info) {
        Service::Impl* self = static_cast<Service::Impl*>(info->ClientData);
        if (info->ResultCode != EOS_EResult::EOS_Success) {
            MEALKIT_ERROR("EOS: could not create a lobby (%s)", EOS_EResult_ToString(info->ResultCode));
            self->state = LobbyState::Failed;
            return;
        }
        self->lobby = info->LobbyId;
        self->setCodeAttribute();
    });
}

void Service::joinLobby(u64 lobbyId) {
    if (!m->available || lobbyId == 0) {
        m->state = LobbyState::Failed;
        return;
    }
    m->state = LobbyState::Joining;
    m->code = lobbyId;
    m->releaseSearch();
    EOS_Lobby_CreateLobbySearchOptions o = {};
    o.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
    o.MaxResults = 1;
    if (EOS_Lobby_CreateLobbySearch(m->lobbies, &o, &m->search) != EOS_EResult::EOS_Success) {
        m->state = LobbyState::Failed;
        return;
    }
    EOS_Lobby_AttributeData data = {};
    data.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
    data.Key = kCodeKey;
    data.Value.AsInt64 = static_cast<int64_t>(lobbyId);
    data.ValueType = EOS_EAttributeType::EOS_AT_INT64;
    EOS_LobbySearch_SetParameterOptions p = {};
    p.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
    p.Parameter = &data;
    p.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
    EOS_LobbySearch_SetParameter(m->search, &p);
    EOS_LobbySearch_FindOptions f = {};
    f.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
    f.LocalUserId = m->localUser;
    EOS_LobbySearch_Find(m->search, &f, m, [](const EOS_LobbySearch_FindCallbackInfo* info) {
        static_cast<Service::Impl*>(info->ClientData)->joinFound(info);
    });
}

void Service::leaveLobby() {
    if (m->available && m->state == LobbyState::InLobby) {
        EOS_Lobby_LeaveLobbyOptions o = {};
        o.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
        o.LocalUserId = m->localUser;
        o.LobbyId = m->lobby.c_str();
        EOS_Lobby_LeaveLobby(m->lobbies, &o, nullptr, [](const EOS_Lobby_LeaveLobbyCallbackInfo*) {});
    }
    m->state = LobbyState::None;
    m->setPresence();
    m->lobby.clear();
    m->owner = 0;
    m->members = 0;
}

void Service::inviteFriends() {
#if MEALKIT_EOS_STEAM
    if (m->available && m->state == LobbyState::InLobby && m->steam) {
        const std::string connectString = kConnectPrefix + std::to_string(m->code);
        SteamFriends()->ActivateGameOverlayInviteDialogConnectString(connectString.c_str());
    }
#endif
    if (m->available && m->state == LobbyState::InLobby)
        MEALKIT_INFO("EOS: lobby code %llu - share it, others join with --steam-join %llu",
                     static_cast<unsigned long long>(m->code), static_cast<unsigned long long>(m->code));
}

LobbyState Service::lobbyState() const { return m->state; }
bool Service::isLobbyOwner() const { return m->state == LobbyState::InLobby && m->owner && m->owner == localUserId(); }
u64 Service::lobbyId() const { return m->state == LobbyState::InLobby ? m->code : 0; }
u32 Service::lobbyMemberCount() const { return m->members; }
net::Address Service::lobbyOwnerAddress() const {
    net::Address address;
    address.userId = m->owner;
    return address;
}

void Service::setLobbyOwner(u64 userId) {
    if (!isLobbyOwner() || userId == 0 || userId == localUserId()) return;
    const EOS_ProductUserId target = m->find(userId);
    if (!target) return;
    EOS_Lobby_PromoteMemberOptions o = {};
    o.ApiVersion = EOS_LOBBY_PROMOTEMEMBER_API_LATEST;
    o.LobbyId = m->lobby.c_str();
    o.LocalUserId = m->localUser;
    o.TargetUserId = target;
    EOS_Lobby_PromoteMember(m->lobbies, &o, nullptr, [](const EOS_Lobby_PromoteMemberCallbackInfo* info) {
        if (info->ResultCode != EOS_EResult::EOS_Success)
            MEALKIT_WARN("EOS: lobby owner change failed (%s)", EOS_EResult_ToString(info->ResultCode));
    });
    MEALKIT_INFO("EOS lobby owner -> %llu (host migration)", static_cast<unsigned long long>(userId));
}

u64 Service::takeJoinRequest() {
    const u64 id = m->pendingJoin;
    m->pendingJoin = 0;
    return id;
}

std::unique_ptr<net::Transport> Service::createTransport() {
    if (!m->available) return nullptr;
    return std::make_unique<Impl::Transport>(m);
}

} // namespace mealkit::online
