#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <wincodec.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static std::ofstream g_logFile;
static std::string g_currentLogPath = "";

ImTextureID LoadTexture(const std::string& path) {
    if (!g_pd3dDevice) return (ImTextureID)0;

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.length(), NULL, 0);
    std::wstring wpath(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.length(), &wpath[0], size_needed);

    IWICImagingFactory* pFactory = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return (ImTextureID)0;

    IWICBitmapDecoder* pDecoder = NULL;
    hr = pFactory->CreateDecoderFromFilename(wpath.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) { pFactory->Release(); return (ImTextureID)0; }

    IWICBitmapFrameDecode* pFrame = NULL;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pDecoder->Release(); pFactory->Release(); return (ImTextureID)0; }

    IWICFormatConverter* pConverter = NULL;
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) { pFrame->Release(); pDecoder->Release(); return (ImTextureID)0; }

    hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { pConverter->Release(); pFrame->Release(); return (ImTextureID)0; }

    UINT width = 0, height = 0;
    pConverter->GetSize(&width, &height);

    std::vector<BYTE> buffer(width * height * 4);
    hr = pConverter->CopyPixels(NULL, width * 4, (UINT)buffer.size(), buffer.data());

    pConverter->Release();
    pFrame->Release();
    pDecoder->Release();
    pFactory->Release();

    if (FAILED(hr)) return (ImTextureID)0;

    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData;
    ZeroMemory(&initData, sizeof(initData));
    initData.pSysMem = buffer.data();
    initData.SysMemPitch = width * 4;

    ID3D11Texture2D* pTex = NULL;
    hr = g_pd3dDevice->CreateTexture2D(&desc, &initData, &pTex);
    if (FAILED(hr)) return (ImTextureID)0;

    ID3D11ShaderResourceView* pSRV = NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = g_pd3dDevice->CreateShaderResourceView(pTex, &srvDesc, &pSRV);
    pTex->Release();

    if (FAILED(hr)) return (ImTextureID)0;

    return (ImTextureID)pSRV;
}

struct AppConfig {
    std::string selected_username = "";
    std::vector<std::string> usernames;
    std::string executable_name = "RFD.exe";
    std::string default_port = "2005";
};

AppConfig appCfg;
std::string configFilePath = "config.cfg";

std::string hostConfigPath = "";
int outputOption = 0; 
std::string currentIp = "";
std::string currentPort = "2005";
bool isHostRunning = false;
bool showNoConfigWarning = false;
bool showAddUserPopup = false;
char newUsernameBuf[256] = "";

int currentTab = 0;

HANDLE hChildStd_OUT_Rd = NULL;
HANDLE hChildStd_OUT_Wr = NULL;
PROCESS_INFORMATION piProcInfo;
std::string logBuffer;

ImTextureID texLogo = (ImTextureID)0;
ImTextureID texUser = (ImTextureID)0;
ImTextureID texAdd = (ImTextureID)0;
ImTextureID texArrowDown = (ImTextureID)0;
ImTextureID texHost = (ImTextureID)0;
ImTextureID texJoin = (ImTextureID)0;
ImTextureID texLog = (ImTextureID)0;
ImTextureID texSettings = (ImTextureID)0;

std::string TrimString(const std::string& str) {
    size_t first = str.find_first_not_of(' ');
    if (std::string::npos == first) {
        return str;
    }
    size_t last = str.find_last_not_of(' ');
    return str.substr(first, (last - first + 1));
}

std::string GetTimestampFilename() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &in_time_t);

    std::ostringstream ss;
    ss << "logs/log_" << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S") << ".txt";
    return ss.str();
}

