// Steamworks 구현 (MEALKIT_STEAM 빌드에서만 컴파일된다).
#include "mealkit/platform/Steam.h"

#include "mealkit/core/Log.h"

#include <cstdlib>
#include <cstring>
#include <deque>

#pragma warning(push, 0)
#include <steam/isteamnetworkingmessages.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steam_api.h>
#pragma warning(pop)

namespace mealkit::steam {
namespace {

bool g_available = false;

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

bool compiledIn() { return true; }

bool init() {
    if (g_available) return true;
    if (!SteamAPI_Init()) {
        MEALKIT_ERROR("Steam: SteamAPI_Init failed - is the Steam client running and steam_appid.txt next to the exe?");
        return false;
    }
    SteamNetworkingUtils()->InitRelayNetworkAccess(); // 릴레이 경로를 미리 준비해 첫 접속을 빠르게
    g_available = true;
    MEALKIT_INFO("Steam: logged in as %s (app %u)", SteamFriends()->GetPersonaName(), SteamUtils()->GetAppID());
    return true;
}

void shutdown() {
    if (!g_available) return;
    SteamAPI_Shutdown();
    g_available = false;
}

void runCallbacks() {
    if (g_available) SteamAPI_RunCallbacks();
}

bool available() { return g_available; }
u64 localSteamId() { return g_available ? SteamUser()->GetSteamID().ConvertToUint64() : 0; }
std::string personaName() { return g_available ? SteamFriends()->GetPersonaName() : std::string(); }

std::unique_ptr<net::Transport> createTransport() {
    if (!g_available) return nullptr;
    return std::make_unique<SteamTransport>();
}

// ---------------------------------------------------------------- 로비

struct Lobby::Impl {
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
};

Lobby::Lobby() : m(new Impl()) {}
Lobby::~Lobby() {
    leave();
    delete m;
}

void Lobby::create(u32 maxMembers) {
    if (!g_available) {
        m->state = LobbyState::Failed;
        return;
    }
    m->state = LobbyState::Creating;
    m->created.Set(SteamMatchmaking()->CreateLobby(k_ELobbyTypeFriendsOnly, static_cast<int>(maxMembers)), m,
                   &Impl::onCreated);
}

void Lobby::join(u64 lobbyId) {
    if (!g_available) {
        m->state = LobbyState::Failed;
        return;
    }
    m->state = LobbyState::Joining;
    m->entered.Set(SteamMatchmaking()->JoinLobby(CSteamID(lobbyId)), m, &Impl::onEntered);
}

void Lobby::leave() {
    if (g_available && m->state == LobbyState::InLobby) SteamMatchmaking()->LeaveLobby(m->lobby);
    m->state = LobbyState::None;
}

void Lobby::inviteFriends() {
    if (g_available && m->state == LobbyState::InLobby) SteamFriends()->ActivateGameOverlayInviteDialog(m->lobby);
}

LobbyState Lobby::state() const { return m->state; }
bool Lobby::isOwner() const { return m->state == LobbyState::InLobby && ownerSteamId() == localSteamId(); }
u64 Lobby::lobbyId() const { return m->state == LobbyState::InLobby ? m->lobby.ConvertToUint64() : 0; }
u64 Lobby::ownerSteamId() const {
    return m->state == LobbyState::InLobby ? SteamMatchmaking()->GetLobbyOwner(m->lobby).ConvertToUint64() : 0;
}
u32 Lobby::memberCount() const {
    return m->state == LobbyState::InLobby ? static_cast<u32>(SteamMatchmaking()->GetNumLobbyMembers(m->lobby)) : 0;
}

u64 Lobby::takeJoinRequest() {
    const u64 id = m->pendingJoin;
    m->pendingJoin = 0;
    return id;
}

u64 Lobby::parseLaunchLobby(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "+connect_lobby") == 0) return std::strtoull(argv[i + 1], nullptr, 10);
    return 0;
}

} // namespace mealkit::steam
