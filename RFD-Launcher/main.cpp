#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <windows.h>
#include <commdlg.h>
#include "imgui.h"

extern ImTextureID LoadTexture(const std::string& path);

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

ImTextureID texLogo = nullptr;
ImTextureID texUser = nullptr;
ImTextureID texAdd = nullptr;
ImTextureID texArrowDown = nullptr;
ImTextureID texHost = nullptr;
ImTextureID texJoin = nullptr;
ImTextureID texLog = nullptr;
ImTextureID texSettings = nullptr;

std::string TrimString(const std::string& str) {
    size_t first = str.find_first_not_of(' ');
    if (std::string::npos == first) {
        return str;
    }
    size_t last = str.find_last_not_of(' ');
    return str.substr(first, (last - first + 1));
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
        }
    }

    DWORD exitCode;
    if (GetExitCodeProcess(piProcInfo.hProcess, &exitCode)) {
        if (exitCode != STILL_ACTIVE) {
            isHostRunning = false;
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

    SECURITY_ATTRIBUTES saAttr; 
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES); 
    saAttr.bInheritHandle = TRUE; 
    saAttr.lpSecurityDescriptor = NULL; 

    CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0);
    SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = appCfg.executable_name + " --config \"" + hostConfigPath + "\" -p " + currentPort;
    if (outputOption == 1) {
        cmd += " --loud";
    } else if (outputOption == 2) {
        cmd += " -q";
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

    BOOL bSuccess = CreateProcessA(NULL, cmdStr.data(), NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo);
    
    if (bSuccess) {
        isHostRunning = true;
        logBuffer.clear();
    }
    
    CloseHandle(hChildStd_OUT_Wr);
}

void StartJoinProcess(const std::string& targetIp, const std::string& targetPort) {
    std::string cmd = appCfg.executable_name + " -h " + targetIp + " -p " + targetPort + " -u " + appCfg.selected_username;
    
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
        if (ImGui::ImageButton(texAdd, ImVec2(20, 20))) {
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
        if (ImGui::ImageButton(texArrowDown, ImVec2(20, 20))) {
            ImGui::OpenPopup("UserSelectPopup");
        }
        ImGui::SameLine();
        if (ImGui::ImageButton(texAdd, ImVec2(20, 20))) {
            showAddUserPopup = true;
        }
        
        if (ImGui::BeginPopup("UserSelectPopup")) {
            for (const auto& uname : appCfg.usernames) {
                if (ImGui::Selectable(uname.c_str())) {
                    appCfg.selected_username = uname;
                    SaveAppConfig();
                }
            }
            ImGui::EndPopup();
        }
    }

    if (showAddUserPopup) {
        ImGui::OpenPopup("Kullanici Ekle");
        showAddUserPopup = false;
    }

    if (ImGui::BeginPopupModal("Kullanici Ekle", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Kullanici adi belirleyin");
        ImGui::InputText("##newuname", newUsernameBuf, IM_ARRAYSIZE(newUsernameBuf));
        if (ImGui::Button("Onayla", ImVec2(120, 0))) {
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
}

void RenderTabButtons() {
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
}

void RenderHostTab() {
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
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Please select your config file");
    }

    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 400, ImGui::GetWindowHeight() - 200));
    if (ImGui::Button("Join", ImVec2(350, 40))) {
        if (!appCfg.selected_username.empty()) {
            StartJoinProcess("127.0.0.1", currentPort);
        }
    }
}

void RenderJoinTab() {
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
    ImGui::SetWindowFontScale(2.5f);
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Join");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    ImGui::TextColored(ImVec4(0.38f, 0.38f, 0.38f, 1.0f), "Join a hosted server");
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 400, ImGui::GetWindowHeight() - 150));
    ImGui::Text("IP Adresi:");
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
}

void RenderLogsTab() {
    if (isHostRunning) {
        ImGui::BeginChild("LogRegion", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::TextUnformatted(logBuffer.c_str());
        ImGui::EndChild();
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "RFD is not currently running.");
    }
}

void RenderSettingsTab() {
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Settings is w.i.p.");
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
