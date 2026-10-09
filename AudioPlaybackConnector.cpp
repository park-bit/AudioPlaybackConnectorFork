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
void RevertAbsoluteVolume();
void CheckFirstRunVolumeFix();
void SetRunAtStartup(bool enable);
bool IsRunningAsAdmin();
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

	// If relaunched as admin to apply/revert the Absolute Volume fix
	if (lpCmdLine)
	{
		bool fix = wcsstr(lpCmdLine, L"--fix-absolute-volume") != nullptr;
		bool revert = wcsstr(lpCmdLine, L"--revert-absolute-volume") != nullptr;

		if (fix || revert)
		{
			const wchar_t* path = L"SYSTEM\\CurrentControlSet\\Control\\Bluetooth\\Audio\\AVRCP\\CT";
			HKEY hKey;
			LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &hKey);
			if (result != ERROR_SUCCESS)
				result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL);

			if (result == ERROR_SUCCESS)
			{
				DWORD value = fix ? 1 : 0;
				RegSetValueExW(hKey, L"DisableAbsoluteVolume", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
				RegCloseKey(hKey);
				TaskDialog(nullptr, nullptr, _(L"AudioPlaybackConnector"), 
					fix ? _(L"Bluetooth Volume Fix Applied") : _(L"Bluetooth Volume Fix Reverted"), 
					fix ? _(L"Absolute Volume has been disabled.\n\nPlease RESTART your PC for changes to take effect.") : 
					      _(L"Absolute Volume has been restored to default.\n\nPlease RESTART your PC for changes to take effect."), 
					TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
			}
			else
			{
				TaskDialog(nullptr, nullptr, _(L"AudioPlaybackConnector"), _(L"Error"), _(L"Failed to write registry key. Run as Administrator."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
			}
			return 0;
		}
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
	// Large canvas size ensures XAML popups and flyouts do not shrink or clip
	g_xamlCanvas.Width(4096);
	g_xamlCanvas.Height(4096);
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupEndpointVolume();
	CheckFirstRunVolumeFix();
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
	{
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
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

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);
			try {
				g_devicePicker.Show(rect, Placement::Above);
			} catch (...) {
				LOG_CAUGHT_EXCEPTION();
				SetWindowPos(hWnd, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_HIDEWINDOW);
			}
		}
		break;
		case WM_CONTEXTMENU:
		{
			POINT pt;
			if (LOWORD(lParam) == WM_CONTEXTMENU) {
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

			SetWindowPos(g_hWndXaml, 0, pt.x, pt.y, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
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
		g_restorePending = false;
		if (g_volumeLock && !g_absVolDisabled && g_endpointVolume)
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
	StackPanel rootPanel;
	rootPanel.Width(250);
	rootPanel.Spacing(10);
	rootPanel.Margin({ 4, 4, 4, 4 });

	TextBlock headerText;
	headerText.Text(_(L"Disconnect and Exit?"));
	headerText.FontSize(14);

	TextBlock descText;
	descText.Text(_(L"Active Bluetooth audio connections will be closed."));
	descText.TextWrapping(TextWrapping::Wrap);
	descText.FontSize(12);
	descText.Opacity(0.8);

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));
	checkbox.FontSize(12);

	Grid buttonGrid;
	ColumnDefinition col0, col1;
	col0.Width(GridLength{ 1, GridUnitType::Star });
	col1.Width(GridLength{ 1, GridUnitType::Star });
	buttonGrid.ColumnDefinitions().Append(col0);
	buttonGrid.ColumnDefinitions().Append(col1);

	Button cancelButton;
	cancelButton.Content(winrt::box_value(_(L"Cancel")));
	cancelButton.HorizontalAlignment(HorizontalAlignment::Stretch);
	cancelButton.Margin({ 0, 0, 4, 0 });
	Grid::SetColumn(cancelButton, 0);
	cancelButton.Click([](const auto&, const auto&) {
		if (g_xamlFlyout) g_xamlFlyout.Hide();
	});

	Button exitButton;
	exitButton.Content(winrt::box_value(_(L"Exit")));
	exitButton.HorizontalAlignment(HorizontalAlignment::Stretch);
	exitButton.Margin({ 4, 0, 0, 0 });
	Grid::SetColumn(exitButton, 1);
	exitButton.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	buttonGrid.Children().Append(cancelButton);
	buttonGrid.Children().Append(exitButton);

	rootPanel.Children().Append(headerText);
	rootPanel.Children().Append(descText);
	rootPanel.Children().Append(checkbox);
	rootPanel.Children().Append(buttonGrid);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(rootPanel);
	flyout.Closed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_HIDEWINDOW);
	});

	g_xamlFlyout = flyout;
}

