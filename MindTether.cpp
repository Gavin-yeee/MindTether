// MindTether.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include "MindTether.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>
#include <ctime>
#include <vector>
#include <fstream>
#include <cstdlib>   // 提供 _wtof, _wtoi
//数据收集相关头文件
#include <sstream>
#include <iomanip>


#define MAX_LOADSTRING 100
//倒计时结束后四个按钮ID
#define ID_BTN_CONTINUE  1001
#define ID_BTN_REST      1002
#define ID_BTN_SETTINGS  1003
#define ID_BTN_EXIT      1004
// ==== 数据收集相关 ====
#define THINK_THRESHOLD_SEC   155   // 时间阈值，2分35秒，超过这时间视为休息，测试时


// 全局变量:
HINSTANCE hInst;                                // 当前实例
WCHAR szTitle[MAX_LOADSTRING];                  // 标题栏文本
WCHAR szWindowClass[MAX_LOADSTRING];            // 主窗口类名
IMMDeviceEnumerator* pEnumerator = nullptr;     //指针，类型为IMMDeviceEnumerator，作用是找到本地电脑上的音频设备，扬声器/耳机
IMMDevice* pDevice = nullptr;
IAudioSessionManager2* pSessionManager = nullptr;
IAudioSessionEnumerator* pSessionEnumerator = nullptr;
// 计时相关
int remindInterval = 20;     // 休息时间/提醒间隔（秒），测试时可先用 20 秒,原定600（10分钟）
bool reminded = false;         // 是否已触发提醒（避免反复弹框）
int updateCounter = 0;        // 计数器，用来控制检测间隔
bool lastPlayingState = false; // 缓存上一次的播放状态
int restSeconds = 0;   // 剩余休息秒数，0 表示不在休息中或已提醒
HWND hStatusText = nullptr;   // 用于显示状态的静态文本框句柄
HWND hBtnContinue = nullptr;
HWND hBtnRest = nullptr;
HWND hBtnSettings = nullptr;
HWND hBtnExit = nullptr;
double defaultRestMinutes = 10.0;   // 默认休息分钟数
int    ringCount = 6;              // 蜂鸣次数
//数据收集相关
bool isPausing = false;              // 是否正处于“暂停”状态
const int LEARN = 1;
const int THINK = 2;
const int REST = 3;
time_t pauseStartTimeStamp = 0;   // 记录暂停开始的时刻（time_t）
time_t learningStartTime = 0;   // 记录当前学习段开始的时刻（time_t）,用于计算本次学习持续多久


// 此代码模块中包含的函数的前向声明:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    SettingsProc(HWND, UINT, WPARAM, LPARAM);

// 根据进程 ID 获取进程名，返回 ANSI 字符串（方便 cout 输出）
std::string GetProcessName(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return "Unknown";

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                // 将宽字符进程名转换成 std::string
                int len = WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, NULL, 0, NULL, NULL);
                std::string name(len, 0);
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, &name[0], len, NULL, NULL);
                // 去除末尾的 '\0'（string 构造已经自动处理了，但为了保险可以 pop_back）
                if (!name.empty() && name.back() == '\0') name.pop_back();
                CloseHandle(snapshot);
                return name;
            }
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return "Unknown";
}

