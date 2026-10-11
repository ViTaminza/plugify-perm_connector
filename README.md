# perm_connector (Plugify)

Links CS2 players (s2sdk) to users of the official 'permissions' plugin. Requests permissions::LoadUser for every connected, authorised human player (from the s2sdk OnClientAuthenticated event and from a once-per-second reconcile, which also covers late plugin load and the fact that OnClientAuthenticated does not reliably fire on a real server), logs OnUserLoaded per slot, removes the user from the core again on disconnect (DeleteUser - the core has no unload API) and triggers the one-time server group load (LoadGroups) so a storage plugin can populate groups. Adds the live-inspection console commands 'perm_status' and 'perm_cookie'.

Original plugin (not a port).

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, permissions

## Installation
Download `perm_connector-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/perm_connector`.

## License
GPL-3.0, see LICENSE.

