#include "table_json.hxx"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#include "decompress.hxx"
#include "file_util.hxx"
#include "sql_text.hxx"
#include "table_duckdb.hxx"

namespace
{
    // Below this size, every row is read to find the column types: a value
    // of another type after the sample would make reading fail later on.
    constexpr std::uintmax_t FULL_SAMPLE_BYTES = 128 * 1024 * 1024;

    bool hasJsonExtension(const std::filesystem::path &path)
    {
        const std::string ext = lowerExtension(stripCompressionExtension(path));
        return ext == ".json" || ext == ".jsonl" || ext == ".ndjson";
    }

    bool startsWithBom(std::span<const std::byte> head)
    {
        return head.size() >= 3 && head[0] == std::byte{0xEF} && head[1] == std::byte{0xBB} &&
               head[2] == std::byte{0xBF};
    }

    // The first character after a UTF-8 BOM and whitespace.
    char firstCharacter(std::span<const std::byte> head)
    {
        size_t i = startsWithBom(head) ? 3 : 0;
        for (; i < head.size(); ++i)
        {
            const char c = static_cast<char>(head[i]);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                return c;
        }
        return '\0';
    }
} // namespace

std::unique_ptr<DataSource> openJsonDataSource(const std::filesystem::path &path, const OpenOptions &)
{
    const char *compression = "uncompressed";
    switch (detectCompression(path))
    {
    case Compression::None:
        break;
    case Compression::Gzip:
        compression = "gzip";
        break;
    case Compression::Zstd:
        compression = "zstd";
        break;
    case Compression::Bzip2:
    case Compression::Xz:
        throw std::runtime_error("Cannot read JSON file: only gzip and zstd compression are supported");
    }
    // read_json() takes an array after a UTF-8 BOM for a single value.
    std::string format;
    if (compression == std::string_view("uncompressed"))
    {
        std::ifstream in(path, std::ios::binary);
        char buffer[4096];
        in.read(buffer, sizeof(buffer));
        const auto head = std::as_bytes(std::span<const char>(buffer, static_cast<size_t>(in.gcount())));
        if (startsWithBom(head) && firstCharacter(head) == '[')
            format = ", format = 'array'";
    }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    const bool full_sample = !ec && size <= FULL_SAMPLE_BYTES;
    try
    {
        return openDuckDbRelations({{
            .sheet_name = utf8String(path.filename()),
            .from_sql = "read_json(" + quoteSqlString(escapeDuckDbGlob(utf8String(path))) +
                        ", compression = '" + compression + "', hive_partitioning = false" + format +
                        (full_sample ? ", sample_size = -1" : "") + ")",
            // No row number to page by: snapshotted in file order.
            .order_column = "",
            .cheap_count = false,
            // SELECT * FROM data for data.json
            .sql_name = utf8String(stripCompressionExtension(path.filename()).stem()),
        }});
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error(std::string("Cannot read JSON file: ") + e.what());
    }
}

void registerJsonBackend(BackendRegistry &registry)
{
    registry.add({
        .name = "JSON",
        .probe = [](const std::filesystem::path &path, std::span<const std::byte> head) -> int
        {
            if (hasJsonExtension(path))
                return 100;
            // Other names: text that starts like JSON, above the CSV fallback.
            const char first = firstCharacter(head);
            return detectCompression(head) == Compression::None && (first == '{' || first == '[') ? 10 : 0;
        },
        .open = [](const std::filesystem::path &path, const OpenOptions &options)
        { return openJsonDataSource(path, options); },
    });
}
