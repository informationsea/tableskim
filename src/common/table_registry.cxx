#include "table_source.hxx"
#include "table_csv.hxx"
#include "table_duckdb.hxx"
#include "table_excel.hxx"
#include "table_json.hxx"
#include "table_parquet.hxx"
#include "table_sqlite.hxx"

const BackendRegistry &BackendRegistry::builtin()
{
    static const BackendRegistry registry = []
    {
        BackendRegistry r;
        registerCsvBackend(r);
        registerExcelBackend(r);
        registerDuckDbBackend(r);
        registerParquetBackend(r);
        registerJsonBackend(r);
        registerSqliteBackend(r);
        return r;
    }();
    return registry;
}
