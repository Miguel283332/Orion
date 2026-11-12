#include "orion/sistema.hpp"

#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#endif

namespace orion {

#ifdef _WIN32
static std::wstring a_utf16(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
// Reglas de comillas de CommandLineToArgvW
static std::wstring linea_de_comandos(const std::vector<std::string>& args) {
    std::wstring r;
    for (size_t i = 0; i < args.size(); ++i) {
        std::wstring a = a_utf16(args[i]);
        if (i) r += L' ';
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) { r += a; continue; }
        r += L'"';
        size_t barras = 0;
        for (wchar_t c : a) {
            if (c == L'\\') { ++barras; continue; }
            if (c == L'"') r.append(barras * 2 + 1, L'\\');
            else r.append(barras, L'\\');
            barras = 0;
            r += c;
        }
        r.append(barras * 2, L'\\');
        r += L'"';
    }
    return r;
}
#endif

Hardware medir_hardware(const fs::path& dir_datos) {
    Hardware h;
    h.hilos = static_cast<int>(std::thread::hardware_concurrency());
    std::error_code ec;
    auto esp = fs::space(dir_datos.empty() ? fs::current_path() : dir_datos, ec);
    if (!ec) h.disco_libre = static_cast<double>(esp.available);
#ifdef _WIN32
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    h.so = "Windows";
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        auto f = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")));
        RTL_OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof(v);
        if (f && f(&v) == 0)
            h.so = std::string(v.dwMajorVersion >= 10 && v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows " + std::to_string(v.dwMajorVersion)) +
                   " (compilación " + std::to_string(v.dwBuildNumber) + ")";
    }
    char nombre[256] = {};
    DWORD tam = sizeof(nombre);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr,
                     nombre, &tam) == ERROR_SUCCESS)
        h.cpu = trim(nombre);
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) {
        h.ram_total = static_cast<double>(m.ullTotalPhys);
        h.ram_libre = static_cast<double>(m.ullAvailPhys);
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) h.ram_proceso = static_cast<double>(pmc.WorkingSetSize);
#else
    utsname u{};
    if (uname(&u) == 0) h.so = std::string(u.sysname) + " " + u.release;
    for (auto& l : split_lines(leer_archivo("/proc/cpuinfo")))
        if (empieza_por(l, "model name")) { h.cpu = trim(l.substr(l.find(':') + 1)); break; }
    auto kb = [](const std::string& l) { return std::atof(trim(l.substr(l.find(':') + 1)).c_str()) * 1024.0; };
    for (auto& l : split_lines(leer_archivo("/proc/meminfo"))) {
        if (empieza_por(l, "MemTotal:")) h.ram_total = kb(l);
        if (empieza_por(l, "MemAvailable:")) h.ram_libre = kb(l);
    }
    for (auto& l : split_lines(leer_archivo("/proc/self/status")))
        if (empieza_por(l, "VmRSS:")) h.ram_proceso = kb(l);
#endif
    if (h.cpu.empty()) h.cpu = "desconocido";
    return h;
}

static const size_t MAX_SALIDA = 1 << 20;   // 1 MB: suficiente y evita llenar la memoria con salidas infinitas

