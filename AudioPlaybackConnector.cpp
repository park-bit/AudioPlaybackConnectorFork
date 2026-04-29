#include "pch.h"
#include "AudioPlaybackConnector.h"
#include <winrt/Windows.UI.Text.h>

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupVolumeFlyout();
void SetupMenu();
void UpdateNotifyIcon();
void DisableAbsoluteVolume();
void RevertAbsoluteVolume();
void SetRunAtStartup(bool enable);
void UpdateVolume();
void SetupEndpointVolume();
void TeardownEndpointVolume();
void SetupSvgIcon();
bool IsRunningAsAdmin();
void SetupDevicePicker();
winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId);
winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device);

// Audio session management globals and helpers
static IAudioSessionManager2* g_sessionManager = nullptr;

// Helper to identify if an audio session belongs to the phone audio stream
static bool IsBluetoothSession(IAudioSessionControl2* ctrl2, IAudioSessionControl* ctrl)
{
	// Check PID first (if it's in our process, it's definitely ours)
	DWORD pid = 0;
	if (SUCCEEDED(ctrl2->GetProcessId(&pid)) && pid == GetCurrentProcessId()) return true;

	// Check Session Identifier (usually contains BTHENUM, A2DP, etc.)
	PWSTR id = nullptr;
	if (SUCCEEDED(ctrl2->GetSessionInstanceIdentifier(&id)))
	{
		std::wstring sid(id);
		CoTaskMemFree(id);
		for (auto& c : sid) c = towlower(c);
		if (sid.find(L"bthenum") != std::wstring::npos || sid.find(L"a2dp") != std::wstring::npos || sid.find(L"bluetooth") != std::wstring::npos || sid.find(L"snk") != std::wstring::npos)
			return true;
	}

	// Check Display Name (e.g. "Microphone (iQOO Z3 5G A2DP SNK)")
	PWSTR disp = nullptr;
	if (SUCCEEDED(ctrl->GetDisplayName(&disp)))
	{
		std::wstring sdisp(disp);
		CoTaskMemFree(disp);
		for (auto& c : sdisp) c = towlower(c);
		if (sdisp.find(L"a2dp") != std::wstring::npos || sdisp.find(L"snk") != std::wstring::npos || sdisp.find(L"iqoo") != std::wstring::npos || sdisp.find(L"phone") != std::wstring::npos)
			return true;
	}

	return false;
}