void LoadAppConfig() {
    std::ifstream file(configFilePath);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            size_t delimPos = line.find(':');
            if (delimPos != std::string::npos) {
                std::string key = TrimString(line.substr(0, delimPos));
                std::string val = TrimString(line.substr(delimPos + 1));
                
                if (key == "selected_username") {
                    appCfg.selected_username = val;
                } else if (key == "executable_name") {
                    appCfg.executable_name = val;
                } else if (key == "default_port") {
                    appCfg.default_port = val;
                    currentPort = val;
                } else if (key == "usernames") {
                    appCfg.usernames.clear();
                    std::stringstream ss(val);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        appCfg.usernames.push_back(TrimString(token));
                    }
                }
            }
        }
        file.close();
    }
}

void SaveAppConfig() {
    std::ofstream file(configFilePath);
    if (file.is_open()) {
        file << "selected_username: " << appCfg.selected_username << "\n";
        file << "usernames: ";
        for (size_t i = 0; i < appCfg.usernames.size(); ++i) {
            file << appCfg.usernames[i];
            if (i < appCfg.usernames.size() - 1) file << ", ";
        }
        file << "\n";
        file << "executable_name: " << appCfg.executable_name << "\n";
        file << "default_port: " << appCfg.default_port << "\n";
        file.close();
    }
}

void FindRFDExecutable() {
    for (const auto& entry : std::filesystem::directory_iterator(".")) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            std::string lowerFilename = filename;
            std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);
            if (lowerFilename == "rfd.exe") {
                appCfg.executable_name = filename;
                SaveAppConfig();
                break;
            }
        }
    }
}

void LoadAllTextures() {
    texLogo = LoadTexture("./launcherassets/ui/logo.png");
    texUser = LoadTexture("./launcherassets/ui/user.png");
    texAdd = LoadTexture("./launcherassets/ui/add.png");
    texArrowDown = LoadTexture("./launcherassets/ui/arrow_down.png");
    texHost = LoadTexture("./launcherassets/ui/host.png");
    texJoin = LoadTexture("./launcherassets/ui/join.png");
    texLog = LoadTexture("./launcherassets/ui/log.png");
    texSettings = LoadTexture("./launcherassets/ui/settings.png");
}

std::string OpenConfigDialog() {
    OPENFILENAMEA ofn;
    char szFile[260] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameA(&ofn) == TRUE) {
        std::string path(szFile);
        std::string lowerPath = path;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        if (lowerPath.length() >= 5 && lowerPath.substr(lowerPath.length() - 5) == ".rbxl") return "";
        if (lowerPath.length() >= 6 && lowerPath.substr(lowerPath.length() - 6) == ".rbxlx") return "";
        return path;
    }
    return "";
}

void ReadRFDLogs() {
    if (!isHostRunning || !hChildStd_OUT_Rd) return;
    
    DWORD dwRead;
    CHAR chBuf[4096];
    BOOL bSuccess = FALSE;
    
    DWORD bytesAvailable = 0;
    PeekNamedPipe(hChildStd_OUT_Rd, NULL, 0, NULL, &bytesAvailable, NULL);
    
    if (bytesAvailable > 0) {
        bSuccess = ReadFile(hChildStd_OUT_Rd, chBuf, sizeof(chBuf) - 1, &dwRead, NULL);
        if (bSuccess && dwRead > 0) {
            chBuf[dwRead] = '\0';
            logBuffer += chBuf;

            if (g_logFile.is_open()) {
                g_logFile << chBuf;
                g_logFile.flush();
            }
        }
    }

    DWORD exitCode;
    if (GetExitCodeProcess(piProcInfo.hProcess, &exitCode)) {
        if (exitCode != STILL_ACTIVE) {
            isHostRunning = false;

            if (g_logFile.is_open()) {
                g_logFile << "\n[LAUNCHER] Process exited with code: " << exitCode << "\n";
                g_logFile.close();
            }

            CloseHandle(piProcInfo.hProcess);
            CloseHandle(piProcInfo.hThread);
            CloseHandle(hChildStd_OUT_Rd);
            hChildStd_OUT_Rd = NULL;
        }
    }
}

