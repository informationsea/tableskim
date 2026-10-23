// Build-time helper: converts the user manual (docs/) to HTML for browsers.
//
//   tableskim-html-manual <docs directory> <output directory>
//
// Every Markdown file becomes an HTML page and every PNG image is copied,
// keeping the directory layout. The output directory is replaced.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "help_docs.hxx"

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        std::fprintf(stderr, "Usage: %s <docs directory> <output directory>\n", argv[0]);
        return 1;
    }
    const std::filesystem::path docs = argv[1];
    const std::filesystem::path output = argv[2];
    try
    {
        std::vector<HelpFile> sources;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(docs))
        {
            const std::filesystem::path &path = entry.path();
            if (!entry.is_regular_file() || (path.extension() != ".md" && path.extension() != ".png"))
                continue;
            std::ifstream in(path, std::ios::binary);
            std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            if (!in && !in.eof())
            {
                std::fprintf(stderr, "Cannot read %s\n", path.string().c_str());
                return 1;
            }
            sources.push_back({path.lexically_relative(docs).generic_string(), std::move(data)});
        }

        std::filesystem::remove_all(output);
        for (const HelpFile &file : buildBrowserManual(sources))
        {
            const std::filesystem::path path = output / std::filesystem::path(file.path);
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            out.write(file.data.data(), static_cast<std::streamsize>(file.data.size()));
            if (!out)
            {
                std::fprintf(stderr, "Cannot write %s\n", path.string().c_str());
                return 1;
            }
        }
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
