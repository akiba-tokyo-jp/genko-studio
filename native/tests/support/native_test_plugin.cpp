// A native filter plugin for the tests (docs/PLUGIN_NATIVE.md, protocol 1): one JSON line and the RGBA pixels on
// stdin, one JSON line and the RGBA pixels back on stdout. What it does is its first argument (plugin.json's "args"):
//   invert        the colours turned over, the alpha kept; the setting "red" (0..255) put in the red of every pixel
//   environment   green when GENKO_TEST_SECRET was not passed on and GENKO_PLUGIN_PROTOCOL is 1, red otherwise; the
//                 blue the number in home.txt of GENKO_PLUGIN_HOME (its own folder)
//   fail          {"ok": false, "error": "no paper today"}
//   small         a picture of 1 × 1
//   crash         stops (abort) after reading the request
//   sleep         sleeps for 1000 seconds
//   flood         writes 64 MB back
// "--mark PATH" (anywhere after the first argument): a line written to PATH first, so a test sees that it ran.
// No Qt, no Genko library: a plugin is any program that speaks the protocol.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

// The number after "key": in a line of JSON (the tests' requests are Genko's own: no spaces inside a number).
long number_of(const std::string& line, const std::string& key, long otherwise) {
    const std::string quoted = "\"" + key + "\":";
    const std::size_t at = line.find(quoted);
    if (at == std::string::npos) return otherwise;
    std::size_t p = at + quoted.size();
    while (p < line.size() && line[p] == ' ') ++p;
    return std::strtol(line.c_str() + p, nullptr, 10);
}

void reply(const std::string& header, const std::string& data = {}) {
    std::fwrite(header.data(), 1, header.size(), stdout);
    std::fputc('\n', stdout);
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), stdout);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const std::string what = argc > 1 ? argv[1] : "";
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--mark") == 0) std::ofstream(argv[i + 1], std::ios::app) << what << "\n";
    }
    std::string line;
    if (!std::getline(std::cin, line)) return 2;
    const long width = number_of(line, "width", 0);
    const long height = number_of(line, "height", 0);
    if (number_of(line, "protocol", 0) != 1 || width <= 0 || height <= 0 || width > 100000 || height > 100000) {
        reply(R"({"ok": false, "error": "not a request of protocol 1"})");
        return 0;
    }
    std::string pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, '\0');
    std::cin.read(pixels.data(), static_cast<std::streamsize>(pixels.size()));
    const std::string size = R"("width": )" + std::to_string(width) + R"(, "height": )" + std::to_string(height);
    if (what == "invert") {
        const long red = number_of(line, "red", -1);
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            for (std::size_t c = 0; c < 3; ++c) pixels[i + c] = static_cast<char>(255 - static_cast<unsigned char>(pixels[i + c]));
            if (red >= 0 && red <= 255) pixels[i] = static_cast<char>(red);
        }
        reply(R"({"ok": true, )" + size + "}", pixels);
    } else if (what == "environment") {
        const char* secret = std::getenv("GENKO_TEST_SECRET");
        const char* protocol = std::getenv("GENKO_PLUGIN_PROTOCOL");
        const char* home = std::getenv("GENKO_PLUGIN_HOME");
        const bool good = secret == nullptr && protocol != nullptr && std::string(protocol) == "1";
        int blue = 0;
        if (home != nullptr) {
            std::ifstream file(std::string(home) + "/home.txt");
            file >> blue;
        }
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = static_cast<char>(good ? 0 : 255);
            pixels[i + 1] = static_cast<char>(good ? 255 : 0);
            pixels[i + 2] = static_cast<char>(blue);
            pixels[i + 3] = static_cast<char>(255);
        }
        reply(R"({"ok": true, )" + size + "}", pixels);
    } else if (what == "fail") {
        reply(R"({"ok": false, "error": "no paper today"})");
    } else if (what == "small") {
        reply(R"({"ok": true, "width": 1, "height": 1})", std::string(4, '\x7f'));
    } else if (what == "crash") {
        std::abort();
    } else if (what == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(1000));
    } else if (what == "flood") {
        const std::string block(1 << 20, 'x');
        for (int i = 0; i < 64; ++i) std::fwrite(block.data(), 1, block.size(), stdout);
        std::fflush(stdout);
    } else {
        reply(R"({"ok": false, "error": "unknown work"})");
    }
    return 0;
}