void SetupVolumeFlyout()
{
	StackPanel rootPanel;
	rootPanel.Width(250);
	rootPanel.Spacing(8);
	rootPanel.Margin({ 4, 4, 4, 4 });

	Grid headerGrid;
	ColumnDefinition col0, col1, col2;
	col0.Width(GridLength{ 0, GridUnitType::Auto });
	col1.Width(GridLength{ 1, GridUnitType::Star });
	col2.Width(GridLength{ 0, GridUnitType::Auto });
	headerGrid.ColumnDefinitions().Append(col0);
	headerGrid.ColumnDefinitions().Append(col1);
	headerGrid.ColumnDefinitions().Append(col2);

	FontIcon volumeIcon;
	volumeIcon.Glyph(L"\xE767");
	volumeIcon.FontSize(15);
	volumeIcon.Margin({ 0, 0, 8, 0 });
	volumeIcon.VerticalAlignment(VerticalAlignment::Center);
	Grid::SetColumn(volumeIcon, 0);

	TextBlock titleText;
	titleText.Text(_(L"Bluetooth Volume"));
	titleText.FontSize(13);
	titleText.VerticalAlignment(VerticalAlignment::Center);
	Grid::SetColumn(titleText, 1);

	TextBlock percentText;
	wchar_t buf[16];
	swprintf_s(buf, L"%d%%", static_cast<int>(std::round(g_volume * 100)));
	percentText.Text(buf);
	percentText.FontSize(13);
	percentText.VerticalAlignment(VerticalAlignment::Center);
	Grid::SetColumn(percentText, 2);

	headerGrid.Children().Append(volumeIcon);
	headerGrid.Children().Append(titleText);
	headerGrid.Children().Append(percentText);

	Slider slider;
	slider.Minimum(0);
	slider.Maximum(100);
	slider.StepFrequency(1);
	slider.Value(std::round(g_volume * 100));
	slider.Width(240);
	slider.HorizontalAlignment(HorizontalAlignment::Stretch);

	TextBlock deviceStatusText;
	deviceStatusText.FontSize(11);
	deviceStatusText.Opacity(0.7);
	deviceStatusText.Text(_(L"Adjusts incoming Bluetooth audio"));

	slider.ValueChanged([percentText, volumeIcon](const auto&, const auto& args) {
		g_volume = args.NewValue() / 100.0;
		wchar_t valBuf[16];
		swprintf_s(valBuf, L"%d%%", static_cast<int>(std::round(args.NewValue())));
		percentText.Text(valBuf);
		if (args.NewValue() == 0)
			volumeIcon.Glyph(L"\xE74F");
		else if (args.NewValue() < 33)
			volumeIcon.Glyph(L"\xE992");
		else if (args.NewValue() < 66)
			volumeIcon.Glyph(L"\xE993");
		else
			volumeIcon.Glyph(L"\xE767");
		UpdateVolume();
	});

	rootPanel.Children().Append(headerGrid);
	rootPanel.Children().Append(slider);
	rootPanel.Children().Append(deviceStatusText);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(rootPanel);

	flyout.Opened([slider, percentText, deviceStatusText, volumeIcon](const auto&, const auto&) {
		int pct = static_cast<int>(std::round(g_volume * 100));
		slider.Value(pct);
		wchar_t valBuf[16];
		swprintf_s(valBuf, L"%d%%", pct);
		percentText.Text(valBuf);
		if (pct == 0)
			volumeIcon.Glyph(L"\xE74F");
		else if (pct < 33)
			volumeIcon.Glyph(L"\xE992");
		else if (pct < 66)
			volumeIcon.Glyph(L"\xE993");
		else
			volumeIcon.Glyph(L"\xE767");

		if (!g_audioPlaybackConnections.empty())
		{
			std::wstring devName = g_audioPlaybackConnections.begin()->second.first.Name().c_str();
			std::wstring statusStr = _(L"Connected: ") + devName;
			deviceStatusText.Text(statusStr);
		}
		else
		{
			deviceStatusText.Text(_(L"Adjusts incoming Bluetooth audio"));
		}
	});

	flyout.Closed([](const auto&, const auto&) {
		SaveSettings();
		SetWindowPos(g_hWnd, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_HIDEWINDOW);
	});

	g_volumeFlyout = flyout;
}

