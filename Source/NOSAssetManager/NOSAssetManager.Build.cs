// Copyright MediaZ Teknoloji A.S. All Rights Reserved.


using System;
using System.IO;
using System.Collections;
using System.Collections.Generic;

using UnrealBuildTool;
using EpicGames.Core;

public class NOSAssetManager : ModuleRules
{
	public NOSAssetManager(ReadOnlyTargetRules Target) : base(Target)
	{
		if (Target.bBuildEditor)
		{
			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				CppStandard = CppStandardVersion.Cpp20;

				NosIncludeDirs? dirs = NOSClient.GetSDKDir(Target.RelativeEnginePath);

				if (dirs == null || String.IsNullOrEmpty(dirs?.NodosSDKDir) || String.IsNullOrEmpty(dirs?.VulkanSubsystemIncludeDir))
				{
					string errorMessage = "Please update NODOS_SDK_DIR environment variable";
					System.Console.WriteLine(errorMessage);
					throw new BuildException(errorMessage);
				}

				var SDKIncludeDir = Path.Combine(dirs?.NodosSDKDir, "include");

				PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));
				PublicIncludePaths.Add(SDKIncludeDir);
				PublicIncludePaths.Add(dirs?.VulkanSubsystemIncludeDir);

				PublicDependencyModuleNames.AddRange(
					new string[]
					{
					"Core",
					"CoreUObject",
					"Engine",
					"AssetRegistry",
					"EditorFramework",
                    "TypedElementFramework",
					"UMG",
					"UnrealEd",
					"NOSClient",
					"LevelSequence",
					}
					);

				PrivateDependencyModuleNames.AddRange(
					new string[]
					{

					"Core",
					"CoreUObject",
					"Engine",
					"AssetRegistry",
					"EditorFramework",
                    "TypedElementFramework",
					"UMG",
					"UnrealEd",
					"NOSClient",
					"LevelSequence",
					}
					);
			}
		}
		else 
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		}
	}
}
