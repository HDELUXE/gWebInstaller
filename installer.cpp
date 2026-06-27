// ============================================================
//  AngerMU Installer — Win32 + GDI+, sem dependências externas
//  Assets embutidos no .exe via RCDATA
//  Extração via miniz (header-only, single file)
// ============================================================

// ── CONFIG (único lugar para editar) ────────────────────────
#define GAME_NAME        "AngerMU"
#define INSTALL_SUBDIR   "AngerMU"   // dentro de Program Files (x86)

// Janela = WINDOW_PCT% da largura da tela.
// Altura e escala derivadas automaticamente do tamanho do background em runtime.
#define WINDOW_PCT       40

// Posições dos elementos — em pixels no espaço da imagem de background original.
// S(v) converte para pixels reais em runtime.

// Botão Fechar (2 frames: normal|hover-click — tamanho lido do bitmap)
#define BTN_CLOSE_X      2660
#define BTN_CLOSE_Y      1473

// Botão Minimizar (tamanho lido do bitmap)
#define BTN_MIN_X        2546
#define BTN_MIN_Y        1473

// Barra de progresso — posição X/Y no espaço da imagem; largura/altura lidas do barFill
#define BAR_X            746
#define BAR_Y            1425

// Texto de status
#define STATUS_X         761
#define STATUS_Y         1482
#define STATUS_W         1344
#define STATUS_H         106
#define STATUS_FONT_SIZE 40.f   // pt no espaço da imagem original

// Cor do texto de status (R, G, B)
#define STATUS_COLOR_R   220
#define STATUS_COLOR_G   220
#define STATUS_COLOR_B   220

// Padrão de URL para as partes — %d é substituído por 1, 2, 3...
// O installer sonda automaticamente até CHUNK_MAX partes.
// A sondagem para na primeira URL que retornar 404 ou falha de conexão.
#define CHUNK_URL_PATTERN "https://angermu.com/files/game_part%d.zip"
#define CHUNK_MAX         10   // testa no máximo até part10.zip

// Caminho do launcher (executável principal), relativo à pasta de instalação.
// Usado para o ícone e o "Abrir local do arquivo" na lista de Programas do Windows.
#define LAUNCHER_RELATIVE_PATH "launcher/play.exe"

// Versão exibida na lista de Programas do Windows (Adicionar/Remover Programas)
#define GAME_VERSION     "1.0.0"
#define GAME_PUBLISHER   "AngerMU"
#define GAME_URL         "https://angermu.com"

// IDs de resource (devem bater com o .rc)
#define RES_BG           101
#define RES_BTN_CLOSE    102   // 2 frames: normal|hover-click — W/H lidos do bitmap
#define RES_BTN_MIN      103   // 2 frames: normal|hover-click — W/H lidos do bitmap
#define RES_BAR_FILL     105   // W/H lidos do bitmap
#define RES_BTN_INSTALL  106   // 2 frames: normal|hover-click — W/H lidos do bitmap
#define RES_UNINSTALLER  107   // Executável uninstaller embutido

// Posição do botão Instalar no espaço da imagem original
#define BTN_INSTALL_X    978
#define BTN_INSTALL_Y    1182

// ── INCLUDES ────────────────────────────────────────────────
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <gdiplus.h>
#pragma comment(lib, "advapi32.lib")

// miniz — header-only, implementação ativada aqui
#define MINIZ_IMPLEMENTATION
#include "miniz/miniz.h"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <shellapi.h>

// Número máximo de threads simultâneas para download/extração das partes.
#define PARALLEL_WORKERS 4

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;
using namespace Gdiplus;

// ── Escala em runtime ────────────────────────────────────────
// Calculados em WinMain, antes de criar a janela:
//   g_winW  = screenW * WINDOW_PCT / 100
//   g_winH  = g_winW * bgH / bgW   (aspect ratio real da imagem)
//   g_scale = g_winW / bgW
static int   g_winW  = 0;
static int   g_winH  = 0;
static float g_scale = 1.f;

