#include "AppAliasCore.h"

#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr int AliasListId = 1001;
    constexpr int RefreshButtonId = 1002;
    constexpr int VerifyButtonId = 1003;
    constexpr int RemoveButtonId = 1004;
    constexpr int StatusTextId = 1005;
    constexpr UINT RefreshCompleteMessage = WM_APP + 1;
    constexpr UINT ActionCompleteMessage = WM_APP + 2;

    enum class ActionKind
    {
        Verify,
        Remove
    };

    struct RefreshResult
    {
        std::vector<appalias::AliasRecord> records;
        std::wstring error;
    };

    struct ActionResult
    {
        ActionKind kind = ActionKind::Verify;
        appalias::OperationResult result;
    };

    struct WindowState
    {
        HWND list = nullptr;
        HWND refreshButton = nullptr;
        HWND verifyButton = nullptr;
        HWND removeButton = nullptr;
        HWND status = nullptr;
        std::wstring selectedAlias;
        bool refreshInFlight = false;
        bool actionInFlight = false;
        std::thread refreshThread;
        std::thread actionThread;
        int dpi = 96;
    };

    WindowState* State(HWND hwnd)
    {
        return reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    int Scale(int value, int dpi)
    {
        return MulDiv(value, dpi, 96);
    }

    void SetBusy(WindowState& state, bool busy)
    {
        EnableWindow(state.refreshButton, !busy);
        EnableWindow(state.verifyButton, !busy);
        EnableWindow(state.removeButton, !busy);
    }

    void SetStatus(WindowState& state, const std::wstring& text)
    {
        if (state.status)
        {
            SetWindowTextW(state.status, text.c_str());
        }
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

    void Layout(HWND hwnd, WindowState& state, int width, int height)
    {
        const int margin = Scale(12, state.dpi);
        const int buttonY = Scale(12, state.dpi);
        const int buttonW = Scale(88, state.dpi);
        const int buttonH = Scale(28, state.dpi);
        const int gap = Scale(8, state.dpi);
        const int listY = Scale(52, state.dpi);
        const int statusH = Scale(24, state.dpi);

        MoveWindow(state.refreshButton, margin, buttonY, buttonW, buttonH, TRUE);
        MoveWindow(state.verifyButton, margin + buttonW + gap, buttonY, buttonW, buttonH, TRUE);
        MoveWindow(state.removeButton, margin + ((buttonW + gap) * 2), buttonY, buttonW, buttonH, TRUE);
        MoveWindow(state.list, margin, listY, width - (margin * 2), height - listY - statusH - margin, TRUE);
        MoveWindow(state.status, margin, height - statusH - margin, width - (margin * 2), statusH, TRUE);
        ListView_SetColumnWidth(state.list, 0, Scale(150, state.dpi));
        ListView_SetColumnWidth(state.list, 1, Scale(300, state.dpi));
        ListView_SetColumnWidth(state.list, 2, Scale(260, state.dpi));
        ListView_SetColumnWidth(state.list, 3, Scale(90, state.dpi));
        ListView_SetColumnWidth(state.list, 4, Scale(120, state.dpi));
        UNREFERENCED_PARAMETER(hwnd);
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
        SetBusy(*state, true);
        SetStatus(*state, L"Refreshing...");

        if (state->refreshThread.joinable())
        {
            state->refreshThread.join();
        }

        state->refreshThread = std::thread([hwnd] {
            auto result = std::make_unique<RefreshResult>();
            try
            {
                result->records = appalias::ListAliases();
            }
            catch (const std::exception&)
            {
                result->error = L"ListAliases failed";
            }

            if (!PostMessageW(hwnd, RefreshCompleteMessage, 0, reinterpret_cast<LPARAM>(result.get())))
            {
                return;
            }
            result.release();
        });
    }

    void RunAction(HWND hwnd, ActionKind kind, std::wstring alias)
    {
        WindowState* state = State(hwnd);
        if (!state || state->actionInFlight || state->refreshInFlight)
        {
            return;
        }

        state->actionInFlight = true;
        SetBusy(*state, true);
        SetStatus(*state, kind == ActionKind::Verify ? L"Verifying..." : L"Removing...");

        if (state->actionThread.joinable())
        {
            state->actionThread.join();
        }

        state->actionThread = std::thread([hwnd, kind, alias = std::move(alias)] {
            auto result = std::make_unique<ActionResult>();
            result->kind = kind;
            result->result = kind == ActionKind::Verify ? appalias::VerifyAlias(alias) : appalias::RemoveAliasByAlias(alias);
            if (!PostMessageW(hwnd, ActionCompleteMessage, 0, reinterpret_cast<LPARAM>(result.get())))
            {
                return;
            }
            result.release();
        });
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            auto state = std::make_unique<WindowState>();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));

            state->dpi = GetDpiForWindow(hwnd);
            state->refreshButton = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(RefreshButtonId)), nullptr, nullptr);
            state->verifyButton = CreateWindowW(L"BUTTON", L"Verify", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(VerifyButtonId)), nullptr, nullptr);
            state->removeButton = CreateWindowW(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(RemoveButtonId)), nullptr, nullptr);

            state->list = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(AliasListId)), nullptr, nullptr);
            state->status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(StatusTextId)), nullptr, nullptr);
            ListView_SetExtendedListViewStyle(state->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            AddColumn(state->list, 0, 150, L"Alias");
            AddColumn(state->list, 1, 300, L"Target");
            AddColumn(state->list, 2, 260, L"Package");
            AddColumn(state->list, 3, 90, L"Owner");
            AddColumn(state->list, 4, 120, L"Settings");
            RECT client{};
            GetClientRect(hwnd, &client);
            Layout(hwnd, *state, client.right - client.left, client.bottom - client.top);
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
                    RunAction(hwnd, ActionKind::Verify, alias);
                }
            }
            else if (LOWORD(wparam) == RemoveButtonId)
            {
                WindowState* state = State(hwnd);
                const auto alias = state ? SelectedAlias(state->list) : std::wstring{};
                if (!alias.empty())
                {
                    RunAction(hwnd, ActionKind::Remove, alias);
                }
            }
            return 0;
        case RefreshCompleteMessage:
        {
            std::unique_ptr<RefreshResult> result(reinterpret_cast<RefreshResult*>(lparam));
            WindowState* state = State(hwnd);
            if (state)
            {
                if (state->refreshThread.joinable())
                {
                    state->refreshThread.join();
                }
                state->refreshInFlight = false;
                SetBusy(*state, false);
                if (!result->error.empty())
                {
                    SetStatus(*state, L"Refresh failed: " + result->error);
                }
                else
                {
                    PopulateList(*state, result->records);
                    SetStatus(*state, L"Ready");
                }
            }
            return 0;
        }
        case ActionCompleteMessage:
        {
            std::unique_ptr<ActionResult> result(reinterpret_cast<ActionResult*>(lparam));
            WindowState* state = State(hwnd);
            if (state)
            {
                if (state->actionThread.joinable())
                {
                    state->actionThread.join();
                }
                state->actionInFlight = false;
                SetBusy(*state, false);
                SetStatus(*state, result->result.message);
                if (result->kind == ActionKind::Remove && result->result.succeeded)
                {
                    Refresh(hwnd);
                }
            }
            return 0;
        }
        case WM_SIZE:
            if (WindowState* state = State(hwnd); state && state->list)
            {
                Layout(hwnd, *state, LOWORD(lparam), HIWORD(lparam));
            }
            return 0;
        case WM_DPICHANGED:
            if (WindowState* state = State(hwnd))
            {
                state->dpi = HIWORD(wparam);
                const RECT* rect = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_DESTROY:
            if (WindowState* state = State(hwnd))
            {
                if (state->refreshThread.joinable())
                {
                    state->refreshThread.join();
                }
                if (state->actionThread.joinable())
                {
                    state->actionThread.join();
                }
                delete state;
            }
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
    if (!RegisterClassW(&windowClass))
    {
        return 1;
    }

    const int dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"AppAlias Generator", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, Scale(980, dpi), Scale(560, dpi), nullptr, nullptr, instance, nullptr);
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
