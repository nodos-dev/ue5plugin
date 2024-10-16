// Copyright MediaZ Teknoloji A.S. Inc. All Rights Reserved.


using System;
using System.IO;
using System.Collections;
using System.Collections.Generic;

using UnrealBuildTool;

public class NOSSceneTreeManager : ModuleRules
{
	public NOSSceneTreeManager(ReadOnlyTargetRules Target) : base(Target)
	{
		if (Target.bBuildEditor)
		{
			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				CppStandard = CppStandardVersion.Cpp20;

				string SDKdir = NOSClient.GetSDKDir(Target.RelativeEnginePath);

				if (String.IsNullOrEmpty(SDKdir))
				{
					string errorMessage = "Please update NODOS_SDK_DIR environment variable";
					System.Console.WriteLine(errorMessage);
					throw new BuildException(errorMessage);
				}

				var SDKIncludeDir = Path.Combine(SDKdir, "include");

				PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));
				PublicIncludePaths.Add(SDKIncludeDir);

				PublicIncludePathModuleNames.Add("NOSDataStructures");

				PublicDependencyModuleNames.AddRange(
					new string[]
					{
					"Core",
					"CoreUObject",
					"Engine",
					"Projects",
					"RenderCore",
					"RHI",
					"RHICore",
					"D3D11RHI",
					"D3D12RHI",
					"EditorFramework",
                    "TypedElementFramework",
					"Slate",
					"SlateCore",
					"UMG",
					"UnrealEd",
					"NOSClient",
					"NOSAssetManager",
					"NOSViewportManager",
					"NOSDataStructures",
					"LevelSequence",
					"PropertyPath",
					"PropertyEditor",
					}
					);

				PrivateDependencyModuleNames.AddRange(
					new string[]
					{
					"Core",
					"CoreUObject",
					"Engine",
					"Projects",
					"RenderCore",
					"RHI",
					"RHICore",
					"D3D11RHI",
					"D3D12RHI",
					"EditorFramework",
                    "TypedElementFramework",
					"Slate",
					"SlateCore",
					"UMG",
					"EditorStyle",
					"ToolMenus",
					"UnrealEd",
					"NOSClient",
					"NOSAssetManager",
					"NOSViewportManager",
					"NOSDataStructures",
					"LevelSequence",
					"PropertyPath",
					"PropertyEditor",
					}
					);

				PrivateIncludePathModuleNames.Add("D3D11RHI");
				PrivateIncludePathModuleNames.Add("D3D12RHI");

				AddEngineThirdPartyPrivateStaticDependencies(Target, "DX11");
				AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");

				PublicDefinitions.Add("PLATFORM_WIN64");
				PrivateIncludePaths.AddRange(
					new string[]{
						//required for "D3D12RHIPrivate.h"
						Path.Combine(EngineDirectory, "Source/Runtime/D3D12RHI/Private"),
					});


			}
		}
		else 
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		}
	}
}
