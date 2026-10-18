#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/net/Transport.h"

#include <memory>
#include <string>

// 온라인 서비스 계층 (docs/CODING_MODEL.md "온라인 기능"): 계정, 친구 초대, 로비, P2P 릴레이 전송.
// 게임과 엔진의 나머지는 플랫폼 SDK를 직접 부르지 않고 이것만 쓴다.
//
// 백엔드는 빌드 때 하나를 고른다 (구현은 백엔드별 .cpp, CMake가 고른다):
//   Steam  third_party/steamworks/sdk 가 있는 게임 빌드 (OnlineSteam.cpp). 개발 중에는 앱 ID 480(Spacewar, steam_appid.txt)
//   None   SDK가 없는 빌드와 Editor 빌드 (OnlineNone.cpp). 로비가 없고, 접속은 net::UdpTransport(IP 직접)로 한다
// EOS(3단계)와 콘솔 플랫폼 서비스(5단계)도 여기에 백엔드로 더한다. 업적·통계·리더보드(4단계)와 클라우드 세이브(1단계 "세이브")는
// 그 항목에서 이 클래스에 더한다.
//
// 플랫폼 SDK가 프로세스 전역이라 한 프로세스에 Service는 하나만 만든다. main이 만들어 필요한 곳에 인자로 넘긴다.
namespace mealkit::online {

enum class Backend : u8 { None, Steam };

enum class LobbyState : u8 { None, Creating, Joining, InLobby, Failed };

class Service {
public:
    Service();
    ~Service(); // shutdown()
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    static Backend backend(); // 이 빌드에 들어간 백엔드
    static const char* backendName();

    // 백엔드에 로그인한다 (Steam: 클라이언트가 실행 중이어야 한다). None 백엔드는 항상 false.
    bool init();
    void shutdown();
    void update();          // 매 프레임 호출 (콜백 처리)
    bool available() const; // init 성공

    // 계정
    u64 localUserId() const;
    std::string displayName() const;

    // 친구 전용 로비. 방장이 락스텝 호스트가 되고, 참가자는 lobbyOwnerAddress()로 접속한다.
    void createLobby(u32 maxMembers);
    void joinLobby(u64 lobbyId);
    void leaveLobby();
    void inviteFriends(); // 플랫폼 오버레이의 초대 창
    LobbyState lobbyState() const;
    bool isLobbyOwner() const;
    u64 lobbyId() const;
    u32 lobbyMemberCount() const;
    net::Address lobbyOwnerAddress() const;

    // 친구 초대를 수락해 들어온 로비 참가 요청. 없으면 0.
    u64 takeJoinRequest();

    // 플랫폼 릴레이 P2P 전송 (NAT·방화벽을 플랫폼이 처리한다). 사용 불가면 nullptr.
    std::unique_ptr<net::Transport> createTransport();

private:
    struct Impl;
    Impl* m;
};

} // namespace mealkit::online
