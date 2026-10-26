; Included by the NSIS uninstaller; reverts register_file_types.nsh.

DeleteRegKey SHCTX "Software\Classes\TableSkim.Table"
DeleteRegKey SHCTX "Software\Classes\Applications\TableSkim.exe"

!macro _TableSkimUnregisterExtension EXT
    DeleteRegKey SHCTX "Software\Classes\TableSkim${EXT}"
    DeleteRegValue SHCTX "Software\Classes\${EXT}\OpenWithProgids" "TableSkim${EXT}"
    DeleteRegValue SHCTX "Software\Classes\${EXT}\OpenWithProgids" "TableSkim.Table"
!macroend
!insertmacro _TableSkimUnregisterExtension ".csv"
!insertmacro _TableSkimUnregisterExtension ".tsv"
!insertmacro _TableSkimUnregisterExtension ".tab"
!insertmacro _TableSkimUnregisterExtension ".xlsx"
!insertmacro _TableSkimUnregisterExtension ".xlsm"
!insertmacro _TableSkimUnregisterExtension ".parquet"
!insertmacro _TableSkimUnregisterExtension ".duckdb"
!insertmacro _TableSkimUnregisterExtension ".ddb"
!insertmacro _TableSkimUnregisterExtension ".sqlite"
!insertmacro _TableSkimUnregisterExtension ".sqlite3"
!insertmacro _TableSkimUnregisterExtension ".db3"
!insertmacro _TableSkimUnregisterExtension ".json"
!insertmacro _TableSkimUnregisterExtension ".jsonl"
!insertmacro _TableSkimUnregisterExtension ".ndjson"
!insertmacro _TableSkimUnregisterExtension ".vcf"

System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