void SetupMenu()
{
	// Section 1: Actions
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE702"); // Bluetooth icon
	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon volumeIcon;
	volumeIcon.Glyph(L"\xE767");
	MenuFlyoutItem volumeItem;
	volumeItem.Text(_(L"Bluetooth Volume"));
	volumeItem.Icon(volumeIcon);
	volumeItem.Click([](const auto&, const auto&) {
		POINT pt; GetCursorPos(&pt);
		auto dpi = GetDpiForWindow(g_hWnd);
		Point point = { static_cast<float>(pt.x * USER_DEFAULT_SCREEN_DPI / dpi), static_cast<float>(pt.y * USER_DEFAULT_SCREEN_DPI / dpi) };
		using namespace winrt::Windows::UI::Xaml::Controls::Primitives;
		FlyoutShowOptions options; options.Position(point);
		SetWindowPos(g_hWndXaml, 0, pt.x, pt.y, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
		SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
		SetForegroundWindow(g_hWnd);
		g_volumeFlyout.ShowAt(g_xamlCanvas, options);
	});

	// Section 2: Preferences
	static ToggleMenuFlyoutItem lockItem;
	lockItem.Text(_(L"Lock Phone Volume Buttons"));
	lockItem.IsChecked(g_volumeLock);
	lockItem.Click([](const auto&, const auto&) {
		g_volumeLock = lockItem.IsChecked();
		if (g_volumeLock && !g_absVolDisabled && g_endpointVolume)
			g_endpointVolume->SetMasterVolumeLevelScalar(g_lastMasterVolume, &g_ourVolumeGuid);
		SaveSettings();
	});

	static ToggleMenuFlyoutItem startupItem;
	startupItem.Text(_(L"Run at Startup"));
	startupItem.IsChecked(g_runAtStartup);
	startupItem.Click([](const auto&, const auto&) {
		g_runAtStartup = startupItem.IsChecked();
		SetRunAtStartup(g_runAtStartup);
		SaveSettings();
	});

	// Section 3: Maintenance & Help
	MenuFlyoutItem revertItem;
	revertItem.Text(_(L"Revert Volume Fix"));
	FontIcon revertIcon;
	revertIcon.Glyph(L"\xE777");
	revertItem.Icon(revertIcon);
	if (!g_absVolDisabled)
	{
		revertItem.IsEnabled(false);
	}
	revertItem.Click([](const auto&, const auto&) {
		int button = 0;
		TaskDialog(g_hWnd, nullptr, _(L"Revert Volume Fix"), _(L"Restore Windows Absolute Volume?"), 
			_(L"This will re-enable phone volume button syncing with your PC master volume.\n\nRequires Administrator permission and a PC restart."), 
			TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, TD_WARNING_ICON, &button);
		if (button == IDYES)
		{
			RevertAbsoluteVolume();
		}
	});

	FontIcon helpIcon;
	helpIcon.Glyph(L"\xE897");
	MenuFlyoutItem helpItem;
	helpItem.Text(_(L"Instructions & Tips"));
	helpItem.Icon(helpIcon);
	helpItem.Click([](const auto&, const auto&) {
		TaskDialog(g_hWnd, nullptr, _(L"Instructions & Tips"), 
			_(L"AudioPlaybackConnector"), 
			_(L"Usage:\n"
			  L"- Left-Click tray icon: Select and connect or disconnect a Bluetooth device.\n"
			  L"- Right-Click tray icon: Open volume control, settings, and options.\n\n"
			  L"Features:\n"
			  L"- Bluetooth Volume: Independently control incoming Bluetooth audio.\n"
			  L"- Lock Phone Volume Buttons: Prevents phone volume buttons from altering PC master volume.\n"
			  L"- Revert Volume Fix: Restores Windows default Absolute Volume behavior.\n\n"
			  L"Tip: If phone audio is quiet or not audible, check the Bluetooth Volume slider in the tray menu."), 
			TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
	});

	// Section 4: Exit
	FontIcon exitIcon;
	exitIcon.Glyph(L"\xE711");
	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(exitIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.empty())
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}
		POINT pt; GetCursorPos(&pt);
		auto dpi = GetDpiForWindow(g_hWnd);
		Point point = { static_cast<float>(pt.x * USER_DEFAULT_SCREEN_DPI / dpi), static_cast<float>(pt.y * USER_DEFAULT_SCREEN_DPI / dpi) };
		using namespace winrt::Windows::UI::Xaml::Controls::Primitives;
		FlyoutShowOptions options; options.Position(point);
		SetWindowPos(g_hWndXaml, 0, pt.x, pt.y, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
		SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
		SetForegroundWindow(g_hWnd);
		g_xamlFlyout.ShowAt(g_xamlCanvas, options);
	});

	MenuFlyout menu;
	menu.Items().Append(settingsItem);
	menu.Items().Append(volumeItem);
	menu.Items().Append(MenuFlyoutSeparator{});
	menu.Items().Append(lockItem);
	menu.Items().Append(startupItem);
	menu.Items().Append(MenuFlyoutSeparator{});
	menu.Items().Append(revertItem);
	menu.Items().Append(helpItem);
	menu.Items().Append(MenuFlyoutSeparator{});
	menu.Items().Append(exitItem);

	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		if (menuItems.Size() > 0) menuItems.GetAt(0).Focus(FocusState::Pointer);
	});
	menu.Closed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_HIDEWINDOW);
	});

	g_xamlMenu = menu;
}

