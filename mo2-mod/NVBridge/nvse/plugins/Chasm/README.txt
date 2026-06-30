NVBridge runtime folder.

The NVSE plugin writes outgoing text to:
  nvse\plugins\Chasm\outbox

chasm writes response status to:
  nvse\plugins\Chasm\inbox

Generated speech WAV files are written to:
  sound\fx\nvbridge

chasm reads and writes these files directly while it runs — there is no
separate helper process to start. Start chasm, then launch the game through
Mod Organizer 2; chasm shows "Connected" once the game is live. See INSTALL.md.
