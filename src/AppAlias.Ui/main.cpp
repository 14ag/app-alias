#include "AppAliasCore.h"

#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace
{
    constexpr int AliasListId = 1001;
    constexpr int RefreshButtonId = 1002;
    constexpr int VerifyButtonId = 1003;
    constexpr int RemoveButtonId = 1004;
    constexpr UINT RefreshCompleteMessage = WM_APP + 1;

    struct WindowState
    {
        HWND list = nullptr;
        std::wstring selectedAlias;
        bool refreshInFlight = false;
    };

    WindowState* State(HWND hwnd)
    {
        return reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    void AddColumn(HWND list, int index, int width, const wchar_t* text)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.cx = width;
        column.pszText = const_cast<wchar_t*>(text);
        column.iSubItem = index;
        ListView_InsertColumn(list, index, &column);
    }

    void SetCell(HWND list, int row, int column, const std::wstring& text)
    {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.iSubItem = column;
        item.pszText = const_cast<wchar_t*>(text.c_str());
        ListView_SetItem(list, &item);
    }

    std::wstring SelectedAlias(HWND list)
    {
        const int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        if (index < 0)
        {
            return {};
        }

        wchar_t buffer[MAX_PATH]{};
        ListView_GetItemText(list, index, 0, buffer, MAX_PATH);
        return buffer;
    }

    void SelectAlias(HWND list, const std::wstring& alias)
    {
        if (alias.empty())
        {
            return;
        }

        const int count = ListView_GetItemCount(list);
        for (int index = 0; index < count; ++index)
        {
            wchar_t buffer[MAX_PATH]{};
            ListView_GetItemText(list, index, 0, buffer, MAX_PATH);
            if (alias == buffer)
            {
                ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(list, index, FALSE);
                return;
            }
        }
    }

    void PopulateList(WindowState& state, const std::vector<appalias::AliasRecord>& records)
    {
        ListView_DeleteAllItems(state.list);
        for (int index = 0; index < static_cast<int>(records.size()); ++index)
        {
            const auto& record = records[static_cast<size_t>(index)];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.pszText = const_cast<wchar_t*>(record.alias.c_str());
            ListView_InsertItem(state.list, &item);
            SetCell(state.list, index, 1, record.targetPath.wstring());
            SetCell(state.list, index, 2, record.packageName);
            SetCell(state.list, index, 3, record.owned ? L"owned" : L"foreign");
            SetCell(state.list, index, 4, record.stubIsAppExecLink ? L"settings" : L"missing");
        }
        SelectAlias(state.list, state.selectedAlias);
    }

    void Refresh(HWND hwnd)
    {
        WindowState* state = State(hwnd);
        if (!state || !state->list || state->refreshInFlight)
        {
            return;
        }

        state->selectedAlias = SelectedAlias(state->list);
        state->refreshInFlight = true;

        std::thread([hwnd] {
            auto records = std::make_unique<std::vector<appalias::AliasRecord>>();
            try
            {
                *records = appalias::ListAliases();
            }
            catch (...)
            {
            }

            if (!PostMessageW(hwnd, RefreshCompleteMessage, 0, reinterpret_cast<LPARAM>(records.get())))
            {
                return;
            }
            records.release();
        }).detach();
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            auto state = std::make_unique<WindowState>();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));

            CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE, 12, 12, 88, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(RefreshButtonId)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Verify", WS_CHILD | WS_VISIBLE, 108, 12, 88, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(VerifyButtonId)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE, 204, 12, 88, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(RemoveButtonId)), nullptr, nullptr);

            state->list = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 12, 52, 940, 420, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(AliasListId)), nullptr, nullptr);
            ListView_SetExtendedListViewStyle(state->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            AddColumn(state->list, 0, 150, L"Alias");
            AddColumn(state->list, 1, 300, L"Target");
            AddColumn(state->list, 2, 260, L"Package");
            AddColumn(state->list, 3, 90, L"Owner");
            AddColumn(state->list, 4, 120, L"Settings");
            state.release();
            Refresh(hwnd);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == RefreshButtonId)
            {
                Refresh(hwnd);
            }
            else if (LOWORD(wparam) == VerifyButtonId)
            {
                WindowState* state = State(hwnd);
                const auto alias = state ? SelectedAlias(state->list) : std::wstring{};
                if (!alias.empty())
                {
                    const auto result = appalias::VerifyAlias(alias);
                    MessageBoxW(hwnd, result.message.c_str(), L"Verify", MB_OK);
                    Refresh(hwnd);
                }
            }
            else if (LOWORD(wparam) == RemoveButtonId)
            {
                WindowState* state = State(hwnd);
                const auto alias = state ? SelectedAlias(state->list) : std::wstring{};
                if (!alias.empty())
                {
                    const auto result = appalias::RemoveAliasByAlias(alias);
                    MessageBoxW(hwnd, result.message.c_str(), L"Remove", MB_OK);
                    Refresh(hwnd);
                }
            }
            return 0;
        case RefreshCompleteMessage:
        {
            std::unique_ptr<std::vector<appalias::AliasRecord>> records(reinterpret_cast<std::vector<appalias::AliasRecord>*>(lparam));
            WindowState* state = State(hwnd);
            if (state)
            {
                state->refreshInFlight = false;
                PopulateList(*state, *records);
            }
            return 0;
        }
        case WM_SIZE:
            if (WindowState* state = State(hwnd); state && state->list)
            {
                MoveWindow(state->list, 12, 52, LOWORD(lparam) - 24, HIWORD(lparam) - 64, TRUE);
            }
            return 0;
        case WM_DESTROY:
            delete State(hwnd);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"AppAliasUiWindow";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&windowClass);

    HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"AppAlias Generator", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 560, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
    {
        return 1;
    }

    ShowWindow(hwnd, show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
