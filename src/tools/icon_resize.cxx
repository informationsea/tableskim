// Build-time helper: downscales a PNG to a square icon size.
//
//   tableskim-icon-resize <input.png> <output.png> <size>
//
// Used to generate the hicolor theme icons on Linux, where no image tool
// can be assumed to be installed.

#include <FL/Fl_PNG_Image.H>

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        std::fprintf(stderr, "Usage: %s <input.png> <output.png> <size>\n", argv[0]);
        return 1;
    }

    Fl_PNG_Image src(argv[1]);
    if (src.fail() || src.count() < 1)
    {
        std::fprintf(stderr, "Cannot read %s\n", argv[1]);
        return 1;
    }
    const int size = std::atoi(argv[3]);
    if (size <= 0 || size > src.w() || size > src.h())
    {
        std::fprintf(stderr, "Invalid size: %s\n", argv[3]);
        return 1;
    }

    const int depth = src.d();
    const int line = src.ld() ? src.ld() : src.w() * depth;
    const bool has_alpha = depth == 2 || depth == 4;
    const int colors = has_alpha ? depth - 1 : depth;
    const auto *pixels = reinterpret_cast<const unsigned char *>(src.data()[0]);

    // Box filter: bilinear scaling would skip most source pixels when
    // shrinking 1024 -> 32. Colors are weighted by alpha so transparent
    // pixels do not darken the edges.
    std::vector<unsigned char> out(static_cast<size_t>(size) * size * depth);
    for (int y = 0; y < size; y++)
    {
        const int y0 = y * src.h() / size;
        const int y1 = (y + 1) * src.h() / size;
        for (int x = 0; x < size; x++)
        {
            const int x0 = x * src.w() / size;
            const int x1 = (x + 1) * src.w() / size;
            double sum[4] = {0, 0, 0, 0};
            double alpha_sum = 0;
            for (int sy = y0; sy < y1; sy++)
            {
                const unsigned char *row = pixels + static_cast<size_t>(sy) * line;
                for (int sx = x0; sx < x1; sx++)
                {
                    const unsigned char *p = row + static_cast<size_t>(sx) * depth;
                    const double a = has_alpha ? p[colors] / 255.0 : 1.0;
                    for (int c = 0; c < colors; c++)
                        sum[c] += p[c] * a;
                    alpha_sum += a;
                }
            }
            const double count = static_cast<double>(x1 - x0) * (y1 - y0);
            unsigned char *q = out.data() + (static_cast<size_t>(y) * size + x) * depth;
            for (int c = 0; c < colors; c++)
                q[c] = alpha_sum > 0 ? static_cast<unsigned char>(sum[c] / alpha_sum + 0.5) : 0;
            if (has_alpha)
                q[colors] = static_cast<unsigned char>(alpha_sum / count * 255.0 + 0.5);
        }
    }

    if (fl_write_png(argv[2], out.data(), size, size, depth) != 0)
    {
        std::fprintf(stderr, "Cannot write %s\n", argv[2]);
        return 1;
    }
    return 0;
}