static void ApplyVolumeToOurSessions(IAudioSessionManager2* mgr);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(nCmdShow);

	// If relaunched as admin to apply the Absolute Volume fix, do it and exit
	if (lpCmdLine && wcsstr(lpCmdLine, L"--fix-absolute-volume") != nullptr)
	{
		HKEY hKey;
		LONG openResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Bluetooth\\Audio\\AVRCP\\CT", 0, KEY_SET_VALUE, &hKey);
		if (openResult != ERROR_SUCCESS)
			openResult = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Bluetooth\\Audio\\AVRCP\\CT", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL);
		if (openResult == ERROR_SUCCESS)
		{
			DWORD value = 1;
			RegSetValueExW(hKey, L"DisableAbsoluteVolume", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
			RegCloseKey(hKey);
			TaskDialog(nullptr, nullptr, L"Success", L"Absolute Volume disabled.\n\nReboot your PC for the change to take effect.\nAfter rebooting, your phone volume buttons will only control phone volume.", nullptr, TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
		}
		else
		{
			TaskDialog(nullptr, nullptr, L"Error", L"Failed to write registry key.", nullptr, TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		}
		return 0;
	}

	g_hInst = hInstance;

	winrt::init_apartment();
	LoadTranslateData();

	// Always run as administrator to ensure registry and startup features work
	if (!IsRunningAsAdmin())
	{
		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(NULL, exePath, MAX_PATH);
		if (reinterpret_cast<INT_PTR>(ShellExecuteW(NULL, L"runas", exePath, lpCmdLine, NULL, SW_SHOWNORMAL)) > 32)
		{
			return 0;
		}
	}

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupEndpointVolume();
	SetupFlyout();
	SetupVolumeFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		TeardownEndpointVolume();
		for (const auto& connection : g_audioPlaybackConnections)
		{
			connection.second.second.Close();
			g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
		}
		if (g_reconnect)
		{
			SaveSettings();
			g_audioPlaybackConnections.clear();
		}
		else
		{
			g_audioPlaybackConnections.clear();
			SaveSettings();
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case WM_LBUTTONUP:
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr)) break;

			auto dpi = GetDpiForWindow(hWnd);
			float dipW = static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI) / dpi;
			float dipH = static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI) / dpi;

			// Place host window exactly over the tray icon so XAML coords match screen coords
			SetWindowPos(hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, iconRect.right - iconRect.left, iconRect.bottom - iconRect.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
			SetWindowPos(g_hWndXaml, 0, 0, 0, static_cast<int>(dipW), static_cast<int>(dipH), SWP_NOZORDER | SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlCanvas.Width(dipW);
			g_xamlCanvas.Height(dipH);

			g_volumeFlyout.ShowAt(g_xamlCanvas);
		}
		break;
		case WM_RBUTTONUP:
		{
			g_menuFocusState = FocusState::Pointer;
			break;
		}
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			// Get the tray icon rect so we can anchor the menu to it
			RECT iconRect;
			if (FAILED(Shell_NotifyIconGetRect(&g_niid, &iconRect)))
			{
				// Fall back to cursor position
				GetCursorPos(reinterpret_cast<POINT*>(&iconRect));
				iconRect.right = iconRect.left + 1;
				iconRect.bottom = iconRect.top + 1;
			}

			auto dpi = GetDpiForWindow(hWnd);
			float dipW = static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI) / dpi;
			float dipH = static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI) / dpi;
			if (dipW < 1.f) dipW = 1.f;
			if (dipH < 1.f) dipH = 1.f;

			// Host window must sit at the icon position; XAML coords are relative to it
			SetWindowPos(hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, iconRect.right - iconRect.left, iconRect.bottom - iconRect.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
			SetWindowPos(g_hWndXaml, 0, 0, 0, static_cast<int>(dipW), static_cast<int>(dipH), SWP_NOZORDER | SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlCanvas.Width(dipW);
			g_xamlCanvas.Height(dipH);

			// Show menu at the top-left of the canvas; XAML will place it above/below based on available space
			g_xamlMenu.ShowAt(g_xamlCanvas, Point{ 0.f, 0.f });
		}
		break;
		}
		break;
	case WM_APP + 10: // Device added
	{
		auto idString = reinterpret_cast<std::wstring*>(wParam);
		
		// Run async task to get device info and append to list
		auto deviceId = *idString;
		auto AddDeviceAsync = [](std::wstring id) -> winrt::Windows::Foundation::IAsyncAction
		{
			auto device = co_await DeviceInformation::CreateFromIdAsync(id);
			g_devices.Append(device);
		};
		AddDeviceAsync(deviceId);
		
		delete idString;
	}
	break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				ConnectDevice(g_devicePicker, i);
			}
			g_lastDevices.clear();
		}
		break;
	case WM_RESTORE_VOLUME:
		// Fired by the volume callback when a remote (phone) source changed the volume
		if (g_volumeLock && g_endpointVolume)
		{
			g_endpointVolume->SetMasterVolumeLevelScalar(g_lastMasterVolume, &g_ourVolumeGuid);
			g_endpointVolume->SetMute(g_lastMute, &g_ourVolumeGuid);
			if (g_sessionManager) ApplyVolumeToOurSessions(g_sessionManager);
		}
		break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.Content(stackPanel);
	flyout.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
		SaveSettings();
	});

	g_xamlFlyout = flyout;
}

void SetupVolumeFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"Mobile Volume"));
	textBlock.Margin({ 0, 0, 0, 12 });

	Slider slider;
	slider.Minimum(0);
	slider.Maximum(100);
	slider.Value(g_volume * 100);
	slider.Width(200);
	slider.ValueChanged([](const auto&, const auto& args) {
		g_volume = args.NewValue() / 100.0;
		UpdateVolume();
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(slider);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);
	flyout.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
		SaveSettings();
	});

	g_volumeFlyout = flyout;
}

