# GodotSteam for Godot Engine 3.x | Community Edition
An ecosystem of tools for [Godot Engine](https://godotengine.org) and [Valve's Steam](https://store.steampowered.com). For the Windows, Linux, and Mac platforms.

**Notice: GodotSteam for Godot 3.x is now semi-retired. This branch still works fine but will not receive any further feature updates.  However, it will receive necessary bug fixes and builds will be produced for new Godot 3.x versions.**


Additional Flavors
---
Standard Module | Standard Plug-ins | Server Module | Server Plug-ins | Tools | Examples
--- | --- | --- | --- | --- | ---
[Godot 2.x](https://codeberg.org/godotsteam/godotsteam/src/branch/godot2) | [GDNative](https://codeberg.org/godotsteam/godotsteam/src/branch/gdnative) | [Server 3.x](https://codeberg.org/godotsteam/godotsteam-server/src/branch/godot3) | [GDNative](https://codeberg.org/godotsteam/godotsteam-server/src/branch/gdnative) | [GodotSteamKit](https://godotsteam.com/projects/godotsteamkit) | [Skillet](https://codeberg.org/godotsteam/skillet)
[Godot 3.x](https://codeberg.org/godotsteam/godotsteam/src/branch/godot3) | [GDExtension](https://codeberg.org/godotsteam/godotsteam/src/branch/gdextension) | [Server 4.x](https://codeberg.org/godotsteam/godotsteam-server/src/branch/godot4) | [GDExtension](https://codeberg.org/godotsteam/godotsteam-server/src/branch/gdextension) | --- | [Skillet UGC Editor](https://codeberg.org/godotsteam/skillet/src/branch/ugc_editor)
[Godot 4.x](https://codeberg.org/godotsteam/godotsteam/src/branch/godot4) | --- | --- | --- | --- | ---
[MultiplayerPeer](https://codeberg.org/godotsteam/multiplayerpeer)| --- | --- | --- | --- | ---


Documentation
---
[Documentation is available here](https://godotsteam.com).  You can also check out the Search Help section inside Godot Engine.  [To start, try checking out our tutorial on initializing Steam.](https://godotsteam.com/tutorials/initializing/)  There are additional tutorials, with more in the works.  You can also [check out additional Godot and Steam related videos, text, additional tools, plug-ins, etc. here.](https://godotsteam.com/resources/external/)

Feel free to chat with us about GodotSteam or ask for assistance on the [Stoat server](https://stt.gg/9DxQ3Dcd) or [IRC on Libera Chat](irc://irc.libera.chat/#godotsteam).


Donate
---
Pull-requests are the best way to help the project out but you can also donate through [Github Sponsors](https://github.com/sponsors/Gramps) or [LiberaPay](https://liberapay.com/godotsteam/donate)! [You can read more about donor perks here.](https://godotsteam.com/contribute/donations/)  [You can also view all our awesome donors here.](https://godotsteam.com/contribute/donors/)


Current Build
---
You can [download pre-compiled versions of this repo here](https://codeberg.org/godotsteam/godotsteam/releases).

**Version 3.31**
- Added: new enums to Result, HTTPStatusCode, RemotePlayScanCode, MarketNotAllowedReasonFlags, ActionOrigin per Steam SDK 1.63
- Added: `getDecompressedVoice()` as custom function to wrap up `getVoice()` and `decompressVoice()` in C++
- Changed: converted functions entirely over to the Flat API system to fix MinGW compatibility
- Changed: `activateGameOverlayInviteDialog()` changed argument name from steam_id to lobby_id for clarity
- Changed: renamed some minor parameters
- Changed: `getAPICallFailureReason()` now returns enum instead of string
- Changed: error messages if Steam is not initialized or classes are missing
- Fixed: `initFilterText()` now takes filter options
- Fixed: VOICE_RESULT_NO_DATA incorrectly named VOICE_RESULT_NO_DATE
- Removed: GameSearch and Music Remote classes, constants, enums per Steam SDK 1.63

[You can read more change-logs here](https://godotsteam.com/changelog/godot3/).


Compatibility
---
While rare, sometimes Steamworks SDK updates will break compatilibity with older GodotSteam versions. Any compatability breaks are noted below. Newer API files (dll, so, dylib) _should_ still work for older versions.

Steamworks SDK Version | GodotSteam Version
---|---
1.63 or newer | 3.31 or newer
1.62 | 3.29 to 3.30.2
1.61 | 3.28
1.59 to 1.60 | 3.23 to 3.27
1.53 to 1.58a | 3.12 to 3.22.4
1.52 or older | 3.11.1 or older

Versions of GodotSteam that have compatibility breaks introduced.

GodotSteam Version | Broken Compatibility
---|---
3.25 | Networking identity system removed, replaced with Steam IDs
3.26 | sendMessages returns an Array
3.27 | setLeaderboardDetailsMax removed
3.29 | getItemDefinitionProperty return a dictionary, html_needs_paint key 'bgra' changed to 'rbga', removed first argument for stat request in steamInit and steamInitEx, steamInit returns intended bool value
3.30 | Variety of small break points, refer to [3.30 changelog for details](https://godotsteam.com/changelog/godot3/)
3.31 | Windows projects using Steam SDK 1.63 are meant to work with Proton 11 or Experimental on Linux / Steam Deck.


Quick How-To
---
For complete instructions on how to build the Godot 3.x version of GodotSteam from scratch, [please refer to our documentation's 'How-To Modules' section.](https://godotsteam.com/howto/modules/) It will have the most up-to-date information.

Alternatively, you can just [download the pre-compiled versions in our Releases section](https://codeberg.org/godotsteam/godotsteam/releases) and skip compiling it yourself!


License
---
MIT license
