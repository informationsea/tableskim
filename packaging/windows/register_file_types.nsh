; Included by the NSIS installer (see cmake/Packaging.cmake).
;
; Adds TableSkim to "Open with" for table files via OpenWithProgids. The
; user's default application is left alone; Windows lets the user pick it.
; SHCTX is HKLM for administrators and HKCU otherwise, as set by the
; CPack template in .onInit / un.onInit.
;
; Each extension gets its own ProgID (TableSkim.csv, TableSkim.xlsx, ...):
; once TableSkim is the default app, Explorer shows the ProgID's name and
; icon for the file, so a shared ProgID would make every format look the
; same. ICON is a resource ID in TableSkim.exe (see src/gui/tableskim.rc.in).

; Older versions registered one shared ProgID for all extensions.
DeleteRegKey SHCTX "Software\Classes\TableSkim.Table"

WriteRegStr SHCTX "Software\Classes\Applications\TableSkim.exe" "FriendlyAppName" "TableSkim"
WriteRegStr SHCTX "Software\Classes\Applications\TableSkim.exe\shell\open\command" "" '"$INSTDIR\TableSkim.exe" "%1"'

!macro _TableSkimRegisterExtension EXT DESCRIPTION ICON
    DeleteRegValue SHCTX "Software\Classes\${EXT}\OpenWithProgids" "TableSkim.Table"
    WriteRegStr SHCTX "Software\Classes\TableSkim${EXT}" "" "${DESCRIPTION}"
    WriteRegStr SHCTX "Software\Classes\TableSkim${EXT}\DefaultIcon" "" "$INSTDIR\TableSkim.exe,-${ICON}"
    WriteRegStr SHCTX "Software\Classes\TableSkim${EXT}\shell\open\command" "" '"$INSTDIR\TableSkim.exe" "%1"'
    WriteRegStr SHCTX "Software\Classes\${EXT}\OpenWithProgids" "TableSkim${EXT}" ""
    WriteRegStr SHCTX "Software\Classes\Applications\TableSkim.exe\SupportedTypes" "${EXT}" ""
!macroend
!insertmacro _TableSkimRegisterExtension ".csv" "CSV File" 201
!insertmacro _TableSkimRegisterExtension ".tsv" "TSV File" 202
!insertmacro _TableSkimRegisterExtension ".tab" "TSV File" 202
!insertmacro _TableSkimRegisterExtension ".xlsx" "Excel Workbook" 203
!insertmacro _TableSkimRegisterExtension ".xlsm" "Excel Macro-Enabled Workbook" 204
!insertmacro _TableSkimRegisterExtension ".parquet" "Parquet File" 205
!insertmacro _TableSkimRegisterExtension ".duckdb" "DuckDB Database" 206
!insertmacro _TableSkimRegisterExtension ".ddb" "DuckDB Database" 206
!insertmacro _TableSkimRegisterExtension ".sqlite" "SQLite Database" 207
!insertmacro _TableSkimRegisterExtension ".sqlite3" "SQLite Database" 207
!insertmacro _TableSkimRegisterExtension ".db3" "SQLite Database" 207
!insertmacro _TableSkimRegisterExtension ".json" "JSON File" 208
!insertmacro _TableSkimRegisterExtension ".jsonl" "JSON Lines File" 208
!insertmacro _TableSkimRegisterExtension ".ndjson" "JSON Lines File" 208
!insertmacro _TableSkimRegisterExtension ".vcf" "Variant Call Format File" 209

; SHCNE_ASSOCCHANGED: make Explorer pick up the change without a restart.
System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
