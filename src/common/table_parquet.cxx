#include "table_parquet.hxx"

#include <stdexcept>
#include <string>

#include "table_duckdb.hxx"

namespace
{
    std::string utf8(const std::filesystem::path &path)
    {
        const std::u8string u8 = path.u8string();
        return std::string(u8.begin(), u8.end());
    }
} // namespace

std::unique_ptr<DataSource> openParquetDataSource(const std::filesystem::path &path, const OpenOptions &)
{
    try
    {
        return openDuckDbRelations({{
            .sheet_name = utf8(path.filename()),
            // Without hive_partitioning = false, "key=value" directories in
            // the path would be added as columns.
            .from_sql = "read_parquet(" + duckDbStringLiteral(escapeDuckDbGlob(utf8(path))) +
                        ", hive_partitioning = false)",
            // read_parquet() exposes the row position as a virtual column.
            .order_column = "file_row_number",
            .cheap_count = true, // stored in the footer
            // SELECT * FROM data for data.parquet
            .sql_name = utf8(path.stem()),
        }});
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error(std::string("Cannot read Parquet file: ") + e.what());
    }
}

void registerParquetBackend(BackendRegistry &registry)
{
    registry.add({
        .name = "Parquet",
        .probe = [](const std::filesystem::path &, std::span<const std::byte> head) -> int
        {
            const bool magic = head.size() >= 4 && head[0] == std::byte{'P'} && head[1] == std::byte{'A'} &&
                               head[2] == std::byte{'R'} && head[3] == std::byte{'1'};
            return magic ? 100 : 0;
        },
        .open = [](const std::filesystem::path &path, const OpenOptions &options)
        { return openParquetDataSource(path, options); },
    });
}