void SetupMenu()
{
	MenuFlyoutItem infoItem;
	infoItem.Text(_(L"Usage Instructions"));
	FontIcon infoIcon;
	infoIcon.Glyph(L"\xE946");
	infoItem.Icon(infoIcon);
	infoItem.Click([](const auto&, const auto&) {
		TaskDialog(g_hWnd, g_hInst, _(L"Usage Instructions"), _(L"Tips for using AudioPlaybackConnector:"), 
			_(L"1. Always run as administrator for all features to work.\n"
			  "2. If no audio, try disconnecting and reconnecting Bluetooth from your phone.\n"
			  "3. If volume sync is broken, use the 'Fix Volume Sync' option and REBOOT.\n"
			  "4. Use 'Lock Phone Volume Buttons' to prevent phone buttons from changing PC volume."), 
			TDCBF_OK_BUTTON, TD_INFORMATION_ICON, NULL);
	});

	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon connectIcon;
	connectIcon.Glyph(L"\xE703");

	MenuFlyoutItem connectItem;
	connectItem.Text(_(L"Connect Device"));
	connectItem.Icon(connectIcon);
	connectItem.Click([](const auto&, const auto&) {
		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			// Fall back to cursor position
			POINT pt;
			GetCursorPos(&pt);
			iconRect = { pt.x, pt.y, pt.x + 1, pt.y + 1 };
		}

		// DevicePicker.Show() takes physical pixel coords (RECT in screen space), not DIPs
		Rect rect = {
			static_cast<float>(iconRect.left),
			static_cast<float>(iconRect.top),
			static_cast<float>(iconRect.right - iconRect.left),
			static_cast<float>(iconRect.bottom - iconRect.top)
		};

		// Make the host window visible so DevicePicker HWND owner is valid
		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, iconRect.right - iconRect.left, iconRect.bottom - iconRect.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
		g_devicePicker.Show(rect);
	});

	ToggleMenuFlyoutItem lockItem;
	lockItem.Text(_(L"Lock Phone Volume Buttons"));
	lockItem.IsChecked(g_volumeLock);
	lockItem.Click([](const auto& sender, const auto&) {
		g_volumeLock = sender.as<ToggleMenuFlyoutItem>().IsChecked();
		if (g_volumeLock && g_endpointVolume)
			g_endpointVolume->SetMasterVolumeLevelScalar(g_lastMasterVolume, &g_ourVolumeGuid);
		SaveSettings();
	});

	ToggleMenuFlyoutItem startupItem;
	startupItem.Text(_(L"Run at Windows Startup"));
	startupItem.IsChecked(g_runAtStartup);
	startupItem.Click([](const auto& sender, const auto&) {
		g_runAtStartup = sender.as<ToggleMenuFlyoutItem>().IsChecked();
		SetRunAtStartup(g_runAtStartup);
		SaveSettings();
	});

	MenuFlyoutItem fixItem;
	fixItem.Text(_(L"Fix Volume Sync (Absolute Volume)"));
	fixItem.Click([](const auto&, const auto&) { DisableAbsoluteVolume(); });

	MenuFlyoutItem revertItem;
	revertItem.Text(_(L"Revert Volume Fix"));
	revertItem.Click([](const auto&, const auto&) { RevertAbsoluteVolume(); });

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}
		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr)) return;
		auto dpi = GetDpiForWindow(g_hWnd);
		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(infoItem);
	menu.Items().Append(MenuFlyoutSeparator());
	menu.Items().Append(settingsItem);
	menu.Items().Append(connectItem);
	menu.Items().Append(MenuFlyoutSeparator());
	menu.Items().Append(lockItem);
	menu.Items().Append(startupItem);
	menu.Items().Append(fixItem);
	menu.Items().Append(revertItem);
	menu.Items().Append(MenuFlyoutSeparator());
	menu.Items().Append(exitItem);

	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		if (menuItems.Size() > 0)
		{
			menuItems.GetAt(menuItems.Size() - 1).as<winrt::Windows::UI::Xaml::Controls::Control>().Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});

	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto&, const auto& args) {
		ConnectDevice(g_devicePicker, args.SelectedDevice());
	});
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		auto device = args.Device();
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
	});
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	if (picker) picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool success = false;
	std::wstring errorMessage;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			g_audioPlaybackConnections.emplace(device.Id(), std::pair(device, connection));

			connection.StateChanged([](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					auto it = g_audioPlaybackConnections.find(std::wstring(sender.DeviceId()));
					if (it != g_audioPlaybackConnections.end())
					{
						if (g_devicePicker) g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
						g_audioPlaybackConnections.erase(it);
					}
					sender.Close();
				}
			});

			co_await connection.StartAsync();
			auto result = co_await connection.OpenAsync();

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				success = true;
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				winrt::throw_hresult(result.ExtendedError());
				break;
			}
		}
		else
		{
			success = false;
			errorMessage = _(L"Unknown error");
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		success = false;
		errorMessage.resize(64);
		while (1)
		{
			auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				errorMessage.resize(errorMessage.size() * 2);
			}
			else
			{
				errorMessage.resize(result);
				break;
			}
		}
		LOG_CAUGHT_EXCEPTION();
	}

	if (success)
	{
		if (picker) picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	else
	{
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		if (picker) picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	ConnectDevice(picker, device);
}


void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}

