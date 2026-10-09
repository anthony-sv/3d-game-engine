#include "Editor/FileDialogs.h"

#include "Strada/Core/Application.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/PlatformDetection.h"
#include "Strada/Core/Window.h"

// Native window handles for parenting the dialogs (X11 only on Linux: NFD cannot parent to Wayland windows).
#if defined(ST_PLATFORM_WINDOWS)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(ST_PLATFORM_MACOS)
#define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(ST_PLATFORM_LINUX)
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <nfd.h>
#include <nfd_glfw3.h>

#include <system_error>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// NFD is initialized around each dialog: dialogs are rare, and COM (Windows) state stays scoped to them.
		class DialogSession
		{
		public:
			DialogSession()
				: m_Result(NFD_Init())
			{
			}

			~DialogSession()
			{
				if (m_Result == NFD_OKAY)
				{
					NFD_Quit();
				}
			}

			DialogSession(DialogSession const&) = delete;
			DialogSession& operator=(DialogSession const&) = delete;

			bool IsReady() const { return m_Result == NFD_OKAY; }

		private:
			nfdresult_t m_Result;
		};

		std::string GetLastError()
		{
			char const* error = NFD_GetError();
			return error != nullptr ? std::string(error) : std::string("unknown error");
		}

		nfdwindowhandle_t GetParentWindow()
		{
			nfdwindowhandle_t handle{};
			if (Application::HasInstance())
			{
				if (Window* window = Application::Get().GetWindow())
				{
					// Without a native handle the dialog is simply not parented.
					(void)NFD_GetNativeWindowFromGLFWWindow(window->GetNativeWindow(), &handle);
				}
			}
			return handle;
		}

		std::string ToDefaultPath(std::filesystem::path const& directory)
		{
			if (directory.empty())
			{
				return {};
			}
			std::error_code errorCode;
			std::filesystem::path const absolute = std::filesystem::absolute(directory, errorCode);
			return FileSystem::PathToUtf8(errorCode ? directory : absolute);
		}

		std::vector<nfdu8filteritem_t> MakeFilterItems(std::span<FileDialogFilter const> filters)
		{
			std::vector<nfdu8filteritem_t> items;
			items.reserve(filters.size());
			for (FileDialogFilter const& filter : filters)
			{
				items.push_back({filter.Name.c_str(), filter.Extensions.c_str()});
			}
			return items;
		}

		Result<std::optional<std::filesystem::path>> Finish(nfdresult_t result, nfdu8char_t* path)
		{
			if (result == NFD_OKAY)
			{
				std::filesystem::path selected = FileSystem::PathFromUtf8(path);
				NFD_FreePathU8(path);
				return std::optional<std::filesystem::path>(std::move(selected));
			}
			if (result == NFD_CANCEL)
			{
				return std::optional<std::filesystem::path>();
			}
			return MakeError("the file dialog failed: {}", GetLastError());
		}
	}

	namespace FileDialogs
	{
		Result<std::optional<std::filesystem::path>> OpenFile(std::span<FileDialogFilter const> filters,
		                                                      std::filesystem::path const& defaultDirectory)
		{
			DialogSession const session;
			if (!session.IsReady())
			{
				return MakeError("file dialogs are unavailable: {}", GetLastError());
			}
			std::vector<nfdu8filteritem_t> const items = MakeFilterItems(filters);
			std::string const defaultPath = ToDefaultPath(defaultDirectory);
			nfdopendialogu8args_t arguments{};
			arguments.filterList = items.empty() ? nullptr : items.data();
			arguments.filterCount = static_cast<nfdfiltersize_t>(items.size());
			arguments.defaultPath = defaultPath.empty() ? nullptr : defaultPath.c_str();
			arguments.parentWindow = GetParentWindow();
			nfdu8char_t* path = nullptr;
			// The dialog runs before path is read: function arguments are evaluated in an unspecified order.
			nfdresult_t const result = NFD_OpenDialogU8_With(&path, &arguments);
			return Finish(result, path);
		}

		Result<std::optional<std::filesystem::path>> SaveFile(std::span<FileDialogFilter const> filters,
		                                                      std::filesystem::path const& defaultDirectory, std::string const& defaultName)
		{
			DialogSession const session;
			if (!session.IsReady())
			{
				return MakeError("file dialogs are unavailable: {}", GetLastError());
			}
			std::vector<nfdu8filteritem_t> const items = MakeFilterItems(filters);
			std::string const defaultPath = ToDefaultPath(defaultDirectory);
			nfdsavedialogu8args_t arguments{};
			arguments.filterList = items.empty() ? nullptr : items.data();
			arguments.filterCount = static_cast<nfdfiltersize_t>(items.size());
			arguments.defaultPath = defaultPath.empty() ? nullptr : defaultPath.c_str();
			arguments.defaultName = defaultName.empty() ? nullptr : defaultName.c_str();
			arguments.parentWindow = GetParentWindow();
			nfdu8char_t* path = nullptr;
			// The dialog runs before path is read: function arguments are evaluated in an unspecified order.
			nfdresult_t const result = NFD_SaveDialogU8_With(&path, &arguments);
			return Finish(result, path);
		}

		Result<std::optional<std::filesystem::path>> PickFolder(std::filesystem::path const& defaultDirectory)
		{
			DialogSession const session;
			if (!session.IsReady())
			{
				return MakeError("file dialogs are unavailable: {}", GetLastError());
			}
			std::string const defaultPath = ToDefaultPath(defaultDirectory);
			nfdpickfolderu8args_t arguments{};
			arguments.defaultPath = defaultPath.empty() ? nullptr : defaultPath.c_str();
			arguments.parentWindow = GetParentWindow();
			nfdu8char_t* path = nullptr;
			// The dialog runs before path is read: function arguments are evaluated in an unspecified order.
			nfdresult_t const result = NFD_PickFolderU8_With(&path, &arguments);
			return Finish(result, path);
		}
	}
}