void SetRunAtStartup(bool enable)
{
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
	{
		if (enable)
		{
			wchar_t path[MAX_PATH];
			GetModuleFileNameW(NULL, path, MAX_PATH);
			RegSetValueExW(hKey, L"AudioPlaybackConnector", 0, REG_SZ, (const BYTE*)path, static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
		}
		else
		{
			RegDeleteValueW(hKey, L"AudioPlaybackConnector");
		}
		RegCloseKey(hKey);
	}
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
		wchar_t path[MAX_PATH]; GetModuleFileNameW(NULL, path, MAX_PATH);
		ShellExecuteW(NULL, L"runas", path, L"--fix-absolute-volume", NULL, SW_SHOWNORMAL);
		return;
	}
}

void RevertAbsoluteVolume()
{
	if (!IsRunningAsAdmin())
	{
		wchar_t path[MAX_PATH]; GetModuleFileNameW(NULL, path, MAX_PATH);
		ShellExecuteW(NULL, L"runas", path, L"--revert-absolute-volume", NULL, SW_SHOWNORMAL);
		return;
	}
}

void CheckFirstRunVolumeFix()
{
	if (g_absVolDisabled || g_volumeFixPrompted)
		return;

	TASKDIALOGCONFIG tdc = { sizeof(tdc) };
	tdc.hwndParent = nullptr;
	tdc.hInstance = g_hInst;
	tdc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
	tdc.pszWindowTitle = _(L"AudioPlaybackConnector");
	tdc.pszMainIcon = TD_INFORMATION_ICON;
	tdc.pszMainInstruction = _(L"Enable Bluetooth Volume Fix?");
	tdc.pszContent = _(L"By default, phone volume buttons alter your PC's master volume. Disabling Absolute Volume keeps phone and PC volumes separate.\n\nApplying this fix requires Administrator permission and a PC restart.");

	TASKDIALOG_BUTTON buttons[] = {
		{ 101, _(L"Apply Fix (Recommended)\nElevate as Administrator to disable Absolute Volume") },
		{ 102, _(L"Not Now\nKeep current settings") }
	};
	tdc.pButtons = buttons;
	tdc.cButtons = ARRAYSIZE(buttons);
	tdc.nDefaultButton = 101;

	int selectedButton = 0;
	if (SUCCEEDED(TaskDialogIndirect(&tdc, &selectedButton, nullptr, nullptr)))
	{
		g_volumeFixPrompted = true;
		SaveSettings();
		if (selectedButton == 101)
		{
			DisableAbsoluteVolume();
		}
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);
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
					if (it != g_audioPlaybackConnections.end()) { g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None); g_audioPlaybackConnections.erase(it); }
					sender.Close();
				}
			});
			co_await connection.StartAsync();
			auto result = co_await connection.OpenAsync();
			if (result.Status() == AudioPlaybackConnectionOpenResultStatus::Success) picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
			else picker.SetDisplayStatus(device, _(L"Failed"), DevicePickerDisplayStatusOptions::ShowRetryButton);
		}
	}
	catch (...) { LOG_CAUGHT_EXCEPTION(); }
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
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) { ConnectDevice(sender, args.SelectedDevice()); });
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		auto device = args.Device();
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end()) { it->second.second.Close(); g_audioPlaybackConnections.erase(it); }
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
	});
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	auto size = SizeofResource(g_hInst, hRes);
	auto hResData = LoadResource(g_hInst, hRes);
	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);
	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue);
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;
	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
		}
	}
}