#ifdef _WIN32
// Ejecuta una línea de comandos ya construida. Usa un "job" para poder matar también a los procesos hijos.
static ResultadoProceso ejecutar_win(std::wstring cmd, const fs::path& dir, int timeout_s, const std::string& entrada, const std::string& nombre) {
    ResultadoProceso r;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE lectura = nullptr, escritura = nullptr;
    if (!CreatePipe(&lectura, &escritura, &sa, 0)) return r;
    SetHandleInformation(lectura, HANDLE_FLAG_INHERIT, 0);
    HANDLE in_lect = nullptr, in_escr = nullptr;
    if (!CreatePipe(&in_lect, &in_escr, &sa, 0)) { CloseHandle(lectura); CloseHandle(escritura); return r; }
    SetHandleInformation(in_escr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = escritura;
    si.hStdError = escritura;
    si.hStdInput = in_lect;
    PROCESS_INFORMATION pi{};
    std::wstring wdir = dir.empty() ? L"" : dir.wstring();
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             wdir.empty() ? nullptr : wdir.c_str(), &si, &pi);
    if (ok) {
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
    }
    CloseHandle(escritura);
    CloseHandle(in_lect);
    if (ok && !entrada.empty()) {
        DWORD n = 0;
        WriteFile(in_escr, entrada.data(), static_cast<DWORD>(entrada.size()), &n, nullptr);
    }
    CloseHandle(in_escr);
    if (!ok) {
        CloseHandle(lectura);
        if (job) CloseHandle(job);
        r.salida = "no se pudo ejecutar " + nombre;
        return r;
    }
    std::string salida;
    std::thread lector([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(lectura, buf, sizeof(buf), &n, nullptr) && n > 0)
            if (salida.size() < MAX_SALIDA) salida.append(buf, n);
    });
    if (WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_s) * 1000) == WAIT_TIMEOUT) {
        r.tiempo_agotado = true;
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD codigo = 1;
    GetExitCodeProcess(pi.hProcess, &codigo);
    if (job) CloseHandle(job);   // mata a los hijos que queden y libera la tubería
    lector.join();
    r.codigo = static_cast<int>(codigo);
    r.salida = salida;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(lectura);
    return r;
}
#else
static ResultadoProceso ejecutar_posix(const std::vector<std::string>& args, const fs::path& dir, int timeout_s, const std::string& entrada) {
    ResultadoProceso r;
    int tubo[2], ent[2];
    if (pipe(tubo) != 0) return r;
    if (pipe(ent) != 0) { close(tubo[0]); close(tubo[1]); return r; }
    pid_t pid = fork();
    if (pid < 0) { close(tubo[0]); close(tubo[1]); close(ent[0]); close(ent[1]); return r; }
    if (pid == 0) {
        setpgid(0, 0);   // grupo propio: así se puede matar también a sus hijos
        dup2(ent[0], 0);
        close(ent[0]);
        close(ent[1]);
        dup2(tubo[1], 1);
        dup2(tubo[1], 2);
        close(tubo[0]);
        close(tubo[1]);
        if (!dir.empty() && chdir(dir.c_str()) != 0) _exit(127);
        std::vector<char*> av;
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    close(tubo[1]);
    close(ent[0]);
    signal(SIGPIPE, SIG_IGN);
    for (size_t enviado = 0; enviado < entrada.size();) {
        ssize_t n = write(ent[1], entrada.data() + enviado, entrada.size() - enviado);
        if (n <= 0) break;
        enviado += static_cast<size_t>(n);
    }
    close(ent[1]);
    auto limite = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    char buf[4096];
    int st = 0;
    bool terminado = false;
    for (;;) {
        pollfd p{tubo[0], POLLIN, 0};
        int n = poll(&p, 1, 100);
        if (n > 0) {
            ssize_t leidos = read(tubo[0], buf, sizeof(buf));
            if (leidos <= 0) break;
            if (r.salida.size() < MAX_SALIDA) r.salida.append(buf, static_cast<size_t>(leidos));
            continue;
        }
        if (n < 0 && errno != EINTR) break;
        if (!terminado && waitpid(pid, &st, WNOHANG) == pid) terminado = true;
        if (terminado) break;   // ya terminó y no queda nada que leer (aunque un nieto mantenga la tubería abierta)
        if (std::chrono::steady_clock::now() > limite) {
            kill(-pid, SIGKILL);
            r.tiempo_agotado = true;
            break;
        }
    }
    close(tubo[0]);
    if (!terminado) waitpid(pid, &st, 0);
    r.codigo = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return r;
}
#endif

ResultadoProceso ejecutar_proceso(const std::vector<std::string>& args, const fs::path& dir, int timeout_s, const std::string& entrada) {
    if (args.empty()) return {};
#ifdef _WIN32
    return ejecutar_win(linea_de_comandos(args), dir, timeout_s, entrada, args[0]);
#else
    return ejecutar_posix(args, dir, timeout_s, entrada);
#endif
}

ResultadoProceso ejecutar_shell(const std::string& comando, const fs::path& dir, int timeout_s) {
#ifdef _WIN32
    // chcp 65001: salida en UTF-8 (si no, las tildes salen mal). /s: cmd quita solo las comillas exteriores.
    return ejecutar_win(L"cmd.exe /d /s /c \"chcp 65001 >nul & " + a_utf16(comando) + L"\"", dir, timeout_s, "", "cmd.exe");
#else
    return ejecutar_posix({"/bin/sh", "-c", comando}, dir, timeout_s, "");
#endif
}

int ejecutar_en_consola(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
#ifdef _WIN32
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = linea_de_comandos(args);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD codigo = 1;
    GetExitCodeProcess(pi.hProcess, &codigo);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(codigo);
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        std::vector<char*> av;
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
}

fs::path ruta_ejecutable_actual(const char* argv0) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    if (n > 0) return fs::path(std::wstring(buf, n));
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p;
#endif
    return fs::absolute(argv0 ? argv0 : "orion");
}

}  // namespace orion
