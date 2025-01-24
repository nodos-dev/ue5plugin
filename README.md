## Nodos Link UE5 PLUGIN

1. Make sure your graphics card drivers are up to date
2. Pull the latest changes for your R5's Nodos Link git submodule (`Engine\Plugins\Media\Nodos`) using [the table here](https://github.com/nodos-dev/ue5plugin/blob/r5/nodoslink-table/README.md).
3. Download and extract a Nodos version that matches with the Nodos Link from [here](https://github.com/nodos-dev).
4. Navigate to Engine\Config\BaseEditorSettings.ini and update the Nodos.exe path. Note that if the path is relative, ./ is the R5 Engine\ folder
5. Run `GenerateProjectFiles.bat` in R5.
6. Build the UE5 solution.
7. Launch an Unreal Engine project and Nodos editor to see the Unreal Engine node on the Nodos editor. You can create the Unreal Engine node by drag-dropping from AppsPane in Nodos Editor.