// 检查 Edge 浏览器是否正在播放音频
bool IsEdgePlaying(IAudioSessionManager2* pSessionManager) {
    if (!pSessionManager) return false;

    IAudioSessionEnumerator* pEnumerator = nullptr;
    if (FAILED(pSessionManager->GetSessionEnumerator(&pEnumerator)))
        return false;

    bool playing = false;
    int count = 0;
    pEnumerator->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        IAudioSessionControl* pControl = nullptr;
        if (FAILED(pEnumerator->GetSession(i, &pControl))) continue;

        // 1. 获取 IAudioSessionControl2 以得到进程 ID
        IAudioSessionControl2* pControl2 = nullptr;
        if (FAILED(pControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pControl2))) {
            pControl->Release();
            continue;
        }

        DWORD pid = 0;
        pControl2->GetProcessId(&pid);
        if (pid != 0) {
            std::string name = GetProcessName(pid);
            // 2. 仅对 Edge 的会话进行音量检测
            if (name == "msedge.exe") {
                // 通过音量峰值精确判断是否在播放
                IAudioMeterInformation* pMeter = nullptr;
                if (SUCCEEDED(pControl->QueryInterface(__uuidof(IAudioMeterInformation), (void**)&pMeter))) {
                    UINT channels = 0;
                    pMeter->GetMeteringChannelCount(&channels);
                    if (channels > 0) {
                        std::vector<float> peaks(channels);
                        if (SUCCEEDED(pMeter->GetChannelsPeakValues(channels, peaks.data()))) {
                            for (UINT j = 0; j < channels; ++j) {  // 注意变量名改为 j，避免和外层 i 冲突
                                if (peaks[j] > 0.001f) {
                                    playing = true;
                                    break;
                                }
                            }
                        }
                    }
                    pMeter->Release();
                }
            }
        }
        pControl2->Release();
        pControl->Release();
        if (playing) break; // 找到就提前结束
    }
    pEnumerator->Release();
    return playing;
}

//设置配置保存函数
void SaveSettings() {       
    std::ofstream file("MindTether_settings.txt");
    if (file) {
        file << defaultRestMinutes << "\n" << ringCount;
    }
}           

//加载配置函数
bool LoadSettings() {
    std::ifstream file("MindTether_settings.txt");
    if (file) {
        file >> defaultRestMinutes >> ringCount;
        if (defaultRestMinutes <= 0) defaultRestMinutes = 10.0;
        if (ringCount <= 0) ringCount = 6;
        remindInterval = static_cast<int>(defaultRestMinutes * 60);
        return true;
    }
    return false;
}

//蜂鸣器提醒休息时间结束
void Remind() {
    // 播放三次 2000Hz 的刺耳声，每次持续 400 毫秒，间隔 200 毫秒
    for (int i = 0; i < ringCount; ++i) {
        Beep(2000, 400);   // 2000Hz，0.4秒
        Sleep(200);        // 间隔0.2秒
    }
}


// 获取当前时间，格式 HH:MM:SS
std::string GetNowTime() {
    time_t now = time(nullptr);
    tm tm_now;
    localtime_s(&tm_now, &now);
    std::ostringstream ss;
    ss << std::put_time(&tm_now, "%H:%M:%S");
    return ss.str();
}
// 获取当前日期，格式 YYYY-MM-DD
std::string GetTodayDate() {
    time_t now = time(nullptr);
    tm tm_now;
    localtime_s(&tm_now, &now);
    std::ostringstream ss;
    ss << std::put_time(&tm_now, "%Y-%m-%d");
    return ss.str();
}
// 写一条学习数据到 study_log.csv
void WriteStudyLog(const std::string& typeText, int durationSec) {
    if (durationSec <= 0) return;

    // 计算当前行数（用于生成ID）
    std::ifstream readLast("study_log.csv");
    int lastID = 0;
    if (readLast.is_open()) {
        std::string line;
        bool firstLine = true;   // 跳过表头
        while (std::getline(readLast, line)) {
            if (firstLine) {
                firstLine = false;
                continue;
            }
            if (!line.empty()) {
                int id = 0;
                sscanf_s(line.c_str(), "%d,", &id);
                lastID = id;
            }
        }
        readLast.close();
    }
    int newID = lastID + 1;

    // 以下原有逻辑
    std::ofstream file("study_log.csv", std::ios::app | std::ios::binary);
    if (!file) return;
    // 判断文件是否为空（即新文件），如果是则写入表头
    file.seekp(0, std::ios::end);
    if (file.tellp() == 0) {
        file << "ID,Date,StartTime,EndTime,Status,Duration(sec)\n";
    }
    // ... 时间计算
    time_t end = time(nullptr);
    tm tm_end;
    localtime_s(&tm_end, &end);
    char dateBuf[20], endTime[10], startTime[10];
    strftime(dateBuf, 20, "%Y-%m-%d", &tm_end);
    strftime(endTime, 10, "%H:%M:%S", &tm_end);
    time_t start = end - durationSec;
    tm tm_start;
    localtime_s(&tm_start, &start);
    strftime(startTime, 10, "%H:%M:%S", &tm_start);

    // 写入时带上ID
    file << newID << "," << dateBuf << "," << startTime << "," << endTime << ","
        << typeText << "," << durationSec << "\n";
    file.close();
}


