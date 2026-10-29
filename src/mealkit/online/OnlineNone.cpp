// No online backend: builds without the Steamworks SDK and Editor builds. No lobbies; connecting uses UDP (direct IP).
// Extracting the SDK to third_party/steamworks/sdk builds OnlineSteam.cpp instead.
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
void Service::setLobbyOwner(u64) {}
u64 Service::takeJoinRequest() { return 0; }
std::unique_ptr<net::Transport> Service::createTransport() { return nullptr; }

} // namespace mealkit::online