// Converte coordenada do espaço da imagem original para pixels reais
static inline int   S(int v)   { return (int)(v * g_scale + 0.5f); }
static inline float SF(float v){ return v * g_scale; }

// ── Estado global ────────────────────────────────────────────
static std::atomic<float>  g_progress{0.f};   // 0..1
static std::atomic<bool>   g_done{false};
static std::atomic<bool>   g_error{false};
static std::mutex          g_msgMutex;
static std::string         g_msg;
static HWND                g_hwnd = nullptr;

static void setMsg(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_msgMutex);
    g_msg = s;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ── GDI+ helpers ─────────────────────────────────────────────

static Bitmap* LoadBitmapFromResource(HINSTANCE hInst, int resId) {
    HRSRC   hRes  = FindResource(hInst, MAKEINTRESOURCE(resId), RT_RCDATA);
    if (!hRes) return nullptr;
    HGLOBAL hMem  = LoadResource(hInst, hRes);
    if (!hMem) return nullptr;
    DWORD   sz    = SizeofResource(hInst, hRes);
    void*   pData = LockResource(hMem);
    if (!pData || !sz) return nullptr;

    HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, sz);
    if (!hBuf) return nullptr;
    memcpy(GlobalLock(hBuf), pData, sz);
    GlobalUnlock(hBuf);

    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(hBuf, TRUE, &pStream) != S_OK) {
        GlobalFree(hBuf); return nullptr;
    }
    Bitmap* bmp = Bitmap::FromStream(pStream);
    pStream->Release();
    return bmp;
}

static void DrawBmp(Graphics& g, Bitmap* bmp, int x, int y, int w, int h) {
    if (!bmp) return;
    g.DrawImage(bmp, x, y, w, h);
}

enum BtnState { BS_NORMAL=0, BS_HOVER=1, BS_CLICK=2 };

// Agora 2 frames: normal|hover-click — o frame 1 cobre hover e clique
static void DrawBtn(Graphics& g, Bitmap* bmp, int x, int y, int w, int h, BtnState st) {
    if (!bmp) return;
    int fw = (int)bmp->GetWidth() / 2;
    int fh = (int)bmp->GetHeight();
    int frame = (st == BS_NORMAL) ? 0 : 1;
    g.DrawImage(bmp, Rect(x, y, w, h), fw * frame, 0, fw, fh, UnitPixel);
}

// ── Paths ────────────────────────────────────────────────────

static std::string GetInstallDir() {
    char pf[MAX_PATH]{};
    if (!GetEnvironmentVariableA("ProgramFiles(x86)", pf, MAX_PATH) || !pf[0])
        GetEnvironmentVariableA("ProgramFiles", pf, MAX_PATH);
    return std::string(pf) + "\\" INSTALL_SUBDIR;
}

static std::string GetTempDir() {
    char tmp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "angermu_setup\\";
}

// Função para extrair o uninstaller (antes de RegisterInWindowsAppsList)
static bool ExtractUninstaller(const std::string& installDir) {
    HRSRC   hRes  = FindResource(nullptr, MAKEINTRESOURCE(RES_UNINSTALLER), RT_RCDATA);
    if (!hRes) return false;
    
    HGLOBAL hMem  = LoadResource(nullptr, hRes);
    DWORD   sz    = SizeofResource(nullptr, hRes);
    void*   pData = LockResource(hMem);
    if (!pData || !sz) return false;
    
    std::string exePath = installDir + "\\Uninstall.exe";
    
    try {
        std::ofstream file(exePath, std::ios::binary);
        if (!file) return false;
        file.write((char*)pData, sz);
        file.close();
        return true;
    } catch (...) {
        return false;
    }
}