int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    std::cout << "版本号：MindTether v1.0.0" << std::endl;
    LoadSettings();
    // 1. 初始化 COM 环境
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    // 2. 创建音频设备枚举器（这是通往扬声器的入口）
    
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&pEnumerator
    );
    if (SUCCEEDED(hr)) {
        std::cout << "枚举器创建成功！" << std::endl;
    }
    else {
        std::cout << "创建失败，错误码: " << std::hex << hr << std::endl;
    }

    // 获取默认的声音渲染设备（扬声器/耳机）
    pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    /*
    eRender：表示要找的是“输出/播放”设备（扬声器），不是录音设备。
    eConsole：表示是给普通桌面程序用的设备角色。
    &pDevice：把拿到的设备对象指针写进 pDevice。
    */
    // 从设备激活会话管理器
    hr = pDevice->Activate(
        __uuidof(IAudioSessionManager2),
        CLSCTX_ALL,
        NULL,
        (void**)&pSessionManager
    );
    if (SUCCEEDED(hr)) {
        std::cout << "会话管理器获取成功！" << std::endl;
    }
    else {
        std::cout << "获取会话管理器失败，错误码: " << std::hex << hr << std::endl;
    }

    // 从管理器拿到会话枚举器
    hr = pSessionManager->GetSessionEnumerator(&pSessionEnumerator);
    if (FAILED(hr)) {
        std::cout << "获取会话枚举器失败, hr=" << std::hex << hr << std::endl;
        return -1;
    }

    // 如果启动时 Edge 正在播放，则记录学习开始时间
    if (IsEdgePlaying(pSessionManager)) {
        learningStartTime = time(nullptr);
    }

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: 在此处放置代码。

    // 初始化全局字符串
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_MINDTETHER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // 执行应用程序初始化:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_MINDTETHER));

    MSG msg;

    // 主消息循环:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}



//
//  函数: MyRegisterClass()
//
//  目标: 注册窗口类。
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MINDTETHER));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_MINDTETHER);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//   函数: InitInstance(HINSTANCE, int)
//
//   目标: 保存实例句柄并创建主窗口
//
//   注释:
//
//        在此函数中，我们在全局变量中保存实例句柄并
//        创建和显示主程序窗口。
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // 将实例句柄存储在全局变量中

   int winWidth = 800, winHeight = 600;
   HWND hWnd = CreateWindowW(szWindowClass, L"MindTether", WS_OVERLAPPEDWINDOW,
       CW_USEDEFAULT, 0, winWidth, winHeight, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   ShowWindow(hWnd, nCmdShow);
   // 首次检测播放状态并设置窗口标题
   bool playing = IsEdgePlaying(pSessionManager);
   SetWindowTextW(hWnd, L"MindTether");
   UpdateWindow(hWnd);

   return TRUE;
}

// 将 UTF-8 的 std::string 转换为宽字符串 std::wstring
std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    std::wstring wstr(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    if (!wstr.empty() && wstr.back() == L'\0') {
        wstr.pop_back();
    }
    return wstr;
}

