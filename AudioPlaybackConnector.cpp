#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupVolumeFlyout();
void SetupMenu();
void UpdateVolume();
void SetupEndpointVolume();
void TeardownEndpointVolume();
void DisableAbsoluteVolume();
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();

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

	// Using 1x1 SHOWN transparent window - most stable for hosting WinRT Flyouts/Pickers
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));
	ShowWindow(g_hWnd, SW_SHOW);

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
	{
		UINT uMsg = LOWORD(lParam);
		switch (uMsg)
		{
		case WM_LBUTTONUP:
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			static DWORD s_lastTick = 0;
			if (GetTickCount() - s_lastTick < 500) break;
			s_lastTick = GetTickCount();

			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			if (FAILED(Shell_NotifyIconGetRect(&g_niid, &iconRect)))
			{
				POINT pt;
				GetCursorPos(&pt);
				iconRect = { pt.x - 8, pt.y - 8, pt.x + 8, pt.y + 8 };
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetForegroundWindow(hWnd);
			try {
				g_devicePicker.Show(rect, Placement::Above);
			} catch (...) {
				LOG_CAUGHT_EXCEPTION();
			}
		}
		break;
		case WM_RBUTTONUP:
		case WM_CONTEXTMENU:
		{
			static DWORD s_lastTick = 0;
			if (GetTickCount() - s_lastTick < 500) break;
			s_lastTick = GetTickCount();

			POINT pt;
			if (uMsg == WM_CONTEXTMENU && LOWORD(lParam) == WM_CONTEXTMENU) {
				// VERSION_4 sends coordinates in wParam for WM_CONTEXTMENU
				pt.x = GET_X_LPARAM(wParam);
				pt.y = GET_Y_LPARAM(wParam);
			} else {
				GetCursorPos(&pt);
			}

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(pt.x * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(pt.y * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetForegroundWindow(hWnd);
			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
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
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

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
		SaveSettings();
	});

	g_volumeFlyout = flyout;
}

void SetupMenu()
{
	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	// Lock toggle: blocks phone volume buttons from changing PC volume
	static ToggleMenuFlyoutItem lockItem;
	lockItem.Text(_(L"Lock Phone Volume Buttons"));
	lockItem.IsChecked(g_volumeLock);
	lockItem.Click([](const auto&, const auto&) {
		g_volumeLock = lockItem.IsChecked();
		// When enabling, immediately restore our preferred master volume level
		if (g_volumeLock && g_endpointVolume)
			g_endpointVolume->SetMasterVolumeLevelScalar(g_lastMasterVolume, &g_ourVolumeGuid);
		SaveSettings();
	});

	FontIcon volumeIcon;
	volumeIcon.Glyph(L"\xE767");

	MenuFlyoutItem volumeItem;
	volumeItem.Text(_(L"Volume Control"));
	volumeItem.Icon(volumeIcon);
	volumeItem.Click([](const auto&, const auto&) {
		POINT pt;
		GetCursorPos(&pt);
		auto dpi = GetDpiForWindow(g_hWnd);
		Point point = {
			static_cast<float>(pt.x * USER_DEFAULT_SCREEN_DPI / dpi),
			static_cast<float>(pt.y * USER_DEFAULT_SCREEN_DPI / dpi)
		};
		using namespace winrt::Windows::UI::Xaml::Controls::Primitives;
		FlyoutShowOptions options;
		options.Position(point);
		g_volumeFlyout.ShowAt(g_xamlCanvas, options);
	});

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

		POINT pt;
		GetCursorPos(&pt);
		auto dpi = GetDpiForWindow(g_hWnd);
		Point point = {
			static_cast<float>(pt.x * USER_DEFAULT_SCREEN_DPI / dpi),
			static_cast<float>(pt.y * USER_DEFAULT_SCREEN_DPI / dpi)
		};

		using namespace winrt::Windows::UI::Xaml::Controls::Primitives;
		FlyoutShowOptions options;
		options.Position(point);
		g_xamlFlyout.ShowAt(g_xamlCanvas, options);
	});

	MenuFlyout menu;
	menu.Items().Append(settingsItem);
	menu.Items().Append(lockItem);
	menu.Items().Append(volumeItem);
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(FocusState::Pointer);
		}
	});

	g_xamlMenu = menu;
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

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
						g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
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
		picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	else
	{
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	ConnectDevice(picker, device);
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
		ConnectDevice(sender, args.SelectedDevice());
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

	Shell_NotifyIconW(NIM_DELETE, &g_nid);
	if (Shell_NotifyIconW(NIM_ADD, &g_nid))
	{
		Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
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

		// Register AVRCP guardian (blocks phone buttons)
		IAudioEndpointVolume* epVol = nullptr;
		winrt::check_hresult(device->Activate(__uuidof(IAudioEndpointVolume),
			CLSCTX_INPROC_SERVER, NULL, (void**)&epVol));
		g_endpointVolume = epVol;

		// Initialize our Authority levels
		float currentVol = 0.5f;
		BOOL currentMute = FALSE;
		if (SUCCEEDED(g_endpointVolume->GetMasterVolumeLevelScalar(&currentVol))) g_lastMasterVolume = currentVol;
		if (SUCCEEDED(g_endpointVolume->GetMute(&currentMute))) g_lastMute = (currentMute != FALSE);

		g_volumeCallback = new VolumeCallback();
		g_endpointVolume->RegisterControlChangeNotify(g_volumeCallback);

		// Register session notifier
		IAudioSessionManager2* mgr = nullptr;
		if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2),
			CLSCTX_INPROC_SERVER, NULL, (void**)&mgr)))
		{
			g_sessionManager = mgr;
			g_sessionNotifier = new SessionNotifier();
			mgr->RegisterSessionNotification(g_sessionNotifier);
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
	if (g_endpointVolume)
	{
		if (g_volumeCallback)
		{
			g_endpointVolume->UnregisterControlChangeNotify(g_volumeCallback);
			g_volumeCallback->Release();
			g_volumeCallback = nullptr;
		}
		g_endpointVolume->Release();
		g_endpointVolume = nullptr;
	}
	if (g_sessionManager)
	{
		if (g_sessionNotifier)
		{
			g_sessionManager->UnregisterSessionNotification(g_sessionNotifier);
			g_sessionNotifier->Release();
			g_sessionNotifier = nullptr;
		}
		g_sessionManager->Release();
		g_sessionManager = nullptr;
	}
}

void UpdateVolume()
{
	if (g_sessionManager) ApplyVolumeToOurSessions(g_sessionManager);
}
