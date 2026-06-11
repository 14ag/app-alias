#include "AppAliasCore.h"

#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace
{
    constexpr int AliasListId = 1001;
    constexpr int RefreshButtonId = 1002;
    constexpr int VerifyButtonId = 1003;
    constexpr int RemoveButtonId = 1004;
    HWND g_list = nullptr;

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
        item.iItem = row;
        item.iSubItem = column;
        item.pszText = const_cast<wchar_t*>(text.c_str());
        ListView_SetItem(list, &item);
    }

    void Refresh()
    {
        if (!g_list)
        {
            return;
        }

        ListView_DeleteAllItems(g_list);
        const auto records = appalias::ListAliases();
        for (int index = 0; index < static_cast<int>(records.size()); ++index)
        {
            const auto& record = records[static_cast<size_t>(index)];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.pszText = const_cast<wchar_t*>(record.alias.c_str());
            ListView_InsertItem(g_list, &item);
            SetCell(g_list, index, 1, record.targetPath.wstring());
            SetCell(g_list, index, 2, record.packageName);
            SetCell(g_list, index, 3, record.owned ? L"owned" : L"foreign");
            SetCell(g_list, index, 4, record.stubIsAppExecLink ? L"settings" : L"missing");
        }
    }

    std::wstring SelectedAlias()
    {
        const int index = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
        if (index < 0)
        {
            return {};
        }

        wchar_t buffer[260]{};
        ListView_GetItemText(g_list, index, 0, buffer, 260);
        return buffer;
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            INITCOMMONCONTROLSEX controls{};
            controls.dwSize = sizeof(controls);
            controls.dwICC = ICC_LISTVIEW_CLASSES;
            InitCommonControlsEx(&controls);

            CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE, 12, 12, 88, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(RefreshButtonId)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Verify", WS_CHILD | WS_VISIBLE, 108, 12, 88, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(VerifyButtonId)), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE, 204, 12, 88, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(RemoveButtonId)), nullptr, nullptr);

            g_list = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 12, 52, 940, 420, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(AliasListId)), nullptr, nullptr);
            ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            AddColumn(g_list, 0, 150, L"Alias");
            AddColumn(g_list, 1, 300, L"Target");
            AddColumn(g_list, 2, 260, L"Package");
            AddColumn(g_list, 3, 90, L"Owner");
            AddColumn(g_list, 4, 120, L"Settings");
            Refresh();
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == RefreshButtonId)
            {
                Refresh();
            }
            else if (LOWORD(wparam) == VerifyButtonId)
            {
                const auto alias = SelectedAlias();
                if (!alias.empty())
                {
                    const auto result = appalias::VerifyAlias(alias);
                    MessageBoxW(hwnd, result.message.c_str(), L"Verify", MB_OK);
                    Refresh();
                }
            }
            else if (LOWORD(wparam) == RemoveButtonId)
            {
                const auto alias = SelectedAlias();
                if (!alias.empty())
                {
                    const auto result = appalias::RemoveAliasByAlias(alias);
                    MessageBoxW(hwnd, result.message.c_str(), L"Remove", MB_OK);
                    Refresh();
                }
            }
            return 0;
        case WM_SIZE:
            if (g_list)
            {
                MoveWindow(g_list, 12, 52, LOWORD(lparam) - 24, HIWORD(lparam) - 64, TRUE);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"AppAliasUiWindow";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
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