//
//  函数: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  目标: 处理主窗口的消息。
//
//  WM_COMMAND  - 处理应用程序菜单
//  WM_PAINT    - 绘制主窗口
//  WM_DESTROY  - 发送退出消息并返回
//
//主窗口函数
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        SetTimer(hWnd, 1, 1000, nullptr);  // 定时器，每秒触发

        // 创建静态文本框，用于显示状态/倒计时
        hStatusText = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            50, 50, 500, 150,   // x, y, 宽, 高
            hWnd, nullptr, hInst, nullptr);

        // 创建四个按钮
        int btnY = 220, btnH = 32, btnW = 120, btnGap = 20;
        int startX = 50;

        hBtnContinue = CreateWindowW(L"BUTTON", L"继续学习",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            startX, btnY, btnW, btnH,
            hWnd, (HMENU)ID_BTN_CONTINUE, hInst, nullptr);

        hBtnRest = CreateWindowW(L"BUTTON", L"再休息 5 分钟",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            startX + (btnW + btnGap), btnY, btnW, btnH,
            hWnd, (HMENU)ID_BTN_REST, hInst, nullptr);

        hBtnSettings = CreateWindowW(L"BUTTON", L"设  置",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            startX + 2 * (btnW + btnGap), btnY, btnW, btnH,
            hWnd, (HMENU)ID_BTN_SETTINGS, hInst, nullptr);

        hBtnExit = CreateWindowW(L"BUTTON", L"退  出",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            startX + 3 * (btnW + btnGap), btnY, btnW, btnH,
            hWnd, (HMENU)ID_BTN_EXIT, hInst, nullptr);


        // 初始状态：隐藏“继续学习”和“再休息”，只留“设置”和“退出”
        ShowWindow(hBtnContinue, SW_HIDE);
        ShowWindow(hBtnRest, SW_HIDE);
        
        // 设置更醒目的字体（微软雅黑，32pt，加粗）
        HFONT hFont = CreateFontW(32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_DONTCARE, L"Microsoft YaHei");
        if (hFont) {
            SendMessage(hStatusText, WM_SETFONT, (WPARAM)hFont, TRUE);
        }
    }
    break;
    case WM_TIMER:
        if (wParam == 1) {          //定时器ID
            bool playing = false;

            // 每 4 次（每 4 秒）真正检测一次，其他秒用缓存
            if (updateCounter % 4 == 0) {
                playing = IsEdgePlaying(pSessionManager);
                lastPlayingState = playing;
            }
            else {
                playing = lastPlayingState;
            }
            updateCounter++;

            if (playing) {
                // ---- 状态切换：如果之前是暂停，现在恢复播放 ----
                if (isPausing) {
                    time_t now = time(nullptr);
                    int pauseSec = (int)difftime(now, pauseStartTimeStamp);
                    if (pauseSec > 0) {
                        if (pauseSec <= THINK_THRESHOLD_SEC)
                            WriteStudyLog("Think", pauseSec);
                        else
                            WriteStudyLog("Rest", pauseSec);
                    }
                    isPausing = false;
                    learningStartTime = now;          // 开始新的学习段
                }

                // ---- 原有 UI 重置 ----
                restSeconds = 0;
                reminded = false;
                SetWindowTextW(hStatusText, L"当前状态：正在播放");
                ShowWindow(hBtnContinue, SW_HIDE);
                ShowWindow(hBtnRest, SW_HIDE);
            }
            else {
                // ---- 状态切换：如果之前是播放，现在暂停 ----
                if (!isPausing) {
                    time_t now = time(nullptr);
                    // 记录刚结束的学习段
                    if (learningStartTime > 0) {
                        int studySec = (int)difftime(now, learningStartTime);
                        if (studySec > 0)
                            WriteStudyLog("Study", studySec);
                        learningStartTime = 0;
                    }
                    // 开始暂停计时
                    pauseStartTimeStamp = now;
                    isPausing = true;

                    // 开始休息倒计时（原有逻辑）
                    if (restSeconds == 0 && !reminded) {
                        restSeconds = remindInterval;
                    }
                }

                // ---- 倒计时与提醒逻辑（原有，但注意判断顺序）----
                if (restSeconds > 0) {
                    restSeconds--;
                }

                if (restSeconds > 0) {
                    int mins = restSeconds / 60;
                    int secs = restSeconds % 60;
                    wchar_t buf[100];
                    swprintf_s(buf, 100, L"休息剩余时间：%02d:%02d", mins, secs);
                    SetWindowTextW(hStatusText, buf);
                }
                else if (restSeconds == 0 && !reminded) {
                    SetWindowTextW(hStatusText, L"该继续学习了！");
                }

                if (restSeconds == 0 && !reminded) {
                    Remind();
                    SetWindowTextW(hStatusText, L"该回去学习了！");
                    ShowWindow(hBtnContinue, SW_SHOW);
                    ShowWindow(hBtnRest, SW_SHOW);
                    reminded = true;
                }
            }
        }
        break;
    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        // 处理按钮点击
        switch (wmId)
        {
        case ID_BTN_CONTINUE:
            // 继续学习：重置休息计时，进入监控状态
            restSeconds = 0;
            reminded = false;
            SetWindowTextW(hStatusText, L"当前状态：正在播放");
            ShowWindow(hBtnContinue, SW_HIDE);
            ShowWindow(hBtnRest, SW_HIDE);
            break;

        case ID_BTN_REST:
            // 再休息 5 分钟 = 300 秒
            restSeconds = 300;
            reminded = false;
            // 立即刷新显示
            {
                int mins = restSeconds / 60;
                int secs = restSeconds % 60;
                wchar_t buf[100];
                swprintf_s(buf, 100, L"休息剩余时间：%02d:%02d", mins, secs);
                SetWindowTextW(hStatusText, buf);
            }
            ShowWindow(hBtnContinue, SW_HIDE);
            ShowWindow(hBtnRest, SW_HIDE);
            break;

        case ID_BTN_SETTINGS:
            if (DialogBox(hInst, MAKEINTRESOURCE(IDD_SETTINGS_DIALOG), hWnd, SettingsProc) == IDOK) {
                // 如果当前正在休息，用新的休息时长重新开始倒计时
                if (restSeconds > 0) {
                    restSeconds = remindInterval;
                    int mins = restSeconds / 60;
                    int secs = restSeconds % 60;
                    wchar_t buf[100];
                    swprintf_s(buf, 100, L"休息剩余时间：%02d:%02d", mins, secs);
                    SetWindowTextW(hStatusText, buf);
                }
            }
            break;

        case ID_BTN_EXIT:
            DestroyWindow(hWnd);
            break;

            // 原有的菜单处理
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            // TODO: 在此处添加使用 hdc 的任何绘图代码...
            EndPaint(hWnd, &ps);
        }
        break;
    case WM_DESTROY:            //释放资源
        KillTimer(hWnd, 1);

        if (learningStartTime > 0) {
            time_t now = time(nullptr);
            int studySec = (int)difftime(now, learningStartTime);
            if (studySec > 0) WriteStudyLog("学习", studySec);
            learningStartTime = 0;
        }

        if (pSessionEnumerator) { pSessionEnumerator->Release(); pSessionEnumerator = nullptr; }
        if (pSessionManager) { pSessionManager->Release(); pSessionManager = nullptr; }
        if (pDevice) { pDevice->Release(); pDevice = nullptr; }
        if (pEnumerator) { pEnumerator->Release(); pEnumerator = nullptr; }
        CoUninitialize();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// “关于”框的消息处理程序。
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}


