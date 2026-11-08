// tools\padglyphs\preview.cpp: every padglyphs icon as BC3 (as the mod uploads it) to out_dir/<name>_<w>x<h>_<letters>.bc3.
//   g++ -O2 -std=c++17 -I../../fc5vr/src/FarCry5 preview.cpp ../../fc5vr/src/FarCry5/FarCry5PadGlyphDraw.cpp -o preview && ./preview out
#include "FarCry5PadGlyphDraw.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace fc5::padglyph;
int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    const int sizes[3][2]{ { 64, 64 }, { 96, 96 }, { 92, 64 } };
    for (int li = 0; li < 2; ++li)
        for (int i = 1; i < int(Icon::Count); ++i)
            for (auto& s : sizes) {
                std::vector<uint8_t> rgba(size_t(s[0]) * s[1] * 4), bc((s[0] / 4) * (s[1] / 4) * 16);
                render(Icon(i), li ? Letters::Index : Letters::Touch, s[0], s[1], rgba.data());
                encode_bc3(rgba.data(), s[0], s[1], bc.data());
                char name[256];
                std::snprintf(name, sizeof name, "%s/%02d_%dx%d_%s.bc3", dir.c_str(), i, s[0], s[1], li ? "index" : "touch");
                FILE* f = std::fopen(name, "wb");
                std::fwrite(bc.data(), 1, bc.size(), f);
                std::fclose(f);
            }
    std::printf("hash test %016llx\n", (unsigned long long)hash_rows("abc", 3, 1, 3));
    return 0;
}