// Applies g_volume to every active audio session belonging to our process.
// AudioPlaybackConnection audio appears as a session in our PID when the phone streams.
static void ApplyVolumeToOurSessions(IAudioSessionManager2* mgr)
{
	IAudioSessionEnumerator* sessionEnum = nullptr;
	if (FAILED(mgr->GetSessionEnumerator(&sessionEnum))) return;

	int count = 0;
	sessionEnum->GetCount(&count);

	for (int i = 0; i < count; ++i)
	{
		IAudioSessionControl* ctrl = nullptr;
		if (FAILED(sessionEnum->GetSession(i, &ctrl))) continue;

		IAudioSessionControl2* ctrl2 = nullptr;
		if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctrl2)))
		{
			if (IsBluetoothSession(ctrl2, ctrl))
			{
				ISimpleAudioVolume* vol = nullptr;
				if (SUCCEEDED(ctrl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&vol)))
				{
					// Use a 0.7x scale to keep mobile audio in a comfortable range.
					vol->SetMasterVolume(static_cast<float>(g_volume * 0.7), nullptr);
					vol->Release();
				}
			}
			ctrl2->Release();
		}
		ctrl->Release();
	}
	sessionEnum->Release();
}

// Intercepts master volume changes; blocks AVRCP (phone buttons) from altering PC volume.
class VolumeCallback : public IAudioEndpointVolumeCallback
{
public:
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		auto r = InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback))
		{
			*ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify) override
	{
		// 1. Allow our own changes
		if (IsEqualGUID(pNotify->guidEventContext, g_ourVolumeGuid))
			return S_OK;

		bool isRemote = false;

		// Check if it's a system key press or mouse move
		if (IsEqualGUID(pNotify->guidEventContext, GUID_NULL))
		{
			LASTINPUTINFO lii = { sizeof(lii) };
			lii.cbSize = sizeof(lii);
			if (GetLastInputInfo(&lii))
			{
				DWORD idleTime = GetTickCount() - lii.dwTime;
				// If the user hasn't touched the PC in the last 1.5 seconds, 
				// this volume change is almost certainly from the phone (AVRCP).
				if (idleTime > 1500)
				{
					isRemote = true;
				}
			}
		}
		else
		{
			// Any other GUID (phone app or other remote source)
			isRemote = true;
		}

		if (isRemote)
		{
			// Remote change detected (Phone buttons)
			if (g_volumeLock && g_hWnd)
			{
				// Sync the phone's requested level to our app's slider (g_volume)
				g_volume = pNotify->fMasterVolume;
				// Post message to restore master volume to our Authority level and re-apply g_volume to the session
				PostMessageW(g_hWnd, WM_RESTORE_VOLUME, 0, 0);
			}
		}
		else
		{
			// Local authority: update the last known good level set by the user (laptop keys)
			g_lastMasterVolume = pNotify->fMasterVolume;
			g_lastMute = pNotify->bMuted;
		}
		return S_OK;
	}
