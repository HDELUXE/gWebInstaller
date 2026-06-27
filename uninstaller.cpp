// ============================================================
//  AngerMU Uninstaller — Simples, apenas diálogo de confirmação
//  Embutido como recurso no installer
// ============================================================

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstdio>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

// ── CONFIG ─────────────────────────────────────────────────
#define GAME_NAME        "AngerMU"
#define INSTALL_SUBDIR   "AngerMU"
#define REGISTRY_KEY     "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" GAME_NAME

// ── Paths ──────────────────────────────────────────────────
static std::string GetInstallDir() {
    char pf[MAX_PATH]{};
    if (!GetEnvironmentVariableA("ProgramFiles(x86)", pf, MAX_PATH) || !pf[0])
        GetEnvironmentVariableA("ProgramFiles", pf, MAX_PATH);
    return std::string(pf) + "\\" INSTALL_SUBDIR;
}

// ── Limpeza de registro ────────────────────────────────────
static bool RemoveRegistryKey(HKEY root, const char* keyPath) {
    HKEY hKey = nullptr;
    LONG res = RegOpenKeyExA(root, keyPath, 0, KEY_WRITE, &hKey);
    if (res != ERROR_SUCCESS) return false;
    
    // Enumera e deleta todas as subchaves recursivamente
    char subkey[256];
    while (RegEnumKeyA(hKey, 0, subkey, sizeof(subkey)) == ERROR_SUCCESS) {
        RemoveRegistryKey(hKey, subkey);
    }
    
    RegCloseKey(hKey);
    return RegDeleteKeyA(root, keyPath) == ERROR_SUCCESS;
}

// ── Thread de desinstalação ────────────────────────────────
static void UninstallThread(const std::string& installDir, HWND hwnd) {
    // Aguarda 500ms para o diálogo fechar
    Sleep(500);
    
    // 1. Remove a chave de registro AGORA (enquanto .exe ainda existe)
    std::string keyPath = REGISTRY_KEY;
    RemoveRegistryKey(HKEY_LOCAL_MACHINE, keyPath.c_str());
    RemoveRegistryKey(HKEY_CURRENT_USER, keyPath.c_str());
    
    // 2. Cria um batch temporário que deleta tudo depois que o uninstaller fecha
    char tmpPath[MAX_PATH];
    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);
    GetTempFileNameA(tmpDir, "unins", 0, tmpPath);
    
    // Sobrescreve com nome .bat
    std::string batchPath = std::string(tmpPath) + ".bat";
    
    // Pega o path do próprio exe
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    
    // Cria o script bat: espera um pouco, deleta a pasta, deleta a si mesmo
    std::string batchContent = 
        "@echo off\n"
        "timeout /t 2 /nobreak >nul\n"
        "rmdir /S /Q \"" + installDir + "\" 2>nul\n"
        "del /F /Q \"" + std::string(exePath) + "\" 2>nul\n"
        "del /F /Q \"%~f0\" 2>nul\n";
    
    try {
        std::ofstream batchFile(batchPath, std::ios::binary);
        if (batchFile) {
            batchFile.write(batchContent.c_str(), batchContent.size());
            batchFile.close();
            
            // Executa o batch em background
            STARTUPINFOA si{};
            PROCESS_INFORMATION pi{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            
            if (CreateProcessA(nullptr, (LPSTR)("cmd.exe /C \"" + batchPath + "\"").c_str(),
                             nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | DETACHED_PROCESS,
                             nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }
    } catch (...) {
        // Silent fail
    }
}

// ── Message Box com suporte a UTF-8 ──────────────────────
static void ShowMessageBoxUTF8(const std::string& text, const std::string& title, UINT flags) {
    int titleLen = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    int textLen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    
    std::wstring wtitle(titleLen, 0);
    std::wstring wtext(textLen, 0);
    
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wtitle.data(), titleLen);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wtext.data(), textLen);
    
    MessageBoxW(nullptr, wtext.c_str(), wtitle.c_str(), flags);
}

// ── Message Box de confirmação ────────────────────────────
static INT_PTR CALLBACK ConfirmDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static std::string* pInstallDir = nullptr;
    
    switch (msg) {
    case WM_INITDIALOG:
        pInstallDir = (std::string*)lp;
        {
            std::string text = "Tem certeza que deseja desinstalar o " GAME_NAME "?\n\n";
            text += "Pasta de instalação:\n";
            text += *pInstallDir + "\n\n";
            text += "Esta ação não pode ser desfeita.";
            
            int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            std::wstring wtext(len, 0);
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wtext.data(), len);
            
            SetDlgItemTextW(hwnd, 1001, wtext.c_str());
        }
        return TRUE;
    
    case WM_COMMAND:
        if (LOWORD(wp) == 1) {  // Botão Desinstalar
            EndDialog(hwnd, IDYES);
        } else if (LOWORD(wp) == 2) {  // Botão Cancelar
            EndDialog(hwnd, IDNO);
        }
        return TRUE;
    
    case WM_CLOSE:
        EndDialog(hwnd, IDNO);
        return TRUE;
    }
    return FALSE;
}

// ── Cria diálogo simples inline ────────────────────────────
static int ShowConfirmDialog(const std::string& installDir) {
    std::string msg = "Tem certeza que deseja desinstalar o " GAME_NAME "?\n\n";
    msg += "Pasta de instalação:\n";
    msg += installDir + "\n\n";
    msg += "Esta ação não pode ser desfeita.";
    
    int titleLen = MultiByteToWideChar(CP_UTF8, 0, GAME_NAME " Uninstaller", -1, nullptr, 0);
    int msgLen = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0);
    
    std::wstring wtitle(titleLen, 0);
    std::wstring wmsg(msgLen, 0);
    
    MultiByteToWideChar(CP_UTF8, 0, GAME_NAME " Uninstaller", -1, wtitle.data(), titleLen);
    MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, wmsg.data(), msgLen);
    
    return MessageBoxW(nullptr, wmsg.c_str(), wtitle.c_str(), MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    std::string installDir = GetInstallDir();
    
    // Se passou /silent como argumento, desinstala sem confirmar
    bool silent = (lpCmdLine && std::string(lpCmdLine).find("/silent") != std::string::npos);
    
    if (!silent) {
        int result = ShowConfirmDialog(installDir);
        if (result != IDYES) {
            CoUninitialize();
            return 0;  // Usuário cancelou
        }
    }
    
    // Executa a desinstalação em uma thread separada
    std::thread t(UninstallThread, installDir, nullptr);
    
    if (silent) {
        t.join();
    } else {
        // Em modo normal, mostra uma mensagem de conclusão
        Sleep(500);
        
        std::string msg = GAME_NAME " será completamente removido em poucos segundos.";
        int msgLen = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0);
        std::wstring wmsg(msgLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, wmsg.data(), msgLen);
        
        int titleLen = MultiByteToWideChar(CP_UTF8, 0, GAME_NAME, -1, nullptr, 0);
        std::wstring wtitle(titleLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, GAME_NAME, -1, wtitle.data(), titleLen);
        
        MessageBoxW(nullptr, wmsg.c_str(), wtitle.c_str(), MB_ICONINFORMATION | MB_OK | MB_TOPMOST);
        
        t.join();
    }
    
    CoUninitialize();
    return 0;
}