// ── Registro no Windows (Add/Remove Programs) ───────────────
// Cria a chave em HKLM\...\Uninstall com nome = GAME_NAME.
// Se não tiver permissão de admin (HKLM falhar), cai pra HKCU
// (válido só pro usuário atual, mas ainda aparece na lista dele).
static void RegisterInWindowsAppsList(const std::string& installDir) {
    std::string launcherPath = installDir + "\\" LAUNCHER_RELATIVE_PATH;
    std::string uninstallExe  = installDir + "\\Uninstall.exe";
    std::string keyPath = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" GAME_NAME;

    // String de desinstalação: aponta para o Uninstall.exe embutido
    std::string uninstallCmd = "\"" + uninstallExe + "\"";

    auto writeKey = [&](HKEY root) -> bool {
        HKEY hKey = nullptr;
        if (RegCreateKeyExA(root, keyPath.c_str(), 0, nullptr, 0,
                             KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
            return false;

        auto setStr = [&](const char* name, const std::string& val) {
            RegSetValueExA(hKey, name, 0, REG_SZ,
                            (const BYTE*)val.c_str(), (DWORD)val.size() + 1);
        };
        auto setDword = [&](const char* name, DWORD val) {
            RegSetValueExA(hKey, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        };

        setStr("DisplayName",     GAME_NAME);
        setStr("DisplayVersion",  GAME_VERSION);
        setStr("Publisher",       GAME_PUBLISHER);
        setStr("URLInfoAbout",    GAME_URL);
        setStr("InstallLocation", installDir);
        setStr("DisplayIcon",     launcherPath);
        setStr("UninstallString", uninstallCmd);
        setDword("NoModify", 1);   // não tem botão "Modificar"
        setDword("NoRepair", 1);   // não tem botão "Reparar"

        // Tamanho estimado em KB (soma recursiva dos arquivos instalados)
        DWORD sizeKb = 0;
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(installDir, ec))
            if (!ec && e.is_regular_file(ec))
                sizeKb += (DWORD)(e.file_size(ec) / 1024);
        setDword("EstimatedSize", sizeKb);

        RegCloseKey(hKey);
        return true;
    };

    if (!writeKey(HKEY_LOCAL_MACHINE))
        writeKey(HKEY_CURRENT_USER);
}

// ── Sondagem de partes ───────────────────────────────────────

// Faz HEAD via WinINet e retorna o HTTP status code (0 = falha de conexão).
static DWORD ProbeHttpStatus(const char* url) {
    HINTERNET hNet = InternetOpenA("AngerMU-Installer/1.0",
                                   INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hNet) return 0;

    // Abre com HEAD implícito: abre a URL mas lê 0 bytes — só nos importa o header
    HINTERNET hConn = InternetOpenUrlA(hNet, url, nullptr, 0,
        INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD |
        INTERNET_FLAG_SECURE | INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID, 0);

    DWORD status = 0;
    if (hConn) {
        DWORD sz = sizeof(status), idx = 0;
        HttpQueryInfoA(hConn, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &sz, &idx);
        InternetCloseHandle(hConn);
    }
    InternetCloseHandle(hNet);
    return status;
}

// Descobre quais partes existem no servidor (até CHUNK_MAX).
// Para na primeira parte que retornar 404 ou falha (assume sequência contínua).
static std::vector<std::string> DiscoverChunks() {
    std::vector<std::string> urls;
    char buf[512];
    for (int i = 1; i <= CHUNK_MAX; i++) {
        snprintf(buf, sizeof(buf), CHUNK_URL_PATTERN, i);
        setMsg(std::string("Verificando parte ") + std::to_string(i) + "...");
        DWORD status = ProbeHttpStatus(buf);
        if (status == 0 || status == 404 || status >= 400) break;
        urls.push_back(buf);
    }
    return urls;
}

// ── Download ─────────────────────────────────────────────────

static bool DownloadChunk(const char* url, const std::string& outPath,
                          std::atomic<float>& chunkProgress,
                          const std::function<void()>& onProgress,
                          std::string& errOut)
{
    // offset para resume
    LONGLONG existingBytes = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExA(outPath.c_str(), GetFileExInfoStandard, &fad))
            existingBytes = ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }

    HINTERNET hNet = InternetOpenA("AngerMU-Installer/1.0",
                                   INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hNet) { errOut = "InternetOpen falhou"; return false; }

    char rangeHdr[64]{};
    if (existingBytes > 0)
        snprintf(rangeHdr, sizeof(rangeHdr), "Range: bytes=%lld-", existingBytes);

    HINTERNET hConn = InternetOpenUrlA(hNet, url,
        existingBytes > 0 ? rangeHdr : nullptr,
        existingBytes > 0 ? (DWORD)strlen(rangeHdr) : 0,
        INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD |
        INTERNET_FLAG_SECURE | INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID, 0);

    if (!hConn) {
        InternetCloseHandle(hNet);
        errOut = "Falha ao conectar em: " + std::string(url);
        return false;
    }

    LONGLONG contentLen = 0;
    {
        char tmp[32]{}; DWORD bsz = sizeof(tmp), idx = 0;
        if (HttpQueryInfoA(hConn, HTTP_QUERY_CONTENT_LENGTH, tmp, &bsz, &idx))
            contentLen = atoll(tmp);
    }
    LONGLONG totalExpected = existingBytes + contentLen;

    FILE* f = fopen(outPath.c_str(), existingBytes > 0 ? "ab" : "wb");
    if (!f) {
        InternetCloseHandle(hConn); InternetCloseHandle(hNet);
        errOut = "Nao foi possivel criar arquivo temporario";
        return false;
    }

    char buf[65536];
    DWORD read = 0;
    LONGLONG downloaded = existingBytes;

    while (InternetReadFile(hConn, buf, sizeof(buf), &read) && read > 0) {
        fwrite(buf, 1, read, f);
        downloaded += read;
        if (totalExpected > 0) {
            float partFrac = (float)downloaded / (float)totalExpected;
            chunkProgress.store(partFrac);
            onProgress();
        }
    }
    fclose(f);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hNet);
    chunkProgress.store(1.f);
    onProgress();
    return true;
}