private:
	long m_ref = 1;
};
static VolumeCallback* g_volumeCallback = nullptr;

// Called by Windows when a new audio session is created.
// We use this to immediately apply our volume when AudioPlaybackConnection starts streaming.
class SessionNotifier : public IAudioSessionNotification
{
public:
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		auto r = InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification))
		{
			*ppv = static_cast<IAudioSessionNotification*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* pNewSession) override
	{
		IAudioSessionControl2* ctrl2 = nullptr;
		if (SUCCEEDED(pNewSession->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctrl2)))
		{
			if (IsBluetoothSession(ctrl2, pNewSession))
			{
				ISimpleAudioVolume* vol = nullptr;
				if (SUCCEEDED(pNewSession->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&vol)))
				{
					vol->SetMasterVolume(static_cast<float>(g_volume * 0.7), nullptr);
					vol->Release();
				}
			}
			ctrl2->Release();
		}
		return S_OK;
	}
private:
	long m_ref = 1;
};

static SessionNotifier* g_sessionNotifier = nullptr;

void SetupEndpointVolume()
{
	try
	{
		IMMDeviceEnumerator* enumerator = nullptr;
		winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
			CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void**)&enumerator));

		IMMDevice* device = nullptr;
		winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
		enumerator->Release();

		// Register AVRCP guardian (blocks phone buttons from changing master volume)
		IAudioEndpointVolume* epVol = nullptr;
		winrt::check_hresult(device->Activate(__uuidof(IAudioEndpointVolume),
			CLSCTX_INPROC_SERVER, NULL, (void**)&epVol));
		g_endpointVolume = epVol;

		// Initialize our Authority levels from the current system state
		float currentVol = 0.5f;
		BOOL currentMute = FALSE;
		if (SUCCEEDED(g_endpointVolume->GetMasterVolumeLevelScalar(&currentVol))) g_lastMasterVolume = currentVol;
		if (SUCCEEDED(g_endpointVolume->GetMute(&currentMute))) g_lastMute = (currentMute != FALSE);

		g_volumeCallback = new VolumeCallback();
		g_endpointVolume->RegisterControlChangeNotify(g_volumeCallback);

		// Register session notifier so we catch AudioPlaybackConnection sessions the moment they start
		IAudioSessionManager2* mgr = nullptr;
		if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2),
			CLSCTX_INPROC_SERVER, NULL, (void**)&mgr)))
		{
			g_sessionManager = mgr; // keep alive for UpdateVolume
			g_sessionNotifier = new SessionNotifier();
			mgr->RegisterSessionNotification(g_sessionNotifier);
			// Apply to any sessions already running
			ApplyVolumeToOurSessions(mgr);
		}
		device->Release();
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

void TeardownEndpointVolume()
{
	if (g_sessionManager && g_sessionNotifier)
	{
		g_sessionManager->UnregisterSessionNotification(g_sessionNotifier);
		g_sessionNotifier->Release();
		g_sessionNotifier = nullptr;
		g_sessionManager->Release();
		g_sessionManager = nullptr;
	}
	if (g_endpointVolume && g_volumeCallback)
	{
		g_endpointVolume->UnregisterControlChangeNotify(g_volumeCallback);
		g_volumeCallback->Release();
		g_volumeCallback = nullptr;
		g_endpointVolume->Release();
		g_endpointVolume = nullptr;
	}
}

void UpdateVolume()
{
	// Set volume on our process's sessions (the AudioPlaybackConnection audio)
	if (g_sessionManager)
		ApplyVolumeToOurSessions(g_sessionManager);
}

bool IsRunningAsAdmin()
{
	BOOL isAdmin = FALSE;
	HANDLE token = NULL;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
	{
		TOKEN_ELEVATION elevation = {};
		DWORD cbSize = sizeof(elevation);
		if (GetTokenInformation(token, TokenElevation, &elevation, cbSize, &cbSize))
			isAdmin = elevation.TokenIsElevated;
		CloseHandle(token);
	}
	return isAdmin != FALSE;
}

