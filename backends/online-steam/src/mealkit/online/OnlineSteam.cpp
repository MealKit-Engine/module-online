// Steam 백엔드 (Steamworks SDK가 있는 게임 빌드에서만 컴파일된다, MEALKIT_STEAM=1).
#include "mealkit/online/Online.h"

#include "mealkit/core/Log.h"

#include <cstring>
#include <deque>

#pragma warning(push, 0)
#include <steam/isteamnetworkingmessages.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steam_api.h>
#pragma warning(pop)

namespace mealkit::online {
namespace {

// Steam 릴레이로 메시지를 주고받는다. 세션 요청은 모두 받아들이고, 실제 접속 허가는 락스텝 세션이
// 빌드/씬 해시로 검사한다.
class SteamTransport final : public net::Transport {
public:
    SteamTransport() : m_sessionRequest(this, &SteamTransport::onSessionRequest) {}

    ~SteamTransport() override {
        for (SteamNetworkingMessage_t* msg : m_pending) msg->Release();
    }

    bool send(const net::Address& to, const void* data, u32 size) override {
        if (!to.steamId) return false;
        SteamNetworkingIdentity identity;
        identity.SetSteamID64(to.steamId);
        const EResult r = SteamNetworkingMessages()->SendMessageToUser(
            identity, data, size,
            k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession, 0);
        return r == k_EResultOK;
    }

    i32 receive(net::Address& from, void* buffer, u32 capacity) override {
        if (m_pending.empty()) {
            SteamNetworkingMessage_t* messages[64];
            const int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(0, messages, 64);
            for (int i = 0; i < count; ++i) m_pending.push_back(messages[i]);
        }
        if (m_pending.empty()) return -1;
        SteamNetworkingMessage_t* msg = m_pending.front();
        m_pending.pop_front();
        const u32 size = static_cast<u32>(msg->m_cbSize) < capacity ? static_cast<u32>(msg->m_cbSize) : capacity;
        std::memcpy(buffer, msg->m_pData, size);
        from = {};
        from.steamId = msg->m_identityPeer.GetSteamID64();
        msg->Release();
        return static_cast<i32>(size);
    }

private:
    void onSessionRequest(SteamNetworkingMessagesSessionRequest_t* request) {
        SteamNetworkingMessages()->AcceptSessionWithUser(request->m_identityRemote);
    }

    CCallback<SteamTransport, SteamNetworkingMessagesSessionRequest_t> m_sessionRequest;
    std::deque<SteamNetworkingMessage_t*> m_pending;
};

} // namespace

struct Service::Impl {
    bool available = false;
    LobbyState state = LobbyState::None;
    CSteamID lobby;
    u64 pendingJoin = 0;

    CCallResult<Impl, LobbyCreated_t> created;
    CCallResult<Impl, LobbyEnter_t> entered;
    CCallback<Impl, GameLobbyJoinRequested_t> joinRequested;

    Impl() : joinRequested(this, &Impl::onJoinRequested) {}

    void onCreated(LobbyCreated_t* result, bool ioFailure) {
        if (ioFailure || result->m_eResult != k_EResultOK) {
            MEALKIT_ERROR("Steam: could not create a lobby (result %d)",
                          ioFailure ? -1 : static_cast<int>(result->m_eResult));
            state = LobbyState::Failed;
            return;
        }
        lobby = CSteamID(result->m_ulSteamIDLobby);
        SteamMatchmaking()->SetLobbyData(lobby, "engine", "mealkit");
        state = LobbyState::InLobby;
        MEALKIT_INFO("Steam: lobby %llu created", static_cast<unsigned long long>(result->m_ulSteamIDLobby));
    }

    void onEntered(LobbyEnter_t* result, bool ioFailure) {
        if (ioFailure || result->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
            MEALKIT_ERROR("Steam: could not join the lobby");
            state = LobbyState::Failed;
            return;
        }
        lobby = CSteamID(result->m_ulSteamIDLobby);
        state = LobbyState::InLobby;
        MEALKIT_INFO("Steam: joined lobby %llu", static_cast<unsigned long long>(result->m_ulSteamIDLobby));
    }

