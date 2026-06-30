# Native xNVSE Plugin Source

This folder contains the source for `fnv_bridge_native.dll`, the thin game-side bridge used by the packaged MO2 mod.

The Visual Studio project expects the xNVSE SDK at `native/xnvse-sdk`. Keep that SDK checkout local and ignored by git.

Build from a Visual Studio developer shell:

```powershell
msbuild native\nvse-plugin\fnv_bridge_native.vcxproj /p:Configuration=Release /p:Platform=Win32
```

With current xNVSE SDK checkouts that target the VS18 toolset, use:

```powershell
msbuild native\nvse-plugin\fnv_bridge_native.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v145
```

The output DLL is written to `native/nvse-plugin/Release/fnv_bridge_native.dll`. Copy that DLL to `mo2-mod/NVBridge/nvse/plugins/fnv_bridge_native.dll` before packaging or testing the MO2 mod.