// 设置对话框的消息处理程序
INT_PTR CALLBACK SettingsProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
    {
        wchar_t buf[32];
        swprintf_s(buf, 32, L"%.1f", defaultRestMinutes);
        SetDlgItemTextW(hDlg, IDC_EDIT_RESTTIME, buf);
        swprintf_s(buf, 32, L"%d", ringCount);
        SetDlgItemTextW(hDlg, IDC_EDIT_RINGCOUNT, buf);
    }
    return (INT_PTR)TRUE;

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        if (wmId == IDOK) {
            wchar_t buf[32];
            GetDlgItemTextW(hDlg, IDC_EDIT_RESTTIME, buf, 32);
            double min = _wtof(buf);
            if (min <= 0) min = 10.0;

            GetDlgItemTextW(hDlg, IDC_EDIT_RINGCOUNT, buf, 32);
            int ring = _wtoi(buf);
            if (ring <= 0) ring = 6;

            defaultRestMinutes = min;
            remindInterval = static_cast<int>(min * 60);
            ringCount = ring;

            SaveSettings();
            EndDialog(hDlg, IDOK);
            return (INT_PTR)TRUE;
        }
        else if (wmId == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return (INT_PTR)TRUE;
        }
    }
    break;
    }
    return (INT_PTR)FALSE;
}