void StopRFDProcess() {
    if (isHostRunning) {
        std::string killCmd = "taskkill /F /IM " + appCfg.executable_name + " /T";
        system(killCmd.c_str());
        isHostRunning = false;
        
        if (g_logFile.is_open()) {
            g_logFile << "\n[LAUNCHER] Process stopped manually by user.\n";
            g_logFile.close();
        }

        if (piProcInfo.hProcess) {
            CloseHandle(piProcInfo.hProcess);
            CloseHandle(piProcInfo.hThread);
        }
        if (hChildStd_OUT_Rd) {
            CloseHandle(hChildStd_OUT_Rd);
            hChildStd_OUT_Rd = NULL;
        }
    }
}

void StartRFDProcess() {
    if (isHostRunning) return;

    SetEnvironmentVariableA("PYTHONIOENCODING", "utf-8");
    SetEnvironmentVariableA("PYTHONUTF8", "1");

    std::filesystem::create_directories("logs");

    g_currentLogPath = GetTimestampFilename();
    g_logFile.open(g_currentLogPath, std::ios::out | std::ios::trunc);

    SECURITY_ATTRIBUTES saAttr; 
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES); 
    saAttr.bInheritHandle = TRUE; 
    saAttr.lpSecurityDescriptor = NULL; 

    CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0);
    SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = appCfg.executable_name + " server --config \"" + hostConfigPath + "\" -p " + currentPort;
    if (outputOption == 1) {
        cmd += " --loud";
    } else if (outputOption == 2) {
        cmd += " -q";
    }

    if (g_logFile.is_open()) {
        g_logFile << "[LAUNCHER] Executing: " << cmd << "\n\n";
        g_logFile.flush();
    }

    STARTUPINFOA siStartInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFOA));
    siStartInfo.cb = sizeof(STARTUPINFOA); 
    siStartInfo.hStdError = hChildStd_OUT_Wr;
    siStartInfo.hStdOutput = hChildStd_OUT_Wr;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    std::vector<char> cmdStr(cmd.begin(), cmd.end());
    cmdStr.push_back('\0');

    BOOL bSuccess = CreateProcessA(NULL, cmdStr.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &siStartInfo, &piProcInfo);
    
    if (bSuccess) {
        isHostRunning = true;
        logBuffer.clear();
    } else {
        if (g_logFile.is_open()) {
            g_logFile << "[LAUNCHER ERROR] CreateProcessA failed with Error Code: " << GetLastError() << "\n";
            g_logFile.close();
        }
    }
    
    CloseHandle(hChildStd_OUT_Wr);
}

void StartJoinProcess(const std::string& targetIp, const std::string& targetPort) {
    SetEnvironmentVariableA("PYTHONIOENCODING", "utf-8");
    SetEnvironmentVariableA("PYTHONUTF8", "1");

    std::string cmd = appCfg.executable_name + " player -h " + targetIp + " -p " + targetPort + " -u " + appCfg.selected_username;
    
    STARTUPINFOA siStartInfo;
    PROCESS_INFORMATION piClientInfo;
    ZeroMemory(&piClientInfo, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFOA));
    siStartInfo.cb = sizeof(STARTUPINFOA);

    std::vector<char> cmdStr(cmd.begin(), cmd.end());
    cmdStr.push_back('\0');

    CreateProcessA(NULL, cmdStr.data(), NULL, NULL, FALSE, 0, NULL, NULL, &siStartInfo, &piClientInfo);
    
    if (piClientInfo.hProcess) {
        CloseHandle(piClientInfo.hProcess);
        CloseHandle(piClientInfo.hThread);
    }
}

