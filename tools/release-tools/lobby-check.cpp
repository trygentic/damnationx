#include "dvlnet/lobby_api.h"
#include <cstdio>
#include <cstdlib>
int main() {
    using namespace devilution::net::lobby;
    if (std::getenv("DAMNATIONX_LOBBY_ORIGIN")) return 2;
    if (Origin() != "https://lobby.damnationx.net") return 3;
    std::vector<Game> games;
    std::string error;
    if (!List(games, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    std::printf("Verified GET %s/api/v1/games (%zu public games)\n", Origin().c_str(), games.size());
    return 0;
}