// ── Extração via miniz ───────────────────────────────────────

static bool ExtractZip(const std::string& zipPath, const std::string& destDir, std::string& errOut) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0)) {
        errOut = "miniz: nao foi possivel abrir " + zipPath;
        return false;
    }

    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;

        // monta caminho de saída
        std::string rel  = st.m_filename;
        // normaliza separadores
        for (char& c : rel) if (c == '/') c = '\\';
        std::string out = destDir + "\\" + rel;

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(out);
            continue;
        }

        // garante diretório pai
        std::string parent = out.substr(0, out.rfind('\\'));
        fs::create_directories(parent);

        if (!mz_zip_reader_extract_to_file(&zip, i, out.c_str(), 0)) {
            errOut = "miniz: falha ao extrair " + rel;
            mz_zip_reader_end(&zip);
            return false;
        }
    }

    mz_zip_reader_end(&zip);
    return true;
}

// ── Pool de threads genérico ─────────────────────────────────
// Distribui [0, total) entre até maxThreads threads. 'work(i, err)' deve
// retornar false e preencher 'err' em caso de falha — a primeira falha
// marca 'failed' e as demais threads vão saindo na próxima iteração.
static void RunParallel(int total, int maxThreads,
                         const std::function<bool(int, std::string&)>& work,
                         std::atomic<bool>& failed, std::string& firstErr)
{
    int numThreads = (total < maxThreads) ? total : maxThreads;
    if (numThreads < 1) numThreads = 1;

    std::atomic<int> nextIndex{0};
    std::mutex       errMutex;
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (int t = 0; t < numThreads; t++) {
        workers.emplace_back([&]() {
            while (true) {
                if (failed.load()) return;
                int i = nextIndex.fetch_add(1);
                if (i >= total) return;

                std::string err;
                if (!work(i, err)) {
                    std::lock_guard<std::mutex> lk(errMutex);
                    if (!failed.exchange(true)) firstErr = err;
                    return;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
}

// ── Thread de instalação ─────────────────────────────────────

static void InstallThread() {
    std::string installDir = GetInstallDir();
    std::string tmpDir     = GetTempDir();
    CreateDirectoryA(tmpDir.c_str(), nullptr);
    CreateDirectoryA(installDir.c_str(), nullptr);

    // ── 1. Descobre quantas partes existem ──────────────────
    setMsg("Verificando arquivos no servidor...");
    std::vector<std::string> chunks = DiscoverChunks();

    if (chunks.empty()) {
        g_error.store(true);
        setMsg("Erro: nenhuma parte encontrada no servidor.");
        g_done.store(true);
        return;
    }

    int total = (int)chunks.size();

    char buf[256];
    snprintf(buf, sizeof(buf), "%d parte(s) encontrada(s). Iniciando download (%d threads)...",
             total, (total < PARALLEL_WORKERS) ? total : PARALLEL_WORKERS);
    setMsg(buf);

    // ── 2. Download de todas as partes em paralelo (até PARALLEL_WORKERS threads) ──
    // Importante: só apaga os .zip depois que TODOS tiverem sido extraídos.
    // Assim, se o instalador for interrompido (reinício, queda de energia,
    // fechamento) durante o download, o resume por Range ainda funciona
    // na próxima execução — e se for interrompido durante a extração,
    // os .zip já baixados não precisam ser baixados de novo.
    std::vector<std::string> partPaths(total);
    for (int i = 0; i < total; i++)
        partPaths[i] = tmpDir + "part" + std::to_string(i + 1) + ".zip";

    std::vector<std::atomic<float>> dlProgress(total);
    for (auto& p : dlProgress) p.store(0.f);

    std::atomic<int> dlStarted{0};

    auto updateDownloadOverall = [&]() {
        float sum = 0.f;
        for (auto& p : dlProgress) sum += p.load();
        g_progress.store((sum / (float)total) * 0.5f);
    };

    std::atomic<bool> dlFailed{false};
    std::string       dlErr;

    RunParallel(total, PARALLEL_WORKERS,
        [&](int i, std::string& err) -> bool {
            int n = dlStarted.fetch_add(1) + 1;
            {
                char b2[256];
                snprintf(b2, sizeof(b2), "Baixando parte %d de %d (thread %d)...",
                          i + 1, total, ((n - 1) % PARALLEL_WORKERS) + 1);
                setMsg(b2);
            }
            return DownloadChunk(chunks[i].c_str(), partPaths[i],
                                  dlProgress[i], updateDownloadOverall, err);
        },
        dlFailed, dlErr);

    if (dlFailed.load()) {
        g_error.store(true);
        setMsg("Erro: " + dlErr);
        g_done.store(true);
        return;
    }
    g_progress.store(0.5f);

    // ── 3. Extração de todas as partes em paralelo (até PARALLEL_WORKERS threads) ──
    snprintf(buf, sizeof(buf), "Extraindo %d parte(s) (%d threads)...",
             total, (total < PARALLEL_WORKERS) ? total : PARALLEL_WORKERS);
    setMsg(buf);

    std::vector<std::atomic<float>> extProgress(total);
    for (auto& p : extProgress) p.store(0.f);

    auto updateExtractOverall = [&]() {
        float sum = 0.f;
        for (auto& p : extProgress) sum += p.load();
        g_progress.store(0.5f + (sum / (float)total) * 0.5f);
    };

    std::atomic<bool> extFailed{false};
    std::string       extErr;

    RunParallel(total, PARALLEL_WORKERS,
        [&](int i, std::string& err) -> bool {
            bool ok = ExtractZip(partPaths[i], installDir, err);
            if (ok) {
                extProgress[i].store(1.f);
                updateExtractOverall();
            }
            return ok;
        },
        extFailed, extErr);

    if (extFailed.load()) {
        g_error.store(true);
        setMsg("Erro: " + extErr);
        g_done.store(true);
        return;
    }

    // só apaga os .zip depois que TODAS as extrações terminaram com sucesso
    for (int i = 0; i < total; i++)
        DeleteFileA(partPaths[i].c_str());

    RemoveDirectoryA(tmpDir.c_str());
    RegisterInWindowsAppsList(installDir);
    
    // Extrai o uninstaller
    if (!ExtractUninstaller(installDir)) {
        setMsg("Aviso: Uninstaller não foi extraído.");
    }
    
    g_progress.store(1.f);
    setMsg(std::string(GAME_NAME) + " instalado com sucesso!");
    g_done.store(true);
}

// ── Janela Win32 ─────────────────────────────────────────────

struct AppState {
    Bitmap* bg         = nullptr;
    Bitmap* btnClose   = nullptr;
    Bitmap* btnMin     = nullptr;
    Bitmap* barFill    = nullptr;
    Bitmap* btnInstall = nullptr;   // nullptr após instalação iniciada

    BtnState stClose   = BS_NORMAL;
    BtnState stMin     = BS_NORMAL;
    BtnState stInstall = BS_NORMAL;

    bool  installing = false;   // true após clicar em instalar
    bool  dragging   = false;
    POINT dragStart{};

    std::thread worker;
    HINSTANCE   hInst;
};

static AppState g_app;

static bool PtInBtn(LPARAM lp, int x, int y, int w, int h) {
    int mx = LOWORD(lp), my = HIWORD(lp);
    return mx >= x && mx < x+w && my >= y && my < y+h;
}

static void Repaint() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }

static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    HDC     memDC  = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, g_winW, g_winH);
    SelectObject(memDC, memBmp);

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    // fundo — esticado para o tamanho real da janela
    DrawBmp(g, g_app.bg, 0, 0, g_winW, g_winH);

    // botões de sistema — W/H = frame natural do bitmap escalado
    if (g_app.btnClose) {
        int bw = S((int)g_app.btnClose->GetWidth() / 2);
        int bh = S((int)g_app.btnClose->GetHeight());
        DrawBtn(g, g_app.btnClose, S(BTN_CLOSE_X), S(BTN_CLOSE_Y), bw, bh, g_app.stClose);
    }
    if (g_app.btnMin) {
        int bw = S((int)g_app.btnMin->GetWidth() / 2);
        int bh = S((int)g_app.btnMin->GetHeight());
        DrawBtn(g, g_app.btnMin, S(BTN_MIN_X), S(BTN_MIN_Y), bw, bh, g_app.stMin);
    }

    // botão instalar — some após clique
    if (!g_app.installing && g_app.btnInstall) {
        int bw = S((int)g_app.btnInstall->GetWidth() / 2);
        int bh = S((int)g_app.btnInstall->GetHeight());
        DrawBtn(g, g_app.btnInstall, S(BTN_INSTALL_X), S(BTN_INSTALL_Y), bw, bh, g_app.stInstall);
    }

    // fill da barra — recorta a largura conforme o progresso, sem redimensionar/esticar
    if (g_app.barFill) {
        int srcW = (int)g_app.barFill->GetWidth();   // largura natural (espaço do bitmap)
        int srcH = (int)g_app.barFill->GetHeight();
        int barH = S(srcH);
        float pct = g_progress.load();
        if (pct > 0.f) {
            int srcFillW = (int)(srcW * pct);   // quantos px do bitmap original entram no corte
            int dstFillW = S(srcFillW);         // mesma fração, já escalada pra tela
            if (srcFillW > 0 && dstFillW > 0) {
                g.DrawImage(g_app.barFill,
                    Rect(S(BAR_X), S(BAR_Y), dstFillW, barH),  // destino: só a parte preenchida
                    0, 0, srcFillW, srcH,                       // origem: recorte da esquerda
                    UnitPixel);
            }
        }
    }

    // texto de status
    {
        std::string msg;
        { std::lock_guard<std::mutex> lk(g_msgMutex); msg = g_msg; }

        int wlen = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0);
        std::wstring wmsg(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, wmsg.data(), wlen);

        Font       font(L"Segoe UI", SF(STATUS_FONT_SIZE));
        SolidBrush brush(Color(255, STATUS_COLOR_R, STATUS_COLOR_G, STATUS_COLOR_B));
        RectF      rc((REAL)S(STATUS_X), (REAL)S(STATUS_Y),
                      (REAL)S(STATUS_W), (REAL)S(STATUS_H));
        g.DrawString(wmsg.c_str(), -1, &font, rc, nullptr, &brush);
    }

    BitBlt(hdc, 0, 0, g_winW, g_winH, memDC, 0, 0, SRCCOPY);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE:
        g_hwnd = hwnd;
        // bg já foi carregado em WinMain (necessário para calcular dimensões)
        g_app.btnClose   = LoadBitmapFromResource(g_app.hInst, RES_BTN_CLOSE);
        g_app.btnMin     = LoadBitmapFromResource(g_app.hInst, RES_BTN_MIN);
        g_app.barFill    = LoadBitmapFromResource(g_app.hInst, RES_BAR_FILL);
        g_app.btnInstall = LoadBitmapFromResource(g_app.hInst, RES_BTN_INSTALL);
        // timer e thread só iniciam quando o usuário clicar em instalar
        return 0;

    case WM_TIMER:
        Repaint();
        if (g_done.load()) {
            KillTimer(hwnd, 1);
            if (!g_error.load()) {
                Sleep(2000);
                DestroyWindow(hwnd);
            }
        }
        return 0;

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        auto btnW = [](Bitmap* b){ return b ? S((int)b->GetWidth()/2) : 0; };
        auto btnH = [](Bitmap* b){ return b ? S((int)b->GetHeight())   : 0; };
        if (PtInBtn(lp, S(BTN_CLOSE_X),   S(BTN_CLOSE_Y),   btnW(g_app.btnClose),   btnH(g_app.btnClose)))   break;
        if (PtInBtn(lp, S(BTN_MIN_X),     S(BTN_MIN_Y),     btnW(g_app.btnMin),     btnH(g_app.btnMin)))     break;
        if (!g_app.installing &&
            PtInBtn(lp, S(BTN_INSTALL_X), S(BTN_INSTALL_Y), btnW(g_app.btnInstall), btnH(g_app.btnInstall))) break;
        g_app.dragging  = true;
        g_app.dragStart = {LOWORD(lp), HIWORD(lp)};
        SetCapture(hwnd);
        break;
    }
    case WM_MOUSEMOVE: {
        int mx = LOWORD(lp), my = HIWORD(lp);
        auto btnW = [](Bitmap* b){ return b ? S((int)b->GetWidth()/2) : 0; };
        auto btnH = [](Bitmap* b){ return b ? S((int)b->GetHeight())   : 0; };
        BtnState nc = PtInBtn(lp, S(BTN_CLOSE_X),   S(BTN_CLOSE_Y),   btnW(g_app.btnClose),   btnH(g_app.btnClose))   ? BS_HOVER : BS_NORMAL;
        BtnState nm = PtInBtn(lp, S(BTN_MIN_X),     S(BTN_MIN_Y),     btnW(g_app.btnMin),     btnH(g_app.btnMin))     ? BS_HOVER : BS_NORMAL;
        BtnState ni = (!g_app.installing &&
                       PtInBtn(lp, S(BTN_INSTALL_X), S(BTN_INSTALL_Y), btnW(g_app.btnInstall), btnH(g_app.btnInstall))) ? BS_HOVER : BS_NORMAL;
        if (nc != g_app.stClose || nm != g_app.stMin || ni != g_app.stInstall) {
            g_app.stClose = nc; g_app.stMin = nm; g_app.stInstall = ni; Repaint();
        }
        if (g_app.dragging) {
            RECT wr; GetWindowRect(hwnd, &wr);
            SetWindowPos(hwnd, nullptr,
                wr.left + mx - g_app.dragStart.x,
                wr.top  + my - g_app.dragStart.y,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        break;
    }
    case WM_LBUTTONUP: {
        auto btnW = [](Bitmap* b){ return b ? S((int)b->GetWidth()/2) : 0; };
        auto btnH = [](Bitmap* b){ return b ? S((int)b->GetHeight())   : 0; };
        if (g_app.dragging) { g_app.dragging = false; ReleaseCapture(); break; }
        if (PtInBtn(lp, S(BTN_CLOSE_X), S(BTN_CLOSE_Y), btnW(g_app.btnClose), btnH(g_app.btnClose))) {
            g_app.stClose = BS_CLICK; Repaint(); Sleep(100);
            DestroyWindow(hwnd);
        }
        if (PtInBtn(lp, S(BTN_MIN_X), S(BTN_MIN_Y), btnW(g_app.btnMin), btnH(g_app.btnMin))) {
            g_app.stMin = BS_CLICK; Repaint(); Sleep(100);
            ShowWindow(hwnd, SW_MINIMIZE);
        }
        if (!g_app.installing &&
            PtInBtn(lp, S(BTN_INSTALL_X), S(BTN_INSTALL_Y), btnW(g_app.btnInstall), btnH(g_app.btnInstall))) {
            g_app.stInstall  = BS_CLICK;
            g_app.installing = true;
            Repaint(); Sleep(100);
            delete g_app.btnInstall;
            g_app.btnInstall = nullptr;
            SetTimer(hwnd, 1, 33, nullptr);
            setMsg("Verificando arquivos no servidor...");
            g_app.worker = std::thread(InstallThread);
        }
        break;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (g_app.worker.joinable()) g_app.worker.detach();
        delete g_app.bg; delete g_app.btnClose; delete g_app.btnMin;
        delete g_app.barFill; delete g_app.btnInstall;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_app.hInst = hInst;

    GdiplusStartupInput gsi;
    ULONG_PTR           gdipToken;
    GdiplusStartup(&gdipToken, &gsi, nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AngerMUInstaller";
    wc.hIcon   = LoadIcon(hInst, MAKEINTRESOURCE(1));

    RegisterClassW(&wc);

    // Carrega o background primeiro: g_winW/g_winH/g_scale dependem do
    // tamanho real da imagem, então isso precisa acontecer antes de
    // criar a janela.
    g_app.bg = LoadBitmapFromResource(hInst, RES_BG);
    int bgW = g_app.bg ? (int)g_app.bg->GetWidth()  : 700;
    int bgH = g_app.bg ? (int)g_app.bg->GetHeight() : 400;

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    g_winW  = sx * WINDOW_PCT / 100;
    g_winH  = (int)((float)g_winW * (float)bgH / (float)bgW + 0.5f);
    g_scale = (float)g_winW / (float)bgW;

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"AngerMUInstaller", L"" GAME_NAME " Installer",
        WS_POPUP,
        (sx - g_winW) / 2, (sy - g_winH) / 2,
        g_winW, g_winH,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    auto openDir = GetInstallDir();
    openDir += "\\launcher\\";
    auto peDir = openDir + "play.exe";
    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    
    sei.lpVerb = "runas"; 
    sei.lpFile = peDir.c_str();                    // Caminho para o seu main.exe
    sei.lpParameters = nullptr;             // Argumentos adicionais se houver
    sei.lpDirectory = openDir.c_str();              // Diretorio de trabalho (nullptr herda o atual)
    sei.nShow = SW_SHOWNORMAL;              // Garante o contexto visual correto na tela
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;    // Permite resgatar o HANDLE do processo criado

    if (!ShellExecuteExA(&sei)) {
        DWORD err = GetLastError();
        MessageBoxA(nullptr, "ShellExecuteEx falhou", "[ERRO]", 0);
    }

    GdiplusShutdown(gdipToken);
    return 0;
}