void DisableAbsoluteVolume()
{
	if (!IsRunningAsAdmin())
	{
		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(NULL, exePath, MAX_PATH);
		ShellExecuteW(g_hWnd, L"runas", exePath, L"--fix-absolute-volume", NULL, SW_SHOWNORMAL);
		return;
	}

	const wchar_t* paths[] = {
		L"SYSTEM\\CurrentControlSet\\Control\\Bluetooth\\Audio\\AVRCP\\CT",
		L"SYSTEM\\ControlSet001\\Control\\Bluetooth\\Audio\\AVRCP\\CT",
		L"SYSTEM\\CurrentControlSet\\Services\\HidBth\\Parameters",
		L"SYSTEM\\CurrentControlSet\\Services\\BthAvrcpTg\\Parameters",
		L"SOFTWARE\\Microsoft\\Bluetooth\\Audio\\AVRCP\\CT"
	};

	bool success = false;
	for (auto path : paths)
	{
		HKEY hKey;
		if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS)
		{
			DWORD val1 = 1;
			DWORD val0 = 0;
			RegSetValueExW(hKey, L"DisableAbsoluteVolume", 0, REG_DWORD, (const BYTE*)&val1, sizeof(val1));
			RegSetValueExW(hKey, L"EnableAbsoluteVolume", 0, REG_DWORD, (const BYTE*)&val0, sizeof(val0));
			RegCloseKey(hKey);
			success = true;
		}
	}

	if (success)
	{
		TaskDialog(g_hWnd, NULL, _(L"System Fix Applied"), _(L"Registry paths for Absolute Volume have been updated.\n\nYou MUST REBOOT your laptop now for this to take effect."), NULL, TDCBF_OK_BUTTON, TD_INFORMATION_ICON, NULL);
	}
}

void RevertAbsoluteVolume()
{
	if (!IsRunningAsAdmin())
	{
		TaskDialog(g_hWnd, NULL, _(L"Admin Required"), _(L"Please run the app as Administrator to revert registry changes."), NULL, TDCBF_OK_BUTTON, TD_WARNING_ICON, NULL);
		return;
	}

	const wchar_t* paths[] = {
		L"SYSTEM\\CurrentControlSet\\Control\\Bluetooth\\Audio\\AVRCP\\CT",
		L"SYSTEM\\ControlSet001\\Control\\Bluetooth\\Audio\\AVRCP\\CT",
		L"SYSTEM\\CurrentControlSet\\Services\\HidBth\\Parameters",
		L"SYSTEM\\CurrentControlSet\\Services\\BthAvrcpTg\\Parameters",
		L"SOFTWARE\\Microsoft\\Bluetooth\\Audio\\AVRCP\\CT"
	};

	bool success = false;
	for (auto path : paths)
	{
		HKEY hKey;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
		{
			DWORD val0 = 0;
			RegSetValueExW(hKey, L"DisableAbsoluteVolume", 0, REG_DWORD, (const BYTE*)&val0, sizeof(val0));
			RegSetValueExW(hKey, L"EnableAbsoluteVolume", 0, REG_DWORD, (const BYTE*)&val0, sizeof(val0));
			RegCloseKey(hKey);
			success = true;
		}
	}

	if (success)
	{
		TaskDialog(g_hWnd, NULL, _(L"Fix Reverted"), _(L"Absolute Volume sync has been restored to default.\n\nYou MUST REBOOT for this to take effect."), NULL, TDCBF_OK_BUTTON, TD_INFORMATION_ICON, NULL);
	}
}

void SetRunAtStartup(bool enable)
{
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
	{
		if (enable)
		{
			wchar_t exePath[MAX_PATH];
			GetModuleFileNameW(NULL, exePath, MAX_PATH);
			RegSetValueExW(hKey, L"AudioPlaybackConnector", 0, REG_SZ, (const BYTE*)exePath, static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
		}
		else
		{
			RegDeleteValueW(hKey, L"AudioPlaybackConnector");
		}
		RegCloseKey(hKey);
	}
}
