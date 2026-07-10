// Copyright MediaZ Teknoloji A.S. All Rights Reserved.


using System;
using System.IO;
using System.Collections;
using System.Collections.Generic;

using UnrealBuildTool;

public class NOSDataStructures : ModuleRules
{
	public NOSDataStructures(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(
			new string[]
				{
				"Core",
				"CoreUObject",
				"Engine",
				"RHI",
				"RenderCore"
				}
			);
	}
}
