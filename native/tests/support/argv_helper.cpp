// genko_argv_helper -i <in> -o <out> [anything…]: a stand-in for a person's upscaler program (Tests only). It writes the
// arguments it was given, one per line, to the file named by $GENKO_ARGV_LOG (appended, after a "--" line), copies <in>
// to <out> (a picture of the same size: Genko resizes it), and prints something on stdout and stderr, which never
// reaches the MCP protocol.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    std::string in;
    std::string out;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "-i") in = argv[i + 1];
        if (std::string(argv[i]) == "-o") out = argv[i + 1];
    }
    if (const char* log = std::getenv("GENKO_ARGV_LOG")) {
        std::ofstream file(log, std::ios::app | std::ios::binary);
        file << "--\n";
        for (int i = 0; i < argc; ++i) file << argv[i] << "\n";
    }
    std::printf("{\"jsonrpc\": \"2.0\", \"id\": 1, \"result\": {\"injected\": true}}\n");
    std::fprintf(stderr, "argv helper ran\n");
    if (in.empty() || out.empty()) return 2;
    std::ifstream from(in, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(from)), std::istreambuf_iterator<char>());
    std::ofstream(out, std::ios::binary) << bytes;
    return 0;
}
