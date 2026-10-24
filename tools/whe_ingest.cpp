// whe_ingest — import a real-instrument sample folder, build the DFD cache,
// and print the automatic key-zone / velocity / round-robin map.
//
//   whe_ingest <sample-folder> [--middle-c 3|4] [--cache <dir>] [--zones]
#include <cstdio>
#include <string>

#include "whe/sample_library.hpp"

using namespace whe;

static const char* noteName(int midi, int middleC) {
    static thread_local char buf[8];
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::snprintf(buf, sizeof buf, "%s%d", names[midi % 12], midi / 12 - (5 - middleC));
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: whe_ingest <sample-folder> [--middle-c 3|4] [--cache <dir>] [--zones]\n");
        return 2;
    }
    ImportOptions opts;
    bool zones = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--middle-c" && i + 1 < argc) opts.naming.middleCOctave = std::stoi(argv[++i]);
        else if (a == "--cache" && i + 1 < argc) opts.cacheDir = argv[++i];
        else if (a == "--zones") zones = true;
    }
    auto lib = SampleLibrary::import(argv[1], opts);
    if (!lib) {
        std::fprintf(stderr, "import failed: %s\n", lib.error().message.c_str());
        return 1;
    }
    std::fputs((*lib)->report().c_str(), stdout);
    if (zones) {
        for (const auto& art : (*lib)->articulations()) {
            std::printf("\n[%s]\n", art.name.c_str());
            for (const auto& z : art.zones) {
                std::string files;
                for (u32 id : z.roundRobins) {
                    if (!files.empty()) files += ", ";
                    files += (*lib)->store().get(id).source.filename().string();
                }
                std::printf("  keys %3d-%-3d root %-4s vel %3d-%-3d  RR x%zu  %s\n", z.loKey, z.hiKey, noteName(z.rootKey, opts.naming.middleCOctave),
                            z.loVel, z.hiVel, z.roundRobins.size(), files.c_str());
            }
        }
    }
    return 0;
}