void RenderTopBar() {
    ImGui::PushID("TopBarScope");

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.168f, 0.168f, 0.168f, 1.0f)); 
    ImGui::BeginChild("TopBar", ImVec2(0, 60), false);
    
    ImGui::SetCursorPos(ImVec2(10, 10));
    if (texLogo) ImGui::Image(texLogo, ImVec2(40, 40));

    ImGui::SameLine(ImGui::GetWindowWidth() - 250);
    
    if (texUser) ImGui::Image(texUser, ImVec2(30, 30));
    ImGui::SameLine();
    
    if (appCfg.usernames.empty()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
        ImGui::Text("Hesap ekleyin");
        ImGui::SameLine();
        if (ImGui::ImageButton("##AddUserBtnEmpty", texAdd, ImVec2(20, 20))) {
            showAddUserPopup = true;
        }
    } else {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
        if (appCfg.selected_username.empty()) {
            ImGui::Text("Hesap Secin");
        } else {
            ImGui::Text("%s", appCfg.selected_username.c_str());
        }
        ImGui::SameLine();
        if (ImGui::ImageButton("##SelectUserBtn", texArrowDown, ImVec2(20, 20))) {
            ImGui::OpenPopup("UserSelectPopup");
        }
        ImGui::SameLine();
        if (ImGui::ImageButton("##AddUserBtnPopulated", texAdd, ImVec2(20, 20))) {
            showAddUserPopup = true;
        }
        
        if (ImGui::BeginPopup("UserSelectPopup")) {
            int userIdx = 0;
            for (const auto& uname : appCfg.usernames) {
                ImGui::PushID(userIdx++);
                if (ImGui::Selectable(uname.c_str())) {
                    appCfg.selected_username = uname;
                    SaveAppConfig();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }

    if (showAddUserPopup) {
        ImGui::OpenPopup("Add User");
        showAddUserPopup = false;
    }

    if (ImGui::BeginPopupModal("Add User", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Select a username");
        ImGui::InputText("##newuname", newUsernameBuf, IM_ARRAYSIZE(newUsernameBuf));
        if (ImGui::Button("Accept", ImVec2(120, 0))) {
            std::string newUname = std::string(newUsernameBuf);
            if (!newUname.empty()) {
                appCfg.usernames.push_back(newUname);
                appCfg.selected_username = newUname;
                SaveAppConfig();
            }
            memset(newUsernameBuf, 0, sizeof(newUsernameBuf));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            memset(newUsernameBuf, 0, sizeof(newUsernameBuf));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::PopID();
}

void RenderTabButtons() {
    ImGui::PushID("TabButtonsScope");

    ImVec2 btnSize(100, 40);
    ImVec4 activeCol = ImVec4(0.121f, 0.121f, 0.121f, 1.0f); 
    ImVec4 inactiveCol = ImVec4(0.168f, 0.168f, 0.168f, 1.0f); 

    auto DrawTab = [&](int index, ImTextureID tex, const char* label) {
        if (currentTab == index) {
            ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, inactiveCol);
        }

        bool clicked = ImGui::Button(label, btnSize);

        ImGui::PopStyleColor(currentTab == index ? 3 : 1);
        if (clicked) currentTab = index;
        ImGui::SameLine();
    };

    DrawTab(0, texHost, "Host");
    DrawTab(1, texJoin, "Join");
    DrawTab(2, texLog, "Logs");
    DrawTab(3, texSettings, "Settings");
    ImGui::NewLine();

    ImGui::PopID();
}

void RenderHostTab() {
    ImGui::PushID("HostTabScope");

    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
    ImGui::SetWindowFontScale(2.5f);
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Host");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    
    ImGui::TextColored(ImVec4(0.38f, 0.38f, 0.38f, 1.0f), "Host a LAN party, to play with friends, port forward or use a tunneling service");
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::Text("Port:");
    char portBuf[16];
    strcpy_s(portBuf, currentPort.c_str());
    if (ImGui::InputText("##hostport", portBuf, IM_ARRAYSIZE(portBuf))) {
        currentPort = portBuf;
    }

    ImGui::Text("Output Option:");
    const char* items[] = { "Normal", "Verbose", "Quiet" };
    ImGui::Combo("##outputopt", &outputOption, items, IM_ARRAYSIZE(items));

    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 400, ImGui::GetWindowHeight() - 150));
    
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::BeginChild("ConfigPicker", ImVec2(350, 40), true);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), hostConfigPath.empty() ? "Select a config file..." : hostConfigPath.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::Button("Select", ImVec2(70, 25))) {
        std::string sel = OpenConfigDialog();
        if (!sel.empty()) hostConfigPath = sel;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 400, ImGui::GetWindowHeight() - 100));
    
    if (hostConfigPath.empty()) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    }
    
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    
    if (ImGui::Button(isHostRunning ? "Stop server" : "Host", ImVec2(350, 40))) {
        if (hostConfigPath.empty()) {
            showNoConfigWarning = true;
        } else {
            showNoConfigWarning = false;
            if (isHostRunning) {
                StopRFDProcess();
            } else {
                StartRFDProcess();
            }
        }
    }
    
    ImGui::PopStyleColor(2);
    
    if (hostConfigPath.empty()) {
        ImGui::PopStyleVar();
    }

    if (showNoConfigWarning && hostConfigPath.empty()) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 400, ImGui::GetWindowHeight() - 55));
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Lutfen config dosyanizi secin");
    }

    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 400, ImGui::GetWindowHeight() - 200));
    if (ImGui::Button("Join", ImVec2(350, 40))) {
        if (!appCfg.selected_username.empty()) {
            StartJoinProcess("127.0.0.1", currentPort);
        }
    }

    ImGui::PopID();
}

