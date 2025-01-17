## Nodos Link UE5 PLUGIN

1. Make sure your graphics card drivers are up to date
2. Download latest version of [Vulkan SDK](https://vulkan.lunarg.com/sdk)
3. Install latest version of [Nodos](https://github.com/nodos-dev)
4. Add the following code into `<UnrealEngineFolder>/Engine/Config/BaseEditorSettings.ini`:
```
[/Script/NOSClient.NOSSettings]
NosmanPath=<NodosFolder>\nodos.exe
```
5. Run `<UnrealEngineFolder>/GenerateProjectFiles.bat` to build the necessary files.
6. Launch an Unreal Engine project and Nodos editor to see the Unreal Engine node on the Nodos editor.
