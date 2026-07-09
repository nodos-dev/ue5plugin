// Copyright MediaZ Teknoloji A.S. All Rights Reserved.


using System;
using System.IO;
using System.Collections;
using System.Collections.Generic;
using EpicGames.Core;
using UnrealBuildTool;

public struct NosIncludeDirs
{
	public string NodosSDKDir;
	public string VulkanSubsystemIncludeDir;
	public string TrackIncludeDir;
	public string LinoIncludeDir;
	public NosIncludeDirs(string NodosSDKDir, string VulkanSubsystemIncludeDir, string TrackIncludeDir, string LinoIncludeDir)
	{
		this.NodosSDKDir = NodosSDKDir;
		this.VulkanSubsystemIncludeDir = VulkanSubsystemIncludeDir;
		this.TrackIncludeDir = TrackIncludeDir;
		this.LinoIncludeDir = LinoIncludeDir;
	}
	public bool IsAllOk()
	{
		return !String.IsNullOrEmpty(NodosSDKDir) && 
			   !String.IsNullOrEmpty(VulkanSubsystemIncludeDir) && 
			   !String.IsNullOrEmpty(TrackIncludeDir) &&
			   !String.IsNullOrEmpty(LinoIncludeDir);
	}
}

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

	public static string InstallNodosModuleAndGetPublicInclude(string NosmanPath, string ModuleName, string Version)
	{
		System.Diagnostics.Process installProc = new System.Diagnostics.Process();
		installProc.StartInfo.FileName = NosmanPath;
		installProc.StartInfo.ArgumentList.Add("install");
		installProc.StartInfo.ArgumentList.Add(ModuleName);
		installProc.StartInfo.ArgumentList.Add(Version);
		installProc.StartInfo.UseShellExecute = false;
		installProc.StartInfo.WorkingDirectory = Path.Combine(NosmanPath, "..");
		installProc.StartInfo.RedirectStandardOutput = true;
		installProc.StartInfo.RedirectStandardError = true;
		installProc.StartInfo.CreateNoWindow = true;
		installProc.Start();
		installProc.WaitForExit();

		string output = installProc.StandardOutput.ReadToEnd();
		// Print stderr or stdout if failed
		if (installProc.ExitCode != 0)
		{
			Console.WriteLine();
			LogError("Failed to get install " + ModuleName);
			var errOut = installProc.StandardError.ReadToEnd();
			if (!String.IsNullOrEmpty(errOut))
			{
				LogError(errOut, true);
			}
			else if (!String.IsNullOrEmpty(output))
			{
				LogError(output, true);
			}
			return null;
		}

		System.Diagnostics.Process infoProc = new System.Diagnostics.Process();
		infoProc.StartInfo.FileName = NosmanPath;
		infoProc.StartInfo.ArgumentList.Add("info");
		infoProc.StartInfo.ArgumentList.Add(ModuleName);
		infoProc.StartInfo.ArgumentList.Add(Version);
		infoProc.StartInfo.ArgumentList.Add("--relaxed");
		infoProc.StartInfo.UseShellExecute = false;
		infoProc.StartInfo.WorkingDirectory = Path.Combine(NosmanPath, "..");
		infoProc.StartInfo.RedirectStandardOutput = true;
		infoProc.StartInfo.RedirectStandardError = true;
		infoProc.StartInfo.CreateNoWindow = true;
		infoProc.Start();
		infoProc.WaitForExit();

		output = infoProc.StandardOutput.ReadToEnd();
		if (infoProc.ExitCode != 0)
		{
			Console.WriteLine();
			LogError("Failed to get info of " + ModuleName + ": ");
			var errOut = infoProc.StandardError.ReadToEnd();
			if (!String.IsNullOrEmpty(errOut))
			{
				LogError(errOut, true);
			}
			else if (!String.IsNullOrEmpty(output))
			{
				LogError(output, true);
			}
			return null;
		}

		if (!JsonObject.TryParse(output, out var SysVulkanInfo))
		{
			LogError("Failed to parse info of " + ModuleName + ": ");
			LogError(output, true);
			return null;
		}

		if (!SysVulkanInfo.TryGetStringField("public_include_folder", out var PublicIncludeDir))
		{
			LogError("Could not find public include folder in info of " + ModuleName + ": " + output, true);
			return null;
		}
		return PublicIncludeDir;
	}

	public static NosIncludeDirs? GetSDKDir(string RelativeEnginePath)
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
			return null;
		}

		//execute shell command
		string NodosSDKDir;
		{
			System.Diagnostics.Process process = new System.Diagnostics.Process();
			process.StartInfo.FileName = NosmanPath;
			process.StartInfo.ArgumentList.Add("sdk-info");
			process.StartInfo.ArgumentList.Add("21.0.0");
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
				return null;
			}

			if (!JsonObject.TryParse(output, out var SDKInfo))
			{
				LogError("Failed to parse Nodos SDK info:");
				LogError(output, true);
				return null;
			}

			if (!SDKInfo.TryGetStringField("path", out var SDKdir))
			{
				LogError("Could not find SDK path in Nodos SDK info:" + output, true);
				return null;
			}
			NodosSDKDir = SDKdir;
		}

		string VulkanSubsystemIncludeDir = InstallNodosModuleAndGetPublicInclude(NosmanPath, "nos.sys.vulkan", "6.0");
		string TrackIncludeDir = InstallNodosModuleAndGetPublicInclude(NosmanPath, "nos.sys.track", "1.0");
		string LinoIncludeDir = InstallNodosModuleAndGetPublicInclude(NosmanPath, "nos.sys.lino", "1.0");

		return new NosIncludeDirs(NodosSDKDir, VulkanSubsystemIncludeDir, TrackIncludeDir, LinoIncludeDir);
	}

	public NOSClient(ReadOnlyTargetRules Target) : base(Target)
	{

		if (Target.bBuildEditor)
		{
			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				CppStandard = CppStandardVersion.Cpp20;

				NosIncludeDirs? dirs = GetSDKDir(Target.RelativeEnginePath);
				if (dirs?.IsAllOk() != true)
				{
					string errorMessage = "Failed to get Nodos SDK info from nodos.exe";
					LogError(errorMessage, true);
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