void RenderJoinTab() {
    ImGui::PushID("JoinTabScope");

    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
    ImGui::SetWindowFontScale(2.5f);
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Join");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    ImGui::TextColored(ImVec4(0.38f, 0.38f, 0.38f, 1.0f), "Join a hosted server");
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 400, ImGui::GetWindowHeight() - 150));
    ImGui::Text("IP Adress:");
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 400);
    char ipBuf[128];
    strcpy_s(ipBuf, currentIp.c_str());
    if (ImGui::InputText("##joinip", ipBuf, IM_ARRAYSIZE(ipBuf))) {
        currentIp = ipBuf;
    }

    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 400);
    ImGui::Text("Port:");
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 400);
    char jportBuf[16];
    strcpy_s(jportBuf, currentPort.c_str());
    if (ImGui::InputText("##joinport", jportBuf, IM_ARRAYSIZE(jportBuf))) {
        currentPort = jportBuf;
    }

    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 120, ImGui::GetWindowHeight() - 100));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    if (ImGui::Button("Join", ImVec2(100, 40))) {
        if (!appCfg.selected_username.empty()) {
            StartJoinProcess(currentIp, currentPort);
        }
    }
    ImGui::PopStyleColor(2);

    ImGui::PopID();
}

void RenderLogsTab() {
    if (isHostRunning || !logBuffer.empty()) {
        ImGui::BeginChild("LogRegion", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::TextUnformatted(logBuffer.c_str());
        ImGui::EndChild();
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "RFD is not currently running.");
    }
}

void RenderSettingsTab() {
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "settings is w.i.p.");
}

void InitializeApp() {
    LoadAppConfig();
    FindRFDExecutable();
    LoadAllTextures();
    currentPort = appCfg.default_port;
}

void MainLoopUpdate() {
    ReadRFDLogs();

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.121f, 0.121f, 0.121f, 1.0f)); 
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("MainWindow", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);

    RenderTopBar();
    RenderTabButtons();

    switch (currentTab) {
        case 0: RenderHostTab(); break;
        case 1: RenderJoinTab(); break;
        case 2: RenderLogsTab(); break;
        case 3: RenderSettingsTab(); break;
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcA(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSEXA wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "RFDLauncherClass", NULL };
    ::RegisterClassExA(&wc);
    HWND hwnd = ::CreateWindowA(wc.lpszClassName, "RFD Launcher", WS_OVERLAPPEDWINDOW, 100, 100, 1024, 720, NULL, NULL, wc.hInstance, NULL);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassA(wc.lpszClassName, wc.hInstance);
        CoUninitialize();
        return 1;
    }

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    InitializeApp();

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        MainLoopUpdate();

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.121f, 0.121f, 0.121f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassA(wc.lpszClassName, wc.hInstance);

    CoUninitialize();

    return 0;
}
