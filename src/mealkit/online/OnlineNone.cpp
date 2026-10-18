// 온라인 백엔드 없음: Steamworks SDK가 없는 빌드와 Editor 빌드. 로비는 없고 접속은 UDP(IP 직접)로 한다.
// third_party/steamworks/sdk 에 SDK를 풀면 OnlineSteam.cpp가 대신 빌드된다.
#include "mealkit/online/Online.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/Version.h"

namespace mealkit::online {

struct Service::Impl {
    LobbyState state = LobbyState::None;
};

Service::Service() : m(new Impl()) {}
Service::~Service() { delete m; }

Backend Service::backend() { return Backend::None; }
const char* Service::backendName() { return "none"; }

bool Service::init() {
    MEALKIT_WARN("Online: this build has no online backend (Steam needs the Steamworks SDK, see " MEALKIT_DOCS_URL
                 "manual/multiplayer.html#steamworks)");
    return false;
}
void Service::shutdown() {}
void Service::update() {}
bool Service::available() const { return false; }
u64 Service::localUserId() const { return 0; }
std::string Service::displayName() const { return {}; }

void Service::createLobby(u32) { m->state = LobbyState::Failed; }
void Service::joinLobby(u64) { m->state = LobbyState::Failed; }
void Service::leaveLobby() { m->state = LobbyState::None; }
void Service::inviteFriends() {}
LobbyState Service::lobbyState() const { return m->state; }
bool Service::isLobbyOwner() const { return false; }
u64 Service::lobbyId() const { return 0; }
u32 Service::lobbyMemberCount() const { return 0; }
net::Address Service::lobbyOwnerAddress() const { return {}; }
u64 Service::takeJoinRequest() { return 0; }
std::unique_ptr<net::Transport> Service::createTransport() { return nullptr; }

} // namespace mealkit::online
