NVBridge runtime folder.

The NVSE script writes outgoing text to:
  nvse\plugins\Chasm\outbox

The helper writes response status to:
  nvse\plugins\Chasm\inbox

Generated speech WAV files are written to:
  sound\fx\nvbridge

Run the helper from this repository:
  node tools\nvbridge-helper.mjs --data-root "<path-to-your-ModOrganizer>\New Vegas\mods\NVBridge"
