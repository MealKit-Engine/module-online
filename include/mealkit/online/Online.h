#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/net/Transport.h"

#include <memory>
#include <string>

// Online services layer (docs/CODING_MODEL.md "Online features"): accounts, friend invites, lobbies, P2P relay transport.
// The game and the rest of the engine don't call platform SDKs directly; they only use this.
//
// One backend is chosen at build time (implementations in per-backend .cpp files, chosen by CMake):
//   Steam  game builds with third_party/steamworks/sdk (OnlineSteam.cpp). During development app ID 480 (Spacewar, steam_appid.txt)
//   EOS    game builds with third_party/eos/sdk + credentials.txt (OnlineEOS.cpp). EOS Connect login with device ID (no Epic account),
//          lobby id is a numeric code chosen by the host (lobby attribute MKCODE), user id is a 64-bit hash of the Product User ID string
//   None   builds without an SDK and Editor builds (OnlineNone.cpp). No lobbies; connecting uses net::UdpTransport (direct IP)
// Selected by CMake MEALKIT_ONLINE (Auto: Steam first). Console platform services (stage 5) are also added here as backends. Achievements/stats/leaderboards (stage 4) and cloud saves (stage 1 "Saves")
// are added to this class in those items.
//
// Platform SDKs are process-global, so only one Service is made per process. main creates it and passes it as an argument where needed.
namespace mealkit::online {

enum class Backend : u8 { None, Steam, EOS };

enum class LobbyState : u8 { None, Creating, Joining, InLobby, Failed };

class Service {
public:
    Service();
    ~Service(); // shutdown()
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    static Backend backend(); // backend included in this build
    static const char* backendName();

    // Logs into the backend (Steam: the client must be running, EOS: internet, waits up to a few seconds). Always false for the None backend.
    bool init();
    void shutdown();
    void update();          // call every frame (processes callbacks)
    bool available() const; // init succeeded

    // Account
    u64 localUserId() const;
    std::string displayName() const;

    // Friends-only lobby. The host becomes the lockstep host, and participants connect via lobbyOwnerAddress().
    void createLobby(u32 maxMembers);
    void joinLobby(u64 lobbyId);
    void leaveLobby();
    void inviteFriends(); // invite window of the platform overlay (EOS: no friend list, so the lobby code is written to the log)
    LobbyState lobbyState() const;
    bool isLobbyOwner() const;
    u64 lobbyId() const;
    u32 lobbyMemberCount() const;
    net::Address lobbyOwnerAddress() const;
    // Transfers lobby ownership (owner only, host migration: matches the new owner chosen by the platform with the lockstep successor)
    void setLobbyOwner(u64 userId);

    // Lobby join request from accepting a friend invite. 0 if none.
    u64 takeJoinRequest();

    // Platform relay P2P transport (the platform handles NAT/firewalls). nullptr if unavailable.
    std::unique_ptr<net::Transport> createTransport();

private:
    struct Impl;
    Impl* m;
};

} // namespace mealkit::online