static void ApplyVolumeToOurSessions(IAudioSessionManager2* mgr)
{
	IAudioSessionEnumerator* sessionEnum = nullptr;
	if (FAILED(mgr->GetSessionEnumerator(&sessionEnum))) return;
	int count = 0; sessionEnum->GetCount(&count);
	for (int i = 0; i < count; ++i)
	{
		IAudioSessionControl* ctrl = nullptr; if (FAILED(sessionEnum->GetSession(i, &ctrl))) continue;
		IAudioSessionControl2* ctrl2 = nullptr;
		if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctrl2)))
		{
			if (IsBluetoothSession(ctrl2, ctrl))
			{
				ISimpleAudioVolume* vol = nullptr;
				if (SUCCEEDED(ctrl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&vol))) { vol->SetMasterVolume(static_cast<float>(g_volume * 0.7), nullptr); vol->Release(); }
			}
			ctrl2->Release();
		}
		ctrl->Release();
	}
	sessionEnum->Release();
}

class VolumeCallback : public IAudioEndpointVolumeCallback
{
public:
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override { auto r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return r; }
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) { *ppv = static_cast<IAudioEndpointVolumeCallback*>(this); AddRef(); return S_OK; }
		*ppv = nullptr; return E_NOINTERFACE;
	}
	HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify) override
	{
		if (IsEqualGUID(pNotify->guidEventContext, g_ourVolumeGuid)) return S_OK;
		// Absolute Volume off (or lock off): phone buttons never reach the PC endpoint.
		// Every change seen here is a PC change. Track it, never revert it.
		if (g_absVolDisabled || !g_volumeLock || !g_hWnd)
		{
			g_lastMasterVolume = pNotify->fMasterVolume; g_lastMute = pNotify->bMuted;
			return S_OK;
		}
		// Fallback path (Absolute Volume still on). Guess source by input idle time only.
		// Old code also treated any non-null context GUID as remote. Windows own volume UI sets one,
		// so normal PC changes were reverted. That was the glitch.
		bool isRemote = false;
		LASTINPUTINFO lii = { sizeof(lii) };
		if (GetLastInputInfo(&lii) && (GetTickCount() - lii.dwTime) > 1500) isRemote = true;
		if (isRemote)
		{
			g_volume = pNotify->fMasterVolume;
			if (!g_restorePending.exchange(true)) PostMessageW(g_hWnd, WM_RESTORE_VOLUME, 0, 0);
		}
		else { g_lastMasterVolume = pNotify->fMasterVolume; g_lastMute = pNotify->bMuted; }
		return S_OK;
	}
