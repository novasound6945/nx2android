# The Legend of Zelda: Echoes of Wisdom (01008CF01BAAC000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # The Legend of Zelda: Echoes of Wisdom 1.1.0 (v196608) main. This fork's (2026-11-26): the frame pacing (main+0x345da0, the same engine as Link's Awakening's) targets 60 and
  # falls to a 30 fps mode by itself: when the byte at the pacing object +0x3e4 is set and frames ran past 1/60 s (the flag at +0x3e5, counted over windows of 12 frames at +0x3d8 /
  # +0x3dc), it sets the mode word at +0x3b0 to 1 and the present interval (graphics object +0xbcc8) to round(60 / 30) = 2 at main+0x346d48; main+0x32ca94 then calls
  # nvnWindowSetPresentInterval, 39 times in two minutes on a Retroid Pocket Nova (Adreno 740), and in that mode the game renders at up to 1920x1080 docked whatever the GPU's time.
  # With +0x3e4 clear the same function takes main+0x346d50: a 30 fps mode is turned back (interval 1, mode 0) and none is entered. The branch on that byte becomes unconditional,
  # as Link's Awakening's two calls are removed (its patches.py, the stable-60fps patch): the game stays in its 60 fps mode and keeps its frame within one vsync with its own
  # dynamic resolution
  '9f221e1afa65ba5443e955158ab97da8': [
    (0x346bc4, '680c0034', '63000014', 'dynamic drop to 30 fps: cbz w8,main+0x346d50 (the pacing object +0x3e4 clear) → b main+0x346d50 (the 60 fps mode stays; present interval 1)'),
    # The movie player's VideoInputReadThread (main+0x188f8d0) takes one message from the player's 128-entry queue (player+0xa50), sends one input, and posts another to
    # its own queue while inputs are still listed (main+0x188fa74); AudioVideoWorkerThread posts one per input index too. When outputs are dropped as late the decoder
    # hands inputs back at once, the surplus grows by one a packet, and the reader, the queue's only receiver, blocked in SendMessageQueue on its own full queue: no
    # more input, and Stop then waited on the player for good (a black screen with sound when a save loads; issue #455's opening movie). TrySendMessageQueue drops
    # that one repost when the queue is full, which holds 128 wake-ups for the reader already
    (0x188fa74, '3f933e94', '83983e94', 'movie input reader re-posts to its own queue: bl SendMessageQueue (main+0x2834770) → bl TrySendMessageQueue (main+0x2835c80)'),
  ],
}
