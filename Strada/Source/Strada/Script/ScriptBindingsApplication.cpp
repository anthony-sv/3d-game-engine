#include "stpch.h"
#include "Strada/Script/ScriptGlue.h"

#include <string>

namespace Strada::ScriptGlue
{
	namespace
	{
		// --- Application ---

		uint8_t Application_IsEditor()
		{
			ScriptHost const* const host = ScriptEngine::GetHost();
			return host != nullptr && host->IsEditor() ? 1 : 0;
		}

		void Application_Quit()
		{
			if (ScriptHost* host = ScriptEngine::GetHost())
			{
				host->RequestQuit();
			}
			else
			{
				Log::GetScriptLogger().error("Application.Quit: no application runs the scene");
			}
		}

		// The size of what the scene renders to: the window, or the editor's viewport. Zero without a running scene.
		void Application_GetWindowSize(uint32_t* width, uint32_t* height)
		{
			Scene const* const scene = ScriptEngine::GetSceneContext();
			*width = scene != nullptr ? scene->GetViewportWidth() : 0;
			*height = scene != nullptr ? scene->GetViewportHeight() : 0;
		}

		// --- SceneManager ---

		char const* SceneManager_GetCurrentSceneName(int32_t* length)
		{
			*length = 0;
			Scene const* const scene = GetScene("SceneManager.CurrentSceneName");
			if (scene == nullptr)
			{
				return nullptr;
			}
			*length = static_cast<int32_t>(scene->GetName().size());
			return scene->GetName().data();
		}

		uint8_t SceneManager_LoadScene(char const* path, int32_t length)
		{
			AssetHandle const scene = FindAsset(ToStringView(path, length), AssetType::Scene, "SceneManager.LoadScene");
			if (!scene.IsValid())
			{
				return 0;
			}
			ScriptHost* const host = ScriptEngine::GetHost();
			if (host == nullptr)
			{
				Log::GetScriptLogger().error("SceneManager.LoadScene: no application runs the scene");
				return 0;
			}
			host->RequestSceneLoad(scene);
			return 1;
		}

		// --- Debug ---

		// Shipped (Dist) games draw no debug lines.
		void Debug_DrawLine([[maybe_unused]] Vector3 const* from, [[maybe_unused]] Vector3 const* to, [[maybe_unused]] Vector4 const* color,
		                    [[maybe_unused]] float duration)
		{
#if !defined(ST_DIST)
			if (!CheckFinite("Debug.DrawLine", *from, *to, *color, duration))
			{
				return;
			}
			if (duration < 0.0f)
			{
				Log::GetScriptLogger().error("Debug.DrawLine: the duration {} is negative", duration);
				return;
			}
			if (Scene* scene = GetScene("Debug.DrawLine"))
			{
				scene->DrawDebugLine(FromScript(*from), FromScript(*to), FromScript(*color), duration);
			}
#endif
		}

		// --- Strada.Testing ---

		void TestReporter_Report(char const* name, int32_t nameLength, uint8_t passed, char const* message, int32_t messageLength)
		{
			ScriptTestResult result;
			result.Name = std::string(ToStringView(name, nameLength));
			result.Passed = passed != 0;
			result.Message = std::string(ToStringView(message, messageLength));
			if (result.Passed)
			{
				Log::GetScriptLogger().info("Test passed: {}", result.Name);
			}
			else
			{
				Log::GetScriptLogger().error("Test failed: {}: {}", result.Name, result.Message);
			}
			if (ScriptHost* host = ScriptEngine::GetHost())
			{
				host->ReportTestResult(result);
			}
		}

		void TestReporter_Finish()
		{
			Log::GetScriptLogger().info("Tests finished");
			if (ScriptHost* host = ScriptEngine::GetHost())
			{
				host->FinishTests();
			}
		}
	}

	void RegisterApplicationBindings(BindingTable& table)
	{
		table.Add("Application_IsEditor", &Application_IsEditor);
		table.Add("Application_Quit", &Application_Quit);
		table.Add("Application_GetWindowSize", &Application_GetWindowSize);
		table.Add("SceneManager_GetCurrentSceneName", &SceneManager_GetCurrentSceneName);
		table.Add("SceneManager_LoadScene", &SceneManager_LoadScene);
		table.Add("Debug_DrawLine", &Debug_DrawLine);
		table.Add("TestReporter_Report", &TestReporter_Report);
		table.Add("TestReporter_Finish", &TestReporter_Finish);
	}
}
