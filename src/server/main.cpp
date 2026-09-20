// =============================================================================
//  main.cpp —— 服务端入口
//
//  用法：
//    blob_server [--port 8080] [--web web] [--data-dir data] [--seed 20260919]
//
//  启动后直接用浏览器打开 http://127.0.0.1:8080 即可试玩。
//  同一个端口同时提供静态资源与 WebSocket，无需额外的 Web 服务器。
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "server/gateway.h"
#include "version.h"

namespace {

void printUsage() {
    std::printf("Blob Arena server\n");
    std::printf("Usage: blob_server [options]\n");
    std::printf("  -p, --port <n>    监听端口 (默认 8080)\n");
    std::printf("  -w, --web  <dir>  静态资源目录 (默认 web)\n");
    std::printf("      --data-dir <dir>  持久化目录 (默认 data)\n");
    std::printf("      --seed <n>    世界随机种子 (默认 20260919)\n");
    std::printf("      --world-size <n>  世界边长 (默认 1800；调小可快速复现密集互吃场景)\n");
    std::printf("      --arena-countdown <s>  开局倒计时秒数 (默认 8)\n");
    std::printf("      --arena-seconds <s>    单局时长秒数 (默认 180)\n");
    std::printf("      --shrink-start <s>     缩圈开始秒数 (默认 90)\n");
    std::printf("      --io-threads <n>       IO Reactor 线程数 (默认 2)\n");
    std::printf("      --reconnect-grace <ms> 断线重连宽限毫秒数 (默认 10000)\n");
    std::printf("  -h, --help        显示帮助\n");
    std::printf("  -v, --version     显示版本\n");
}

}  // namespace

int main(int argc, char** argv) {
    blob::ServerOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            options.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if ((arg == "--web" || arg == "-w") && i + 1 < argc) {
            options.webRoot = argv[++i];
        } else if (arg == "--data-dir" && i + 1 < argc) {
            options.dataDir = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            options.seed = static_cast<uint32_t>(
                std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--world-size" && i + 1 < argc) {
            const unsigned long n = std::strtoul(argv[++i], nullptr, 10);
            if (n >= 200ul) {  // 太小的世界连出生半径都放不下
                options.worldSize = static_cast<uint32_t>(n);
            }
        } else if (arg == "--arena-countdown" && i + 1 < argc) {
            const int n = std::atoi(argv[++i]);
            if (n > 0) options.arenaCountdownSec = n;
        } else if (arg == "--arena-seconds" && i + 1 < argc) {
            const int n = std::atoi(argv[++i]);
            if (n > 0) options.arenaDurationSec = n;
        } else if (arg == "--shrink-start" && i + 1 < argc) {
            const int n = std::atoi(argv[++i]);
            if (n >= 0) options.arenaShrinkStartSec = n;
        } else if (arg == "--io-threads" && i + 1 < argc) {
            const int n = std::atoi(argv[++i]);
            if (n > 0) options.ioThreads = static_cast<std::size_t>(n);
        } else if (arg == "--reconnect-grace" && i + 1 < argc) {
            const int n = std::atoi(argv[++i]);
            if (n > 0) options.reconnectGraceMs = n;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--version" || arg == "-v") {
            std::printf("%s %s\n", blob::kProjectName,
                        blob::kProjectVersion);
            return 0;
        } else {
            std::printf("unknown argument: %s\n", arg.c_str());
            printUsage();
            return 1;
        }
    }

    blob::Gateway gateway(options);
    if (!gateway.start()) {
        std::printf("[main] server start failed\n");
        return 1;
    }

    gateway.run();
    return 0;
}
