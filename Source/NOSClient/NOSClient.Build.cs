// Copyright MediaZ Teknoloji A.S. All Rights Reserved.


using System;
using System.IO;
using System.Collections;
using System.Collections.Generic;
using EpicGames.Core;
using UnrealBuildTool;

public class NOSClient : ModuleRules
{
	private string CopyToBinaries(string Filepath)
	{
		string BinaryDir = Path.Combine(PluginDirectory, "Binaries", Target.Platform.ToString());

		string path = Path.Combine(BinaryDir, Path.GetFileName(Filepath));

		if (!Directory.Exists(BinaryDir))
		{
			Directory.CreateDirectory(BinaryDir);
		}

		File.Copy(Filepath, path, true);
		return path;
	}
	public static void LogError(string Message, bool shouldThrow = false)
	{
		ConsoleColor oldColor = Console.ForegroundColor;
		Console.ForegroundColor = ConsoleColor.Red;
		Console.Error.WriteLine(Message);
		Console.ForegroundColor = oldColor;
		if (shouldThrow)
		throw new BuildException(Message);
	}

	public static string GetSDKDir(string RelativeEnginePath)
	{
		string NosmanPath;
		
		ConfigHierarchy PlatformGameConfig = ConfigCache.ReadHierarchy(ConfigHierarchyType.EditorSettings, null, UnrealTargetPlatform.Win64);

		PlatformGameConfig.GetString("/Script/NOSClient.NOSSettings", "NosmanPath", out NosmanPath);


		if (!Path.IsPathRooted(NosmanPath))
		{
			var EngineDir = Path.GetFullPath(RelativeEnginePath);
			NosmanPath = Path.Combine(EngineDir, NosmanPath);
		}


		if(!File.Exists(NosmanPath))
		{
			string errorMessage = "Please verify Nosman Executable exist at " +
				"(you can provide it from BaseEditorSettings.ini and it can be relative to Engine folder or it can be an absolute path) " + NosmanPath;
			LogError(errorMessage, true);
		}

		//execute shell command
		System.Diagnostics.Process process = new System.Diagnostics.Process();
		process.StartInfo.FileName = NosmanPath;
		process.StartInfo.ArgumentList.Add("sdk-info");
		process.StartInfo.ArgumentList.Add("16.0.0");
		process.StartInfo.ArgumentList.Add("process");
		process.StartInfo.UseShellExecute = false;
		process.StartInfo.WorkingDirectory = Path.Combine(NosmanPath, "..");
		process.StartInfo.RedirectStandardOutput = true;
		process.StartInfo.RedirectStandardError = true;
		process.StartInfo.CreateNoWindow = true;
		process.Start();
		process.WaitForExit();
		string output = process.StandardOutput.ReadToEnd();
		// Print stderr or stdout if failed
		if (process.ExitCode != 0)
		{
			Console.WriteLine();
			LogError("Failed to get Nodos SDK info");
			var errOut = process.StandardError.ReadToEnd();
			if (!String.IsNullOrEmpty(errOut))
			{
				LogError(errOut, true);
			}
			else if (!String.IsNullOrEmpty(output))
			{
				LogError(output, true);
			}
			return "";
		}
		
		if (!JsonObject.TryParse(output, out var SDKInfo))
		{
			LogError("Failed to parse Nodos SDK info:");
			LogError(output, true);
		}

		if(!SDKInfo.TryGetStringField("path", out var SDKdir))
		{
			LogError("Could not find SDK path in Nodos SDK info:" + output, true);
		}

		return SDKdir;
	}

	public NOSClient(ReadOnlyTargetRules Target) : base(Target)
	{

		if (Target.bBuildEditor)
		{
			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				CppStandard = CppStandardVersion.Cpp20;

				string SDKdir = GetSDKDir(Target.RelativeEnginePath);

				if (String.IsNullOrEmpty(SDKdir))
				{
					string errorMessage = "Failed to get Nodos SDK info from nodos.exe";
					LogError(errorMessage, true);
				}

				var SDKIncludeDir = Path.Combine(SDKdir, "include");


				PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));
				PublicIncludePaths.Add(SDKIncludeDir);

				PublicDependencyModuleNames.AddRange(
					new string[]
					{
					"Core",
					"CoreUObject",
					"Engine",
					"Projects",
					"Slate",
					"SlateCore",
					"UnrealEd",
					"Json",
					"DeveloperSettings"
					}
					);

				PrivateDependencyModuleNames.AddRange(
					new string[]
					{

					"Core",
					"CoreUObject",
					"Engine",
					"Projects",
					"Slate",
					"SlateCore",
					"EditorStyle",
					"ToolMenus",
					"UnrealEd"
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
