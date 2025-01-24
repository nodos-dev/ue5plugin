## Nodos Link UE5 PLUGIN

1. Make sure your graphics card drivers are up to date
2. Download latest version of [Vulkan SDK](https://vulkan.lunarg.com/sdk)
3. Pull the latest changes for your R5's Nodos Link git submodule (`Engine\Plugins\Media\Nodos`) using [the table here](https://github.com/nodos-dev/ue5plugin/blob/r5/nodoslink-table/README.md).
4. Download and extract a Nodos version that matches with the Nodos Link from [here](https://github.com/nodos-dev).
5. In R5, add an entry that points to the Nodos. Entry should be added to `Engine\Config\BaseEditorSettings.ini`.
    NosmanPath can be either relative or absolute. Relative should be relative to Engine folder.
```
;#if WITH_REALITY
[/Script/NOSClient.NOSSettings]
NosmanPath=<NODOS_DIRECTORY>/nodos.exe
;#endif // WITH_REALITY
```
6. Run `GenerateProjectFiles.bat` in R5.
7. Build the UE5 solution.
8. Launch an Unreal Engine project and Nodos editor to see the Unreal Engine node on the Nodos editor. You can create the Unreal Engine node by drag-dropping from AppsPane in Nodos Editor.
