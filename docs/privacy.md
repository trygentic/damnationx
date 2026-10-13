# DamnationX privacy

Single-player saves and settings stay on your device or in your browser's local
storage. The public lobby needs some information to list games and connect
players. This page describes the DamnationX lobby; other DevilutionX network
providers have their own behavior.

## Public lobby

When you create or join a lobby game, the service receives your network address.
It stores the host's address (IPv6 is grouped by /64), lobby name, host hero
name, game version, mod identity, transport choice, and game status. It stores
join addresses and times with short-lived room tokens. These records support
game discovery, admission limits, and bans. Ended game records and join records
are deleted after 24 hours. Active bans remain until they expire or an operator
removes them.

Public games appear in the in-game list. Lobby Bot also posts their lobby name,
host hero name, player count, and join link to the configured Discord channel.
It edits the post when the game fills or ends and attempts to delete it about
five minutes after the game ends. Private games are not listed or posted.

## How players connect

Browser games use WebRTC by default. The lobby forwards connection setup
messages and uses a STUN server to find a direct path. Other players may learn
your public network address through that process, and the STUN provider receives
the address used to contact it. Game traffic then travels directly between
players. The lobby does not record gameplay, chat, or a player action log.

Hosts can choose the WebSocket relay instead. In that mode, game traffic passes
through the lobby service, which forwards packets without storing their
contents. Peers do not receive one another's network addresses from the relay.
The service does not currently offer a TURN fallback for WebRTC connections
that cannot connect directly.

For questions or deletion requests about lobby records, contact the operator
of the DamnationX server you used. Removing local saves and settings is done
through your device or browser storage controls.
