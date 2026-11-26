// This fork's: Epic Online Services, offline. The host never loads an online SDK's NRO (hle_ro.c), so a game's EOS_* imports stay stubs that return 0. Hades logs
// "[EOS] Failed to create platform" and plays on. MARVEL Cosmic Invasion (FNA; its C# is compiled ahead of time) takes the null platform and reads through it at
// main+0xc2bea0: a null check its runtime turns into an exception on the console (nn::os::SetUserExceptionHandler), which the host does not deliver, so the game
// died before its first frame. Here the platform and its interfaces exist as handles that do nothing: nothing is ever logged in, no notification ever fires, and
// the polls a game makes every frame answer "nothing" without a log line. Asynchronous calls (logins, queries, lobbies) stay unimplemented stubs: their callbacks
// are never called, which only an online menu waits for.
#include "hle.h"

static uint64_t g_eos_obj[40];   // the handles: addresses the game only passes back
static _Atomic uint64_t g_eos_note = 1;   // notification ids (0 is EOS_INVALID_NOTIFICATIONID)

HLE("EOS_Initialize", int, h_eos_initialize, const void *o) { (void)o; LOG("[eos] Initialize: Epic Online Services stay offline (the platform is a handle that does nothing)"); return 0; }
HLE("EOS_Shutdown", int, h_eos_shutdown, void) { return 0; }
HLE("EOS_Logging_SetCallback", int, h_eos_log_cb, void *cb) { (void)cb; return 0; }
HLE("EOS_Logging_SetLogLevel", int, h_eos_log_lvl, int cat, int lvl) { (void)cat; (void)lvl; return 0; }
HLE("EOS_Platform_Create", void *, h_eos_platform_create, const void *o) { (void)o; return &g_eos_obj[0]; }
HLE("EOS_Platform_Release", void, h_eos_platform_release, void *h) { (void)h; }
HLE("EOS_Platform_Tick", void, h_eos_platform_tick, void *h) { (void)h; }
HLE("EOS_Platform_GetNetworkStatus", int, h_eos_net_get, void *h) { (void)h; return 1; }   // EOS_NS_Offline
HLE("EOS_Platform_SetNetworkStatus", int, h_eos_net_set, void *h, int s) { (void)h; (void)s; return 0; }
#define EOS_IFACE(name, k) HLE("EOS_Platform_Get" #name "Interface", void *, h_eos_iface_##name, void *h) { (void)h; return &g_eos_obj[k]; }
EOS_IFACE(Auth, 1) EOS_IFACE(Connect, 2) EOS_IFACE(Friends, 3) EOS_IFACE(Lobby, 4) EOS_IFACE(P2P, 5) EOS_IFACE(Presence, 6) EOS_IFACE(Reports, 7) EOS_IFACE(Sanctions, 8) EOS_IFACE(UserInfo, 9)
// Scott Pilgrim EX's wrapper reads every interface (it faulted on a null Achievements one at main+0x83d46c): all of them exist. Their calls stay stubs; the ones
// that hand back a handle fail (hle.c stub_ret_for), so a game sees what it would see offline
EOS_IFACE(Achievements, 12) EOS_IFACE(AntiCheatClient, 13) EOS_IFACE(AntiCheatServer, 14) EOS_IFACE(CustomInvites, 15) EOS_IFACE(Ecom, 16) EOS_IFACE(IntegratedPlatform, 17)
EOS_IFACE(KWS, 18) EOS_IFACE(Leaderboards, 19) EOS_IFACE(Metrics, 20) EOS_IFACE(Mods, 21) EOS_IFACE(PlayerDataStorage, 22) EOS_IFACE(ProgressionSnapshot, 23)
EOS_IFACE(RTCAdmin, 24) EOS_IFACE(RTC, 25) EOS_IFACE(Sessions, 26) EOS_IFACE(Stats, 27) EOS_IFACE(TitleStorage, 28) EOS_IFACE(UI, 29)
HLE("EOS_RTC_GetAudioInterface", void *, h_eos_rtc_audio, void *h) { (void)h; return &g_eos_obj[30]; }
HLE("EOS_RTC_GetDataInterface", void *, h_eos_rtc_data, void *h) { (void)h; return &g_eos_obj[31]; }
// notifications: an id each, never fired
#define EOS_NOTIFY(sym, k) HLE(sym, uint64_t, h_eos_note_##k, void *h, const void *o, void *client, void *cb) { (void)h; (void)o; (void)client; (void)cb; return atomic_fetch_add(&g_eos_note, 1); }
EOS_NOTIFY("EOS_Connect_AddNotifyAuthExpiration", 0) EOS_NOTIFY("EOS_Connect_AddNotifyLoginStatusChanged", 1)
EOS_NOTIFY("EOS_Lobby_AddNotifyJoinLobbyAccepted", 2) EOS_NOTIFY("EOS_Lobby_AddNotifyLobbyInviteAccepted", 3) EOS_NOTIFY("EOS_Lobby_AddNotifyLobbyInviteReceived", 4)
EOS_NOTIFY("EOS_Lobby_AddNotifyLobbyMemberStatusReceived", 5) EOS_NOTIFY("EOS_Lobby_AddNotifyLobbyMemberUpdateReceived", 6) EOS_NOTIFY("EOS_Lobby_AddNotifyLobbyUpdateReceived", 7)
EOS_NOTIFY("EOS_P2P_AddNotifyPeerConnectionClosed", 8) EOS_NOTIFY("EOS_P2P_AddNotifyPeerConnectionEstablished", 9) EOS_NOTIFY("EOS_P2P_AddNotifyPeerConnectionInterrupted", 10)
EOS_NOTIFY("EOS_P2P_AddNotifyPeerConnectionRequest", 11) EOS_NOTIFY("EOS_Presence_AddNotifyOnPresenceChanged", 12)
#define EOS_UNNOTIFY(sym, k) HLE(sym, void, h_eos_unnote_##k, void *h, uint64_t id) { (void)h; (void)id; }
EOS_UNNOTIFY("EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived", 0) EOS_UNNOTIFY("EOS_Lobby_RemoveNotifyLobbyMemberUpdateReceived", 1) EOS_UNNOTIFY("EOS_Lobby_RemoveNotifyLobbyUpdateReceived", 2)
EOS_UNNOTIFY("EOS_P2P_RemoveNotifyPeerConnectionClosed", 3) EOS_UNNOTIFY("EOS_P2P_RemoveNotifyPeerConnectionEstablished", 4) EOS_UNNOTIFY("EOS_P2P_RemoveNotifyPeerConnectionInterrupted", 5)
EOS_UNNOTIFY("EOS_P2P_RemoveNotifyPeerConnectionRequest", 6)
// what a game polls: nobody is logged in, nothing has arrived
HLE("EOS_Auth_GetLoginStatus", int, h_eos_auth_status, void *h, void *user) { (void)h; (void)user; return 0; }   // EOS_LS_NotLoggedIn
HLE("EOS_ProductUserId_IsValid", int, h_eos_puid_valid, void *id) { (void)id; return 0; }
HLE("EOS_EResult_IsOperationComplete", int, h_eos_complete, int r) { (void)r; return 1; }
HLE("EOS_P2P_ReceivePacket", int, h_eos_p2p_recv, void *h, const void *o, void *peer, void *sock, void *chan, void *data, uint32_t *n) { (void)h; (void)o; (void)peer; (void)sock; (void)chan; (void)data; if (n) *n = 0; return 18; }   // EOS_NotFound
HLE("EOS_Friends_GetFriendsCount", int, h_eos_friends_n, void *h, const void *o) { (void)h; (void)o; return 0; }
HLE("EOS_Connect_GetProductUserExternalAccountCount", uint32_t, h_eos_ext_n, void *h, const void *o) { (void)h; (void)o; return 0; }