    void onJoinRequested(GameLobbyJoinRequested_t* request) { pendingJoin = request->m_steamIDLobby.ConvertToUint64(); }

    u64 owner() const {
        return state == LobbyState::InLobby ? SteamMatchmaking()->GetLobbyOwner(lobby).ConvertToUint64() : 0;
    }
};

Service::Service() : m(new Impl()) {}
Service::~Service() {
    shutdown();
    delete m;
}

Backend Service::backend() { return Backend::Steam; }
const char* Service::backendName() { return "Steam"; }

bool Service::init() {
    if (m->available) return true;
    if (!SteamAPI_Init()) {
        MEALKIT_ERROR("Steam: SteamAPI_Init failed - is the Steam client running and steam_appid.txt next to the exe?");
        return false;
    }
    SteamNetworkingUtils()->InitRelayNetworkAccess(); // 릴레이 경로를 미리 준비해 첫 접속을 빠르게
    m->available = true;
    MEALKIT_INFO("Steam: logged in as %s (app %u)", SteamFriends()->GetPersonaName(), SteamUtils()->GetAppID());
    return true;
}

void Service::shutdown() {
    if (!m->available) return;
    leaveLobby();
    SteamAPI_Shutdown();
    m->available = false;
}

void Service::update() {
    if (m->available) SteamAPI_RunCallbacks();
}

bool Service::available() const { return m->available; }
u64 Service::localUserId() const { return m->available ? SteamUser()->GetSteamID().ConvertToUint64() : 0; }
std::string Service::displayName() const { return m->available ? SteamFriends()->GetPersonaName() : std::string(); }

void Service::createLobby(u32 maxMembers) {
    if (!m->available) {
        m->state = LobbyState::Failed;
        return;
    }
    m->state = LobbyState::Creating;
    m->created.Set(SteamMatchmaking()->CreateLobby(k_ELobbyTypeFriendsOnly, static_cast<int>(maxMembers)), m,
                   &Impl::onCreated);
}

void Service::joinLobby(u64 lobbyId) {
    if (!m->available) {
        m->state = LobbyState::Failed;
        return;
    }
    m->state = LobbyState::Joining;
    m->entered.Set(SteamMatchmaking()->JoinLobby(CSteamID(lobbyId)), m, &Impl::onEntered);
}

void Service::leaveLobby() {
    if (m->available && m->state == LobbyState::InLobby) SteamMatchmaking()->LeaveLobby(m->lobby);
    m->state = LobbyState::None;
}

void Service::inviteFriends() {
    if (m->available && m->state == LobbyState::InLobby) SteamFriends()->ActivateGameOverlayInviteDialog(m->lobby);
}

LobbyState Service::lobbyState() const { return m->state; }
bool Service::isLobbyOwner() const { return m->state == LobbyState::InLobby && m->owner() == localUserId(); }
u64 Service::lobbyId() const { return m->state == LobbyState::InLobby ? m->lobby.ConvertToUint64() : 0; }
u32 Service::lobbyMemberCount() const {
    return m->state == LobbyState::InLobby ? static_cast<u32>(SteamMatchmaking()->GetNumLobbyMembers(m->lobby)) : 0;
}
net::Address Service::lobbyOwnerAddress() const {
    net::Address address;
    address.steamId = m->owner();
    return address;
}

void Service::setLobbyOwner(u64 userId) {
    if (!isLobbyOwner() || userId == 0 || userId == localUserId()) return;
    if (SteamMatchmaking()->SetLobbyOwner(m->lobby, CSteamID(userId)))
        MEALKIT_INFO("Steam lobby owner -> %llu (host migration)", static_cast<unsigned long long>(userId));
}

u64 Service::takeJoinRequest() {
    const u64 id = m->pendingJoin;
    m->pendingJoin = 0;
    return id;
}

std::unique_ptr<net::Transport> Service::createTransport() {
    if (!m->available) return nullptr;
    return std::make_unique<SteamTransport>();
}

} // namespace mealkit::online