private:
	long m_ref = 1;
};
static VolumeCallback* g_volumeCallback = nullptr;

class SessionNotifier : public IAudioSessionNotification
{
public:
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override { auto r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return r; }
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification)) { *ppv = static_cast<IAudioSessionNotification*>(this); AddRef(); return S_OK; }
		*ppv = nullptr; return E_NOINTERFACE;
	}
	HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* pNewSession) override
	{
		IAudioSessionControl2* ctrl2 = nullptr;
		if (SUCCEEDED(pNewSession->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctrl2)))
		{
			if (IsBluetoothSession(ctrl2, pNewSession))
			{
				ISimpleAudioVolume* vol = nullptr;
				if (SUCCEEDED(pNewSession->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&vol))) { vol->SetMasterVolume(static_cast<float>(g_volume * 0.7), nullptr); vol->Release(); }
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
	{
		DWORD v = 0, cb = sizeof(v);
		g_absVolDisabled = RegGetValueW(HKEY_LOCAL_MACHINE, LR"(SYSTEM\CurrentControlSet\Control\Bluetooth\Audio\AVRCP\CT)", L"DisableAbsoluteVolume", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS && v == 1;
	}
	try
	{
		IMMDeviceEnumerator* enumerator = nullptr;
		CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
		IMMDevice* device = nullptr; enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device); enumerator->Release();
		IAudioEndpointVolume* epVol = nullptr; device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL, (void**)&epVol);
		g_endpointVolume = epVol;
		float currentVol = 0.5f; BOOL currentMute = FALSE;
		if (SUCCEEDED(g_endpointVolume->GetMasterVolumeLevelScalar(&currentVol))) g_lastMasterVolume = currentVol;
		if (SUCCEEDED(g_endpointVolume->GetMute(&currentMute))) g_lastMute = (currentMute != FALSE);
		g_volumeCallback = new VolumeCallback(); g_endpointVolume->RegisterControlChangeNotify(g_volumeCallback);
		IAudioSessionManager2* mgr = nullptr;
		if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, NULL, (void**)&mgr)))
		{
			g_sessionManager = mgr; g_sessionNotifier = new SessionNotifier(); mgr->RegisterSessionNotification(g_sessionNotifier);
			ApplyVolumeToOurSessions(mgr);
		}
		device->Release();
	}
	catch (...) {}
}

void TeardownEndpointVolume()
{
	if (g_endpointVolume) { if (g_volumeCallback) { g_endpointVolume->UnregisterControlChangeNotify(g_volumeCallback); g_volumeCallback->Release(); } g_endpointVolume->Release(); }
	if (g_sessionManager) { if (g_sessionNotifier) { g_sessionManager->UnregisterSessionNotification(g_sessionNotifier); g_sessionNotifier->Release(); } g_sessionManager->Release(); }
}

void UpdateVolume() { if (g_sessionManager) ApplyVolumeToOurSessions(g_sessionManager); }
